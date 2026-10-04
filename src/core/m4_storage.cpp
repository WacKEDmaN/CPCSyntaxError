// CPCSyntaxError — the M4 Board's microSD card: a host folder. See m4_storage.h.
#include "m4_storage.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>

namespace cpcse {

namespace fs = std::filesystem;

std::string m4ErrorText(int fr) {
    switch (fr) {
        case M4_FR_OK: return "OK";
        case M4_FR_DISK_ERR: return "Disk error";
        case M4_FR_NOT_READY: return "SD card not ready";
        case M4_FR_NO_FILE: return "File not found";
        case M4_FR_NO_PATH: return "Path not found";
        case M4_FR_INVALID_NAME: return "Invalid name";
        case M4_FR_DENIED: return "Access denied";
        case M4_FR_EXIST: return "File exists";
        case M4_FR_INVALID_OBJECT: return "Invalid file";
        case M4_FR_WRITE_PROTECTED: return "Write protected";
        case M4_FR_TOO_MANY_OPEN_FILES: return "Too many open files";
        case M4_FR_INVALID_PARAMETER: return "Invalid parameter";
        default: return "Error " + std::to_string(fr);
    }
}

// ============================================================== paths
std::string M4Storage::lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string M4Storage::parentOf(const std::string& path) {
    const size_t at = path.rfind('/');
    return (at == std::string::npos || at == 0) ? "/" : path.substr(0, at);
}
std::string M4Storage::leafOf(const std::string& path) {
    if (path == "/") return "";
    const size_t at = path.rfind('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}
// FatFs pattern matching: '*' any run, '?' one character, without regard to case.
bool M4Storage::wildcardMatch(const std::string& pattern, const std::string& name) {
    if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
    const std::string p = lower(pattern), n = lower(name);
    std::function<bool(size_t, size_t)> match = [&](size_t i, size_t j) -> bool {
        while (i < p.size()) {
            if (p[i] == '*') {
                while (i < p.size() && p[i] == '*') i++;
                if (i == p.size()) return true;
                for (size_t k = j; k <= n.size(); k++) if (match(i, k)) return true;
                return false;
            }
            if (j >= n.size()) return false;
            if (p[i] != '?' && p[i] != n[j]) return false;
            i++; j++;
        }
        return j == n.size();
    };
    return match(0, 0);
}

M4Storage::M4Storage(std::string hostFolder) : root(std::move(hostFolder)) {}

std::string M4Storage::absolute(const std::string& nameIn) const {
    std::string name = nameIn;
    for (char& c : name) if (c == '\\') c = '/';
    while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == '\n')) name.pop_back();
    size_t s = 0;
    while (s < name.size() && name[s] == ' ') s++;
    name = name.substr(s);
    if (name.size() >= 2 && std::isalpha((unsigned char)name[0]) && name[1] == ':') name = name.substr(2);
    std::vector<std::string> parts;
    if (name.empty() || name[0] != '/') {
        std::string c = cwd;
        size_t a = 0;
        while (a < c.size()) {
            size_t b = c.find('/', a);
            if (b == std::string::npos) b = c.size();
            if (b > a) parts.push_back(c.substr(a, b - a));
            a = b + 1;
        }
    }
    size_t a = 0;
    while (a <= name.size()) {
        size_t b = name.find('/', a);
        if (b == std::string::npos) b = name.size();
        std::string part = name.substr(a, b - a);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        a = b + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

fs::path M4Storage::host(const std::string& realPath) const {
    fs::path p(root);
    if (realPath.size() > 1) p /= fs::path(realPath.substr(1));
    return p;
}

std::string M4Storage::find(const std::string& path) const {
    const std::string abs = absolute(path);
    if (abs == "/") return "/";
    std::error_code ec;
    std::string real;
    size_t a = 1;
    while (a <= abs.size()) {
        size_t b = abs.find('/', a);
        if (b == std::string::npos) b = abs.size();
        const std::string want = lower(abs.substr(a, b - a));
        const fs::path dir = host(real.empty() ? "/" : real);
        std::string hit;
        if (fs::is_directory(dir, ec))
            for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
                const std::string n = it->path().filename().string();
                if (!n.empty() && n[0] != '.' && lower(n) == want) { hit = n; break; }
            }
        if (hit.empty()) return "";
        real += "/" + hit;
        a = b + 1;
    }
    return real;
}

bool M4Storage::isDir(const std::string& realPath) const {
    std::error_code ec;
    return !realPath.empty() && fs::is_directory(host(realPath), ec);
}
bool M4Storage::isFile(const std::string& realPath) const {
    std::error_code ec;
    return !realPath.empty() && realPath != "/" && fs::is_regular_file(host(realPath), ec);
}

static void fatDateTime(const fs::path& p, int& date, int& time) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    std::time_t t = std::time(nullptr);
    if (!ec) {
        const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        t = std::chrono::system_clock::to_time_t(sys);
    }
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    int year = tmv.tm_year + 1900;
    if (year < 1980) year = 1980;
    date = ((year - 1980) << 9 | (tmv.tm_mon + 1) << 5 | tmv.tm_mday) & 0xffff;
    time = (tmv.tm_hour << 11 | tmv.tm_min << 5 | tmv.tm_sec / 2) & 0xffff;
}

bool M4Storage::stat(const std::string& realPath, M4Entry& out) const {
    if (realPath.empty()) return false;
    std::error_code ec;
    const fs::path p = host(realPath);
    auto st = fs::status(p, ec);
    if (ec || !fs::exists(st)) return false;
    out.name = realPath == "/" ? "" : leafOf(realPath);
    out.dir = fs::is_directory(st);
    out.size = out.dir ? 0 : (uint64_t)fs::file_size(p, ec);
    out.readOnly = (st.permissions() & fs::perms::owner_write) == fs::perms::none;
    fatDateTime(p, out.fatDate, out.fatTime);
    return true;
}

std::vector<M4Entry> M4Storage::list(const std::string& realDir) const {
    std::vector<M4Entry> rows;
    std::error_code ec;
    const fs::path dir = host(realDir);
    if (!fs::is_directory(dir, ec)) return rows;
    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.empty() || n[0] == '.') continue;
        M4Entry e;
        if (stat((realDir == "/" ? "" : realDir) + "/" + n, e)) rows.push_back(e);
    }
    std::sort(rows.begin(), rows.end(), [](const M4Entry& a, const M4Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return lower(a.name) < lower(b.name);
    });
    return rows;
}

static bool validLongName(const std::string& leaf) {
    if (leaf.empty() || leaf == "." || leaf == "..") return false;
    for (unsigned char c : leaf) if (c < 32 || std::strchr("\\/:*?\"<>|", c)) return false;
    // Windows drops a trailing dot or space (so the file is not the one named) ...
    if (leaf.back() == '.' || leaf.back() == ' ') return false;
    // ... and CON, PRN, AUX, NUL, COM1-9 and LPT1-9 -- with any extension -- are devices:
    // a CPC program must not reach a serial port or a printer through the M4's folder.
    std::string base = leaf.substr(0, leaf.find('.'));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (char& c : base) c = (char)std::toupper((unsigned char)c);
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") return false;
    if (base.size() == 4 && (base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '0' && base[3] <= '9')
        return false;
    return true;
}

int M4Storage::resolveForCreate(const std::string& path, std::string& realPath) const {
    const std::string abs = absolute(path);
    const std::string existing = find(abs);
    if (!existing.empty()) { realPath = existing; return M4_FR_OK; }
    const std::string parent = find(parentOf(abs));
    if (parent.empty() || !isDir(parent)) return M4_FR_NO_PATH;
    const std::string leaf = leafOf(abs);
    if (!validLongName(leaf)) return M4_FR_INVALID_NAME;
    realPath = (parent == "/" ? "" : parent) + "/" + leaf;
    return M4_FR_OK;
}

int M4Storage::readFile(const std::string& realPath, Bytes& out) const {
    if (!isFile(realPath)) return M4_FR_NO_FILE;
    std::ifstream f(host(realPath), std::ios::binary);
    if (!f) return M4_FR_DISK_ERR;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return M4_FR_OK;
}

int M4Storage::writeFile(const std::string& realPath, const Bytes& data) {
    if (readOnly) return M4_FR_WRITE_PROTECTED;
    if (isDir(realPath)) return M4_FR_DENIED;
    std::ofstream f(host(realPath), std::ios::binary | std::ios::trunc);
    if (!f) return M4_FR_DENIED;
    f.write((const char*)data.data(), (std::streamsize)data.size());
    revision += 1;
    return f ? M4_FR_OK : M4_FR_DISK_ERR;
}

int M4Storage::remove(const std::string& realPath) {
    if (readOnly) return M4_FR_WRITE_PROTECTED;
    if (realPath.empty() || realPath == "/") return M4_FR_NO_FILE;
    M4Entry e;
    if (!stat(realPath, e)) return M4_FR_NO_FILE;
    if (e.readOnly) return M4_FR_DENIED;
    if (e.dir && !list(realPath).empty()) return M4_FR_DENIED;   // FatFs: a directory must be empty
    std::error_code ec;
    fs::remove(host(realPath), ec);
    if (ec) return M4_FR_DENIED;
    revision += 1;
    return M4_FR_OK;
}

int M4Storage::rename(const std::string& fromReal, const std::string& toPath) {
    if (readOnly) return M4_FR_WRITE_PROTECTED;
    if (fromReal.empty() || fromReal == "/") return M4_FR_NO_FILE;
    const std::string abs = absolute(toPath);
    const std::string existing = find(abs);
    if (!existing.empty() && lower(existing) != lower(fromReal)) return M4_FR_EXIST;
    const std::string parent = find(parentOf(abs));
    if (parent.empty() || !isDir(parent)) return M4_FR_NO_PATH;
    const std::string leaf = leafOf(abs);
    if (!validLongName(leaf)) return M4_FR_INVALID_NAME;
    const std::string to = (parent == "/" ? "" : parent) + "/" + leaf;
    std::error_code ec;
    fs::rename(host(fromReal), host(to), ec);
    if (ec) return M4_FR_DENIED;
    revision += 1;
    return M4_FR_OK;
}

int M4Storage::makeDir(const std::string& path) {
    if (readOnly) return M4_FR_WRITE_PROTECTED;
    const std::string abs = absolute(path);
    if (!find(abs).empty()) return M4_FR_EXIST;
    std::string real;
    const int r = resolveForCreate(abs, real);
    if (r != M4_FR_OK) return r;
    std::error_code ec;
    fs::create_directory(host(real), ec);
    if (ec) return M4_FR_DENIED;
    revision += 1;
    return M4_FR_OK;
}

uint64_t M4Storage::usedBytes() const {
    uint64_t used = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) used += it->file_size(ec);
    return used;
}

// ============================================================== FAT16 volume
static void put16(uint8_t* p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int get16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t fnv(const uint8_t* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ull; }
    return h;
}

// Microsoft FAT specification, "Short name generation": the basis name, and whether the
// long name is needed beside it.
static bool validShortChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80 || std::strchr("$%'-_@~`!(){}^#&", c);
}
struct ShortName { std::string e11; bool needsLong = false; };
static ShortName makeShortName(const std::string& name, std::set<std::string>& used) {
    bool lossy = false;
    std::string up;
    for (unsigned char c : name) {
        const unsigned char u = (unsigned char)std::toupper(c);
        if (c == ' ') { lossy = true; continue; }
        if (c == '.' || validShortChar(u)) up += (char)u;
        else { up += '_'; lossy = true; }
    }
    size_t lead = 0;
    while (lead < up.size() && up[lead] == '.') lead++;
    if (lead) { up = up.substr(lead); lossy = true; }
    const size_t dot = up.rfind('.');
    std::string base = dot == std::string::npos ? up : up.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : up.substr(dot + 1);
    std::string cleanBase;
    for (char c : base) { if (c == '.') lossy = true; else cleanBase += c; }
    base = cleanBase;
    if (base.empty()) { base = "_"; lossy = true; }
    if (base.size() > 8) { base = base.substr(0, 8); lossy = true; }
    if (ext.size() > 3) { ext = ext.substr(0, 3); lossy = true; }
    const std::string asShort = base + (ext.empty() ? "" : "." + ext);
    ShortName out;
    out.needsLong = lossy || asShort != name;
    auto pack = [](const std::string& b, const std::string& e) {
        std::string s = b; s.resize(8, ' ');
        std::string x = e; x.resize(3, ' ');
        return s + x;
    };
    std::string candidate = pack(base, ext);
    if (lossy || used.count(candidate)) {
        for (int n = 1; n < 1000000; n++) {
            const std::string tail = "~" + std::to_string(n);
            candidate = pack(base.substr(0, std::min(base.size(), 8 - tail.size())) + tail, ext);
            if (!used.count(candidate)) break;
        }
    }
    used.insert(candidate);
    out.e11 = candidate;
    return out;
}
std::vector<std::string> M4Storage::shortNames(const std::vector<M4Entry>& rows) {
    std::set<std::string> used;
    std::vector<std::string> out;
    for (const M4Entry& e : rows) out.push_back(makeShortName(e.name, used).e11);
    return out;
}
static uint8_t lfnChecksum(const std::string& e11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)e11[i]);
    return sum;
}
static const int LFN_POS[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

namespace {
struct Node {
    std::string realPath, name;
    bool dir = false;
    uint64_t size = 0;
    int date = 0, time = 0;
    bool readOnly = false;
    Bytes data;
    std::vector<Node*> children;
    Node* parent = nullptr;
    ShortName shortName;
    uint32_t cluster = 0, clusterCount = 0;
    int dirEntries = 0;
};
}

bool M4SdCard::build() {
    built = false;
    clusters.clear();
    snapshot.clear();
    lastError.clear();
    std::vector<std::unique_ptr<Node>> nodes;
    std::function<Node*(const std::string&, const M4Entry&)> walk = [&](const std::string& real, const M4Entry& e) {
        nodes.push_back(std::make_unique<Node>());
        Node* n = nodes.back().get();
        n->realPath = real; n->name = e.name; n->dir = e.dir; n->size = e.size;
        n->date = e.fatDate; n->time = e.fatTime; n->readOnly = e.readOnly;
        if (e.dir) {
            for (const M4Entry& c : storage.list(real)) { Node* child = walk((real == "/" ? "" : real) + "/" + c.name, c); child->parent = n; n->children.push_back(child); }
            std::set<std::string> used;
            n->dirEntries = real == "/" ? 1 : 2;              // the volume label, or . and ..
            for (Node* c : n->children) {
                c->shortName = makeShortName(c->name, used);
                n->dirEntries += 1 + (c->shortName.needsLong ? (int)((c->name.size() + 12) / 13) : 0);
            }
        } else {
            storage.readFile(real, n->data);
            n->size = n->data.size();
        }
        return n;
    };
    M4Entry rootEntry; rootEntry.dir = true;
    storage.stat("/", rootEntry);
    rootEntry.dir = true;
    Node* rootNode = walk("/", rootEntry);
    if (rootNode->dirEntries > (int)rootEntries) { lastError = "more than 511 entries in the card's root"; return false; }

    // The cluster size: the smallest that leaves 64 MB free in FAT16's 65524 clusters.
    const uint32_t maxClusters = 65524;
    uint32_t chosen = 0;
    for (uint32_t s : { 8u, 16u, 32u, 64u }) {
        const uint64_t cb = (uint64_t)s * 512;
        uint64_t need = 0;
        for (auto& n : nodes) {
            if (n.get() == rootNode) continue;
            need += n->dir ? std::max<uint64_t>(1, ((uint64_t)n->dirEntries * 32 + cb - 1) / cb) : (n->size + cb - 1) / cb;
        }
        if (need + (64ull << 20) / cb <= maxClusters || s == 64) { chosen = s; if (need > maxClusters) { lastError = "the folder is bigger than a 2 GB card"; return false; } break; }
    }
    spc = chosen;
    clusterBytes = spc * 512;
    clusterCount = maxClusters;
    fatSectors = ((clusterCount + 2) * 2 + 511) / 512;
    const uint32_t rootSectors = rootEntries * 32 / 512;
    fatStart = partStart + 1;
    rootStart = fatStart + 2 * fatSectors;
    dataStart = rootStart + rootSectors;
    total = dataStart + clusterCount * spc;
    meta.assign((size_t)dataStart * 512, 0);

    // MBR (one FAT16 partition, 1 MB aligned) and the boot sector.
    uint8_t* mbr = meta.data();
    uint8_t* pe = mbr + 0x1be;
    pe[4] = 0x06;
    put32(pe + 8, partStart);
    put32(pe + 12, total - partStart);
    mbr[510] = 0x55; mbr[511] = 0xaa;
    uint8_t* bs = &meta[(size_t)partStart * 512];
    bs[0] = 0xeb; bs[1] = 0x3c; bs[2] = 0x90;
    std::memcpy(bs + 3, "MSWIN4.1", 8);
    put16(bs + 11, 512); bs[13] = (uint8_t)spc; put16(bs + 14, 1); bs[16] = 2; put16(bs + 17, (int)rootEntries);
    const uint32_t partSectors = total - partStart;
    put16(bs + 19, partSectors < 0x10000 ? (int)partSectors : 0);
    bs[21] = 0xf8; put16(bs + 22, (int)fatSectors); put16(bs + 24, 63); put16(bs + 26, 255);
    put32(bs + 28, partStart); put32(bs + 32, partSectors < 0x10000 ? 0 : partSectors);
    bs[36] = 0x80; bs[38] = 0x29; put32(bs + 39, 0x19840416u);
    std::memcpy(bs + 43, "M4 BOARD   ", 11); std::memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xaa;
    auto setFat = [&](uint32_t c, int v) {
        for (int f = 0; f < 2; f++) put16(&meta[(size_t)(fatStart + f * fatSectors) * 512 + c * 2], v);
    };
    setFat(0, 0xfff8); setFat(1, 0xffff);

    // Clusters, depth first: a directory, then what it holds.
    uint32_t next = 2;
    std::function<void(Node*)> allocate = [&](Node* n) {
        if (n != rootNode) {
            const uint64_t bytes = n->dir ? (uint64_t)n->dirEntries * 32 : n->size;
            n->clusterCount = (uint32_t)(n->dir ? std::max<uint64_t>(1, (bytes + clusterBytes - 1) / clusterBytes)
                                                : (bytes + clusterBytes - 1) / clusterBytes);
            if (n->clusterCount) {
                n->cluster = next;
                for (uint32_t i = 0; i < n->clusterCount; i++) setFat(next + i, i + 1 == n->clusterCount ? 0xffff : (int)(next + i + 1));
                next += n->clusterCount;
            }
            if (!n->dir)
                for (uint32_t i = 0; i < n->clusterCount; i++) {
                    std::vector<uint8_t>& c = clusters[n->cluster + i];
                    c.assign(clusterBytes, 0);
                    const size_t at = (size_t)i * clusterBytes;
                    std::memcpy(c.data(), n->data.data() + at, std::min<size_t>(clusterBytes, n->data.size() - at));
                }
        }
        for (Node* c : n->children) allocate(c);
    };
    allocate(rootNode);

    auto shortEntry = [&](uint8_t* e, const std::string& e11, int attr, uint32_t cluster, uint32_t size, int date, int time) {
        std::memcpy(e, e11.data(), 11);
        e[11] = (uint8_t)attr;
        put16(e + 14, time); put16(e + 16, date); put16(e + 18, date);
        put16(e + 22, time); put16(e + 24, date);
        put16(e + 26, (int)cluster); put32(e + 28, size);
    };
    std::function<void(Node*)> writeDir = [&](Node* n) {
        if (!n->dir) return;
        std::vector<uint8_t> bytes((size_t)std::max(1, n->dirEntries) * 32 + 32, 0);
        int at = 0;
        if (n == rootNode) {
            shortEntry(&bytes[0], "M4 BOARD   ", 0x08, 0, 0, n->date, n->time);
            at = 1;
        } else {
            Node* parent = n->parent;
            shortEntry(&bytes[0], ".          ", 0x10, n->cluster, 0, n->date, n->time);
            shortEntry(&bytes[32], "..         ", 0x10, parent == rootNode || !parent ? 0 : parent->cluster, 0, n->date, n->time);
            at = 2;
        }
        for (Node* c : n->children) {
            if (c->shortName.needsLong) {
                const int count = (int)((c->name.size() + 12) / 13);
                const uint8_t sum = lfnChecksum(c->shortName.e11);
                for (int i = count; i >= 1; i--) {
                    uint8_t* e = &bytes[(size_t)at * 32];
                    e[0] = (uint8_t)(i | (i == count ? 0x40 : 0));
                    e[11] = 0x0f; e[13] = sum;
                    for (int k = 0; k < 13; k++) {
                        const size_t pos = (size_t)(i - 1) * 13 + k;
                        const int ch = pos < c->name.size() ? (unsigned char)c->name[pos] : pos == c->name.size() ? 0 : 0xffff;
                        put16(e + LFN_POS[k], ch);
                    }
                    at++;
                }
            }
            shortEntry(&bytes[(size_t)at * 32], c->shortName.e11, (c->dir ? 0x10 : 0x20) | (c->readOnly ? 0x01 : 0),
                       c->cluster, c->dir ? 0 : (uint32_t)c->size, c->date, c->time);
            at++;
        }
        if (n == rootNode) {
            std::memcpy(&meta[(size_t)rootStart * 512], bytes.data(), std::min<size_t>(bytes.size(), (size_t)rootEntries * 32));
        } else {
            for (uint32_t i = 0; i < n->clusterCount; i++) {
                std::vector<uint8_t>& c = clusters[n->cluster + i];
                c.assign(clusterBytes, 0);
                const size_t off = (size_t)i * clusterBytes;
                if (off < bytes.size()) std::memcpy(c.data(), bytes.data() + off, std::min<size_t>(clusterBytes, bytes.size() - off));
            }
        }
        for (Node* c : n->children) writeDir(c);
    };
    writeDir(rootNode);

    for (auto& n : nodes) {
        if (n.get() == rootNode) continue;
        Known k; k.dir = n->dir; k.size = n->size; k.realPath = n->realPath;
        k.hash = n->dir ? 0 : fnv(n->data.data(), n->data.size());
        snapshot[M4Storage::lower(n->realPath)] = k;
    }
    builtRevision = storage.revision;
    dirtySectors = false;
    built = true;
    return true;
}

bool M4SdCard::ensureBuilt() {
    if (built && builtRevision == storage.revision) return true;
    if (built && dirtySectors) syncToHost();
    if (built && builtRevision == storage.revision) return true;
    return build();
}

uint8_t* M4SdCard::sector(uint32_t lba, bool forWrite) {
    if (lba < dataStart) return &meta[(size_t)lba * 512];
    const uint32_t rel = lba - dataStart;
    const uint32_t c = rel / spc + 2, off = (rel % spc) * 512;
    auto it = clusters.find(c);
    if (it == clusters.end()) {
        if (!forWrite) return nullptr;
        it = clusters.emplace(c, std::vector<uint8_t>(clusterBytes, 0)).first;
    }
    return it->second.data() + off;
}

int M4SdCard::read(uint32_t lba, int count, uint8_t* out) {
    if (!ensureBuilt()) return 3;
    for (int i = 0; i < count; i++) {
        if (lba >= total || (uint32_t)i >= total - lba) return 1;   // never lba + i: it wraps
        const uint8_t* s = sector(lba + i, false);
        if (s) std::memcpy(out + i * 512, s, 512); else std::memset(out + i * 512, 0, 512);
    }
    return 0;
}

int M4SdCard::write(uint32_t lba, int count, const uint8_t* in) {
    if (storage.readOnly) return 2;
    if (!ensureBuilt()) return 3;
    for (int i = 0; i < count; i++) {
        if (lba >= total || (uint32_t)i >= total - lba) return 1;   // never lba + i: it wraps
        std::memcpy(sector(lba + i, true), in + i * 512, 512);
    }
    dirtySectors = true;
    return 0;
}

// Reads the volume back as FAT16 -- by its own boot sector, as whatever wrote it may have
// laid it out -- and applies what changed since the card was last in step with the folder.
bool M4SdCard::syncToHost(std::string* report) {
    if (!built) return true;
    auto rd = [&](uint32_t lba) -> const uint8_t* {
        static const uint8_t zero[512] = {};
        if (lba >= total) return zero;
        const uint8_t* s = sector(lba, false);
        return s ? s : zero;
    };
    const uint8_t* mbr = rd(0);
    uint32_t part = (mbr[510] == 0x55 && mbr[511] == 0xaa && mbr[0x1be + 4]) ? get32(mbr + 0x1be + 8) : 0;
    const uint8_t* bs = rd(part);
    if (bs[510] != 0x55 || bs[511] != 0xaa || get16(bs + 11) != 512 || !bs[13] || !bs[16] || !get16(bs + 22)) {
        lastError = "the card is not FAT16 any more"; return false;
    }
    const uint32_t sPc = bs[13], fStart = part + get16(bs + 14), nFats = bs[16], rootN = get16(bs + 17), fSz = get16(bs + 22);
    const uint32_t rStart = fStart + nFats * fSz, dStart = rStart + (rootN * 32 + 511) / 512;
    auto fatEntry = [&](uint32_t c) { return get16(rd(fStart + c * 2 / 512) + (c * 2) % 512); };
    auto chain = [&](uint32_t first, std::vector<uint32_t>& out) {
        std::set<uint32_t> seen;
        for (uint32_t c = first; c >= 2 && c < 0xfff8 && !seen.count(c) && out.size() < 70000; c = fatEntry(c)) { seen.insert(c); out.push_back(c); }
    };
    struct Parsed { bool dir; std::string path; Bytes data; };
    std::map<std::string, Parsed> now;
    // What the CPC wrote is not to be trusted: a directory's clusters may lead back to
    // itself or a parent, and cross-linked files may each claim the whole card.
    std::set<uint32_t> dirClustersSeen;
    int depth = 0;
    uint64_t bytesTaken = 0;
    const uint64_t cardBytes = (uint64_t)total * 512;
    std::function<bool(const std::string&, const std::vector<uint32_t>&, bool)> readDir =
        [&](const std::string& path, const std::vector<uint32_t>& lbas, bool isRoot) -> bool {
        std::string lfn; int lfnSum = -1, lfnNext = 0; bool lfnValid = false;
        for (uint32_t lba : lbas) {
            const uint8_t* s = rd(lba);
            for (int at = 0; at < 512; at += 32) {
                const uint8_t* e = s + at;
                if (e[0] == 0x00) return true;
                if (e[0] == 0xe5) { lfnValid = false; continue; }
                if (e[11] == 0x0f) {
                    if (e[0] & 0x40) { lfnNext = e[0] & 0x1f; lfnSum = e[13]; lfn.assign((size_t)lfnNext * 13, '\0'); lfnValid = true; }
                    // Sequence numbers run from 1: a 0 here would write before the name.
                    if (!lfnValid || lfnNext < 1 || (e[0] & 0x1f) != lfnNext || e[13] != lfnSum) { lfnValid = false; continue; }
                    for (int k = 0; k < 13; k++) {
                        const int ch = get16(e + LFN_POS[k]);
                        lfn[(size_t)(lfnNext - 1) * 13 + k] = ch == 0xffff ? '\0' : (char)(ch & 0xff);
                    }
                    lfnNext -= 1;
                    continue;
                }
                if (e[11] & 0x08) { lfnValid = false; continue; }                 // volume label
                std::string base((const char*)e, 8), ext((const char*)e + 8, 3);
                if ((uint8_t)base[0] == 0x05) base[0] = (char)0xe5;
                while (!base.empty() && base.back() == ' ') base.pop_back();
                while (!ext.empty() && ext.back() == ' ') ext.pop_back();
                if (e[12] & 0x08) base = M4Storage::lower(base);                   // NT: base in lower case
                if (e[12] & 0x10) ext = M4Storage::lower(ext);
                std::string name = ext.empty() ? base : base + "." + ext;
                if (lfnValid && lfnNext == 0 && lfnChecksum(std::string((const char*)e, 11)) == lfnSum) {
                    name = lfn.substr(0, lfn.find('\0'));
                }
                lfnValid = false;
                if (name == "." || name == ".." || name.empty()) continue;
                const std::string full = (path == "/" ? "" : path) + "/" + name;
                const uint32_t first = get16(e + 26);
                std::vector<uint32_t> cl;
                chain(first, cl);
                if (e[11] & 0x10) {
                    for (uint32_t c : cl)
                        if (!dirClustersSeen.insert(c).second) { lastError = full + ": a directory loops back on itself"; return false; }
                    if (depth >= 32) { lastError = full + ": directories nest too deeply"; return false; }
                    now[M4Storage::lower(full)] = { true, full, {} };
                    std::vector<uint32_t> sub;
                    for (uint32_t c : cl) for (uint32_t k = 0; k < sPc; k++) sub.push_back(dStart + (c - 2) * sPc + k);
                    depth += 1;
                    const bool ok = readDir(full, sub, false);
                    depth -= 1;
                    if (!ok) return false;
                } else {
                    const uint32_t size = get32(e + 28);
                    if ((uint64_t)cl.size() * sPc * 512 < size) { lastError = full + ": its clusters end before its size"; return false; }
                    bytesTaken += size;
                    if (bytesTaken > cardBytes) { lastError = full + ": the files hold more than the card"; return false; }
                    Bytes data(size);
                    for (uint32_t i = 0, got = 0; got < size; i++) {
                        for (uint32_t k = 0; k < sPc && got < size; k++) {
                            const uint32_t take = std::min<uint32_t>(512, size - got);
                            std::memcpy(data.data() + got, rd(dStart + (cl[i] - 2) * sPc + k), take);
                            got += take;
                        }
                    }
                    now[M4Storage::lower(full)] = { false, full, std::move(data) };
                }
            }
        }
        (void)isRoot;
        return true;
    };
    std::vector<uint32_t> rootLbas;
    for (uint32_t i = 0; i < (rootN * 32 + 511) / 512; i++) rootLbas.push_back(rStart + i);
    if (!readDir("/", rootLbas, true)) return false;

    // Apply: directories first (shallow to deep), then files, then what went away.
    std::vector<std::string> lines;
    for (auto& kv : now) {
        if (!kv.second.dir) continue;
        auto was = snapshot.find(kv.first);
        if (was == snapshot.end()) {
            if (storage.makeDir(kv.second.path) == M4_FR_OK) lines.push_back("mkdir " + kv.second.path);
        } else if (was->second.realPath != kv.second.path && storage.find(kv.second.path) == was->second.realPath) {
            if (storage.rename(was->second.realPath, kv.second.path) == M4_FR_OK) lines.push_back("rename " + kv.second.path);
        }
    }
    for (auto& kv : now) {
        if (kv.second.dir) continue;
        const Parsed& p = kv.second;
        const uint64_t h = fnv(p.data.data(), p.data.size());
        auto was = snapshot.find(kv.first);
        if (was != snapshot.end() && !was->second.dir && was->second.hash == h && was->second.size == p.data.size()) {
            if (was->second.realPath != p.path && storage.rename(was->second.realPath, p.path) == M4_FR_OK)
                lines.push_back("rename " + p.path);
            continue;
        }
        std::string real;
        if (storage.resolveForCreate(p.path, real) == M4_FR_OK && storage.writeFile(real, p.data) == M4_FR_OK)
            lines.push_back("write " + real + " (" + std::to_string(p.data.size()) + " bytes)");
    }
    std::vector<std::pair<std::string, Known>> gone;
    for (auto& kv : snapshot) if (!now.count(kv.first)) gone.push_back(kv);
    for (auto& g : gone) {                                   // files: only if the folder still has what the card had
        if (g.second.dir) continue;
        Bytes host;
        if (storage.readFile(g.second.realPath, host) != M4_FR_OK) continue;
        if (fnv(host.data(), host.size()) != g.second.hash) { lines.push_back("kept " + g.second.realPath + " (changed in the folder)"); continue; }
        if (storage.remove(g.second.realPath) == M4_FR_OK) lines.push_back("delete " + g.second.realPath);
    }
    std::sort(gone.begin(), gone.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (auto& g : gone)
        if (g.second.dir && storage.isDir(g.second.realPath) && storage.list(g.second.realPath).empty()
            && storage.remove(g.second.realPath) == M4_FR_OK) lines.push_back("rmdir " + g.second.realPath);

    snapshot.clear();
    for (auto& kv : now) {
        Known k; k.dir = kv.second.dir; k.size = kv.second.data.size(); k.realPath = storage.find(kv.second.path);
        if (k.realPath.empty()) k.realPath = kv.second.path;
        k.hash = k.dir ? 0 : fnv(kv.second.data.data(), kv.second.data.size());
        snapshot[kv.first] = k;
    }
    builtRevision = storage.revision;
    dirtySectors = false;
    if (report) for (const std::string& l : lines) *report += l + "\n";
    return true;
}

} // namespace cpcse
