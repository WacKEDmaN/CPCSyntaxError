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
    C_ROMLIST = 0x432c, C_DSKEXT = 0x4330, C_NETSOCKET = 0x4331, C_NETRSSI = 0x4337, C_GETNETWORK = 0x433b, C_WIFIPOW = 0x433c,
    C_ROMLOW = 0x433d, C_ROMCP = 0x43fc, C_ROMWRITE = 0x43fd, C_CONFIG = 0x43fe;
// ff.i
static const int FA_READ = 1, FA_WRITE = 2, FA_CREATE_NEW = 4, FA_CREATE_ALWAYS = 8, FA_OPEN_ALWAYS = 16, FA_REALMODE = 128;

M4Board::M4Board(GX4000* emulator) : emulator(emulator) {
    rom.assign(0x4000, 0xff);
    hackRom.assign(0x4000, 0xff);
    net = std::make_unique<M4Network>();
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
    dsk.reset();
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
    flushDsk();
    dsk.reset();
    extractKey.clear(); extractQueue.clear(); extractIndex = 0;
    net->closeAll();
    net->wifiOn = true;
    heldFor = 0;
    httpBuffer.clear();
}

void M4Board::reset() {
    // Only the half-sent command is lost; the ESP was not reset. A download the Z80 was
    // held for finishes unread.
    cmd.clear();
    heldFor = 0;
    flush();
}

void M4Board::netTick() {
    if (!enabled) return;
    // Sockets every emulated millisecond; a download the Z80 waits on, every call.
    if (!heldFor && now() - lastNetTick < 4000 && now() >= lastNetTick) return;
    lastNetTick = now();
    net->poll();
    M4Network::HttpResult r;
    if (net->httpTake(r) && heldFor) completeDownload(r);
    net->sockInfo(&at(0xfe00));
}

void M4Board::completeDownload(M4Network::HttpResult& r) {
    const int command = heldFor;
    heldFor = 0;
    std::fill(rom.begin() + (RESP_BASE - 0xc000), rom.begin() + (RESP_BASE - 0xc000) + RESP_SIZE, 0);
    respOff = 3;
    if (command == C_HTTPGETMEM) {
        // m4info: data[0..1] = downloaded size, into the internal buffer.
        httpBuffer = r.ok ? r.body : Bytes{};
        resp16((int)httpBuffer.size());
    } else {
        std::string text;
        if (!r.ok) text = r.error;
        else if (!storage) text = "No SD card";
        else {
            // ">name" first, then the attachment filename, then the URL's own (m4info v2.0.x).
            std::string fileName = getTarget;
            if (fileName.empty()) {
                // The server's name is only a name: never a path out of the current folder.
                std::string served = r.fileName;
                for (char& c : served) if (c == '\\') c = '/';
                fileName = M4Storage::leafOf("/" + served);
                if (fileName == "." || fileName == "..") fileName.clear();
            }
            if (fileName.empty()) {
                std::string host, path; int port = 80;
                if (M4Network::splitUrl(r.request, host, port, path)) fileName = M4Storage::leafOf(path.substr(0, path.find('?')));
            }
            if (fileName.empty()) fileName = "INDEX.HTM";
            std::string to;
            int err = storage->readOnly ? M4_FR_WRITE_PROTECTED : storage->resolveForCreate(fileName, to);
            if (!err) err = storage->writeFile(to, r.body);
            text = err ? std::string("Can't save ") + fileName + ": " + m4ErrorText(err)
                       : "Downloaded " + fileName + " (" + std::to_string(r.body.size()) + " bytes)";
        }
        respStr(silentGet ? "" : "\r\n" + text + "\r\n");
    }
    finishResponse(command, {});
}

void M4Board::flush() {
    if (sd && sd->dirty()) {
        std::string report;
        if (sd->syncToHost(&report)) lastSync = report;
    }
    flushDsk();
}

// ------------------------------------------------------------------ a .dsk as a folder
bool M4Board::mountDsk(const std::string& realPath) {
    if (!storage) return false;
    Bytes image;
    if (storage->readFile(realPath, image) != M4_FR_OK) return false;
    std::shared_ptr<Disk> disk;
    try { disk = parseDsk(image); } catch (const std::exception&) { return false; }
    if (!disk) return false;
    const std::optional<DskGeometry> g = dskDetect(*disk);
    if (!g) return false;                            // not a format with a CP/M directory
    flushDsk();
    MountedDsk m;
    m.real = realPath;
    m.disk = disk;
    m.geometry = *g;
    dsk = m;
    return true;
}

void M4Board::flushDsk() {
    if (!dsk || !dsk->dirty || !storage) return;
    dsk->dirty = false;
    storage->writeFile(dsk->real, serializeDsk(*dsk->disk, !dsk->disk->extended && dskFitsStandard(*dsk->disk)));
}

void M4Board::unmountDsk() {
    flushDsk();
    dsk.reset();
}

std::vector<DskFsFile> M4Board::dskFiles(bool listing) const {
    std::vector<DskFsFile> out;
    if (!dsk) return out;
    try {
        DskFs fs(dsk->disk, dsk->geometry);
        // m4info v2.0.x: "Do not show files with system attribute set" -- in a listing; a
        // program still opens them by name (Chany's Dream 2 loads its system file DREAM2.CH1)
        for (DskFsFile& f : fs.files()) if (!listing || !f.system) out.push_back(f);
    } catch (const std::exception&) {}
    return out;
}

const DskFsFile* M4Board::dskFind(const std::vector<DskFsFile>& files, const std::string& nameIn) const {
    std::string name = M4Storage::leafOf("/" + nameIn);
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    const std::string want = M4Storage::lower(name);
    for (const DskFsFile& f : files) if (M4Storage::lower(f.displayName()) == want) return &f;
    // AMSDOS: a name without a type is looked for as it is, then .BAS, then .BIN
    if (name.find('.') == std::string::npos)
        for (const char* ext : { ".bas", ".bin" })
            for (const DskFsFile& f : files) if (M4Storage::lower(f.displayName()) == want + ext) return &f;
    return nullptr;
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
    if (dsk) {
        // Inside a .dsk: its files are read-only (m4info: "DSK files are read only for now").
        if (mode & (FA_WRITE | FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)) return M4_FR_WRITE_PROTECTED;
        const std::vector<DskFsFile> files = dskFiles();
        const DskFsFile* f = dskFind(files, name);
        if (!f) return M4_FR_NO_FILE;
        int fd = 1;
        if (mode & FA_REALMODE) {
            fd = -1;
            for (int i = 3; i < (int)fds.size(); i++) if (!fds[i].used) { fd = i; break; }
            if (fd < 0) return M4_FR_TOO_MANY_OPEN_FILES;
        } else closeFd(1);
        Fd o;
        try { o.data = DskFs(dsk->disk, dsk->geometry).read(*f); } catch (const std::exception&) { return M4_FR_DISK_ERR; }
        // A file with an AMSDOS header ends where the header says, not at its last record.
        if (f->header) {
            const size_t length = (size_t)(f->header->fullLength ? f->header->fullLength : f->header->logicalLength) + 128;
            if (length <= o.data.size()) o.data.resize(length);
        }
        o.used = true;
        o.canRead = true;
        o.path = dsk->real + "/" + f->displayName();
        fds[fd] = o;
        fdOut = fd;
        return M4_FR_OK;
    }
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

// The directory commands while a .dsk is the current folder. false: not one of them (or
// it left the image), and the card's own handling goes on.
bool M4Board::dskCommand(int command, const std::vector<int>& p) {
    auto str = [&](size_t from) { std::string s; for (size_t i = from; i < p.size() && p[i]; i++) s += (char)p[i]; return s; };
    const std::string arg = str(0);
    auto s11 = [](const DskFsFile& f) {
        std::string n = f.name.substr(0, 8), x = f.ext.substr(0, 3);
        n.resize(8, ' ');
        x.resize(3, ' ');
        return n + x;
    };
    auto fileSize = [](const DskFsFile& f) -> uint64_t {
        if (f.header) return (uint64_t)(f.header->fullLength ? f.header->fullLength : f.header->logicalLength) + 128;
        return (uint64_t)f.records * 128;
    };
    switch (command) {
        case C_CD:
            if (arg == "..") { unmountDsk(); resp8(0); return true; }   // back to the folder it is in
            unmountDsk();                            // "/" or anything else: from the image's folder
            return false;
        case C_GETPATH: {
            const std::string folder = storage ? storage->cwd : "/";
            respStr((folder == "/" ? "" : folder) + "/" + M4Storage::leafOf(dsk->real));
            return true;
        }
        case C_DIRSETARGS: {
            dirRows.clear(); dirShort.clear(); dirIndex = 0;
            const std::string pattern = arg.empty() ? std::string("*") : M4Storage::leafOf("/" + arg);
            for (const DskFsFile& f : dskFiles(true)) {
                if (!M4Storage::wildcardMatch(pattern, f.displayName())) continue;
                M4Entry e;
                e.name = f.displayName();
                e.size = fileSize(f);
                e.readOnly = f.readOnly;
                dirRows.push_back(e);
                dirShort.push_back(s11(f));
            }
            return true;
        }
        case C_FREE: {
            int freeK = 0;
            try { DskFs fs(dsk->disk, dsk->geometry); freeK = fs.freeBlocks() * dsk->geometry.blockSize / 1024; } catch (const std::exception&) {}
            respStr("\r\n" + std::to_string(freeK) + "K free\r\n\r\n");
            return true;
        }
        case C_MAKEDIR: case C_ERASEFILE: case C_RENAME: case C_COPYFILE:
            resp8(M4_FR_WRITE_PROTECTED);            // the image's files are read-only
            respStr(m4ErrorText(M4_FR_WRITE_PROTECTED));
            return true;
        case C_FSTAT: {
            const std::vector<DskFsFile> files = dskFiles();
            const DskFsFile* f = dskFind(files, arg);
            if (!f) { resp8(M4_FR_NO_FILE); return true; }
            resp8(0);
            resp32((uint32_t)fileSize(*f));
            resp16(0); resp16(0);
            resp8(0x20 | (f->readOnly ? 0x01 : 0));
            std::string shortName = f->displayName();
            shortName.resize(13, '\0');
            for (char c : shortName) resp8((unsigned char)c);
            std::string longName = f->displayName();
            longName.resize(258, '\0');
            for (char c : longName) resp8((unsigned char)c);
            return true;
        }
        default:
            return false;
    }
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
    const bool networkCommand = (command >= C_NETSOCKET && command <= C_WIFIPOW) || command == C_NETSTAT ||
                                command == C_SETNETWORK || command == C_HTTPGETMEM || command == C_COPYBUF;
    const bool storageCommand = command != C_TIME && command != C_CONFIG && command != C_VERSION && command != C_SDREAD &&
                                command != C_SDWRITE && !networkCommand;
    if (storageCommand) flush();                     // file commands see what raw writes did
    poll();

    if (dsk && dskCommand(command, p)) { finishResponse(command, p); return false; }
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
        case C_M4OFF:
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
            else if (!target.empty() && storage->isFile(target) && M4Storage::lower(target.substr(target.size() >= 4 ? target.size() - 4 : 0)) == ".dsk" &&
                     mountDsk(target)) {
                storage->cwd = M4Storage::parentOf(target);   // the image is a folder inside its own
                resp8(0);
            } else resp8(0xff);                      // M4ROM.s: 0xFF prints "unknown dir"
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
        case C_READSECTOR: case C_WRITESECTOR: {
            // The ROM's BIOS read/write-sector replacements (M4ROM.s read_sector,
            // write_sector): data[0] track, data[1] sector ID, data[2] drive (unused);
            // a read answers [3] the result and the sector from [4].
            if (!dsk) { resp8(M4_FR_NOT_READY); break; }
            if (p.size() < 3) { resp8(M4_FR_INVALID_PARAMETER); break; }
            Sector* s = dskFindSector(*dsk->disk, p[0], 0, p[1]);
            if (!s && dsk->geometry.sides > 1) s = dskFindSector(*dsk->disk, p[0], 1, p[1]);
            if (!s || s->data.empty()) { resp8(M4_FR_DISK_ERR); break; }
            Bytes& data = s->data[0];
            if (command == C_READSECTOR) {
                resp8(M4_FR_OK);
                for (int i = 0; i < 512; i++) resp8(i < (int)data.size() ? data[i] : 0xe5);
            } else {
                // The board does not write to its DSK images (the user, 2026-10-09; m4info:
                // "DSK files are read only"): refused, the image as it was.
                resp8(M4_FR_WRITE_PROTECTED);
            }
            break;
        }
        case C_FORMATTRACK:
            resp8(M4_FR_NOT_READY);                  // m4info: "Not implemented yet"
            break;
        case C_DSKEXT: {
            // |DSKX,"disc.dsk","/path": the arguments arrive last-typed first (the path,
            // then the image). Each call extracts one file: [3] 0 = more to come, then the
            // text the ROM prints from [4]; [3] 1 ends the loop (M4ROM.s dsk_extract).
            const std::string key = str(0) + "\n" + secondStr();
            if (key != extractKey) {
                extractKey = key;
                extractQueue.clear();
                extractIndex = 0;
                const std::string image = storage ? storage->find(secondStr()) : "";
                std::optional<MountedDsk> keep = dsk;
                dsk.reset();
                if (!image.empty() && mountDsk(image)) {
                    for (const DskFsFile& f : dskFiles()) {
                        Bytes data;
                        try { data = DskFs(dsk->disk, dsk->geometry).read(f); } catch (const std::exception&) { continue; }
                        if (f.header) {
                            const size_t length = (size_t)(f.header->fullLength ? f.header->fullLength : f.header->logicalLength) + 128;
                            if (length <= data.size()) data.resize(length);
                        }
                        extractQueue.push_back({ f.displayName(), data });
                    }
                }
                dsk = keep;
                if (extractQueue.empty()) { extractKey.clear(); resp8(1); respStr("\r\nNo such DSK image\r\n"); break; }
            }
            if (extractIndex >= extractQueue.size()) { extractKey.clear(); resp8(1); respStr("Done.\r\n"); break; }
            const auto& [name, data] = extractQueue[extractIndex++];
            std::string to;
            const std::string dest = str(0).empty() ? std::string(".") : str(0);
            const std::string destDir = storage ? storage->find(dest) : "";
            err = !storage ? M4_FR_NOT_READY : destDir.empty() || !storage->isDir(destDir) ? M4_FR_NO_PATH
                  : storage->resolveForCreate((destDir == "/" ? "" : destDir) + "/" + name, to);
            if (!err) err = storage->writeFile(to, data);
            if (err == M4_FR_NO_PATH) { extractKey.clear(); resp8(1); respStr("\r\nNo such folder\r\n"); break; }
            resp8(0);
            respStr(err ? name + ": " + m4ErrorText(err) + "\r\n" : name + "\r\n");
            break;
        }
        // ---------------------------------------------------------- network (the host's)
        case C_HTTPGET: {
            // "[@]host[:port]/file[>name]": @ = print nothing, >name = save as (m4info v2.0.x)
            std::string url = str(0);
            silentGet = !url.empty() && url[0] == '@';
            if (silentGet) url.erase(url.begin());
            getTarget.clear();
            const size_t gt = url.rfind('>');
            if (gt != std::string::npos) { getTarget = url.substr(gt + 1); url = url.substr(0, gt); }
            while (!getTarget.empty() && getTarget.front() == ' ') getTarget.erase(getTarget.begin());
            net->startHttp(url, 0, 64u << 20);
            heldFor = command;
            return false;
        }
        case C_HTTPGETMEM: {
            // data[0..1] = size (at most the 16K buffer), data[2] = "url[, offset=n]"
            const size_t size = std::min<size_t>(p.size() >= 2 ? (size_t)(p[0] | p[1] << 8) : 0, 0x4000);
            std::string url = str(2);
            size_t offset = 0;
            const size_t o = M4Storage::lower(url).find("offset=");
            if (o != std::string::npos) {
                std::string n = url.substr(o + 7);
                while (!n.empty() && n.front() == ' ') n.erase(n.begin());
                int base = 10;
                if (n.size() > 2 && n[0] == '0' && (n[1] == 'x' || n[1] == 'X')) { n = n.substr(2); base = 16; }
                else if (!n.empty() && (n[0] == '&' || n[0] == '#' || n[0] == '$')) { n = n.substr(1); base = 16; }
                offset = (size_t)std::strtoull(n.c_str(), nullptr, base) & 0xffffffffu;
                url = url.substr(0, o);
                while (!url.empty() && (url.back() == ' ' || url.back() == ',')) url.pop_back();
            }
            if (size == 0) { httpBuffer.clear(); resp16(0); break; }
            net->startHttp(url, offset, size);
            heldFor = command;
            return false;
        }
        case C_COPYBUF: {                            // data[0..1] offset, data[2..3] size
            const size_t from = p.size() >= 2 ? (size_t)(p[0] | p[1] << 8) : 0;
            const size_t n = std::min<size_t>(p.size() >= 4 ? (size_t)(p[2] | p[3] << 8) : 0, RESP_SIZE - 3);
            for (size_t i = 0; i < n; i++) resp8(from + i < httpBuffer.size() ? httpBuffer[from + i] : 0);
            break;
        }
        case C_UPGRADE: respStr("\r\nThe emulator's M4 firmware is built in: nothing to upgrade.\r\n"); break;
        case C_ROMLIST: break;
        default: {
            std::vector<uint8_t> out;
            if (net->command(command, p, out)) respBytes(out.data(), out.size());
            break;
        }
    }
    finishResponse(command, p);
    return false;
}

void M4Board::finishResponse(int command, const std::vector<int>& p) {
    net->sockInfo(&at(0xfe00));
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
}

} // namespace cpcse
