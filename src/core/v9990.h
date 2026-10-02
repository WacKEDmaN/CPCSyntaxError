// CPCSyntaxError — Yamaha V9990 E-VDP-III (the GFX9000 cartridge), at &FF60-&FF6F.
//
// Built from the V9990 Application Manual (docs/reference/V9990-application-manual.pdf;
// tools/v9990page.py renders its pages). Chapter and page numbers below are the manual's
// printed ones. Where the manual is silent or wrong, "Powergraph notes" are tests of the
// V9990 on the CPC Powergraph board (the user's, 2026-10-03): what a real chip does.
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
    // Vertical scroll, as the V9990 does it (openMSX's V9990, its renderer "verified";
    // the manual's R#17-24, p.84, says nothing of when a write takes effect). R#18/R#22,
    // the high bits, are taken at the start of a field. A write of R#17/R#21, the low
    // byte, while the display is on restarts that layer's vertical count: each line after
    // the write shows the next row from the scroll value. The line bases are the raster
    // lines the counts run from -- the top border's end at a field start.
    int scrollAYHigh = 0, scrollBYHigh = 0;
    int scrollALineBase = 0, scrollBLineBase = 0;
    bool fieldDisplayOn = false;             // R#8 DISP as the field started
    // Powergraph notes: PSET / ADVN / LINE work on an internal X and Y, not on DX/DY.
    // X is taken from DX only by a command with AXE = AXM = 0; a write of R#38 or R#39
    // sets Y at once. Both wrap in the image space.
    int pointerX = 0, pointerY = 0;
    int pointHighByte = 0;                   // the high byte of the last 16-bit POINT
    int lmmvHighByte = 0;                    // FC's high byte as the last bit-map LMMV took it

    // A command still exchanging data with the CPU (LMMC, CMMC out; LMCM, POINT in).
    // The command engine (chapter 11), run in time (v9990.cpp): a step a dot -- a byte for
    // BMXL and BMLL -- each taking the time openMSX measured for it.
    struct Engine {
        int op = -1;                 // R#52's OP of the command running; -1 = none
        double nextUs = 0;           // when its last step ended (the next starts there)
        int x0 = 0, x = 0, y = 0, sx0 = 0, sx = 0, sy = 0;   // the rectangle walk (LMMM's source)
        int col = 0, row = 0, nx = 0, ny = 0;
        bool walkDone = false;       // every dot done; LMCM / POINT may still hold bytes
        int linear = 0, linearSrc = 0, remaining = 0, readsLeft = 0;   // BMxx / CMMM addresses
        int bits = 0, bitsLeft = 0;  // the byte being spent: its value, the dots (bits) left
        int lowByte = -1;            // 16 bits a dot: the low byte, waiting for the high
        int packed = 0, packedDots = 0;                       // LMCM / BMLX: dots into a byte
        int fetched = 0;             // CMMK: font bytes taken
        int lineStep = 0, lineErr = 0, lineMj = 0, lineMi = 0, searchEnd = 0;
        bool wantsByte = false;      // LMMC / CMMC: TR, waiting for the CPU's byte
        std::deque<int> out;         // LMCM / POINT: bytes for the CPU (TR while any)
        std::deque<int> readAhead;   // BMLL: source bytes read ahead of the writes
    };
    Engine engine;

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
    // /WAIT (pin description p.5: "active (Low) while VDP is busy when reading or writing
    // from CPU is executed"): how long an access at `port` made at cpcCycles is held, in
    // us -- until the V9990 is done with what it is busy with, never for good.
    double ioWait(int port, bool write, long long cpcCycles);
    static const char* commandName(int op);
    const V9990Picture& picture() const { return shown; }
    // Anything on the chip's monitor (not in stand-by, and fitted).
    bool displaying() const { return enabled && !systemReset && mode() != V9990Mode::Standby; }

    V9990Mode mode() const;
    V9990Raster rasterFor(V9990Mode m, int fieldIndex) const;
    int readStatus() const;
    // R#8 VSL (p.82): 00 128K, 01 256K, 10 512K; 11 acts as 512K (Powergraph notes).
    int vramBytes() const;
    // The debugger's views. A dot of the image space coloured as the display would colour
    // it (9.1: BP2..BD16, YJK/YUV); in P1/P2 the 4-bit dot through palette 0-15. A palette
    // entry. Both 0xAABBGGRR, opaque.
    uint32_t imageColour(int x, int y) const;
    uint32_t paletteEntry(int index) const { return paletteColour(index) | 0xff000000u; }
    int vramWriteAddress() const { return vramAddress(0); }    // R#0-2
    int vramReadAddress() const { return vramAddress(3); }     // R#3-5
    int vramIndex(int cpuAddress) const { return mapCpuAddress(cpuAddress); }   // P#0's address -> vram[]
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
    void latchFieldScroll();
    void drawLine(int visibleLine);
    void drawLineColours(int visibleLine, uint32_t* out);
    uint32_t bitmapDot(int x, int imageY) const;
    uint32_t yjkDot(int x, int imageY, bool yuv) const;
    uint32_t cursorOver(uint32_t colour, int displayX, int displayY) const;
    uint32_t p1Dot(int displayX, int displayY, int rowA, int rowB) const;
    uint32_t p2Dot(int displayX, int displayY, int rowA) const;
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
    void writeCommandData(int value);
    int readCommandData();
    void movePointer(int opcode);
    double stepUs(int op) const;
    void runEngine(double untilUs);
    void engineStep();
    bool walkNext();
    double commandLeftUs() const;
    int dotLogical(int x, int y) const;
    int readLinear(int address) const { return vram[v9990TransformBx(address)]; }
    // A VRAM smaller than 512K (VSL) leaves the upper address lines undriven: a write
    // lands in every mirror of the byte, a read comes from the first.
    void storeVram(int physical, int value);
    int loadVram(int physical) const;
    int lmmvColour(int column, int x, int y) const;
    void writeLinear(int address, int value);
};

} // namespace cpcse
