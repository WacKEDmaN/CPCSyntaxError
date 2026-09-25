// CPCSyntaxError — ZX Spectrum machine core.
// 48K, 128K and +3 memory, I/O, display, sound and floppy profiles.
#pragma once
#include "common.h"
#include "z80.h"
#include <any>

namespace cpcse {

class AY38912; class UPD765A; class SpectrumTapeDrive;

class ZXKeyboard {
public:
    std::array<uint8_t, 8> rows{};
    std::unordered_set<std::string> pressedKeys;
    std::unordered_map<std::string, std::array<int, 4>> pressedSymbols;
    std::unordered_map<std::string, std::string> codeToEffective;
    ZXKeyboard();
    void reset();
    void rebuildKeyboardState();
    bool setKey(const std::string& code, bool pressed, const std::string& character = "");
    void setJoystickEnabled(bool) {}
    void setLightgunTrigger(bool) {}
    int readPort(int portAddress);
};

class ZXMemory : public Z80Memory {
public:
    std::vector<Bytes> romBanks;   // 4 x 16384
    std::vector<Bytes> ramBanks;   // 8 x 16384
    int romSelect = 0, ramSelect = 0, screenSelect = 5;
    bool pagingLocked = false;
    int port7ffd = 0, port1ffd = 0;
    bool plus3SpecialMode = false;
    int plus3SpecialConfig = 0;
    std::string model = "zx48";
    std::function<int(int, int, bool)> contentionProvider;

    ZXMemory();
    void reset();
    void setModel(const std::string& model);
    void setRom(int bank, const Bytes& data);
    void updateRomSelect();
    bool isContended(int addr);
    int contend(int addr, int instructionOffset, bool write) override;
    int read(int addr) override;
    void write(int addr, int value) override;
    Bytes& getScreenRam();
    void write7ffd(int value);
    void write1ffd(int value);
};

class ZXVideo {
public:
    ZXMemory* memory;
    int border = 0;
    bool flashState = false;
    int flashCounter = 0;
    std::string monitorMode;
    std::vector<uint8_t> imageData;   // RGBA framebuffer
    int imageWidth = 0, imageHeight = 0;

    explicit ZXVideo(ZXMemory* memory);
    void setMonitorMode(const std::string& mode);
    std::array<int, 3> zxColor(int index);
    void setBorder(int color);
    void tickFrame();
    void render(int width = 768, int height = 544);
};

// Minimal CRTC placeholder exposed by the ZX system (lightgun/registers stubs).
struct ZxCrtcStub {
    std::array<uint8_t, 18> registers{};
    void setTrojanLightgunEnabled(bool) {}
    void releaseTrojanLightgun() {}
    void setTrojanLightgun(double, double, bool) {}
};

class ZXSpectrum {
public:
    ZXMemory* memory = nullptr;
    ZXVideo* video = nullptr;
    ZXKeyboard* keyboard = nullptr;
    AY38912* ay = nullptr;
    SpectrumTapeDrive* tape = nullptr;
    UPD765A* fdc = nullptr;
    Z80* cpu = nullptr;
    ZxCrtcStub crtc;

    std::string model = "zx48";
    int frameTStates = 0;
    bool fastTapeLoading = true;
    bool debuggerPaused = false;
    bool cpuIoAccessed = false;
    int beeperBit = 0, earOutputBit = 0;
    bool timingInstructionActive = false;
    int tapeCyclesAdvanced = 0;
    int interruptPulseRemaining = 0;
    int breakpointSkipOnce = -1;
    std::any watchpointPending;
    int stepTarget = -1;
    std::unordered_set<int>* breakpoints = nullptr;
    std::function<bool(int)> breakpointPredicate;
    std::function<void(const std::any&)> onWatchpoint;
    std::function<void()> onBreakpoint;
    bool lastTapeLoadSucceeded = false;

    ZXSpectrum();
    ~ZXSpectrum();
    int getFrameLength();
    int ulaContentionDelay(int frameTState);
    int memoryContentionDelay(int address, int instructionOffset = 0);
    int portContentionDelay(int port, int instructionOffset = 0);
    void clockTapeToInstructionOffset(int offset = 0);
    int stepInstruction();
    void setModel(const std::string& model);
    void setDacType(const std::string& = "none") {}
    void captureRasterState() {}
    void captureRasterCharacter() {}
    void acknowledgeInterrupt() {}
    void reset();
    void writePort(int port, int value);
    int readPort(int port);
    bool tryFastTapeLoad();
    bool debuggerBreakpointHit();
    int runFrame();
};

} // namespace cpcse
