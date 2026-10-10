// CPCSyntaxError — CPC memory mapper.
#include "memory.h"
#include <stdexcept>

namespace cpcse {

GXMemory::GXMemory(int ramKiB) {
    validateRamSize(ramKiB);
    ram.assign((size_t)ramKiB * 1024, 0);
    asicRam.assign(ROM_SIZE, 0);
    cartridge.assign(BANK_COUNT, Bytes{});
    lowerRom.clear();
    upperRoms.assign(256, Bytes{});
    snapshotLowerRom.clear();
    snapshotUpperRoms.assign(256, Bytes{});
    reset();
}

void GXMemory::validateRamSize(int ramKiB) {
    switch (ramKiB) {
        case 64: case 128: case 256: case 320: case 512: case 576: return;
        default: break;
    }
    // 64K + 1..8 segments of 512K: 1088, 1600, ... 4160.
    if (ramKiB > 576 && ramKiB <= 4160 && (ramKiB - 64) % 512 == 0) return;
    throw std::range_error("RAM size must be 64, 128, 256, 320, 512 or 576 KiB, or 64 KiB + 1 to 8 x 512 KiB (up to 4160).");
}

void GXMemory::setRamSize(int ramKiB) {
    validateRamSize(ramKiB);
    expansionPages = 0xff;
    if ((int)ram.size() == ramKiB * 1024) return;
    ram.assign((size_t)ramKiB * 1024, 0);
    reset(false);
}

void GXMemory::reset(bool clearRam) {
    if (clearRam) { std::fill(ram.begin(), ram.end(), 0); std::fill(asicRam.begin(), asicRam.end(), 0); }
    lowerEnabled = true; upperEnabled = false; ramConfig = 0; ram4MbSegment = 0;
    upperRom = 0; useCartridgeUpper = true; upperCartridgePage = 1;
    lowerPage = 0; lowerLocation = 0; asicRamEnabled = false; asicRamLocation = 0;
    remap();
}

void GXMemory::loadCartridge(const std::vector<Bytes>& banks) {
    cartridge.assign(BANK_COUNT, Bytes{});
    for (int i = 0; i < BANK_COUNT; i++) if (i < (int)banks.size() && !banks[i].empty()) cartridge[i] = banks[i];
    remap();
}
void GXMemory::setLowerRom(const Bytes& bytes) { lowerRom = normalizeRom(bytes); remap(); }
void GXMemory::setUpperRom(int index, const Bytes& bytes) { upperRoms[index & 0xff] = normalizeRom(bytes); remap(); }
void GXMemory::setSnapshotLowerRom(const Bytes& bytes) { snapshotLowerRom = normalizeRom(bytes); remap(); }
void GXMemory::setSnapshotUpperRom(int index, const Bytes& bytes) { snapshotUpperRoms[index & 0xff] = normalizeRom(bytes); remap(); }
void GXMemory::clearSnapshotRoms() { snapshotLowerRom.clear(); std::fill(snapshotUpperRoms.begin(), snapshotUpperRoms.end(), Bytes{}); remap(); }

Bytes GXMemory::normalizeRom(const Bytes& bytes) {
    if (bytes.empty()) return Bytes{};
    Bytes rom(ROM_SIZE, 0);
    int copy = std::min((int)bytes.size(), ROM_SIZE);
    for (int k = 0; k < copy; k++) rom[k] = bytes[k];
    return rom;
}
void GXMemory::setUpperRomSelect(int value) { upperRom = value & 0xff; useCartridgeUpper = false; remap(); }
void GXMemory::setUpperCartridge(int page) { upperCartridgePage = page & 0x1f; useCartridgeUpper = true; remap(); }
void GXMemory::selectUpperCartridgeFromPort(int value) {
    value &= 0xff;
    setUpperCartridge(value > 0x7f ? value & 0x1f : value == 7 ? 3 : 1);
}
void GXMemory::setLowerCartridge(int page, int location) { lowerPage = page & 0x1f; lowerLocation = location; remap(); }
void GXMemory::setAsicRam(bool enabled) { asicRamEnabled = enabled; asicRamLocation = 2; remap(); }

void GXMemory::accessTiming(int address, int instructionOffset, bool write) {
    if (timingProvider) timingProvider(instructionOffset, write, address & 0xffff);
}

void GXMemory::setGateArrayConfig(int value) {
    lowerEnabled = (value & 0x04) == 0;
    upperEnabled = (value & 0x08) == 0;
    remap();
}

int GXMemory::expansionPageOffset(int page, int segment) const {
    if (segmentedExpansion()) {
        int seg = std::max(0, std::min(7, segment));
        int offset = 0x10000 + (seg * 8 + (page & 7)) * 0x10000;
        return offset + 0x10000 <= (int)ram.size() ? offset : -1;
    }
    if (((expansionPages >> (page & 7)) & 1) == 0) return -1;
    int offset = (page + 1) * 0x10000;
    return offset + 0x10000 <= (int)ram.size() ? offset : -1;
}

void GXMemory::setRamConfig(int value, int port) {
    int config = value & 0x3f;
    if (segmentedExpansion()) {
        int high = (port >> 8) & 0xff;
        ram4MbSegment = (high >= 0x78 && high <= 0x7f) ? 0x7f - high : 0;
        // A segment the board does not have does not answer: as a missing page below.
        if (expansionPageOffset((config & 0x38) >> 3, ram4MbSegment) < 0) config = 0;
    } else {
        ram4MbSegment = 0;
        if (expansionPageOffset((config & 0x38) >> 3, 0) < 0) config = 0;
    }
    ramConfig = config;
    remap();
}

int GXMemory::extendedPageOffset() const {
    int page = (ramConfig & 0x38) >> 3;
    int offset = expansionPageOffset(page);
    return offset >= 0 ? offset : 0;
}

void GXMemory::remapRam() {
    int base = (ramConfig & 7) == 2 ? extendedPageOffset() : 0;
    for (int slot = 0; slot < 8; slot += 1) readMap[slot] = writeMap[slot] = base + slot * SLOT_SIZE;
    int bank = ramConfig & 7, extended = extendedPageOffset();
    if ((bank & 5) == 1) { // C1/C3/C9/CB: extra RAM at C000
        readMap[6] = writeMap[6] = extended + 0xc000;
        readMap[7] = writeMap[7] = extended + 0xe000;
        if (bank & 2) { // C3/CB additionally maps main C000 at 4000
            readMap[2] = writeMap[2] = base + 0xc000;
            readMap[3] = writeMap[3] = base + 0xe000;
        }
    } else if (bank & 4) { // C4..C7: one of the four 16-KiB sections at 4000
        int offset = extended + (bank & 3) * ROM_SIZE;
        readMap[2] = writeMap[2] = offset;
        readMap[3] = writeMap[3] = offset + SLOT_SIZE;
    }
    if (asicRamEnabled) {
        int slot = asicRamLocation;
        readMap[slot] = writeMap[slot] = 0xffffffff;
        readMap[slot + 1] = writeMap[slot + 1] = 0xffffffff;
    }
}

int GXMemory::cartridgeByte(int page, int address) const {
    // populated = this.cartridge.filter(Boolean)
    int populatedCount = 0; for (auto& b : cartridge) if (!b.empty()) populatedCount++;
    const Bytes* bank = nullptr;
    if (page >= 0 && page < (int)cartridge.size() && !cartridge[page].empty()) bank = &cartridge[page];
    else if (populatedCount) {
        // populated[page % populated.length]
        int target = ((page % populatedCount) + populatedCount) % populatedCount;
        int seen = 0;
        for (auto& b : cartridge) if (!b.empty()) { if (seen == target) { bank = &b; break; } seen++; }
    }
    return bank ? (*bank)[address & 0x3fff] : -1;
}

bool GXMemory::lowerSlotFor(int address) const {
    return lowerLocation <= 6 && address >= lowerLocation * SLOT_SIZE && address < (lowerLocation + 2) * SLOT_SIZE;
}

MemoryIdentity GXMemory::debugIdentity(int address) {
    address &= 0xffff;
    if (lowerEnabled && lowerSlotFor(address)) {
        int cart = cartridgeByte(lowerPage, address);
        if (cart != -1) return { "rom", lowerPage, true, -1 };
        if (lowerLocation == 0 && (!snapshotLowerRom.empty() || !lowerRom.empty())) return { "rom", 256, false, -1 };
    }
    if (upperEnabled && address >= 0xc000) {
        if (useCartridgeUpper && cartridgeByte(upperCartridgePage, address) != -1) return { "rom", upperCartridgePage, true, -1 };
        return { "rom", upperRom & 0xff, false, -1 };
    }
    int slot = (unsigned)address >> 13;
    uint32_t offset = readMap[slot];
    if (offset == 0xffffffff) return { "asic", 0, false, -1 };
    int physical = (int)offset + (address & 0x1fff);
    return { "ram", (int)std::floor(physical / (double)ROM_SIZE), false, physical };
}

int GXMemory::read(int address) {
    if (!watchArmed) return readMapped(address);
    int value = readMapped(address);
    watchHook(address & 0xffff, value & 0xff, false);
    return value;
}

int GXMemory::readMapped(int address) {
    address &= 0xffff;
    if (overlayLow && address < 0x4000) return address < 0x2000 ? overlayLow[address] : overlayRam[address - 0x2000];
    if (lowerEnabled && lowerSlotFor(address)) {
        int value = cartridgeByte(lowerPage, address); if (value != -1) return value;
        const Bytes& lower = !snapshotLowerRom.empty() ? snapshotLowerRom : lowerRom;
        if (lowerLocation == 0 && !lower.empty()) return lower[address];
    }
    if (upperEnabled && address >= 0xc000) {
        if (!useCartridgeUpper && upperRomReadHandler) {
            int hooked = upperRomReadHandler(address, upperRom);
            if (hooked != -1) return hooked & 0xff;
        }
        if (useCartridgeUpper) { int value = cartridgeByte(upperCartridgePage, address); if (value != -1) return value; }
        const Bytes* rom = nullptr;
        if (!snapshotUpperRoms[upperRom].empty()) rom = &snapshotUpperRoms[upperRom];
        else if (!upperRoms[upperRom].empty()) rom = &upperRoms[upperRom];
        else if (!snapshotUpperRoms[0].empty()) rom = &snapshotUpperRoms[0];
        else if (!upperRoms[0].empty()) rom = &upperRoms[0];
        if (rom) return (*rom)[address & 0x3fff];
    }
    int slot = (unsigned)address >> 13; uint32_t offset = readMap[slot];
    if (offset == 0xffffffff) {
        int asicAddress = (address - asicRamLocation * SLOT_SIZE) & 0x3fff;
        return asicRamReadHandler ? asicRamReadHandler(asicAddress) : asicRam[asicAddress];
    }
    size_t idx = (size_t)offset + (address & 0x1fff);
    return idx < ram.size() ? ram[idx] : 0xff;
}

void GXMemory::write(int address, int value) {
    address &= 0xffff; value &= 0xff;
    if (watchArmed) watchHook(address, value, true);
    if (overlayRam && address >= 0x2000 && address < 0x4000) { overlayRam[address - 0x2000] = (uint8_t)value; return; }
    int slot = (unsigned)address >> 13; uint32_t offset = writeMap[slot];
    if (offset == 0xffffffff) {
        int asicAddress = (address - asicRamLocation * SLOT_SIZE) & 0x3fff;
        int previous = asicRam[asicAddress];
        asicRam[asicAddress] = (uint8_t)value;
        if (asicRamHandler) asicRamHandler(asicAddress, value, previous);
    }
    else if ((size_t)offset + (address & 0x1fff) < ram.size()) ram[(size_t)offset + (address & 0x1fff)] = (uint8_t)value;
}

int GXMemory::physicalAddress(int address, bool write) const {
    address &= 0xffff;
    if (overlayLow && address < 0x4000 && (!write || address >= 0x2000)) return -1;   // the card's, not RAM
    if (!write) {   // readMapped()'s order: the ROMs and the cartridge before the RAM
        if (lowerEnabled && lowerSlotFor(address)) {
            if (cartridgeByte(lowerPage, address) != -1) return -1;
            const Bytes& lower = !snapshotLowerRom.empty() ? snapshotLowerRom : lowerRom;
            if (lowerLocation == 0 && !lower.empty()) return -1;
        }
        if (upperEnabled && address >= 0xc000) {
            if (useCartridgeUpper && cartridgeByte(upperCartridgePage, address) != -1) return -1;
            if (!snapshotUpperRoms[upperRom].empty() || !upperRoms[upperRom].empty() ||
                !snapshotUpperRoms[0].empty() || !upperRoms[0].empty() || (!useCartridgeUpper && upperRomReadHandler))
                return -1;
        }
    }
    const uint32_t offset = (write ? writeMap : readMap)[(unsigned)address >> 13];
    if (offset == 0xffffffff) return -1;
    const size_t idx = (size_t)offset + (address & 0x1fff);
    return idx < ram.size() ? (int)idx : -1;
}

void GXMemory::noteAccess(int address, int kind) {
    const int at = physicalAddress(address, kind == Z80Memory::ACCESS_WRITE);
    if (at < 0) return;
    if (accessMap.size() != ram.size()) accessMap.resize(ram.size(), 0);
    accessMap[(size_t)at] |= (uint8_t)kind;
}

int GXMemory::readVideo(int address) const {
    size_t idx = address & 0xffff;
    return idx < ram.size() ? ram[idx] : 0;
}
int GXMemory::readBase(int address) const {
    size_t idx = address & 0xffff;
    return idx < ram.size() ? ram[idx] : 0;
}
} // namespace cpcse
