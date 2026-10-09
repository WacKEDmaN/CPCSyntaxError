// CPCSyntaxError — the SYMBiFACE II / III IDE interface. See symbiface_ide.h.
#include "symbiface_ide.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cpcse {

// ATA status bits
static const uint8_t BSY = 0x80, DRDY = 0x40, DSC = 0x10, DRQ = 0x08, ERR = 0x01;
// ATA error bits
static const uint8_t ABRT = 0x04, IDNF = 0x10, UNC = 0x40;
// The geometry IDENTIFY gives for CHS addressing (the usual translation of a CF card).
static const uint32_t HEADS = 16, SPT = 63;

SymbifaceIde::SymbifaceIde() { signature(); }
SymbifaceIde::~SymbifaceIde() { flush(); }

void SymbifaceIde::setEnabled(bool on) {
    if (enabled == on) return;
    if (!on) flush();
    enabled = on;
    reset();
}

void SymbifaceIde::setFolder(const std::string& hostFolder) {
    if (storage && storage->root == hostFolder) return;
    flush();
    disc.reset();
    storage.reset();
    folder = hostFolder;
    if (!hostFolder.empty()) {
        storage = std::make_unique<M4Storage>(hostFolder);
        disc = std::make_unique<M4SdCard>(*storage);
    }
    reset();
}

void SymbifaceIde::reset() {
    flush();
    phase = Idle;
    bufferPos = 0;
    remaining = 0;
    control = 0;
    features = 0;
    signature();
}

// What a drive holds after a reset or EXECUTE DEVICE DIAGNOSTIC (ATA: the signature of a
// non-packet device, diagnostic code 01 = no error).
void SymbifaceIde::signature() {
    error = 1;
    count = 1;
    sector = 1;
    cylLow = cylHigh = 0;
    device = 0;
    status = DRDY | DSC;
}

void SymbifaceIde::flush() {
    if (disc && disc->dirty()) {
        std::string report;
        if (disc->syncToHost(&report)) lastSync = report;
    }
}

void SymbifaceIde::poll(long long cycles) {
    now = cycles;
    // Half a second after the last sector written, as the M4's card.
    if (disc && disc->dirty() && lastWrite >= 0 && now - lastWrite > 2000000) flush();
}

void SymbifaceIde::rescan() {
    flush();
    if (storage) storage->touch();
}

uint32_t SymbifaceIde::sectors() {
    if (!disc) return 0;
    uint8_t probe[512];
    disc->read(0, 1, probe);                     // the volume is laid out on first use
    return disc->totalSectors();
}

bool SymbifaceIde::handlesPort(int port) const {
    if (!enabled || !disc) return false;
    port &= 0xffff;
    return port >= 0xfd06 && port <= 0xfd0f;
}

bool SymbifaceIde::address(uint32_t& out) const {
    if (device & 0x40) {                         // LBA
        out = (uint32_t)sector | (uint32_t)cylLow << 8 | (uint32_t)cylHigh << 16 | (uint32_t)(device & 0x0f) << 24;
        return true;
    }
    if (sector == 0) return false;               // CHS sectors count from 1
    const uint32_t cylinder = (uint32_t)cylLow | (uint32_t)cylHigh << 8;
    const uint32_t head = device & 0x0f;
    if (head >= HEADS || sector > SPT) return false;
    out = (cylinder * HEADS + head) * SPT + (sector - 1);
    return true;
}

void SymbifaceIde::setAddress(uint32_t value) {
    if (device & 0x40) {
        sector = (uint8_t)value;
        cylLow = (uint8_t)(value >> 8);
        cylHigh = (uint8_t)(value >> 16);
        device = (uint8_t)((device & 0xf0) | ((value >> 24) & 0x0f));
    } else {
        const uint32_t cylinder = value / (HEADS * SPT), rest = value % (HEADS * SPT);
        sector = (uint8_t)(rest % SPT + 1);
        cylLow = (uint8_t)cylinder;
        cylHigh = (uint8_t)(cylinder >> 8);
        device = (uint8_t)((device & 0xf0) | (rest / SPT));
    }
}

void SymbifaceIde::fail(uint8_t err) {
    phase = Idle;
    error = err;
    status = DRDY | DSC | ERR;
}

void SymbifaceIde::done() {
    phase = Idle;
    status = DRDY | DSC;
}

bool SymbifaceIde::loadSector() {
    if (lba >= sectors()) { fail(IDNF); return false; }
    if (disc->read(lba, 1, buffer) != 0) { fail(UNC); return false; }
    setAddress(lba);
    bufferPos = 0;
    status = DRDY | DSC | DRQ;
    return true;
}

// IDENTIFY DEVICE: 256 words, low byte first; strings two characters a word, the first in
// the high byte (ATA's own order).
void SymbifaceIde::identify() {
    std::memset(buffer, 0, sizeof buffer);
    auto word = [&](int w, uint32_t v) { buffer[w * 2] = (uint8_t)v; buffer[w * 2 + 1] = (uint8_t)(v >> 8); };
    auto text = [&](int w, int words, const std::string& s) {
        for (int i = 0; i < words * 2; i++) buffer[w * 2 + (i ^ 1)] = (uint8_t)(i < (int)s.size() ? s[i] : ' ');
    };
    const uint32_t total = sectors();
    const uint32_t cylinders = std::min<uint32_t>(16383, total / (HEADS * SPT));
    word(0, 0x0040);                             // a fixed, non-removable ATA device
    word(1, cylinders);
    word(3, HEADS);
    word(6, SPT);
    text(10, 10, "CPCSE-SYMIDE");                // serial number
    text(23, 4, "1.0");                          // firmware revision
    text(27, 20, "CPCSyntaxError symide folder");
    word(47, 0x8001);                            // READ/WRITE MULTIPLE: one sector a block
    word(49, 0x0200);                            // LBA supported
    word(51, 0x0200);
    word(53, 0x0001);                            // words 54-58 valid
    word(54, cylinders);
    word(55, HEADS);
    word(56, SPT);
    const uint32_t chs = cylinders * HEADS * SPT;
    word(57, chs & 0xffff);
    word(58, chs >> 16);
    word(59, 0x0101);                            // multiple: one sector, valid
    word(60, total & 0xffff);                    // sectors addressable by LBA
    word(61, total >> 16);
    phase = Reading;
    remaining = 0;
    bufferPos = 0;
    status = DRDY | DSC | DRQ;
}

void SymbifaceIde::command(int c) {
    static const bool trace = std::getenv("CPCSE_TRACE_IDE") != nullptr;
    if (trace) {
        uint32_t at = 0;
        const bool ok = address(at);
        std::fprintf(stderr, "IDE %02x dev=%02x count=%d %s=%u%s\n", c, device, count ? count : 256,
                     (device & 0x40) ? "lba" : "chs", at, ok ? "" : " (bad)");
    }
    if (!present()) return;                      // no slave: nothing takes the command
    error = 0;
    auto sectorsAsked = [&]() { return count == 0 ? 256 : (int)count; };
    switch (c) {
        case 0x20: case 0x21: case 0xc4: {       // READ SECTORS (/no retry), READ MULTIPLE
            if (!address(lba)) { fail(IDNF); return; }
            remaining = sectorsAsked() - 1;
            phase = Reading;
            loadSector();
            return;
        }
        case 0x30: case 0x31: case 0xc5: {       // WRITE SECTORS (/no retry), WRITE MULTIPLE
            if (!address(lba)) { fail(IDNF); return; }
            if (lba >= sectors()) { fail(IDNF); return; }
            remaining = sectorsAsked() - 1;
            phase = Writing;
            bufferPos = 0;
            status = DRDY | DSC | DRQ;
            return;
        }
        case 0x40: case 0x41: {                  // READ VERIFY SECTORS
            uint32_t at = 0;
            if (!address(at) || at + (uint32_t)sectorsAsked() > sectors()) { fail(IDNF); return; }
            setAddress(at + (uint32_t)sectorsAsked() - 1);
            done();
            return;
        }
        case 0xec: identify(); return;           // IDENTIFY DEVICE
        case 0xf8: {                             // READ NATIVE MAX ADDRESS
            const uint32_t total = sectors();
            setAddress(total ? total - 1 : 0);
            done();
            return;
        }
        case 0x90:                               // EXECUTE DEVICE DIAGNOSTIC
            signature();
            return;
        case 0xe5: count = 0xff; done(); return; // CHECK POWER MODE: active
        case 0xef:                               // SET FEATURES: 8-bit transfers (01/81), caches,
            switch (features) {                  // transfer modes -- all taken, none change the bytes
                case 0x01: case 0x81: case 0x02: case 0x82: case 0x03: case 0x55: case 0xaa: case 0x66: case 0xcc:
                    done(); return;
                default: fail(ABRT); return;
            }
        case 0x91: case 0xc6: case 0xe7: case 0xea:   // INITIALIZE DEVICE PARAMETERS, SET MULTIPLE, FLUSH CACHE
        case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe6: case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
            done();                              // standby/idle/sleep: a folder never spins down
            return;
        default:
            if ((c & 0xf0) == 0x10 || (c & 0xf0) == 0x70) { done(); return; }   // RECALIBRATE, SEEK
            fail(ABRT);
            return;
    }
}

int SymbifaceIde::readPort(int port) {
    if (!handlesPort(port)) return 0xff;
    switch (port & 0x0f) {
        case 0x6: return present() ? status : 0x00;          // alternate status
        case 0x7:                                            // drive address: active-low selects and head
            return 0x40 | ((~device & 0x0f) << 2) | ((device & 0x10) ? 0x01 : 0x02);
        case 0x8: {                                          // data
            if (phase != Reading || !present()) return 0xff;
            const int v = buffer[bufferPos++];
            if (bufferPos >= 512) {
                if (remaining > 0) {
                    remaining -= 1;
                    count = (uint8_t)(count - 1);
                    lba += 1;
                    loadSector();
                } else done();
            }
            return v;
        }
        case 0x9: return error;
        case 0xa: return count;
        case 0xb: return sector;
        case 0xc: return cylLow;
        case 0xd: return cylHigh;
        case 0xe: return device | 0xa0;                      // bits 7 and 5 read as 1 (obsolete)
        case 0xf: return present() ? status : 0x00;          // status (no slave: 0)
    }
    return 0xff;
}

void SymbifaceIde::writePort(int port, int value) {
    if (!handlesPort(port)) return;
    value &= 0xff;
    switch (port & 0x0f) {
        case 0x6: {                                          // device control
            const bool wasReset = control & 0x04;
            control = (uint8_t)value;
            if (control & 0x04) { phase = Idle; status = BSY; }   // SRST held: busy
            else if (wasReset) signature();                       // SRST let go: the drive is back
            return;
        }
        case 0x8: {                                          // data
            if (phase != Writing || !present()) return;
            buffer[bufferPos++] = (uint8_t)value;
            if (bufferPos >= 512) {
                bufferPos = 0;
                const int r = disc->write(lba, 1, buffer);
                if (r != 0) { fail(r == 2 ? ABRT : UNC); return; }   // 2: the folder is read-only
                lastWrite = now;
                setAddress(lba);
                if (remaining > 0) {
                    remaining -= 1;
                    count = (uint8_t)(count - 1);
                    lba += 1;
                    if (lba >= sectors()) { fail(IDNF); return; }
                    status = DRDY | DSC | DRQ;
                } else done();
            }
            return;
        }
        case 0x9: features = (uint8_t)value; return;
        case 0xa: count = (uint8_t)value; return;
        case 0xb: sector = (uint8_t)value; return;
        case 0xc: cylLow = (uint8_t)value; return;
        case 0xd: cylHigh = (uint8_t)value; return;
        case 0xe: device = (uint8_t)value; return;
        case 0xf: if (!(control & 0x04)) command(value); return;
    }
}

} // namespace cpcse
