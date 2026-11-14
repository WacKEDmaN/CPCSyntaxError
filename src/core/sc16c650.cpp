// CPCSyntaxError: the NXP SC16C650B UART. See sc16c650.h for the data sheet's rules.
#include "sc16c650.h"

namespace cpcse {

void Sc16c650::reset() {
    // Table 24: IER, LCR, MCR, FCR, EFR 0; LSR 60h; ISR 01h; MSR[3:0] 0. Table 8: SPR FFh,
    // Xon/Xoff 00h (§6.4). DLL/DLM are "XX": 0 here, so the line has no rate until set.
    ier = fcr = lcr = mcr = efr = 0;
    spr = 0xff;
    dll = dlm = 0;
    xon1 = xon2 = xoff1 = xoff2 = 0;
    rxFifo.clear(); txFifo.clear(); txControl.clear();
    overrun = false;
    tsrBusy = false; tsrClocks = 0;
    rxByte = -1; rxClocks = 0;
    idleClocks = 0;
    timeoutPending = thrIntPending = xoffIntPending = ctsRtsPending = false;
    txHalted = xoffSent = rtsAutoOff = false;
    pairFirst = -1;
    msrDelta = 0;
    updateMsr();
    msrDelta = 0;                        // the inputs as they are now, with no change
    lastRtsHigh = rtsHigh();
}

// ---------------------------------------------------------------- the rate

long long Sc16c650::charClocks() const {
    const int divisor = dll | dlm << 8;
    if (divisor == 0) return 0;          // no rate in the data sheet: the line stops
    // start bit, data bits, parity, stop bits (in halves for 1.5), each 16 clocks of the
    // divided clock (§6.7)
    const int halves = 2 * (1 + wordLength() + (parity() ? 1 : 0)) + stopHalves();
    const long long prescale = (mcr & 0x80) ? 4 : 1;
    return (long long)halves * 8 * divisor * prescale;
}

int Sc16c650::baud(int xtalHz) const {
    const int divisor = dll | dlm << 8;
    if (divisor == 0) return 0;
    return xtalHz / (16 * divisor * ((mcr & 0x80) ? 4 : 1));
}

int Sc16c650::rxTrigger() const {
    static const int levels[4] = { 8, 16, 24, 28 };      // Table 11
    return levels[(fcr >> 6) & 3];
}

int Sc16c650::txTrigger() const {
    static const int levels[4] = { 16, 8, 24, 30 };      // Table 12
    return levels[(fcr >> 4) & 3];
}

int Sc16c650::rtsOnLevel() const {
    static const int levels[4] = { 0, 7, 15, 23 };       // Table 4, "Assert RTS or send Xon"
    return levels[(fcr >> 6) & 3];
}

// The transmitter's interrupt condition: the holding register (or FIFO) empty; with EFR[4]
// and the FIFOs, fewer characters than the TX trigger level (§6.2, Table 10 FCR[5:4]).
bool Sc16c650::txCondition() const {
    if (fifoOn() && efr4()) return (int)txFifo.size() < txTrigger();
    return txFifo.empty();
}

// ---------------------------------------------------------------- registers

int Sc16c650::isr() const {
    int v = 1;
    const bool lineStatus = overrun;     // OE (PE, FE, BI are never made here)
    const bool rxData = fifoOn() ? (int)rxFifo.size() >= rxTrigger() : !rxFifo.empty();
    if ((ier & 0x04) && lineStatus) v = 0x06;
    else if ((ier & 0x01) && rxData) v = 0x04;
    else if ((ier & 0x01) && timeoutPending) v = 0x0c;
    else if ((ier & 0x02) && thrIntPending) v = 0x02;
    else if ((ier & 0x08) && msrDelta) v = 0x00;
    else if (efr4() && (ier & 0x20) && xoffIntPending) v = 0x10;
    else if (efr4() && (ier & 0xc0) && ctsRtsPending) v = 0x20;
    if (fifoOn()) v |= 0xc0;
    return v;
}

int Sc16c650::lsr() const {
    int v = 0;
    if (!rxFifo.empty()) v |= 0x01;
    if (overrun) v |= 0x02;
    if (txFifo.empty()) v |= 0x20;                       // THR (or the TX FIFO) empty
    if (txFifo.empty() && !tsrBusy && txControl.empty()) v |= 0x40;   // and the shift register
    return v;
}

void Sc16c650::write(int reg, int value) {
    reg &= 7;
    value &= 0xff;
    const bool dlab = (lcr & 0x80) != 0;
    switch (reg) {
    case 0:
        if (dlab) dll = (uint8_t)value;
        else writeThr(value);
        break;
    case 1:
        if (dlab) { dlm = (uint8_t)value; break; }
        {
            const bool thrWasOff = !(ier & 0x02);
            const uint8_t mask = efr4() ? 0xff : 0x0f;   // IER[7:4] only with EFR[4]
            ier = (uint8_t)((ier & ~mask) | (value & mask));
            // a THR-empty interrupt for an empty holding register as soon as it is enabled
            if (thrWasOff && (ier & 0x02) && txCondition()) thrIntPending = true;
        }
        break;
    case 2:
        if (enhanced()) efr = (uint8_t)value;
        else setFcr(value);
        break;
    case 3:
        lcr = (uint8_t)value;
        break;
    case 4:
        if (enhanced()) { xon1 = (uint8_t)value; break; }
        {
            const uint8_t mask = efr4() ? 0xff : 0x1f;   // MCR[7:5] only with EFR[4]
            mcr = (uint8_t)((mcr & ~mask) | (value & mask));
            updateMsr();
            updateRtsPin();
        }
        break;
    case 5: if (enhanced()) xon2 = (uint8_t)value; break;   // LSR: read only
    case 6: if (enhanced()) xoff1 = (uint8_t)value; break;  // MSR: read only
    case 7:
        if (enhanced()) xoff2 = (uint8_t)value;
        else spr = (uint8_t)value;
        break;
    }
}

int Sc16c650::read(int reg) {
    reg &= 7;
    const bool dlab = (lcr & 0x80) != 0;
    switch (reg) {
    case 0:
        if (dlab) return dll;
        if (!rxFifo.empty()) { lastRxRead = rxFifo.front(); rxFifo.pop_front(); }
        idleClocks = 0;                  // the time-out counter restarts on an RHR read (§6.6)
        timeoutPending = false;
        flowAfterRx();
        return lastRxRead;
    case 1: return dlab ? dlm : ier;
    case 2: {
        if (enhanced()) return efr;
        const int v = isr();
        // reading the ISR clears the THR-empty source when it is the one shown (Albireo
        // manual: "cleared by reading the IIR or writing more data bytes"), and the Xoff /
        // special character and CTS/RTS sources the same way
        if ((v & 0x3f) == 0x02) thrIntPending = false;
        else if ((v & 0x3f) == 0x10) xoffIntPending = false;
        else if ((v & 0x3f) == 0x20) ctsRtsPending = false;
        return v;
    }
    case 3: return lcr;
    case 4: return enhanced() ? xon1 : mcr;
    case 5: {
        if (enhanced()) return xon2;
        const int v = lsr();
        overrun = false;                 // OE and LSR[7] clear on a read (Table 20)
        return v;
    }
    case 6: {
        if (enhanced()) return xoff1;
        const int v = msr();
        msrDelta = 0;                    // Table 21: MSR[3:0] clear when read
        return v;
    }
    default: return enhanced() ? xoff2 : spr;
    }
}

void Sc16c650::setFcr(int value) {
    // Table 10: FCR[0] must be 1 for the other bits to be programmed
    if (!(value & 1)) {
        if (fifoOn()) { rxFifo.clear(); txFifo.clear(); }
        fcr &= (uint8_t)~1;
        return;
    }
    if (!fifoOn()) { rxFifo.clear(); txFifo.clear(); }
    if (value & 0x02) { rxFifo.clear(); timeoutPending = false; }   // RX FIFO reset (the shift register is not touched)
    if (value & 0x04) txFifo.clear();                               // TX FIFO reset
    const uint8_t keep = efr4() ? 0x00 : 0x30;                      // FCR[5:4] only with EFR[4]
    fcr = (uint8_t)((fcr & keep) | (value & ~keep & 0xf9));         // bits 1 and 2 return to 0
    if (txCondition() && (ier & 0x02)) thrIntPending = true;
    flowAfterRx();
}

void Sc16c650::writeThr(int value) {
    if (fifoOn()) {
        if ((int)txFifo.size() < FIFO) txFifo.push_back((uint8_t)value);   // a full FIFO loses it
    } else {
        if (txFifo.empty()) txFifo.push_back((uint8_t)value);
        else txFifo.back() = (uint8_t)value;                              // the holding register is overwritten
    }
    // writing clears the THR-empty interrupt; with a TX trigger level it stays while the
    // FIFO is still below the level
    thrIntPending = (fifoOn() && efr4()) ? ((ier & 0x02) && txCondition()) : false;
    startTx();
}

// ---------------------------------------------------------------- modem lines

void Sc16c650::setInputs(bool ctsHigh, bool dsrHigh, bool riHigh, bool dcdHigh) {
    pinCts = ctsHigh; pinDsr = dsrHigh; pinRi = riHigh; pinDcd = dcdHigh;
    updateMsr();
}

void Sc16c650::updateMsr() {
    uint8_t now;
    if (loopback()) {
        // §6.10: CTS = RTS, DSR = DTR, RI = OUT1, DCD = OUT2, as MCR's bits. RTS is the
        // chip's own RTS: with auto-RTS the FIFO drives it (EFR[6]), not MCR[1]; the manual's
        // 16c650.bas runs loopback with EFR C0H and MCR 10H and gets its echo, which a CTS
        // from MCR[1] (0) would stop.
        const bool rtsActive = autoRts() ? !rtsAutoOff : (mcr & 0x02) != 0;
        now = (uint8_t)((rtsActive ? 0x10 : 0) | ((mcr & 0x01) ? 0x20 : 0) |
                        ((mcr & 0x04) ? 0x40 : 0) | ((mcr & 0x08) ? 0x80 : 0));
    } else {
        // Table 21: the complements of the pins
        now = (uint8_t)((pinCts ? 0 : 0x10) | (pinDsr ? 0 : 0x20) | (pinRi ? 0 : 0x40) | (pinDcd ? 0 : 0x80));
    }
    const uint8_t changed = (uint8_t)(now ^ msrState);
    if (changed & 0x10) msrDelta |= 0x01;
    if (changed & 0x20) msrDelta |= 0x02;
    if ((msrState & 0x40) && !(now & 0x40)) msrDelta |= 0x04;   // RI from LOW to HIGH at the pin
    if (changed & 0x80) msrDelta |= 0x08;
    // IER[7]: CTS from logic 0 to logic 1 at the pin (MSR[4] from 1 to 0)
    if ((msrState & 0x10) && !(now & 0x10) && efr4() && (ier & 0x80)) ctsRtsPending = true;
    msrState = now;
    startTx();                           // auto-CTS may let the transmitter go again
}

bool Sc16c650::rtsHigh() const {
    if (loopback()) return true;
    if (autoRts()) return rtsAutoOff;
    return !(mcr & 0x02);
}

void Sc16c650::updateRtsPin() {
    const bool high = rtsHigh();
    // IER[6]: RTS from logic 0 to logic 1
    if (high && !lastRtsHigh && efr4() && (ier & 0x40)) ctsRtsPending = true;
    lastRtsHigh = high;
}

// ---------------------------------------------------------------- receiving

// Auto-RTS and the Xoff/Xon this chip sends, from the RX FIFO's level (Table 4).
void Sc16c650::flowAfterRx() {
    const int n = (int)rxFifo.size();
    if (autoRts()) {
        const bool was = rtsAutoOff;
        if (!rtsAutoOff && n >= rxTrigger()) rtsAutoOff = true;
        else if (rtsAutoOff && n <= rtsOnLevel()) rtsAutoOff = false;
        if (was != rtsAutoOff && loopback()) updateMsr();   // loopback CTS is this RTS
    }
    updateRtsPin();
    const int tx = (efr >> 2) & 3;       // Table 23: Cont-3, Cont-2
    if (tx && txControl.size() < 64) {   // (a queue the line never empties stays small)
        if (!xoffSent && n >= rxTrigger()) {
            if (tx & 2) txControl.push_back(xoff1);
            if (tx & 1) txControl.push_back(xoff2);
            xoffSent = true;
            startTx();
        } else if (xoffSent && n <= rtsOnLevel()) {
            if (tx & 2) txControl.push_back(xon1);
            if (tx & 1) txControl.push_back(xon2);
            xoffSent = false;
            startTx();
        }
    }
}

void Sc16c650::storeRx(uint8_t byte) {
    if (fifoOn()) {
        if ((int)rxFifo.size() < FIFO) rxFifo.push_back(byte);
        else overrun = true;             // Table 20: the new character is not stored
    } else {
        if (rxFifo.empty()) rxFifo.push_back(byte);
        else { rxFifo.back() = byte; overrun = true; }   // the holding register is overwritten
    }
}

// A character fully received: software flow control, the special character, the FIFO.
void Sc16c650::takeRx(uint8_t byte) {
    idleClocks = 0;
    timeoutPending = false;
    const uint8_t mask = (uint8_t)((1 << wordLength()) - 1);
    const uint8_t b = (uint8_t)(byte & mask);
    const int rxMode = efr & 3;          // Table 23: Cont-1, Cont-0
    if (rxMode == 1 || rxMode == 2) {
        const uint8_t xon = rxMode == 2 ? xon1 : xon2, xoff = rxMode == 2 ? xoff1 : xoff2;
        if (b == (xoff & mask)) { txHalted = true; if (efr4() && (ier & 0x20)) xoffIntPending = true; return; }
        if (b == (xon & mask)) { txHalted = false; xoffIntPending = false; startTx(); return; }
    } else if (rxMode == 3) {
        // two characters in sequence: Xon1 Xon2, Xoff1 Xoff2
        if (pairFirst < 0) {
            if (b == (xon1 & mask) || b == (xoff1 & mask)) { pairFirst = b; return; }
        } else {
            const uint8_t first = (uint8_t)pairFirst;
            pairFirst = -1;
            if (first == (xoff1 & mask) && b == (xoff2 & mask)) { txHalted = true; if (efr4() && (ier & 0x20)) xoffIntPending = true; return; }
            if (first == (xon1 & mask) && b == (xon2 & mask)) { txHalted = false; xoffIntPending = false; startTx(); return; }
            storeRx(first);              // not a pair: the first one is data after all
            takeRx(byte);
            return;
        }
    }
    if ((efr & 0x20) && b == (xoff2 & mask) && efr4() && (ier & 0x20)) xoffIntPending = true;   // §6.5
    storeRx(byte);
    flowAfterRx();
}

// ---------------------------------------------------------------- time

void Sc16c650::startTx() {
    if (tsrBusy) return;
    const long long len = charClocks();
    if (len == 0) return;
    if (!txControl.empty()) {            // Xon/Xoff go out before data, even while halted
        tsrData = txControl.front(); txControl.pop_front();
    } else {
        if (txFifo.empty() || txHalted) return;
        if ((efr & 0x80) && !(msrState & 0x10)) return;   // auto-CTS: CTS HIGH stops the transmitter
        const bool before = txCondition();
        tsrData = txFifo.front(); txFifo.pop_front();
        if (!before && txCondition() && (ier & 0x02)) thrIntPending = true;
    }
    tsrBusy = true;
    tsrClocks = len;
}

void Sc16c650::advance(int clocks) {
    if (clocks <= 0) return;
    // the transmitter
    long long left = clocks;
    while (left > 0) {
        if (!tsrBusy) { startTx(); if (!tsrBusy) break; }
        const long long step = left < tsrClocks ? left : tsrClocks;
        tsrClocks -= step;
        left -= step;
        if (tsrClocks == 0) {
            tsrBusy = false;
            const uint8_t sent = tsrData;
            if (loopback()) takeRx(sent);            // §6.10: TX to RX inside the chip
            else if (transmit) transmit(sent);
        }
    }
    // the receiver: the far end sends a character over one character time (never in
    // loopback, where RX is cut from its pin)
    const long long len = charClocks();
    if (!loopback() && len > 0) {
        long long rleft = clocks;
        while (rleft > 0) {
            if (rxByte < 0) {
                const int b = receive ? receive() : -1;
                if (b < 0) break;
                rxByte = b & 0xff;
                rxClocks = len;
            }
            const long long step = rleft < rxClocks ? rleft : rxClocks;
            rxClocks -= step;
            rleft -= step;
            if (rxClocks == 0) { const int b = rxByte; rxByte = -1; takeRx((uint8_t)b); }
        }
    }
    // the receive time-out: 4 character times with data in the FIFO and nothing in or out
    if (fifoOn() && !rxFifo.empty() && len > 0) {
        idleClocks += clocks;
        if (idleClocks >= 4 * len) timeoutPending = true;
    } else {
        idleClocks = 0;
    }
}

} // namespace cpcse
