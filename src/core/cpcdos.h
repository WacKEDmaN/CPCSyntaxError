// CPCSyntaxError — CPCDOS virtual-drive bridge.
// The server-backed CpcDosDrive lives in the app layer; the emulator core
// constructs CpcDos with drive == null, so the bridge is inert without one.
#pragma once
#include "common.h"

namespace cpcse {

class GX4000;
class Z80;

struct CpcDosFile { std::string path; std::string name; Bytes data; int size = 0; };
struct CpcDosRow { std::string kind; std::string path; std::string name; int size = 0; bool dot = false; };

// Abstract drive interface (methods CpcDos calls). Concrete impl is app-layer.
struct CpcDosDrive {
    std::string cwd = "/";
    bool readOnly = false;
    virtual ~CpcDosDrive() = default;
    virtual void noteActivity(int ms) { (void)ms; }
    virtual CpcDosFile* find(const std::string& name) { (void)name; return nullptr; }
    virtual void addFile(const std::string& name, const Bytes& data) { (void)name; (void)data; }
    virtual std::vector<CpcDosFile*> matchFiles(const std::string& pattern) { (void)pattern; return {}; }
    virtual void deletePath(const std::string& path) { (void)path; }
    virtual bool rename(const std::string& path, const std::string& newName) { (void)path; (void)newName; return false; }
    virtual void chdir(const std::string& dir) { (void)dir; }
    virtual std::vector<CpcDosRow> list(const std::string& cwd) { (void)cwd; return {}; }
};

class CpcDos {
public:
    GX4000* emulator;
    CpcDosDrive* drive = nullptr;
    bool enabled = false;
    bool readOpen = false, writeOpen = false;
    int readPos = 0;
    Bytes readHead;
    Bytes readStream;
    std::vector<int> writeBuffer;
    int writeStart = 0;
    std::string writeName;
    bool writeAscii = false;
    int fWhere = 0;
    int storeIY = 0;
    bool eof = false;

    explicit CpcDos(GX4000* emulator, CpcDosDrive* drive = nullptr);
    void setDrive(CpcDosDrive* drive) { this->drive = drive; }
    void setEnabled(bool enabled) { this->enabled = enabled; }
    void reset();
    bool handleEdff(Z80* cpu);
    int memRead(int address);
    int memReadRam(int address);
    void memWrite(int address, int value);
    std::string readCpcString(int address, int length, bool keepSpaces = false);
    void flushRom();
    void writeRom(const Bytes& bytes);
    void fString(const std::string& text);
    void execute(Z80* cpu);
    std::vector<CpcDosRow> hiddenCat();
    Bytes makeFakeHeader(const Bytes& data, int start = 0, const std::string& name = "CPCSE.BIN");
    void fOpenIn(Z80* cpu);
    void fOpenOut(Z80* cpu);
    void fCloseIn(Z80* cpu);
    void fCloseOut(Z80* cpu);
    void fRead(Z80* cpu, int hl, int bc);
    void fWrite(Z80* cpu, int hl, int de);
    void fErase(Z80* cpu);
    void fRename(Z80* cpu);
    std::string virtualDosHeader();
    std::vector<CpcDosRow> directoryRows();
    std::string catFileField(const CpcDosRow& row, const std::vector<CpcDosRow>& rows, int index);
    std::string dirFileField(const CpcDosRow& row);
    std::string sizeTextForRow(const CpcDosRow& row);
    int screenMode();
    void fGetCat(Z80* cpu);
    void fGetDir(Z80* cpu);
    void fChDir(Z80* cpu);
};

} // namespace cpcse
