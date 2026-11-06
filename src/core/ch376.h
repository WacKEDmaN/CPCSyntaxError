// CPCSyntaxError: the Albireo board: a WCH CH376 on the CPC's I/O bus.
//
// The board (PulkoMandy's Albireo programmer's manual, pulkomandy.github.io/shinra.github.io/
// albireo.html; CPCWiki "CPC CH376 USB"): the CH376 at &FE80 (data, read/write) and &FE81
// (command write, status read), fully decoded, no mirrors. Albireo 2.0 has no UART (the
// 1.x serial chip at &FEB0-&FEB7 is not fitted) and two solder jumpers move the CH376 to
// &FE40/&FE41, so two boards can share one CPC; SymbOS uses one for USB storage and the
// other for a USB mouse. The wiki says it is "unclear" how 2.0 treats interrupts, so no
// interrupt line is wired here: software polls the status register, as the manual's own
// examples do.
//
// The chip (CH376DS1.PDF "Datasheet (I)" and CH376DS2.PDF "Datasheet (II)", WCH):
//   * Parallel port, §6.2: a write with A0=1 is a command, A0=0 data; a read with A0=1 is
//     the status: bit 7 the interrupt flag, active low (INT#), bit 4 busy.
//   * A command takes its input bytes on the data port and gives its output bytes there.
//     Commands marked "generate interrupt" end with INT# low; GET_STATUS (&22) reads the
//     interrupt status and cancels the request (§5.10).
//   * RD_USB_DATA0 (&27) gives a length, then that many bytes (0-255, §5.11).
//   * SET_USB_MODE (&15, §5.9): 3 the SD card, 5-7 the USB host port (7 resets the bus).
//
// What this model holds on each port:
//   SD card    a host folder, as one FAT16 volume (the M4's code: M4Storage for files,
//              M4SdCard for raw sectors; both views of one folder).
//   USB port   nothing, a USB stick (another folder, the same way), or a USB mouse (a HID
//              boot mouse with a wheel: its descriptors, SET_ADDRESS / SET_CONFIGURATION /
//              SET_PROTOCOL, and reports on its interrupt endpoint 1).
//
// The file system commands are the CH376's own (§5.15-§5.35): names are DOS 8.3 short names
// in upper case, a path is opened one level at a time from "/", '*' at the end of a name
// enumerates. Sector commands (DISK_READ / DISK_WRITE, §5.37-§5.40) move 64-byte blocks, 8
// to a sector. The datasheet says these and SEC_* do "not support SD card"; the Albireo
// manual lists raw sector access for the SD card as well ("Raw sector access also possible
// for homegrown file-systems"), so here they work on both media.
//
// Timing: every command is done when it is written (the CH376 answers in microseconds; a
// Z80 OUT/IN pair takes longer), except RESET_ALL, which holds the chip busy for 1 ms (§5.4:
// "In the parallel communication mode, hardware reset is generally completed within 1mS"),
// and DELAY_100US. An IN token to the mouse that the mouse NAKs waits, as SET_RETRY says.
#pragma once
#include "common.h"
#include "m4_storage.h"

namespace cpcse {

class Ch376 {
public:
    explicit Ch376(int basePort = 0xfe80) : base(basePort) { powerOn(); }
    ~Ch376();

    int base;                          // &FE80, or &FE40 with JP2/JP3 cut
    bool enabled = false;
    enum class Usb { None, Stick, Mouse };

    void setEnabled(bool on);
    void setSdFolder(const std::string& folder);                // "" = no card in the slot
    void setUsb(Usb kind, const std::string& folder = "");      // what is plugged in the USB port
    Usb usbKind() const { return usb; }
    const std::string& sdFolder() const { return sd.folder; }
    const std::string& usbFolder() const { return stick.folder; }

    bool handlesPort(int port) const;
    int readPort(int port, long long cycles);
    void writePort(int port, int value, long long cycles);
    void poll(long long cycles);       // NAK retry time-outs; the write-back of raw sector writes
    void reset();                      // the CPC's reset (switch S2 ON: the CH376 resets with it)
    void flush();                      // write raw sector changes back to the folders now

    // The USB mouse (Usb::Mouse): host mouse movement in mouse counts, buttons 0 left, 1 right,
    // 2 middle; the wheel in notches (positive away from the user).
    void mouseMove(int dx, int dy);
    void mouseButton(int index, bool pressed);
    void mouseWheel(int notches);

    // State a check can read (the chip's own registers, not derived values).
    int mode = 0;
    bool intPending = false;
    uint8_t intStatus = 0;

private:
    struct Medium {
        std::string folder;
        std::unique_ptr<M4Storage> fs;
        std::unique_ptr<M4SdCard> card;
        void open(const std::string& f);
        bool present() const { return fs != nullptr; }
    };
    Medium sd, stick;
    Usb usb = Usb::None;

    long long now = 0, busyUntil = 0, delayUntil = 0, nakDeadline = 0, lastRawWrite = -1;

    // the command being given
    int cmd = -1;
    Bytes in;                          // its input bytes so far
    int need = 0;                      // how many it takes (-1: decided by the bytes)
    Bytes out;                         // its output bytes
    size_t outPos = 0;

    uint8_t usbData[256] = {};         // what RD_USB_DATA0 gives (the interrupt buffer)
    int usbLen = 0;
    uint8_t hostData[64] = {};         // what WR_HOST_DATA / WR_REQ_DATA wrote
    int hostLen = 0;
    uint8_t dirInfo[32] = {};          // FAT_DIR_INFO, for DIR_INFO_READ / WR_OFS_DATA / DIR_INFO_SAVE
    std::string fileName;              // SET_FILE_NAME
    uint8_t retry = 0x8f;              // SET_RETRY (§1.4 of Datasheet II: 8FH after reset)

    // file system state
    enum class Open { None, File, Dir } opened = Open::None;
    std::string cwd = "/";             // the directory a name without '/' is in
    std::string openPath;              // real path of the open file or directory
    Bytes content;                     // the open file's bytes
    uint32_t sizeVar = 0;              // the file length variable (§5.7)
    uint32_t dirLength = 0;            // the length in the directory entry
    uint32_t pointer = 0;              // the file pointer
    bool contentDirty = false;
    std::vector<M4Entry> enumRows;     // FILE_OPEN with '*' and FILE_ENUM_GO
    std::vector<std::string> enumNames;
    std::string enumPrefix;
    size_t enumIndex = 0;

    // transfers
    enum class Xfer { None, ByteRead, ByteWrite, DiskRead, DiskWrite } xfer = Xfer::None;
    uint32_t remaining = 0;            // bytes (byte transfers) or 64-byte blocks (disk)
    uint32_t xferLba = 0;
    uint32_t writeAt = 0;              // where WR_REQ_DATA's bytes go (byte write)
    int reqLen = 0;                    // the chunk WR_REQ_DATA asks for
    int reqGot = 0;
    uint8_t sector[512] = {};
    int block = 0;                     // the 64-byte block of the sector in this step

    // the USB device
    bool connectReported = false;
    int hostAddress = 0;               // SET_USB_ADDR: the address the CH376 talks to
    int deviceAddress = 0;             // the device's own address
    bool configured = false;
    int protocol = 1;                  // HID: 0 boot, 1 report (a device starts in report)
    Bytes controlIn;                   // a control transfer's data stage
    size_t controlPos = 0;
    bool waitingIn = false;            // an IN token NAKed, retrying
    int mouseDx = 0, mouseDy = 0, mouseWheelCount = 0, mouseButtons = 0, sentButtons = 0;

    void powerOn();
    void command(int c);
    void data(int v);
    void execute();
    void complete(uint8_t status);
    void outByte(int v) { out.push_back((uint8_t)v); }
    void setUsbData(const uint8_t* p, int n);
    bool busy() const { return now < busyUntil; }

    Medium* medium();                  // the mass storage of the current mode, or nullptr
    void closeFiles();
    void mountState();                 // a mode change: the file system starts again
    bool syncRaw(Medium& m);           // raw sector changes into the folder before file access
    uint8_t mountCheck(Medium*& m);    // 0 or the status for "no disk"
    void saveFile();
    std::string resolve(const std::string& dirReal, const std::string& name, M4Entry* entry);
    void fillDirInfo(uint8_t* p, const M4Entry& e, const std::string& e11);
    std::string dirOf(const std::string& name, std::string& leaf);   // the directory a name is in

    void fileOpen();
    void enumNext();
    void fileCreate();
    void fileErase();
    void fileClose(int update);
    void dirInfoRead(int index);
    void dirInfoSave();
    void dirCreate();
    void byteLocate(uint32_t offset);
    void byteReadStep();
    void byteWriteStep();
    void secLocate(uint32_t sectors);
    void secRead(int count, bool write);
    void diskQuery(bool capacityOnly);
    void diskReadStep();
    void diskWriteStep();
    uint32_t lbaOfPointer();

    bool deviceAnswers() const { return usb != Usb::None && hostAddress == deviceAddress; }
    const Bytes& deviceDescriptor() const;
    const Bytes& configDescriptor() const;
    void issueToken(int sync, int attribute);
    void setupPacket();
    bool mouseReport();
};

} // namespace cpcse
