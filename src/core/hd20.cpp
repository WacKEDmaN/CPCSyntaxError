// CPCSyntaxError — the Dobbertin HD20 hard disc (see hd20.h).
#include "hd20.h"
#include <algorithm>
#include <filesystem>

namespace cpcse {

// The reasons request sense gives: the usual SASI controller codes (Xebec).
static const int SENSE_NONE = 0x00, SENSE_BAD_COMMAND = 0x20, SENSE_BAD_ADDRESS = 0x21, SENSE_READ = 0x11;

bool Hd20::open(const std::string& imagePath) {
    file.reset();
    path.clear();
    error.clear();
    reset();
    if (imagePath.empty()) return true;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p = fs::u8path(imagePath);
    const auto size = fs::file_size(p, ec);
    if (ec || size < 512) { error = "no image at " + imagePath; return false; }
    auto f = std::make_shared<std::fstream>(p, std::ios::in | std::ios::out | std::ios::binary);
    readOnly = false;
    if (!*f) { f = std::make_shared<std::fstream>(p, std::ios::in | std::ios::binary); readOnly = true; }
    if (!*f) { error = "cannot open " + imagePath; return false; }
    file = f;
    path = imagePath;
    sectorsOnDisc = (uint32_t)(size / 512);
    return true;
}

void Hd20::reset() {
    phase = BusFree;
    cdb.clear();
    buffer.clear();
    at = 0;
}

bool Hd20::address(uint32_t& out) const {
    const int head = cdb[1] & 0x1f;
    const int sector = cdb[2] & 0x3f;
    const uint32_t cylinder = (uint32_t)(cdb[2] & 0xc0) << 2 | cdb[3];
    if (head >= HEADS || sector >= SECTORS) return false;
    out = (cylinder * HEADS + (uint32_t)head) * SECTORS + (uint32_t)sector;
    return true;
}

bool Hd20::readSectors(uint32_t first, int n, uint8_t* to) {
    if (!file || first + (uint32_t)n > sectorsOnDisc) return false;
    file->clear();
    file->seekg((std::streamoff)first * 512);
    file->read((char*)to, (std::streamsize)n * 512);
    return (bool)*file;
}

bool Hd20::writeSectors(uint32_t first, int n, const uint8_t* from) {
    if (!file || readOnly || first + (uint32_t)n > sectorsOnDisc) return false;
    file->clear();
    file->seekp((std::streamoff)first * 512);
    file->write((const char*)from, (std::streamsize)n * 512);
    file->flush();
    return (bool)*file;
}

void Hd20::finish(bool ok, int senseCode) {
    statusByte = ok ? 0 : 2;
    sense = ok ? SENSE_NONE : senseCode;
    buffer.clear();
    at = 0;
    phase = Status;
}

void Hd20::execute() {
    opcode = cdb[0];
    count = cdb[4] ? cdb[4] : 256;
    buffer.clear();
    at = 0;
    switch (opcode) {
        case 0x00: case 0x01: finish(true); return;    // test unit ready, recalibrate
        case 0x0b: {                                    // seek
            uint32_t a;
            finish(address(a) && a < sectorsOnDisc, SENSE_BAD_ADDRESS);
            return;
        }
        case 0x03: {                                    // request sense: the reason, then the address
            const int reason = sense;
            buffer = { (uint8_t)reason, cdb[1], cdb[2], cdb[3] };
            sense = SENSE_NONE;
            statusByte = 0;
            phase = DataIn;
            return;
        }
        case 0x08: {                                    // read
            if (!address(lba)) { finish(false, SENSE_BAD_ADDRESS); return; }
            buffer.resize((size_t)count * 512);
            if (!readSectors(lba, count, buffer.data())) { finish(false, lba + (uint32_t)count > sectorsOnDisc ? SENSE_BAD_ADDRESS : SENSE_READ); return; }
            statusByte = 0;
            phase = DataIn;
            return;
        }
        case 0x0a:                                      // write: the bytes come next
            if (!address(lba)) { finish(false, SENSE_BAD_ADDRESS); return; }
            buffer.resize((size_t)count * 512);
            phase = DataOut;
            return;
        case 0x0c:                                      // drive parameters: 8 bytes come next
            buffer.resize(8);
            phase = DataOut;
            return;
        case 0x06: {                                    // format track: its sectors filled with &E5 (CP/M's empty)
            if (!address(lba)) { finish(false, SENSE_BAD_ADDRESS); return; }
            const uint32_t track = lba - lba % SECTORS;
            std::vector<uint8_t> blank((size_t)SECTORS * 512, 0xe5);
            finish(writeSectors(track, SECTORS, blank.data()), SENSE_BAD_ADDRESS);
            return;
        }
        default: finish(false, SENSE_BAD_COMMAND); return;
    }
}

int Hd20::readPort(int port) {
    port &= 0xffff;
    switch (port) {
        case 0xfbe0:
            if (phase == DataIn) {
                const int v = at < buffer.size() ? buffer[at++] : 0;
                if (at >= buffer.size()) { buffer.clear(); at = 0; phase = Status; }
                return v;
            }
            if (phase == Status) { phase = BusFree; return statusByte; }
            return 0xff;
        case 0xfbe1: return phase;
        case 0xfbe2: return 1;                          // there (X-DDOS's start-up test)
        default: return 0xff;
    }
}

void Hd20::writePort(int port, int value) {
    port &= 0xffff;
    value &= 0xff;
    switch (port) {
        case 0xfbe0:
            if (phase == Command) {
                cdb.push_back((uint8_t)value);
                if (cdb.size() == 6) execute();
            } else if (phase == DataOut) {
                if (at < buffer.size()) buffer[at++] = (uint8_t)value;
                if (at >= buffer.size()) {
                    if (opcode == 0x0a) finish(writeSectors(lba, count, buffer.data()), readOnly ? SENSE_READ : SENSE_BAD_ADDRESS);
                    else finish(true);              // the drive parameters: the image says them already
                }
            }
            return;
        case 0xfbe2: cdb.clear(); buffer.clear(); at = 0; phase = Command; return;   // select
        case 0xfbe3: case 0xfbe4: reset(); return;
        default: return;
    }
}

} // namespace cpcse
