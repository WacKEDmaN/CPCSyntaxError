// CPCSyntaxError — Yamaha V9990 E-VDP-III. See v9990.h; page numbers are the manual's.
#include "v9990.h"

namespace cpcse {

static const int VRAM_MASK = V9990_VRAM_SIZE - 1;
// P#6 / R#9 (p.76, p.82)
static const int IRQ_VI = 0x01, IRQ_HI = 0x02, IRQ_CE = 0x04;
// P#5 (p.76)
static const int STATUS_TR = 0x80, STATUS_VR = 0x40, STATUS_HR = 0x20, STATUS_BD = 0x10;
static const int STATUS_MCS = 0x04, STATUS_EO = 0x02, STATUS_CE = 0x01;
// R#44 (p.52)
static const int ARG_DIY = 0x08, ARG_DIX = 0x04, ARG_NEQ = 0x02, ARG_MAJ = 0x01;

// 12.2 (p.80-85): the bits each register holds. A bit the manual shows as 0 cannot be set.
static const uint8_t REG_MASK[29] = {
    0xff, 0xff, 0x87,           // R#0-2  VRAM write address (R#2: CVWAIH + A18-16)
    0xff, 0xff, 0x87,           // R#3-5  VRAM read address
    0xff, 0x7f, 0xff,           // R#6-8  screen mode, control
    0x07, 0xff, 0x83, 0x0f,     // R#9-12 interrupt
    0xff, 0xff, 0x3f, 0xff,     // R#13-16 palette control/pointer, back drop, adjust
    0xff, 0xdf, 0x07, 0xff,     // R#17-20 scroll A
    0xff, 0x01, 0x07, 0x3f,     // R#21-24 scroll B
    0x0f, 0x1f, 0x0f, 0x0f,     // R#25-28 SGBA, LCD, priority, sprite palette
};

static int expand5(int v) { v &= 31; return v << 3 | v >> 2; }
static uint32_t rgb5(int r, int g, int b) {
    return 0xff000000u | (uint32_t)expand5(b) << 16 | (uint32_t)expand5(g) << 8 | (uint32_t)expand5(r);
}
static int clamp31(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }

int v9990TransformBx(int address) {
    address &= VRAM_MASK;
    return (address & 1) << 18 | (address & 0x7fffe) >> 1;
}

const char* v9990ModeName(V9990Mode m) {
    static const char* names[] = { "P1", "P2", "B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "stand-by" };
    return names[(int)m];
}

// 11.2 (p.53): WC for the bit with (SC, DC) = (0,0) is L00, (0,1) L01, (1,0) L10, (1,1) L11.
static int logicalOp(int op, int source, int dest, int mask) {
    int out = 0;
    for (int bit = 0; bit < 16; bit++) {
        const int index = ((source >> bit) & 1) << 1 | ((dest >> bit) & 1);
        if ((op >> index) & 1) out |= 1 << bit;
    }
    return out & mask;
}

V9990::V9990() {
    vram.assign(V9990_VRAM_SIZE, 0);
    // DRAM at power-on is not blank; the familiar stripes of a real board.
    for (int base = 0; base < (int)vram.size(); base += 1024)
        for (int a = base + 512; a < base + 1024; a++) vram[a] = 0xff;
    reset();
}

void V9990::setEnabled(bool on) { enabled = on; }

void V9990::reset() {
    registers.fill(0);
    palette.fill(0);
    regSelect = 0;
    readBuffer = 0;
    systemReset = false;
    mcs = false;
    outputControl = 0;
    outputControlWritten = false;
    pendingIrq = 0;
    commandStatus = 0;
    borderX = 0;
    transfer.reset();
    commandReadQueue.clear();
    field = 0;
    line = 0;
    lineStartUs = nowUs;
    hiFiredThisLine = false;
    raster = rasterFor(mode(), 0);
    work.width = raster.visibleDots();
    work.height = raster.visibleLines();
    work.pixels.assign((size_t)work.width * work.height, 0xff000000u);
    work.ys.assign((size_t)work.width * work.height, 0);
    work.mode = mode();
    shown = work;
}

bool V9990::handlesPort(int port) const {
    return enabled && (port & 0xfff0) == V9990_CPC_PORT_BASE;
}

// ============================================================== modes and raster
V9990Mode V9990::mode() const {
    const int r6 = registers[6], r7 = registers[7];
    switch (r6 >> 6) {                     // DSPM (p.80)
        case 0: return V9990Mode::P1;
        case 1: return V9990Mode::P2;
        case 3: return V9990Mode::Standby;
        default: break;
    }
    // HSCN picks the high-scan modes whatever DCKM says -- the manual's own sample code
    // (p.110) sets DCKM=3 for them, a value R#6's table does not list.
    if (r7 & 0x01) return (r7 & 0x40) ? V9990Mode::B6 : V9990Mode::B5;
    switch ((r6 >> 4) & 3) {               // DCKM with P#7 MCS (p.80, table 8.3 p.17)
        case 0: return mcs ? V9990Mode::B0 : V9990Mode::B1;
        case 1: return mcs ? V9990Mode::B2 : V9990Mode::B3;
        default: return mcs ? V9990Mode::B4 : V9990Mode::B7;
    }
}

V9990Raster V9990::rasterFor(V9990Mode m, int fieldIndex) const {
    V9990Raster r;
    const int r7 = registers[7];
    const bool pal = r7 & 0x08, sm = r7 & 0x10, sm1 = r7 & 0x20, interlace = r7 & 0x02;
    if (m == V9990Mode::B5) {              // 13.3.12 high scan, 400 lines
        r.clockMHz = 21.47727; r.lineClocks = 848; r.hsync = 64; r.leftErase = 80;
        r.leftBorder = 0; r.display = 640; r.rightBorder = 0; r.clocksPerDot = 1;
        r.topBorder = 0; r.displayLines = 400; r.bottomBorder = 0; r.totalLines = 440;
        return r;
    }
    if (m == V9990Mode::B6) {              // 480 lines on the 25.175 MHz clock
        r.clockMHz = 25.175; r.lineClocks = 800; r.hsync = 96; r.leftErase = 48;
        r.leftBorder = 0; r.display = 640; r.rightBorder = 0; r.clocksPerDot = 1;
        r.topBorder = 0; r.displayLines = 480; r.bottomBorder = 0; r.totalLines = 525;
        return r;
    }
    const bool mckin = m == V9990Mode::B0 || m == V9990Mode::B2 || m == V9990Mode::B4;
    if (mckin) {                           // "when using 14MHz": 912 clocks, no borders
        r.clockMHz = 14.31818; r.lineClocks = sm ? 910 : 912; r.hsync = 68; r.leftErase = sm ? 54 : 56;
        r.leftBorder = 0; r.display = 768; r.rightBorder = 0;
        r.clocksPerDot = m == V9990Mode::B0 ? 4 : m == V9990Mode::B2 ? 2 : 1;
    } else {                               // "when using 21MHz": 1368 clocks
        r.clockMHz = 21.47727; r.lineClocks = sm ? 1365 : 1368; r.hsync = 100; r.leftErase = sm ? 97 : 100;
        r.leftBorder = 56; r.display = 1024; r.rightBorder = 56;
        r.clocksPerDot = (m == V9990Mode::P2 || m == V9990Mode::B3) ? 2 : m == V9990Mode::B7 ? 1 : 4;
    }
    // 13.3.13 (p.103). Interlace: 312.5 / 262.5 lines a field, as 313+312 / 263+262.
    if (pal) {
        r.topBorder = 41; r.displayLines = 212; r.bottomBorder = 37;
        r.totalLines = interlace ? (fieldIndex ? 312 : 313) : 313;
    } else {
        r.topBorder = 14; r.displayLines = 212; r.bottomBorder = 14;
        r.totalLines = interlace ? (fieldIndex ? 262 : 263) : (sm1 ? 263 : 262);
    }
    // "The border period becomes the display period during over-scan" (B0, B2, B4).
    if (mckin) {
        r.displayLines += r.topBorder + r.bottomBorder;
        r.topBorder = r.bottomBorder = 0;
    }
    return r;
}

void V9990::advanceTo(long long cpcCycles) {
    const double target = cpcCycles / 4.0;
    if (!enabled) { nowUs = lineStartUs = target; return; }
    if (target <= nowUs) return;
    // Plugged in (or the machine held) for a long while: pick the raster up where it is
    // rather than draw every field that went by.
    if (target - lineStartUs > 250000.0) { lineStartUs = target; line = 0; hiFiredThisLine = false; }
    for (;;) {
        // HI (p.82-83): at IX x 64 master clocks from the display start of line IL (lines
        // counted from the display start line), or of every line with IEHM.
        if (!hiFiredThisLine) {
            const int il = registers[10] | (registers[11] & 3) << 8;
            const bool everyLine = registers[11] & 0x80;
            if (everyLine || line - raster.topBorder == il) {
                const double at = lineStartUs + (raster.displayStartClocks() + 64 * (registers[12] & 15)) / raster.clockMHz;
                if (target >= at) { pendingIrq |= IRQ_HI; hiFiredThisLine = true; }
            }
        }
        const double end = lineStartUs + raster.lineUs();
        if (target < end) break;
        lineStartUs = end;
        line += 1;
        hiFiredThisLine = false;
        if (line >= raster.totalLines) endField();
        beginLine();
    }
    nowUs = target;
}

void V9990::beginLine() {
    // VI (p.76): "vertical display period completion" -- the first line after it.
    if (line == raster.topBorder + raster.displayLines) pendingIrq |= IRQ_VI;
    if (line < raster.visibleLines()) drawLine(line);
}

void V9990::endField() {
    shown.width = work.width;
    shown.height = work.height;
    shown.pixels = work.pixels;
    shown.ys = work.ys;
    shown.mode = work.mode;
    shown.fields += 1;
    shown.startUs = (raster.hsync + raster.leftErase) / raster.clockMHz;
    shown.spanUs = (double)raster.visibleDots() * raster.clocksPerDot / raster.clockMHz;
    shown.lines = raster.visibleLines();
    const bool interlace = registers[7] & 0x02;
    field = interlace ? field ^ 1 : 0;
    line = 0;
    const V9990Mode m = mode();
    raster = rasterFor(m, field);
    const int w = raster.visibleDots(), h = raster.visibleLines() * (interlace ? 2 : 1);
    if (w != work.width || h != work.height) {
        work.width = w; work.height = h;
        work.pixels.assign((size_t)w * h, 0xff000000u);
        work.ys.assign((size_t)w * h, 0);
    }
    work.mode = m;
}

int V9990::readStatus() {
    const double clocks = (nowUs - lineStartUs) * raster.clockMHz;
    const int start = raster.displayStartClocks();
    const bool hr = !(clocks >= start && clocks < start + raster.display);
    const int displayLine = line - raster.topBorder;
    const bool vr = !(displayLine >= 0 && displayLine < raster.displayLines);
    const bool eo = (registers[7] & 0x02) && field == 1;
    return (commandStatus & (STATUS_TR | STATUS_BD | STATUS_CE)) | (vr ? STATUS_VR : 0) | (hr ? STATUS_HR : 0)
         | (mcs ? STATUS_MCS : 0) | (eo ? STATUS_EO : 0);
}

// ============================================================== ports (12.1)
int V9990::vramAddress(int base) const {
    return (registers[base] | registers[base + 1] << 8 | (registers[base + 2] & 7) << 16) & VRAM_MASK;
}
void V9990::setVramAddress(int base, int address) {
    address &= VRAM_MASK;
    registers[base] = (uint8_t)address;
    registers[base + 1] = (uint8_t)(address >> 8);
    registers[base + 2] = (uint8_t)((registers[base + 2] & 0x80) | (address >> 16 & 7));
}
// The CPU's view of VRAM (10.1.16 p.33, 10.2.14 p.38, 10.11 p.47).
int V9990::mapCpuAddress(int address) const {
    address &= VRAM_MASK;
    switch (mode()) {
        case V9990Mode::P1: return address;                    // upper bit picks VRAM0/1
        case V9990Mode::P2:
            if (address < 0x78000) return v9990TransformBx(address);   // PGT: lowest bit
            if (address < 0x7c000) return address - 0x3c000;           // SPAT: VRAM0
            return address;                                            // PNT: VRAM1
        default: return v9990TransformBx(address);
    }
}

int V9990::readPort(int port, long long cpcCycles) {
    if (!handlesPort(port)) return 0xff;
    advanceTo(cpcCycles);
    const int id = port & 15;
    if (systemReset && id != 7) return 0xff;
    switch (id) {
        case 0: {                          // 7.2: the byte prepared, then the next prepared
            const int value = readBuffer;
            if (!(registers[5] & 0x80)) {
                const int next = (vramAddress(3) + 1) & VRAM_MASK;
                setVramAddress(3, next);
                readBuffer = vram[mapCpuAddress(next)];
            }
            return value;
        }
        case 1: {                          // PLTAIH (R#13 b4) holds the pointer on a read only
            const int value = palette[registers[14]];
            if (!(registers[13] & 0x10)) advancePalettePointer();
            return value;
        }
        case 2: return readCommandData();
        case 3: {
            const int value = readRegister(regSelect & 63);
            if (!(regSelect & 0x40)) regSelect = (regSelect & 0xc0) | ((regSelect + 1) & 63);
            return value;
        }
        case 5: return readStatus();
        case 6: return pendingIrq & 7;
        case 15: return outputControl;
        default: return 0xff;              // P#4, P#7 write-only; no kanji ROM on the GFX9000
    }
}

bool V9990::writePort(int port, int value, long long cpcCycles) {
    if (!handlesPort(port)) return false;
    advanceTo(cpcCycles);
    value &= 0xff;
    const int id = port & 15;
    if (id == 7) {                         // P#7 (p.76)
        mcs = value & 1;
        const bool srs = value & 2;
        if (srs && !systemReset) {         // "all ports except this one in power ON reset state"
            registers.fill(0); regSelect = 0; pendingIrq = 0; commandStatus = 0;
            transfer.reset(); commandReadQueue.clear();
        }
        systemReset = srs;
        return true;
    }
    if (systemReset) return true;
    switch (id) {
        case 0: {
            const int address = vramAddress(0);
            vram[mapCpuAddress(address)] = (uint8_t)value;
            if (!(registers[2] & 0x80)) setVramAddress(0, address + 1);
            break;
        }
        case 1: {                          // 7.3 / p.75: R: YS + 5 bits, G and B 5 bits
            const int pointer = registers[14];
            palette[pointer] = (uint8_t)(value & ((pointer & 3) == 0 ? 0x9f : 0x1f));
            advancePalettePointer();       // a write always advances (PLTAIH is for read-out)
            break;
        }
        case 2: writeCommandData(value); break;
        case 3:
            writeRegister(regSelect & 63, value);
            if (!(regSelect & 0x80)) regSelect = (regSelect & 0xc0) | ((regSelect + 1) & 63);
            break;
        case 4: regSelect = value; break;
        case 6: pendingIrq &= ~value; break;   // "By writing 1, the related bit flag is reset"
        case 15: outputControl = value; outputControlWritten = true; break;
        default: break;
    }
    return true;
}

int V9990::readRegister(int index) {
    index &= 63;
    if (index <= 5 || index == 13 || index == 14 || index == 28 || (index >= 29 && index <= 52) || index > 54) return 0xff;
    if (index == 53) return borderX & 0xff;
    if (index == 54) return borderX >> 8 & 7;
    return registers[index];
}

void V9990::writeRegister(int index, int value) {
    index &= 63;
    if (index >= 53 || (index >= 29 && index <= 31)) return;
    if (index < 29) value &= REG_MASK[index];
    registers[index] = (uint8_t)value;
    if (index == 5) readBuffer = vram[mapCpuAddress(vramAddress(3))];   // p.75: prepared on R#5
    if (index == 52) startCommand(value);
}

// R#14's low two bits count R, G, B (p.83) and then move on to the next colour.
void V9990::advancePalettePointer() {
    const int pointer = registers[14];
    registers[14] = (uint8_t)((pointer & 3) >= 2 ? (pointer & 0xfc) + 4 : pointer + 1);
}

uint32_t V9990::paletteColour(int index) const {
    const int at = (index & 63) << 2;
    const uint32_t c = rgb5(palette[at], palette[at + 1], palette[at + 2]);
    return (palette[at] & 0x80) ? c & 0x00ffffffu : c;   // YS: alpha 0 until drawLine files it
}

// ============================================================== image space
int V9990::bitsPerDot() const {
    const V9990Mode m = mode();
    if (m == V9990Mode::P1 || m == V9990Mode::P2) return 4;
    static const int bpp[4] = { 2, 4, 8, 16 };
    return bpp[registers[6] & 3];
}
int V9990::imageWidth() const {
    const V9990Mode m = mode();
    if (m == V9990Mode::P1) return 256;    // 10.1.12: PGT laid out 256 dots wide
    if (m == V9990Mode::P2) return 512;    // 10.2.10: 512 dots wide
    return 256 << ((registers[6] >> 2) & 3);
}
int V9990::imageHeight() const {
    const V9990Mode m = mode();
    if (m == V9990Mode::P1 || m == V9990Mode::P2) return 2048;
    return (V9990_VRAM_SIZE * 8) / (imageWidth() * bitsPerDot());
}

int V9990::dotAddress(int x, int y) const {
    const V9990Mode m = mode();
    if (m == V9990Mode::P1) {
        // 11.2 (p.51): "screen A is selected at DX9=0 and screen B at DX9=1"
        const int screen = (x >> 9) & 1;
        return (screen << 18) + (y & 0x7ff) * 128 + ((x & 255) >> 1);
    }
    if (m == V9990Mode::P2) return mapCpuAddress((y & 0x7ff) * 256 + ((x & 511) >> 1));
    const int w = imageWidth(), bpp = bitsPerDot();
    x &= w - 1;
    y &= imageHeight() - 1;
    const int dot = x + y * w;
    const int byte = bpp == 16 ? dot * 2 : bpp == 8 ? dot : bpp == 4 ? dot >> 1 : dot >> 2;
    return v9990TransformBx(byte);
}

int V9990::getDot(int x, int y) const {
    const int a = dotAddress(x, y), bpp = bitsPerDot();
    if (bpp == 16) return vram[a] | vram[a ^ 0x40000] << 8;   // the odd byte is in VRAM1
    const int v = vram[a];
    if (bpp == 8) return v;
    if (bpp == 4) return (x & 1) ? v & 15 : v >> 4;
    return v >> (6 - 2 * (x & 3)) & 3;
}

void V9990::setDot(int x, int y, int source) {
    const int bpp = bitsPerDot();
    const int all = bpp == 16 ? 0xffff : (1 << bpp) - 1;
    source &= all;
    if ((registers[45] & 0x10) && source == 0) return;          // TP
    const int result = logicalOp(registers[45] & 15, source, getDot(x, y), all);
    const int a = dotAddress(x, y);
    // WM7-0 guard VRAM0 and WM15-8 VRAM1, bit for bit (p.53)
    if (bpp == 16) {
        vram[a] = (uint8_t)((vram[a] & ~registers[46]) | (result & registers[46]));
        const int hi = a ^ 0x40000;
        vram[hi] = (uint8_t)((vram[hi] & ~registers[47]) | ((result >> 8) & registers[47]));
        return;
    }
    const int wm = (a & 0x40000) ? registers[47] : registers[46];
    int shift = 0, dotMask = 0xff;
    if (bpp == 4) { shift = (x & 1) ? 0 : 4; dotMask = 15 << shift; }
    else if (bpp == 2) { shift = 6 - 2 * (x & 3); dotMask = 3 << shift; }
    const int mask = dotMask & wm;
    vram[a] = (uint8_t)((vram[a] & ~mask) | ((result << shift) & mask));
}

// FC/BC (p.53): "Correspondence with VRAM bit position is the same as write mask" -- a dot
// takes the bits of the colour register that sit where the dot sits in its byte.
int V9990::fontColourAt(int x, int y, bool foreground) const {
    const int fc = wordAt(foreground ? 48 : 50);
    const int bpp = bitsPerDot();
    if (bpp == 16) return fc;
    const int byte = (dotAddress(x, y) & 0x40000) ? fc >> 8 : fc & 0xff;
    if (bpp == 8) return byte;
    if (bpp == 4) return (x & 1) ? byte & 15 : byte >> 4;
    return byte >> (6 - 2 * (x & 3)) & 3;
}

// ============================================================== the picture
// R#17/18 (p.84): display start line SCAY; R512/R256 roll the displayed page by 512 or
// 256 lines, else by the image size.
int V9990::scrolledY(int displayY, int scrollY, int imageLines) const {
    int roll = imageLines - 1;
    switch (registers[18] >> 6) { case 1: roll = 255; break; case 2: roll = 511; break; case 3: roll = 255; break; default: break; }
    roll &= imageLines - 1;
    return ((scrollY & ~roll) + ((displayY + scrollY) & roll)) & (imageLines - 1);
}

void V9990::drawLine(int visibleLine) {
    const bool interlace = registers[7] & 0x02;
    const int row = interlace ? visibleLine * 2 + field : visibleLine;
    if (row >= work.height) return;
    uint32_t* out = &work.pixels[(size_t)row * work.width];
    drawLineColours(visibleLine, out);
    // File the YS bits: R#8 YSE turns the system on (Video9000 manual p.14); the
    // high-scan modes cannot be superimposed (p.18).
    const V9990Mode m = mode();
    const bool yse = (registers[8] & 0x20) && m != V9990Mode::B5 && m != V9990Mode::B6;
    uint8_t* ys = &work.ys[(size_t)row * work.width];
    for (int c = 0; c < work.width; c++) {
        ys[c] = yse && !(out[c] >> 24);
        out[c] |= 0xff000000u;
    }
}

void V9990::drawLineColours(int visibleLine, uint32_t* out) {
    const bool interlace = registers[7] & 0x02;
    const V9990Mode m = mode();
    if (m == V9990Mode::Standby || systemReset) { std::fill(out, out + work.width, 0xff000000u); return; }
    const uint32_t backdrop = paletteColour(registers[15]);
    std::fill(out, out + work.width, backdrop);
    if (!(registers[8] & 0x80)) return;                 // DISP=0: back drop all over
    // R#16 (p.84): 0 standard, 1..7 up/left, 8..15 down/right; horizontally in units of
    // 4 master clocks (P1/B1 1 dot, P2/B2/B3 2, B4-B6 4).
    const int adjH = registers[16] & 15, adjV = registers[16] >> 4;
    const int shiftH = adjH < 8 ? adjH : adjH - 16, shiftV = adjV < 8 ? adjV : adjV - 16;
    const int displayY = visibleLine - raster.topBorder + shiftV;
    if (displayY < 0 || displayY >= raster.displayLines) return;
    const int shiftDots = 4 * shiftH / raster.clocksPerDot;
    const int dots = raster.dots(), border = raster.borderDots();
    const int first = std::max(0, border - shiftDots), last = std::min(work.width, border - shiftDots + dots);
    if (m == V9990Mode::P1 || m == V9990Mode::P2) {
        const bool sprites = !(registers[8] & 0x40);
        if (sprites) collectSprites(displayY, m == V9990Mode::P2); else lineSprites.clear();
        for (int c = first; c < last; c++) {
            const int x = c - border + shiftDots;
            out[c] = m == V9990Mode::P1 ? p1Dot(x, displayY, 0) : p2Dot(x, displayY);
        }
        return;
    }
    const bool eo = registers[7] & 0x04;
    const int lineInImage = (interlace && eo) ? displayY * 2 + field : displayY;
    const int imageY = scrolledY(lineInImage, registers[17] | (registers[18] & 0x1f) << 8, imageHeight());
    const int scrollX = (registers[19] & 7) | registers[20] << 3;
    const int w = imageWidth();
    const int palettePlan = registers[13] >> 6;         // PLTM
    const bool yjk = bitsPerDot() == 8 && palettePlan >= 2;
    for (int c = first; c < last; c++) {
        const int x = c - border + shiftDots;
        const int ix = (x + scrollX) & (w - 1);
        const uint32_t colour = yjk ? yjkDot(ix, imageY, palettePlan == 3) : bitmapDot(ix, imageY);
        out[c] = cursorOver(colour, x, (interlace && eo) ? lineInImage : displayY);
    }
}

// 9.1 (p.19-28): what a bit-map dot means.
uint32_t V9990::bitmapDot(int x, int imageY) const {
    const int v = getDot(x, imageY);
    const int r13 = registers[13];
    switch (bitsPerDot()) {
        case 16: {                                                  // BD16: YS G5 R5 B5
            const uint32_t c = rgb5(v >> 5, v >> 10, v);
            return (v & 0x8000) ? c & 0x00ffffffu : c;
        }
        case 8:
            if ((r13 >> 6) == 1) {                                  // BD8: G3 R3 B2 (p.26)
                const int g = v >> 5 & 7, r = v >> 2 & 7, b = v & 3;
                const uint32_t c = rgb5(r << 2 | r >> 1, g << 2 | g >> 1, b << 3 | b << 1 | (b >> 1 | (b & 1)));
                return v == 0 ? c & 0x00ffffffu : c;                // p.26: "WHEN VD0~VD7 = ALL 0" -> YS
            }
            return paletteColour(v & 63);                           // BP6
        case 4: return paletteColour(((r13 & 0x0c) << 2) | v);      // BP4: PLTO5,4 + 4 bits
        default: return paletteColour(((r13 & 0x0f) << 2) | v);     // BP2: PLTO5-2 + 2 bits
    }
}

// 17 (p.115-116): YJK / YUV share their colour across each group of four dots.
uint32_t V9990::yjkDot(int x, int imageY, bool yuv) const {
    const int group = x & ~3;
    int d[4];
    for (int i = 0; i < 4; i++) d[i] = getDot(group + i, imageY);
    const int me = d[x & 3];
    const bool withAttribute = registers[13] & 0x20;              // YAE
    if (withAttribute && (me & 0x08)) return paletteColour(((registers[13] & 0x0c) << 2) | me >> 4);
    int lo = (d[0] & 7) | (d[1] & 7) << 3, hi = (d[2] & 7) | (d[3] & 7) << 3;
    if (lo >= 32) lo -= 64;
    if (hi >= 32) hi -= 64;
    const int y = withAttribute ? (me >> 4) << 1 : me >> 3;
    int r, g, b;
    if (yuv) {                              // V = dots 1,2; U = dots 3,4
        const int v = lo, u = hi;
        r = y + u; b = y + v; g = (5 * y - 2 * u - v) >> 2;
    } else {                                // K = dots 1,2; J = dots 3,4
        const int k = lo, j = hi;
        r = y + j; g = y + k; b = (5 * y - 2 * j - k) >> 2;
    }
    return rgb5(clamp31(r), clamp31(g), clamp31(b));
}

// 10.12/10.13 (p.48-49): two 32x32 cursors, cursor 0 in front.
uint32_t V9990::cursorOver(uint32_t colour, int displayX, int displayY) const {
    if (registers[8] & 0x40) return colour;                      // SPD
    for (int c = 0; c < 2; c++) {
        const int at = 0x7fe00 + c * 8;
        const int attr = readLinear(at + 6);
        if (attr & 0x10) continue;                                // PR0: not displayed
        const int cc = attr >> 6, eor = attr & 0x20;
        const int y = (readLinear(at) | (readLinear(at + 2) & 1) << 8) + ((registers[7] & 0x02) ? 2 : 1);
        const int ln = (displayY - y) & 511;
        if (ln >= 32) continue;
        const int x = readLinear(at + 4) | (attr & 3) << 8;
        const int col = (displayX - x) & 1023;
        if (col >= 32) continue;
        const int bits = readLinear(0x7ff00 + c * 0x80 + ln * 4 + (col >> 3));
        if (!((bits >> (7 - (col & 7))) & 1)) continue;
        if (cc == 0) {
            if (eor) return colour ^ 0x00ffffffu;                 // EOR colour on the image
            continue;                                             // clear colour
        }
        return paletteColour(((registers[28] & 15) << 2) | cc);
    }
    return colour;
}

int V9990::patternDot(int nameBase, int patternBase, int patternsPerRow, int rowBytes, int x, int y, bool p2) const {
    const int columns = p2 ? 128 : 64;
    const int nameAt = nameBase + ((y >> 3) * columns + (x >> 3)) * 2;
    const int lo = p2 ? vram[mapCpuAddress(nameAt)] : vram[nameAt & VRAM_MASK];
    const int hi = p2 ? vram[mapCpuAddress(nameAt + 1)] : vram[(nameAt + 1) & VRAM_MASK];
    const int pattern = (lo | hi << 8) & (p2 ? 0x3fff : 0x1fff);
    const int logical = patternBase + (pattern / patternsPerRow) * rowBytes * 8 + (y & 7) * rowBytes
                      + (pattern % patternsPerRow) * 4 + ((x & 7) >> 1);
    const int v = p2 ? vram[mapCpuAddress(logical)] : vram[logical & VRAM_MASK];
    return (x & 1) ? v & 15 : v >> 4;
}

// 10.1.13/10.2.11: up to 16 sprites on a line, the smaller number in front.
void V9990::collectSprites(int displayY, bool p2) {
    lineSprites.clear();
    const int table = p2 ? 0x7be00 : 0x3fe00;
    auto spat = [&](int a) { return p2 ? vram[mapCpuAddress(a)] : vram[a & VRAM_MASK]; };
    const int base = p2 ? (registers[25] & 0x0f) << 15 : (registers[25] & 0x0e) << 14;
    int onLine = 0;
    for (int s = 0; s < 125 && onLine < 16; s++) {
        const int at = table + s * 4;
        const int ln = (displayY - ((spat(at) + 1) & 0xff)) & 0xff;   // shown at Y + 1
        if (ln >= 16) continue;
        onLine += 1;
        const int attr = spat(at + 3);
        if (attr & 0x10) continue;                                  // PR0
        int x = spat(at + 2) | (attr & 3) << 8;
        if (x > 1008) x -= 1024;                                    // X rolls at 1024
        const int number = spat(at + 1);
        const int perRow = p2 ? 32 : 16, rowBytes = p2 ? 256 : 128;
        const int address = base + (number / perRow) * rowBytes * 16 + ln * rowBytes + (number % perRow) * 8;
        lineSprites.push_back({ x, (attr & 0x20) != 0, (attr >> 2) & 0x30, address });
    }
}

std::optional<uint32_t> V9990::spriteDot(int displayX, bool frontOpaque, bool p2) const {
    for (const LineSprite& s : lineSprites) {
        const int col = displayX - s.x;
        if (col < 0 || col >= 16) continue;
        if (s.behind && frontOpaque) continue;                      // PR1: behind the front
        const int logical = s.address + (col >> 1);
        const int v = p2 ? vram[mapCpuAddress(logical)] : vram[logical & VRAM_MASK];
        const int dot = (col & 1) ? v & 15 : v >> 4;
        if (dot) return paletteColour(s.palette | dot);
    }
    return std::nullopt;
}

uint32_t V9990::p1Dot(int x, int displayY, int) const {
    const int scrollAX = (registers[19] & 7) | registers[20] << 3;
    const int scrollAY = registers[17] | (registers[18] & 0x1f) << 8;
    const int scrollBX = (registers[23] & 7) | (registers[24] & 0x3f) << 3;
    const int scrollBY = registers[21] | (registers[22] & 1) << 8;
    const int ax = (x + scrollAX) & 511, ay = scrolledY(displayY, scrollAY, 512);
    const int bx = (x + scrollBX) & 511, by = (displayY + scrollBY) & 511;
    const int a = patternDot(0x7c000, 0x00000, 32, 128, ax, ay, false);
    const int b = patternDot(0x7e000, 0x40000, 32, 128, bx, by, false);
    // R#27 (p.85): B in front right of PRX x 64 dots or below PRY x 64 lines; 0 = never.
    const int prx = registers[27] & 3, pry = (registers[27] >> 2) & 3;
    const bool aFront = (prx == 0 || x < prx * 64) && (pry == 0 || displayY < pry * 64);
    const int paletteA = (registers[13] & 3) << 4, paletteB = ((registers[13] >> 2) & 3) << 4;   // 9.1.1
    const int front = aFront ? a : b, back = aFront ? b : a;
    uint32_t colour = front ? paletteColour((aFront ? paletteA : paletteB) | front)
                    : back ? paletteColour((aFront ? paletteB : paletteA) | back)
                    : paletteColour(registers[15]);
    if (auto s = spriteDot(x, front != 0, false)) colour = *s;
    return colour;
}

uint32_t V9990::p2Dot(int x, int displayY) const {
    const int scrollAX = (registers[19] & 7) | registers[20] << 3;
    const int scrollAY = registers[17] | (registers[18] & 0x1f) << 8;
    const int ix = (x + scrollAX) & 1023, iy = scrolledY(displayY, scrollAY, 512);
    const int v = patternDot(0x7c000, 0x00000, 64, 256, ix, iy, true);
    // 9.1.1: dots 8n+0,1,4,5 take PLTO3,2; 8n+2,3,6,7 take PLTO5,4
    const int plt = (ix & 2) ? ((registers[13] >> 2) & 3) << 4 : (registers[13] & 3) << 4;
    uint32_t colour = v ? paletteColour(plt | v) : paletteColour(registers[15]);
    if (auto s = spriteDot(x, v != 0, true)) colour = *s;
    return colour;
}

// ============================================================== commands (chapter 11)
int V9990::linearSize() const {
    const int v = (registers[40] | (wordAt(42) & 0x7ff) << 8) & VRAM_MASK;
    return v ? v : V9990_VRAM_SIZE;
}

void V9990::writeLinear(int address, int value) {
    if ((registers[45] & 0x10) && value == 0) return;              // TP, by byte
    const int a = v9990TransformBx(address);
    const int old = vram[a];
    const int result = logicalOp(registers[45] & 15, value, old, 0xff);
    const int wm = (a & 0x40000) ? registers[47] : registers[46];
    vram[a] = (uint8_t)((old & ~wm) | (result & wm));
}

void V9990::finishCommand() {
    transfer.reset();
    commandReadQueue.clear();
    commandStatus &= ~(STATUS_CE | STATUS_TR);
    pendingIrq |= IRQ_CE;                                           // 11.1
}

void V9990::stepTransfer(Transfer& t) {
    const int dx = (registers[44] & ARG_DIX) ? -1 : 1, dy = (registers[44] & ARG_DIY) ? -1 : 1;
    t.x = (t.x + dx) & 0x7ff;
    if (--t.remainingX > 0) return;
    t.remainingX = sizeX();
    t.x = t.originX;
    t.y = (t.y + dy) & 0xfff;
    if (--t.remainingY <= 0) finishCommand();
}

// The dots of the source rectangle, packed as 11.4 (p.55) lays them out, for the CPU.
void V9990::queueDots(int x0, int y0) {
    const int dx = (registers[44] & ARG_DIX) ? -1 : 1, dy = (registers[44] & ARG_DIY) ? -1 : 1;
    const int bpp = bitsPerDot(), nx = sizeX(), ny = sizeY();
    int packed = 0, filled = 0;
    for (int row = 0, y = y0; row < ny; row++, y += dy)
        for (int col = 0, x = x0; col < nx; col++, x += dx) {
            const int v = getDot(x, y);
            if (bpp == 16) { commandReadQueue.push_back(v & 0xff); commandReadQueue.push_back(v >> 8); continue; }
            packed |= v << (8 - bpp * (filled + 1));
            if (++filled == 8 / bpp) { commandReadQueue.push_back(packed & 0xff); packed = 0; filled = 0; }
        }
    if (filled) commandReadQueue.push_back(packed & 0xff);
}

void V9990::startCommand(int opcode) {
    transfer.reset();
    commandReadQueue.clear();
    commandStatus |= STATUS_CE;
    const int dx = (registers[44] & ARG_DIX) ? -1 : 1, dy = (registers[44] & ARG_DIY) ? -1 : 1;
    const int bpp = bitsPerDot();
    switch (opcode >> 4) {                                          // 11.3 (p.54)
        case 0: finishCommand(); return;                            // STOP
        case 1: case 5:                                             // LMMC, CMMC: from the CPU
            transfer = Transfer{ opcode >> 4, destX(), destY(), sizeX(), sizeY(), destX(), -1 };
            commandStatus |= STATUS_TR;
            return;
        case 2: {                                                   // LMMV
            const int nx = sizeX(), ny = sizeY();
            for (int row = 0, y = destY(); row < ny; row++, y += dy)
                for (int col = 0, x = destX(); col < nx; col++, x += dx) setDot(x, y, fontColourAt(x, y, true));
            break;
        }
        case 3: case 13:                                            // LMCM, POINT: to the CPU
            if ((opcode >> 4) == 3) queueDots(sourceX(), sourceY());
            else {
                const int v = getDot(sourceX(), sourceY());          // p.56
                if (bpp == 16) { commandReadQueue.push_back(v & 0xff); commandReadQueue.push_back(v >> 8); }
                else commandReadQueue.push_back((v << (8 - bpp)) & 0xff);
            }
            commandStatus |= STATUS_TR;
            return;
        case 4: {                                                   // LMMM
            const int nx = sizeX(), ny = sizeY();
            for (int row = 0, sy = sourceY(), ty = destY(); row < ny; row++, sy += dy, ty += dy)
                for (int col = 0, sx = sourceX(), tx = destX(); col < nx; col++, sx += dx, tx += dx)
                    setDot(tx, ty, getDot(sx, sy));
            break;
        }
        case 6: break;                                              // CMMK: the GFX9000 has no kanji ROM
        case 7: {                                                   // CMMM: bits from linear SA
            Transfer t{ 7, destX(), destY(), sizeX(), sizeY(), destX(), -1 };
            transfer = t;
            int address = linearSource();
            while (transfer) {
                const int bits = readLinear(address++);
                for (int b = 7; b >= 0 && transfer; b--) {
                    setDot(transfer->x, transfer->y, fontColourAt(transfer->x, transfer->y, (bits >> b) & 1));
                    stepTransfer(*transfer);
                }
            }
            return;                                                 // stepTransfer finished it
        }
        case 8: {                                                   // BMXL: linear -> rectangle
            transfer = Transfer{ 8, destX(), destY(), sizeX(), sizeY(), destX(), -1 };
            int address = linearSource();
            while (transfer) {
                const int v = readLinear(address++);
                if (bpp == 16) {
                    const int hi = readLinear(address++);
                    setDot(transfer->x, transfer->y, v | hi << 8);
                    stepTransfer(*transfer);
                } else {
                    for (int i = 0; i < 8 / bpp && transfer; i++) {
                        setDot(transfer->x, transfer->y, v >> (8 - bpp * (i + 1)));
                        stepTransfer(*transfer);
                    }
                }
            }
            return;
        }
        case 9: {                                                   // BMLX: rectangle -> linear
            queueDots(sourceX(), sourceY());
            int address = linearDest();
            for (int v : commandReadQueue) writeLinear(address++, v);
            commandReadQueue.clear();
            break;
        }
        case 10: {                                                  // BMLL: linear -> linear
            const int delta = (registers[44] & ARG_DIX) ? -1 : 1;
            int s = linearSource(), d = linearDest();
            for (int n = linearSize(); n > 0; n--) {
                writeLinear(d, readLinear(s));
                s = (s + delta) & VRAM_MASK; d = (d + delta) & VRAM_MASK;
            }
            break;
        }
        case 11: commandLine(); break;
        case 12: commandSearch(); break;
        case 14: {                                                  // PSET, then move the pointer
            const int x = destX(), y = destY();
            setDot(x, y, fontColourAt(x, y, true));
            movePointer(opcode);
            break;
        }
        case 15: movePointer(opcode); break;                        // ADVN
        default: break;
    }
    finishCommand();
}

void V9990::writeCommandData(int value) {
    if (!transfer) return;
    Transfer& t = *transfer;
    const int bpp = bitsPerDot();
    if (t.kind == 5) {                                              // CMMC: 1 = FC, 0 = BC
        for (int b = 7; b >= 0 && transfer; b--) {
            setDot(transfer->x, transfer->y, fontColourAt(transfer->x, transfer->y, (value >> b) & 1));
            stepTransfer(*transfer);
        }
        return;
    }
    if (bpp == 16) {                                                // LMMC: low, then high
        if (t.lowByte < 0) { t.lowByte = value; return; }
        setDot(t.x, t.y, t.lowByte | value << 8);
        t.lowByte = -1;
        stepTransfer(t);
        return;
    }
    for (int i = 0; i < 8 / bpp && transfer; i++) {
        setDot(transfer->x, transfer->y, value >> (8 - bpp * (i + 1)));
        stepTransfer(*transfer);
    }
}

int V9990::readCommandData() {
    if (commandReadQueue.empty()) return 0xff;
    const int v = commandReadQueue.front();
    commandReadQueue.pop_front();
    if (commandReadQueue.empty()) finishCommand();
    return v;
}

// 11.5.11 (p.69): MJ along the major axis (X, or Y with MAJ), MI along the other.
void V9990::commandLine() {
    int x = destX(), y = destY();
    const int mj = wordAt(40) & 0xfff, mi = wordAt(42) & 0xfff;
    const int sx = (registers[44] & ARG_DIX) ? -1 : 1, sy = (registers[44] & ARG_DIY) ? -1 : 1;
    const bool yMajor = registers[44] & ARG_MAJ;
    int error = mj / 2;
    for (int i = 0; i <= mj; i++) {
        setDot(x, y, fontColourAt(x, y, true));
        if (yMajor) y = (y + sy) & 0xfff; else x = (x + sx) & 0x7ff;
        error -= mi;
        if (error < 0) {
            if (yMajor) x = (x + sx) & 0x7ff; else y = (y + sy) & 0xfff;
            error += mj;
        }
    }
}

// 11.5.12 (p.70): from (SX, SY) towards DIX until the FC colour (or, with NEQ, another).
void V9990::commandSearch() {
    const int dir = (registers[44] & ARG_DIX) ? -1 : 1;
    const bool neq = registers[44] & ARG_NEQ;
    const int w = mode() == V9990Mode::P1 ? 1024 : imageWidth();
    const int y = sourceY();
    int x = sourceX();
    commandStatus &= ~STATUS_BD;
    for (int n = 0; n < w; n++, x = (x + dir) & 0x7ff) {
        const bool equal = getDot(x, y) == fontColourAt(x, y, true);
        if (equal != neq) { commandStatus |= STATUS_BD; break; }
    }
    borderX = x & 0x7ff;
}

// PSET / ADVN (p.54): AXE/AXM and AYE/AYM move DX, DY by one dot.
void V9990::movePointer(int opcode) {
    int x = destX(), y = destY();
    if (opcode & 0x01) x = (x + ((opcode & 0x02) ? -1 : 1)) & 0x7ff;
    if (opcode & 0x04) y = (y + ((opcode & 0x08) ? -1 : 1)) & 0xfff;
    registers[36] = (uint8_t)x; registers[37] = (uint8_t)(x >> 8);
    registers[38] = (uint8_t)y; registers[39] = (uint8_t)(y >> 8);
}

} // namespace cpcse
