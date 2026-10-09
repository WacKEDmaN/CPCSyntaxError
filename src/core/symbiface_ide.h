// CPCSyntaxError — the SYMBiFACE II / III IDE (CF) interface, with a host folder as its disc.
//
// The card puts an ATA task file on the CPC's I/O bus (MAME's symbfac2.cpp, the register
// map of the ATA standard):
//   &FD08 data       &FD09 error (r) / features (w)   &FD0A sector count
//   &FD0B sector / LBA 0-7    &FD0C cylinder low / LBA 8-15   &FD0D cylinder high / LBA 16-23
//   &FD0E device/head (bit 4 slave, bit 6 LBA, bits 0-3 LBA 24-27)
//   &FD0F status (r) / command (w)
//   &FD06 alternate status (r) / device control (w: bit 2 software reset)
//   &FD07 drive address (r)
// The CPC's bus is 8 bits wide and the drive's data register 16: the card latches, so a
// word is two accesses, low byte first (MAME: the high byte kept from the read of the
// low). A sector is therefore 512 accesses in order, which is also what 8-bit mode
// (SET FEATURES 01h, which CF cards have) gives -- either way the bytes come in order.
//
// The disc is a host folder (by default "symide" beside the program), shown as one FAT16
// partition by the same code that gives the M4 its card: what a program writes goes back
// into the folder once the disc has been idle a moment, and on reset, power off and exit.
//
// One drive, the master. The drive answers at once (BSY is never seen): the CPC's drivers
// poll the status, and a real CF card is ready in microseconds.
#pragma once
#include "common.h"
#include "m4_storage.h"

namespace cpcse {

class SymbifaceIde {
public:
    SymbifaceIde();
    ~SymbifaceIde();
    bool enabled = false;
    std::string folder;

    void setEnabled(bool on);
    void setFolder(const std::string& hostFolder);   // "" = no drive fitted
    void reset();                    // the CPC's reset line: the drive resets too
    bool handlesPort(int port) const;
    int readPort(int port);
    void writePort(int port, int value);
    // Writes the disc's changes back to the folder once it has been idle; flush() at once.
    void poll(long long cycles);
    void flush();
    void rescan();                   // the folder changed from outside
    uint32_t sectors();              // the disc's size in 512-byte sectors
    std::string lastSync;            // what the last write-back changed

    // the task file, as the drive holds it
    uint8_t error = 1, features = 0, count = 1, sector = 1, cylLow = 0, cylHigh = 0, device = 0, status = 0x50, control = 0;

private:
    std::unique_ptr<M4Storage> storage;
    std::unique_ptr<M4SdCard> disc;
    enum Phase { Idle, Reading, Writing } phase = Idle;
    uint8_t buffer[512] = {};
    int bufferPos = 0;
    int remaining = 0;               // sectors still to come in this command
    uint32_t lba = 0;                // the sector in the buffer
    long long now = 0, lastWrite = -1;

    bool present() const { return disc && !(device & 0x10); }   // the master is the only drive
    void command(int c);
    void signature();
    void fail(uint8_t err);
    void done();
    bool address(uint32_t& out) const;   // the task file's sector, LBA or CHS
    void setAddress(uint32_t value);
    bool loadSector();
    void identify();
};

} // namespace cpcse
