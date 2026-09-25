// CPCSyntaxError — M4 Board storage device.
#include "m4.h"
#include "emulator.h"
#include "z80.h"
#include "memory.h"
#include <regex>
#include <set>

namespace cpcse {

static const int M4_ROM_SLOT = 6;
static const int RESP_BASE = 0xe800;
static const int RESP_SIZE = 0x0c00;
static const int CFG_BASE = 0xf400;
static const int SOCK_BASE = 0xfe00;
static const int M4_CMD_BUF = 2304;
static const int M4_MAX_FDS = 8;
static const int M4_HSEND_TRAP_ADDR = 0xf300;
static const int M4_HRECV_TRAP_ADDR = 0xf310;
static const uint8_t M4_HSEND_STUB[5] = { 0x01, 0x3e, 0xfd, 0xed, 0x79 };
static const uint8_t M4_HRECV_STUB[5] = { 0x01, 0x3f, 0xfd, 0xed, 0x79 };

static const int M4_OK = 0, M4_ERR_IO = 1, M4_ERR_NOFILE = 4, M4_ERR_NOPATH = 5, M4_ERR_BADNAME = 6,
    M4_ERR_DENIED = 7, M4_ERR_EXIST = 8, M4_ERR_BADFD = 9, M4_ERR_RDONLY = 10, M4_ERR_FULL = 18,
    M4_ERR_EOF = 20, M4_ERR_NOTSUP = 0xff;

static const int C_OPEN = 0x4301, C_READ = 0x4302, C_WRITE = 0x4303, C_CLOSE = 0x4304, C_SEEK = 0x4305,
    C_READDIR = 0x4306, C_EOF = 0x4307, C_CD = 0x4308, C_FREE = 0x4309, C_FTELL = 0x430a,
    C_ERASEFILE = 0x430e, C_RENAME = 0x430f, C_MAKEDIR = 0x4310, C_FSIZE = 0x4311,
    C_READ2 = 0x4312, C_GETPATH = 0x4313, C_SDREAD = 0x4314, C_SDWRITE = 0x4315,
    C_ROMSOFF = 0x4318, C_NMIOFF = 0x4319, C_RAMDISOFF = 0x431a, C_WRITE2 = 0x431b,
    C_HTTPGET = 0x4320, C_SETNETWORK = 0x4321, C_M4OFF = 0x4322, C_NETSTAT = 0x4323,
    C_TIME = 0x4324, C_DIRSETARGS = 0x4325, C_VERSION = 0x4326, C_HTTPGETMEM = 0x4328,
    C_COPYFILE = 0x432a, C_ROMLIST = 0x432c, C_NETSOCKET = 0x4331, C_NETCONNECT = 0x4332,
    C_NETCLOSE = 0x4333, C_NETSEND = 0x4334, C_NETRECV = 0x4335, C_NETHOSTIP = 0x4336,
    C_NETRSSI = 0x4337, C_NETBIND = 0x4338, C_NETLISTEN = 0x4339, C_NETACCEPT = 0x433a,
    C_GETNETWORK = 0x433b, C_WIFIPOW = 0x433c, C_ROMLOW = 0x433d, C_READMEM = 0x43fd,
    C_CONFIG = 0x43fe;

static std::string cstr(const std::vector<int>& bytes, int offset = 0) {
    std::string out;
    for (int i = offset; i < (int)bytes.size() && bytes[i] != 0; i += 1) out += (char)bytes[i];
    return out;
}
static std::string upper(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c; }
    out.push_back(cur); return out;
}
static std::string replaceAll(std::string s, const std::regex& re, const std::string& rep) { return std::regex_replace(s, re, rep); }

static std::string normalise(const std::string& pathIn) {
    std::string value = pathIn.empty() ? "/" : pathIn;
    value = replaceAll(value, std::regex("\\\\+"), "/");
    value = trim(value);
    if (std::regex_search(value, std::regex("^[A-Za-z]:"))) value = value.substr(2);
    if (value.empty() || value[0] != '/') value = "/" + value;
    std::vector<std::string> parts;
    for (const std::string& raw : split(value, '/')) {
        std::string part = trim(raw);
        if (part.empty() || part == ".") continue;
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else parts.push_back(upper(replaceAll(part, std::regex("[\\x00-\\x1f<>:\"|?*]"), "_")));
    }
    std::string joined;
    for (size_t i = 0; i < parts.size(); i++) { joined += parts[i]; if (i + 1 < parts.size()) joined += "/"; }
    return "/" + joined;
}
static std::string parentPath(const std::string& path) {
    std::string p = normalise(path); size_t at = p.rfind('/'); return (at == std::string::npos || at == 0) ? "/" : p.substr(0, at);
}
static std::string basePath(const std::string& path) {
    std::string p = normalise(path); return p == "/" ? "/" : p.substr(p.rfind('/') + 1);
}
static std::string joinPath(const std::string& cwd, const std::string& name) {
    std::string n = replaceAll(name, std::regex("\\\\+"), "/"); n = trim(n);
    if (n.empty() || n == ".") return normalise(cwd.empty() ? "/" : cwd);
    if (std::regex_search(n, std::regex("^[A-Za-z]:")) || (!n.empty() && n[0] == '/')) return normalise(n);
    return normalise((cwd.empty() ? "/" : cwd) + "/" + n);
}
static bool wildcard(const std::string& pattern, const std::string& name) {
    std::string p = upper(pattern.empty() ? "*" : pattern);
    p = replaceAll(p, std::regex("[.+^${}()|\\[\\]\\\\]"), "\\$&");
    p = replaceAll(p, std::regex("\\*"), ".*");
    p = replaceAll(p, std::regex("\\?"), ".");
    try { return std::regex_match(upper(name), std::regex("^" + p + "$", std::regex::icase)); }
    catch (...) { return false; }
}
static bool m4StorageCommand(int command) {
    return command == C_OPEN || command == C_READ || command == C_READ2 || command == C_WRITE || command == C_WRITE2
        || command == C_CLOSE || command == C_SEEK || command == C_EOF || command == C_FTELL || command == C_FSIZE
        || command == C_READDIR || command == C_DIRSETARGS || command == C_CD || command == C_GETPATH || command == C_FREE
        || command == C_ERASEFILE || command == C_RENAME || command == C_MAKEDIR || command == C_COPYFILE
        || command == C_SDREAD || command == C_SDWRITE;
}

static const int SD_SECTOR_SIZE = 512;
static const int SD_PART_START = 2048;
static const int SD_SECTORS_PER_CLUSTER = 4;
static const int SD_RESERVED_SECTORS = 1;
static const int SD_NUM_FATS = 2;
static const int SD_ROOT_ENTRIES = 512;
static const int SD_ROOT_DIR_SECTORS = 32;
static const int SD_FAT_SECTORS = 128;
static const int SD_FAT_START = SD_PART_START + SD_RESERVED_SECTORS;
static const int SD_ROOT_DIR_START = SD_FAT_START + SD_NUM_FATS * SD_FAT_SECTORS;
static const int SD_DATA_START = SD_ROOT_DIR_START + SD_ROOT_DIR_SECTORS;
static const int SD_CLUSTER_SIZE = SD_SECTOR_SIZE * SD_SECTORS_PER_CLUSTER;

static int sdTotalSectorsForDrive(M4Drive* drive) {
    long long bytes = drive ? drive->limitBytes : (16LL * 1024 * 1024);
    if (!bytes) bytes = 16LL * 1024 * 1024;
    return (int)std::max(32768LL, std::min(131072LL, bytes / SD_SECTOR_SIZE));
}
static void put16(Bytes& buf, int off, int value) { buf[off] = (uint8_t)(value & 0xff); buf[off + 1] = (uint8_t)((unsigned)value >> 8 & 0xff); }
static void put32(Bytes& buf, int off, unsigned value) { for (int i = 0; i < 4; i += 1) buf[off + i] = (uint8_t)(value >> (i * 8) & 0xff); }
static int sdOffset(int lba) { return (unsigned)lba * SD_SECTOR_SIZE; }
struct FatDT { int time; int date; };
static FatDT fatDateTimeNow() {
    std::time_t t = std::time(nullptr); std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    std::tm* tp = std::localtime(&t); if (tp) tmv = *tp;
#endif
    int time = (tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2);
    int date = (((tmv.tm_year + 1900) - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday;
    return { time & 0xffff, date & 0xffff };
}
static bool isFatSanChar(char c) {
    // regex [A-Z0-9$%'-_@~`!(){}^#&] (note '-_ is a range 0x27..0x5f)
    return std::regex_search(std::string(1, c), std::regex("[A-Z0-9$%'-_@~`!(){}^#&]"));
}
static std::string fatShortSanitizePart(const std::string& value, const std::string& fallback) {
    std::string s = upper(value);
    s = replaceAll(s, std::regex("[^A-Z0-9$%'-_@~`!(){}^#&]"), "_");
    if (s.empty()) s = fallback;
    s = replaceAll(s, std::regex("^\\.+|\\.+$"), "");
    if (s.empty()) return fallback;
    return s;
}
static std::string padEnd(const std::string& s, int len, char c = ' ') { std::string r = s; while ((int)r.size() < len) r += c; return r; }
static std::string fatAliasForName(const std::string& nameIn, std::set<std::string>& used) {
    std::string raw = nameIn.empty() ? "FILE.BIN" : nameIn;
    size_t dot = raw.rfind('.');
    std::string rawBase = dot != std::string::npos ? raw.substr(0, dot) : raw;
    std::string rawExt = dot != std::string::npos ? raw.substr(dot + 1) : "";
    std::string base = fatShortSanitizePart(rawBase, "FILE");
    std::string ext = fatShortSanitizePart(rawExt, "");
    bool simple = std::regex_search(base, std::regex("^[A-Z0-9$%'-_@~`!(){}^#&]{1,8}$"))
        && (ext.empty() || std::regex_search(ext, std::regex("^[A-Z0-9$%'-_@~`!(){}^#&]{1,3}$")))
        && raw == upper(raw);
    std::string shortBase = base.substr(0, 8);
    std::string shortExt = ext.substr(0, 3);
    std::string candidate = (padEnd(shortBase, 8) + padEnd(shortExt, 3)).substr(0, 11);
    int n = 1;
    while (!simple || used.count(candidate)) {
        std::string suffix = "~" + std::to_string(n);
        shortBase = base.substr(0, std::max((size_t)1, (size_t)(8 - suffix.size()))) + suffix;
        candidate = (padEnd(shortBase, 8) + padEnd(shortExt, 3)).substr(0, 11);
        if (!used.count(candidate)) break;
        n += 1;
    }
    used.insert(candidate);
    return candidate;
}
static int fatLfnChecksum(const std::string& shortName) {
    int sum = 0;
    for (int i = 0; i < 11; i += 1) sum = (((sum & 1) ? 0x80 : 0) + ((unsigned)sum >> 1) + (unsigned char)shortName[i]) & 0xff;
    return sum;
}
static bool fatNeedsLfn(const std::string& name, const std::string& /*shortName*/) {
    size_t dot = name.rfind('.');
    std::string base = dot != std::string::npos ? name.substr(0, dot) : name;
    std::string ext = dot != std::string::npos ? name.substr(dot + 1) : "";
    return name != upper(name) || base.size() > 8 || ext.size() > 3 || std::regex_search(name, std::regex("[^A-Za-z0-9$%'-_@~`!(){}^#&.]"));
}
static void writeLfnEntry(std::vector<Bytes>& out, int seq, bool last, const std::string& chars, int checksum) {
    Bytes e(32, 0xff);
    e[0] = (uint8_t)(seq | (last ? 0x40 : 0)); e[11] = 0x0f; e[13] = (uint8_t)checksum;
    static const int positions[13] = { 1,3,5,7,9,14,16,18,20,22,24,28,30 };
    for (int i = 0; i < 13; i += 1) {
        if (i >= (int)chars.size()) { e[positions[i]] = 0x00; e[positions[i] + 1] = 0x00; break; }
        e[positions[i]] = (uint8_t)((unsigned char)chars[i] & 0xff); e[positions[i] + 1] = 0;
    }
    out.push_back(e);
}
struct DirChild { std::string name; std::string kind; int cluster; int size; };
static std::vector<Bytes> makeDirEntries(const std::vector<DirChild>& children, int currentCluster, int parentCluster) {
    std::set<std::string> used;
    std::vector<Bytes> out;
    auto addShort = [&](const std::string& name, int attr, int cluster, int size) {
        std::string shortName = name == "." ? padEnd(".", 11)
            : name == ".." ? padEnd("..", 11)
            : fatAliasForName(name, used);
        if (name != "." && name != ".." && fatNeedsLfn(name, shortName)) {
            int sum = fatLfnChecksum(shortName);
            std::string chars = name;
            std::vector<std::string> chunks;
            for (size_t i = 0; i < chars.size(); i += 13) chunks.push_back(chars.substr(i, 13));
            for (int i = (int)chunks.size() - 1; i >= 0; i -= 1) writeLfnEntry(out, i + 1, i == (int)chunks.size() - 1, chunks[i], sum);
        }
        Bytes e(32, 0);
        for (int i = 0; i < 11; i += 1) e[i] = (uint8_t)shortName[i];
        e[11] = (uint8_t)attr;
        FatDT dt = fatDateTimeNow(); put16(e, 14, dt.time); put16(e, 16, dt.date); put16(e, 22, dt.time); put16(e, 24, dt.date);
        put16(e, 26, cluster ? cluster : 0); put32(e, 28, (unsigned)size);
        out.push_back(e);
    };
    if (currentCluster) {
        addShort(".", 0x10, currentCluster, 0);
        addShort("..", 0x10, parentCluster ? parentCluster : 0, 0);
    }
    for (const DirChild& child : children) addShort(child.name, child.kind == "dir" ? 0x10 : 0x20, child.cluster, child.size);
    return out;
}
static int fatDirectoryBytes(const std::vector<DirChild>& children, bool includeDots = true) {
    int entries = includeDots ? 2 : 0;
    for (const DirChild& child : children)
        entries += 1 + std::max(1, (int)std::ceil((child.name.size()) / 13.0));
    return (entries + 1) * 32;
}
static unsigned get16(const Bytes& data, int offset) { return (unsigned)(data[offset] | data[offset + 1] << 8); }
static unsigned get32(const Bytes& data, int offset) { return (unsigned)(data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16 | data[offset + 3] << 24); }

M4Fat16View parseM4Fat16Image(const Bytes& image) {
    if (image.size() < 4096) throw std::runtime_error("invalid M4 SD image");
    int partition = 0;
    if (image[510] == 0x55 && image[511] == 0xaa && image[0x1be + 4] != 0) partition = (int)get32(image, 0x1be + 8);
    int boot = partition * 512;
    if (image[boot + 510] != 0x55 || image[boot + 511] != 0xaa) throw std::runtime_error("invalid FAT boot sector");
    int bytesPerSector = get16(image, boot + 11);
    int sectorsPerCluster = image[boot + 13];
    int reserved = get16(image, boot + 14);
    int fats = image[boot + 16];
    int rootEntries = get16(image, boot + 17);
    int fatSectors = get16(image, boot + 22);
    if (bytesPerSector != 512 || !sectorsPerCluster || !fats || !fatSectors) throw std::runtime_error("unsupported M4 FAT geometry");
    int fatStart = partition + reserved;
    int rootStart = fatStart + fats * fatSectors;
    int rootSectors = (int)std::ceil(rootEntries * 32.0 / bytesPerSector);
    int dataStart = rootStart + rootSectors;
    int clusterBytes = bytesPerSector * sectorsPerCluster;
    auto fatEntry = [&](int cluster) { return (int)get16(image, fatStart * bytesPerSector + cluster * 2); };
    auto clusterOffset = [&](int cluster) { return (dataStart + (cluster - 2) * sectorsPerCluster) * bytesPerSector; };
    std::function<std::vector<int>(int)> chain = [&](int first) {
        std::vector<int> out; std::set<int> seen; int cluster = first;
        while (cluster >= 2 && cluster < 0xfff8 && !seen.count(cluster)) {
            int offset = clusterOffset(cluster);
            if (offset < 0 || offset + clusterBytes > (int)image.size()) break;
            out.push_back(offset); seen.insert(cluster); cluster = fatEntry(cluster);
        }
        return out;
    };
    auto lfnChars = [&](const Bytes& entry, int base0) {
        static const int positions[13] = { 1,3,5,7,9,14,16,18,20,22,24,28,30 }; std::string text;
        for (int pos : positions) { int code = entry[base0 + pos] | entry[base0 + pos + 1] << 8; if (code == 0 || code == 0xffff) break; text += (char)code; }
        return text;
    };
    auto shortNameFn = [&](const Bytes& data, int at) {
        std::string base; for (int i = 0; i < 8; i++) base += (char)data[at + i]; while (!base.empty() && base.back() == ' ') base.pop_back();
        std::string ext; for (int i = 8; i < 11; i++) ext += (char)data[at + i]; while (!ext.empty() && ext.back() == ' ') ext.pop_back();
        return ext.empty() ? base : base + "." + ext;
    };
    M4Fat16View view; view.dirs.insert("/");
    std::set<std::string> visited;
    std::function<void(const std::string&, const std::vector<int>&, long long)> parseDirectory =
        [&](const std::string& path, const std::vector<int>& offsets, long long byteLimit) {
        std::string key = path + ":" + std::to_string(offsets.empty() ? -1 : offsets[0]);
        if (visited.count(key)) return; visited.insert(key);
        Bytes data;
        for (int offset : offsets) {
            long long take = std::min((long long)clusterBytes, byteLimit - (long long)data.size());
            if (take <= 0) break;
            for (int i = 0; i < take && offset + i < (int)image.size(); i++) data.push_back(image[offset + i]);
        }
        std::vector<std::string> lfn;
        for (int at = 0; at + 32 <= (int)data.size(); at += 32) {
            if (data[at] == 0x00) break;
            if (data[at] == 0xe5) { lfn.clear(); continue; }
            if (data[at + 11] == 0x0f) {
                int idx = (data[at] & 0x1f) - 1;
                if (idx >= 0) { if ((int)lfn.size() <= idx) lfn.resize(idx + 1); lfn[idx] = lfnChars(data, at); }
                continue;
            }
            if (data[at + 11] & 0x08) { lfn.clear(); continue; }
            std::string rawName;
            if (!lfn.empty()) { for (auto& s : lfn) rawName += s; } else rawName = shortNameFn(data, at);
            lfn.clear();
            std::string name = trim(rawName);
            if (name.empty() || name == "." || name == "..") continue;
            std::string full = normalise(path == "/" ? "/" + name : path + "/" + name);
            int firstCluster = data[at + 26] | data[at + 27] << 8;
            int size = (int)get32(data, at + 28);
            if (data[at + 11] & 0x10) {
                view.dirs.insert(full);
                if (firstCluster >= 2) parseDirectory(full, chain(firstCluster), (long long)1 << 62);
            } else {
                std::vector<int> chunks = chain(firstCluster); Bytes payload(size, 0); int pos = 0;
                for (int offset : chunks) {
                    int take = std::min(clusterBytes, size - pos); if (take <= 0) break;
                    for (int i = 0; i < take; i++) payload[pos + i] = image[offset + i]; pos += take;
                }
                view.files[full] = { full, basePath(full), payload, size };
            }
        }
    };
    std::vector<int> rootOffsets = { rootStart * bytesPerSector };
    parseDirectory("/", rootOffsets, (long long)rootEntries * 32);
    return view;
}

M4Board::M4Board(GX4000* emulator, M4Drive* drive) : emulator(emulator) {
    this->drive = nullptr;
    syncingSdToDrive = false;
    enabled = false;
    romConfigSeed.assign(0x100, 0);
    reset();
    setDrive(drive);
}
void M4Board::setDrive(M4Drive* drive_) {
    drive = drive_;
    sdImage.clear(); sdDirty = false;
}
bool M4Board::syncDriveFromSdImage(bool persist) {
    (void)persist;
    if (sdImage.empty() || !drive || drive->readOnly) return false;
    try {
        M4Fat16View parsed = parseM4Fat16Image(sdImage);
        syncingSdToDrive = true;
        drive->files = parsed.files;
        drive->dirs = parsed.dirs;
        if (!parsed.dirs.count(drive->cwd)) drive->cwd = "/";
        drive->emitChange();
        sdDirty = false;
        syncingSdToDrive = false;
        return true;
    } catch (...) {
        lastError = M4_ERR_IO;
        syncingSdToDrive = false;
        return false;
    }
}
void M4Board::scheduleSdSync() {
    // Headless: no timer/event loop; the server persistence path is out of scope.
}
void M4Board::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
    reset();
}
void M4Board::loadRomDefaults(Bytes& romBytes) {
    std::fill(romConfigSeed.begin(), romConfigSeed.end(), 0);
    if (!romBytes.empty()) {
        for (int i = 0; i < 0x100 && 0x3400 + i < (int)romBytes.size(); i++) romConfigSeed[i] = romBytes[0x3400 + i];
        patchHelperTable(romBytes);
    }
    if (!cfgMem.empty()) for (int i = 0; i < 0x100; i++) cfgMem[i] = romConfigSeed[i];
    installHelperShim();
}
void M4Board::patchHelperTable(Bytes& romBytes) {
    if (romBytes.size() < 0x2434) return;
    romBytes[0x2430] = (uint8_t)(M4_HSEND_TRAP_ADDR & 0xff);
    romBytes[0x2431] = (uint8_t)((unsigned)M4_HSEND_TRAP_ADDR >> 8);
    romBytes[0x2432] = (uint8_t)(M4_HRECV_TRAP_ADDR & 0xff);
    romBytes[0x2433] = (uint8_t)((unsigned)M4_HRECV_TRAP_ADDR >> 8);
}
void M4Board::installHelperShim() {
    if (busMem.empty()) return;
    for (int i = 0; i < 5; i++) busMem[M4_HSEND_TRAP_ADDR - RESP_BASE + i] = M4_HSEND_STUB[i];
    for (int i = 0; i < 5; i++) busMem[M4_HRECV_TRAP_ADDR - RESP_BASE + i] = M4_HRECV_STUB[i];
}
void M4Board::reset() {
    if (sdDirty) syncDriveFromSdImage(true);
    cmd.clear();
    busMem.assign(RESP_SIZE, 0);
    cfgMem.assign(0x100, 0);
    for (int i = 0; i < 0x100; i++) cfgMem[i] = romConfigSeed[i];
    sockMem.assign(0x100, 0);
    for (auto& fd : fds) fd = M4Fd{};
    dirList.clear();
    dirIndex = 0;
    dirFilter = "*";
    nmiEnabled = false;
    lastError = M4_OK;
    ramMode = false;
    initCount = 0;
    sdImage.clear();
    sdDirty = false;
    installHelperShim();
}
void M4Board::dataPortWrite(int value) { if ((int)cmd.size() < M4_CMD_BUF) cmd.push_back(value & 0xff); }
int M4Board::dataPortRead() { return 0; }
int M4Board::readMemory(int address, int selectedRom) {
    if (!enabled) return -1;
    address &= 0xffff;
    bool helperStub = (address >= M4_HSEND_TRAP_ADDR && address < M4_HSEND_TRAP_ADDR + 5)
        || (address >= M4_HRECV_TRAP_ADDR && address < M4_HRECV_TRAP_ADDR + 5);
    if (selectedRom != M4_ROM_SLOT && !helperStub) return -1;
    if (emulator && emulator->cpu) {
        int sp = emulator->cpu->sp;
        if (address == (sp & 0xffff) || address == ((sp + 1) & 0xffff)) return -1;
    }
    if (helperStub) return busMem[address - RESP_BASE];
    if (address >= RESP_BASE && address < RESP_BASE + RESP_SIZE) return busMem[address - RESP_BASE];
    if (address >= CFG_BASE && address < CFG_BASE + (int)cfgMem.size()) return cfgMem[address - CFG_BASE];
    if (address >= SOCK_BASE && address < SOCK_BASE + (int)sockMem.size()) return sockMem[address - SOCK_BASE];
    return -1;
}
int M4Board::readMemory(int address) { return readMemory(address, M4_ROM_SLOT); }
void M4Board::resp8(RespCtx& ctx, int value) { if (ctx.off < (int)busMem.size()) busMem[ctx.off] = (uint8_t)(value & 0xff); ctx.off += 1; }
void M4Board::resp16(RespCtx& ctx, int value) { resp8(ctx, value); resp8(ctx, (unsigned)value >> 8); }
void M4Board::resp32(RespCtx& ctx, int value) { for (int i = 0; i < 4; i += 1) resp8(ctx, (unsigned)value >> (i * 8)); }
void M4Board::respStr(RespCtx& ctx, const std::string& text) { for (char ch : text) resp8(ctx, (unsigned char)ch); resp8(ctx, 0); }
void M4Board::frame(int command, int off) { busMem[0] = (uint8_t)((off - 1) & 0xff); busMem[1] = (uint8_t)(command & 0xff); busMem[2] = (uint8_t)((unsigned)command >> 8 & 0xff); }
bool M4Board::validFd(int fd) { return fd >= 1 && fd <= M4_MAX_FDS && fds[fd - 1].inUse; }
void M4Board::closeFd(int fd) { if (fd >= 1 && fd <= M4_MAX_FDS) fds[fd - 1] = M4Fd{}; }
int M4Board::allocFd(int mode) {
    if (mode & 0x80) { for (int i = 2; i < M4_MAX_FDS; i += 1) if (!fds[i].inUse) return i + 1; return -1; }
    int fd = (mode & 2) ? 2 : 1; closeFd(fd); return fd;
}
std::vector<M4DriveRow> M4Board::driveRows() { return drive ? drive->list(drive->cwd) : std::vector<M4DriveRow>(); }
M4File* M4Board::resolveFile(const std::string& name) {
    if (!drive) return nullptr;
    M4File* f = drive->find(name); if (f) return f;
    f = drive->find(name + ".BAS"); if (f) return f;
    return drive->find(name + ".BIN");
}
void M4Board::restoreRamConfig(int config, int segment) {
    if (!emulator || !emulator->memory) return;
    int high = ((0x7f - (segment ? segment : 0)) & 0xff) << 8;
    emulator->memory->setRamConfig(config & 0x3f, high);
}
void M4Board::selectRamConfigFromBank(int bank) {
    if (!emulator || !emulator->memory) return;
    if ((bank & 0xc0) == 0xc0) emulator->memory->setRamConfig(bank & 0x3f);
}
int M4Board::readHelperByte(int address) {
    address &= 0xffff;
    if (address >= RESP_BASE && address < RESP_BASE + RESP_SIZE) return busMem[address - RESP_BASE];
    if (address >= CFG_BASE && address < CFG_BASE + (int)cfgMem.size()) return cfgMem[address - CFG_BASE];
    if (address >= SOCK_BASE && address < SOCK_BASE + (int)sockMem.size()) return sockMem[address - SOCK_BASE];
    return (emulator && emulator->memory) ? emulator->memory->read(address) : 0xff;
}
void M4Board::writeHelperByte(int address, int value) {
    address &= 0xffff; value &= 0xff;
    if (address >= RESP_BASE && address < RESP_BASE + RESP_SIZE) { busMem[address - RESP_BASE] = (uint8_t)value; return; }
    if (emulator && emulator->memory) emulator->memory->write(address, value);
}
bool M4Board::helperTrap(int lowByte, Z80* cpu) {
    if (!enabled || !cpu) return false;
    GXMemory* mem = emulator ? emulator->memory : nullptr;
    int src = cpu->hl() & 0xffff;
    int dest = cpu->de() & 0xffff;
    int length = ((cpu->iy & 0xff00) | (cpu->c & 0xff)) & 0xffff;
    int destBank = cpu->a & 0xff;
    int sourceBank = cpu->iy & 0xff;
    int savedConfig = mem ? mem->ramConfig : 0;
    int savedSegment = mem ? mem->ram4MbSegment : 0;
    selectRamConfigFromBank(destBank);
    for (int i = 0; i < length; i += 1) {
        int sa = (src + i) & 0xffff;
        int da = (dest + i) & 0xffff;
        if ((lowByte & 0xff) == 0x3f) { if (mem) mem->write(da, readHelperByte(sa)); }
        else writeHelperByte(da, mem ? mem->read(sa) : 0xff);
    }
    if ((sourceBank & 0xc0) == 0xc0) selectRamConfigFromBank(sourceBank);
    else restoreRamConfig(savedConfig, savedSegment);
    if ((lowByte & 0xff) == 0x3f) ramMode = false;
    cpu->pc = cpu->ix & 0xffff;
    return true;
}
Bytes& M4Board::ensureSdImage() {
    int totalSectors = sdTotalSectorsForDrive(drive);
    if (!sdImage.empty() && (int)sdImage.size() == totalSectors * SD_SECTOR_SIZE) return sdImage;
    int partSectors = totalSectors - SD_PART_START;
    int dataClusters = (totalSectors - SD_DATA_START) / SD_SECTORS_PER_CLUSTER;
    int maxCluster = dataClusters + 1;
    Bytes image(totalSectors * SD_SECTOR_SIZE, 0);
    image[0x1be] = 0x00; image[0x1be + 4] = 0x06; put32(image, 0x1be + 8, SD_PART_START); put32(image, 0x1be + 12, partSectors); image[510] = 0x55; image[511] = 0xaa;
    int boot = sdOffset(SD_PART_START);
    { const uint8_t sig[3] = { 0xeb, 0x3c, 0x90 }; for (int i = 0; i < 3; i++) image[boot + i] = sig[i]; }
    { const char* oem = "CPCSE   "; for (int i = 0; i < 8; i += 1) image[boot + 3 + i] = (uint8_t)oem[i]; }
    put16(image, boot + 11, 512); image[boot + 13] = SD_SECTORS_PER_CLUSTER; put16(image, boot + 14, SD_RESERVED_SECTORS); image[boot + 16] = SD_NUM_FATS; put16(image, boot + 17, SD_ROOT_ENTRIES);
    put16(image, boot + 19, partSectors <= 0xffff ? partSectors : 0); image[boot + 21] = 0xf8; put16(image, boot + 22, SD_FAT_SECTORS); put16(image, boot + 24, 63); put16(image, boot + 26, 255);
    put32(image, boot + 28, SD_PART_START); put32(image, boot + 32, partSectors > 0xffff ? partSectors : 0); image[boot + 36] = 0x80; image[boot + 38] = 0x29; put32(image, boot + 39, 0x19840001u);
    { const char* lab = "CPCSE M4   "; for (int i = 0; i < 11; i += 1) image[boot + 43 + i] = (uint8_t)lab[i]; }
    { const char* fs = "FAT16   "; for (int i = 0; i < 8; i += 1) image[boot + 54 + i] = (uint8_t)fs[i]; }
    image[boot + 510] = 0x55; image[boot + 511] = 0xaa;
    auto writeFat = [&](int cluster, int value) { for (int f = 0; f < SD_NUM_FATS; f += 1) put16(image, sdOffset(SD_FAT_START + f * SD_FAT_SECTORS) + cluster * 2, value); };
    writeFat(0, 0xfff8); writeFat(1, 0xffff);
    int nextCluster = 2;
    struct Chain { int first; int count; };
    auto allocChain = [&](int bytes) -> Chain {
        int count = std::max(1, (int)std::ceil((double)bytes / SD_CLUSTER_SIZE));
        if (nextCluster + count - 1 > maxCluster) throw std::runtime_error("M4 SD image capacity exceeded");
        int first = nextCluster;
        for (int i = 0; i < count; i += 1) writeFat(nextCluster + i, i == count - 1 ? 0xffff : nextCluster + i + 1);
        nextCluster += count; return { first, count };
    };
    struct Node { std::string path, name, kind; std::vector<Node*> children; Bytes data; int size = 0; int cluster = 0; };
    std::map<std::string, std::unique_ptr<Node>> tree;
    std::function<Node* (const std::string&)> getNode = [&](const std::string& path) -> Node* {
        std::string p = normalise(path);
        if (!tree.count(p)) { auto n = std::make_unique<Node>(); n->path = p; n->name = basePath(p) == "/" ? "" : basePath(p); n->kind = "dir"; tree[p] = std::move(n); }
        return tree[p].get();
    };
    getNode("/");
    if (drive) for (const std::string& d : drive->dirs) getNode(d);
    if (drive) for (auto& kv : drive->files) {
        const M4File& file = kv.second;
        getNode(parentPath(file.path));
        auto n = std::make_unique<Node>(); n->path = file.path; n->name = basePath(file.path); n->kind = "file"; n->data = file.data; n->size = !file.data.empty() ? (int)file.data.size() : file.size;
        tree[file.path] = std::move(n);
    }
    std::vector<Node*> nodes; for (auto& kv : tree) nodes.push_back(kv.second.get());
    for (Node* node : nodes) if (node->path != "/") getNode(parentPath(node->path))->children.push_back(node);
    std::vector<Node*> sorted = nodes; std::sort(sorted.begin(), sorted.end(), [](Node* a, Node* b) { return a->path < b->path; });
    for (Node* node : sorted) {
        if (node->path == "/") continue;
        if (node->kind == "dir") {
            std::vector<DirChild> ch; for (Node* c : node->children) ch.push_back({ c->name, c->kind, c->cluster, c->size });
            node->cluster = allocChain(fatDirectoryBytes(ch, true)).first;
        } else {
            Chain ch = allocChain(node->size ? node->size : 1); node->cluster = ch.first;
            int off = sdOffset(SD_DATA_START + (node->cluster - 2) * SD_SECTORS_PER_CLUSTER);
            if (off + (int)node->data.size() > (int)image.size()) throw std::runtime_error("M4 SD image capacity exceeded");
            for (int i = 0; i < (int)node->data.size(); i++) image[off + i] = node->data[i];
        }
    }
    auto writeDir = [&](Node* node) {
        std::vector<DirChild> ch; for (Node* c : node->children) ch.push_back({ c->name, c->kind, c->cluster, c->size });
        std::sort(ch.begin(), ch.end(), [](const DirChild& a, const DirChild& b) { return a.kind == b.kind ? a.name < b.name : (a.kind == "dir" ? true : false); });
        std::vector<Bytes> entries = makeDirEntries(ch, node->cluster, node->path == "/" ? 0 : getNode(parentPath(node->path))->cluster);
        if (node->path == "/" && (int)entries.size() > SD_ROOT_ENTRIES) throw std::runtime_error("M4 SD root directory is full");
        int start = node->path == "/" ? sdOffset(SD_ROOT_DIR_START) : sdOffset(SD_DATA_START + (node->cluster - 2) * SD_SECTORS_PER_CLUSTER);
        for (int i = 0; i < (int)entries.size(); i += 1) for (int k = 0; k < 32; k++) image[start + i * 32 + k] = entries[i][k];
    };
    writeDir(getNode("/"));
    for (auto& kv : tree) if (kv.second->kind == "dir" && kv.second->path != "/") writeDir(kv.second.get());
    sdImage = image; sdDirty = false; return sdImage;
}

static void writeFdByte(M4Fd& f, int value) {
    if (f.pos >= (int)f.data.size()) f.data.resize(f.pos + 1, 0);
    f.data[f.pos] = value & 0xff; f.pos++;
}

bool M4Board::ack(Z80* cpu) {
    std::fill(busMem.begin(), busMem.end(), 0);
    int start = 0;
    if ((int)cmd.size() < 3 || cmd[2] != 0x43) {
        start = -1;
        for (int i = 0; i < (int)cmd.size(); i++) if (i + 2 < (int)cmd.size() && cmd[i + 2] == 0x43) { start = i; break; }
    }
    if (start < 0 || (int)cmd.size() - start < 3) { frame(0, 3); cmd.clear(); return nmiEnabled; }
    std::vector<int> packet(cmd.begin() + start, cmd.end());
    int command = packet[1] | (packet[2] << 8);
    std::vector<int> p(packet.begin() + 3, packet.end());
    RespCtx ctx{ 3 };
    if (m4StorageCommand(command) && drive) drive->noteActivity(220);
    if (sdDirty && command != C_SDREAD && command != C_SDWRITE) syncDriveFromSdImage(false);
    ramMode = false;
    int err = M4_ERR_NOTSUP;
    try {
        switch (command) {
            case C_CONFIG:
                if (p.size() >= 1) {
                    for (int i = 1; i < (int)p.size() && p[0] + i - 1 < (int)cfgMem.size(); i += 1) cfgMem[p[0] + i - 1] = (uint8_t)p[i];
                    if (p[0] == 5 && p.size() >= 2) initCount = p[1];
                }
                err = M4_OK; break;
            case C_VERSION: err = M4_OK; respStr(ctx, "M4 CPCSyntaxError Emulator"); break;
            case C_TIME: {
                std::time_t t = std::time(nullptr); std::tm tmv{};
#if defined(_WIN32)
                localtime_s(&tmv, &t);
#else
                std::tm* tp = std::localtime(&t); if (tp) tmv = *tp;
#endif
                auto pad = [](int n) { std::string s = std::to_string(n); return s.size() < 2 ? "0" + s : s; };
                err = M4_OK; respStr(ctx, pad(tmv.tm_hour) + ":" + pad(tmv.tm_min) + ":" + pad(tmv.tm_sec) + " " + std::to_string(tmv.tm_year + 1900) + "-" + pad(tmv.tm_mon + 1) + "-" + pad(tmv.tm_mday)); break;
            }
            case C_NMIOFF: nmiEnabled = p.size() ? p[0] == 0 : false; err = M4_OK; break;
            case C_ROMLOW: err = M4_OK; break;
            case C_M4OFF: case C_ROMSOFF: case C_RAMDISOFF: err = M4_OK; break;
            case C_GETPATH: err = M4_OK; respStr(ctx, drive ? drive->cwd : "/"); break;
            case C_CD: {
                std::string target = joinPath(drive ? drive->cwd : "/", cstr(p));
                if (drive && drive->dirs.count(target)) { drive->chdir(target); err = M4_OK; }
                else err = M4_ERR_NOFILE;
                break;
            }
            case C_FREE: {
                long long used = 0; if (drive) for (auto& kv : drive->files) used += !kv.second.data.empty() ? (long long)kv.second.data.size() : kv.second.size;
                long long capacity = drive && drive->limitBytes ? drive->limitBytes : (16LL * 1024 * 1024);
                long long freeK = std::max(0LL, (capacity - used) / 1024);
                err = M4_OK; respStr(ctx, "\r\n" + std::to_string(freeK) + "K free\r\n\r\n"); break;
            }
            case C_DIRSETARGS: {
                dirFilter = cstr(p).empty() ? "*" : cstr(p);
                dirList.clear();
                for (auto& row : driveRows()) if (wildcard(dirFilter, row.name)) dirList.push_back(row);
                dirIndex = 0; err = M4_OK; break;
            }
            case C_READDIR: {
                if (dirIndex >= (int)dirList.size()) { err = 2; break; }
                M4DriveRow row = dirList[dirIndex++];
                bool isDir = row.kind == "dir";
                std::string name8(8, ' '), ext3(3, ' ');
                std::string n = row.name;
                if (isDir) { name8[0] = '>'; for (int i = 1; i < 8 && i - 1 < (int)n.size(); i += 1) name8[i] = n[i - 1]; }
                else { size_t dot = n.rfind('.'); std::string base = dot != std::string::npos ? n.substr(0, dot) : n; std::string ext = dot != std::string::npos ? n.substr(dot + 1) : ""; for (int i = 0; i < std::min(8, (int)base.size()); i += 1) name8[i] = base[i]; for (int i = 0; i < std::min(3, (int)ext.size()); i += 1) ext3[i] = ext[i]; }
                err = M4_OK; for (char ch : name8) resp8(ctx, (unsigned char)ch); resp8(ctx, '.'); for (char ch : ext3) resp8(ctx, (unsigned char)ch);
                std::string sizeText = isDir ? "  DIR" : std::to_string(std::min(99999, row.size)); while (sizeText.size() < 5) sizeText = " " + sizeText;
                for (char ch : sizeText) resp8(ctx, (unsigned char)ch); resp8(ctx, 0); resp16(ctx, row.size & 0xffff); break;
            }
            case C_OPEN: {
                if (p.size() < 2) { err = M4_ERR_IO; break; }
                int mode = p[0]; std::string name = cstr(p, 1); bool isWrite = !!(mode & 2);
                bool createAlways = !!(mode & 0x08), openAlways = !!(mode & 0x10);
                int fd = allocFd(mode); int openErr = M4_OK, outFd = fd & 0xff;
                if (fd < 0) openErr = M4_ERR_FULL;
                else if (isWrite) {
                    if (drive && drive->readOnly) openErr = M4_ERR_RDONLY;
                    else {
                        M4File* existing = !createAlways && openAlways ? resolveFile(name) : nullptr;
                        M4Fd f; f.inUse = true; f.pos = 0; f.write = true;
                        if (existing) { for (uint8_t b : existing->data) f.data.push_back(b); f.path = existing->path; }
                        else f.path = joinPath(drive ? drive->cwd : "/", name);
                        fds[fd - 1] = f;
                    }
                } else {
                    M4File* file = resolveFile(name);
                    if (!file) openErr = M4_ERR_NOFILE;
                    else { M4Fd f; f.inUse = true; f.pos = 0; f.write = false; for (uint8_t b : file->data) f.data.push_back(b); f.path = file->path; fds[fd - 1] = f; }
                }
                if (openErr != M4_OK) { outFd = 0xff; closeFd(fd); }
                err = openErr; resp8(ctx, outFd); resp8(ctx, openErr); break;
            }
            case C_READ: case C_READ2: {
                int fd = p.size() ? p[0] : 0, count = p.size() >= 3 ? (p[1] | (p[2] << 8)) : 1;
                if (!validFd(fd)) { resp8(ctx, M4_ERR_BADFD); err = M4_ERR_BADFD; break; }
                M4Fd& f = fds[fd - 1]; int actual = 0;
                if (command == C_READ2) { busMem[3] = 0; busMem[4] = 0; busMem[5] = 0; ctx.off = 8; }
                else resp8(ctx, M4_OK);
                for (int i = 0; i < count; i += 1) {
                    bool has = f.pos < (int)f.data.size();
                    if (!has) {
                        if (command == C_READ) resp8(ctx, 0);
                        else { busMem[3] = M4_ERR_EOF; break; }
                    } else {
                        int value = f.data[f.pos]; f.pos += 1; resp8(ctx, value); actual += 1;
                    }
                }
                if (command == C_READ2) { busMem[4] = (uint8_t)(actual & 0xff); busMem[5] = (uint8_t)((unsigned)actual >> 8); }
                err = M4_OK; break;
            }
            case C_WRITE: case C_WRITE2: {
                int fd = p.size() ? p[0] : 0; if (!validFd(fd)) { resp8(ctx, M4_ERR_BADFD); err = M4_ERR_BADFD; break; }
                M4Fd& f = fds[fd - 1]; if (!f.write) { resp8(ctx, M4_ERR_DENIED); err = M4_ERR_DENIED; break; }
                if (command == C_WRITE2) { if (p.size() >= 2) writeFdByte(f, p[1]); }
                else { for (int i = 1; i < (int)p.size(); i += 1) writeFdByte(f, p[i]); }
                err = M4_OK; resp8(ctx, M4_OK); break;
            }
            case C_CLOSE: {
                int fd = p.size() ? p[0] : 0; if (!validFd(fd)) { err = M4_ERR_BADFD; resp8(ctx, 0xff); break; }
                M4Fd& f = fds[fd - 1]; if (f.write && drive) { Bytes b; for (int v : f.data) b.push_back((uint8_t)v); drive->addFile(f.path, b); } closeFd(fd); err = M4_OK; resp8(ctx, 0); break;
            }
            case C_SEEK: { int fd = p.size() ? p[0] : 0; if (!validFd(fd) || p.size() < 5) { err = M4_ERR_BADFD; resp8(ctx, err); break; } fds[fd - 1].pos = (int)((unsigned)(p[1] | p[2] << 8 | p[3] << 16 | p[4] << 24)); err = M4_OK; resp8(ctx, M4_OK); break; }
            case C_EOF: { int fd = p.size() ? p[0] : 0; if (!validFd(fd)) { err = M4_ERR_BADFD; break; } err = M4_OK; resp8(ctx, fds[fd - 1].pos >= (int)fds[fd - 1].data.size() ? 1 : 0); break; }
            case C_FTELL: { int fd = p.size() ? p[0] : 0; if (!validFd(fd)) { err = M4_ERR_BADFD; break; } err = M4_OK; resp32(ctx, fds[fd - 1].pos); break; }
            case C_FSIZE: { int fd = p.size() ? p[0] : 0; if (!validFd(fd)) { err = M4_ERR_BADFD; break; } err = M4_OK; resp32(ctx, (int)fds[fd - 1].data.size()); break; }
            case C_ERASEFILE: { M4File* file = resolveFile(cstr(p)); if (file && !(drive && drive->readOnly)) { if (drive) drive->deletePath(file->path); resp8(ctx, M4_OK); } else resp8(ctx, file ? M4_ERR_RDONLY : M4_ERR_NOFILE); err = M4_OK; break; }
            case C_RENAME: { std::string oldName = cstr(p); int next = (int)oldName.size() + 1; std::string newName = cstr(p, next); bool ok = drive && !drive->readOnly && drive->rename(oldName, newName); err = ok ? M4_OK : M4_ERR_IO; break; }
            case C_MAKEDIR: { if (!(drive && drive->readOnly)) { if (drive) drive->ensureDir(joinPath(drive->cwd, cstr(p))); err = M4_OK; } else err = M4_ERR_RDONLY; break; }
            case C_NETSTAT: err = M4_OK; respStr(ctx, "Connected (CPCSyntaxError)"); resp8(ctx, 5); break;
            case C_NETRSSI: err = M4_OK; resp8(ctx, 0xb8); resp8(ctx, 5); break;
            case C_GETNETWORK: {
                err = M4_OK;
                Bytes net(196, 0);
                auto putAscii = [&](int offset, const std::string& text, int limit) { for (int i = 0; i < std::min(limit, (int)text.size()); i += 1) net[offset + i] = (uint8_t)(text[i] & 0xff); };
                putAscii(0, "CPCSyntaxError", 16);
                putAscii(16, "emulator", 32);
                net[112] = 192; net[113] = 168; net[114] = 1; net[115] = 100;
                net[116] = 255; net[117] = 255; net[118] = 255; net[119] = 0;
                net[120] = 192; net[121] = 168; net[122] = 1; net[123] = 1;
                net[124] = 8; net[125] = 8; net[126] = 8; net[127] = 8;
                net[128] = 8; net[129] = 8; net[130] = 4; net[131] = 4;
                net[132] = 0;
                { const uint8_t mac[6] = { 0x02, 0x19, 0x84, 0x00, 0x00, 0x01 }; for (int i = 0; i < 6; i++) net[190 + i] = mac[i]; }
                for (uint8_t b : net) resp8(ctx, b);
                break;
            }
            case C_SETNETWORK: case C_WIFIPOW: err = M4_OK; break;
            case C_SDREAD: {
                if (p.size() < 5) { resp8(ctx, 3); err = M4_OK; break; }
                int lba = (int)((unsigned)(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24));
                int sectors = p[4] & 0xff;
                if (sectors == 0 || sectors > 4) { resp8(ctx, 4); err = M4_OK; break; }
                Bytes& image = ensureSdImage();
                resp8(ctx, 0);
                for (int i = 0; i < sectors * SD_SECTOR_SIZE; i += 1) { int idx = sdOffset(lba) + i; resp8(ctx, idx < (int)image.size() ? image[idx] : 0); }
                err = M4_OK; break;
            }
            case C_SDWRITE: {
                if (p.size() < 5) { resp8(ctx, 3); err = M4_OK; break; }
                int lba = (int)((unsigned)(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24));
                int sectors = p[4] & 0xff;
                int bytes = sectors * SD_SECTOR_SIZE;
                if (sectors == 0 || sectors > 4 || (int)p.size() < 5 + bytes) { resp8(ctx, 4); err = M4_OK; break; }
                Bytes& image = ensureSdImage();
                int offset = sdOffset(lba);
                if (offset + bytes > (int)image.size()) { resp8(ctx, 1); err = M4_OK; break; }
                for (int i = 0; i < bytes; i++) image[offset + i] = (uint8_t)p[5 + i];
                sdDirty = true;
                scheduleSdSync();
                resp8(ctx, 0);
                err = M4_OK; break;
            }
            case C_READMEM: {
                if (p.size() < 3) { err = M4_ERR_IO; break; }
                int addr = p[0] | (p[1] << 8);
                int count = p[2] & 0xff;
                Bytes* source = nullptr;
                int cap = 0, sourceBase = 0;
                if (addr >= RESP_BASE && addr < RESP_BASE + RESP_SIZE) { source = &busMem; cap = RESP_BASE + RESP_SIZE - addr; sourceBase = RESP_BASE; }
                else if (addr >= CFG_BASE && addr < CFG_BASE + (int)cfgMem.size()) { source = &cfgMem; cap = CFG_BASE + (int)cfgMem.size() - addr; sourceBase = CFG_BASE; }
                else if (addr >= SOCK_BASE && addr < SOCK_BASE + (int)sockMem.size()) { source = &sockMem; cap = SOCK_BASE + (int)sockMem.size() - addr; sourceBase = SOCK_BASE; }
                if (!source) { err = M4_ERR_IO; break; }
                count = std::min(count, cap);
                int baseOff = addr - sourceBase;
                for (int i = 0; i < count; i += 1) resp8(ctx, (*source)[baseOff + i]);
                err = M4_OK; break;
            }
            default: err = M4_ERR_NOTSUP; break;
        }
    } catch (...) { err = M4_ERR_IO; }
    lastError = err;
    frame(command, ctx.off);
    cmd.clear();
    if (nmiEnabled && cpu) cpu->requestNmi();
    return nmiEnabled;
}

} // namespace cpcse
