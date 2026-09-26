// CPCSyntaxError — CPC memory mapper.
// Maps base RAM, expansion banks, ROM, cartridge and ASIC windows.
#pragma once
#include "common.h"
#include "cpr_loader.h"
#include "z80.h"

namespace cpcse {

inline constexpr int SLOT_SIZE = 0x2000;
inline constexpr int ROM_SIZE = 0x4000;

// debugIdentity() result: { kind, bank, cartridge?, physical? }.
struct MemoryIdentity {
    std::string kind;      // 'rom' | 'asic' | 'ram'
    int bank = 0;
    bool cartridge = false;
    int physical = -1;
};

// CPC/GX4000 64-KiB address space and Gate-Array banking. Stores eight 8-KiB
// offsets, matching JavaCPC's CPCMemory.readMap/writeMap arrangement.
class GXMemory : public Z80Memory {
public:
    Bytes ram;
    Bytes asicRam;
    std::vector<Bytes> cartridge;      // BANK_COUNT slots, empty == null
    Bytes lowerRom;                    // empty == null
    std::vector<Bytes> upperRoms;      // 256 slots
    Bytes snapshotLowerRom;
    std::vector<Bytes> snapshotUpperRoms;
    std::array<uint32_t, 8> readMap{};
    std::array<uint32_t, 8> writeMap{};

    bool lowerEnabled = true, upperEnabled = false;
    int ramConfig = 0, ram4MbSegment = 0;
    int upperRom = 0; bool useCartridgeUpper = true; int upperCartridgePage = 1;
    int lowerPage = 0, lowerLocation = 0; bool asicRamEnabled = false; int asicRamLocation = 0;
    // Which 64K expansion pages (&7Fxx bits 5..3) answer, one bit each. All of them in a
    // contiguous expansion; a silicon disc answers only pages 4..7. Set to all by
    // setRamSize, so only a caller that fits such a board changes it.
    int expansionPages = 0xff;

    // Handlers (optional; empty == absent). Read handlers return -1 for null.
    std::function<void(int address, int value, int previous)> asicRamHandler;
    std::function<int(int address)> asicRamReadHandler;
    std::function<void(int offset, bool write, int address)> timingProvider;
    std::function<int(int address, int rom)> upperRomReadHandler;

    explicit GXMemory(int ramKiB = 128);

    void validateRamSize(int ramKiB);
    void setRamSize(int ramKiB);
    void reset(bool clearRam);
    void reset() { reset(true); }

    void loadCartridge(const std::vector<Bytes>& banks);
    void setLowerRom(const Bytes& bytes);
    void setUpperRom(int index, const Bytes& bytes);
    void setSnapshotLowerRom(const Bytes& bytes);
    void setSnapshotUpperRom(int index, const Bytes& bytes);
    void clearSnapshotRoms();
    Bytes normalizeRom(const Bytes& bytes);
    void setUpperRomSelect(int value);
    void setUpperCartridge(int page);
    void selectUpperCartridgeFromPort(int value);
    void setLowerCartridge(int page, int location = 0);
    void setAsicRam(bool enabled);
    void setAsicRamHandler(std::function<void(int, int, int)> handler) { asicRamHandler = std::move(handler); }
    void setAsicRamReadHandler(std::function<int(int)> handler) { asicRamReadHandler = std::move(handler); }
    void setTimingProvider(std::function<void(int, bool, int)> provider) { timingProvider = std::move(provider); }
    void setUpperRomReadHandler(std::function<int(int, int)> handler) { upperRomReadHandler = std::move(handler); }

    // Z80Memory contract.
    int contend(int address, int instructionOffset, bool write) override { (void)address; (void)instructionOffset; (void)write; return 0; }
    void accessTiming(int address, int instructionOffset, bool write) override;

    void setGateArrayConfig(int value);
    bool has4MB() const { return (int)ram.size() == 4160 * 1024; }
    int expansionPageOffset(int page, int segment) const;
    int expansionPageOffset(int page) const { return expansionPageOffset(page, ram4MbSegment ? ram4MbSegment : 0); }
    void setRamConfig(int value, int port = 0x7f00);
    int extendedPageOffset() const;
    void remapRam();
    void remap() { remapRam(); }

    int cartridgeByte(int page, int address) const;  // returns byte or -1 (null)
    bool lowerSlotFor(int address) const;
    MemoryIdentity debugIdentity(int address);
    int read(int address) override;
    void write(int address, int value) override;
    int readMapped(int address);           // read() without the debugger's watch hook
    // Debugger watchpoints. While armed, every read()/write() is reported to the hook;
    // the front end arms it only while the machine runs, so its own views stay silent.
    std::function<void(int address, int value, bool write)> watchHook;
    bool watchArmed = false;
    int readVideo(int address) const;
    int readBase(int address) const;
};

} // namespace cpcse
