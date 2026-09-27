// CPCSyntaxError — the M4 Board's microSD card: a host folder.
//
// The M4's ESP8266 runs FatFs over its card and answers the CPC's file commands with it
// (m4info.txt; the error numbers are FatFs's FR_*, ff.i in the M4 ROM source). Programs
// that bring their own file system -- SymbOS -- read and write the card's raw sectors
// instead (C_SDREAD / C_SDWRITE). Both see one card here:
//
//   M4Storage  the folder, as FatFs would: case-insensitive, case-preserving names,
//              '/' paths from the card's root.
//   M4SdCard   the same folder as a FAT16 volume, sector by sector, built when a program
//              first asks for a raw sector. What the program writes goes back to the
//              folder -- new, changed, renamed and deleted files and directories -- once
//              the card has been idle a moment, and whenever it is flushed.
#pragma once
#include "common.h"
#include <filesystem>
#include <map>

namespace cpcse {

// FatFs result codes (ff.i).
enum M4Fr {
    M4_FR_OK = 0, M4_FR_DISK_ERR = 1, M4_FR_INT_ERR = 2, M4_FR_NOT_READY = 3, M4_FR_NO_FILE = 4,
    M4_FR_NO_PATH = 5, M4_FR_INVALID_NAME = 6, M4_FR_DENIED = 7, M4_FR_EXIST = 8, M4_FR_INVALID_OBJECT = 9,
    M4_FR_WRITE_PROTECTED = 10, M4_FR_TOO_MANY_OPEN_FILES = 18, M4_FR_INVALID_PARAMETER = 19,
};
std::string m4ErrorText(int fr);

struct M4Entry {
    std::string name;          // as on the card, real case
    bool dir = false;
    uint64_t size = 0;
    int fatDate = 0, fatTime = 0;
    bool readOnly = false;
};

class M4Storage {
public:
    explicit M4Storage(std::string hostFolder);
    std::string root;           // the host folder
    std::string cwd = "/";      // the ESP's current directory (real case)
    bool readOnly = false;
    uint64_t revision = 0;      // bumps on every change to the card's content

    // A name as the CPC gives it, made absolute: '\' and '/' both separate, a drive
    // letter is dropped, "." and ".." resolved. The case is kept as given.
    std::string absolute(const std::string& name) const;
    // The entry a path names, matched without regard to case; its real path, or "".
    std::string find(const std::string& path) const;
    bool isDir(const std::string& realPath) const;
    bool isFile(const std::string& realPath) const;
    bool stat(const std::string& realPath, M4Entry& out) const;
    std::vector<M4Entry> list(const std::string& realDir) const;   // sorted, dirs and files
    std::filesystem::path host(const std::string& realPath) const;

    // Where a file that may not exist yet would go: the parent must exist (FR_NO_PATH);
    // an existing entry is used with its own case.
    int resolveForCreate(const std::string& path, std::string& realPath) const;

    int readFile(const std::string& realPath, Bytes& out) const;
    int writeFile(const std::string& realPath, const Bytes& data);
    int remove(const std::string& realPath);                         // a file, or an empty directory
    int rename(const std::string& fromReal, const std::string& toPath);
    int makeDir(const std::string& path);
    uint64_t usedBytes() const;
    void touch() { revision += 1; }   // the folder changed behind the card's back

    static std::string parentOf(const std::string& path);
    static std::string leafOf(const std::string& path);
    static std::string lower(std::string s);
    static bool wildcardMatch(const std::string& pattern, const std::string& name);
    // The FAT short names (11 characters, "NAME    EXT") FatFs gives these entries of one
    // directory, in this order -- the same the raw card carries.
    static std::vector<std::string> shortNames(const std::vector<M4Entry>& rows);
};

// One sector-addressed FAT16 volume over the folder.
class M4SdCard {
public:
    explicit M4SdCard(M4Storage& storage) : storage(storage) {}
    // SD error codes (m4info: 0 ok, 1 R/W error, 2 write protected, 3 not ready, 4 invalid parameter)
    int read(uint32_t lba, int count, uint8_t* out);
    int write(uint32_t lba, int count, const uint8_t* in);
    bool dirty() const { return dirtySectors; }
    // Writes the program's changes back to the folder. Returns false (and changes
    // nothing) when the volume cannot be read as FAT -- a program mid-way through.
    bool syncToHost(std::string* report = nullptr);
    uint32_t totalSectors() const { return total; }
    uint64_t capacityBytes() const { return (uint64_t)clusterCount * clusterBytes; }
    std::string lastError;

private:
    M4Storage& storage;
    bool built = false;
    uint64_t builtRevision = 0;
    bool dirtySectors = false;
    // geometry
    uint32_t partStart = 2048, spc = 8, fatSectors = 256, rootEntries = 512;
    uint32_t fatStart = 0, rootStart = 0, dataStart = 0, total = 0, clusterCount = 0, clusterBytes = 4096;
    std::vector<uint8_t> meta;                                  // sectors [0, dataStart)
    std::unordered_map<uint32_t, std::vector<uint8_t>> clusters;  // cluster -> its bytes
    struct Known { bool dir = false; uint64_t size = 0; uint64_t hash = 0; std::string realPath; };
    std::map<std::string, Known> snapshot;                      // lower-cased path -> what the card held

    bool ensureBuilt();
    bool build();
    uint8_t* sector(uint32_t lba, bool forWrite);
};

} // namespace cpcse
