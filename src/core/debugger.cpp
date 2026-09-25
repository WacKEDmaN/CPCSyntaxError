// CPCSyntaxError — debugger tooling helpers (headless byte-level exports).
// The interactive debugger is the GUI's (src/gui/gui_debugger.*).
#include "debugger.h"
#include <cstdio>
#include <algorithm>

namespace cpcse {

// ---- CRC32 (IEEE 802.3 / zlib polynomial) ----------------------------
uint32_t zipCrc32(const Bytes& bytes) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (int n = 0; n < 256; n++) {
            uint32_t c = (uint32_t)n;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[n] = c;
        }
        built = true;
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < bytes.size(); i++) crc = table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

// ---- ZIP archive (STORE method) --------------------------------------
namespace {
void putU16(Bytes& out, uint32_t v) { out.push_back(v & 0xff); out.push_back((v >> 8) & 0xff); }
void putU32(Bytes& out, uint32_t v) {
    out.push_back(v & 0xff); out.push_back((v >> 8) & 0xff);
    out.push_back((v >> 16) & 0xff); out.push_back((v >> 24) & 0xff);
}
} // namespace

Bytes buildZipArchive(const std::vector<ZipEntry>& files) {
    Bytes parts;      // local headers + data
    Bytes central;    // central directory
    uint32_t offset = 0;

    for (const auto& file : files) {
        Bytes nameBytes;
        for (char c : file.name) nameBytes.push_back((uint8_t)(c & 0xff));
        const Bytes& data = file.bytes;
        uint32_t crc = zipCrc32(data);

        // local file header
        putU32(parts, 0x04034b50);          // signature
        putU16(parts, 20);                  // version needed
        putU16(parts, 0);                   // flags
        putU16(parts, 0);                   // method (store)
        putU16(parts, 0); putU16(parts, 0); // mod time/date
        putU32(parts, crc);
        putU32(parts, (uint32_t)data.size()); // compressed size
        putU32(parts, (uint32_t)data.size()); // uncompressed size
        putU16(parts, (uint32_t)nameBytes.size()); // filename length
        putU16(parts, 0);                   // extra length
        parts.insert(parts.end(), nameBytes.begin(), nameBytes.end());
        parts.insert(parts.end(), data.begin(), data.end());
        uint32_t localLen = 30u + (uint32_t)nameBytes.size() + (uint32_t)data.size();

        // central directory record
        putU32(central, 0x02014b50);        // signature
        putU16(central, 20);                // version made by
        putU16(central, 20);                // version needed
        putU16(central, 0);                 // flags
        putU16(central, 0);                 // method (store)
        putU16(central, 0); putU16(central, 0); // mod time/date
        putU32(central, crc);
        putU32(central, (uint32_t)data.size()); // compressed size
        putU32(central, (uint32_t)data.size()); // uncompressed size
        putU16(central, (uint32_t)nameBytes.size()); // filename length
        putU16(central, 0);                 // extra length
        putU16(central, 0);                 // comment length
        putU16(central, 0);                 // disk number start
        putU16(central, 0);                 // internal attrs
        putU32(central, 0);                 // external attrs
        putU32(central, offset);            // local header offset
        central.insert(central.end(), nameBytes.begin(), nameBytes.end());

        offset += localLen;
    }

    uint32_t centralSize = (uint32_t)central.size();
    Bytes out;
    out.insert(out.end(), parts.begin(), parts.end());
    out.insert(out.end(), central.begin(), central.end());
    // end of central directory
    putU32(out, 0x06054b50);                // signature
    putU16(out, 0);                         // disk number
    putU16(out, 0);                         // disk with central dir
    putU16(out, (uint32_t)files.size());    // entries this disk
    putU16(out, (uint32_t)files.size());    // total entries
    putU32(out, centralSize);               // central dir size
    putU32(out, offset);                    // central dir offset
    putU16(out, 0);                         // comment length
    return out;
}

// ---- multi-region AMSDOS export naming -------------------------------
std::string regionFileBaseName(const std::string& name, int count, int bank) {
    size_t dot = name.rfind('.');
    std::string baseNoExt = (dot != std::string::npos && dot > 0) ? name.substr(0, dot) : name;
    std::string ext = (dot != std::string::npos && dot > 0) ? name.substr(dot + 1) : "BIN";
    int bankDigit = ((bank == 0 ? 0xC0 : bank)) & 7;
    if (bankDigit != 0) {
        char h[4]; std::snprintf(h, sizeof(h), "%X", bankDigit);
        std::string tag = std::string("BC") + h;
        return count == 0 ? baseNoExt + "." + tag : baseNoExt + "." + tag + std::to_string(count);
    }
    if (count == 0) return name;
    return baseNoExt + "." + ext.substr(0, std::min<size_t>(2, ext.size())) + std::to_string(count);
}

} // namespace cpcse
