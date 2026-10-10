// CPCSyntaxError — Amstrad's RS232C interface: a Z80 DART and an 8253. See rs232.h.
#include "rs232.h"

namespace cpcse {

// ------------------------------------------------------------------ the 8253
// Intel's 8253 data sheet: the control word is SC (bits 7-6: the counter), RW (5-4: 00
// latch, 01 LSB, 10 MSB, 11 LSB then MSB), M (3-1: the mode; 6 and 7 are 2 and 3 again) and
// BCD (0). A count of 0 is the largest, 65536 (binary) or 10000 (BCD). The card's GATE
// inputs are held high, so modes 1 and 5 (started by a GATE edge) never start.

namespace {
int fromBcd(int v) { return (v >> 12 & 15) * 1000 + (v >> 8 & 15) * 100 + (v >> 4 & 15) * 10 + (v & 15); }
int toBcd(int v) { return (v / 1000 % 10) << 12 | (v / 100 % 10) << 8 | (v / 10 % 10) << 4 | v % 10; }
}

void Intel8253::reset() { c = {}; }

int Intel8253::decrement(const Counter& k, int by) const {
    const int range = k.bcd ? 10000 : 65536;
    return ((k.count - by) % range + range) % range;
}

int Intel8253::divisor(int i) const {
    const Counter& k = c[(size_t)i];
    if (!k.loaded) return 0;
    return k.reload ? k.reload : (k.bcd ? 10000 : 65536);
}

void Intel8253::load(Counter& k, int value) {
    k.reload = k.bcd ? fromBcd(value) : value;
    const bool first = !k.loaded;
    k.loaded = true;
    switch (k.mode) {
    case 0: k.out = false; k.armed = true; k.pendingLoad = k.reload; break;   // counting restarts
    case 4: k.out = true; k.armed = true; k.pendingLoad = k.reload; break;
    case 2: case 3: if (first) k.pendingLoad = k.reload; break;          // later counts at the next reload
    default: break;                                                      // 1, 5: wait for GATE
    }
}

void Intel8253::write(int reg, int value) {
    reg &= 3;
    value &= 0xff;
    if (reg == 3) {
        const int sc = value >> 6;
        if (sc == 3) return;                                 // the 8254's read-back; nothing on an 8253
        Counter& k = c[(size_t)sc];
        const int rw = value >> 4 & 3;
        if (rw == 0) {                                       // counter latch
            if (!k.latched) { k.latched = true; k.latch = k.count; }
            return;
        }
        int mode = value >> 1 & 7;
        if (mode > 5) mode -= 4;
        k = Counter{};
        k.mode = mode;
        k.access = rw;
        k.bcd = value & 1;
        k.out = mode != 0;
        return;
    }
    Counter& k = c[(size_t)reg];
    if (k.access == 1) load(k, value);
    else if (k.access == 2) load(k, value << 8);
    else if (!k.writeHigh) { k.writeLow = value; k.writeHigh = true; }
    else { k.writeHigh = false; load(k, k.writeLow | value << 8); }
}

int Intel8253::read(int reg) {
    reg &= 3;
    if (reg == 3) return 0xff;
    Counter& k = c[(size_t)reg];
    int v = k.latched ? k.latch : k.count;
    if (k.bcd) v = toBcd(v);
    int out;
    if (k.access == 1) { out = v & 0xff; k.latched = false; }
    else if (k.access == 2) { out = v >> 8 & 0xff; k.latched = false; }
    else if (!k.readHigh) { out = v & 0xff; k.readHigh = true; }
    else { out = v >> 8 & 0xff; k.readHigh = false; k.latched = false; }
    return out;
}

int Intel8253::clock() {
    int rose = 0;
    for (int i = 0; i < 3; i++) {
        Counter& k = c[(size_t)i];
        const bool before = k.out;
        const int full = k.reload ? k.reload : (k.bcd ? 10000 : 65536);
        if (k.pendingLoad >= 0) {                            // the clock that loads the count
            k.count = k.mode == 3 ? (full & 1 ? full + 1 : full) : full;
            if (k.count >= (k.bcd ? 10000 : 65536) && (k.mode == 0 || k.mode == 4)) k.count = 0;   // wraps from 0
            k.pendingLoad = -1;
        } else if (k.loaded) {
            switch (k.mode) {
            case 0:
                k.count = decrement(k, 1);
                if (k.armed && k.count == 0) { k.out = true; k.armed = false; }
                break;
            case 4:
                if (!k.out) k.out = true;                    // the strobe is one clock long
                k.count = decrement(k, 1);
                if (k.armed && k.count == 0) { k.out = false; k.armed = false; }
                break;
            case 2:
                // OUT low for the clock the count is 1; the next clock reloads it and raises OUT
                k.count = k.count - 1;
                if (k.count == 1) k.out = false;
                else if (k.count <= 0) { k.count = full; k.out = true; }
                break;
            case 3:
                // by twos; an odd count is high (N+1)/2 clocks and low (N-1)/2
                k.count -= 2;
                if (k.count <= 0) {
                    k.out = !k.out;
                    k.count = (full & 1) ? (k.out ? full + 1 : full - 1) : full;
                    if (k.count <= 0) k.count = 2;
                }
                break;
            default: break;
            }
        }
        if (!before && k.out) rose |= 1 << i;
    }
    return rose;
}

// ------------------------------------------------------------------ the DART

namespace {
int bitsPerChar(int code) { return code == 0 ? 5 : code == 1 ? 7 : code == 2 ? 6 : 8; }
int clockMultiplier(int wr4) { const int m = wr4 >> 6 & 3; return m == 0 ? 1 : m == 1 ? 16 : m == 2 ? 32 : 64; }
int stopHalves(int wr4) { const int s = wr4 >> 2 & 3; return s == 2 ? 3 : s == 3 ? 4 : 2; }   // 00 (sync) as one
}

void AmstradSerial::reset() {
    pit.reset();
    channelReset(0);
    channelReset(1);
    pollCountdown = 0;
    lastBaud = -1;
    applyLine();
}

void AmstradSerial::channelReset(int i) {
    Channel& c = ch[(size_t)i];
    const int vector = c.wr[2];
    c = Channel{};
    if (i == 1) c.wr[2] = vector;            // a channel reset leaves the vector
}

bool AmstradSerial::handlesPort(int port) const {
    if (!enabled) return false;
    const int p = port & 0xffff;
    return (p >= 0xfadc && p <= 0xfadf) || (p >= 0xfbdc && p <= 0xfbdf);
}

int AmstradSerial::frameEdges(const Channel& c, bool tx) const {
    const int bits = tx ? bitsPerChar(c.wr[5] >> 5 & 3) : bitsPerChar(c.wr[3] >> 6 & 3);
    const int halves = 2 * (1 + bits + (c.wr[4] & 1)) + stopHalves(c.wr[4]);
    return halves * clockMultiplier(c.wr[4]) / 2;
}

int AmstradSerial::baud() const {
    const int d = pit.divisor(0);
    if (!d) return 0;
    return 2000000 / d / clockMultiplier(ch[0].wr[4]);
}

void AmstradSerial::applyLine() {
    link->setRts(ch[0].wr[5] >> 1 & 1);
    link->setDtr(ch[0].wr[5] >> 7 & 1);
    const int b = baud();
    if (b != lastBaud) {
        lastBaud = b;
        link->setBaud(b, bitsPerChar(ch[0].wr[5] >> 5 & 3), stopHalves(ch[0].wr[4]), (ch[0].wr[4] & 1) ? ((ch[0].wr[4] & 2) ? 2 : 1) : 0);
    }
}

void AmstradSerial::clockTx(int i) {
    Channel& c = ch[(size_t)i];
    if (c.txEdges > 0 && --c.txEdges == 0) {
        const int mask = (1 << bitsPerChar(c.wr[5] >> 5 & 3)) - 1;
        if (i == 0) { link->write((uint8_t)(c.txShift & mask)); sentBytes++; }
    }
    if (c.txEdges == 0 && c.txFull && (c.wr[5] & 0x08)) {
        const bool cts = i == 0 && link->cts();
        if (!(c.wr[3] & 0x20) || cts) {                       // auto enables: CTS gates the transmitter
            c.txShift = c.txData;
            c.txFull = false;
            c.txEdges = frameEdges(c, true);
            if (c.wr[1] & 0x02) c.txIntPending = true;        // the buffer is empty again
        }
    }
    c.rr1 = (c.rr1 & ~0x01) | (!c.txFull && c.txEdges == 0 ? 0x01 : 0);
}

void AmstradSerial::clockRx(int i) {
    Channel& c = ch[(size_t)i];
    if (c.rxEdges > 0 && --c.rxEdges == 0) {
        // the stop bit: the character is in, if the receiver is on
        if (c.wr[3] & 0x01) {
            const int v = c.rxShift & ((1 << bitsPerChar(c.wr[3] >> 6 & 3)) - 1);
            if (c.fifoCount < 3) c.fifo[(size_t)c.fifoCount++] = v;
            else { c.fifo[2] = v; c.rr1 |= 0x20; }            // overrun: the last one is lost
            receivedBytes++;
            const int mode = c.wr[1] >> 3 & 3;
            if (mode >= 2 || (mode == 1 && c.rxIntNext)) { c.rxIntPending = true; c.rxIntNext = false; }
        }
    }
    // the next character on the line (only channel A has one): sent at this rate by the far
    // end, and lost if the receiver is off -- as it would be on the wire
    if (c.rxEdges == 0 && i == 0 && link->readable()) {
        if (!(c.wr[3] & 0x20) || link->dcd()) {               // auto enables: DCD gates the receiver
            c.rxShift = link->read();
            c.rxEdges = frameEdges(c, false);
        }
    }
}

void AmstradSerial::advanceMicrosecond() {
    if (!enabled) return;
    if (--pollCountdown <= 0) { link->poll(); pollCountdown = 500; }   // the host, every half millisecond
    for (int k = 0; k < 2; k++) {                              // 2 MHz
        const int rose = pit.clock();
        if (rose & 1) clockTx(0);
        if (rose & 2) clockRx(0);
        if (rose & 4) { clockTx(1); clockRx(1); }
    }
}

void AmstradSerial::writeControl(int i, int value) {
    Channel& c = ch[(size_t)i];
    const int reg = c.pointer;
    c.pointer = 0;
    if (reg != 0) {
        c.wr[(size_t)reg] = value;
        if (reg == 2) ch[1].wr[2] = value;                    // one vector, in channel B
        if (i == 0) applyLine();
        return;
    }
    c.wr[0] = value;
    c.pointer = value & 7;
    switch (value >> 3 & 7) {
    case 2: c.extIntPending = false; break;
    case 3: channelReset(i); if (i == 0) applyLine(); break;
    case 4: c.rxIntNext = true; break;
    case 5: c.txIntPending = false; break;
    case 6: c.rr1 &= ~0x70; break;
    case 7: break;                                            // return from interrupt: no daisy chain here
    default: break;
    }
}

int AmstradSerial::readControl(int i) {
    Channel& c = ch[(size_t)i];
    const int reg = c.pointer;
    c.pointer = 0;
    if (reg == 1) return c.rr1;
    if (reg == 2) {
        // the vector; with "status affects vector" (WR1 bit 2, channel B) bits 1-3 name the
        // highest-priority interrupt pending
        int v = ch[1].wr[2];
        if (ch[1].wr[1] & 0x04) {
            int code = 3;
            if (ch[0].rxIntPending) code = 6;
            else if (ch[0].txIntPending) code = 4;
            else if (ch[0].extIntPending) code = 5;
            else if (ch[1].rxIntPending) code = 2;
            else if (ch[1].txIntPending) code = 0;
            else if (ch[1].extIntPending) code = 1;
            v = (v & ~0x0e) | code << 1;
        }
        return v;
    }
    int rr0 = 0;
    if (c.fifoCount) rr0 |= 0x01;
    if (i == 0) {
        bool pending = false;
        for (const Channel& k : ch) pending |= k.rxIntPending || k.txIntPending || k.extIntPending;
        if (pending) rr0 |= 0x02;
    }
    if (!c.txFull) rr0 |= 0x04;
    if (i == 0) {
        if (link->dcd()) rr0 |= 0x08;
        if (link->ri()) rr0 |= 0x10;
        if (link->cts()) rr0 |= 0x20;
    }
    return rr0;
}

void AmstradSerial::writePort(int port, int value) {
    const int p = port & 0xffff;
    value &= 0xff;
    if (p >= 0xfbdc) { pit.write(p - 0xfbdc, value); applyLine(); return; }
    const int i = (p >> 1) & 1;                               // A0 control/data, A1 channel
    if (p & 1) { writeControl(i, value); return; }
    Channel& c = ch[(size_t)i];
    c.txData = value;
    c.txFull = true;
    c.txIntPending = false;
    c.rr1 &= ~0x01;
}

int AmstradSerial::readPort(int port) {
    const int p = port & 0xffff;
    if (p >= 0xfbdc) return pit.read(p - 0xfbdc);
    const int i = (p >> 1) & 1;
    if (p & 1) return readControl(i);
    Channel& c = ch[(size_t)i];
    if (c.fifoCount) {
        c.lastData = c.fifo[0];
        c.fifo[0] = c.fifo[1]; c.fifo[1] = c.fifo[2];
        c.fifoCount--;
        if (!c.fifoCount) c.rxIntPending = false;
    }
    return c.lastData;
}

} // namespace cpcse
