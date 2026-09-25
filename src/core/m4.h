// CPCSyntaxError — M4 Board storage device.
// M4 commands, ROM helper memory and coherent FAT16 RAW storage.
// The virtual HDD "drive" is an app-layer construct; the emulator core
// constructs M4Board with drive == null, so every drive path is null-guarded.
#pragma once
#include "common.h"

namespace cpcse {

class GX4000;
class Z80;

struct M4File { std::string path; std::string name; Bytes data; int size = 0; };
struct M4DriveRow { std::string name; std::string kind; int size = 0; };

// Abstract virtual-drive interface. Concrete implementations live in the app
// layer; the emulator core keeps a null pointer and never calls these.
struct M4Drive {
    bool readOnly = false;
    long long limitBytes = 16 * 1024 * 1024;
    std::string cwd = "/";
    std::unordered_map<std::string, M4File> files;
    std::unordered_set<std::string> dirs{ "/" };
    virtual ~M4Drive() = default;
    virtual std::vector<M4DriveRow> list(const std::string& path) { (void)path; return {}; }
    virtual M4File* find(const std::string& name) { (void)name; return nullptr; }
    virtual void chdir(const std::string& path) { (void)path; }
    virtual void addFile(const std::string& path, const Bytes& data) { (void)path; (void)data; }
    virtual void deletePath(const std::string& path) { (void)path; }
    virtual bool rename(const std::string& oldName, const std::string& newName) { (void)oldName; (void)newName; return false; }
    virtual void ensureDir(const std::string& path) { (void)path; }
    virtual void emitChange() {}
    virtual void noteActivity(int ms) { (void)ms; }
    virtual bool csrfProvider() { return false; }
};

// Parsed FAT16 view: dirs set + files map.
struct M4Fat16View {
    std::unordered_set<std::string> dirs;
    std::unordered_map<std::string, M4File> files;
};
M4Fat16View parseM4Fat16Image(const Bytes& image);

struct M4Fd { bool inUse = false; std::vector<int> data; int pos = 0; bool write = false; std::string path; };

class M4Board {
public:
    GX4000* emulator;
    M4Drive* drive = nullptr;
    bool syncingSdToDrive = false;
    bool enabled = false;
    Bytes romConfigSeed;
    Bytes busMem, cfgMem, sockMem;
    std::vector<int> cmd;
    std::array<M4Fd, 8> fds;
    std::vector<M4DriveRow> dirList;
    int dirIndex = 0;
    std::string dirFilter = "*";
    bool nmiEnabled = false;
    int lastError = 0;
    bool ramMode = false;
    int initCount = 0;
    Bytes sdImage;      // empty == null
    bool sdDirty = false;

    explicit M4Board(GX4000* emulator, M4Drive* drive = nullptr);
    void setDrive(M4Drive* drive);
    bool syncDriveFromSdImage(bool persist = false);
    void scheduleSdSync();
    void setEnabled(bool enabled);
    void loadRomDefaults(Bytes& romBytes);
    void patchHelperTable(Bytes& romBytes);
    void installHelperShim();
    void reset();
    void dataPortWrite(int value);
    int dataPortRead();
    int readMemory(int address, int selectedRom); // -1 == null
    int readMemory(int address);

    struct RespCtx { int off; };
    void resp8(RespCtx& ctx, int value);
    void resp16(RespCtx& ctx, int value);
    void resp32(RespCtx& ctx, int value);
    void respStr(RespCtx& ctx, const std::string& text);
    void frame(int command, int off);
    bool validFd(int fd);
    void closeFd(int fd);
    int allocFd(int mode);
    std::vector<M4DriveRow> driveRows();
    M4File* resolveFile(const std::string& name);
    void restoreRamConfig(int config, int segment);
    void selectRamConfigFromBank(int bank);
    int readHelperByte(int address);
    void writeHelperByte(int address, int value);
    bool helperTrap(int lowByte, Z80* cpu);
    Bytes& ensureSdImage();
    bool ack(Z80* cpu = nullptr);
};

} // namespace cpcse
