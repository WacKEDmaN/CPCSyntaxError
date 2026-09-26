// CPCSyntaxError — Amstrad Plus ASIC.
#include "asic.h"
#include "memory.h"
#include "z80.h"
#include "ay.h"

namespace cpcse {

const int PLUS_PALETTE[32] = {
    0x666, 0x666, 0xf06, 0xff6, 0x006, 0x0f6, 0x606, 0x6f6,
    0x0f6, 0xff6, 0xff0, 0xfff, 0x0f0, 0x0ff, 0x6f0, 0x6ff,
    0x006, 0xf06, 0xf00, 0xf0f, 0x000, 0x00f, 0x600, 0x60f,
    0x066, 0xf66, 0xf60, 0xf6f, 0x060, 0x06f, 0x660, 0x66f
};
static const int LOCK_SEQUENCE[16] = { 0x00, 0x00, 0xff, 0x77, 0xb3, 0x51, 0xa8, 0xd4, 0x62, 0x39, 0x9c, 0x46, 0x2b, 0x15, 0x8a, 0xcd };
enum { DMA_IDLE = 0, DMA_DEAD = 1, DMA_FETCH_0 = 2, DMA_FETCH_1 = 3, DMA_FETCH_2 = 4,
    DMA_EXECUTE_0 = 5, DMA_LOAD_0 = 6, DMA_EXECUTE_1 = 7, DMA_LOAD_1 = 8,
    DMA_EXECUTE_2 = 9, DMA_LOAD_2 = 10, DMA_INTERRUPT = 12 };

PlusAsic::PlusAsic(GXMemory* memory) : memory(memory) {
    memory->setAsicRamHandler([this](int address, int value, int previous) { writeAsicRam(address, value, previous); });
    memory->setAsicRamReadHandler([this](int address) { return readAsicRam(address); });
    reset();
}

void PlusAsic::reset() {
    palette.fill(0);
    for (auto& sprite : sprites) sprite.fill(0);
    spriteMagnification.fill(0);

    videoRevision = videoRevision + 1;
    spriteDataRevision = spriteDataRevision + 1;
    spriteRevision = spriteRevision + 1;
    locked = true; unlockPosition = 0;
    rasterInterruptLine = 0; rasterSplitLine = 0; splitAddress = 0;
    softScrollControl = 0; horizontalScroll = 0; verticalScroll = 0;
    interruptVector = 0x01;
    rasterInterruptDelay = 0; pendingRasterLine = -1; suppressPriDelayLine = -1;
    rasterInterruptPending = false; shadowInterrupt = false; rasterAcknowledgeLatched = false;
    legacyDmaPauseTiming = false;
    irqStatus = 0; plus8kBug = 6;
    lastReadValue = 0;
    for (auto& ch : dma) ch = DmaChannel{};
    dmaCycle = DMA_IDLE; dmaCycleDelay = 0; dmaAy = nullptr;
    dmaStatus = 0;
    memory->asicRam[0x2805] = 0x01;
    for (int a = 0x2808; a < 0x2810; a++) memory->asicRam[a] = 0xff;
    updateDmaStatusRam();
    memory->setAsicRam(false); memory->setLowerCartridge(0, 0); memory->setUpperCartridge(1); memory->setGateArrayConfig(0x80);
}

int PlusAsic::color(int index) { return colorValue(palette[index & 0x1f]); }
int PlusAsic::colorValue(int value) {
    auto expand = [](int nibble) { int value8 = (nibble & 0x0f) << 4; return value8 + ((unsigned)value8 >> 5); };
    return (expand(value >> 4) << 16) | (expand(value >> 8) << 8) | expand(value);
}
int PlusAsic::spriteColor(int index) { return spriteColorValue(palette[index & 0x1f]); }
int PlusAsic::spriteColorValue(int value) {
    auto expand = [](int nibble) { return (nibble & 0x0f) * 17; };
    return (expand(value >> 4) << 16) | (expand(value >> 8) << 8) | expand(value);
}

// ACCC §9: the &7Fxx decode is the GATE ARRAY's and now lives there. These two are
// what it calls back for -- the parts that exist only on a CPC+.
bool PlusAsic::plusCartridgePageWrite(int value) {
    if (locked || (value & 0xe0) != 0xa0) return false;
    int page = value & 7, region = value >> 3 & 3;
    if (region == 3) { memory->setLowerCartridge(page, 0); memory->setAsicRam(true); }
    else { memory->setAsicRam(false); memory->setLowerCartridge(page, region * 2); }
    return true;
}
// The CPC+ keeps a 12-bit shadow of every ink the GATE ARRAY is given, so that an
// unlocked ASIC can show the same colour at its own finer resolution.
void PlusAsic::setGateArrayInk(int pen, int hardwareColour) {
    setPlusPalette(pen, PLUS_PALETTE[hardwareColour & 0x1f]);
}
void PlusAsic::advanceUnlock(int value) {
    value &= 0xff;
    if (unlockPosition == 0) {
        if (value != 0) unlockPosition = 1;
        return;
    }
    if (unlockPosition < (int)(sizeof(LOCK_SEQUENCE) / sizeof(LOCK_SEQUENCE[0]))) {
        if (value == LOCK_SEQUENCE[unlockPosition]) unlockPosition += 1;
        else {
            unlockPosition += 1;
            if (unlockPosition == (int)(sizeof(LOCK_SEQUENCE) / sizeof(LOCK_SEQUENCE[0])) && !locked) { locked = true; videoRevision += 1; }
            unlockPosition = value == 0 ? 2 : 1;
        }
        return;
    }
    if (locked) { locked = false; videoRevision += 1; }
    unlockPosition = value == 0 ? 0 : 1;
}
void PlusAsic::setPlusPalette(int index, int value) {
    palette[index] = (uint16_t)(value & 0xfff);
    int address = 0x2400 + (index << 1);
    memory->asicRam[address] = (uint8_t)(value & 0xff);
    memory->asicRam[address + 1] = (uint8_t)(value >> 8 & 0x0f);
    videoRevision += 1;
}
void PlusAsic::writeAsicRam(int address, int value) { writeAsicRam(address, value, memory->asicRam[address]); }
void PlusAsic::writeAsicRam(int address, int value, int previous) {
    value &= 0xff;
    if (address < 0x1000) {
        value &= 0x0f;
        if ((previous & 0x0f) != value) { spriteDataRevision += 1; spriteRevision += 1; }
        memory->asicRam[address] = (uint8_t)value;
        sprites[(unsigned)address >> 8][address & 0xff] = (uint8_t)value; return;
    }
    if (address >= 0x2000 && address < 0x2080) {
        int sprite = (address - 0x2000) >> 3, reg = address & 7, canonical = address & 0x3ffb;
        uint8_t before[8];
        for (int i = 0; i < 8; i++) before[i] = memory->asicRam[(address & ~7) + i];
        int beforeMagnification = spriteMagnification[sprite];
        if (reg >= 4) {
            spriteMagnification[sprite] = (uint8_t)(value & 0x0f);
            memory->asicRam[address] = memory->asicRam[canonical];
        } else if (reg == 1) {
            int masked = (value & 3) == 3 ? 0xff : value & 3;
            memory->asicRam[address] = memory->asicRam[address + 4] = (uint8_t)masked;
        } else if (reg == 3) {
            int masked = value & 1 ? 0xff : 0;
            memory->asicRam[address] = memory->asicRam[address + 4] = (uint8_t)masked;
        } else memory->asicRam[address] = memory->asicRam[address + 4] = (uint8_t)value;
        bool changed = beforeMagnification != spriteMagnification[sprite];
        for (int i = 0, base = address & ~7; !changed && i < 8; i += 1) changed = before[i] != memory->asicRam[base + i];
        if (changed) spriteRevision += 1;
        return;
    }
    if (address >= 0x2400 && address < 0x2440) {
        if (address & 1) memory->asicRam[address] &= 0x0f;
        int index = (address - 0x2400) >> 1, low = memory->asicRam[0x2400 + (index << 1)], high = memory->asicRam[0x2401 + (index << 1)];
        palette[index] = (uint16_t)(low | (high & 0x0f) << 8); videoRevision += 1; return;
    }
    if (address == 0x2800) {
        if (rasterInterruptLine != value) {
            rasterInterruptLine = value;
            if (value && hasPendingInterrupt && hasPendingInterrupt()) {
                if (clearPendingInterrupt) clearPendingInterrupt();
                shadowInterrupt = true;
            }
        }
    }
    else if (address == 0x2801) rasterSplitLine = value;
    else if (address == 0x2802) splitAddress = (splitAddress & 0x00ff) | (value & 0x3f) << 8;
    else if (address == 0x2803) splitAddress = (splitAddress & 0xff00) | value;
    else if (address == 0x2804) {
        softScrollControl = (unsigned)value >> 7; horizontalScroll = value & 0x0f; verticalScroll = value >> 4 & 7;
        if (onSoftScrollChange) onSoftScrollChange();
        videoRevision += 1;
    } else if (address == 0x2805) {
        interruptVector = value & 0xf9;
    }
    else if (address >= 0x2c00 && address < 0x2c0c) {
        int channel = (address - 0x2c00) >> 2, reg = address & 3; DmaChannel& d = dma[channel];
        if (reg == 0) d.pointer = (d.pointer & 0xff00) | (value & 0xfe);
        else if (reg == 1) d.pointer = (d.pointer & 0x00ff) | value << 8;
        else if (reg == 2) d.prescale = value;
        updateDmaStatusRam();
    } else if (address == 0x2c0f) setDmaStatus(value);
    else if (address >= 0x2808 && address < 0x2810) memory->asicRam[address] = (uint8_t)previous;
    else memory->asicRam[address] = (uint8_t)previous;
}
int PlusAsic::readAsicRam(int address) {
    address &= 0x3fff;
    int value;
    if (address < 0x1000 || (address >= 0x2400 && address < 0x2440) || address == 0x2c0f) value = memory->asicRam[address];
    else if (address >= 0x2000 && address < 0x2080) {
        int reg = address & 3, canonical = address & 0x3ffb;
        if (reg == 0 || reg == 2) value = memory->asicRam[canonical];
        else if (reg == 1) {
            int high = memory->asicRam[canonical] & 3; value = high == 3 ? 0xff : high;
        } else value = memory->asicRam[canonical] & 1 ? 0xff : 0;
    } else if (address >= 0x2808 && address < 0x2810) value = analogueInput[address - 0x2808] & 0x3f;
    else if (address >= 0x2c00 && address < 0x2c0f) value = dmaStatus;
    else value = lastReadValue;
    lastReadValue = value & 0xff;
    return lastReadValue;
}
void PlusAsic::updateDmaStatusRam() { for (int a = 0x2c00; a < 0x2c10; a++) memory->asicRam[a] = (uint8_t)(dmaStatus & 0xff); }
void PlusAsic::setDmaStatus(int value) {
    value &= 0xff;
    int ack = value & 0x70;
    if (interruptVector & 1) {
        dmaStatus &= ~ack;
        irqStatus &= ~ack;
        for (int index = 0; index < 3; index++) { if (ack & (0x40 >> index)) { dma[index].irq = false; dma[index].irqRequest = false; } }
    }
    dmaStatus = (dmaStatus & ~0x07) | (value & 0x07);
    for (int index = 0; index < 3; index++) {
        DmaChannel& channel = dma[index];
        channel.active = !!(dmaStatus & (1 << index));
        if (!channel.active) { channel.prescaleCurrent = 0; channel.pause = 0; channel.loopCount = 0; channel.instruction = 0; channel.instructionFetched = false; }
    }
    updateDmaStatusRam();
}
void PlusAsic::onHsync(AY38912* ay, int rasterLine, int hsyncWidth) {
    onHsyncStart(ay, rasterLine, hsyncWidth);
    for (int character = 0; character < 64; character += 1) onCharacter();
}
void PlusAsic::onHsyncStart(AY38912* ay, int rasterLine, int hsyncWidth) {
    dmaAy = ay; dmaCycle = DMA_DEAD; dmaCycleDelay = 0;
    legacyDmaPauseTiming = rasterLine == -1 && hsyncWidth == -1;
    pendingRasterLine = rasterLine;
    bool legacyPriTiming = hsyncWidth == -1;
    int width = legacyPriTiming ? 10 : std::max(1, std::min(16, hsyncWidth ? hsyncWidth : 16));
    if (rasterLine != -1 && suppressPriDelayLine == rasterLine) {
        suppressPriDelayLine = -1;
        rasterInterruptDelay = 0;
    } else if (priAtHsyncStart && rasterLine != -1 && rasterInterruptEnabled() && rasterLine == rasterInterruptLine) {
        rasterInterruptDelay = 0; raiseRasterInterrupt();
    } else {
        rasterInterruptDelay = rasterLine == -1 ? 0 : (legacyPriTiming ? width : std::min(width, 7));
    }
    triggerAsicInterrupt();
}
void PlusAsic::onCharacter() {
    tickDmaCycle(dmaAy);
    if (rasterInterruptDelay) {
        rasterInterruptDelay -= 1;
        if (!rasterInterruptDelay) checkRasterInterrupt(pendingRasterLine);
    }
}
void PlusAsic::checkRasterInterrupt(int rasterLine) {
    if (rasterLine != -1 && rasterInterruptEnabled() && rasterLine == rasterInterruptLine) raiseRasterInterrupt();
}
bool PlusAsic::fetchDma(int channelIndex) {
    DmaChannel& channel = dma[channelIndex];
    channel.instructionFetched = false;
    if (channel.pause > 0) {
        if (channel.prescaleCurrent > 0) channel.prescaleCurrent -= 1;
        else {
            channel.prescaleCurrent = channel.prescale;
            channel.pause -= 1;
        }
        if (channel.pause > 0 || legacyDmaPauseTiming) return false;
    }
    channel.currentAddress = channel.pointer;
    channel.instruction = memory->readBase(channel.pointer) | memory->readBase((channel.pointer + 1) & 0xffff) << 8;
    channel.pointer = (channel.pointer + 2) & 0xffff;
    channel.instructionFetched = true;
    return true;
}
bool PlusAsic::executeDma(int channelIndex) {
    DmaChannel& channel = dma[channelIndex];
    int instruction = channel.instruction, opcode = (unsigned)instruction >> 12 & 0x0f;
    if (opcode == 0) { dmaCycleDelay = 2; return true; }
    if (opcode == 1) { channel.pause = instruction & 0x0fff; channel.prescaleCurrent = 0; }
    else if (opcode == 2) { channel.loopCount = instruction & 0x0fff; channel.loopStart = channel.pointer; }
    else if (opcode == 4) {
        if (instruction & 1 && channel.loopCount > 0) { channel.pointer = channel.loopStart; channel.loopCount -= 1; }
        if (instruction & 0x10) channel.irqRequest = true;
        if (instruction & 0x20) { channel.active = false; dmaStatus &= ~(1 << channelIndex); }
    }
    channel.instruction = 0; channel.instructionFetched = false; updateDmaStatusRam(); return false;
}
void PlusAsic::tickDmaCycle(AY38912* ay) {
    if (dmaCycleDelay) { dmaCycleDelay -= 1; return; }
    for (int guard = 0; guard < 12; guard += 1) {
        if (dmaCycle == DMA_IDLE) return;
        if (dmaCycle == DMA_DEAD) {
            int bits = 0; for (int i = 0; i < 3; i++) bits |= dma[i].active ? (1 << i) : 0;
            dmaStatus = (dmaStatus & ~7) | bits;
            updateDmaStatusRam(); dmaCycle = dmaStatus & 7 ? DMA_FETCH_0 : DMA_INTERRUPT; return;
        }
        if (dmaCycle >= DMA_FETCH_0 && dmaCycle <= DMA_FETCH_2) {
            int channelIndex = dmaCycle - DMA_FETCH_0; dmaCycle += 1;
            if (!dma[channelIndex].active) continue;
            fetchDma(channelIndex); return;
        }
        if (dmaCycle == DMA_EXECUTE_0 || dmaCycle == DMA_EXECUTE_1 || dmaCycle == DMA_EXECUTE_2) {
            int channelIndex = dmaCycle == DMA_EXECUTE_0 ? 0 : dmaCycle == DMA_EXECUTE_1 ? 1 : 2;
            int next = channelIndex == 0 ? DMA_EXECUTE_1 : channelIndex == 1 ? DMA_EXECUTE_2 : DMA_INTERRUPT;
            DmaChannel& channel = dma[channelIndex];
            if (!channel.active || !channel.instructionFetched) { dmaCycle = next; continue; }
            dmaCycle = executeDma(channelIndex) ? DMA_LOAD_0 + channelIndex * 2 : next; return;
        }
        if (dmaCycle == DMA_LOAD_0 || dmaCycle == DMA_LOAD_1 || dmaCycle == DMA_LOAD_2) {
            int channelIndex = dmaCycle == DMA_LOAD_0 ? 0 : dmaCycle == DMA_LOAD_1 ? 1 : 2;
            DmaChannel& channel = dma[channelIndex]; if (ay) ay->writeRegister((unsigned)channel.instruction >> 8 & 0x0f, channel.instruction);
            channel.instruction = 0; channel.instructionFetched = false;
            dmaCycle = channelIndex == 0 ? DMA_EXECUTE_1 : channelIndex == 1 ? DMA_EXECUTE_2 : DMA_INTERRUPT; return;
        }
        if (dmaCycle == DMA_INTERRUPT) {
            for (int index = 0; index < 3; index += 1) {
                DmaChannel& channel = dma[index]; if (!channel.irqRequest) continue;
                int bit = 0x40 >> index;
                channel.irqRequest = false; channel.irq = true;
                dmaStatus |= bit; irqStatus |= bit;
                if ((interruptVector & 1) == 0) channel.irq = false;
            }
            updateDmaStatusRam(); triggerAsicInterrupt(); dmaCycle = DMA_IDLE; return;
        }
        dmaCycle = DMA_IDLE; return;
    }
}
bool PlusAsic::rasterInterruptEnabled() { return !locked && rasterInterruptLine != 0; }
void PlusAsic::clearShadowInterrupt() { shadowInterrupt = false; }
void PlusAsic::raiseRasterInterrupt(Z80* cpu) {
    rasterInterruptPending = true;
    irqStatus |= 0x80;
    if (cpu) cpu->requestInterrupt(interruptVectorForSources(cpu)); else triggerAsicInterrupt();
}
void PlusAsic::clearGateArrayInterrupt() {
    rasterInterruptPending = false;
    irqStatus &= 0x70;
    shadowInterrupt = false;
}
void PlusAsic::raiseGateArrayInterrupt(Z80* cpu, bool classicBus) {
    rasterInterruptPending = true;
    irqStatus |= 0x80;
    int vector = classicBus ? 0xff : interruptVectorForSources(cpu);
    if (cpu) cpu->requestInterrupt(vector); else if (onAsicInterrupt) onAsicInterrupt(vector);
}
std::optional<DmaSource> PlusAsic::highestDmaInterruptSource(int status) {
    int source = status;
    if (source & 0x10) return DmaSource{ 0x10, 2, 0 };
    if (source & 0x20) return DmaSource{ 0x20, 1, 2 };
    if (source & 0x40) return DmaSource{ 0x40, 0, 4 };
    return std::nullopt;
}
std::optional<DmaSource> PlusAsic::highestDmaInterruptSource() {
    int source = irqStatus ? irqStatus : (dmaStatus & 0x70);
    return highestDmaInterruptSource(source);
}
int PlusAsic::interruptVectorForSources(Z80* cpu) {
    int source = irqStatus; if (!source) source = rasterInterruptPending ? 0x80 : 0; if (!source) source = dmaStatus & 0x70;
    if (source & 0x80) {
        int pcHigh = cpu ? ((unsigned)cpu->pc >> 8 & 0xff) : 0;
        return (interruptVector & 0xf8) | ((pcHigh & 0x20) ? 6 : plus8kBug);
    }
    std::optional<DmaSource> d = highestDmaInterruptSource(source);
    return (interruptVector & 0xf8) | (d ? d->offset : 0);
}
void PlusAsic::triggerAsicInterrupt() {
    if (rasterInterruptPending || (irqStatus & 0x70)) { if (onAsicInterrupt) onAsicInterrupt(interruptVectorForSources()); }
}
void PlusAsic::acknowledgeInterrupt() {
    int source = irqStatus; if (!source) source = rasterInterruptPending ? 0x80 : 0; if (!source) source = dmaStatus & 0x70;
    if (source & 0x80) {
        dmaStatus |= 0x80;
        irqStatus &= 0x70;
        rasterInterruptPending = false;
        rasterAcknowledgeLatched = true;
        plus8kBug = 6;
    } else {
        bool hadRasterLatch = !!(dmaStatus & 0x80);
        dmaStatus &= ~0x80;
        if (hadRasterLatch && !rasterAcknowledgeLatched && (interruptVector & 1) == 0) {
            dmaStatus &= ~0x70;
            irqStatus &= ~0x70;
            for (auto& channel : dma) channel.irq = false;
        } else {
            std::optional<DmaSource> d = highestDmaInterruptSource(source);
            if (d) {
                dmaStatus |= d->bit;
                if ((interruptVector & 1) == 0) {
                    dmaStatus &= ~d->bit;
                    irqStatus &= ~d->bit;
                    dma[d->channel].irq = false;
                } else {
                    irqStatus &= ~d->bit;
                }
            }
        }
        rasterAcknowledgeLatched = false;
    }
    updateDmaStatusRam();
    triggerAsicInterrupt();
}
void PlusAsic::onScanline(int /*line*/) {}

} // namespace cpcse
