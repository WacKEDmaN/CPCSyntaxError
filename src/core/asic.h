// CPCSyntaxError — Amstrad Plus ASIC.
// Palette, sprites, DMA, interrupts, ROM mapping and unlock state.
#pragma once
#include "common.h"

namespace cpcse {

class GXMemory;
class Z80;
class AY38912;

struct DmaChannel {
    int pointer = 0, currentAddress = 0, instruction = 0;
    bool instructionFetched = false;
    int loopStart = 0, loopCount = 0, prescale = 0, prescaleCurrent = 0, pause = 0;
    bool active = false, irq = false, irqRequest = false;
};

struct DmaSource { int bit; int channel; int offset; };

class PlusAsic {
public:
    GXMemory* memory;
    std::array<uint16_t, 32> palette{};
    std::array<std::array<uint8_t, 256>, 16> sprites{};
    std::array<uint8_t, 16> spriteMagnification{};

    // The pen select, the 17 inks, the graphic mode, its mid-byte latch and the RMR
    // byte are NOT here: ACCC §9.1 puts them in the GATE ARRAY ("a 17th colour, stored
    // in the GATE ARRAY"), and a 464 has them without having any of this chip. See
    // GateArray. What remains below is the CPC+ extension proper.
    int videoRevision = 0, spriteDataRevision = 0, spriteRevision = 0;
    bool locked = true; int unlockPosition = 0;
    int rasterInterruptLine = 0, rasterSplitLine = 0, splitAddress = 0;
    int softScrollControl = 0, horizontalScroll = 0, verticalScroll = 0;
    int interruptVector = 0x01;
    int rasterInterruptDelay = 0;
    int pendingRasterLine = -1;       // null -> -1
    int suppressPriDelayLine = -1;    // null -> -1
    bool rasterInterruptPending = false, shadowInterrupt = false, rasterAcknowledgeLatched = false;
    bool legacyDmaPauseTiming = false;
    int irqStatus = 0, plus8kBug = 6;
    int lastReadValue = 0;
    std::array<DmaChannel, 3> dma{};
    int dmaCycle = 0, dmaCycleDelay = 0;
    AY38912* dmaAy = nullptr;
    int dmaStatus = 0;
    bool priAtHsyncStart = false;

    // Callbacks wired by the emulator (optional; empty == absent).
    std::function<void()> onSoftScrollChange;
    std::function<void()> onInterruptReset;
    std::function<void(int vector)> onAsicInterrupt;
    std::function<bool()> hasPendingInterrupt;
    std::function<void()> clearPendingInterrupt;

    explicit PlusAsic(GXMemory* memory);
    void reset();
    // Called by the GATE ARRAY for the two things inside its port decode that are
    // genuinely this chip's: the unlocked cartridge-page write, and the 12-bit shadow
    // of an ink write. Returns true if it consumed the write.
    bool plusCartridgePageWrite(int value);
    void setGateArrayInk(int pen, int hardwareColour);
    void bumpVideoRevision() { videoRevision += 1; }
    int color(int index);
    int colorValue(int value);
    int spriteColor(int index);
    int spriteColorValue(int value);
    void latchMode(int switchPixel = -1);
    void advanceUnlock(int value);
    void setPlusPalette(int index, int value);
    void writeAsicRam(int address, int value, int previous);
    void writeAsicRam(int address, int value);
    int readAsicRam(int address);
    void updateDmaStatusRam();
    void setDmaStatus(int value);
    void onHsync(AY38912* ay, int rasterLine = -1, int hsyncWidth = -1);
    void onHsyncStart(AY38912* ay, int rasterLine = -1, int hsyncWidth = -1);
    void onCharacter();
    void checkRasterInterrupt(int rasterLine);
    bool fetchDma(int channelIndex);
    bool executeDma(int channelIndex);
    void tickDmaCycle(AY38912* ay);
    bool rasterInterruptEnabled();
    void clearShadowInterrupt();
    void raiseRasterInterrupt(Z80* cpu = nullptr);
    void clearGateArrayInterrupt();
    void raiseGateArrayInterrupt(Z80* cpu = nullptr, bool classicBus = false);
    std::optional<DmaSource> highestDmaInterruptSource();
    std::optional<DmaSource> highestDmaInterruptSource(int status);
    int interruptVectorForSources(Z80* cpu = nullptr);
    void triggerAsicInterrupt();
    void acknowledgeInterrupt();
    void onScanline(int line);
};

extern const int PLUS_PALETTE[32];

} // namespace cpcse
