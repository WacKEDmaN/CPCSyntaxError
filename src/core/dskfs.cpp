// CPCSyntaxError — AMSDOS file headers and the CPC disc formats' list.
#include "dskfs.h"

namespace cpcse {

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
std::vector<DiskFormat> listDiskFormats() {
    std::vector<DiskFormat> out;
    for (auto& kv : diskFormats()) out.push_back(kv.second);
    return out;
}

} // namespace cpcse
