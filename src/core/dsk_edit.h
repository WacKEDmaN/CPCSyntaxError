// CPCSyntaxError — the DSK editor's model: new discs, track formatting, and the CP/M 2.2
// filesystem AMSDOS uses (and ParaDOS, ROMDOS, Vortex...), all on the track/sector Disk, so
// any image -- standard, extended, odd sectors, weak sectors -- can be worked on.
//
// The filesystem (CP/M 2.2's directory and allocation): the directory is the first DRM+1
// 32-byte entries of the data area, blocks 0.. of it; an entry is user (&E5 = free), name
// (8), type (3; bit 7 of the type bytes: read-only, system, archived), EX, S1, S2, RC and
// 16 bytes of block numbers (8-bit while the disc has 256 blocks or fewer, else 16-bit).
// An entry holds up to 16K a logical extent; EX/S2 number the last logical extent it
// covers and RC its 128-byte records.
#pragma once
#include "dsk.h"
#include "dskfs.h"

namespace cpcse {

struct DskGeometry {
    std::string id, name;
    int tracks = 40, sides = 1, sectors = 9, sizeCode = 2, firstSector = 0xc1;
    int gap3 = 0x52, filler = 0xe5;
    // the order of the sector IDs round a track, as offsets from firstSector (empty: in order)
    std::vector<int> interleave;
    // the filesystem: logical tracks alternate sides (ROMDOS, Vortex...) or run side 0 first
    bool sidesAlternate = true;
    int reservedTracks = 0, blockSize = 1024, dirEntries = 64;
    // blocks the filesystem has (DSM + 1); 0 = all the data area holds
    int blocks = 0;
    int sectorSize() const { return 128 << (sizeCode & 7); }
};

// The CPC formats (AMSDOS DATA, SYSTEM and IBM; 42- and 80-track; ParaDOS, ROMDOS,
// Vortex, Dobbertin), from the format table the emulator already has.
std::vector<DskGeometry> dskPresets();
std::optional<DskGeometry> dskPreset(const std::string& id);
// A blank disc of that geometry, every track formatted, every sector the filler byte.
std::shared_ptr<Disk> dskCreate(const DskGeometry& g, const std::string& creator = "CPCSE");
// (Re)format one track: g's sectors, IDs and N, in its interleave, filled with g.filler.
void dskFormatTrack(Disk& disk, int cylinder, int side, const DskGeometry& g);
// Give the disc this many tracks and sides (new tracks unformatted; fewer drops the rest).
void dskResize(Disk& disk, int tracks, int sides);
// The first sector on (cylinder, side) with ID R, or null.
Sector* dskFindSector(Disk& disk, int cylinder, int side, int r);
// Which preset the disc's own sectors say it is (track 0's lowest sector ID, the sides
// and tracks); empty when nothing fits.
std::optional<DskGeometry> dskDetect(const Disk& disk);

struct DskFsFile {
    int user = 0;
    std::string name, ext;                 // as stored, 7-bit, trailing spaces dropped
    bool readOnly = false, system = false, archived = false;
    int records = 0;                       // 128-byte records the directory gives it
    int size() const { return records * 128; }
    std::vector<int> entries;              // its directory entries, in extent order
    std::vector<int> blocks;               // its blocks, in order
    std::optional<AmsdosHeader> header;    // its first record, when that is an AMSDOS header
    std::string displayName() const { return ext.empty() ? name : name + "." + ext; }
};

struct DskBlockUse { enum Kind { Free, Directory, File, Shared } kind = Free; int file = -1; };

class DskFs {
public:
    DskFs(std::shared_ptr<Disk> disk, const DskGeometry& g);
    const DskGeometry& geometry() const { return g; }
    int totalBlocks() const;               // DSM + 1
    int directoryBlocks() const;
    bool wideBlockNumbers() const { return totalBlocks() > 256; }
    int freeBlocks() const;
    int freeEntries() const;

    std::vector<DskFsFile> files() const;
    // The file's bytes: every record (with its AMSDOS header, if any) -- or, stripHeader,
    // the data its header says it holds.
    Bytes read(const DskFsFile& f, bool stripHeader = false) const;
    // A new file; one of the same user, name and type is replaced. "" or why not.
    std::string write(int user, const std::string& name, const std::string& ext, const Bytes& data);
    void remove(const DskFsFile& f);
    std::string rename(const DskFsFile& f, int user, const std::string& name, const std::string& ext);
    void setAttributes(const DskFsFile& f, bool readOnly, bool system, bool archived);
    // Who has each block (files are indexes into files()).
    std::vector<DskBlockUse> blockMap(const std::vector<DskFsFile>& list) const;
    // A block's sectors: (cylinder, side, R) of each; false when one is missing on the disc.
    bool blockSectors(int block, std::vector<std::array<int, 3>>& out) const;
    Bytes readBlock(int block) const;
    bool writeBlock(int block, const Bytes& data);
    // The 32-byte entries, raw (for the directory view).
    Bytes directory() const;

private:
    std::shared_ptr<Disk> disk;
    DskGeometry g;
    bool logicalSector(int index, int& cylinder, int& side, int& r) const;
    void writeDirectory(const Bytes& dir);
    std::vector<int> entryBlocks(const Bytes& dir, int entry) const;
};

// 8.3 as CP/M stores it: upper case, no spaces or CP/M's separators, cut to 8 and 3.
void dskSplitName(const std::string& in, std::string& name, std::string& ext);
// The 128-byte header AMSDOS writes on a disc file, laid out as its own SAVE lays it:
// user, name, type at 18, load address 21, length 24 (16-bit) and 64 (24-bit), entry 26,
// the checksum of bytes 0-66 at 67; the tape-only fields (block length at 19, first-block
// flag at 23) left 0, as AMSDOS leaves them.
Bytes dskAmsdosHeader(int user, const std::string& name, const std::string& ext, int type, int load, int exec, int length);

} // namespace cpcse
