// CPCSyntaxError — the M4 Board (Duke, spinpoint.org).
//
// What the CPC sees of it, from its own Z80 ROM source (github.com/M4Duke/m4rom) and the
// command reference (spinpoint.org/cpc/m4info.txt):
//   - a 16K upper ROM (slot 6) that is really RAM the board's ESP8266 writes into: the
//     Z80 code, the response area at &E800, the config area at &F400 and the socket
//     status at &FE00 (the link table at &FF00 points at them);
//   - a command sent byte by byte to &FExx (size, command lo, hi, data...) and started
//     by any write to &FCxx; the board holds the Z80 (READY) until the response is in
//     the ROM, so it is there by the next instruction;
//   - the ESP's FatFs over a microSD card -- here a host folder (m4_storage.h) -- both
//     as files and as raw sectors.
// The ESP keeps running through a CPC reset: open files, current directory and config
// area survive it ("config area, reset only by power cycle"), and so do open sockets.
//
// The WiFi is the host's own network (m4_net.h). The two commands that wait for the
// internet -- C_HTTPGET and C_HTTPGETMEM -- hold the Z80 until the download is done,
// as the board does: the ROM reads their answer the instruction after the kick.
//
// A .dsk on the card is a folder too (m4info: "|cd,"robocop.dsk" and it takes you inside
// the DSK file as it was a folder"): while one is mounted the file commands list and read
// its CP/M files (read-only, as the M4's are), and the ROM's BIOS read/write-sector
// replacements reach its sectors by track and sector ID (C_READSECTOR / C_WRITESECTOR),
// written back into the .dsk on the card.
#pragma once
#include "common.h"
#include "m4_storage.h"
#include "m4_net.h"
#include "dsk_edit.h"

namespace cpcse {

class GX4000;
class Z80;

class M4Board {
public:
    explicit M4Board(GX4000* emulator);
    ~M4Board();
    GX4000* emulator;
    bool enabled = false;
    int romSlot = 6;
    Bytes rom;                       // the board's 16K ROM image (the ESP writes into it)
    Bytes hackRom;                   // the NMI hack-menu ROM (C_ROMWRITE rom 255)
    std::unique_ptr<M4Storage> storage;
    std::unique_ptr<M4SdCard> sd;
    std::vector<int> cmd;            // the command being sent
    std::string lastSync;            // what the last write-back of the raw card changed

    void setRom(const Bytes& image);
    void setCard(const std::string& hostFolder);   // "" = no card
    void setEnabled(bool on);
    void powerOn();                  // the ESP starts afresh
    void reset();                    // a CPC reset: the ESP carries on
    void dataPortWrite(int value);
    int dataPortRead();
    int readMemory(int address, int selectedRom);  // -1 = not the M4's
    bool ack(Z80* cpu = nullptr);
    // Writes raw-sector changes back to the folder once the card has been idle; flush()
    // does it at once (reset, power off, exit).
    void poll();
    void flush();
    void rescan();                   // the folder changed from outside

    std::unique_ptr<M4Network> net;
    // A download the Z80 is held for (C_HTTPGET / C_HTTPGETMEM), 0 when none.
    int heldFor = 0;
    bool holdsCpu() const { return heldFor != 0; }
    bool networkActive() const { return enabled && (heldFor || net->active()); }
    // Called as the machine runs: moves socket data, ends a download. Cheap between
    // its millisecond steps.
    void netTick();

    struct Fd {
        bool used = false, canRead = false, canWrite = false, dirty = false;
        std::string path;            // real path on the card
        Bytes data;
        size_t pos = 0;
    };
    std::array<Fd, 13> fds;          // 1-2 AMSDOS in/out, 3-12 FA_REALMODE

    // The .dsk the current directory is inside, if any.
    struct MountedDsk {
        std::string real;            // its path on the card
        std::shared_ptr<Disk> disk;
        DskGeometry geometry;
        bool dirty = false;          // sectors written, not yet back in the .dsk
    };
    std::optional<MountedDsk> dsk;
    bool mountDsk(const std::string& realPath);
    void unmountDsk();

private:
    std::vector<M4Entry> dirRows;
    std::vector<std::string> dirShort;
    size_t dirIndex = 0;
    long long lastSdWrite = -1;
    long long lastNetTick = 0;
    int respOff = 3;
    bool silentGet = false;          // |HTTPGET,"@..." prints nothing
    std::string getTarget;           // |HTTPGET,"...>name"
    Bytes httpBuffer;                // C_HTTPGETMEM's internal buffer, read by C_COPYBUF
    void finishResponse(int command, const std::vector<int>& p);
    std::vector<DskFsFile> dskFiles(bool listing = false) const;   // the mounted image's files (a listing hides system files)
    const DskFsFile* dskFind(const std::vector<DskFsFile>& files, const std::string& name) const;
    void flushDsk();
    bool dskCommand(int command, const std::vector<int>& p);
    // |DSKX: the files still to extract, one a call
    std::string extractKey;
    std::vector<std::pair<std::string, Bytes>> extractQueue;
    size_t extractIndex = 0;
    void completeDownload(M4Network::HttpResult& r);

    void resp8(int v);
    void resp16(int v);
    void resp32(uint32_t v);
    void respStr(const std::string& s);
    void respBytes(const uint8_t* p, size_t n);
    uint8_t& at(int address) { return rom[(address - 0xc000) & 0x3fff]; }
    void closeFd(int fd);
    int commitFd(int fd);
    int openFile(int mode, const std::string& name, int& fdOut);
    long long now() const;
};

} // namespace cpcse
