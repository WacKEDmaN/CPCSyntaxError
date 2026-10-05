// CPCSyntaxError — AMSDOS file headers and the CPC disc formats' list (the DSK editor's
// filesystem itself is dsk_edit's DskFs).
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

bool hasAmsdosHeader(const Bytes& data);
std::optional<AmsdosHeader> parseAmsdosHeader(const Bytes& data);
std::vector<DiskFormat> listDiskFormats();

} // namespace cpcse
