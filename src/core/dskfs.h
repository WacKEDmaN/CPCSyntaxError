// CPCSyntaxError — AMSDOS DSK filesystem manager.
#pragma once
#include "common.h"

namespace cpcse {

struct DiskFormat {
    std::string id;
    int tracks = 0, sides = 0, sectors = 0, sectorSize = 0, reservedTracks = 0, blockSize = 0, dirBlocks = 0, sectorBase = 0, gap3 = 0;
    std::string name;
    std::string interleave;
    bool extended = true;
    std::string skew;
    bool valid = false;
};

struct AmsdosHeader {
    int user; std::string filename; std::string ext;
    int type; std::string typeName;
    int length; int loadAddress; int logicalLength; int execAddress; int fullLength;
};

struct DskExtent { int extent; int records; std::vector<int> blocks; int entryOffset; };
struct DskFile {
    int user = 0; std::string filename; std::string ext;
    bool readOnly = false; bool system = false;
    std::vector<DskExtent> extents;
    int size = 0;
    std::optional<AmsdosHeader> header;
};

bool hasAmsdosHeader(const Bytes& data);
std::optional<AmsdosHeader> parseAmsdosHeader(const Bytes& data);
Bytes createAmsdosHeader(const std::string& filename, const std::string& ext, int type, int loadAddr, int execAddr, int dataLength);
std::vector<DiskFormat> listDiskFormats();

struct DiskInfo {
    std::string format; std::string formatId; int tracks, sides, sectors, sectorSize, blockSize, reservedTracks;
    int totalBlocks, usedBlocks, freeBlocks, totalBytes, freeBytes; bool isExtended;
};

class AmstradDSK {
public:
    Bytes data;
    DiskFormat format;
    bool isExtended = false;
    std::string diskLabel;
    bool modified = false;

    bool createNew(const std::string& formatId, const std::string& label = "");
    void initTrack(int trackNum);
    bool load(const Bytes& arrayBuffer);
    DiskFormat detectFormat(int tracks, int sides);
    bool isDirectoryTrack(int track, int side, int sectorBase);
    int detectBlockSizeFromDirectory(int dirTrack, int sectorBase);
    int getTrackOffset(int track, int side = 0);
    Bytes getSectorData(int track, int side, int sectorId, bool* found = nullptr);
    bool setSectorData(int track, int side, int sectorId, const Bytes& sectorData);
    Bytes readBlock(int blockNum);
    void writeBlock(int blockNum, const Bytes& blockData);
    int getTotalBlocks();
    std::unordered_set<int> getUsedBlocks();
    std::vector<int> getFreeBlocks();
    std::vector<DskFile> getDirectory();
    std::optional<AmsdosHeader> readAmsdosHeader(const DskFile& file);
    Bytes readFile(const DskFile& file, bool includeHeader = true);
    Bytes readFileRaw(const DskFile& file);
    bool writeFileData(const DskFile& file, const Bytes& fileData);
    struct HeaderOptions { int type; int loadAddr; int execAddr; };
    bool addFile(const std::string& filename, const std::string& ext, const Bytes& data, int user = 0, bool addHeader = false, const HeaderOptions* headerOptions = nullptr);
    bool writeDirEntry(const Bytes& entryData);
    void deleteFile(const DskFile& file);
    void renameFile(const DskFile& file, const std::string& newName, const std::string& newExt);
    void updateFileAttribute(const DskFile& file, const std::string& attribute, bool value);
    Bytes toBlob() { return data; }
    std::optional<DiskInfo> getDiskInfo();
    bool installCpm22();
    bool isSystemFormat();

private:
    std::vector<DskExtent> createExtentsForFile(const std::vector<int>& allocatedBlocks, int fileLength, int blockSize, int blockPtrSize);
};

} // namespace cpcse
