// CPCSyntaxError — the Dobbertin HD20 hard disc, with an image file as its disc.
//
// Its controller is a SASI one, as X-DDOS 2.10 (the HD20's DOS ROM) and FutureOS drive it
// (X-DDOS &EEC2-&EF95; FutureOS's write routine, TMTLOGIC's Write_HD20_512.txt):
//   &FBE0  data: the command's 6 bytes, the sector bytes, then the status byte
//   &FBE1  status (low 4 bits): bit 0 REQ, bit 1 I/O, bit 2 C/D, bit 3 BSY, so
//          &0D command, &09 data out, &0B data in, &0F status, 0 bus free
//   &FBE2  write: select (a command follows); read: 1, the HD20 is there (X-DDOS &EED2)
//   &FBE3, &FBE4  written by X-DDOS's start-up: the controller back to bus free
// Commands (the templates at X-DDOS &EF75): &00 test unit ready, &01 recalibrate, &03
// request sense (4 bytes), &06 format track, &08 read, &0A write, &0B seek, &0C drive
// parameters (8 bytes follow: &0266 = 614 cylinders, 4 heads). The address bytes are the
// head (0-3), then 2 bits of cylinder above 6 of sector (0-16), then the cylinder's low 8
// bits; then the sector count. The status byte is 0, or 2 when the command failed
// (X-DDOS &EE47 tests bit 1); request sense then gives the reason.
//
// The image is the disc's sectors in order, ((cylinder x 4 + head) x 17 + sector) x 512:
// TMTLOGIC's HD20_01.IMG is 614 x 4 x 17 sectors (21,377,024 bytes), and its four CP/M
// directories are at cylinders 0, 153, 306 and 459 (head 3, sector 0), where X-DDOS puts
// its drives E: to H:. Writes go straight into the file.
#pragma once
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace cpcse {

class Hd20 {
public:
    static constexpr int HEADS = 4, SECTORS = 17;
    bool enabled() const { return file != nullptr; }
    std::string image() const { return path; }
    bool readOnly = false;
    // Opens the image ("" takes the disc out). False, with `error`, if it cannot be used.
    bool open(const std::string& imagePath);
    std::string error;

    bool handlesPort(int port) const { port &= 0xffff; return enabled() && port >= 0xfbe0 && port <= 0xfbe4; }
    int readPort(int port);
    void writePort(int port, int value);
    void reset();

private:
    std::shared_ptr<std::fstream> file;
    std::string path;
    uint32_t sectorsOnDisc = 0;
    enum Phase { BusFree = 0, Command = 0x0d, DataOut = 0x09, DataIn = 0x0b, Status = 0x0f };
    Phase phase = BusFree;
    std::vector<uint8_t> cdb, buffer;
    size_t at = 0;
    int statusByte = 0, sense = 0;
    uint32_t lba = 0;
    int count = 0, opcode = 0;
    void execute();
    void finish(bool ok, int senseCode = 0);
    bool address(uint32_t& out) const;
    bool readSectors(uint32_t first, int n, uint8_t* to);
    bool writeSectors(uint32_t first, int n, const uint8_t* from);
};

} // namespace cpcse
