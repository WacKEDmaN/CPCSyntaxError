// CPCSyntaxError — the M4 Board. See m4.h.
#include "m4.h"
#include "emulator.h"
#include "z80.h"

#include <ctime>

namespace cpcse {

// ROM link table (M4ROM.s): rom_response, rom_config, sock_status.
static const int RESP_BASE = 0xe800;
static const int RESP_SIZE = 0x0c00;
static const int CMD_MAX = 2304;

// m4cmds.i
static const int C_OPEN = 0x4301, C_READ = 0x4302, C_WRITE = 0x4303, C_CLOSE = 0x4304, C_SEEK = 0x4305,
    C_READDIR = 0x4306, C_EOF = 0x4307, C_CD = 0x4308, C_FREE = 0x4309, C_FTELL = 0x430a,
    C_READSECTOR = 0x430b, C_WRITESECTOR = 0x430c, C_FORMATTRACK = 0x430d, C_ERASEFILE = 0x430e,
    C_RENAME = 0x430f, C_MAKEDIR = 0x4310, C_FSIZE = 0x4311, C_READ2 = 0x4312, C_GETPATH = 0x4313,
    C_SDREAD = 0x4314, C_SDWRITE = 0x4315, C_FSTAT = 0x4316, C_ROMSOFF = 0x4318, C_NMIOFF = 0x4319,
    C_RAMDISOFF = 0x431a, C_WRITE2 = 0x431b, C_HTTPGET = 0x4320, C_SETNETWORK = 0x4321, C_M4OFF = 0x4322,
    C_NETSTAT = 0x4323, C_TIME = 0x4324, C_DIRSETARGS = 0x4325, C_VERSION = 0x4326, C_UPGRADE = 0x4327,
    C_HTTPGETMEM = 0x4328, C_COPYBUF = 0x4329, C_COPYFILE = 0x432a, C_ROMSUPDATE = 0x432b,
    C_ROMLIST = 0x432c, C_NETSOCKET = 0x4331, C_NETRSSI = 0x4337, C_GETNETWORK = 0x433b, C_WIFIPOW = 0x433c,
    C_ROMLOW = 0x433d, C_ROMCP = 0x43fc, C_ROMWRITE = 0x43fd, C_CONFIG = 0x43fe;
// ff.i
static const int FA_READ = 1, FA_WRITE = 2, FA_CREATE_NEW = 4, FA_CREATE_ALWAYS = 8, FA_OPEN_ALWAYS = 16, FA_REALMODE = 128;

M4Board::M4Board(GX4000* emulator) : emulator(emulator) {
    rom.assign(0x4000, 0xff);
    hackRom.assign(0x4000, 0xff);
    powerOn();
}
M4Board::~M4Board() { flush(); }

long long M4Board::now() const { return emulator ? emulator->machineCycles : 0; }

void M4Board::setRom(const Bytes& image) {
    rom.assign(0x4000, 0xff);
    for (size_t i = 0; i < image.size() && i < rom.size(); i++) rom[i] = image[i];
}

void M4Board::setCard(const std::string& hostFolder) {
    flush();
    for (int fd = 1; fd < (int)fds.size(); fd++) fds[fd] = Fd{};
    sd.reset();
    storage.reset();
    if (!hostFolder.empty()) {
        storage = std::make_unique<M4Storage>(hostFolder);
        sd = std::make_unique<M4SdCard>(*storage);
    }
}

void M4Board::setEnabled(bool on) {
    if (enabled == on) return;
    if (!on) flush();
    enabled = on;
    powerOn();
}

void M4Board::powerOn() {
    cmd.clear();
    for (Fd& f : fds) f = Fd{};
    dirRows.clear(); dirShort.clear(); dirIndex = 0;
    if (storage) storage->cwd = "/";
}

void M4Board::reset() {
    // Only the half-sent command is lost; the ESP was not reset.
    cmd.clear();
    flush();
}

void M4Board::flush() {
    if (sd && sd->dirty()) {
        std::string report;
        if (sd->syncToHost(&report)) lastSync = report;
    }
}

void M4Board::poll() {
    // Half a second after the last sector written: long enough for a program to have
    // finished the FAT, the directory and the data of one save.
    if (sd && sd->dirty() && lastSdWrite >= 0 && now() - lastSdWrite > 2000000) flush();
}

void M4Board::rescan() {
    flush();
    if (storage) storage->touch();
}

void M4Board::dataPortWrite(int value) {
    if ((int)cmd.size() < CMD_MAX) cmd.push_back(value & 0xff);
}
int M4Board::dataPortRead() { return 0xff; }

int M4Board::readMemory(int address, int selectedRom) {
    if (!enabled || selectedRom != romSlot) return -1;
    return rom[(address - 0xc000) & 0x3fff];
}

void M4Board::resp8(int v) { if (respOff < RESP_SIZE) at(RESP_BASE + respOff) = (uint8_t)v; respOff += 1; }
void M4Board::resp16(int v) { resp8(v); resp8(v >> 8); }
void M4Board::resp32(uint32_t v) { for (int i = 0; i < 4; i++) resp8((int)(v >> (8 * i))); }
void M4Board::respStr(const std::string& s) { for (char c : s) resp8((unsigned char)c); resp8(0); }
void M4Board::respBytes(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) resp8(p[i]); }


int M4Board::commitFd(int fd) {
    Fd& f = fds[fd];
    if (!f.used || !f.dirty || !storage) return M4_FR_OK;
    f.dirty = false;
    return storage->writeFile(f.path, f.data);
}
void M4Board::closeFd(int fd) {
    if (fd < 1 || fd >= (int)fds.size()) return;
    commitFd(fd);
    fds[fd] = Fd{};
}

// FatFs f_open, as the ESP runs it.
int M4Board::openFile(int mode, const std::string& name, int& fdOut) {
    fdOut = 0xff;
    if (!storage) return M4_FR_NOT_READY;
    int fd = -1;
    if (mode & FA_REALMODE) {
        for (int i = 3; i < (int)fds.size(); i++) if (!fds[i].used) { fd = i; break; }
        if (fd < 0) return M4_FR_TOO_MANY_OPEN_FILES;
    } else {
        fd = (mode & FA_WRITE) ? 2 : 1;          // the AMSDOS input and output files
        closeFd(fd);
    }
    const bool write = mode & FA_WRITE, read = mode & FA_READ;
    const bool createNew = mode & FA_CREATE_NEW, createAlways = mode & FA_CREATE_ALWAYS, openAlways = mode & FA_OPEN_ALWAYS;
    std::string real = storage->find(name);
    // As AMSDOS: a name without a type is looked for as it is, then .BAS, then .BIN.
    const std::string leaf = M4Storage::leafOf(storage->absolute(name));
    if (real.empty() && !write && !createNew && !createAlways && !openAlways && leaf.find('.') == std::string::npos)
        for (const char* ext : { ".BAS", ".BIN" }) { real = storage->find(name + ext); if (!real.empty()) break; }
    if (!real.empty() && storage->isDir(real)) return M4_FR_NO_FILE;
    Fd f;
    if (!real.empty()) {
        if (createNew) return M4_FR_EXIST;
        M4Entry e;
        if ((write || createAlways) && storage->stat(real, e) && e.readOnly) return M4_FR_DENIED;
        if ((write || createAlways) && storage->readOnly) return M4_FR_WRITE_PROTECTED;
        f.path = real;
        if (createAlways) f.dirty = true;
        else if (storage->readFile(real, f.data) != M4_FR_OK) return M4_FR_DISK_ERR;
    } else {
        if (!(createNew || createAlways || openAlways)) {
            const std::string parent = storage->find(M4Storage::parentOf(storage->absolute(name)));
            return parent.empty() ? M4_FR_NO_PATH : M4_FR_NO_FILE;
        }
        if (storage->readOnly) return M4_FR_WRITE_PROTECTED;
        const int r = storage->resolveForCreate(name, f.path);
        if (r != M4_FR_OK) return r;
        if (storage->writeFile(f.path, {}) != M4_FR_OK) return M4_FR_DENIED;   // the entry exists from the open
    }
    f.used = true;
    f.canRead = read || !write;
    f.canWrite = write;
    fds[fd] = f;
    fdOut = fd;
    return M4_FR_OK;
}

static std::string asciiSizeK(uint64_t bytes) {
    char b[16];
    std::snprintf(b, sizeof b, "%4uK", (unsigned)((bytes + 1023) / 1024));
    return b;
}

bool M4Board::ack(Z80* cpu) {
    (void)cpu;
    if (!enabled) { cmd.clear(); return false; }
    // The packet: size (the bytes after it), command lo, hi, data.
    std::vector<int> packet = cmd;
    cmd.clear();
    if (packet.size() < 3) return false;
    // The ESP takes every byte sent since the last kick: the size byte is not to be
    // trusted -- the ROM's own C_READ2 (cas_in_char) sends it as 0.
    const int command = packet[1] | packet[2] << 8;
    std::vector<int> p(packet.begin() + 3, packet.end());
    auto str = [&](size_t from) { std::string s; for (size_t i = from; i < p.size() && p[i]; i++) s += (char)p[i]; return s; };
    auto secondStr = [&]() { size_t i = 0; while (i < p.size() && p[i]) i++; return str(i + 1); };
    auto fdArg = [&]() -> int { return p.empty() ? 0 : p[0]; };
    auto validFd = [&](int fd) { return fd >= 1 && fd < (int)fds.size() && fds[fd].used; };

    std::fill(rom.begin() + (RESP_BASE - 0xc000), rom.begin() + (RESP_BASE - 0xc000) + RESP_SIZE, 0);
    respOff = 3;
    int err = 0;
    const bool storageCommand = command != C_TIME && command != C_CONFIG && command != C_VERSION && command != C_SDREAD && command != C_SDWRITE;
    if (storageCommand) flush();                     // file commands see what raw writes did
    poll();

    switch (command) {
        case C_CONFIG:                               // data[0] = offset in the config area
            if (!p.empty()) for (size_t i = 1; i < p.size(); i++) at(0xf400 + ((p[0] + (int)i - 1) & 0xff)) = (uint8_t)p[i];
            break;
        case C_ROMWRITE: {                           // dest offset, size, rom (255 = hack menu, 0 = M4), data
            if (p.size() < 5) break;
            const int dest = p[0] | p[1] << 8, n = p[2] | p[3] << 8;
            Bytes& target = p[4] == 255 ? hackRom : rom;
            for (int i = 0; i < n && 5 + i < (int)p.size(); i++) target[(dest + i) & 0x3fff] = (uint8_t)p[5 + i];
            break;
        }
        case C_ROMCP: case C_ROMSUPDATE: case C_ROMLOW: case C_ROMSOFF: case C_RAMDISOFF: case C_NMIOFF:
        case C_M4OFF: case C_WIFIPOW: case C_SETNETWORK:
            break;
        case C_VERSION: respStr("M4 board v2.0.8 (CPCSyntaxError)"); break;
        case C_TIME: {
            const std::time_t t = std::time(nullptr);
            std::tm tmv{};
#if defined(_WIN32)
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char b[32];
            std::snprintf(b, sizeof b, "%02d:%02d:%02d %04d-%02d-%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
            respStr(b);
            break;
        }
        // ---------------------------------------------------------- files
        case C_OPEN: {
            int fd = 0xff;
            err = p.size() >= 2 ? openFile(p[0], str(1), fd) : M4_FR_INVALID_NAME;
            resp8(err ? 0xff : fd);
            resp8(err);
            break;
        }
        case C_READ: case C_READ2: {
            const int fd = fdArg();
            const int want = p.size() >= 3 ? (p[1] | p[2] << 8) : 0;
            if (!validFd(fd) || !fds[fd].canRead) {
                if (command == C_READ2) { resp8(validFd(fd) ? M4_FR_DENIED : M4_FR_INVALID_OBJECT); resp16(0); }
                else resp8(validFd(fd) ? M4_FR_DENIED : M4_FR_INVALID_OBJECT);
                break;
            }
            Fd& f = fds[fd];
            const size_t n = std::min<size_t>(std::min(want, 0x800), f.pos < f.data.size() ? f.data.size() - f.pos : 0);
            if (command == C_READ2) {
                // [3] 0, or 20 when the read came up short; [4..5] the count; data from [8]
                resp8(n < (size_t)want ? 20 : 0); resp16((int)n); resp16(0);
            } else {
                resp8(M4_FR_OK);
            }
            respBytes(f.data.data() + f.pos, n);
            f.pos += n;
            break;
        }
        case C_WRITE: case C_WRITE2: {
            const int fd = fdArg();
            if (!validFd(fd)) { resp8(M4_FR_INVALID_OBJECT); break; }
            Fd& f = fds[fd];
            if (!f.canWrite) { resp8(M4_FR_DENIED); break; }
            for (size_t i = 1; i < p.size(); i++) {
                if (f.pos >= f.data.size()) f.data.resize(f.pos + 1);
                f.data[f.pos++] = (uint8_t)p[i];
            }
            f.dirty = true;
            resp8(M4_FR_OK);
            break;
        }
        case C_CLOSE: {
            const int fd = fdArg();
            if (!validFd(fd)) { resp8(0xff); break; }  // the ROM's "nothing was open" (M4ROM.s fclose)
            err = commitFd(fd);
            fds[fd] = Fd{};
            resp8(err);
            break;
        }
        case C_SEEK: {
            const int fd = fdArg();
            if (!validFd(fd) || p.size() < 5) { resp8(M4_FR_INVALID_OBJECT); break; }
            Fd& f = fds[fd];
            size_t to = (size_t)((uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24);
            // A seek may grow a file, but not without bound: any program could otherwise
            // make the emulator allocate up to 4 GB with one command. 256 MB is far beyond
            // what a CPC can fill.
            static const size_t MAX_GROW = 256u << 20;
            if (to > f.data.size() && f.canWrite && to > MAX_GROW) { resp8(M4_FR_DENIED); break; }
            if (to > f.data.size()) {
                if (f.canWrite) { f.data.resize(to); f.dirty = true; }   // FatFs expands a file open for writing
                else to = f.data.size();
            }
            f.pos = to;
            resp8(M4_FR_OK);
            break;
        }
        case C_EOF: {
            const int fd = fdArg();
            if (!validFd(fd)) { resp8(0xff); break; }
            resp8(fds[fd].pos >= fds[fd].data.size() ? 1 : 0);
            break;
        }
        case C_FTELL: { const int fd = fdArg(); resp32(validFd(fd) ? (uint32_t)fds[fd].pos : 0); break; }
        case C_FSIZE: { const int fd = fdArg(); resp32(validFd(fd) ? (uint32_t)fds[fd].data.size() : 0); break; }
        // ---------------------------------------------------------- directories
        case C_CD: {
            const std::string target = storage ? storage->find(str(0).empty() ? "/" : str(0)) : "";
            if (!target.empty() && storage->isDir(target)) { storage->cwd = target; resp8(0); }
            else resp8(0xff);                        // M4ROM.s: 0xFF prints "unknown dir"
            break;
        }
        case C_GETPATH: respStr(storage ? storage->cwd : "/"); break;
        case C_MAKEDIR: {
            err = storage ? storage->makeDir(str(0)) : M4_FR_NOT_READY;
            resp8(err);
            if (err) respStr(m4ErrorText(err));
            break;
        }
        case C_DIRSETARGS: {                         // "folder/match": a folder, a pattern, or both
            dirRows.clear(); dirShort.clear(); dirIndex = 0;
            if (!storage) break;
            const std::string arg = str(0);
            std::string dir = storage->cwd, pattern = "*";
            if (!arg.empty()) {
                const std::string asDir = storage->find(arg);
                if (!asDir.empty() && storage->isDir(asDir)) dir = asDir;
                else {
                    const std::string abs = storage->absolute(arg);
                    dir = storage->find(M4Storage::parentOf(abs));
                    pattern = M4Storage::leafOf(abs);
                }
            }
            if (dir.empty()) break;
            const std::vector<M4Entry> all = storage->list(dir);
            const std::vector<std::string> shorts = M4Storage::shortNames(all);
            for (size_t i = 0; i < all.size(); i++) {
                const std::string s83 = shorts[i].substr(0, 8) + "." + shorts[i].substr(8);
                if (M4Storage::wildcardMatch(pattern, all[i].name) || M4Storage::wildcardMatch(pattern, s83)) {
                    dirRows.push_back(all[i]);
                    dirShort.push_back(shorts[i]);
                }
            }
            break;
        }
        case C_READDIR: {
            if (dirIndex >= dirRows.size()) break;   // an empty response ends the listing
            const M4Entry& e = dirRows[dirIndex];
            const std::string& s11 = dirShort[dirIndex];
            dirIndex += 1;
            if (!p.empty() && p[0] > 0) {
                // |LS: the long name (cut to the width given), then its size
                std::string name = (e.dir ? ">" : "") + e.name;
                if ((int)name.size() > p[0]) name = name.substr(0, p[0]);
                respStr(name);
                respStr(e.dir ? "" : asciiSizeK(e.size));
            } else {
                // CAT / |DIR: "NAME    .EXT" (high bits: ext[0] read-only), "  12K", 0, size in K
                std::string n8 = e.dir ? (">" + s11.substr(0, 7)) : s11.substr(0, 8);
                for (char c : n8) resp8((unsigned char)c);
                resp8('.');
                for (int i = 0; i < 3; i++) resp8((unsigned char)s11[8 + i] | (i == 0 && e.readOnly ? 0x80 : 0));
                const std::string sz = e.dir ? "  DIR" : asciiSizeK(e.size);
                for (char c : sz) resp8((unsigned char)c);
                resp8(0);
                resp16(e.dir ? 0 : (int)((e.size + 1023) / 1024));
            }
            break;
        }
        case C_FREE: {
            const uint64_t used = storage ? storage->usedBytes() : 0;
            const uint64_t capacity = sd && sd->capacityBytes() ? sd->capacityBytes() : std::max<uint64_t>(256ull << 20, used + (64ull << 20));
            respStr("\r\n" + std::to_string((unsigned long long)((capacity > used ? capacity - used : 0) / 1024)) + "K free\r\n\r\n");
            break;
        }
        case C_ERASEFILE: {                          // wildcards allowed
            if (!storage) { err = M4_FR_NOT_READY; }
            else {
                const std::string abs = storage->absolute(str(0));
                const std::string dir = storage->find(M4Storage::parentOf(abs));
                const std::string pattern = M4Storage::leafOf(abs);
                err = dir.empty() ? M4_FR_NO_PATH : M4_FR_NO_FILE;
                if (!dir.empty())
                    for (const M4Entry& e : storage->list(dir))
                        if (!e.dir && M4Storage::wildcardMatch(pattern, e.name)) {
                            err = storage->remove((dir == "/" ? "" : dir) + "/" + e.name);
                            if (err) break;
                        }
            }
            resp8(err);
            if (err) respStr(m4ErrorText(err));
            break;
        }
        case C_RENAME: {
            // The ROM sends |REN's arguments last-typed first: the old name, then the new.
            const std::string from = storage ? storage->find(str(0)) : "";
            err = !storage ? M4_FR_NOT_READY : from.empty() ? M4_FR_NO_FILE : storage->rename(from, secondStr());
            resp8(err);
            if (err) respStr(m4ErrorText(err));
            break;
        }
        case C_COPYFILE: {
            // |COPYF,"source","destination" arrives destination first.
            std::string dest = str(0), src = secondStr();
            const std::string from = storage ? storage->find(src) : "";
            Bytes data;
            if (!storage) err = M4_FR_NOT_READY;
            else if (from.empty() || !storage->isFile(from)) err = M4_FR_NO_FILE;
            else {
                const std::string destDir = storage->find(dest);
                if (!destDir.empty() && storage->isDir(destDir)) dest = destDir + "/" + M4Storage::leafOf(from);
                std::string to;
                err = storage->readFile(from, data);
                if (!err) err = storage->resolveForCreate(dest, to);
                if (!err) err = storage->writeFile(to, data);
            }
            resp8(err);
            if (err) respStr(m4ErrorText(err));
            break;
        }
        case C_FSTAT: {
            M4Entry e;
            const std::string real = storage ? storage->find(str(0)) : "";
            if (real.empty() || !storage->stat(real, e)) { resp8(M4_FR_NO_FILE); break; }
            resp8(0);
            resp32((uint32_t)e.size);
            resp16(e.fatDate); resp16(e.fatTime);
            resp8((e.dir ? 0x10 : 0x20) | (e.readOnly ? 0x01 : 0));
            const std::string parent = M4Storage::parentOf(real);
            std::string s11 = "           ";
            const std::vector<M4Entry> rows = storage->list(parent);
            const std::vector<std::string> shorts = M4Storage::shortNames(rows);
            for (size_t i = 0; i < rows.size(); i++) if (M4Storage::lower(rows[i].name) == M4Storage::lower(e.name)) s11 = shorts[i];
            std::string b = s11.substr(0, 8), x = s11.substr(8);
            while (!b.empty() && b.back() == ' ') b.pop_back();
            while (!x.empty() && x.back() == ' ') x.pop_back();
            std::string shortName = x.empty() ? b : b + "." + x;
            shortName.resize(13, '\0');
            for (char c : shortName) resp8((unsigned char)c);
            std::string longName = e.name;
            longName.resize(258, '\0');
            for (char c : longName) resp8((unsigned char)c);
            break;
        }
        // ---------------------------------------------------------- the raw card
        case C_SDREAD: {
            if (p.size() < 5) { resp8(4); break; }
            const uint32_t lba = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            const int n = p[4];
            if (n < 1 || n > 4) { resp8(4); break; }
            if (!sd) { resp8(3); break; }
            uint8_t buf[4 * 512];
            err = sd->read(lba, n, buf);
            resp8(err);
            if (!err) respBytes(buf, (size_t)n * 512);
            break;
        }
        case C_SDWRITE: {
            if (p.size() < 5) { resp8(4); break; }
            const uint32_t lba = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            const int n = p[4];
            if (n < 1 || n > 4 || (int)p.size() < 5 + n * 512) { resp8(4); break; }
            if (!sd) { resp8(3); break; }
            uint8_t buf[4 * 512];
            for (int i = 0; i < n * 512; i++) buf[i] = (uint8_t)p[5 + i];
            err = sd->write(lba, n, buf);
            if (!err) lastSdWrite = now();
            resp8(err);
            break;
        }
        case C_READSECTOR: case C_WRITESECTOR: case C_FORMATTRACK:
            resp8(M4_FR_NOT_READY);                  // no DSK image mounted on the board
            break;
        // ---------------------------------------------------------- network (no WiFi here)
        case C_NETSTAT: respStr("WiFi not connected (emulator)"); resp8(0); break;
        case C_NETRSSI: resp8(31); break;            // m4info: 31 = fail
        case C_GETNETWORK: for (int i = 0; i < 196; i++) resp8(0); break;
        case C_NETSOCKET: resp8(0xff); break;
        case C_HTTPGET: respStr("No network in the emulator"); break;
        case C_HTTPGETMEM: resp16(0); break;
        case C_UPGRADE: respStr("No network in the emulator"); break;
        case C_ROMLIST: break;
        default:
            if (command >= 0x4332 && command <= 0x433a) { resp8(0xff); break; }   // socket calls: error
            break;
    }

    static const bool trace = std::getenv("CPCSE_TRACE_M4") != nullptr;
    if (trace) {
        std::fprintf(stderr, "M4 %04x resp=%d p=[", command, respOff - 3);
        for (size_t i = 0; i < p.size() && i < 24; i++) std::fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
        std::fprintf(stderr, "%s] \"", p.size() > 24 ? " ..." : "");
        for (size_t i = 0; i < p.size() && i < 40; i++) std::fputc(p[i] >= 32 && p[i] < 127 ? p[i] : '.', stderr);
        std::fprintf(stderr, "\" -> [");
        for (int i = 3; i < respOff && i < 19; i++) std::fprintf(stderr, "%s%02x", i > 3 ? " " : "", at(RESP_BASE + i));
        std::fprintf(stderr, "]\n");
    }
    // rom_response: [0] the size after it, [1..2] the command
    at(RESP_BASE) = (uint8_t)(respOff - 1);
    at(RESP_BASE + 1) = (uint8_t)command;
    at(RESP_BASE + 2) = (uint8_t)(command >> 8);
    return false;
}

} // namespace cpcse
