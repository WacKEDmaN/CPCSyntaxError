// CPCSyntaxError: the Albireo 1.1's serial link and interrupt logic. See albireo_serial.h.
#include "albireo_serial.h"

namespace cpcse {

AlbireoSerial::AlbireoSerial() {
    uart.transmit = [this](uint8_t b) { link->write(b); sentBytes++; };
    uart.receive = [this]() -> int {
        // the far end waits while the CPC's RTS is HIGH, when the CPC uses auto-RTS (header)
        if (uart.autoRts() && uart.rtsHigh()) return -1;
        if (!link->readable()) return -1;
        const int b = link->read();
        if (b >= 0) receivedBytes++;
        return b;
    };
    uart.reset();
}

void AlbireoSerial::reset() {
    uart.reset();
    lastNmi = false;
    resetPending = false;
    lastBaud = -1; lastLcr = -1;
    pollCountdown = 0;
    applyLine();
}

// The CPC's outputs to the far end (the FT231X's CTS and DSR take RTS and DTR), and the
// line's rate for a host COM port.
void AlbireoSerial::applyLine() {
    link->setRts(!uart.rtsHigh());       // the pins are active LOW
    link->setDtr(!uart.dtrHigh());
    const int b = uart.baud(XTAL);
    if (b != lastBaud || uart.lcr != lastLcr) {
        lastBaud = b; lastLcr = uart.lcr;
        if (b > 0) link->setBaud(b, uart.wordLength(), uart.stopHalves(), uart.parity());
    }
}

void AlbireoSerial::advanceMicrosecond(bool cursor, bool ch376Interrupt) {
    if (--pollCountdown <= 0) {
        // the host's lines are read with the bytes (a COM port's status is a system call)
        link->poll();
        remoteRts = link->cts();
        remoteDtr = link->dsr();
        pollCountdown = 250;
    }
    // CTS: the far end's RTS (active LOW at the pin). DSR: its DTR through S4, else pulled
    // HIGH. RI: CURSOR as it is (HIGH while active). DCD: the CH376's INT# through S1.
    uart.setInputs(!remoteRts, !(s4 && remoteDtr), cursor, !(s1 && ch376Interrupt));
    if (s3 && remoteDtr && !lastRemoteDtr) resetPending = true;
    lastRemoteDtr = remoteDtr;
    uart.advance(XTAL / 1000000);
    applyLine();
}

bool AlbireoSerial::takeNmi() {
    const bool now = nmiLevel();
    const bool edge = now && !lastNmi;
    lastNmi = now;
    return edge;
}

bool AlbireoSerial::takeReset() {
    const bool r = resetPending;
    resetPending = false;
    return r;
}

} // namespace cpcse
