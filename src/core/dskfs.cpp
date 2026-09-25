// CPCSyntaxError — AMSDOS DSK filesystem manager.
#include "dskfs.h"
#include "cpm22.h"
#include <stdexcept>

namespace cpcse {

static const std::vector<int> SKEW_5_9 = { 0, 5, 1, 6, 2, 7, 3, 8, 4 };
static const std::vector<int> SKEW_5_10 = { 0, 5, 1, 6, 2, 7, 3, 8, 4, 9 };
static const std::vector<int> SKEW_2_9 = { 0, 2, 4, 6, 8, 1, 3, 5, 7 };
static const std::vector<int> SKEW_2_10 = { 0, 2, 4, 6, 8, 1, 3, 5, 7, 9 };
static const std::vector<int>* sectorSkew(const std::string& skew) {
    if (skew == "skew5_9") return &SKEW_5_9;
    if (skew == "skew5_10") return &SKEW_5_10;
    if (skew == "skew2_9") return &SKEW_2_9;
    if (skew == "skew2_10") return &SKEW_2_10;
    return nullptr;
}

static std::vector<std::pair<std::string, DiskFormat>> diskFormats() {
    auto F = [](const std::string& id, int tracks, int sides, int sectors, int sectorSize, int reservedTracks, int blockSize, int dirBlocks, int sectorBase, int gap3, const std::string& name, const std::string& interleave, bool extended, const std::string& skew) {
        DiskFormat f; f.id = id; f.tracks = tracks; f.sides = sides; f.sectors = sectors; f.sectorSize = sectorSize; f.reservedTracks = reservedTracks;
        f.blockSize = blockSize; f.dirBlocks = dirBlocks; f.sectorBase = sectorBase; f.gap3 = gap3; f.name = name; f.interleave = interleave; f.extended = extended; f.skew = skew; f.valid = true;
        return std::make_pair(id, f);
    };
    return {
        F("data",40,1,9,512,0,1024,2,0xC1,0x52,"DATA 40T/9S (178K)","side",true,"skew5_9"),
        F("data42",42,1,9,512,0,1024,2,0xC1,0x52,"DATA 42T/9S (187K)","side",true,"skew5_9"),
        F("system",40,1,9,512,2,1024,2,0x41,0x52,"SYSTEM 40T/9S (169K)","side",true,"skew5_9"),
        F("system42",42,1,9,512,2,1024,2,0x41,0x52,"SYSTEM 42T/9S (178K)","side",true,"skew5_9"),
        F("ibm",40,1,8,512,1,1024,2,0x01,0x50,"IBM 40T/8S (154K)","side",true,"none"),
        F("data80",80,1,9,512,0,2048,1,0xC1,0x52,"DATA 80T/9S (358K)","side",true,"skew2_9"),
        F("system80",80,1,9,512,2,2048,1,0x41,0x52,"SYSTEM 80T/9S (348K)","side",true,"skew2_9"),
        F("parados",80,1,10,512,0,2048,2,0x91,0x10,"ParaDOS SS/80T/10S (400K)","side",true,"skew2_10"),
        F("romdos_d1",80,2,9,512,0,2048,2,0x01,0x10,"ROMDOS D1 DS/80T/9S (716K)","sides",true,"none"),
        F("romdos_d2",80,2,9,512,0,2048,4,0x21,0x10,"ROMDOS D2 DS/80T/9S (712K)","sides",true,"none"),
        F("romdos_d10",80,2,10,512,0,2048,2,0x11,0x10,"ROMDOS D10 DS/80T/10S (796K)","sides",true,"skew2_10"),
        F("romdos_d20",80,2,10,512,0,2048,4,0x31,0x10,"ROMDOS D20 DS/80T/10S (792K)","sides",true,"skew2_10"),
        F("romdos_d40",80,1,10,512,0,2048,2,0x51,0x10,"ROMDOS D40 SS/80T/10S (400K)","side",true,"skew2_10"),
        F("vortex_f1d",80,2,9,512,1,4096,1,0x01,0x4E,"Vortex F1-D DS/80T/9S (704K)","sides",true,"skew2_9"),
        F("dobbertin",80,2,9,512,0,4096,1,0xC1,0x10,"Dobbertin DS/80T/9S (716K)","sides",true,"skew2_9"),
    };
}
static const DiskFormat* findFormat(const std::string& id) {
    static std::vector<std::pair<std::string, DiskFormat>> f = diskFormats();
    for (auto& kv : f) if (kv.first == id) return &kv.second;
    return nullptr;
}

static std::string upperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string padEnd(const std::string& s, int len, char c = ' ') { std::string r = s; while ((int)r.size() < len) r += c; return r; }
static std::string trimStr(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }

static const std::unordered_map<int, std::string> AMSDOS_TYPES = { {0,"BASIC"},{1,"Prot BASIC"},{2,"Binary"},{8,"Binary"},{16,"Binary"},{22,"ASCII"} };

bool hasAmsdosHeader(const Bytes& data) {
    if (data.size() < 128) return false;
    int checksum = 0;
    for (int i = 0; i < 67; i++) checksum = (checksum + data[i]) & 0xffff;
    return checksum == (data[67] | (data[68] << 8));
}
std::optional<AmsdosHeader> parseAmsdosHeader(const Bytes& data) {
    if (!hasAmsdosHeader(data)) return std::nullopt;
    std::string filename, ext;
    for (int i = 1; i <= 8; i++) { int c = data[i] & 0x7f; if (c > 32) filename += (char)c; }
    for (int i = 9; i <= 11; i++) { int c = data[i] & 0x7f; if (c > 32) ext += (char)c; }
    AmsdosHeader h;
    h.user = data[0]; h.filename = trimStr(filename); h.ext = trimStr(ext);
    h.type = data[18]; auto it = AMSDOS_TYPES.find(data[18]); h.typeName = it != AMSDOS_TYPES.end() ? it->second : "Unknown";
    h.length = data[19] | (data[20] << 8);
    h.loadAddress = data[21] | (data[22] << 8);
    h.logicalLength = data[24] | (data[25] << 8);
    h.execAddress = data[26] | (data[27] << 8);
    h.fullLength = data[64] | (data[65] << 8) | (data[66] << 16);
    return h;
}
Bytes createAmsdosHeader(const std::string& filename, const std::string& ext, int type, int loadAddr, int execAddr, int dataLength) {
    Bytes header(128, 0);
    header[0] = 0;
    std::string fname = padEnd(upperStr(filename), 8);
    for (int i = 0; i < 8; i++) header[1 + i] = (uint8_t)fname[i];
    std::string fext = padEnd(upperStr(ext), 3);
    for (int i = 0; i < 3; i++) header[9 + i] = (uint8_t)fext[i];
    header[18] = (uint8_t)(type & 0xff);
    header[19] = (uint8_t)(dataLength & 0xff); header[20] = (uint8_t)((dataLength >> 8) & 0xff);
    header[21] = (uint8_t)(loadAddr & 0xff); header[22] = (uint8_t)((loadAddr >> 8) & 0xff);
    header[23] = 0xff;
    header[24] = (uint8_t)(dataLength & 0xff); header[25] = (uint8_t)((dataLength >> 8) & 0xff);
    header[26] = (uint8_t)(execAddr & 0xff); header[27] = (uint8_t)((execAddr >> 8) & 0xff);
    header[64] = (uint8_t)(dataLength & 0xff); header[65] = (uint8_t)((dataLength >> 8) & 0xff); header[66] = (uint8_t)((dataLength >> 16) & 0xff);
    int checksum = 0;
    for (int i = 0; i < 67; i++) checksum = (checksum + header[i]) & 0xffff;
    header[67] = (uint8_t)(checksum & 0xff); header[68] = (uint8_t)((checksum >> 8) & 0xff);
    return header;
}
std::vector<DiskFormat> listDiskFormats() {
    std::vector<DiskFormat> out;
    for (auto& kv : diskFormats()) out.push_back(kv.second);
    return out;
}

bool AmstradDSK::createNew(const std::string& formatId, const std::string& label) {
    const DiskFormat* fmtp = findFormat(formatId);
    if (!fmtp) throw std::runtime_error("Unknown format: " + formatId);
    format = *fmtp; format.id = formatId;
    diskLabel = label;
    isExtended = format.extended != false;
    int totalTracks = format.tracks * format.sides;
    int trackDataSize = format.sectors * format.sectorSize;
    int trackSize = trackDataSize + 256;
    int diskSize = 256 + totalTracks * trackSize;
    data.assign(diskSize, 0xe5);
    for (int i = 0; i < 256; i++) data[i] = 0;
    std::string sig = isExtended ? "EXTENDED CPC DSK File\r\nDisk-Info\r\n" : "MV - CPCEMU Disk-File\r\nDisk-Info\r\n";
    for (int i = 0; i < (int)sig.size(); i++) data[i] = (uint8_t)sig[i];
    if (isExtended) for (int i = 0; i < (int)label.size() && i < 14; i++) data[0x22 + i] = (uint8_t)label[i];
    data[0x30] = (uint8_t)format.tracks;
    data[0x31] = (uint8_t)format.sides;
    if (isExtended) {
        int trackSizeHigh = (int)std::ceil(trackSize / 256.0);
        for (int t = 0; t < totalTracks; t++) data[0x34 + t] = (uint8_t)trackSizeHigh;
    } else {
        data[0x32] = (uint8_t)(trackSize & 0xff); data[0x33] = (uint8_t)((trackSize >> 8) & 0xff);
    }
    for (int t = 0; t < totalTracks; t++) initTrack(t);
    modified = false;
    return true;
}
void AmstradDSK::initTrack(int trackNum) {
    const DiskFormat& fmt = format;
    int trackDataSize = fmt.sectors * fmt.sectorSize;
    int trackSize = trackDataSize + 256;
    int trackSizeHigh = (int)std::ceil(trackSize / 256.0);
    int offset = isExtended ? 256 + trackNum * (trackSizeHigh * 256) : 256 + trackNum * trackSize;
    int side = fmt.sides > 1 ? (trackNum % 2) : 0;
    int track = fmt.sides > 1 ? trackNum / 2 : trackNum;
    for (int i = 0; i < 256; i++) data[offset + i] = 0x00;
    std::string th = "Track-Info\r\n";
    for (int i = 0; i < (int)th.size(); i++) data[offset + i] = (uint8_t)th[i];
    data[offset + 0x10] = (uint8_t)track;
    data[offset + 0x11] = (uint8_t)side;
    data[offset + 0x14] = 2;
    data[offset + 0x15] = (uint8_t)fmt.sectors;
    data[offset + 0x16] = (uint8_t)fmt.gap3;
    data[offset + 0x17] = 0xe5;
    const std::vector<int>* skewTable = sectorSkew(fmt.skew);
    for (int s = 0; s < fmt.sectors; s++) {
        int so = offset + 0x18 + s * 8;
        int logicalSector = skewTable && s < (int)skewTable->size() ? (*skewTable)[s] : s;
        int sectorId = fmt.sectorBase + logicalSector;
        data[so + 0] = (uint8_t)track; data[so + 1] = (uint8_t)side; data[so + 2] = (uint8_t)sectorId; data[so + 3] = 2;
        data[so + 4] = 0; data[so + 5] = 0;
        if (isExtended) { data[so + 6] = (uint8_t)(fmt.sectorSize & 0xff); data[so + 7] = (uint8_t)((fmt.sectorSize >> 8) & 0xff); }
    }
    for (int i = 0; i < trackDataSize; i++) data[offset + 256 + i] = 0xe5;
}
bool AmstradDSK::load(const Bytes& arrayBuffer) {
    format = DiskFormat{};
    data = arrayBuffer;
    modified = false;
    std::string signature; for (int i = 0; i < 22 && i < (int)data.size(); i++) signature += (char)data[i];
    if (signature.rfind("MV - CPC", 0) == 0) isExtended = false;
    else if (signature.rfind("EXTENDED", 0) == 0) isExtended = true;
    else throw std::runtime_error("Invalid DSK file format - unknown signature");
    int tracks = data[0x30], sides = data[0x31];
    format = detectFormat(tracks, sides);
    if (!format.valid) throw std::runtime_error("Could not detect disk format");
    return true;
}
DiskFormat AmstradDSK::detectFormat(int tracks, int sides) {
    int trackOffset = 256;
    int sectors = data[trackOffset + 0x15];
    int sectorBase = 0xff;
    for (int s = 0; s < sectors && s < 29; s++) {
        int sid = data[trackOffset + 0x18 + s * 8 + 2];
        if (sid > 0 && sid < sectorBase) sectorBase = sid;
    }
    bool hasReservedTracks = !isDirectoryTrack(0, 0, sectorBase);
    std::vector<DiskFormat> candidates;
    for (auto& kv : diskFormats()) {
        const DiskFormat& fmt = kv.second;
        if (fmt.tracks == tracks && fmt.sides == sides && fmt.sectors == sectors && fmt.sectorBase == sectorBase) {
            DiskFormat c = fmt; c.id = kv.first; candidates.push_back(c);
        }
    }
    if (candidates.size() > 1) {
        std::vector<DiskFormat> filtered;
        for (auto& f : candidates) if ((f.reservedTracks > 0) == hasReservedTracks) filtered.push_back(f);
        if (!filtered.empty()) candidates = filtered;
    }
    if (candidates.size() > 1) {
        int dirTrack = hasReservedTracks ? 1 : 0;
        int detectedBlockSize = detectBlockSizeFromDirectory(dirTrack, sectorBase);
        if (detectedBlockSize > 0) {
            std::vector<DiskFormat> filtered;
            for (auto& f : candidates) if (f.blockSize == detectedBlockSize) filtered.push_back(f);
            if (!filtered.empty()) candidates = filtered;
        }
    }
    if (!candidates.empty()) return candidates[0];
    int blockSize = tracks >= 80 ? 2048 : 1024;
    int reservedTracks = hasReservedTracks ? 2 : 0;
    DiskFormat custom; custom.id = "custom"; custom.name = "Custom " + std::to_string(tracks) + "T/" + std::to_string(sides) + "S/" + std::to_string(sectors) + "Sec";
    custom.tracks = tracks; custom.sides = sides; custom.sectors = sectors; custom.sectorSize = 512; custom.reservedTracks = reservedTracks;
    custom.blockSize = blockSize; custom.dirBlocks = 2; custom.sectorBase = sectorBase; custom.gap3 = 0x4e; custom.interleave = "side"; custom.extended = isExtended; custom.skew = "none"; custom.valid = true;
    return custom;
}
bool AmstradDSK::isDirectoryTrack(int track, int side, int sectorBase) {
    bool found = false;
    Bytes sector = getSectorData(track, side, sectorBase, &found);
    if (!found || sector.size() < 32) return false;
    int validEntries = 0, emptyEntries = 0;
    int entriesPerSector = (int)sector.size() / 32;
    for (int e = 0; e < entriesPerSector; e++) {
        int offset = e * 32;
        int userByte = sector[offset];
        if (userByte == 0xe5) { emptyEntries++; continue; }
        if (userByte > 15) return false;
        bool validFilename = true;
        for (int i = 1; i <= 11; i++) { int c = sector[offset + i] & 0x7f; if (c < 32 || c > 126) { validFilename = false; break; } }
        if (validFilename) validEntries++; else return false;
    }
    return (validEntries > 0) || (emptyEntries == entriesPerSector);
}
int AmstradDSK::detectBlockSizeFromDirectory(int dirTrack, int sectorBase) {
    bool found = false;
    Bytes sector = getSectorData(dirTrack, 0, sectorBase, &found);
    if (!found || sector.size() < 32) return 0;
    for (int e = 0; e < (int)sector.size() / 32; e++) {
        int offset = e * 32;
        int userByte = sector[offset];
        if (userByte <= 15) {
            int firstBlock = sector[offset + 16];
            if (firstBlock > 0) {
                int maxBlock = 0;
                for (int i = 16; i < 32; i++) { int blk = sector[offset + i]; if (blk > maxBlock) maxBlock = blk; }
                int sectorsPerBlock = (int)sector.size() / 512;
                int totalBlocks = (maxBlock * 1024) / 512 / sectorsPerBlock + 1;
                int blockBytes = totalBlocks * 1024;
                if (blockBytes > 2048) return 2048;
                if (blockBytes > 1024) return 1024;
                return 512;
            }
        }
    }
    return 0;
}
int AmstradDSK::getTrackOffset(int track, int side) {
    const DiskFormat& fmt = format;
    if (!fmt.valid) return -1;
    int trackNum = fmt.sides > 1 ? (track * 2 + side) : track;
    int totalTracks = fmt.tracks * fmt.sides;
    if (trackNum < 0 || trackNum >= totalTracks) return -1;
    if (isExtended) {
        int offset = 256;
        for (int t = 0; t < trackNum; t++) { int size = data[0x34 + t] * 256; if (size == 0) break; offset += size; }
        return offset;
    } else {
        int trackSize = data[0x32] | (data[0x33] << 8);
        return 256 + trackNum * trackSize;
    }
}
Bytes AmstradDSK::getSectorData(int track, int side, int sectorId, bool* found) {
    if (found) *found = false;
    int trackOffset = getTrackOffset(track, side);
    if (trackOffset < 0 || trackOffset >= (int)data.size()) return {};
    int sectors = data[trackOffset + 0x15];
    int sectorDataOffset = trackOffset + 256;
    for (int s = 0; s < sectors; s++) {
        int infoOffset = trackOffset + 0x18 + s * 8;
        if (infoOffset + 8 > (int)data.size()) return {};
        int sid = data[infoOffset + 2];
        int sizeCode = data[infoOffset + 3];
        int sectorSize;
        if (isExtended) { sectorSize = data[infoOffset + 6] | (data[infoOffset + 7] << 8); if (sectorSize == 0) sectorSize = 128 << sizeCode; }
        else sectorSize = 128 << sizeCode;
        if (sid == sectorId) {
            if (sectorDataOffset + sectorSize > (int)data.size()) return {};
            if (found) *found = true;
            return Bytes(data.begin() + sectorDataOffset, data.begin() + sectorDataOffset + sectorSize);
        }
        sectorDataOffset += sectorSize;
    }
    return {};
}
bool AmstradDSK::setSectorData(int track, int side, int sectorId, const Bytes& sectorData) {
    int trackOffset = getTrackOffset(track, side);
    if (trackOffset < 0) return false;
    int sectors = data[trackOffset + 0x15];
    int sectorDataOffset = trackOffset + 256;
    for (int s = 0; s < sectors; s++) {
        int infoOffset = trackOffset + 0x18 + s * 8;
        int sid = data[infoOffset + 2];
        int sizeCode = data[infoOffset + 3];
        int sectorSize;
        if (isExtended) { sectorSize = data[infoOffset + 6] | (data[infoOffset + 7] << 8); if (sectorSize == 0) sectorSize = 128 << sizeCode; }
        else sectorSize = 128 << sizeCode;
        if (sid == sectorId) {
            int len = std::min((int)sectorData.size(), sectorSize);
            for (int i = 0; i < len; i++) data[sectorDataOffset + i] = sectorData[i];
            modified = true;
            return true;
        }
        sectorDataOffset += sectorSize;
    }
    return false;
}
Bytes AmstradDSK::readBlock(int blockNum) {
    const DiskFormat& fmt = format;
    Bytes result(fmt.blockSize, 0);
    int sectorsPerBlock = fmt.blockSize / fmt.sectorSize;
    int sectorsPerTrack = fmt.sectors;
    int reservedSectors = fmt.reservedTracks * sectorsPerTrack * fmt.sides;
    int startSector = blockNum * sectorsPerBlock + reservedSectors;
    for (int i = 0; i < sectorsPerBlock; i++) {
        int sectorNum = startSector + i;
        int sectorsPerCylinder = sectorsPerTrack * fmt.sides;
        int track = sectorNum / sectorsPerCylinder;
        int sectorInCylinder = sectorNum % sectorsPerCylinder;
        int side = sectorInCylinder / sectorsPerTrack;
        int sectorInTrack = sectorInCylinder % sectorsPerTrack;
        int sectorId = fmt.sectorBase + sectorInTrack;
        bool found = false;
        Bytes sectorData = getSectorData(track, side, sectorId, &found);
        if (found) {
            int copyLen = std::min((int)sectorData.size(), fmt.sectorSize);
            for (int k = 0; k < copyLen; k++) result[i * fmt.sectorSize + k] = sectorData[k];
        }
    }
    return result;
}
void AmstradDSK::writeBlock(int blockNum, const Bytes& blockData) {
    const DiskFormat& fmt = format;
    int sectorsPerBlock = fmt.blockSize / fmt.sectorSize;
    int sectorsPerTrack = fmt.sectors;
    int reservedSectors = fmt.reservedTracks * sectorsPerTrack * fmt.sides;
    int startSector = blockNum * sectorsPerBlock + reservedSectors;
    for (int i = 0; i < sectorsPerBlock; i++) {
        int sectorNum = startSector + i;
        int sectorsPerCylinder = sectorsPerTrack * fmt.sides;
        int track = sectorNum / sectorsPerCylinder;
        int sectorInCylinder = sectorNum % sectorsPerCylinder;
        int side = sectorInCylinder / sectorsPerTrack;
        int sectorInTrack = sectorInCylinder % sectorsPerTrack;
        int sectorId = fmt.sectorBase + sectorInTrack;
        Bytes sectorData(fmt.sectorSize, 0);
        int srcOffset = i * fmt.sectorSize;
        int len = std::min(fmt.sectorSize, (int)blockData.size() - srcOffset);
        if (len > 0) for (int k = 0; k < len; k++) sectorData[k] = blockData[srcOffset + k];
        for (int j = len; j < fmt.sectorSize; j++) sectorData[j] = 0xe5;
        setSectorData(track, side, sectorId, sectorData);
    }
    modified = true;
}
int AmstradDSK::getTotalBlocks() {
    const DiskFormat& fmt = format;
    int totalSectors = (fmt.tracks - fmt.reservedTracks) * fmt.sides * fmt.sectors;
    return (totalSectors * fmt.sectorSize) / fmt.blockSize;
}
std::unordered_set<int> AmstradDSK::getUsedBlocks() {
    const DiskFormat& fmt = format;
    std::unordered_set<int> used;
    int blockPtrSize = getTotalBlocks() > 255 ? 2 : 1;
    for (int b = 0; b < fmt.dirBlocks; b++) {
        used.insert(b);
        Bytes block = readBlock(b);
        int entriesPerBlock = fmt.blockSize / 32;
        for (int e = 0; e < entriesPerBlock; e++) {
            int offset = e * 32;
            int user = block[offset];
            if (user <= 15) {
                for (int i = 16; i < 32; i += blockPtrSize) {
                    int blk = blockPtrSize == 2 ? (block[offset + i] | (block[offset + i + 1] << 8)) : block[offset + i];
                    if (blk > 0) used.insert(blk);
                }
            }
        }
    }
    return used;
}
std::vector<int> AmstradDSK::getFreeBlocks() {
    std::unordered_set<int> used = getUsedBlocks();
    int total = getTotalBlocks();
    std::vector<int> free;
    for (int b = 0; b < total; b++) if (!used.count(b)) free.push_back(b);
    return free;
}
std::vector<DskFile> AmstradDSK::getDirectory() {
    const DiskFormat& fmt = format;
    std::vector<DskFile> entries;
    std::unordered_map<std::string, int> fileMap;
    int blockPtrSize = getTotalBlocks() > 255 ? 2 : 1;
    for (int b = 0; b < fmt.dirBlocks; b++) {
        Bytes block = readBlock(b);
        int entriesPerBlock = fmt.blockSize / 32;
        for (int e = 0; e < entriesPerBlock; e++) {
            int offset = e * 32;
            int entryOffset = b * fmt.blockSize + offset;
            int userByte = block[offset];
            if (userByte != 0xe5 && userByte <= 15) {
                std::string filename;
                for (int i = 1; i <= 8; i++) { int c = block[offset + i] & 0x7f; if (c > 32) filename += (char)c; }
                filename = trimStr(filename);
                std::string ext;
                for (int i = 9; i <= 11; i++) { int c = block[offset + i] & 0x7f; if (c > 32) ext += (char)c; }
                ext = trimStr(ext);
                int ex = block[offset + 12] & 0x1f;
                int s2 = block[offset + 14];
                int fullExtent = ex + (s2 << 5);
                int rc = block[offset + 15];
                bool readOnly = (block[offset + 9] & 0x80) != 0;
                bool system = (block[offset + 10] & 0x80) != 0;
                std::vector<int> blocks;
                for (int i = 16; i < 32; i += blockPtrSize) {
                    int blk = blockPtrSize == 2 ? (block[offset + i] | (block[offset + i + 1] << 8)) : block[offset + i];
                    if (blk > 0) blocks.push_back(blk);
                }
                std::string key = std::to_string(userByte) + ":" + filename + "." + ext;
                DskExtent extent{ fullExtent, rc, blocks, entryOffset };
                auto it = fileMap.find(key);
                if (it != fileMap.end()) entries[it->second].extents.push_back(extent);
                else {
                    DskFile fileEntry; fileEntry.user = userByte; fileEntry.filename = filename; fileEntry.ext = ext;
                    fileEntry.readOnly = readOnly; fileEntry.system = system; fileEntry.extents.push_back(extent);
                    fileMap[key] = (int)entries.size();
                    entries.push_back(fileEntry);
                }
            }
        }
    }
    for (DskFile& file : entries) {
        std::sort(file.extents.begin(), file.extents.end(), [](const DskExtent& a, const DskExtent& b) { return a.extent < b.extent; });
        if (!file.extents.empty()) { const DskExtent& lastExt = file.extents.back(); file.size = lastExt.extent * 16384 + lastExt.records * 128; }
        else file.size = 0;
        file.header = readAmsdosHeader(file);
        if (file.header) file.size = file.header->logicalLength;
    }
    return entries;
}
std::optional<AmsdosHeader> AmstradDSK::readAmsdosHeader(const DskFile& file) {
    if (file.extents.empty() || file.extents[0].blocks.empty()) return std::nullopt;
    Bytes firstBlock = readBlock(file.extents[0].blocks[0]);
    if (firstBlock.size() < 128) return std::nullopt;
    return hasAmsdosHeader(firstBlock) ? parseAmsdosHeader(firstBlock) : std::nullopt;
}
Bytes AmstradDSK::readFile(const DskFile& file, bool includeHeader) {
    Bytes result;
    for (const DskExtent& ext : file.extents) for (int blockNum : ext.blocks) { Bytes b = readBlock(blockNum); result.insert(result.end(), b.begin(), b.end()); }
    int actualSize = file.header ? file.header->logicalLength : file.size;
    int totalSize = actualSize + (file.header ? 128 : 0);
    if (!includeHeader && file.header) {
        int dataSize = std::min(actualSize, (int)result.size() - 128);
        if (dataSize <= 0) return {};
        return Bytes(result.begin() + 128, result.begin() + 128 + dataSize);
    } else {
        int returnSize = std::min(totalSize, (int)result.size());
        return Bytes(result.begin(), result.begin() + returnSize);
    }
}
Bytes AmstradDSK::readFileRaw(const DskFile& file) {
    Bytes result;
    for (const DskExtent& ext : file.extents) for (int blockNum : ext.blocks) { Bytes b = readBlock(blockNum); result.insert(result.end(), b.begin(), b.end()); }
    return result;
}
bool AmstradDSK::writeFileData(const DskFile& file, const Bytes& fileData) {
    const DiskFormat& fmt = format;
    int offset = 0;
    for (const DskExtent& ext : file.extents) {
        for (int blockNum : ext.blocks) {
            Bytes blockData(fmt.blockSize, 0xe5);
            int len = std::min(fmt.blockSize, (int)fileData.size() - offset);
            if (len > 0) for (int k = 0; k < len; k++) blockData[k] = fileData[offset + k];
            writeBlock(blockNum, blockData);
            offset += fmt.blockSize;
        }
    }
    modified = true;
    return true;
}
std::vector<DskExtent> AmstradDSK::createExtentsForFile(const std::vector<int>& allocatedBlocks, int fileLength, int blockSize, int blockPtrSize) {
    std::vector<DskExtent> extents;
    int blocksPerExtent = 16 / blockPtrSize;
    int bytesPerExtent = blocksPerExtent * blockSize;
    int blockIndex = 0, bytesRemaining = fileLength, extentNumber = 0;
    while (blockIndex < (int)allocatedBlocks.size()) {
        std::vector<int> blocksInThisExtent;
        for (int i = 0; i < blocksPerExtent && blockIndex < (int)allocatedBlocks.size(); i++) { blocksInThisExtent.push_back(allocatedBlocks[blockIndex]); blockIndex++; }
        int bytesInExtent = std::min(bytesRemaining, bytesPerExtent);
        int records = (int)std::ceil(bytesInExtent / 128.0);
        int rc = records > 128 ? 128 : records;
        extents.push_back({ extentNumber, rc, blocksInThisExtent, -1 });
        bytesRemaining -= bytesInExtent;
        extentNumber++;
    }
    return extents;
}
bool AmstradDSK::addFile(const std::string& filename, const std::string& ext, const Bytes& dataIn, int user, bool addHeader, const HeaderOptions* headerOptions) {
    const DiskFormat& fmt = format;
    Bytes dataToWrite = dataIn;
    if (addHeader && headerOptions) {
        Bytes header = createAmsdosHeader(filename, ext, headerOptions->type, headerOptions->loadAddr, headerOptions->execAddr, (int)dataIn.size());
        dataToWrite.assign(128 + dataIn.size(), 0);
        for (int i = 0; i < 128; i++) dataToWrite[i] = header[i];
        for (int i = 0; i < (int)dataIn.size(); i++) dataToWrite[128 + i] = dataIn[i];
    }
    int blocksNeeded = (int)std::ceil((double)dataToWrite.size() / fmt.blockSize);
    std::vector<int> freeBlocks = getFreeBlocks();
    if (blocksNeeded > (int)freeBlocks.size()) throw std::runtime_error("Not enough space on disk");
    std::vector<int> allocatedBlocks(freeBlocks.begin(), freeBlocks.begin() + blocksNeeded);
    for (int i = 0; i < (int)allocatedBlocks.size(); i++) {
        int blockNum = allocatedBlocks[i];
        Bytes blockData(fmt.blockSize, 0xe5);
        int srcOffset = i * fmt.blockSize;
        int len = std::min(fmt.blockSize, (int)dataToWrite.size() - srcOffset);
        if (len > 0) for (int k = 0; k < len; k++) blockData[k] = dataToWrite[srcOffset + k];
        writeBlock(blockNum, blockData);
    }
    int blockPtrSize = getTotalBlocks() > 255 ? 2 : 1;
    std::vector<DskExtent> extents = createExtentsForFile(allocatedBlocks, (int)dataToWrite.size(), fmt.blockSize, blockPtrSize);
    for (const DskExtent& extent : extents) {
        Bytes entry(32, 0);
        entry[0] = (uint8_t)user;
        std::string fname = padEnd(upperStr(filename), 8);
        for (int i = 0; i < 8; i++) entry[1 + i] = (uint8_t)fname[i];
        std::string fext = padEnd(upperStr(ext), 3);
        for (int i = 0; i < 3; i++) entry[9 + i] = (uint8_t)fext[i];
        entry[12] = (uint8_t)(extent.extent & 0x1f);
        entry[14] = (uint8_t)((extent.extent >> 5) & 0xff);
        entry[15] = (uint8_t)extent.records;
        int bpOffset = 16;
        for (int blk : extent.blocks) {
            if (blockPtrSize == 2) { entry[bpOffset] = (uint8_t)(blk & 0xff); entry[bpOffset + 1] = (uint8_t)((blk >> 8) & 0xff); bpOffset += 2; }
            else { entry[bpOffset] = (uint8_t)blk; bpOffset++; }
        }
        if (!writeDirEntry(entry)) throw std::runtime_error("No free directory entries");
    }
    modified = true;
    return true;
}
bool AmstradDSK::writeDirEntry(const Bytes& entryData) {
    const DiskFormat& fmt = format;
    for (int b = 0; b < fmt.dirBlocks; b++) {
        Bytes block = readBlock(b);
        int entriesPerBlock = fmt.blockSize / 32;
        for (int e = 0; e < entriesPerBlock; e++) {
            int offset = e * 32;
            if (block[offset] == 0xe5) {
                for (int i = 0; i < (int)entryData.size(); i++) block[offset + i] = entryData[i];
                writeBlock(b, block);
                return true;
            }
        }
    }
    return false;
}
void AmstradDSK::deleteFile(const DskFile& file) {
    const DiskFormat& fmt = format;
    for (const DskExtent& ext : file.extents) {
        int blockNum = ext.entryOffset / fmt.blockSize;
        Bytes block = readBlock(blockNum);
        int offset = ext.entryOffset % fmt.blockSize;
        block[offset] = 0xe5;
        writeBlock(blockNum, block);
    }
    modified = true;
}
void AmstradDSK::renameFile(const DskFile& file, const std::string& newName, const std::string& newExt) {
    const DiskFormat& fmt = format;
    for (const DskExtent& ext : file.extents) {
        int blockNum = ext.entryOffset / fmt.blockSize;
        Bytes block = readBlock(blockNum);
        int offset = ext.entryOffset % fmt.blockSize;
        std::string fname = padEnd(upperStr(newName), 8);
        for (int i = 0; i < 8; i++) { int cur = block[offset + 1 + i]; int nb = fname[i]; block[offset + 1 + i] = (uint8_t)((cur & 0x80) | (nb & 0x7f)); }
        std::string fext = padEnd(upperStr(newExt), 3);
        for (int i = 0; i < 3; i++) { int cur = block[offset + 9 + i]; int nb = fext[i]; block[offset + 9 + i] = (uint8_t)((cur & 0x80) | (nb & 0x7f)); }
        writeBlock(blockNum, block);
    }
    modified = true;
}
void AmstradDSK::updateFileAttribute(const DskFile& file, const std::string& attribute, bool value) {
    const DiskFormat& fmt = format;
    for (const DskExtent& ext : file.extents) {
        int blockNum = ext.entryOffset / fmt.blockSize;
        Bytes block = readBlock(blockNum);
        int offset = ext.entryOffset % fmt.blockSize;
        if (attribute == "readOnly") { if (value) block[offset + 9] |= 0x80; else block[offset + 9] &= 0x7f; }
        else if (attribute == "system") { if (value) block[offset + 10] |= 0x80; else block[offset + 10] &= 0x7f; }
        else if (attribute == "user") block[offset] = (uint8_t)(value & 0x0f);
        writeBlock(blockNum, block);
    }
    modified = true;
}
std::optional<DiskInfo> AmstradDSK::getDiskInfo() {
    if (!format.valid) return std::nullopt;
    const DiskFormat& fmt = format;
    int totalBlocks = getTotalBlocks();
    int usedBlocks = (int)getUsedBlocks().size();
    int freeBlocks = totalBlocks - usedBlocks;
    DiskInfo info; info.format = fmt.name; info.formatId = fmt.id; info.tracks = fmt.tracks; info.sides = fmt.sides;
    info.sectors = fmt.sectors; info.sectorSize = fmt.sectorSize; info.blockSize = fmt.blockSize; info.reservedTracks = fmt.reservedTracks;
    info.totalBlocks = totalBlocks; info.usedBlocks = usedBlocks; info.freeBlocks = freeBlocks;
    info.totalBytes = totalBlocks * fmt.blockSize; info.freeBytes = freeBlocks * fmt.blockSize; info.isExtended = isExtended;
    return info;
}
bool AmstradDSK::installCpm22() {
    const DiskFormat& fmt = format;
    if (!fmt.valid) throw std::runtime_error("No disk format loaded");
    if (!fmt.reservedTracks || fmt.reservedTracks < 2) throw std::runtime_error("CP/M 2.2 needs a SYSTEM disk (2 reserved tracks)");
    Bytes sys = cpm22SystemBytes();
    int sectorsPerTrack = fmt.sectors;
    int sectorSize = fmt.sectorSize;
    int offset = 0;
    for (int track = 0; track < 2; track++) {
        for (int n = 0; n < sectorsPerTrack; n++) {
            Bytes sectorData(sectorSize, 0);
            int len = std::min(sectorSize, (int)sys.size() - offset);
            if (len > 0) for (int k = 0; k < len; k++) sectorData[k] = sys[offset + k];
            for (int j = len; j < sectorSize; j++) sectorData[j] = 0xe5;
            bool ok = setSectorData(track, 0, fmt.sectorBase + n, sectorData);
            if (!ok) throw std::runtime_error("Failed to write CP/M system to track " + std::to_string(track) + ", sector " + std::to_string(n));
            offset += sectorSize;
        }
    }
    modified = true;
    return true;
}
bool AmstradDSK::isSystemFormat() { return format.valid && format.reservedTracks > 0; }

} // namespace cpcse
