// CPCSyntaxError — Yamaha V9990 E-VDP-III (the GFX9000 cartridge), at &FF60-&FF6F.
//
// Built from the V9990 Application Manual (docs/reference/V9990-application-manual.pdf;
// tools/v9990page.py renders its pages). Chapter and page numbers below are the manual's
// printed ones.
//
// The chip runs its own raster from its own clocks -- XTAL 21.477 MHz, MCKIN 14.318 MHz,
// 25.175 MHz for B6 -- free of the CPC's: 13.3.12/13.3.13 give every line and field. Its
// picture goes to its own monitor, so it is drawn a line at a time as the raster reaches
// each line (a split made from a line interrupt shows where it was made), into a picture
// of every visible line of the field, borders included.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int V9990_CPC_PORT_BASE = 0xff60;
inline constexpr int V9990_VRAM_SIZE = 512 * 1024;

// 10.11 (p.47): in the bit-map modes VRAM0 holds the even logical addresses and VRAM1 the
// odd ones, each at the logical address / 2. The vram array is VRAM0 then VRAM1.
int v9990TransformBx(int address);

// 8.3 (p.17): the display mode, from P#7 MCS and R#6/R#7.
enum class V9990Mode { P1, P2, B0, B1, B2, B3, B4, B5, B6, B7, Standby };
const char* v9990ModeName(V9990Mode mode);

// One line and one field of the raster (13.3.12 p.102, 13.3.13 p.103), in the master
// clock that drives the mode. A line starts at the HSYNC; a field at the top border.
struct V9990Raster {
    double clockMHz = 21.47727;
    int lineClocks = 1368, hsync = 100, leftErase = 100, leftBorder = 56, display = 1024, rightBorder = 56;
    int clocksPerDot = 4;
    int topBorder = 41, displayLines = 212, bottomBorder = 37, totalLines = 313;
    int dots() const { return display / clocksPerDot; }
    int borderDots() const { return leftBorder / clocksPerDot; }
    int visibleDots() const { return (leftBorder + display + rightBorder) / clocksPerDot; }
    int visibleLines() const { return topBorder + displayLines + bottomBorder; }
    double lineUs() const { return lineClocks / clockMHz; }
    int displayStartClocks() const { return hsync + leftErase + leftBorder; }
};

// What the chip's monitor shows: every visible line of the last complete field (or, when
// interlaced, both fields woven), 0xAABBGGRR like the CPC's own picture.
struct V9990Picture {
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
    // The YS output per dot (1 = transparent): what a genlock (the Video9000) lets the
    // external picture show through. Only with R#8 YSE on, never in B5/B6.
    std::vector<uint8_t> ys;
    V9990Mode mode = V9990Mode::B1;
    long long fields = 0;          // bumps with every field completed
    // Where it sits on the monitor, for a genlock: the first visible dot, in us after the
    // HSYNC, and the span of a line; the visible lines of a field (rows = lines, or twice
    // that interlaced).
    double startUs = 9.3, spanUs = 52.9;
    int lines = 290;
};

class V9990 {
public:
    bool enabled = false;
    Bytes vram;
    std::array<uint8_t, 256> palette{};      // 64 entries x R,G,B,(unused)
    std::array<uint8_t, 64> registers{};
    int regSelect = 0;
    int readBuffer = 0;
    bool systemReset = false;                // P#7 SRS
    bool mcs = false;                        // P#7 MCS: 1 = MCKIN
    int outputControl = 0;                   // P#F: the GFX9000's superimpose control
    bool outputControlWritten = false;       // a program has set P#F since the reset
    int pendingIrq = 0;                      // P#6: b2 CE, b1 HI, b0 VI
    int commandStatus = 0;                   // P#5 TR, BD, CE
    int borderX = 0;                         // R#53/54

    // A command still exchanging data with the CPU (LMMC, CMMC out; LMCM, POINT in).
    struct Transfer { int kind = 0; int x = 0, y = 0, remainingX = 0, remainingY = 0, originX = 0, lowByte = -1; };
    std::optional<Transfer> transfer;
    std::deque<int> commandReadQueue;

    // The raster: where the chip is.
    V9990Raster raster;
    double nowUs = 0;          // the chip's time, in microseconds of the CPC's clock
    double lineStartUs = 0;
    int line = 0;              // 0 = the first top-border line
    int field = 0;             // interlace: 0 = first field, 1 = second
    bool hiFiredThisLine = false;

    V9990();
    void setEnabled(bool on);
    void reset();
    bool handlesPort(int port) const;
    // The CPC's time rides with every access, in its 4 MHz T-states, so status and
    // interrupts are read where the raster really is.
    int readPort(int port, long long cpcCycles);
    bool writePort(int port, int value, long long cpcCycles);
    void advanceTo(long long cpcCycles);
    // The /INT pin: low while an enabled flag is set (R#9 masks P#6). A level, not an
    // edge -- held until the program writes the flag back to P#6.
    bool intAsserted() const { return enabled && (pendingIrq & registers[9] & 7) != 0; }
    const V9990Picture& picture() const { return shown; }
    // Anything on the chip's monitor (not in stand-by, and fitted).
    bool displaying() const { return enabled && !systemReset && mode() != V9990Mode::Standby; }

    V9990Mode mode() const;
    V9990Raster rasterFor(V9990Mode m, int fieldIndex) const;
    int readStatus();
    int readRegister(int index);
    void writeRegister(int index, int value);

    // image space (10.1-10.9): the dot a command or the display addresses
    int bitsPerDot() const;
    int imageWidth() const;
    int imageHeight() const;
    int dotAddress(int x, int y) const;      // physical vram index of the byte holding the dot
    int getDot(int x, int y) const;
    void setDot(int x, int y, int source);   // through LOP, TP and WM (11.2 p.53)
    int fontColourAt(int x, int y, bool foreground) const;

private:
    V9990Picture work, shown;
    int wordAt(int index) const { return registers[index] | registers[index + 1] << 8; }
    int vramAddress(int base) const;
    void setVramAddress(int base, int address);
    int mapCpuAddress(int address) const;
    void advancePalettePointer();
    uint32_t paletteColour(int index) const;

    // raster
    void beginLine();
    void endField();
    void drawLine(int visibleLine);
    void drawLineColours(int visibleLine, uint32_t* out);
    uint32_t bitmapDot(int x, int imageY) const;
    uint32_t yjkDot(int x, int imageY, bool yuv) const;
    uint32_t cursorOver(uint32_t colour, int displayX, int displayY) const;
    uint32_t p1Dot(int displayX, int displayY, int spriteLine) const;
    uint32_t p2Dot(int displayX, int displayY) const;
    int patternDot(int nameBase, int patternBase, int patternsPerRow, int rowBytes, int x, int y, bool p2) const;
    struct LineSprite { int x; bool behind; int palette; int address; };
    std::vector<LineSprite> lineSprites;
    void collectSprites(int displayY, bool p2);
    std::optional<uint32_t> spriteDot(int displayX, bool frontOpaque, bool p2) const;
    int scrolledY(int displayY, int scrollY, int imageLines) const;

    // commands (chapter 11)
    int sourceX() const { return wordAt(32) & 0x7ff; }
    int sourceY() const { return wordAt(34) & 0xfff; }
    int destX() const { return wordAt(36) & 0x7ff; }
    int destY() const { return wordAt(38) & 0xfff; }
    int sizeX() const { int v = wordAt(40) & 0x7ff; return v ? v : 2048; }
    int sizeY() const { int v = wordAt(42) & 0xfff; return v ? v : 4096; }
    int linearSource() const { return (registers[32] | (wordAt(34) & 0x7ff) << 8) & (V9990_VRAM_SIZE - 1); }
    int linearDest() const { return (registers[36] | (wordAt(38) & 0x7ff) << 8) & (V9990_VRAM_SIZE - 1); }
    int linearSize() const;
    void startCommand(int opcode);
    void finishCommand();
    void stepTransfer(Transfer& t);
    void writeCommandData(int value);
    int readCommandData();
    void queueDots(int x0, int y0);
    void commandLine();
    void commandSearch();
    void movePointer(int opcode);
    int readLinear(int address) const { return vram[v9990TransformBx(address)]; }
    void writeLinear(int address, int value);
};

} // namespace cpcse
