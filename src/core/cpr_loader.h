// CPCSyntaxError — CPR cartridge loader.
// Parses RIFF AMS! cartridges and raw cartridge images into 16 KiB banks.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int BANK_SIZE = 0x4000;
inline constexpr int BANK_COUNT = 32;

// { format, banks, bankCount } — banks[i] empty == null.
struct Cartridge {
    std::string format;
    std::vector<Bytes> banks;
    int bankCount = 0;
};

// Parses native CPR files as implemented by JavaCPC's CPRLoader. A CRO file gives the
// bankable ROMs of its first group (GNUM 0 first) laid out by croCartridge.
Cartridge parseCartridge(const Bytes& input);

// CRO, "Containers ROms" (github.com/Logon-System/CRO-Format): RIFF, form "CRO ", made of
// GRRO groups; each holds GNUM (number), GLBL (label), GMSK (address mask) and ROM chunks
// of RID (name), RTYP (type), RLOG (logical number), RPHY (physical number) and RDT (data).
// Unknown chunks are skipped, as RIFF allows.
enum CroRomType { CRO_ROM_LOW = 0, CRO_ROM_HIGH = 1, CRO_ROM_BANKABLE = 2, CRO_ROM_MF2 = 3 };
struct CroRom {
    std::string id;
    uint32_t type = CRO_ROM_HIGH, logical = 0, physical = 0;
    Bytes data;
};
struct CroGroup {
    uint32_t number = 0;               // GNUM; without one, the order the group was found in
    std::string label;
    uint32_t mask = 0xffffffff;        // GMSK; without one, every address bit counts
    std::vector<CroRom> roms;
    bool hasBankable() const;
};
bool isCro(const Bytes& input);
std::vector<CroGroup> parseCro(const Bytes& input);   // throws for a file it cannot take
// The group a CRO starts with: GNUM 0, else the first group. -1 when there is none.
int croDefaultGroup(const std::vector<CroGroup>& groups);
// A CRO file of these groups, laid out as Logon-System's own CRO manager writes it
// (rom manager/CROMANAGER.html, exportCRO): each GRRO holds GNUM, GLBL, GMSK, then its ROM
// chunks of RID, RTYP, RLOG, RPHY, RDT; text with no 0 at its end; a pad byte after data of
// odd length, inside the GRRO's and ROM's own lengths.
Bytes serializeCro(const std::vector<CroGroup>& groups);
// What would make these groups a bad CRO or a bad board: a GNUM twice, a bankable page past
// 31 or twice, two high ROMs on one number, RLOG past 255, more than one low or MF2 ROM, ROM
// sizes the hardware does not have. Empty when there is nothing to say.
std::vector<std::string> croProblems(const std::vector<CroGroup>& groups);
// A cartridge's pages as CRO ROMs: a CPR's own cbNN chunks (the pages it holds, no mirror),
// or a raw image's 16K pages; bankable, named cbNN, RPHY the page, RLOG 7 for page 3 and 1
// for the others (as Logon-System's CRO manager imports a CPR). Throws for a file it cannot take.
std::vector<CroRom> croRomsFromCartridge(const Bytes& file);
// The GMSK that makes `pages` 16K pages repeat through the cartridge space, as a ROM chip
// of that size does (the next power of two).
uint32_t croMaskForPages(int pages);
// The cartridge a group's bankable ROMs make, as the Plus's 19-bit address (page << 14 |
// offset) sees it: the address is ANDed with GMSK and the ROM whose RPHY is the page of the
// result answers; a page with no ROM reads 0 (the guide: "An empty or norom ROM always
// returns 0"). All 32 pages are filled, so the mask alone decides the mirroring.
Cartridge croCartridge(const CroGroup& group);

} // namespace cpcse
