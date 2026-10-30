// CPCSyntaxError GUI — CPCtelera's disc and tape images. See cpct_media.h.
#include "cpct_media.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "core/dsk.h"
#include "core/dsk_edit.h"

namespace cpcse {

namespace {

// The AMSDOS header test iDSK and cpc2cdt make: the 16-bit sum of bytes 0-66 at 67, and
// not 0 (a run of zeros is no header).
bool hasHeader(const std::vector<uint8_t>& d) {
    if (d.size() < 128) return false;
    unsigned sum = 0;
    for (int i = 0; i < 67; i++) sum += d[(size_t)i];
    return sum != 0 && (sum & 0xffff) == (unsigned)(d[0x43] | d[0x44] << 8);
}

// iDSK's FiltrerNom: the part after the last '/', up to its first '.' (8 at most) and the
// 3 characters after that '.', upper case
void discName(const std::string& hostName, std::string& name, std::string& ext) {
    std::string base = hostName;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.find('.');
    name = base.substr(0, std::min<size_t>(dot == std::string::npos ? base.size() : dot, 8));
    ext = dot == std::string::npos ? "" : base.substr(dot + 1, 3);
    for (char& c : name) c = (char)std::toupper((unsigned char)c);
    for (char& c : ext) c = (char)std::toupper((unsigned char)c);
}

DskGeometry discGeometry() {
    DskGeometry g = dskPreset("data").value_or(DskGeometry{});
    g.tracks = 42;
    g.gap3 = 0x4e;
    g.interleave = { 0, 5, 1, 6, 2, 7, 3, 8, 4 };
    g.blocks = 180;                    // AMSDOS's DATA format: DSM 179, whatever the tracks
    return g;
}

void put16(std::vector<uint8_t>& o, int v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }

} // namespace

std::shared_ptr<Disk> cpctNewDisc() {
    std::shared_ptr<Disk> d = dskCreate(discGeometry(), "iDSK");
    d->extended = false;
    return d;
}

std::vector<uint8_t> cpctDiscImage(const Disk& disk) {
    std::vector<uint8_t> image = serializeDsk(disk, !disk.extended);
    if (disk.extended || image.size() < 0x100) return image;
    std::fill(image.begin() + 0x22, image.begin() + 0x30, 0);       // iDSK names no creator
    // iDSK's standard image puts each sector's length in the two bytes after its ID and
    // status (the extended format's field; the standard format leaves them 0)
    const size_t trackSize = (size_t)(image[0x32] | image[0x33] << 8);
    for (size_t at = 0x100; trackSize && at + 0x18 <= image.size(); at += trackSize) {
        if (std::memcmp(&image[at], "Track-Info", 10) != 0) continue;
        const int sectors = image[at + 0x15];
        for (int s = 0; s < sectors && at + 0x18 + (size_t)s * 8 + 8 <= image.size(); s++) {
            uint8_t* e = &image[at + 0x18 + (size_t)s * 8];
            const int length = 128 << (e[3] & 7);
            e[6] = (uint8_t)length; e[7] = (uint8_t)(length >> 8);
        }
    }
    return image;
}

std::string cpctDiscImport(Disk& disk, const std::string& hostName, const std::vector<uint8_t>& data, int type, int load, int exec) {
    if (data.size() > 0x10080) return hostName + " is over 64K";
    std::string name, ext;
    discName(hostName, name, ext);
    std::vector<uint8_t> file;
    const bool header = hasHeader(data);
    if (type == 0) {
        file.assign(data.begin() + (header ? 128 : 0), data.end());      // text: any header goes
    } else if (header) {
        file = data;                                                       // its own header is kept
    } else {
        // iDSK's header: the name, type 2, the length; the load and entry when not 0
        file = dskAmsdosHeader(0, name, ext, 2, load & 0xffff, exec & 0xffff, (int)data.size());
        file.insert(file.end(), data.begin(), data.end());
    }
    DskFs fs(std::shared_ptr<Disk>(&disk, [](Disk*) {}), discGeometry());
    const std::string why = fs.write(0, name, ext, file);
    if (!why.empty()) return why;
    // iDSK copies whole blocks from a zeroed buffer: the last block's bytes after the file
    // are 0 (DskFs pads the last record with &1A and leaves the rest as formatted)
    for (const DskFsFile& f : fs.files()) {
        if (f.user != 0 || f.name != name || f.ext != ext || f.blocks.empty()) continue;
        const int blockSize = fs.geometry().blockSize;
        const size_t used = file.size() - (f.blocks.size() - 1) * (size_t)blockSize;
        Bytes last = fs.readBlock(f.blocks.back());
        if (used < last.size()) {
            std::fill(last.begin() + (std::ptrdiff_t)used, last.end(), 0);
            fs.writeBlock(f.blocks.back(), last);
        }
    }
    return "";
}

std::string cpctDiscWriteSectors(Disk& disk, const std::vector<uint8_t>& data, const std::string& location) {
    // the location list: words split by spaces; each "T", "T-T2" or "T+" (to the last
    // track), then ":R" or ":R-R2" (else every sector of the track, as they lie on it)
    struct Loc { int track; int sector; };            // sector -1: the whole track
    std::vector<Loc> locs;
    std::string upper = location;
    for (char& c : upper) c = (char)std::toupper((unsigned char)c);
    auto number = [](const std::string& s, bool& ok) {
        std::string t = s;
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        long v = 0;
        char* end = nullptr;
        if (t.size() > 1 && (t[0] == '#' || t[0] == '&' || t[0] == '$')) v = std::strtol(t.c_str() + 1, &end, 16);
        else if (t.size() > 2 && t[0] == '0' && t[1] == 'X') v = std::strtol(t.c_str() + 2, &end, 16);
        else if (!t.empty() && (t.back() == 'H')) { t.pop_back(); v = std::strtol(t.c_str(), &end, 16); }
        else v = std::strtol(t.c_str(), &end, 10);
        ok = !t.empty() && end && !*end;
        return (int)v;
    };
    size_t at = 0;
    while (at < upper.size()) {
        while (at < upper.size() && upper[at] == ' ') at++;
        if (at >= upper.size()) break;
        size_t end = upper.find(' ', at);
        if (end == std::string::npos) end = upper.size();
        const std::string word = upper.substr(at, end - at);
        at = end;
        const size_t colon = word.find(':');
        const std::string tracks = word.substr(0, colon);
        const std::string sectors = colon == std::string::npos ? "" : word.substr(colon + 1);
        bool ok1 = true, ok2 = true;
        int t0, t1;
        if (tracks.find('+') != std::string::npos) { t0 = number(tracks.substr(0, tracks.find('+')), ok1); t1 = disk.tracks - 1; }
        else if (tracks.find('-') != std::string::npos) { t0 = number(tracks.substr(0, tracks.find('-')), ok1); t1 = number(tracks.substr(tracks.find('-') + 1), ok2); }
        else { t0 = t1 = number(tracks, ok1); }
        if (!ok1 || !ok2 || t1 < t0) return "location '" + word + "': track interval error";
        int s0 = -1, s1 = 256;
        if (!sectors.empty()) {
            if (sectors.find('-') != std::string::npos) { s0 = number(sectors.substr(0, sectors.find('-')), ok1); s1 = number(sectors.substr(sectors.find('-') + 1), ok2); }
            else { s0 = s1 = number(sectors, ok1); }
            if (!ok1 || !ok2 || s1 < s0) return "location '" + word + "': sector interval error";
        }
        for (int t = t0; t <= t1; t++) {
            if (s0 >= 0 && s0 < 256 && s1 < 256) for (int s = s0; s <= s1; s++) locs.push_back({ t, s });
            else locs.push_back({ t, -1 });
        }
    }
    if (locs.empty()) return "no location";
    size_t left = data.size(), from = 0;
    for (const Loc& l : locs) {
        if (l.track >= disk.tracks) return "track " + std::to_string(l.track) + " is not on the disc (" + std::to_string(disk.tracks) + " tracks)";
        if (disk.trackData.size() <= (size_t)l.track || disk.trackData[(size_t)l.track].empty() || !disk.trackData[(size_t)l.track][0])
            return "track " + std::to_string(l.track) + " is not formatted";
        Track& tr = *disk.trackData[(size_t)l.track][0];
        for (Sector& s : tr.sectors) {
            if (l.sector >= 0 && s.r != l.sector) continue;
            if (s.data.empty()) s.data.emplace_back((size_t)(128 << (s.n & 7)), (uint8_t)tr.filler);
            Bytes& b = s.data[0];
            for (size_t i = 0; left && i < b.size(); i++, left--) b[i] = data[from++];
            if (!left) break;
        }
        if (!left) break;
    }
    if (left) return "not enough room in the location: " + std::to_string(left) + " byte(s) left";
    disk.extended = true;              // RASM writes the image back in the extended format
    disk.modified = true;
    return "";
}

bool cpctLooksAscii(const std::vector<uint8_t>& data) {
    if (data.empty()) return false;
    for (uint8_t c : data) {
        if (c >= 0x80) return false;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != 0x1b && c != '\b') return false;
        if (c == 0x7f) return false;
    }
    return true;
}

void cpctTapeAddFile(std::vector<uint8_t>& cdt, const std::vector<uint8_t>& input, const std::string& name, const std::string& mode,
                     int load, int run, int pauseMs) {
    if (cdt.empty()) {
        const char head[] = "ZXTape!\032\001\000";
        cdt.assign(head, head + 10);
        cdt.push_back(0x20); put16(cdt, 2560);             // the first pause
    }
    // the file: its AMSDOS header's values, or the data as it is (cpc2cdt reads 128 bytes
    // first, so a shorter file is 128 bytes long, the rest zero)
    std::vector<uint8_t> body(0x10000 + 0x800, 0);
    int type = 2, size, fileLoad = load, fileBoot = run;
    if (hasHeader(input)) {
        type = input[0x12];
        size = input[0x40] | input[0x41] << 8;
        fileLoad = input[0x15] | input[0x16] << 8;
        fileBoot = input[0x1a] | input[0x1b] << 8;
        std::copy(input.begin() + 128, input.begin() + 128 + std::min<size_t>((size_t)size, input.size() - 128), body.begin());
    } else {
        size = (int)std::min<size_t>(input.size(), 0x10000);
        std::copy(input.begin(), input.begin() + size, body.begin());
        if (size < 128) size = 128;
        if (fileBoot < 0) fileBoot = 0x4000;
        if (fileLoad < 0) fileLoad = 0x4000;
    }
    if (mode == "cpctxt") type = 0x16;
    const int baudT = (3500000 / 3 + 2000 / 2) / 2000, baudT2 = baudT * 2;
    const int pause = pauseMs < 0 ? 10240 : pauseMs;
    uint8_t head[256] = {};
    std::memcpy(head, name.c_str(), std::min<size_t>(name.size(), 255));
    head[0x12] = (uint8_t)type;
    head[0x18] = (uint8_t)size; head[0x19] = (uint8_t)(size >> 8);
    head[0x1a] = (uint8_t)fileBoot; head[0x1b] = (uint8_t)(fileBoot >> 8);
    auto update = [&](int n, bool first, bool last, int length) {
        head[0x10] = (uint8_t)n;
        head[0x11] = last ? 0xff : 0;
        head[0x13] = (uint8_t)length; head[0x14] = (uint8_t)(length >> 8);
        head[0x15] = (uint8_t)fileLoad; head[0x16] = (uint8_t)(fileLoad >> 8);
        head[0x17] = first ? 0xff : 0;
    };
    // a TZX turbo block: the record's sync byte, then 256-byte segments each with the
    // firmware's CRC (CCITT, &FFFF, inverted, high byte first), then 4 bytes &FF
    auto record = [&](const uint8_t* t, int sync, int length, int pauseAfter) {
        cdt.push_back(0x11);
        put16(cdt, baudT2); put16(cdt, baudT); put16(cdt, baudT); put16(cdt, baudT); put16(cdt, baudT2);
        put16(cdt, 4096);
        cdt.push_back(8);
        put16(cdt, pauseAfter);
        const int bytes = 1 + ((length + 255) / 256) * 258 + 4;
        cdt.push_back((uint8_t)bytes); cdt.push_back((uint8_t)(bytes >> 8)); cdt.push_back((uint8_t)(bytes >> 16));
        cdt.push_back((uint8_t)sync);
        int p = 0;
        for (int l = length; l > 0; l -= 256) {
            cdt.insert(cdt.end(), t + p, t + p + 256);
            int crc = 0xffff;
            for (int k = 0; k < 256; k++) {
                int x = (t[p++] << 8) + 1;
                while (x & 0xff) {
                    if ((x ^ crc) & 0x8000) crc = ((crc ^ 0x0810) << 1) + 1;
                    else crc <<= 1;
                    x <<= 1;
                }
            }
            crc = ~crc;
            cdt.push_back((uint8_t)(crc >> 8));
            cdt.push_back((uint8_t)crc);
        }
        for (int k = 0; k < 4; k++) cdt.push_back(0xff);
    };
    if (size > 0x800) {
        update(1, true, false, 0x800);
        record(head, 44, 28, 16);
        record(body.data(), 22, 0x800, 2560);
        fileLoad += 0x800;
        update(2, false, true, size - 0x800);              // -t: the rest in one block
        record(head, 44, 28, 16);
        record(body.data() + 0x800, 22, size - 0x800, pause);
    } else {
        update(1, true, true, size);
        record(head, 44, 28, 16);
        record(body.data(), 22, size, pause);
    }
}

} // namespace cpcse
