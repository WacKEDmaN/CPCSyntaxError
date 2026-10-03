// CPCSyntaxError — the DSK editor's model. See dsk_edit.h.
#include "dsk_edit.h"
#include <algorithm>
#include <map>

namespace cpcse {

// ============================================================== presets
static std::vector<int> interleaveFor(const std::string& skew) {
    // a position round the track -> the sector there, as an offset from the first ID
    if (skew == "skew5_9") return { 0, 5, 1, 6, 2, 7, 3, 8, 4 };      // AMSDOS's own format order
    if (skew == "skew5_10") return { 0, 5, 1, 6, 2, 7, 3, 8, 4, 9 };
    if (skew == "skew2_9") return { 0, 2, 4, 6, 8, 1, 3, 5, 7 };
    if (skew == "skew2_10") return { 0, 2, 4, 6, 8, 1, 3, 5, 7, 9 };
    return {};
}

std::vector<DskGeometry> dskPresets() {
    std::vector<DskGeometry> out;
    for (const DiskFormat& f : listDiskFormats()) {
        DskGeometry g;
        g.id = f.id; g.name = f.name;
        g.tracks = f.tracks; g.sides = f.sides; g.sectors = f.sectors;
        g.sizeCode = 0;
        while ((128 << g.sizeCode) < f.sectorSize && g.sizeCode < 7) g.sizeCode++;
        g.firstSector = f.sectorBase; g.gap3 = f.gap3; g.filler = 0xe5;
        g.interleave = interleaveFor(f.skew);
        g.sidesAlternate = true;
        g.reservedTracks = f.reservedTracks; g.blockSize = f.blockSize;
        g.dirEntries = f.dirBlocks * f.blockSize / 32;
        out.push_back(g);
    }
    return out;
}

std::optional<DskGeometry> dskPreset(const std::string& id) {
    for (const DskGeometry& g : dskPresets()) if (g.id == id) return g;
    return std::nullopt;
}

// ============================================================== tracks and sectors
void dskFormatTrack(Disk& disk, int cylinder, int side, const DskGeometry& g) {
    if (cylinder < 0 || cylinder >= disk.tracks || side < 0 || side >= disk.sides) return;
    auto track = std::make_shared<Track>();
    track->cylinder = cylinder; track->side = side;
    track->sizeCode = g.sizeCode; track->gap3 = g.gap3; track->filler = g.filler;
    for (int pos = 0; pos < g.sectors; pos++) {
        const int offset = pos < (int)g.interleave.size() ? g.interleave[pos] : pos;
        Sector s;
        s.c = cylinder; s.h = side; s.r = (g.firstSector + offset) & 0xff; s.n = g.sizeCode;
        s.data.push_back(Bytes((size_t)g.sectorSize(), (uint8_t)g.filler));
        track->sectors.push_back(s);
    }
    disk.trackData[cylinder][side] = track;
    disk.modified = true;
}

std::shared_ptr<Disk> dskCreate(const DskGeometry& g, const std::string& creator) {
    auto disk = std::make_shared<Disk>();
    disk->creator = creator;
    disk->extended = true;
    dskResize(*disk, g.tracks, g.sides);
    for (int c = 0; c < g.tracks; c++)
        for (int s = 0; s < g.sides; s++) dskFormatTrack(*disk, c, s, g);
    disk->modified = false;
    return disk;
}

void dskResize(Disk& disk, int tracks, int sides) {
    tracks = std::clamp(tracks, 1, 255);
    sides = std::clamp(sides, 1, 2);
    disk.trackData.resize(tracks);
    for (auto& t : disk.trackData) t.resize(sides);
    disk.tracks = tracks;
    disk.sides = sides;
    disk.modified = true;
}

Sector* dskFindSector(Disk& disk, int cylinder, int side, int r) {
    if (cylinder < 0 || cylinder >= (int)disk.trackData.size() || side < 0 || side >= (int)disk.trackData[cylinder].size()) return nullptr;
    auto& track = disk.trackData[cylinder][side];
    if (!track) return nullptr;
    for (Sector& s : track->sectors) if (s.r == r) return &s;
    return nullptr;
}

std::optional<DskGeometry> dskDetect(const Disk& disk) {
    if (disk.trackData.empty() || disk.trackData[0].empty() || !disk.trackData[0][0] || disk.trackData[0][0]->sectors.empty())
        return std::nullopt;
    const auto& sectors = disk.trackData[0][0]->sectors;
    int minR = 0x100;
    for (const Sector& s : sectors) minR = std::min(minR, s.r);
    const int count = (int)sectors.size();
    const bool big = disk.tracks >= 80;
    std::string id;
    // A 40-track AMSDOS disc imaged with an extra track or two is still the 40-track format:
    // the 42-track ones (and the 80-track ones on a 40-track image) are chosen by hand.
    switch (minR) {
        case 0xc1: id = big ? "data80" : "data"; break;
        case 0x41: id = big ? "system80" : "system"; break;
        case 0x91: id = "parados"; break;
        case 0x21: id = "romdos_d2"; break;
        case 0x11: id = "romdos_d10"; break;
        case 0x31: id = "romdos_d20"; break;
        case 0x51: id = "romdos_d40"; break;
        case 0x01: id = disk.sides == 2 && big && count >= 9 ? "romdos_d1" : "ibm"; break;
        default: return std::nullopt;
    }
    return dskPreset(id);
}

// ============================================================== the filesystem
DskFs::DskFs(std::shared_ptr<Disk> d, const DskGeometry& geometry) : disk(std::move(d)), g(geometry) {}

int DskFs::totalBlocks() const {
    if (g.blocks > 0) return g.blocks;
    const long long dataBytes = (long long)(g.tracks * g.sides - g.reservedTracks) * g.sectors * g.sectorSize();
    return (int)std::max(0LL, dataBytes / std::max(1, g.blockSize));
}
int DskFs::directoryBlocks() const { return (g.dirEntries * 32 + g.blockSize - 1) / g.blockSize; }

bool DskFs::logicalSector(int index, int& cylinder, int& side, int& r) const {
    if (g.sectors <= 0) return false;
    const int logicalTrack = g.reservedTracks + index / g.sectors;
    if (g.sides == 2) {
        if (g.sidesAlternate) { cylinder = logicalTrack / 2; side = logicalTrack % 2; }
        else { cylinder = logicalTrack % g.tracks; side = logicalTrack / g.tracks; }
    } else {
        cylinder = logicalTrack; side = 0;
    }
    r = (g.firstSector + index % g.sectors) & 0xff;
    return cylinder < disk->tracks && side < disk->sides;
}

bool DskFs::blockSectors(int block, std::vector<std::array<int, 3>>& out) const {
    out.clear();
    const int ss = g.sectorSize();
    const long long first = (long long)block * g.blockSize / ss, last = ((long long)(block + 1) * g.blockSize - 1) / ss;
    bool all = true;
    for (long long i = first; i <= last; i++) {
        int c = 0, s = 0, r = 0;
        if (!logicalSector((int)i, c, s, r) || !dskFindSector(*disk, c, s, r)) all = false;
        out.push_back({ c, s, r });
    }
    return all;
}

Bytes DskFs::readBlock(int block) const {
    Bytes out((size_t)g.blockSize, 0xe5);
    const int ss = g.sectorSize();
    for (int k = 0; k < g.blockSize; ) {
        const long long byte = (long long)block * g.blockSize + k;
        const int index = (int)(byte / ss), within = (int)(byte % ss), n = std::min(ss - within, g.blockSize - k);
        int c = 0, s = 0, r = 0;
        if (logicalSector(index, c, s, r))
            if (const Sector* sec = dskFindSector(*disk, c, s, r))
                if (!sec->data.empty())
                    for (int i = 0; i < n && within + i < (int)sec->data[0].size(); i++) out[(size_t)(k + i)] = sec->data[0][(size_t)(within + i)];
        k += n;
    }
    return out;
}

bool DskFs::writeBlock(int block, const Bytes& data) {
    const int ss = g.sectorSize();
    bool ok = true;
    for (int k = 0; k < g.blockSize; ) {
        const long long byte = (long long)block * g.blockSize + k;
        const int index = (int)(byte / ss), within = (int)(byte % ss), n = std::min(ss - within, g.blockSize - k);
        int c = 0, s = 0, r = 0;
        Sector* sec = logicalSector(index, c, s, r) ? dskFindSector(*disk, c, s, r) : nullptr;
        if (!sec) ok = false;
        else {
            if (sec->data.empty()) sec->data.push_back(Bytes((size_t)ss, 0xe5));
            sec->data.resize(1);                 // a written sector reads back one way
            Bytes& d = sec->data[0];
            if ((int)d.size() < within + n) d.resize((size_t)(within + n), 0xe5);
            for (int i = 0; i < n; i++) d[(size_t)(within + i)] = k + i < (int)data.size() ? data[(size_t)(k + i)] : 0xe5;
        }
        k += n;
    }
    disk->modified = true;
    return ok;
}

Bytes DskFs::directory() const {
    Bytes dir;
    for (int b = 0; b < directoryBlocks(); b++) { Bytes blk = readBlock(b); dir.insert(dir.end(), blk.begin(), blk.end()); }
    dir.resize((size_t)g.dirEntries * 32, 0xe5);
    return dir;
}

void DskFs::writeDirectory(const Bytes& dir) {
    for (int b = 0; b < directoryBlocks(); b++) {
        Bytes blk = readBlock(b);
        for (int i = 0; i < g.blockSize; i++) {
            const size_t at = (size_t)b * g.blockSize + i;
            if (at < dir.size()) blk[(size_t)i] = dir[at];
        }
        writeBlock(b, blk);
    }
}

std::vector<int> DskFs::entryBlocks(const Bytes& dir, int entry) const {
    std::vector<int> out;
    const uint8_t* e = &dir[(size_t)entry * 32];
    if (wideBlockNumbers()) {
        for (int i = 0; i < 8; i++) { const int b = e[16 + i * 2] | e[17 + i * 2] << 8; if (b) out.push_back(b); }
    } else {
        for (int i = 0; i < 16; i++) if (e[16 + i]) out.push_back(e[16 + i]);
    }
    return out;
}

static std::string entryText(const uint8_t* p, int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += (char)(p[i] & 0x7f);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// The entry's 16K logical extents: its capacity (16 block numbers, or 8) over 16K, less one.
static int extentMask(const DskFs& fs) {
    const int capacity = (fs.wideBlockNumbers() ? 8 : 16) * fs.geometry().blockSize;
    return std::max(1, capacity / 16384) - 1;
}

std::vector<DskFsFile> DskFs::files() const {
    const Bytes dir = directory();
    const int exm = extentMask(*this);
    std::map<std::string, DskFsFile> byKey;
    std::map<std::string, std::vector<std::pair<int, int>>> order;   // (extent number, entry)
    for (int i = 0; i < g.dirEntries; i++) {
        const uint8_t* e = &dir[(size_t)i * 32];
        if (e[0] > 15) continue;                 // &E5 free; 16-31 CP/M 3 passwords, labels, stamps
        std::string key;
        key += (char)e[0];
        for (int k = 1; k < 12; k++) key += (char)(e[k] & 0x7f);
        DskFsFile& f = byKey[key];
        if (f.entries.empty() && order[key].empty()) {
            f.user = e[0];
            f.name = entryText(e + 1, 8); f.ext = entryText(e + 9, 3);
            f.readOnly = e[9] & 0x80; f.system = e[10] & 0x80; f.archived = e[11] & 0x80;
        }
        order[key].push_back({ (e[14] & 0x3f) * 32 + (e[12] & 0x1f), i });
        f.records += (e[12] & exm) * 128 + std::min<int>(e[15], 128);
    }
    std::vector<DskFsFile> out;
    for (auto& [key, f] : byKey) {
        auto& o = order[key];
        std::sort(o.begin(), o.end());
        for (auto& [ext, entry] : o) {
            f.entries.push_back(entry);
            for (int b : entryBlocks(dir, entry)) f.blocks.push_back(b);
        }
        if (!f.blocks.empty()) f.header = parseAmsdosHeader(readBlock(f.blocks[0]));
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const DskFsFile& a, const DskFsFile& b) {
        return a.user != b.user ? a.user < b.user : a.displayName() < b.displayName();
    });
    return out;
}

Bytes DskFs::read(const DskFsFile& f, bool stripHeader) const {
    Bytes out;
    for (int b : f.blocks) { Bytes blk = readBlock(b); out.insert(out.end(), blk.begin(), blk.end()); }
    out.resize((size_t)f.size(), 0x1a);
    if (stripHeader && f.header && out.size() >= 128) {
        int length = f.header->fullLength ? f.header->fullLength : f.header->logicalLength;
        length = std::clamp(length, 0, (int)out.size() - 128);
        return Bytes(out.begin() + 128, out.begin() + 128 + length);
    }
    return out;
}

std::vector<DskBlockUse> DskFs::blockMap(const std::vector<DskFsFile>& list) const {
    std::vector<DskBlockUse> map((size_t)std::max(0, totalBlocks()));
    for (int b = 0; b < directoryBlocks() && b < (int)map.size(); b++) map[(size_t)b].kind = DskBlockUse::Directory;
    for (int i = 0; i < (int)list.size(); i++)
        for (int b : list[(size_t)i].blocks) {
            if (b < 0 || b >= (int)map.size()) continue;
            DskBlockUse& u = map[(size_t)b];
            if (u.kind == DskBlockUse::Free) { u.kind = DskBlockUse::File; u.file = i; }
            else u.kind = DskBlockUse::Shared;
        }
    return map;
}

int DskFs::freeBlocks() const {
    int n = 0;
    for (const DskBlockUse& u : blockMap(files())) if (u.kind == DskBlockUse::Free) n++;
    return n;
}

int DskFs::freeEntries() const {
    const Bytes dir = directory();
    int n = 0;
    for (int i = 0; i < g.dirEntries; i++) if (dir[(size_t)i * 32] == 0xe5) n++;
    return n;
}

void dskSplitName(const std::string& in, std::string& name, std::string& ext) {
    auto clean = [](const std::string& s, size_t max) {
        std::string r;
        for (char c : s) {
            const unsigned char u = (unsigned char)std::toupper((unsigned char)c);
            if (u <= ' ' || u >= 0x7f || std::string("<>.,;:=?*[]\"/\\|").find((char)u) != std::string::npos) continue;
            if (r.size() < max) r += (char)u;
        }
        return r;
    };
    const size_t dot = in.find_last_of('.');
    name = clean(dot == std::string::npos ? in : in.substr(0, dot), 8);
    ext = dot == std::string::npos ? "" : clean(in.substr(dot + 1), 3);
}

Bytes dskAmsdosHeader(int user, const std::string& nameIn, const std::string& extIn, int type, int load, int exec, int length) {
    std::string name, ext;
    dskSplitName(nameIn + (extIn.empty() ? "" : "." + extIn), name, ext);
    Bytes h(128, 0);
    h[0] = (uint8_t)(user & 15);
    for (int k = 0; k < 8; k++) h[1 + k] = (uint8_t)(k < (int)name.size() ? name[(size_t)k] : ' ');
    for (int k = 0; k < 3; k++) h[9 + k] = (uint8_t)(k < (int)ext.size() ? ext[(size_t)k] : ' ');
    h[18] = (uint8_t)type;
    h[21] = (uint8_t)(load & 0xff); h[22] = (uint8_t)(load >> 8 & 0xff);
    h[24] = (uint8_t)(length & 0xff); h[25] = (uint8_t)(length >> 8 & 0xff);
    h[26] = (uint8_t)(exec & 0xff); h[27] = (uint8_t)(exec >> 8 & 0xff);
    h[64] = (uint8_t)(length & 0xff); h[65] = (uint8_t)(length >> 8 & 0xff); h[66] = (uint8_t)(length >> 16 & 0xff);
    int sum = 0;
    for (int i = 0; i < 67; i++) sum += h[(size_t)i];
    h[67] = (uint8_t)(sum & 0xff); h[68] = (uint8_t)(sum >> 8 & 0xff);
    return h;
}

void DskFs::remove(const DskFsFile& f) {
    Bytes dir = directory();
    for (int e : f.entries) dir[(size_t)e * 32] = 0xe5;
    writeDirectory(dir);
}

std::string DskFs::write(int user, const std::string& nameIn, const std::string& extIn, const Bytes& data) {
    std::string name, ext;
    dskSplitName(nameIn + (extIn.empty() ? "" : "." + extIn), name, ext);
    if (name.empty()) return "no file name";
    user &= 15;
    const std::vector<DskFsFile> list = files();
    const DskFsFile* old = nullptr;
    for (const DskFsFile& f : list) if (f.user == user && f.name == name && f.ext == ext) old = &f;
    const int perEntry = wideBlockNumbers() ? 8 : 16;
    const int blocksNeeded = (int)((data.size() + g.blockSize - 1) / g.blockSize);
    const int entriesNeeded = std::max(1, (blocksNeeded + perEntry - 1) / perEntry);
    const int freeB = freeBlocks() + (old ? (int)old->blocks.size() : 0);
    const int freeE = freeEntries() + (old ? (int)old->entries.size() : 0);
    if (blocksNeeded > freeB) return "the disc is full (" + std::to_string(blocksNeeded) + " blocks needed, " + std::to_string(freeB) + " free)";
    if (entriesNeeded > freeE) return "the directory is full";
    if (old) remove(*old);

    // the blocks, lowest free first
    std::vector<int> blocks;
    const auto map = blockMap(files());
    for (int b = 0; b < (int)map.size() && (int)blocks.size() < blocksNeeded; b++)
        if (map[(size_t)b].kind == DskBlockUse::Free) blocks.push_back(b);
    for (int i = 0; i < blocksNeeded; i++) {
        Bytes blk((size_t)g.blockSize, 0xe5);
        for (int k = 0; k < g.blockSize; k++) {
            const size_t at = (size_t)i * g.blockSize + k;
            if (at < data.size()) blk[(size_t)k] = data[at];
            else if (at < (data.size() + 127) / 128 * 128) blk[(size_t)k] = 0x1a;   // CP/M: ^Z to the record's end
        }
        writeBlock(blocks[(size_t)i], blk);
    }
    // the entries
    Bytes dir = directory();
    const int capacity = perEntry * g.blockSize;
    int slot = 0;
    for (int j = 0; j < entriesNeeded; j++) {
        while (slot < g.dirEntries && dir[(size_t)slot * 32] != 0xe5) slot++;
        uint8_t* e = &dir[(size_t)slot * 32];
        std::fill(e, e + 32, 0);
        e[0] = (uint8_t)user;
        for (int k = 0; k < 8; k++) e[1 + k] = (uint8_t)(k < (int)name.size() ? name[(size_t)k] : ' ');
        for (int k = 0; k < 3; k++) e[9 + k] = (uint8_t)(k < (int)ext.size() ? ext[(size_t)k] : ' ');
        const long long start = (long long)j * capacity;
        const long long bytes = std::max(0LL, std::min<long long>(capacity, (long long)data.size() - start));
        const int records = (int)((bytes + 127) / 128);
        const int firstLogical = (int)(start / 16384);
        const int lastLogical = firstLogical + std::max(0, records - 1) / 128;
        e[12] = (uint8_t)(lastLogical & 0x1f);
        e[14] = (uint8_t)(lastLogical >> 5);
        e[15] = (uint8_t)(records - (lastLogical - firstLogical) * 128);
        for (int k = 0; k < perEntry; k++) {
            const int bi = j * perEntry + k;
            if (bi >= blocksNeeded) break;
            const int b = blocks[(size_t)bi];
            if (wideBlockNumbers()) { e[16 + k * 2] = (uint8_t)(b & 0xff); e[17 + k * 2] = (uint8_t)(b >> 8); }
            else e[16 + k] = (uint8_t)b;
        }
        slot++;
    }
    writeDirectory(dir);
    return "";
}

std::string DskFs::rename(const DskFsFile& f, int user, const std::string& nameIn, const std::string& extIn) {
    std::string name, ext;
    dskSplitName(nameIn + (extIn.empty() ? "" : "." + extIn), name, ext);
    if (name.empty()) return "no file name";
    user &= 15;
    for (const DskFsFile& o : files())
        if (o.user == user && o.name == name && o.ext == ext && o.entries != f.entries) return o.displayName() + " is already there";
    Bytes dir = directory();
    for (int i : f.entries) {
        uint8_t* e = &dir[(size_t)i * 32];
        e[0] = (uint8_t)user;
        for (int k = 0; k < 8; k++) e[1 + k] = (uint8_t)(k < (int)name.size() ? name[(size_t)k] : ' ');
        for (int k = 0; k < 3; k++) e[9 + k] = (uint8_t)((e[9 + k] & 0x80) | (k < (int)ext.size() ? (uint8_t)ext[(size_t)k] : (uint8_t)' '));
    }
    writeDirectory(dir);
    return "";
}

void DskFs::setAttributes(const DskFsFile& f, bool readOnly, bool system, bool archived) {
    Bytes dir = directory();
    for (int i : f.entries) {
        uint8_t* e = &dir[(size_t)i * 32];
        e[9] = (uint8_t)((e[9] & 0x7f) | (readOnly ? 0x80 : 0));
        e[10] = (uint8_t)((e[10] & 0x7f) | (system ? 0x80 : 0));
        e[11] = (uint8_t)((e[11] & 0x7f) | (archived ? 0x80 : 0));
    }
    writeDirectory(dir);
}

} // namespace cpcse
