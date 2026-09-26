// CPCSyntaxError — Yamaha V9990 PowerGraph.
#include "v9990.h"

namespace cpcse {

static const int VRAM_MASK = V9990_VRAM_SIZE - 1;
static const int IRQ_VI = 0x01, IRQ_HI = 0x02, IRQ_CE = 0x04;
static const int STATUS_TR = 0x80, STATUS_VR = 0x40, STATUS_HR = 0x20;
static const int STATUS_BD = 0x10, STATUS_MCS = 0x04, STATUS_EO = 0x02, STATUS_CE = 0x01;
static const int ARG_DIX = 0x04, ARG_DIY = 0x08;

static const uint8_t REG_MASK[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x87, 0xff, 0x83, 0x0f, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xdf, 0x07, 0xff, 0xff, 0xc1, 0x07,
    0x3f, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static int expand5(int value) { value &= 31; return value << 3 | ((unsigned)value >> 2); }
static int wrapv(int value, int size) { return ((value % size) + size) % size; }

int v9990TransformBx(int address) {
    address &= VRAM_MASK;
    return ((address & 1) << 18) | (((unsigned)(address & 0x7fffe)) >> 1);
}

static int logicalBit(int op, int source, int destination) {
    int index = (source << 1) | destination;
    return (unsigned)op >> index & 1;
}
static int logicalValue(int op, int source, int destination, int mask) {
    op &= 0x0f;
    int out = 0;
    for (int bit = 0; bit < 16; bit += 1) {
        if (logicalBit(op, (unsigned)source >> bit & 1, (unsigned)destination >> bit & 1)) out |= 1 << bit;
    }
    return out & mask;
}

V9990Layout v9990RenderLayout(const std::string& mode, int sourceWidth, int sourceHeight,
    int outputWidth, int outputHeight, bool stretched) {
    if (stretched) return { 0, 0, outputWidth, outputHeight,
        (double)outputWidth / sourceWidth, (double)outputHeight / sourceHeight, true, true };
    int targetWidth;
    bool interpolateX = false;
    if (mode == "B2") targetWidth = std::min(outputWidth, sourceWidth * 2);
    else if (mode == "B3") targetWidth = std::min(outputWidth, sourceWidth);
    else if (mode == "B4") targetWidth = std::min(outputWidth, sourceWidth);
    else if (mode == "B7") { targetWidth = outputWidth; interpolateX = sourceWidth > targetWidth; }
    else if (mode == "B0") targetWidth = std::min(outputWidth, sourceWidth * 3);
    else if (mode == "B1" || mode == "P1") targetWidth = std::min(outputWidth, sourceWidth * 2);
    else if (mode == "P2") targetWidth = std::min(outputWidth, sourceWidth);
    else targetWidth = std::min(outputWidth, sourceWidth);
    int targetHeight = std::min(outputHeight, sourceHeight <= 300 ? sourceHeight * 2 : sourceHeight);
    return {
        (int)std::floor((outputWidth - targetWidth) / 2.0),
        (int)std::floor((outputHeight - targetHeight) / 2.0),
        targetWidth, targetHeight,
        (double)targetWidth / sourceWidth, (double)targetHeight / sourceHeight,
        interpolateX, false
    };
}

V9990::V9990(std::function<void(int)> irq) : irq(irq) {
    enabled = false;
    vram.assign(V9990_VRAM_SIZE, 0);
    for (int base = 0; base < (int)vram.size(); base += 1024) for (int a = base + 512; a < base + 1024; a++) vram[a] = 0xff;
    reset();
}
void V9990::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
}
void V9990::reset() {
    registers.fill(0);
    palette.fill(0);
    regSelect = 0xff;
    readBuffer = 0;
    systemReset = false;
    statusLatch = 0;
    outputControl = 0;
    pendingIrq = 0;
    commandStatus = 0;
    command.reset();
    commandReadQueue.clear();
    borderX = 0;
    frameCounter = 0;
    lastDisplayActive = false;
}
bool V9990::handlesPort(int port) {
    if (!enabled || (port & 0xfff0) != V9990_CPC_PORT_BASE) return false;
    int id = port & 15;
    return id <= 7 || id == 15;
}
std::string V9990::displayMode() {
    int r6 = registers[6];
    switch (r6 & 0xc0) {
        case 0x00: return "P1";
        case 0x40: return "P2";
        case 0x80: {
            bool mcs = !!(statusLatch & STATUS_MCS);
            switch (r6 & 0x30) {
                case 0x00: return mcs ? "B0" : "B1";
                case 0x10: return mcs ? "B2" : "B3";
                case 0x20: return mcs ? "B4" : "B7";
                default: return "B1";
            }
        }
        default: return "P1";
    }
}
int V9990::bitsPerPixel() { static const int t[4] = { 2, 4, 8, 16 }; int v = t[colorDepth()]; return v ? v : 4; }
double V9990::pixelsPerByte() { return bitsPerPixel() == 16 ? 0.5 : 8.0 / bitsPerPixel(); }
V9990Geometry V9990::displayGeometry() {
    std::string mode = displayMode();
    struct G { int w, h, l; };
    std::unordered_map<std::string, G> table = {
        {"P1",{256,212,320}}, {"P2",{512,212,640}},
        {"B0",{192,240,213}}, {"B1",{256,212,320}}, {"B2",{384,240,426}},
        {"B3",{512,212,640}}, {"B4",{768,240,853}},
        {"B5",{640,400,800}}, {"B6",{640,480,800}}, {"B7",{1024,212,1280}},
    };
    G g = table.count(mode) ? table[mode] : table["B1"];
    return { mode, g.w, g.h, g.l, mode[0] == 'B' };
}
int V9990::mapCpuAddress(int address) {
    address &= VRAM_MASK;
    std::string mode = displayMode();
    if (mode == "P1") return address;
    if (mode == "P2") {
        if (address < 0x78000) return v9990TransformBx(address);
        if (address < 0x7c000) return address - 0x3c000;
        return address;
    }
    return v9990TransformBx(address);
}
int V9990::getAddress(int base) {
    return (registers[base] | registers[base + 1] << 8 | (registers[base + 2] & 7) << 16) & VRAM_MASK;
}
void V9990::setAddress(int base, int address) {
    address &= VRAM_MASK;
    registers[base] = (uint8_t)(address & 0xff);
    registers[base + 1] = (uint8_t)((unsigned)address >> 8 & 0xff);
    registers[base + 2] = (uint8_t)((registers[base + 2] & 0x80) | ((unsigned)address >> 16 & 7));
}
int V9990::readPort(int port, long long tStates) {
    if (!handlesPort(port)) return 0xff;
    int id = port & 15;
    if (systemReset && id != 7 && id != 15) return 0xff;
    switch (id) {
        case 0: {
            int value = readBuffer;
            if (!(registers[5] & 0x80)) {
                int next = (getAddress(3) + 1) & VRAM_MASK;
                setAddress(3, next);
                readBuffer = vram[mapCpuAddress(next)];
            }
            return value;
        }
        case 1: {
            int value = palette[registers[14]];
            advancePalettePointer();
            return value;
        }
        case 2: return readCommandData();
        case 3: {
            int value = readRegister(regSelect & 0x3f);
            if (!(regSelect & 0x40)) regSelect = (regSelect + 1) & ~0x40;
            return value;
        }
        case 5: return readStatus(tStates);
        case 6: return pendingIrq & 7;
        case 15: return outputControl;
        default: return 0xff;
    }
}
bool V9990::writePort(int port, int value, long long tStates) {
    (void)tStates;
    if (!handlesPort(port)) return false;
    value &= 0xff;
    int id = port & 15;
    if (systemReset && id != 7 && id != 15) {
        if (id == 4) regSelect = 0;
        return true;
    }
    switch (id) {
        case 0: {
            int address = getAddress(0);
            vram[mapCpuAddress(address)] = (uint8_t)value;
            if (!(registers[2] & 0x80)) setAddress(0, address + 1);
            break;
        }
        case 1: writePalette(value); break;
        case 2: writeCommandData(value); break;
        case 3:
            writeRegister(regSelect & 0x3f, value);
            if (!(regSelect & 0x80)) regSelect = (regSelect & 0xc0) | ((regSelect + 1) & 0x3f);
            break;
        case 4: regSelect = value; break;
        case 6: pendingIrq &= ~value; break;
        case 7: {
            statusLatch = (statusLatch & ~STATUS_MCS) | ((value & 1) ? STATUS_MCS : 0);
            bool reset0 = !!(value & 2);
            if (reset0 && !systemReset) {
                registers.fill(0); commandStatus = 0; command.reset(); pendingIrq = 0;
            }
            systemReset = reset0;
            break;
        }
        case 15: outputControl = value; break;
        default: break;
    }
    notifyDisplayChange();
    return true;
}
int V9990::readStatus(long long tStates) {
    int framePhase = wrapv((int)tStates, 80000);
    int linePhase = framePhase % 256;
    bool vr = framePhase >= 68000;
    bool hr = linePhase < 36 || linePhase >= 220;
    return (commandStatus & (STATUS_TR | STATUS_BD | STATUS_CE))
        | (vr ? STATUS_VR : 0) | (hr ? STATUS_HR : 0)
        | (statusLatch & (STATUS_MCS | STATUS_EO));
}
int V9990::readRegister(int index) {
    index &= 63;
    if (systemReset) return 0xff;
    if (index <= 5 || index == 13 || index == 14 || index == 28 || (index >= 29 && index <= 52)) return 0xff;
    if (index == 53) return borderX & 0xff;
    if (index == 54) return (unsigned)borderX >> 8 & 7;
    return registers[index];
}
void V9990::writeRegister(int index, int value) {
    index &= 63;
    if (index >= 53 || (index >= 29 && index <= 31)) return;
    if (index < 32) value &= REG_MASK[index];
    registers[index] = (uint8_t)value;
    if (index == 5) {
        int address = getAddress(3);
        readBuffer = vram[mapCpuAddress(address)];
    }
    if (index == 52) startCommand(value);
}
void V9990::advancePalettePointer() {
    if (registers[13] & 0x10) return;
    int pointer = registers[14];
    switch (pointer & 3) {
        case 0: case 1: registers[14] = (uint8_t)((pointer + 1) & 0xff); break;
        case 2: registers[14] = (uint8_t)((pointer + 2) & 0xff); break;
        default: registers[14] = (uint8_t)((pointer - 3) & 0xff); break;
    }
}
void V9990::writePalette(int value) {
    int pointer = registers[14];
    int part = pointer & 3;
    palette[pointer] = (uint8_t)(part == 0 ? value & 0x9f : (part == 1 || part == 2) ? value & 0x1f : 0);
    advancePalettePointer();
}
std::array<int, 3> V9990::paletteRgb(int index) {
    index &= 63; int at = index << 2;
    return { expand5(palette[at]), expand5(palette[at + 1]), expand5(palette[at + 2]) };
}
uint32_t V9990::paletteCanvasColor(int index) {
    index &= 63; int at = index << 2;
    int r = expand5(palette[at]), g = expand5(palette[at + 1]), b = expand5(palette[at + 2]);
    return (uint32_t)(0xff000000u | b << 16 | g << 8 | r);
}
std::array<int, 3> V9990::fixed256Rgb(int index) {
    static const int mapRG[8] = { 0, 4, 9, 13, 18, 22, 27, 31 };
    static const int mapB[4] = { 0, 11, 21, 31 };
    return { expand5(mapRG[(unsigned)index >> 2 & 7]), expand5(mapRG[(unsigned)index >> 5 & 7]), expand5(mapB[index & 3]) };
}
uint32_t V9990::fixed256CanvasColor(int index) {
    static const int mapRG[8] = { 0, 4, 9, 13, 18, 22, 27, 31 };
    static const int mapB[4] = { 0, 11, 21, 31 };
    int r = expand5(mapRG[(unsigned)index >> 2 & 7]), g = expand5(mapRG[(unsigned)index >> 5 & 7]), b = expand5(mapB[index & 3]);
    return (uint32_t)(0xff000000u | b << 16 | g << 8 | r);
}
std::string V9990::colorMode() {
    if (!(registers[6] & 0x80)) return "BP4";
    switch (registers[6] & 3) {
        case 0: return "BP2";
        case 1: return "BP4";
        case 2: { static const char* t[4] = { "BP6", "BD8", "BYJK", "BYUV" }; return t[(unsigned)registers[13] >> 6 & 3]; }
        case 3: return "BD16";
        default: return "BP4";
    }
}
int V9990::pixelAddress(int x, int y, int bpp) {
    int width = imageWidth();
    int byteAddress = bpp == 16 ? 2 * (x + y * width)
        : bpp == 8 ? x + y * width
        : bpp == 4 ? ((unsigned)(x + y * width) >> 1)
        : ((unsigned)(x + y * width) >> 2);
    return v9990TransformBx(byteAddress) & VRAM_MASK;
}
int V9990::getPixel(int x, int y) {
    int width = imageWidth();
    x = wrapv(x, width);
    int bpp = bitsPerPixel();
    int address = pixelAddress(x, y, bpp);
    if (bpp == 16) {
        int low = vram[address];
        int high = vram[pixelAddress(x, y, bpp) ^ 0x40000];
        return low | high << 8;
    }
    int value = vram[address];
    if (bpp == 8) return value;
    if (bpp == 4) return x & 1 ? value & 15 : (unsigned)value >> 4;
    return (unsigned)value >> (6 - 2 * (x & 3)) & 3;
}
void V9990::setPixel(int x, int y, int source) {
    int log = registers[45], mask = word(46);
    int width = imageWidth();
    x = wrapv(x, width);
    y &= 0x0fff;
    int bpp = bitsPerPixel();
    int mx = bpp == 16 ? 0xffff : (1 << bpp) - 1;
    source &= mx;
    int destination = getPixel(x, y);
    if ((log & 0x10) && source == 0) return;
    int result = logicalValue(log, source, destination, mx);
    int address = pixelAddress(x, y, bpp);
    if (bpp == 16) {
        int loMask = mask & 0xff, hiMask = (unsigned)mask >> 8;
        int highAddress = address ^ 0x40000;
        vram[address] = (uint8_t)((vram[address] & ~loMask) | (result & loMask));
        vram[highAddress] = (uint8_t)((vram[highAddress] & ~hiMask) | ((unsigned)result >> 8 & hiMask));
        return;
    }
    int bankMask = address & 0x40000 ? (unsigned)mask >> 8 & 0xff : mask & 0xff;
    int old = vram[address];
    if (bpp == 8) vram[address] = (uint8_t)((old & ~bankMask) | (result & bankMask));
    else if (bpp == 4) {
        int pixelMask = (x & 1 ? 0x0f : 0xf0) & bankMask;
        int shifted = x & 1 ? result : result << 4;
        vram[address] = (uint8_t)((old & ~pixelMask) | (shifted & pixelMask));
    } else {
        int shift = 6 - 2 * (x & 3);
        int pixelMask = (3 << shift) & bankMask;
        vram[address] = (uint8_t)((old & ~pixelMask) | (result << shift & pixelMask));
    }
}
int V9990::linearSize() { int v = (registers[40] | (word(42) & 0x7ff) << 8) & VRAM_MASK; return v ? v : V9990_VRAM_SIZE; }
void V9990::finishCommand() {
    command.reset();
    commandStatus &= ~(STATUS_CE | STATUS_TR);
    pendingIrq |= IRQ_CE;
    raiseIrqIfNeeded();
}
void V9990::startCommand(int opcode) {
    int operation = (unsigned)opcode >> 4;
    commandStatus = (commandStatus & ~STATUS_BD) | STATUS_CE;
    commandReadQueue.clear();
    command.reset();
    if (operation == 0) { finishCommand(); return; }
    if (operation == 1 || operation == 5) {
        command = makeTransferState(operation == 1 ? "LMMC" : "CMMC");
        commandStatus |= STATUS_TR;
        return;
    }
    if (operation == 3) { prepareLmcm(); return; }
    switch (operation) {
        case 2: commandLmmv(); break;
        case 4: commandLmmm(); break;
        case 6: finishCommand(); break;
        case 7: commandCmmm(); break;
        case 8: commandBmxl(); break;
        case 9: commandBmlx(); break;
        case 10: commandBmll(); break;
        case 11: commandLine(); break;
        case 12: commandSearch(); break;
        case 13: commandPoint(); break;
        case 14: setPixel(destX(), destY(), word(48)); finishCommand(); break;
        case 15: finishCommand(); break;
        default: finishCommand(); break;
    }
}
V9990Transfer V9990::makeTransferState(const std::string& type) {
    return { type, destX(), destY(), sizeX(), sizeY(), destX(), -1 };
}
void V9990::advanceTransfer(V9990Transfer& state, int pixels) {
    int dx = registers[44] & ARG_DIX ? -1 : 1;
    int dy = registers[44] & ARG_DIY ? -1 : 1;
    while (pixels-- > 0 && state.remainingY > 0) {
        state.x = (state.x + dx) & 0x7ff;
        state.remainingX -= 1;
        if (state.remainingX <= 0) {
            state.remainingY -= 1;
            if (state.remainingY <= 0) break;
            state.x = state.originX;
            state.y = (state.y + dy) & 0x0fff;
            state.remainingX = sizeX();
        }
    }
    if (state.remainingY <= 0) finishCommand();
}
void V9990::writeCommandData(int value) {
    if (!command || !(commandStatus & STATUS_CE)) return;
    commandStatus &= ~STATUS_TR;
    V9990Transfer& state = *command;
    if (state.type == "CMMC") {
        for (int bit = 7; bit >= 0 && state.remainingY > 0; bit -= 1) {
            setPixel(state.x, state.y, (unsigned)value >> bit & 1 ? word(48) : word(50));
            advanceTransfer(state);
            if (!command) break;
        }
    } else if (state.type == "LMMC") {
        int bpp = bitsPerPixel();
        if (bpp == 16) {
            if (state.lowByte == -1) state.lowByte = value;
            else {
                setPixel(state.x, state.y, state.lowByte | value << 8);
                state.lowByte = -1; advanceTransfer(state);
            }
        } else {
            int count = 8 / bpp;
            int pixelMask = (1 << bpp) - 1;
            for (int i = 0; i < count && state.remainingY > 0; i += 1) {
                setPixel(state.x, state.y, (unsigned)value >> (8 - bpp * (i + 1)) & pixelMask);
                advanceTransfer(state);
                if (!command) break;
            }
        }
    }
    if (command) commandStatus |= STATUS_TR;
}
int V9990::readCommandData() {
    if (!(commandStatus & STATUS_TR)) return 0xff;
    int value = commandReadQueue.empty() ? -1 : commandReadQueue.front();
    if (!commandReadQueue.empty()) commandReadQueue.pop_front();
    if (commandReadQueue.empty()) finishCommand();
    return value == -1 ? 0xff : value;
}
void V9990::commandLmmv() {
    int dx = registers[44] & ARG_DIX ? -1 : 1;
    int dy = registers[44] & ARG_DIY ? -1 : 1;
    int originX = destX(), nx = sizeX(), ny = sizeY(), color = word(48);
    for (int row = 0, y = destY(); row < ny; row += 1, y += dy)
        for (int col = 0, x = originX; col < nx; col += 1, x += dx) setPixel(x, y, color);
    finishCommand();
}
void V9990::commandLmmm() {
    int dx = registers[44] & ARG_DIX ? -1 : 1;
    int dy = registers[44] & ARG_DIY ? -1 : 1;
    int nx = sizeX(), ny = sizeY();
    for (int row = 0, sy = sourceY(), dyPos = destY(); row < ny; row += 1, sy += dy, dyPos += dy)
        for (int col = 0, sx = sourceX(), dxPos = destX(); col < nx; col += 1, sx += dx, dxPos += dx)
            setPixel(dxPos, dyPos, getPixel(sx, sy));
    finishCommand();
}
void V9990::prepareLmcm() {
    int dx = registers[44] & ARG_DIX ? -1 : 1;
    int dy = registers[44] & ARG_DIY ? -1 : 1;
    std::vector<int> values;
    for (int row = 0, y = sourceY(); row < sizeY(); row += 1, y += dy)
        for (int col = 0, x = sourceX(); col < sizeX(); col += 1, x += dx) values.push_back(getPixel(x, y));
    int bpp = bitsPerPixel();
    if (bpp == 16) {
        for (int pixel : values) { commandReadQueue.push_back(pixel & 0xff); commandReadQueue.push_back((unsigned)pixel >> 8 & 0xff); }
    } else {
        int perByte = 8 / bpp, mask = (1 << bpp) - 1;
        for (int i = 0; i < (int)values.size(); i += perByte) {
            int packed = 0;
            for (int p = 0; p < perByte; p += 1) packed |= ((i + p < (int)values.size() ? values[i + p] : 0) & mask) << (8 - bpp * (p + 1));
            commandReadQueue.push_back(packed & 0xff);
        }
    }
    commandStatus |= STATUS_TR;
}
void V9990::commandCmmm() {
    int address = linearSource();
    V9990Transfer state = makeTransferState("CMMM");
    command = state;
    while (command && command->remainingY > 0) {
        int byte = readBx(address++);
        for (int bit = 7; bit >= 0 && command && command->remainingY > 0; bit -= 1) {
            setPixel(command->x, command->y, (unsigned)byte >> bit & 1 ? word(48) : word(50));
            advanceTransfer(*command);
        }
    }
    if (command) finishCommand();
}
void V9990::commandBmxl() {
    int address = linearSource();
    command = makeTransferState("BMXL");
    int bpp = bitsPerPixel();
    while (command && command->remainingY > 0) {
        int value = readBx(address++);
        if (bpp == 16) {
            int high = readBx(address++); setPixel(command->x, command->y, value | high << 8); advanceTransfer(*command);
        } else {
            int count = 8 / bpp, mask = (1 << bpp) - 1;
            for (int i = 0; i < count && command && command->remainingY > 0; i += 1) {
                setPixel(command->x, command->y, (unsigned)value >> (8 - bpp * (i + 1)) & mask); advanceTransfer(*command);
            }
        }
    }
    if (command) finishCommand();
}
void V9990::commandBmlx() {
    int address = linearDest();
    std::vector<int> values;
    int dx = registers[44] & ARG_DIX ? -1 : 1, dy = registers[44] & ARG_DIY ? -1 : 1;
    for (int row = 0, y = sourceY(); row < sizeY(); row += 1, y += dy)
        for (int col = 0, x = sourceX(); col < sizeX(); col += 1, x += dx) values.push_back(getPixel(x, y));
    int bpp = bitsPerPixel();
    if (bpp == 16) for (int value : values) { writeBx(address++, value); writeBx(address++, (unsigned)value >> 8); }
    else {
        int perByte = 8 / bpp, mask = (1 << bpp) - 1;
        for (int i = 0; i < (int)values.size(); i += perByte) {
            int packed = 0;
            for (int p = 0; p < perByte; p += 1) packed |= ((i + p < (int)values.size() ? values[i + p] : 0) & mask) << (8 - bpp * (p + 1));
            writeBx(address++, packed);
        }
    }
    finishCommand();
}
void V9990::commandBmll() {
    int source = linearSource(), destination = linearDest(), remaining = linearSize();
    int delta = registers[44] & ARG_DIX ? -1 : 1;
    while (remaining-- > 0) {
        int src = readBx(source);
        int dst = readBx(destination);
        if (!((registers[45] & 0x10) && src == 0)) {
            int result = logicalValue(registers[45], src, dst, 0xff);
            int phys = v9990TransformBx(destination);
            int wm = phys & 0x40000 ? registers[47] : registers[46];
            writeBx(destination, (dst & ~wm) | (result & wm));
        }
        source = (source + delta) & VRAM_MASK; destination = (destination + delta) & VRAM_MASK;
    }
    finishCommand();
}
void V9990::commandLine() {
    int x = destX(), y = destY();
    int major = sizeX(), minor = sizeY();
    int sx = registers[44] & ARG_DIX ? -1 : 1, sy = registers[44] & ARG_DIY ? -1 : 1;
    bool yMajor = !!(registers[44] & 1);
    int error = (unsigned)major >> 1;
    for (int i = 0; i <= major; i += 1) {
        setPixel(x, y, word(48));
        if (yMajor) y += sy; else x += sx;
        error -= minor;
        if (error < 0) { if (yMajor) x += sx; else y += sy; error += major; }
    }
    finishCommand();
}
void V9990::commandSearch() {
    int direction = registers[44] & ARG_DIX ? -1 : 1;
    bool neq = !!(registers[44] & 2);
    int color = word(48) & ((1 << std::min(16, bitsPerPixel())) - 1);
    int width = imageWidth();
    int x = sourceX();
    for (int count = 0; count < width; count += 1, x += direction) {
        bool equal = getPixel(x, sourceY()) == color;
        if (equal != neq) { borderX = x & 0x7ff; commandStatus |= STATUS_BD; finishCommand(); return; }
    }
    borderX = x & 0x7ff; commandStatus &= ~STATUS_BD; finishCommand();
}
void V9990::commandPoint() {
    int pixel = getPixel(sourceX(), sourceY());
    commandReadQueue.push_back(pixel & 0xff);
    if (bitsPerPixel() == 16) commandReadQueue.push_back((unsigned)pixel >> 8 & 0xff);
    commandStatus |= STATUS_TR;
}
void V9990::raiseIrqIfNeeded() {
    if ((pendingIrq & registers[9]) && irq) irq(0xff);
}
void V9990::endFrame() {
    if (!enabled) return;
    frameCounter += 1;
    statusLatch ^= STATUS_EO;
    pendingIrq |= IRQ_VI;
    if (registers[9] & IRQ_HI) pendingIrq |= IRQ_HI;
    raiseIrqIfNeeded();
    notifyDisplayChange();
}
void V9990::notifyDisplayChange() {
    bool active = displayActive();
    if (active != lastDisplayActive) lastDisplayActive = active;
}
uint32_t V9990::pixelCanvasColor(int x, int y) {
    int value = getPixel(x, y);
    std::string mode = colorMode();
    std::string dm = displayMode();
    bool hiRes = dm == "B4" || dm == "B5" || dm == "B6" || dm == "B7";
    if (mode == "BP4") {
        int base = hiRes ? ((registers[13] & 4) << 2) + (x & 1 ? 32 : 0) : ((registers[13] & 0x0c) << 2);
        return paletteCanvasColor(base + value);
    }
    if (mode == "BP2") {
        int base = hiRes ? ((registers[13] & 7) << 2) + (x & 1 ? 32 : 0) : ((registers[13] & 15) << 2);
        return paletteCanvasColor(base + value);
    }
    if (mode == "BP6") return paletteCanvasColor(value & 63);
    if (mode == "BD8") return fixed256CanvasColor(value);
    if (mode == "BD16") {
        int r = expand5((unsigned)value >> 5), g = expand5((unsigned)value >> 10), b = expand5(value);
        return (uint32_t)(0xff000000u | b << 16 | g << 8 | r);
    }
    int luminance = expand5((unsigned)value >> 3);
    return (uint32_t)(0xff000000u | luminance << 16 | luminance << 8 | luminance);
}
std::array<int, 3> V9990::pixelRgb(int x, int y) {
    uint32_t packed = pixelCanvasColor(x, y);
    return { (int)(packed & 0xff), (int)(packed >> 8 & 0xff), (int)(packed >> 16 & 0xff) };
}
int V9990::verticalRollMask(int maxMask) {
    switch ((unsigned)registers[18] >> 6) {
        case 1: return 0x00ff;
        case 2: return 0x01ff;
        case 3: return 0x00ff;
        default: return maxMask;
    }
}
int V9990::scrolledDisplayY(int displayY, int maxMask) {
    int scrollY = scrollAY();
    int rollMask = verticalRollMask(maxMask);
    int base = scrollY & ~rollMask & maxMask;
    return base + ((displayY + scrollY) & rollMask);
}
int V9990::p1LayerPixel(char layer, int x, int y) {
    x &= 511; y &= 511;
    int nameBase = layer == 'A' ? 0x7c000 : 0x7e000;
    int patternBase = layer == 'A' ? 0x00000 : 0x40000;
    int nameAddress = nameBase + ((((unsigned)y >> 3) * 64 + ((unsigned)x >> 3)) * 2);
    int pattern = (vram[nameAddress & VRAM_MASK] | vram[(nameAddress + 1) & VRAM_MASK] << 8) & 0x1fff;
    int address = patternBase
        + (int)std::floor(pattern / 32.0) * 1024
        + (y & 7) * 128
        + (pattern & 31) * 4
        + (((unsigned)(x & 7)) >> 1);
    int value = vram[address & VRAM_MASK];
    return x & 1 ? value & 15 : (unsigned)value >> 4;
}
std::vector<P1Sprite> V9990::p1VisibleSprites(int displayY) {
    int key = displayY & 0xff;
    if (p1SpriteLineCache && p1SpriteLineCache->count(key)) return (*p1SpriteLineCache)[key];
    std::vector<P1Sprite> result; int table = 0x3fe00;
    int patternBase = (registers[25] & 0x0e) << 14;
    int lineSlots = 16;
    for (int sprite = 0; sprite < 125 && lineSlots > 0; sprite += 1) {
        int at = table + sprite * 4;
        int line = (key - ((vram[at] + 1) & 0xff)) & 0xff;
        if (line >= 16) continue;
        lineSlots -= 1;
        int attr = vram[at + 3];
        if (attr & 0x10) continue;
        int x = vram[at + 2] | (attr & 3) << 8;
        if (x > 1008) x -= 1024;
        int spriteNo = vram[at + 1];
        result.push_back({ x, !!(attr & 0x20), (int)(((unsigned)attr >> 2) & 0x30),
            patternBase + 128 * ((spriteNo & 0xf0) + line) + 8 * (spriteNo & 0x0f) });
    }
    if (p1SpriteLineCache) (*p1SpriteLineCache)[key] = result;
    return result;
}
std::optional<uint32_t> V9990::p1SpritePixel(int displayX, int displayY, bool foregroundOpaque) {
    for (const P1Sprite& sprite : p1VisibleSprites(displayY)) {
        int column = displayX - sprite.x;
        if (column < 0) column += 1024;
        if (column >= 16 || (sprite.behind && foregroundOpaque)) continue;
        int packed = vram[(sprite.patternAddress + ((unsigned)column >> 1)) & VRAM_MASK];
        int pixel = column & 1 ? packed & 15 : (unsigned)packed >> 4;
        if (pixel) return paletteCanvasColor(sprite.palette + pixel);
    }
    return std::nullopt;
}
uint32_t V9990::p1DisplayCanvasColor(int displayX, int displayY) {
    displayX &= 255; displayY &= 0xff;
    int scrollAXv = scrollAX();
    int sourceAY = scrolledDisplayY(displayY, 0x01ff);
    int scrollBXv = scrollBX();
    int scrollBYv = scrollBY();
    int prioXBits = registers[27] & 3;
    int prioYBits = registers[27] & 0x0c;
    int prioX = prioXBits ? prioXBits << 6 : 256;
    int prioY = prioYBits ? prioYBits << 4 : 256;
    bool aFront = displayX < prioX && displayY < prioY;
    int aPixel = p1LayerPixel('A', displayX + scrollAXv, sourceAY);
    int bPixel = p1LayerPixel('B', displayX + scrollBXv, displayY + scrollBYv);
    int frontPixel = aFront ? aPixel : bPixel;
    int backPixel = aFront ? bPixel : aPixel;
    int paletteA = (registers[13] & 3) << 4;
    int paletteB = (registers[13] & 0x0c) << 2;
    int frontPalette = aFront ? paletteA : paletteB;
    int backPalette = aFront ? paletteB : paletteA;
    uint32_t color = frontPixel
        ? paletteCanvasColor(frontPalette + frontPixel)
        : backPixel ? paletteCanvasColor(backPalette + backPixel)
            : paletteCanvasColor(registers[15] & 63);
    std::optional<uint32_t> sprite = p1SpritePixel(displayX, displayY, frontPixel != 0);
    if (sprite) color = *sprite;
    return color;
}
std::vector<V9990Cursor> V9990::cursorDescriptors() {
    if (!spritesEnabled()) return {};
    std::vector<V9990Cursor> cursors;
    for (int cursor = 0; cursor < 2; cursor += 1) {
        int attrAddress = 0x7fe00 + cursor * 8;
        int patternAddress = 0x7ff00 + cursor * 0x80;
        int attr = readBx(attrAddress + 6);
        if ((attr & 0x10) || (attr & 0xe0) == 0) continue;
        int yDelay = registers[7] & 2 ? 2 : 1;
        int y = ((readBx(attrAddress) | (readBx(attrAddress + 2) & 1) << 8) + yDelay) & 511;
        int x = (readBx(attrAddress + 4) | (attr & 3) << 8) & 1023;
        int cc = (unsigned)attr >> 6 & 3;
        bool doBackgroundXor = (attr & 0xe0) == 0x20;
        uint32_t color = paletteCanvasColor(((registers[28] << 2) + cc) & 63);
        if ((attr & 0x20) && cc != 0) color = (uint32_t)(color ^ 0x00ffffff);
        std::array<uint32_t, 32> pattern{};
        for (int line = 0; line < 32; line += 1) {
            pattern[line] = (uint32_t)(
                (uint32_t)readBx(patternAddress + line * 4) * 0x1000000u
                + (readBx(patternAddress + line * 4 + 1) << 16)
                + (readBx(patternAddress + line * 4 + 2) << 8)
                + readBx(patternAddress + line * 4 + 3));
        }
        cursors.push_back({ cursor, x, y, color, doBackgroundXor, pattern });
    }
    std::sort(cursors.begin(), cursors.end(), [](const V9990Cursor& a, const V9990Cursor& b) { return a.cursor < b.cursor; });
    return cursors;
}
uint32_t V9990::compositeCursorColor(uint32_t background, int displayX, int displayY, const std::vector<V9990Cursor>& cursors) {
    displayX = wrapv(displayX, 1024);
    displayY = wrapv(displayY, 512);
    for (const V9990Cursor& cursor : cursors) {
        int line = wrapv(displayY - cursor.y, 512);
        int column = wrapv(displayX - cursor.x, 1024);
        if (line >= 32 || column >= 32) continue;
        if (!(cursor.pattern[line] & (0x80000000u >> column))) continue;
        return cursor.doBackgroundXor ? (uint32_t)(background ^ 0x00ffffff) : cursor.color;
    }
    return background;
}
uint32_t V9990::displayPixelCanvasColor(int displayX, int displayY, int scrollX, int scrollY, const std::vector<V9990Cursor>& cursors) {
    (void)scrollY;
    if (displayMode() == "P1") return p1DisplayCanvasColor(displayX, displayY);
    int sourceY = scrolledDisplayY(displayY, 0x1fff);
    uint32_t background = pixelCanvasColor(displayX + scrollX, sourceY);
    return compositeCursorColor(background, displayX, displayY, cursors);
}
uint32_t V9990::interpolatedDisplayCanvasColor(double x, double y, int scrollX, int scrollY, const std::vector<V9990Cursor>& cursors, bool interpolateX, bool interpolateY) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    double fx = interpolateX ? x - x0 : 0;
    double fy = interpolateY ? y - y0 : 0;
    auto sample = [&](int sx, int sy) { return displayPixelCanvasColor(sx, sy, scrollX, scrollY, cursors); };
    uint32_t a = sample(x0, y0);
    uint32_t b = fx ? sample(x0 + 1, y0) : a;
    uint32_t c = fy ? sample(x0, y0 + 1) : a;
    uint32_t d = (fx && fy) ? sample(x0 + 1, y0 + 1) : (fy ? c : b);
    auto channel = [&](int shift) {
        double av = a >> shift & 0xff, bv = b >> shift & 0xff;
        double cv = c >> shift & 0xff, dv = d >> shift & 0xff;
        double top = av + (bv - av) * fx;
        double bottom = cv + (dv - cv) * fx;
        return (int)std::lround(top + (bottom - top) * fy);
    };
    int r = channel(0), g = channel(8), blue = channel(16);
    return (uint32_t)(0xff000000u | blue << 16 | g << 8 | r);
}
bool V9990::render(int width, int height, bool stretched) {
    if (!displayActive()) return false;
    V9990Geometry geometry = displayGeometry();
    if (geometry.mode == "P1") p1SpriteLineCache = std::make_unique<std::unordered_map<int, std::vector<P1Sprite>>>();
    if ((int)outputPixels.size() != width * height) outputPixels.assign((size_t)width * height, 0);
    std::fill(outputPixels.begin(), outputPixels.end(), paletteCanvasColor(registers[15] & 63));
    if (geometry.bitmap || geometry.mode == "P1") {
        V9990Layout layout = v9990RenderLayout(geometry.mode, geometry.width, geometry.height, width, height, stretched);
        int left = layout.left, top = layout.top;
        double scaleX = layout.scaleX, scaleY = layout.scaleY;
        bool interpolateX = layout.interpolateX, interpolateY = layout.interpolateY;
        int scrollX = scrollAX();
        int scrollY = scrollAY();
        std::vector<V9990Cursor> cursors = geometry.bitmap ? cursorDescriptors() : std::vector<V9990Cursor>();
        int bottom = std::min(height, top + layout.height);
        int right = std::min(width, left + layout.width);
        for (int oy = std::max(0, top); oy < bottom; oy += 1) {
            int localY = oy - top;
            double displayY = interpolateY
                ? localY * (double)(geometry.height - 1) / std::max(1, layout.height - 1)
                : std::floor(localY / scaleY);
            for (int ox = std::max(0, left); ox < right; ox += 1) {
                int localX = ox - left;
                uint32_t packed;
                if (interpolateX || interpolateY) {
                    double displayX = stretched
                        ? localX * (double)(geometry.width - 1) / std::max(1, layout.width - 1)
                        : interpolateX ? (localX + 0.5) / scaleX - 0.5 : std::floor(localX / scaleX);
                    packed = interpolatedDisplayCanvasColor(displayX, displayY, scrollX, scrollY, cursors, interpolateX, interpolateY);
                } else {
                    int displayX = (int)std::floor(localX / scaleX);
                    packed = displayPixelCanvasColor(displayX, (int)displayY, scrollX, scrollY, cursors);
                }
                outputPixels[(size_t)oy * width + ox] = packed;
            }
        }
    }
    return true;
}

} // namespace cpcse
