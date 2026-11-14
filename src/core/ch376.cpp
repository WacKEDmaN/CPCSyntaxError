// CPCSyntaxError: the Albireo board's CH376. See ch376.h.
#include "ch376.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cpcse {

// Commands (Datasheet I §5, Datasheet II §1).
enum : int {
    GET_IC_VER = 0x01, SET_BAUDRATE = 0x02, ENTER_SLEEP = 0x03, SET_USB_SPEED = 0x04, RESET_ALL = 0x05,
    CHECK_EXIST = 0x06, READ_VAR8 = 0x0a, WRITE_VAR8 = 0x0b, READ_VAR32 = 0x0c, WRITE_VAR32 = 0x0d,
    DELAY_100US = 0x0f, SET_USB_ADDR = 0x13, SET_USB_MODE = 0x15, TEST_CONNECT = 0x16, ABORT_NAK = 0x17,
    SET_ENDP6 = 0x1c, SET_ENDP7 = 0x1d, GET_STATUS = 0x22, DIRTY_BUFFER = 0x25, RD_USB_DATA0 = 0x27,
    RD_USB_DATA = 0x28, WR_HOST_DATA = 0x2c, WR_REQ_DATA = 0x2d, WR_OFS_DATA = 0x2e, SET_FILE_NAME = 0x2f,
    DISK_CONNECT = 0x30, DISK_MOUNT = 0x31, FILE_OPEN = 0x32, FILE_ENUM_GO = 0x33, FILE_CREATE = 0x34,
    FILE_ERASE = 0x35, FILE_CLOSE = 0x36, DIR_INFO_READ = 0x37, DIR_INFO_SAVE = 0x38, BYTE_LOCATE = 0x39,
    BYTE_READ = 0x3a, BYTE_RD_GO = 0x3b, BYTE_WRITE = 0x3c, BYTE_WR_GO = 0x3d, DISK_CAPACITY = 0x3e,
    DISK_QUERY = 0x3f, DIR_CREATE = 0x40, CLR_STALL = 0x41, SET_ADDRESS = 0x45, GET_DESCR = 0x46,
    SET_CONFIG = 0x49, SEC_LOCATE = 0x4a, SEC_READ = 0x4b, SEC_WRITE = 0x4c, AUTO_SETUP = 0x4d, DISK_BOC_CMD = 0x50,
    ISSUE_TKN_X = 0x4e, DISK_INIT = 0x51, DISK_RESET = 0x52, DISK_SIZE = 0x53, DISK_READ = 0x54,
    DISK_RD_GO = 0x55, DISK_WRITE = 0x56, DISK_WR_GO = 0x57, DISK_INQUIRY = 0x58, DISK_READY = 0x59,
    DISK_R_SENSE = 0x5a, DISK_MAX_LUN = 0x5d,
};
// Operation status (§5) and interrupt status (§5.10).
static const uint8_t CMD_RET_SUCCESS = 0x51, CMD_RET_ABORT = 0x5f;
static const uint8_t USB_INT_SUCCESS = 0x14, USB_INT_CONNECT = 0x15, USB_INT_DISCONNECT = 0x16,
    USB_INT_USB_READY = 0x18, USB_INT_DISK_READ = 0x1d, USB_INT_DISK_WRITE = 0x1e, USB_INT_DISK_ERR = 0x1f;
static const uint8_t ERR_OPEN_DIR = 0x41, ERR_MISS_FILE = 0x42, ERR_FOUND_NAME = 0x43, ERR_DISK_DISCON = 0x82,
    ERR_DISK_FULL = 0xb1, ERR_FILE_CLOSE = 0xb4;
// A USB transaction that failed (§5.10's second table): bit 5 set, bits 3-0 what the device
// returned: 1010 NAK, 1110 STALL, xx00 time-out.
static const uint8_t USB_NAK = 0x2a, USB_STALL = 0x2e, USB_TIMEOUT = 0x20;
// The chip's version (§5.1: bit 6 set, bits 5-0 the version). The Albireo manual: "In all
// Albireo boards currently available, you should get v = 4" (the byte minus &40).
static const uint8_t IC_VERSION = 0x44;
// The CPC's clock: machine cycles at 4 MHz.
static const long long CYCLES_PER_MS = 4000;
// The largest file a program can make through the chip here. Its lengths are the program's
// own (SET_FILE_SIZE, a directory entry it writes, BYTE_WRITE past the end), and the file is
// held in memory: a length of FFFFFFFFH asked for 4 GB. A file larger than this is DISK FULL.
static const uint32_t MAX_FILE_BYTES = 256u << 20;
static uint32_t capFileSize(uint32_t v) { return std::min(v, MAX_FILE_BYTES); }

Ch376::~Ch376() { flush(); }

void Ch376::Medium::open(const std::string& f) {
    folder = f;
    card.reset();
    fs.reset();
    if (!f.empty()) {
        fs = std::make_unique<M4Storage>(f);
        card = std::make_unique<M4SdCard>(*fs);
    }
}

void Ch376::setEnabled(bool on) {
    if (enabled == on) return;
    if (!on) flush();
    enabled = on;
    powerOn();
}

void Ch376::setSdFolder(const std::string& folder) {
    if (sd.folder == folder && (sd.present() || folder.empty())) return;
    flush();
    sd.open(folder);
    if (mode == 3) mountState();
}

void Ch376::setUsb(Usb kind, const std::string& folder) {
    const bool same = kind == usb && (kind != Usb::Stick || stick.folder == folder);
    if (same) return;
    flush();
    const bool wasThere = usb != Usb::None;
    usb = kind;
    stick.open(kind == Usb::Stick ? folder : "");
    deviceAddress = 0;
    configured = false;
    protocol = 1;
    waitingIn = false;
    if (mode >= 5) {
        mountState();
        // An enabled host mode sees the plug (§5.9: "When the USB device is connected or
        // disconnected, an interrupt will be generated").
        if (wasThere) complete(USB_INT_DISCONNECT);
        connectReported = false;
        if (usb != Usb::None && (mode == 5 || mode == 6)) { complete(USB_INT_CONNECT); connectReported = true; }
    }
}

bool Ch376::handlesPort(int port) const {
    if (!enabled) return false;
    port &= 0xffff;
    return port == base || port == base + 1;
}

void Ch376::powerOn() {
    closeFiles();                      // first: an unsaved file still needs the mode's medium
    mode = 0;
    intPending = false;
    intStatus = 0;
    busyUntil = delayUntil = 0;
    cmd = -1;
    in.clear(); out.clear(); outPos = 0;
    need = 0;
    usbLen = hostLen = 0;
    fileName.clear();
    retry = 0x8f;
    connectReported = false;
    hostAddress = deviceAddress = 0;
    configured = false;
    protocol = 1;
    controlIn.clear(); controlPos = 0;
    waitingIn = false;
    mouseDx = mouseDy = mouseWheelCount = 0;
    sentButtons = mouseButtons;
    cwd = "/";
    xfer = Xfer::None;
}

void Ch376::reset() {
    flush();
    powerOn();
}

void Ch376::flush() {
    for (Medium* m : { &sd, &stick })
        if (m->card && m->card->dirty()) m->card->syncToHost();
    if (opened == Open::File && contentDirty) saveFile();
}

void Ch376::poll(long long cycles) {
    now = cycles;
    // SET_RETRY bits 7-6 = 11: "finite retry will be performed for 3 seconds around".
    if (waitingIn && nakDeadline > 0 && now >= nakDeadline) { waitingIn = false; complete(USB_NAK); }
    // Raw sectors back into the folder half a second after the last one, as the M4's card.
    if (lastRawWrite >= 0 && now - lastRawWrite > 500 * CYCLES_PER_MS) {
        for (Medium* m : { &sd, &stick })
            if (m->card && m->card->dirty()) m->card->syncToHost();
        lastRawWrite = -1;
    }
}

int Ch376::readPort(int port, long long cycles) {
    poll(cycles);
    if ((port & 0xffff) == base + 1) return (intPending ? 0x00 : 0x80) | (busy() ? 0x10 : 0x00);
    if (busy()) return 0x00;
    // DELAY_100US (Datasheet II §1.8): "The output data is 0 during delay, and is non-0
    // (usually the chip version number) at the end of the delay."
    if (cmd == DELAY_100US) return now < delayUntil ? 0x00 : IC_VERSION;
    if (outPos < out.size()) return out[outPos++];
    return 0x00;   // §5.5: the parallel port reads 00H when there is nothing to give
}

void Ch376::writePort(int port, int value, long long cycles) {
    poll(cycles);
    if (busy()) return;
    value &= 0xff;
    if ((port & 0xffff) == base + 1) command(value);
    else data(value);
}

// CPCSE_TRACE_CH376=1: every command with its input, the file name it acts on and the
// interrupt status it ends with; what a driver does, read before theorising about it.
static const bool traceCh376 = std::getenv("CPCSE_TRACE_CH376") != nullptr;

void Ch376::complete(uint8_t status) {
    if (traceCh376) std::printf("[ch376 %04X] -> status %02X\n", base, status);
    intStatus = status;
    intPending = true;
}

void Ch376::setUsbData(const uint8_t* p, int n) {
    usbLen = std::clamp(n, 0, 255);
    if (usbLen) std::memcpy(usbData, p, (size_t)usbLen);
}

// ---------------------------------------------------------------- the command interface

void Ch376::command(int c) {
    cmd = c;
    in.clear();
    out.clear();
    outPos = 0;
    need = 0;
    switch (c) {
        case SET_USB_SPEED: case CHECK_EXIST: case READ_VAR8: case READ_VAR32: case SET_USB_ADDR: case SET_USB_MODE:
        case SET_ENDP6: case SET_ENDP7: case FILE_CLOSE: case DIR_INFO_READ: case CLR_STALL: case SET_ADDRESS:
        case GET_DESCR: case SET_CONFIG: case SEC_READ: case SEC_WRITE:
            need = 1; break;
        case SET_BAUDRATE: case WRITE_VAR8: case BYTE_READ: case BYTE_WRITE: case ISSUE_TKN_X:
            need = 2; break;
        case BYTE_LOCATE: case SEC_LOCATE:
            need = 4; break;
        case WRITE_VAR32: case DISK_READ: case DISK_WRITE:
            need = 5; break;
        case WR_HOST_DATA: case WR_OFS_DATA: case SET_FILE_NAME:
            need = -1;
            if (c == SET_FILE_NAME) fileName.clear();
            break;
        case WR_REQ_DATA:
            // §5.13: the chip first gives the length it asks for, then takes that many bytes.
            reqGot = 0;
            if (xfer == Xfer::ByteWrite) outByte(reqLen);
            else if (xfer == Xfer::DiskWrite) { reqLen = 64; outByte(64); }
            else { reqLen = 0; outByte(0); }
            hostLen = 0;
            return;
        default:
            need = 0; break;
    }
    if (need == 0) execute();
}

void Ch376::data(int v) {
    if (cmd < 0) return;
    if (cmd == WR_REQ_DATA) {
        if (reqGot >= reqLen) return;
        if (xfer == Xfer::ByteWrite) {
            const uint32_t at = writeAt + (uint32_t)reqGot;
            if (content.size() <= at) content.resize((size_t)at + 1, 0);
            content[at] = (uint8_t)v;
            contentDirty = true;
        } else if (hostLen < 64) {
            hostData[hostLen++] = (uint8_t)v;
        }
        reqGot += 1;
        return;
    }
    if (cmd == SET_FILE_NAME) {
        // §5.15: a string ending in 0, at most 14 characters with the 0.
        if (need == -1) {
            if (v == 0 || fileName.size() >= 13) { need = 0; if (v != 0) fileName.push_back((char)v); return; }
            fileName.push_back((char)v);
        }
        return;
    }
    if (need == 0) return;             // a command's input is complete: further bytes are lost
    in.push_back((uint8_t)v);
    if (cmd == WR_HOST_DATA) {
        // §5.12: a length (0-64), then the bytes.
        const int len = std::min<int>(in[0], 64);
        if ((int)in.size() == 1) hostLen = 0;
        else if (hostLen < len) hostData[hostLen++] = (uint8_t)v;
        if ((int)in.size() >= 1 + len) need = 0;
        return;
    }
    if (cmd == WR_OFS_DATA) {
        // §5.14: an offset, a length, then the bytes; offset + length within 32.
        if (in.size() >= 3) {
            const int at = in[0] + (int)in.size() - 3;
            if (at < 32) dirInfo[at] = (uint8_t)v;
        }
        if (in.size() >= 2 && (int)in.size() >= 2 + in[1]) need = 0;
        return;
    }
    if ((int)in.size() == need) { need = 0; execute(); }
}

static uint32_t le32(const Bytes& b, size_t at) {
    return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}
static uint32_t le32(const uint8_t* b, size_t at) {
    return (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}

void Ch376::execute() {
    if (traceCh376 && cmd != GET_STATUS && cmd != RD_USB_DATA0) {
        std::printf("[ch376 %04X] cmd %02X", base, cmd);
        for (uint8_t b : in) std::printf(" %02X", b);
        if (cmd >= FILE_OPEN && cmd <= FILE_ERASE) std::printf(" \"%s\"", fileName.c_str());
        if (cmd == DIR_INFO_SAVE) { std::printf(" entry \""); for (int k = 0; k < 11; k++) std::printf("%c", dirInfo[k]); std::printf("\""); }
        std::printf("\n");
    }
    switch (cmd) {
        case GET_IC_VER: outByte(IC_VERSION); break;
        case SET_BAUDRATE: outByte(CMD_RET_SUCCESS); break;   // serial only: no meaning on the parallel port
        case ENTER_SLEEP: case SET_USB_SPEED: case SET_ENDP6: case SET_ENDP7: case DIRTY_BUFFER: break;
        case RESET_ALL: {
            flush();
            const long long t = now;
            powerOn();
            now = t;
            busyUntil = now + CYCLES_PER_MS;
            break;
        }
        case CHECK_EXIST: outByte(~in[0] & 0xff); break;
        case READ_VAR8:
            // Datasheet II §1.2: with 07H it is GET_DEV_RATE, bit 4 set for a 1.5 Mbps device.
            outByte(in[0] == 0x07 ? (usb == Usb::Mouse ? 0x10 : 0x00) : 0x00);
            break;
        case WRITE_VAR8:
            if (in[0] == 0x25) retry = in[1];   // SET_RETRY
            break;                               // SET_SD0_INT (16H) and the other variables: nothing to do here
        case READ_VAR32: {
            // GET_FILE_SIZE is READ_VAR32 of the variable 68H (§5.7): the file length variable.
            uint32_t v = 0;
            if (in[0] == 0x68) v = opened == Open::Dir ? 0xffffffffu : sizeVar;
            for (int k = 0; k < 4; k++) outByte((int)(v >> (8 * k)));
            break;
        }
        case WRITE_VAR32:
            if (in[0] == 0x68 && opened == Open::File) sizeVar = capFileSize(le32(in, 1));   // SET_FILE_SIZE (§5.8)
            break;
        case DELAY_100US: delayUntil = now + CYCLES_PER_MS / 10; break;
        case SET_USB_ADDR: hostAddress = in[0] & 0x7f; break;
        case SET_USB_MODE: {
            const int m = in[0];
            if (m > 7) { outByte(CMD_RET_ABORT); break; }
            mode = m;
            mountState();
            if (m == 7 || m < 5) { deviceAddress = 0; configured = false; protocol = 1; waitingIn = false; }
            if (m < 5) connectReported = false;
            else if (usb != Usb::None && !connectReported && m != 7) { complete(USB_INT_CONNECT); connectReported = true; }
            outByte(CMD_RET_SUCCESS);
            break;
        }
        case TEST_CONNECT:
            // Datasheet II §1.10.
            outByte(mode < 5 || usb == Usb::None ? USB_INT_DISCONNECT : deviceAddress != 0 ? USB_INT_USB_READY : USB_INT_CONNECT);
            break;
        case ABORT_NAK: waitingIn = false; break;
        case GET_STATUS: outByte(intStatus); intPending = false; break;
        case RD_USB_DATA0: case RD_USB_DATA:
            outByte(usbLen);
            for (int k = 0; k < usbLen; k++) outByte(usbData[k]);
            break;
        case WR_HOST_DATA: case WR_OFS_DATA: case SET_FILE_NAME: break;   // taken byte by byte in data()

        case DISK_CONNECT:
            // §5.16: "does not support SD card".
            if (mode >= 5) complete(usb == Usb::Stick ? USB_INT_SUCCESS : usb == Usb::None ? USB_INT_DISCONNECT : USB_INT_DISK_ERR);
            else complete(USB_INT_DISK_ERR);
            break;
        case DISK_MOUNT: case DISK_INIT: {
            Medium* m = nullptr;
            const uint8_t err = mountCheck(m);
            if (err) { complete(err); break; }
            if (cmd == DISK_INIT) { deviceAddress = hostAddress = 1; configured = true; }
            // §5.17: "The data is usually 36 bytes": the SCSI INQUIRY of the drive.
            uint8_t inquiry[36] = { 0x00, 0x80, 0x02, 0x02, 0x1f, 0, 0, 0 };
            std::memcpy(inquiry + 8, "CPCSE   ", 8);
            std::memcpy(inquiry + 16, mode == 3 ? "Albireo SD card " : "Albireo USB disk", 16);
            std::memcpy(inquiry + 32, "1.00", 4);
            setUsbData(inquiry, 36);
            complete(USB_INT_SUCCESS);
            break;
        }
        case FILE_OPEN: fileOpen(); break;
        case FILE_ENUM_GO: enumNext(); break;
        case FILE_CREATE: fileCreate(); break;
        case FILE_ERASE: fileErase(); break;
        case FILE_CLOSE: fileClose(in[0]); break;
        case DIR_INFO_READ: dirInfoRead(in[0]); break;
        case DIR_INFO_SAVE: dirInfoSave(); break;
        case BYTE_LOCATE: byteLocate(le32(in, 0)); break;
        case BYTE_READ:
            if (opened != Open::File) { complete(ERR_FILE_CLOSE); break; }
            xfer = Xfer::ByteRead;
            remaining = (uint32_t)in[0] | (uint32_t)in[1] << 8;
            byteReadStep();
            break;
        case BYTE_RD_GO:
            if (xfer != Xfer::ByteRead) { complete(USB_INT_SUCCESS); break; }
            byteReadStep();
            break;
        case BYTE_WRITE:
            if (opened != Open::File) { complete(ERR_FILE_CLOSE); break; }
            remaining = (uint32_t)in[0] | (uint32_t)in[1] << 8;
            if (remaining == 0) {
                // §5.29: a request of 0 bytes only writes the file length.
                xfer = Xfer::None;
                dirLength = sizeVar;
                saveFile();
                complete(USB_INT_SUCCESS);
                break;
            }
            xfer = Xfer::ByteWrite;
            byteWriteStep();
            break;
        case BYTE_WR_GO:
            if (xfer != Xfer::ByteWrite) { complete(USB_INT_SUCCESS); break; }
            byteWriteStep();
            break;
        case DISK_CAPACITY: diskQuery(true); break;
        case DISK_QUERY: diskQuery(false); break;
        case DIR_CREATE: dirCreate(); break;
        case SEC_LOCATE: secLocate(le32(in, 0)); break;
        case SEC_READ: secRead(in[0], false); break;
        case SEC_WRITE: secRead(in[0], true); break;
        case DISK_READ: case DISK_WRITE: {
            Medium* m = nullptr;
            const uint8_t err = mountCheck(m);
            if (err) { complete(err); break; }
            xferLba = le32(in, 0);
            const int count = in[4];
            if (count == 0) { complete(USB_INT_SUCCESS); break; }
            // §5.38: "Each sector is broken into eight 64-byte data blocks."
            remaining = (uint32_t)count * 8;
            block = 0;
            if (cmd == DISK_READ) {
                xfer = Xfer::DiskRead;
                if (m->card->read(xferLba, 1, sector) != 0) { xfer = Xfer::None; complete(USB_INT_DISK_ERR); break; }
                diskReadStep();
            } else {
                xfer = Xfer::DiskWrite;
                if (xferLba >= m->card->totalSectors()) { xfer = Xfer::None; complete(USB_INT_DISK_ERR); break; }
                hostLen = 0;
                complete(USB_INT_DISK_WRITE);
            }
            break;
        }
        case DISK_RD_GO:
            if (xfer != Xfer::DiskRead) { complete(USB_INT_SUCCESS); break; }
            remaining -= 1;
            block += 1;
            if (remaining == 0) { xfer = Xfer::None; complete(USB_INT_SUCCESS); break; }
            if (block == 8) {
                block = 0;
                xferLba += 1;
                Medium* m = medium();
                if (!m || m->card->read(xferLba, 1, sector) != 0) { xfer = Xfer::None; complete(USB_INT_DISK_ERR); break; }
            }
            diskReadStep();
            break;
        case DISK_WR_GO:
            if (xfer != Xfer::DiskWrite) { complete(USB_INT_SUCCESS); break; }
            diskWriteStep();
            break;
        case DISK_SIZE: {
            // Datasheet II §1.23: sectors, then bytes a sector, both high byte first.
            Medium* m = nullptr;
            const uint8_t err = mountCheck(m);
            if (err) { complete(err); break; }
            uint8_t probe[512];
            m->card->read(0, 1, probe);
            const uint32_t last = m->card->totalSectors() - 1;
            const uint8_t d[8] = { (uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last, 0, 0, 2, 0 };
            setUsbData(d, 8);
            complete(USB_INT_SUCCESS);
            break;
        }
        case DISK_BOC_CMD: {
            // Datasheet I §5.36: a BulkOnly command for a USB disk ("not support SD card"):
            // the CBW was written with WR_HOST_DATA, data comes back through RD_USB_DATA0.
            // The CBW (CH376INC.H BULK_ONLY_CBW, docs/hw_CH376INC.h): 00H signature, 04H tag,
            // 08H the data length ("for input data 0 to 48, for output data 0 to 33": the
            // 64-byte buffer less the CBW's 31 bytes takes the output data after it), 0CH
            // flags (bit 7 = data in), 0DH LUN, 0EH the command block's length (1-16), 0FH
            // the command block.
            if (mode < 5) { complete(USB_INT_DISK_ERR); break; }
            Medium* m = nullptr;
            const uint8_t err = mountCheck(m);
            if (err) { complete(err); break; }
            const uint32_t len = le32(hostData, 8);
            const bool dataIn = (hostData[12] & 0x80) != 0;
            const int cbLen = hostData[14];
            const uint8_t* cb = hostData + 15;
            if (hostLen < 31 || le32(hostData, 0) != 0x43425355u || cbLen < 1 || cbLen > 16 ||
                len > (dataIn ? 48u : 33u) || hostLen < 31 + (dataIn ? 0 : (int)len)) {
                complete(USB_INT_DISK_ERR);
                break;
            }
            // SCSI (SPC / SBC) on a 512-byte-sector disk
            uint8_t reply[48] = {};
            int n = 0;
            bool ok = true;
            const uint32_t total = m->card->totalSectors();
            switch (cb[0]) {
            case 0x00: case 0x1b: case 0x1e: break;                 // TEST UNIT READY, START STOP, PREVENT ALLOW
            case 0x03: {                                            // REQUEST SENSE: fixed format, 18 bytes
                const uint8_t s[18] = { 0x70, 0, bocSenseKey, 0, 0, 0, 0, 10, 0, 0, 0, 0, bocAsc, 0 };
                std::memcpy(reply, s, 18); n = 18;
                bocSenseKey = bocAsc = 0;
                break;
            }
            case 0x12: {                                            // INQUIRY: the 36 bytes DISK_MOUNT gives
                uint8_t q[36] = { 0x00, 0x80, 0x02, 0x02, 0x1f, 0, 0, 0 };
                std::memcpy(q + 8, "CPCSE   ", 8);
                std::memcpy(q + 16, "Albireo USB disk", 16);
                std::memcpy(q + 32, "1.00", 4);
                std::memcpy(reply, q, 36); n = 36;
                break;
            }
            case 0x25: {                                            // READ CAPACITY (10): last LBA, block size
                const uint32_t last = total - 1;
                const uint8_t c[8] = { (uint8_t)(last >> 24), (uint8_t)(last >> 16), (uint8_t)(last >> 8), (uint8_t)last, 0, 0, 2, 0 };
                std::memcpy(reply, c, 8); n = 8;
                break;
            }
            case 0x1a: { const uint8_t h[4] = { 3, 0, 0, 0 }; std::memcpy(reply, h, 4); n = 4; break; }               // MODE SENSE (6): header, writable
            case 0x5a: { const uint8_t h[8] = { 0, 6, 0, 0, 0, 0, 0, 0 }; std::memcpy(reply, h, 8); n = 8; break; }   // MODE SENSE (10)
            case 0x28: case 0x2a: {                                 // READ (10), WRITE (10)
                const uint32_t lba = (uint32_t)cb[2] << 24 | cb[3] << 16 | cb[4] << 8 | cb[5];
                const uint32_t blocks = (uint32_t)(cb[7] << 8 | cb[8]);
                if (lba + blocks > total) { ok = false; bocSenseKey = 5; bocAsc = 0x21; break; }   // LBA out of range
                // a sector is 512 bytes, more than the 48 / 33 this command can carry: only
                // a transfer of no blocks fits (DISK_READ / DISK_WRITE move sectors)
                if (blocks * 512u != len) { ok = false; bocSenseKey = 5; bocAsc = 0x24; }
                break;
            }
            default: ok = false; bocSenseKey = 5; bocAsc = 0x20; break;   // ILLEGAL REQUEST: invalid command
            }
            if (!ok) { complete(USB_INT_DISK_ERR); break; }
            if (dataIn) setUsbData(reply, std::min<int>(n, (int)len));
            complete(USB_INT_SUCCESS);
            break;
        }
        case DISK_INQUIRY: case DISK_READY: case DISK_RESET: case DISK_R_SENSE: case DISK_MAX_LUN: {
            Medium* m = nullptr;
            const uint8_t err = mountCheck(m);
            if (err) { complete(err); break; }
            if (cmd == DISK_R_SENSE) { const uint8_t s[18] = { 0x70, 0, 0, 0, 0, 0, 0, 10 }; setUsbData(s, 18); }
            if (cmd == DISK_MAX_LUN) { const uint8_t z = 0; setUsbData(&z, 1); }
            complete(USB_INT_SUCCESS);
            break;
        }

        // USB control transfers, made simple by the chip (Datasheet II §1.15-§1.19).
        case CLR_STALL: case SET_CONFIG: case SET_ADDRESS: case GET_DESCR: case AUTO_SETUP: {
            if (mode < 5 || usb == Usb::None) { complete(USB_INT_DISCONNECT); break; }
            if (!deviceAnswers()) { complete(USB_TIMEOUT); break; }
            if (cmd == SET_ADDRESS) deviceAddress = in[0] & 0x7f;
            if (cmd == SET_CONFIG) configured = in[0] != 0;
            if (cmd == AUTO_SETUP) { deviceAddress = hostAddress = 1; configured = true; }
            if (cmd == GET_DESCR) {
                const Bytes& d = in[0] == 1 ? deviceDescriptor() : configDescriptor();
                if (in[0] != 1 && in[0] != 2) { complete(USB_STALL); break; }
                setUsbData(d.data(), (int)d.size());
            }
            complete(USB_INT_SUCCESS);
            break;
        }
        case ISSUE_TKN_X: issueToken(in[0], in[1]); break;
        default: break;                  // a command this chip does not have: no answer
    }
}

// ---------------------------------------------------------------- mass storage

Ch376::Medium* Ch376::medium() {
    if (mode == 3) return sd.present() ? &sd : nullptr;
    if (mode >= 5 && usb == Usb::Stick && stick.present()) return &stick;
    return nullptr;
}

uint8_t Ch376::mountCheck(Medium*& m) {
    m = medium();
    if (m) return 0;
    if (mode >= 5 && usb == Usb::Mouse) return USB_INT_DISK_ERR;   // a device, but not storage
    return ERR_DISK_DISCON;
}

void Ch376::closeFiles() {
    if (opened == Open::File && contentDirty) saveFile();
    opened = Open::None;
    openPath.clear();
    content.clear();
    contentDirty = false;
    sizeVar = dirLength = pointer = 0;
    enumRows.clear();
    enumNames.clear();
    enumIndex = 0;
    xfer = Xfer::None;
}

void Ch376::mountState() {
    closeFiles();
    cwd = "/";
}

bool Ch376::syncRaw(Medium& m) {
    if (m.card && m.card->dirty()) m.card->syncToHost();
    return true;
}

// The open file into its folder: the bytes up to the directory entry's length (a file
// closed without updating its length keeps its old one, §5.22).
void Ch376::saveFile() {
    Medium* m = medium();
    if (!m || opened != Open::File) return;
    Bytes bytes = content;
    bytes.resize(dirLength, 0);
    m->fs->writeFile(openPath, bytes);
    contentDirty = false;
}

static std::string trimRight(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
// "NAME    EXT" -> "NAME.EXT"
static std::string dotted(const std::string& e11) {
    const std::string base = trimRight(e11.substr(0, 8)), ext = trimRight(e11.substr(8, 3));
    return ext.empty() ? base : base + "." + ext;
}
static std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return s;
}
static std::string join(const std::string& dir, const std::string& leaf) { return (dir == "/" ? "" : dir) + "/" + leaf; }
// A SET_FILE_NAME name as the 8.3 name it stands for: the part before the last '.' (at most
// 8 characters) and after it (at most 3), the spaces that pad either dropped. UniDOS gives
// its names padded ("TEST    .$$$", "TEST    .   " for no extension); in a FAT entry the
// padding is what fills the 11 characters, so the chip's name is "TEST.$$$" and "TEST".
static std::string shortForm(const std::string& name) {
    const std::string u = upper(name);
    const size_t dot = u.rfind('.');
    if (u == "." || u == ".." || dot == std::string::npos) return trimRight(u.substr(0, 8));
    const std::string base = trimRight(u.substr(0, std::min<size_t>(dot, 8))), ext = trimRight(u.substr(dot + 1, 3));
    return ext.empty() ? base : base + "." + ext;
}

// A name in a directory, by its short name (what the CPC sees) or its own name.
std::string Ch376::resolve(const std::string& dirReal, const std::string& name, M4Entry* entry) {
    Medium* m = medium();
    if (!m) return "";
    const std::string want = shortForm(name);
    if (want == "." ) { if (entry) { *entry = M4Entry{}; entry->dir = true; } return dirReal; }
    if (want == "..") {
        const std::string up = dirReal == "/" ? "/" : M4Storage::parentOf(dirReal);
        if (entry) { *entry = M4Entry{}; entry->dir = true; }
        return up.empty() ? "/" : up;
    }
    const std::vector<M4Entry> rows = m->fs->list(dirReal);
    const std::vector<std::string> names = M4Storage::shortNames(rows);
    for (size_t i = 0; i < rows.size(); i++) {
        if (dotted(names[i]) == want || upper(rows[i].name) == upper(name)) {
            if (entry) *entry = rows[i];
            return join(dirReal, rows[i].name);
        }
    }
    return "";
}

// The directory a SET_FILE_NAME string names its object in, and the object's name. A
// leading '/' or '\' is the root (§5.15); without one it is the current directory.
std::string Ch376::dirOf(const std::string& name, std::string& leaf) {
    if (!name.empty() && (name[0] == '/' || name[0] == '\\')) { leaf = name.substr(1); return "/"; }
    leaf = name;
    return cwd;
}

void Ch376::fillDirInfo(uint8_t* p, const M4Entry& e, const std::string& e11) {
    std::memset(p, 0, 32);
    std::memcpy(p, e11.data(), std::min<size_t>(11, e11.size()));
    p[11] = (uint8_t)((e.dir ? 0x10 : 0x20) | (e.readOnly ? 0x01 : 0));
    auto put16 = [&](int at, int v) { p[at] = (uint8_t)v; p[at + 1] = (uint8_t)(v >> 8); };
    put16(14, e.fatTime); put16(16, e.fatDate); put16(18, e.fatDate);
    put16(22, e.fatTime); put16(24, e.fatDate);
    const uint32_t size = e.dir ? 0 : (uint32_t)std::min<uint64_t>(e.size, 0xffffffffu);
    for (int k = 0; k < 4; k++) p[28 + k] = (uint8_t)(size >> (8 * k));
}

void Ch376::fileOpen() {
    Medium* m = nullptr;
    const uint8_t err = mountCheck(m);
    if (err) { complete(err); return; }
    syncRaw(*m);
    if (opened == Open::File && contentDirty) saveFile();
    xfer = Xfer::None;
    // §5.18: "" initialises the file system and opens nothing; "/" opens the root.
    if (fileName.empty()) { opened = Open::None; complete(USB_INT_SUCCESS); return; }
    std::string leaf;
    const std::string dir = dirOf(fileName, leaf);
    if (leaf.empty()) {                                         // "/"
        cwd = "/"; opened = Open::Dir; openPath = "/";
        sizeVar = 0xffffffffu;
        complete(ERR_OPEN_DIR);
        return;
    }
    if (!leaf.empty() && leaf.back() == '*') {
        // §5.19: enumerate the entries whose name starts with what comes before the '*'.
        // A padded base before the '.' ("TEST    .B*") is the name's base, as above.
        std::string before = upper(leaf.substr(0, leaf.size() - 1));
        const size_t dot = before.find('.');
        if (dot != std::string::npos) before = trimRight(before.substr(0, dot)) + before.substr(dot);
        enumPrefix = before;
        enumRows.clear(); enumNames.clear();
        if (dir != "/") {                                       // a sub-directory's "." and ".."
            M4Entry d; d.dir = true;
            m->fs->stat(dir, d); d.dir = true;
            enumRows.push_back(d); enumNames.push_back(".          ");
            enumRows.push_back(d); enumNames.push_back("..         ");
        }
        const std::vector<M4Entry> rows = m->fs->list(dir);
        const std::vector<std::string> names = M4Storage::shortNames(rows);
        enumRows.insert(enumRows.end(), rows.begin(), rows.end());
        enumNames.insert(enumNames.end(), names.begin(), names.end());
        enumIndex = 0;
        enumNext();
        return;
    }
    M4Entry e;
    const std::string real = resolve(dir, leaf, &e);
    if (real.empty()) { opened = Open::None; complete(ERR_MISS_FILE); return; }
    if (e.dir) {
        cwd = real; opened = Open::Dir; openPath = real;
        sizeVar = 0xffffffffu;
        complete(ERR_OPEN_DIR);
        return;
    }
    content.clear();
    m->fs->readFile(real, content);
    opened = Open::File;
    openPath = real;
    sizeVar = dirLength = (uint32_t)std::min<size_t>(content.size(), 0xffffffffu);
    pointer = 0;
    contentDirty = false;
    complete(USB_INT_SUCCESS);
}

void Ch376::enumNext() {
    while (enumIndex < enumRows.size()) {
        const size_t i = enumIndex++;
        if (dotted(enumNames[i]).compare(0, enumPrefix.size(), enumPrefix) != 0) continue;
        uint8_t info[32];
        fillDirInfo(info, enumRows[i], enumNames[i]);
        setUsbData(info, 32);
        complete(USB_INT_DISK_READ);
        return;
    }
    enumRows.clear(); enumNames.clear();
    complete(ERR_MISS_FILE);
}

void Ch376::fileCreate() {
    Medium* m = nullptr;
    const uint8_t err = mountCheck(m);
    if (err) { complete(err); return; }
    syncRaw(*m);
    closeFiles();
    std::string leaf;
    const std::string dir = dirOf(fileName, leaf);
    if (leaf.empty() || leaf.find('*') != std::string::npos) { complete(ERR_MISS_FILE); return; }
    M4Entry e;
    std::string real = resolve(dir, leaf, &e);
    if (!real.empty() && e.dir) { complete(ERR_FOUND_NAME); return; }
    // a new name through the folder's own checks (a name with '\', "..", ':' or a device's
    // name is refused: the program's name must not reach outside the folder)
    if (real.empty() && m->fs->resolveForCreate(join(dir, shortForm(leaf)), real) != M4_FR_OK) { complete(ERR_MISS_FILE); return; }
    // §5.20: an existing file is deleted first; "the default file length is 1".
    content.assign(1, 0);
    if (m->fs->writeFile(real, content) != M4_FR_OK) { complete(ERR_DISK_FULL); return; }
    opened = Open::File;
    openPath = real;
    sizeVar = dirLength = 1;
    pointer = 0;
    contentDirty = false;
    complete(USB_INT_SUCCESS);
}

void Ch376::fileErase() {
    Medium* m = nullptr;
    const uint8_t err = mountCheck(m);
    if (err) { complete(err); return; }
    syncRaw(*m);
    // §5.21: an open file (or directory) is deleted as it is; else the name is opened first.
    std::string target;
    if (opened != Open::None && openPath != "/") target = openPath;
    else {
        std::string leaf;
        const std::string dir = dirOf(fileName, leaf);
        M4Entry e;
        target = resolve(dir, leaf, &e);
        if (target.empty() || e.dir) { complete(ERR_MISS_FILE); return; }   // a directory must be opened first
    }
    const bool wasDir = opened == Open::Dir;
    opened = Open::None; contentDirty = false; content.clear();
    if (m->fs->remove(target) != M4_FR_OK) { complete(USB_INT_DISK_ERR); return; }
    if (wasDir) { const std::string up = M4Storage::parentOf(target); cwd = up.empty() ? "/" : up; }
    complete(USB_INT_SUCCESS);
}

void Ch376::fileClose(int update) {
    // §5.22: 1 lets the chip write the file length, 0 keeps the length on the disk.
    if (opened == Open::File) {
        if (update) dirLength = sizeVar;
        if (contentDirty || update) saveFile();
    }
    opened = Open::None;
    openPath.clear();
    content.clear();
    contentDirty = false;
    xfer = Xfer::None;
    complete(USB_INT_SUCCESS);
}

void Ch376::dirInfoRead(int index) {
    Medium* m = nullptr;
    const uint8_t err = mountCheck(m);
    if (err) { complete(err); return; }
    // §5.23: 0FFH is the open file's entry; 0-15 the entries kept around it (here the first
    // sixteen of its directory, in the volume's order).
    std::string dir = cwd;
    if (opened == Open::File) { const std::string up = M4Storage::parentOf(openPath); dir = up.empty() ? "/" : up; }
    const std::vector<M4Entry> rows = m->fs->list(dir);
    const std::vector<std::string> names = M4Storage::shortNames(rows);
    size_t pick = rows.size();
    if (index == 0xff) {
        if (opened != Open::File) { complete(ERR_FILE_CLOSE); return; }
        for (size_t i = 0; i < rows.size(); i++) if (M4Storage::lower(join(dir, rows[i].name)) == M4Storage::lower(openPath)) pick = i;
    } else if (index < 16) pick = (size_t)index;
    if (pick >= rows.size()) { complete(ERR_MISS_FILE); return; }
    M4Entry e = rows[pick];
    if (index == 0xff) e.size = dirLength;
    fillDirInfo(dirInfo, e, names[pick]);
    setUsbData(dirInfo, 32);
    complete(USB_INT_SUCCESS);
}

void Ch376::dirInfoSave() {
    // §5.24: the entry edited with WR_OFS_DATA goes back to the disk. What a folder can keep
    // of it: the length, the name (a program renames a file this way, as the CH376 has no
    // rename command: UniDOS's SAVE writes TEST.$$$ and renames it TEST.BAS), and E5H in the
    // first byte, FAT's mark of a deleted entry.
    if (opened != Open::File) { complete(ERR_FILE_CLOSE); return; }
    Medium* m = medium();
    if (!m) { complete(ERR_DISK_DISCON); return; }
    dirLength = sizeVar = capFileSize((uint32_t)dirInfo[28] | (uint32_t)dirInfo[29] << 8 | (uint32_t)dirInfo[30] << 16 | (uint32_t)dirInfo[31] << 24);
    saveFile();
    const std::string up = M4Storage::parentOf(openPath), dir = up.empty() ? "/" : up;
    if (dirInfo[0] == 0xe5) {
        m->fs->remove(openPath);
        opened = Open::None; openPath.clear(); content.clear();
        complete(USB_INT_SUCCESS);
        return;
    }
    const std::string wanted = dotted(std::string((const char*)dirInfo, 11));
    std::string current;                                    // the file's own short name now
    {
        const std::vector<M4Entry> rows = m->fs->list(dir);
        const std::vector<std::string> names = M4Storage::shortNames(rows);
        for (size_t i = 0; i < rows.size(); i++)
            if (M4Storage::lower(join(dir, rows[i].name)) == M4Storage::lower(openPath)) current = dotted(names[i]);
    }
    if (!wanted.empty() && current != wanted) {
        // the entry the folder has under the new name, if another: the program's new entry
        // takes its place (a folder cannot hold two of one name)
        M4Entry other;
        const std::string clash = resolve(dir, wanted, &other);
        if (!clash.empty() && M4Storage::lower(clash) != M4Storage::lower(openPath)) m->fs->remove(clash);
        if (m->fs->rename(openPath, join(dir, wanted)) == M4_FR_OK) {
            const std::string moved = m->fs->find(join(dir, wanted));   // where it really is now
            if (!moved.empty()) openPath = moved;
        }
    }
    complete(USB_INT_SUCCESS);
}

void Ch376::dirCreate() {
    Medium* m = nullptr;
    const uint8_t err = mountCheck(m);
    if (err) { complete(err); return; }
    syncRaw(*m);
    closeFiles();
    std::string leaf;
    const std::string dir = dirOf(fileName, leaf);
    if (leaf.empty()) { complete(ERR_MISS_FILE); return; }
    M4Entry e;
    std::string real = resolve(dir, leaf, &e);
    if (!real.empty() && !e.dir) { complete(ERR_FOUND_NAME); return; }   // §5.32
    if (real.empty()) {
        const std::string wanted = join(dir, shortForm(leaf));
        if (m->fs->makeDir(wanted) != M4_FR_OK) { complete(ERR_DISK_FULL); return; }
        real = m->fs->find(wanted);                   // the folder's own path for it
        if (real.empty()) { complete(ERR_DISK_FULL); return; }
    }
    cwd = real; opened = Open::Dir; openPath = real;
    sizeVar = 0xffffffffu;
    complete(USB_INT_SUCCESS);
}

// The volume's sector holding the file pointer, or FFFFFFFFH at the end of the file.
uint32_t Ch376::lbaOfPointer() {
    Medium* m = medium();
    if (!m || opened != Open::File || pointer >= sizeVar) return 0xffffffffu;
    if (contentDirty) { dirLength = std::max(dirLength, sizeVar); saveFile(); }
    uint32_t first = 0;
    if (!m->card->fileStartSector(openPath, first)) return 0xffffffffu;
    return first + pointer / 512;
}

void Ch376::byteLocate(uint32_t offset) {
    if (opened != Open::File) { complete(ERR_FILE_CLOSE); return; }
    xfer = Xfer::None;
    // §5.25: a large offset (FFFFFFFFH) moves to the end of the file, to append.
    pointer = std::min(offset, sizeVar);
    const uint32_t lba = lbaOfPointer();
    const uint8_t d[4] = { (uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16), (uint8_t)(lba >> 24) };
    setUsbData(d, 4);
    complete(USB_INT_SUCCESS);
}

// §5.27: each step gives what is left of the request, as much as the file holds, up to the
// 255 bytes a data block can be (§5.11), and moves the pointer; at the end USB_INT_SUCCESS.
void Ch376::byteReadStep() {
    const uint32_t left = pointer < sizeVar ? sizeVar - pointer : 0;
    const uint32_t n = std::min<uint32_t>({ remaining, left, 255u });
    if (n == 0) { xfer = Xfer::None; usbLen = 0; complete(USB_INT_SUCCESS); return; }
    uint8_t chunk[255];
    for (uint32_t k = 0; k < n; k++) chunk[k] = pointer + k < content.size() ? content[pointer + k] : 0;
    setUsbData(chunk, (int)n);
    pointer += n;
    remaining -= n;
    complete(USB_INT_DISK_READ);
}

// §5.29: each step asks for up to 255 bytes with USB_INT_DISK_WRITE (WR_REQ_DATA takes
// them), moves the pointer and grows the length variable when the write goes past the end.
void Ch376::byteWriteStep() {
    if (remaining == 0) { xfer = Xfer::None; complete(USB_INT_SUCCESS); return; }
    const uint32_t n = std::min<uint32_t>(remaining, 255u);
    if ((uint64_t)pointer + n > MAX_FILE_BYTES) { xfer = Xfer::None; remaining = 0; complete(ERR_DISK_FULL); return; }
    writeAt = pointer;
    reqLen = (int)n;
    reqGot = 0;
    pointer += n;
    remaining -= n;
    if (pointer > sizeVar) sizeVar = pointer;
    if (content.size() < pointer) content.resize(pointer, 0);
    contentDirty = true;
    complete(USB_INT_DISK_WRITE);
}

void Ch376::secLocate(uint32_t sectors) {
    if (opened != Open::File) { complete(ERR_FILE_CLOSE); return; }
    const uint64_t at = (uint64_t)sectors * 512;
    pointer = at >= sizeVar ? sizeVar : (uint32_t)at;
    const uint32_t lba = lbaOfPointer();
    const uint8_t d[4] = { (uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16), (uint8_t)(lba >> 24) };
    setUsbData(d, 4);
    complete(USB_INT_SUCCESS);
}

// §5.34 / §5.35: 8 bytes back: the sectors allowed, then (bytes 4-7) the first one's LBA.
// A write that goes past the end grows the file by whole sectors first, so the sectors the
// program then writes with DISK_WRITE are the file's.
void Ch376::secRead(int count, bool write) {
    if (opened != Open::File) { complete(ERR_FILE_CLOSE); return; }
    Medium* m = medium();
    if (!m) { complete(ERR_DISK_DISCON); return; }
    if (write && count == 0) { dirLength = sizeVar; saveFile(); complete(USB_INT_SUCCESS); return; }
    pointer &= ~511u;
    uint32_t allowed;
    if (write) {
        allowed = (uint32_t)count;
        if ((uint64_t)pointer + (uint64_t)allowed * 512 > MAX_FILE_BYTES) { complete(ERR_DISK_FULL); return; }
        const uint32_t end = pointer + allowed * 512;
        if (end > sizeVar) { sizeVar = end; content.resize(end, 0); }
        dirLength = std::max(dirLength, sizeVar);
        contentDirty = true;
        saveFile();
    } else {
        const uint32_t sectorsLeft = pointer < sizeVar ? (sizeVar - pointer + 511) / 512 : 0;
        allowed = std::min<uint32_t>((uint32_t)count, sectorsLeft);
    }
    uint32_t lba = 0;
    if (allowed && !m->card->fileStartSector(openPath, lba)) allowed = 0;
    lba += pointer / 512;
    pointer = std::min<uint32_t>(pointer + allowed * 512, sizeVar);
    const uint8_t d[8] = { (uint8_t)allowed, 0, 0, 0, (uint8_t)lba, (uint8_t)(lba >> 8), (uint8_t)(lba >> 16), (uint8_t)(lba >> 24) };
    setUsbData(d, 8);
    complete(USB_INT_SUCCESS);
}

void Ch376::diskQuery(bool capacityOnly) {
    Medium* m = nullptr;
    const uint8_t err = mountCheck(m);
    if (err) { complete(err); return; }
    uint8_t probe[512];
    m->card->read(0, 1, probe);                 // the volume is laid out on first use
    const uint32_t total = m->card->totalSectors();
    if (capacityOnly) {
        // §5.30: the total number of sectors, low byte first.
        const uint8_t d[4] = { (uint8_t)total, (uint8_t)(total >> 8), (uint8_t)(total >> 16), (uint8_t)(total >> 24) };
        setUsbData(d, 4);
    } else {
        // §5.31: the logical disk's sectors, its free sectors, its FAT type (CH376INC.H:
        // 1 FAT12, 2 FAT16, 3 FAT32). The volume is FAT16, after a 2048-sector gap.
        const uint32_t logical = total > 2048 ? total - 2048 : 0;
        const uint64_t used = m->fs->usedBytes() / 512;
        const uint64_t cap = m->card->capacityBytes() / 512;
        const uint32_t freeSectors = (uint32_t)(cap > used ? cap - used : 0);
        const uint8_t d[9] = { (uint8_t)logical, (uint8_t)(logical >> 8), (uint8_t)(logical >> 16), (uint8_t)(logical >> 24),
                               (uint8_t)freeSectors, (uint8_t)(freeSectors >> 8), (uint8_t)(freeSectors >> 16), (uint8_t)(freeSectors >> 24), 0x02 };
        setUsbData(d, 9);
    }
    complete(USB_INT_SUCCESS);
}

void Ch376::diskReadStep() {
    setUsbData(sector + block * 64, 64);
    complete(USB_INT_DISK_READ);
}

// §5.40: each step takes the block written with WR_HOST_DATA; a sector is written when its
// eighth block is in.
void Ch376::diskWriteStep() {
    std::memset(sector + block * 64, 0, 64);
    std::memcpy(sector + block * 64, hostData, (size_t)std::min(hostLen, 64));
    hostLen = 0;
    block += 1;
    remaining -= 1;
    if (block == 8) {
        Medium* m = medium();
        if (!m || m->card->write(xferLba, 1, sector) != 0) { xfer = Xfer::None; complete(USB_INT_DISK_ERR); return; }
        lastRawWrite = now;
        block = 0;
        xferLba += 1;
    }
    if (remaining == 0) { xfer = Xfer::None; complete(USB_INT_SUCCESS); return; }
    complete(USB_INT_DISK_WRITE);
}

// ---------------------------------------------------------------- the USB device

// A low-speed HID boot mouse (USB 1.1 §9.6; HID 1.11 appendix B.2 and E.10): 8-byte
// endpoint 0, one interface of class 3 subclass 1 (boot) protocol 2 (mouse), an interrupt
// IN endpoint 1 of 4 bytes polled every 10 ms. The vendor ID is pid.codes' open one.
static const Bytes MOUSE_DEVICE = { 0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x08, 0x09, 0x12, 0x01, 0x00,
                                    0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
// Three buttons, X, Y and a wheel, each one byte (HID 1.11 E.10, with the wheel usage 38H).
static const Bytes MOUSE_REPORT = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
                                    0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
                                    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30,
                                    0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03,
                                    0x81, 0x06, 0xc0, 0xc0 };
static const Bytes MOUSE_CONFIG = { 0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
                                    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
                                    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, (uint8_t)MOUSE_REPORT.size(), 0x00,
                                    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x0a };
// A full-speed USB stick: class 8 (mass storage), subclass 6 (SCSI), protocol 50H (bulk only).
static const Bytes STICK_DEVICE = { 0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x09, 0x12, 0x02, 0x00,
                                    0x00, 0x01, 0x00, 0x00, 0x00, 0x01 };
static const Bytes STICK_CONFIG = { 0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
                                    0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
                                    0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
                                    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00 };

const Bytes& Ch376::deviceDescriptor() const { return usb == Usb::Mouse ? MOUSE_DEVICE : STICK_DEVICE; }
const Bytes& Ch376::configDescriptor() const { return usb == Usb::Mouse ? MOUSE_CONFIG : STICK_CONFIG; }

// Datasheet II §1.20: bits 7/6 of the first byte the receive / send toggles, the second
// byte's low 4 bits the PID (0DH SETUP, 01H OUT, 09H IN), high 4 bits the endpoint.
void Ch376::issueToken(int, int attribute) {
    const int pid = attribute & 0x0f, endpoint = attribute >> 4;
    waitingIn = false;
    if (mode < 5 || usb == Usb::None) { complete(USB_INT_DISCONNECT); return; }
    // Mode 7 holds the bus in reset (§5.9): the device cannot answer until the mode changes.
    if (mode == 7 || !deviceAnswers()) { complete(USB_TIMEOUT); return; }
    if (endpoint == 0) {
        if (pid == 0x0d) { setupPacket(); return; }
        if (pid == 0x09) {                                       // the data stage, IN
            const size_t packet = usb == Usb::Mouse ? 8 : 64;
            const size_t n = std::min(packet, controlIn.size() - std::min(controlPos, controlIn.size()));
            setUsbData(controlIn.data() + controlPos, (int)n);
            controlPos += n;
            complete(USB_INT_SUCCESS);
            return;
        }
        if (pid == 0x01) { usbLen = 0; complete(USB_INT_SUCCESS); return; }   // OUT: a status stage or data out
        complete(USB_STALL);
        return;
    }
    if (usb == Usb::Mouse && endpoint == 1 && pid == 0x09 && configured) {
        if (mouseReport()) return;
        // Nothing to report: the mouse NAKs. SET_RETRY bits 7-6 (Datasheet II §1.4): 10 retry
        // until it answers (ABORT_NAK gives up), 11 retry for about 3 seconds, 0x hand the
        // NAK to the program.
        if ((retry & 0xc0) == 0x80) { waitingIn = true; nakDeadline = 0; return; }
        if ((retry & 0xc0) == 0xc0) { waitingIn = true; nakDeadline = now + 3000 * CYCLES_PER_MS; return; }
        complete(USB_NAK);
        return;
    }
    complete(USB_STALL);                                         // an endpoint the device does not have
}

void Ch376::setupPacket() {
    // The 8 bytes WR_HOST_DATA put in the send buffer: bmRequestType, bRequest, wValue,
    // wIndex, wLength (USB 1.1 §9.3).
    const uint8_t* s = hostData;
    if (hostLen < 8) { complete(USB_STALL); return; }
    const int type = s[0], request = s[1], value = s[2] | s[3] << 8, length = s[6] | s[7] << 8;
    controlIn.clear();
    controlPos = 0;
    bool ok = true;
    if (type == 0x80 && request == 0x06) {                       // GET_DESCRIPTOR
        const int kind = value >> 8;
        if (kind == 1) controlIn = deviceDescriptor();
        else if (kind == 2) controlIn = configDescriptor();
        else ok = false;
    } else if (type == 0x81 && request == 0x06 && (value >> 8) == 0x22 && usb == Usb::Mouse) {
        controlIn = MOUSE_REPORT;                                // the HID report descriptor
    } else if (type == 0x00 && request == 0x05) {
        deviceAddress = value & 0x7f;                            // SET_ADDRESS
    } else if (type == 0x00 && request == 0x09) {
        configured = (value & 0xff) != 0;                        // SET_CONFIGURATION
    } else if (type == 0x80 && request == 0x00) {
        controlIn = { 0x00, 0x00 };                              // GET_STATUS
    } else if (type == 0x80 && request == 0x08) {
        controlIn = { (uint8_t)(configured ? 1 : 0) };           // GET_CONFIGURATION
    } else if (usb == Usb::Mouse && type == 0x21 && request == 0x0b) {
        protocol = value & 1;                                    // HID SET_PROTOCOL
    } else if (usb == Usb::Mouse && type == 0x21 && (request == 0x0a || request == 0x09)) {
        // SET_IDLE, SET_REPORT: nothing a mouse keeps
    } else if (usb == Usb::Mouse && type == 0xa1 && request == 0x03) {
        controlIn = { (uint8_t)protocol };                       // HID GET_PROTOCOL
    } else if (usb == Usb::Stick && type == 0xa1 && request == 0xfe) {
        controlIn = { 0x00 };                                    // GET_MAX_LUN: one unit
    } else if (usb == Usb::Stick && type == 0x21 && request == 0xff) {
        // Bulk-Only Mass Storage Reset
    } else if (type == 0x02 && request == 0x01) {
        // CLEAR_FEATURE (ENDPOINT_HALT)
    } else ok = false;
    if (!ok) { complete(USB_STALL); return; }
    if ((int)controlIn.size() > length) controlIn.resize((size_t)length);
    complete(USB_INT_SUCCESS);
}

// A report when the mouse has something to say: buttons, X, Y (each -127..127 of what has
// moved since the last), and in report protocol the wheel.
bool Ch376::mouseReport() {
    if (mouseDx == 0 && mouseDy == 0 && mouseWheelCount == 0 && mouseButtons == sentButtons) return false;
    const int x = std::clamp(mouseDx, -127, 127), y = std::clamp(mouseDy, -127, 127), w = std::clamp(mouseWheelCount, -127, 127);
    mouseDx -= x; mouseDy -= y; mouseWheelCount -= w;
    sentButtons = mouseButtons;
    const uint8_t report[4] = { (uint8_t)mouseButtons, (uint8_t)x, (uint8_t)y, (uint8_t)w };
    setUsbData(report, protocol == 0 ? 3 : 4);
    complete(USB_INT_SUCCESS);
    return true;
}

void Ch376::mouseMove(int dx, int dy) {
    if (usb != Usb::Mouse) return;
    mouseDx += dx; mouseDy += dy;
    if (waitingIn && mouseReport()) waitingIn = false;   // a NAKed IN token gets its answer now
}

void Ch376::mouseButton(int index, bool pressed) {
    if (usb != Usb::Mouse || index < 0 || index > 2) return;
    if (pressed) mouseButtons |= 1 << index; else mouseButtons &= ~(1 << index);
    if (waitingIn && mouseReport()) waitingIn = false;   // a NAKed IN token gets its answer now
}

void Ch376::mouseWheel(int notches) {
    if (usb != Usb::Mouse) return;
    mouseWheelCount += notches;
    if (waitingIn && mouseReport()) waitingIn = false;   // a NAKed IN token gets its answer now
}

} // namespace cpcse
