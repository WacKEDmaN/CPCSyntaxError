// CPCSyntaxError — ZX Spectrum machine core.
#include "zx.h"
#include "ay.h"
#include "fdc.h"
#include "tape.h"
#include "monitor_palette.h"

namespace cpcse {

static const int ZX_PALETTE[16][3] = {
    {0x00,0x00,0x00},{0x00,0x00,0xc0},{0xc0,0x00,0x00},{0xc0,0x00,0xc0},
    {0x00,0xc0,0x00},{0x00,0xc0,0xc0},{0xc0,0xc0,0x00},{0xc0,0xc0,0xc0},
    {0x00,0x00,0x00},{0x00,0x00,0xff},{0xff,0x00,0x00},{0xff,0x00,0xff},
    {0x00,0xff,0x00},{0x00,0xff,0xff},{0xff,0xff,0x00},{0xff,0xff,0xff}
};

// [row, bit] (+optional needsShift as 3rd)
static const std::unordered_map<std::string, std::array<int, 3>>& zxKeyMap() {
    static const std::unordered_map<std::string, std::array<int, 3>> m = {
        {"ShiftLeft",{0,0,0}},{"ShiftRight",{0,0,0}},{"KeyZ",{0,1,0}},{"KeyX",{0,2,0}},{"KeyC",{0,3,0}},{"KeyV",{0,4,0}},
        {"KeyA",{1,0,0}},{"KeyS",{1,1,0}},{"KeyD",{1,2,0}},{"KeyF",{1,3,0}},{"KeyG",{1,4,0}},
        {"KeyQ",{2,0,0}},{"KeyW",{2,1,0}},{"KeyE",{2,2,0}},{"KeyR",{2,3,0}},{"KeyT",{2,4,0}},
        {"Digit1",{3,0,0}},{"Digit2",{3,1,0}},{"Digit3",{3,2,0}},{"Digit4",{3,3,0}},{"Digit5",{3,4,0}},
        {"Digit0",{4,0,0}},{"Digit9",{4,1,0}},{"Digit8",{4,2,0}},{"Digit7",{4,3,0}},{"Digit6",{4,4,0}},
        {"KeyP",{5,0,0}},{"KeyO",{5,1,0}},{"KeyI",{5,2,0}},{"KeyU",{5,3,0}},{"KeyY",{5,4,0}},
        {"Enter",{6,0,0}},{"KeyL",{6,1,0}},{"KeyK",{6,2,0}},{"KeyJ",{6,3,0}},{"KeyH",{6,4,0}},
        {"Space",{7,0,0}},{"ControlLeft",{7,1,0}},{"ControlRight",{7,1,0}},{"AltLeft",{7,1,0}},{"AltRight",{7,1,0}},{"KeyM",{7,2,0}},{"KeyN",{7,3,0}},{"KeyB",{7,4,0}},
        {"NumpadEnter",{6,0,0}},
        {"ArrowLeft",{3,4,1}},{"ArrowDown",{4,4,1}},{"ArrowUp",{4,3,1}},{"ArrowRight",{4,2,1}},
        {"Backspace",{4,0,1}},{"Escape",{7,0,1}}
    };
    return m;
}
// [row, bit, isSymShift, isCapsShift]
static const std::unordered_map<std::string, std::array<int, 4>>& zxSymbolMap() {
    static const std::unordered_map<std::string, std::array<int, 4>> m = {
        {".",{7,2,1,0}},{",",{7,3,1,0}},{";",{5,1,1,0}},{"\"",{5,0,1,0}},{"'",{4,3,1,0}},{":",{0,1,1,0}},
        {"=",{6,1,1,0}},{"+",{6,2,1,0}},{"-",{6,3,1,0}},{"*",{7,4,1,0}},{"/",{0,4,1,0}},{"?",{0,3,1,0}},
        {"(",{4,2,1,0}},{")",{4,1,1,0}},{"_",{4,0,1,0}},{"@",{3,1,1,0}},{"#",{3,2,1,0}},{"$",{3,3,1,0}},
        {"%",{3,4,1,0}},{"&",{4,4,1,0}},{"<",{2,3,1,0}},{">",{2,4,1,0}},{"^",{6,4,1,0}},{"!",{3,0,1,0}}
    };
    return m;
}

ZXKeyboard::ZXKeyboard() { reset(); }
void ZXKeyboard::reset() { rows.fill(0xff); pressedKeys.clear(); pressedSymbols.clear(); codeToEffective.clear(); }
void ZXKeyboard::rebuildKeyboardState() {
    rows.fill(0xff);
    bool symbolActive = pressedSymbols.size() != 0;
    for (const std::string& code : pressedKeys) {
        if (symbolActive && (code == "ShiftLeft" || code == "ShiftRight")) continue;
        auto it = zxKeyMap().find(code);
        if (it != zxKeyMap().end()) {
            int row = it->second[0], bit = it->second[1], needsShift = it->second[2];
            rows[row] &= ~(1 << bit);
            if (needsShift) rows[0] &= ~1;
        }
    }
    for (auto& kv : pressedSymbols) {
        int row = kv.second[0], bit = kv.second[1], symShift = kv.second[2], capsShift = kv.second[3];
        rows[row] &= ~(1 << bit);
        if (symShift) rows[7] &= ~(1 << 1);
        if (capsShift) rows[0] &= ~1;
    }
}
bool ZXKeyboard::setKey(const std::string& code, bool pressed, const std::string& character) {
    bool hasSymbol = character.size() == 1 && zxSymbolMap().count(character);
    std::array<int, 4> symbol{}; if (hasSymbol) symbol = zxSymbolMap().at(character);
    std::string effectiveCode = code;
    if (!hasSymbol && character.size() == 1) {
        std::string upper = character; for (auto& c : upper) c = (char)std::toupper((unsigned char)c);
        char u = upper[0];
        if (u >= 'A' && u <= 'Z') effectiveCode = std::string("Key") + u;
        else if (u >= '0' && u <= '9') effectiveCode = std::string("Digit") + u;
    }
    if (pressed) {
        codeToEffective[code] = effectiveCode;
        if (hasSymbol) pressedSymbols[code] = symbol;
    } else {
        auto it = codeToEffective.find(code); if (it != codeToEffective.end()) effectiveCode = it->second;
        codeToEffective.erase(code);
        pressedSymbols.erase(code);
    }
    bool hasMap = zxKeyMap().count(effectiveCode) != 0;
    if (!hasSymbol) {
        if (pressed) pressedKeys.insert(effectiveCode);
        else pressedKeys.erase(effectiveCode);
    }
    rebuildKeyboardState();
    return hasSymbol || hasMap;
}
int ZXKeyboard::readPort(int portAddress) {
    int result = 0xff;
    int high = (unsigned)portAddress >> 8;
    for (int i = 0; i < 8; i += 1) if (!(((unsigned)high >> i) & 1)) result &= rows[i];
    return result;
}

static const int SPECIAL_PAGING[4][4] = { {0,1,2,3},{4,5,6,7},{4,5,6,3},{4,7,6,3} };

ZXMemory::ZXMemory() {
    romBanks.assign(4, Bytes(16384, 0));
    ramBanks.assign(8, Bytes(16384, 0));
    reset();
}
void ZXMemory::reset() {
    romSelect = 0; ramSelect = 0; screenSelect = 5; pagingLocked = false;
    port7ffd = 0; port1ffd = 0; plus3SpecialMode = false; plus3SpecialConfig = 0;
    if (model.empty()) model = "zx48";
}
void ZXMemory::setModel(const std::string& model_) { model = model_; reset(); }
void ZXMemory::setRom(int bank, const Bytes& data) {
    for (int i = 0; i < 16384 && i < (int)data.size(); i++) romBanks[bank][i] = data[i];
}
void ZXMemory::updateRomSelect() {
    if (model == "zxplus3") {
        int romLow = (port7ffd & 0x10) ? 1 : 0;
        int romHigh = (port1ffd & 0x04) ? 2 : 0;
        romSelect = romHigh | romLow;
    } else {
        romSelect = (port7ffd & 0x10) ? 1 : 0;
    }
}
bool ZXMemory::isContended(int addr) {
    addr &= 0xffff;
    if (plus3SpecialMode && model == "zxplus3") {
        int bank = SPECIAL_PAGING[plus3SpecialConfig][(unsigned)addr >> 14];
        return (bank & 1) != 0;
    }
    if (addr >= 0x4000 && addr < 0x8000) return true;
    if (addr >= 0xc000 && model != "zx48") return (ramSelect & 1) != 0;
    return false;
}
int ZXMemory::contend(int addr, int instructionOffset, bool write) {
    return contentionProvider ? contentionProvider(addr & 0xffff, instructionOffset, write) : 0;
}
int ZXMemory::read(int addr) {
    addr &= 0xffff;
    int bankIndex = (unsigned)addr >> 14;
    int offset = addr & 0x3fff;
    if (plus3SpecialMode && model == "zxplus3") {
        int bank = SPECIAL_PAGING[plus3SpecialConfig][bankIndex];
        return ramBanks[bank][offset];
    }
    if (bankIndex == 0) return romBanks[romSelect][offset];
    if (bankIndex == 1) return ramBanks[5][offset];
    if (bankIndex == 2) return ramBanks[2][offset];
    int targetRam = model == "zx48" ? 0 : ramSelect;
    return ramBanks[targetRam][offset];
}
void ZXMemory::write(int addr, int value) {
    addr &= 0xffff; value &= 0xff;
    int bankIndex = (unsigned)addr >> 14;
    int offset = addr & 0x3fff;
    if (plus3SpecialMode && model == "zxplus3") {
        int bank = SPECIAL_PAGING[plus3SpecialConfig][bankIndex];
        ramBanks[bank][offset] = (uint8_t)value; return;
    }
    if (bankIndex == 0) return;
    if (bankIndex == 1) { ramBanks[5][offset] = (uint8_t)value; return; }
    if (bankIndex == 2) { ramBanks[2][offset] = (uint8_t)value; return; }
    int targetRam = model == "zx48" ? 0 : ramSelect;
    ramBanks[targetRam][offset] = (uint8_t)value;
}
Bytes& ZXMemory::getScreenRam() { return ramBanks[screenSelect]; }
void ZXMemory::write7ffd(int value) {
    if (pagingLocked) return;
    port7ffd = value;
    ramSelect = value & 7;
    screenSelect = (value & 8) ? 7 : 5;
    if (value & 32) pagingLocked = true;
    updateRomSelect();
}
void ZXMemory::write1ffd(int value) {
    if (model != "zxplus3") return;
    port1ffd = value;
    plus3SpecialMode = (value & 1) != 0;
    plus3SpecialConfig = (unsigned)value >> 1 & 3;
    updateRomSelect();
}

ZXVideo::ZXVideo(ZXMemory* memory) : memory(memory) { border = 0; flashState = false; flashCounter = 0; monitorMode = MONITOR_MODE_COLOUR; }
void ZXVideo::setMonitorMode(const std::string& mode) { monitorMode = normaliseMonitorMode(mode); }
std::array<int, 3> ZXVideo::zxColor(int index) {
    const int* c = ZX_PALETTE[index & 15];
    return monitorTransformTuple({ c[0], c[1], c[2] }, monitorMode);
}
void ZXVideo::setBorder(int color) { border = color & 7; }
void ZXVideo::tickFrame() {
    flashCounter = (flashCounter + 1) % 32;
    if (flashCounter == 0) flashState = !flashState;
}
void ZXVideo::render(int width, int height) {
    if (imageWidth != width || imageHeight != height || (int)imageData.size() != width * height * 4) {
        imageWidth = width; imageHeight = height; imageData.assign((size_t)width * height * 4, 0);
    }
    std::vector<uint8_t>& data = imageData;
    Bytes& screen = memory->getScreenRam();
    std::array<int, 3> borderRgb = zxColor(border);
    int dispW = 512, dispH = 384;
    int borderX = (width - dispW) / 2;
    int borderY = (height - dispH) / 2;
    for (int y = 0; y < height; y += 1) {
        for (int x = 0; x < width; x += 1) {
            int idx = (y * width + x) * 4;
            std::array<int, 3> rgb = borderRgb;
            if (x >= borderX && x < borderX + dispW && y >= borderY && y < borderY + dispH) {
                int zxY = (y - borderY) >> 1;
                int zxX = (x - borderX) >> 1;
                int zxRow = zxY & 0xff;
                int charCol = zxX >> 3;
                int pixelBit = 7 - (zxX & 7);
                int pAddr = ((zxRow & 0xc0) << 5) | ((zxRow & 0x38) << 2) | ((zxRow & 7) << 8) | charCol;
                int aAddr = 0x1800 | ((zxRow >> 3) << 5) | charCol;
                int pixByte = pAddr < (int)screen.size() ? screen[pAddr] : 0;
                int attrByte = aAddr < (int)screen.size() ? screen[aAddr] : 0x38;
                int ink = (attrByte & 7) | ((attrByte & 0x40) ? 8 : 0);
                int paper = (((unsigned)attrByte >> 3) & 7) | ((attrByte & 0x40) ? 8 : 0);
                if ((attrByte & 0x80) && flashState) { int tmp = ink; ink = paper; paper = tmp; }
                int isSet = ((unsigned)pixByte >> pixelBit) & 1;
                rgb = zxColor(isSet ? ink : paper);
            }
            data[idx] = (uint8_t)rgb[0];
            data[idx + 1] = (uint8_t)rgb[1];
            data[idx + 2] = (uint8_t)rgb[2];
            data[idx + 3] = 255;
        }
    }
}

ZXSpectrum::ZXSpectrum() {
    memory = new ZXMemory();
    frameTStates = 0;
    memory->contentionProvider = [this](int address, int offset, bool) { return memoryContentionDelay(address, offset); };
    video = new ZXVideo(memory);
    keyboard = new ZXKeyboard();
    // The ZX keyboard is read directly (readPort), never through AY R14, so the
    // AY gets no KeyboardMatrix — matching CPCSyntaxError, where AY.read finds no read().
    ay = new AY38912(nullptr, 3546900, 16);
    tape = new SpectrumTapeDrive(ay);
    fastTapeLoading = true;
    fdc = new UPD765A();
    debuggerPaused = false;
    cpuIoAccessed = false;
    beeperBit = 0;
    earOutputBit = 0;
    Z80Ports ports;
    ports.write = [this](int port, int value, int timingOffset, const std::string&) {
        clockTapeToInstructionOffset(timingOffset);
        writePort(port, value);
    };
    ports.read = [this](int port, int timingOffset, const std::string&) {
        clockTapeToInstructionOffset(timingOffset);
        return readPort(port);
    };
    ports.contend = [this](int port, int timingOffset) { return portContentionDelay(port, timingOffset); };
    cpu = new Z80(memory, ports);
    reset();
}
ZXSpectrum::~ZXSpectrum() { delete cpu; delete fdc; delete tape; delete ay; delete keyboard; delete video; delete memory; }

int ZXSpectrum::getFrameLength() { return model == "zx48" ? 69888 : 70908; }
int ZXSpectrum::ulaContentionDelay(int frameTState) {
    int frameLength = getFrameLength();
    int t = frameTState % frameLength;
    if (t < 0) t += frameLength;
    int first = model == "zx48" ? 14335 : 14361;
    int lineLength = model == "zx48" ? 224 : 228;
    if (t < first) return 0;
    int offset = t - first;
    int line = offset / lineLength;
    int position = offset % lineLength;
    if (line >= 192 || position >= 128) return 0;
    static const int pattern[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
    return pattern[position & 7];
}
int ZXSpectrum::memoryContentionDelay(int address, int instructionOffset) {
    if (!memory->isContended(address)) return 0;
    return ulaContentionDelay(frameTStates + instructionOffset);
}
int ZXSpectrum::portContentionDelay(int port, int instructionOffset) {
    port &= 0xffff;
    bool highAddressContended = memory->isContended(port);
    int t = frameTStates + instructionOffset;
    int extra = 0;
    if ((port & 1) == 0) {
        if (highAddressContended) {
            int delay = ulaContentionDelay(t);
            extra += delay; t += delay + 1;
            delay = ulaContentionDelay(t);
            extra += delay;
        } else {
            t += 1;
            extra += ulaContentionDelay(t);
        }
    } else if (highAddressContended) {
        for (int cycle = 0; cycle < 4; cycle += 1) {
            int delay = ulaContentionDelay(t);
            extra += delay;
            t += delay + 1;
        }
    }
    return extra;
}
void ZXSpectrum::clockTapeToInstructionOffset(int offset) {
    if (!timingInstructionActive) return;
    int target = std::max(tapeCyclesAdvanced, offset);
    int delta = target - tapeCyclesAdvanced;
    if (delta > 0) tape->advanceCycles(delta);
    tapeCyclesAdvanced = target;
}
int ZXSpectrum::stepInstruction() {
    timingInstructionActive = true;
    tapeCyclesAdvanced = 0;
    int cycles = cpu->step();
    timingInstructionActive = false;
    if (cycles > tapeCyclesAdvanced) tape->advanceCycles(cycles - tapeCyclesAdvanced);
    if (watchpointPending.has_value()) {
        std::any hit = watchpointPending;
        watchpointPending.reset();
        debuggerPaused = true;
        stepTarget = -1;
        if (onWatchpoint) onWatchpoint(hit);
    }
    return cycles;
}
void ZXSpectrum::setModel(const std::string& model_) {
    model = model_;
    memory->setModel(model_);
    tape->setModel(model_);
    reset();
}
void ZXSpectrum::reset() {
    breakpointSkipOnce = -1; watchpointPending.reset();
    frameTStates = 0;
    interruptPulseRemaining = 0;
    timingInstructionActive = false;
    tapeCyclesAdvanced = 0;
    memory->reset();
    keyboard->reset();
    ay->reset();
    tape->reset();
    fdc->reset();
    cpu->reset();
    beeperBit = 0;
    earOutputBit = 0;
}
void ZXSpectrum::writePort(int port, int value) {
    value &= 0xff;
    if ((port & 1) == 0) {
        video->setBorder(value & 7);
        earOutputBit = (value & 0x08) ? 1 : 0;
        beeperBit = (value & 0x10) ? 1 : 0;
        ay->beeperNoise = (beeperBit || earOutputBit) ? 0.65 : 0;
        return;
    }
    if (model != "zx48") {
        if ((port & 0xc002) == 0x4000) memory->write7ffd(value);
        if ((port & 0xc002) == 0xc000) ay->select(value);
        if ((port & 0xc002) == 0x8000) ay->write(value);
        if (model == "zxplus3") {
            if ((port & 0xf002) == 0x1000) { memory->write1ffd(value); fdc->setMotor(!!(value & 0x08)); }
            if ((port & 0xe002) == 0x2000 && (port & 0x1000) != 0) fdc->write(port, value);
        }
    }
}
int ZXSpectrum::readPort(int port) {
    if ((port & 1) == 0) {
        int kb = keyboard->readPort(port);
        int ear = tape->getEarLevel() ? 0x40 : 0x00;
        return (kb & 0xbf) | ear;
    }
    if (model != "zx48") {
        if ((port & 0xc002) == 0xc000) return ay->read();
        if (model == "zxplus3") {
            if ((port & 0xe002) == 0x2000) {
                if ((port & 0x1000) == 0) return fdc->status();
                return fdc->read(port);
            }
        }
    }
    return 0xff;
}
bool ZXSpectrum::tryFastTapeLoad() {
    if (!fastTapeLoading || !tape->loaded) return false;
    if (cpu->pendingNmi || (cpu->pendingInterrupt != -1 && cpu->iff1 && cpu->eiDelay == 0)) return false;
    int pc = cpu->pc & 0xffff;
    if ((pc != 0x056b && pc != 0x0111) || memory->read(pc) != 0xc0) return false;
    std::optional<Bytes> blockOpt = tape->getNextLoadableBlock();
    if (!blockOpt.has_value()) return false;
    Bytes& block = *blockOpt;
    int expectedFlag = cpu->ap & 0xff;
    int startAddress = cpu->ix & 0xffff;
    int requestedLength = cpu->de() & 0xffff;
    int actualFlag = block.size() ? block[0] : -1;
    bool doLoad = (cpu->fp & 0x01) != 0;
    bool success = actualFlag == expectedFlag;
    int checksum = actualFlag & 0xff;
    if (success) {
        for (int offset = 0; offset < requestedLength; offset += 1) {
            int sourceIndex = offset + 1;
            if (sourceIndex >= (int)block.size()) { success = false; break; }
            int value = block[sourceIndex] & 0xff;
            int address = (startAddress + offset) & 0xffff;
            if (doLoad) memory->write(address, value);
            else if (memory->read(address) != value) success = false;
            checksum ^= value;
        }
        int checksumIndex = requestedLength + 1;
        if (checksumIndex >= (int)block.size() || checksum != block[checksumIndex]) success = false;
    }
    if (success) {
        int checksumByte = block[requestedLength + 1] & 0xff;
        cpu->ix = (startAddress + requestedLength) & 0xffff;
        cpu->setDe(0);
        cpu->h = 0;
        cpu->l = checksumByte;
        cpu->a = 0;
        cpu->f = 0x93;
    } else {
        cpu->f &= 0xfe;
    }
    cpu->pc = 0x05e2;
    cpu->halted = false;
    lastTapeLoadSucceeded = success;
    return true;
}
bool ZXSpectrum::debuggerBreakpointHit() {
    int pc = cpu->pc & 0xffff;
    bool stepTargetHit = stepTarget == pc;
    bool hasBreakpoint = breakpoints && breakpoints->count(pc);
    if (breakpointSkipOnce == pc) { breakpointSkipOnce = -1; return stepTargetHit; }
    if (!hasBreakpoint) return stepTargetHit;
    bool breakpoint = breakpointPredicate ? breakpointPredicate(pc) : true;
    return breakpoint || stepTargetHit;
}
int ZXSpectrum::runFrame() {
    if (debuggerPaused) return 0;
    int targetCycles = getFrameLength();
    int elapsed = 0;
    interruptPulseRemaining = std::max(0, 32 - frameTStates);
    if (interruptPulseRemaining > 0) cpu->requestInterrupt(0xff);
    else if (cpu->pendingInterrupt == 0xff) cpu->pendingInterrupt = -1;
    while (frameTStates < targetCycles) {
        if (debuggerBreakpointHit()) {
            debuggerPaused = true;
            stepTarget = -1;
            if (onBreakpoint) onBreakpoint();
            break;
        }
        int step;
        if (tryFastTapeLoad()) {
            step = 4;
            cpu->tStates += step;
            tape->advanceCycles(step);
        } else {
            step = stepInstruction();
        }
        elapsed += step;
        frameTStates += step;
        if (debuggerPaused) break;
        if (interruptPulseRemaining > 0) {
            interruptPulseRemaining -= step;
            if (interruptPulseRemaining <= 0 && cpu->pendingInterrupt == 0xff) cpu->pendingInterrupt = -1;
        }
        ay->advanceTStates(step);
        fdc->advanceCycles(std::max(1, step / 4));
    }
    frameTStates -= targetCycles;
    video->tickFrame();
    return elapsed;
}

} // namespace cpcse
