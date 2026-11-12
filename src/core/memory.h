// CPCSyntaxError — CPC memory mapper.
// Maps base RAM, expansion banks, ROM, cartridge and ASIC windows.
#pragma once
#include "common.h"
#include "cpr_loader.h"
#include "z80.h"

namespace cpcse {

class PicoGX;

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
    int populatedBanks = 0;            // how many of them are not empty: set where they are (the constructor, loadCartridge)
    Bytes lowerRom;                    // empty == null
    std::vector<Bytes> upperRoms;      // 256 slots
    Bytes snapshotLowerRom;
    std::vector<Bytes> snapshotUpperRoms;
    std::array<uint32_t, 8> readMap{};
    std::array<uint32_t, 8> writeMap{};

    bool lowerEnabled = true, upperEnabled = false;
    int ramConfig = 0, ram4MbSegment = 0;
    int upperRom = 0; bool useCartridgeUpper = true; int upperCartridgePage = 1;
    // Plus: the number the last &DFxx write selected. Below &80 the ASIC maps a cartridge page,
    // but a board on the expansion port that answers that number (the M4) overrides it.
    int plusSelectedRom = 0;
    // A classic CPC running a cartridge from the M4 (|CTR): bank 0 is the lower ROM (as on a
    // Plus) and the ROM select takes the Plus's mapping, &80+n for bank n.
    bool cartridgeOnClassic = false;
    int lowerPage = 0, lowerLocation = 0; bool asicRamEnabled = false; int asicRamLocation = 0;
    // Which 64K expansion pages (&7Fxx bits 5..3) answer, one bit each. All of them in a
    // contiguous expansion; a silicon disc answers only pages 4..7. Set to all by
    // setRamSize, so only a caller that fits such a board changes it.
    int expansionPages = 0xff;
    // A PLUS WITH A CLASSIC RAM EXPANSION (a board on the expansion port). The board decodes
    // the Gate Array's RAM configuration itself and does not know the ASIC's page: when it
    // maps its RAM at &4000-&7FFF while the ASIC page is in, a write goes to the ASIC AND to
    // the board's RAM (AndyCadley, CPCWiki forum: "Old RAM expansions don't understand and so
    // mirror the write to RAM"). The machine's own RAM does not: internalPages is how many
    // 64K expansion pages it has itself (1 on a 6128 Plus, 0 on a 464 Plus or GX4000; an
    // internal upgrade counts as its own). Reads stay the ASIC's.
    bool classicExpansion = false;
    int internalPages = 0;
    std::array<uint32_t, 8> asicMirror{};     // per slot: the RAM a write under the ASIC also reaches (or none)

    // Handlers (optional; empty == absent). Read handlers return -1 for null.
    std::function<void(int address, int value, int previous)> asicRamHandler;
    std::function<int(int address)> asicRamReadHandler;
    std::function<void(int offset, bool write, int address)> timingProvider;
    std::function<int(int address, int rom)> upperRomReadHandler;
    // A card over &0000-&3FFF (the Multiface II while paged in): reads of &0000-&1FFF
    // from overlayLow, reads and writes of &2000-&3FFF to overlayRam. null = none.
    const uint8_t* overlayLow = nullptr;
    uint8_t* overlayRam = nullptr;
    // What a board's RAMDIS does to the internal RAM (service manuals, CPU circuit diagrams):
    // on the 6128 it goes to the banking PAL (IC118 pin 4, which makes CAS0/CAS1), so the
    // internal RAM is off for reads AND writes. On the 464 and the 664 there is no PAL: RAMDIS
    // only reaches the OR gate (IC112) that enables the read latch IC114, while /CAS and /WE
    // go from the Gate Array straight to the 4164s. There a write the board takes ALSO lands
    // in the internal RAM, at the Z80's own address. false: a 464 or 664 (set by the host).
    bool ramdisBlocksInternalWrites = true;

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
    // A board past 512K is a SEGMENTED expansion (the 4 MB design): eight segments of
    // 512K, the segment picked by the &7Fxx port's high byte (&7F = segment 0 .. &78 = 7).
    // A smaller board fits fewer segments, one per 512K: 1088K = 64K + two segments.
    bool segmentedExpansion() const { return ram.size() > (size_t)576 * 1024; }
    int expansionSegments() const { return (int)((ram.size() / 1024 - 64) / 512); }
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
    // A PicoGX in the cartridge slot (picogx.h): every CPU read the cartridge answers goes
    // past it, with the page it was on (A14-A18). The host owns it; readMapped (the
    // debugger's views) does not reach it. lastReadCartPage is the page readMapped took a
    // byte from, -1 when the byte came from elsewhere (set on the cartridge paths only).
    PicoGX* picoGx = nullptr;
    int lastReadCartPage = -1;
    // Debugger watchpoints. While armed, every read()/write() is reported to the hook;
    // the front end arms it only while the machine runs, so its own views stay silent.
    std::function<void(int address, int value, bool write)> watchHook;
    bool watchArmed = false;
    // The memory map's recording: one byte per byte of `ram`, the Z80Memory::AccessKind
    // bits of every access the CPU made there since it was cleared. Kept only while
    // trackAccess is set (the debugger's Memory map window); ROM and ASIC RAM are not kept.
    std::vector<uint8_t> accessMap;
    void noteAccess(int address, int kind) override;
    void clearAccessMap() { accessMap.assign(ram.size(), 0); }
    // Where a CPU address lands in `ram` for a read or a write, or -1 when a ROM, the
    // cartridge or the ASIC page answers there instead.
    int physicalAddress(int address, bool write) const;
    int readVideo(int address) const;
    int readBase(int address) const;
};

} // namespace cpcse
