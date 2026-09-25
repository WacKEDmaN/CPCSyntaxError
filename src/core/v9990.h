// CPCSyntaxError — Yamaha V9990 PowerGraph.
// VRAM, bitmap/pattern display, sprites, cursors, palette and commands.
// Renders into a headless framebuffer.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int V9990_CPC_PORT_BASE = 0xff60;
inline constexpr int V9990_VRAM_SIZE = 512 * 1024;

int v9990TransformBx(int address);

struct V9990Layout {
    int left, top, width, height;
    double scaleX, scaleY;
    bool interpolateX, interpolateY;
};
V9990Layout v9990RenderLayout(const std::string& mode, int sourceWidth, int sourceHeight,
    int outputWidth = 768, int outputHeight = 544, bool stretched = false);

struct V9990Geometry { std::string mode; int width, height, lineWidth; bool bitmap; };
struct V9990Transfer { std::string type; int x, y, remainingX, remainingY, originX; int lowByte = -1; };
struct P1Sprite { int x; bool behind; int palette; int patternAddress; };
struct V9990Cursor { int cursor; int x; int y; uint32_t color; bool doBackgroundXor; std::array<uint32_t, 32> pattern; };

class V9990 {
public:
    std::function<void(int)> irq;
    bool enabled = false;
    Bytes vram;
    std::array<uint8_t, 256> palette{};
    std::array<uint8_t, 64> registers{};
    int regSelect = 0xff;
    int readBuffer = 0;
    bool systemReset = false;
    int statusLatch = 0;
    int outputControl = 0;
    int pendingIrq = 0;
    int commandStatus = 0;
    std::optional<V9990Transfer> command;
    std::deque<int> commandReadQueue;
    int borderX = 0;
    int frameCounter = 0;
    bool lastDisplayActive = false;

    std::unique_ptr<std::unordered_map<int, std::vector<P1Sprite>>> p1SpriteLineCache;
    std::vector<uint32_t> outputPixels; // headless framebuffer (filled by render)

    explicit V9990(std::function<void(int)> irq = nullptr);
    void setEnabled(bool enabled = false);
    void reset();
    bool handlesPort(int port);
    bool displayActive() { return enabled && !systemReset && (registers[8] & 0x80) != 0; }
    bool spritesEnabled() { return (registers[8] & 0x40) == 0; }
    std::string displayMode();
    int colorDepth() { return registers[6] & 3; }
    int bitsPerPixel();
    double pixelsPerByte();
    int imageWidth() { return 256 << ((registers[6] & 0x0c) >> 2); }
    V9990Geometry displayGeometry();
    int mapCpuAddress(int address);
    int getAddress(int base);
    void setAddress(int base, int address);
    int readPort(int port, long long tStates = 0);
    bool writePort(int port, int value, long long tStates = 0);
    int readStatus(long long tStates = 0);
    int readRegister(int index);
    void writeRegister(int index, int value);
    void advancePalettePointer();
    void writePalette(int value);
    std::array<int, 3> paletteRgb(int index);
    uint32_t paletteCanvasColor(int index);
    std::array<int, 3> fixed256Rgb(int index);
    uint32_t fixed256CanvasColor(int index);
    std::string colorMode();
    int pixelAddress(int x, int y, int bpp);
    int pixelAddress(int x, int y) { return pixelAddress(x, y, bitsPerPixel()); }
    int getPixel(int x, int y);
    void setPixel(int x, int y, int source);
    int word(int index) { return registers[index] | registers[index + 1] << 8; }
    int sourceX() { return word(32) & 0x7ff; }
    int sourceY() { return word(34) & 0x0fff; }
    int destX() { return word(36) & 0x7ff; }
    int destY() { return word(38) & 0x0fff; }
    int sizeX() { return (word(40) & 0x0fff) ? (word(40) & 0x0fff) : 2048; }
    int sizeY() { return (word(42) & 0x0fff) ? (word(42) & 0x0fff) : 4096; }
    int linearSource() { return (registers[32] | (word(34) & 0x7ff) << 8) & (V9990_VRAM_SIZE - 1); }
    int linearDest() { return (registers[36] | (word(38) & 0x7ff) << 8) & (V9990_VRAM_SIZE - 1); }
    int linearSize();
    void finishCommand();
    void startCommand(int opcode);
    V9990Transfer makeTransferState(const std::string& type);
    void advanceTransfer(V9990Transfer& state, int pixels = 1);
    void writeCommandData(int value);
    int readCommandData();
    void commandLmmv();
    void commandLmmm();
    void prepareLmcm();
    void commandCmmm();
    void commandBmxl();
    void commandBmlx();
    void commandBmll();
    void commandLine();
    void commandSearch();
    void commandPoint();
    int readBx(int address) { return vram[v9990TransformBx(address)]; }
    void writeBx(int address, int value) { vram[v9990TransformBx(address)] = (uint8_t)(value & 0xff); }
    void raiseIrqIfNeeded();
    void endFrame();
    void notifyDisplayChange();
    uint32_t pixelCanvasColor(int x, int y);
    std::array<int, 3> pixelRgb(int x, int y);
    int scrollAX() { return registers[19] + registers[20] * 8; }
    int scrollAY() { return registers[17] + registers[18] * 256; }
    int scrollBX() { return registers[23] + registers[24] * 8; }
    int scrollBY() { return registers[21] + registers[22] * 256; }
    int verticalRollMask(int maxMask = 0x1fff);
    int scrolledDisplayY(int displayY, int maxMask = 0x1fff);
    int p1LayerPixel(char layer, int x, int y);
    std::vector<P1Sprite> p1VisibleSprites(int displayY);
    std::optional<uint32_t> p1SpritePixel(int displayX, int displayY, bool foregroundOpaque);
    uint32_t p1DisplayCanvasColor(int displayX, int displayY);
    std::vector<V9990Cursor> cursorDescriptors();
    uint32_t compositeCursorColor(uint32_t background, int displayX, int displayY, const std::vector<V9990Cursor>& cursors);
    uint32_t displayPixelCanvasColor(int displayX, int displayY, int scrollX, int scrollY, const std::vector<V9990Cursor>& cursors);
    uint32_t interpolatedDisplayCanvasColor(double x, double y, int scrollX, int scrollY, const std::vector<V9990Cursor>& cursors, bool interpolateX = true, bool interpolateY = false);
    bool render(int width, int height, bool stretched = false); // headless: fills outputPixels
};

} // namespace cpcse
