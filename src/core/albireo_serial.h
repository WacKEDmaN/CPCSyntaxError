// CPCSyntaxError: the Albireo 1.1's serial link and interrupt logic (its SC16C650B UART and
// the FT231X USB serial chip behind it). Albireo 2.0 has none of this (ch376.h).
//
// From the Albireo programmer's manual (pulkomandy.github.io/shinra.github.io/albireo.html):
//   * the UART at &FEB0-&FEB7 (A2-A0 = port bits 0-2): RBR/THR, IER, IIR/FCR, LCR, MCR, LSR,
//     MSR, SCR; with LCR = &BF, EFR at &FEB2. Its clock: 24 MHz (the manual's test program
//     16c650.bas takes the divisor as 1500000 / baud; DLL 13 = 115200, DLL 1 = 1.5 Mbaud).
//   * "All interrupts on the board are handled by the SC16C650B": its INT goes to the CPC's
//     INT when MCR OUT1 (bit 2) is 0, and to NMI when OUT2 (bit 3) is 0 (both 1: neither; both
//     0: both). On the chip, a 0 bit leaves its OUT pin HIGH, so the board passes INT while
//     the pin is HIGH; in loopback both pins are HIGH (sc16c650.h).
//   * the modem inputs (MSR bits 0-3): CTS = the far end's RTS (flow control); DSR = the far
//     end's DTR, through switch S4 ("Remote interrupt enable"); RI = the CRTC's CURSOR
//     ("triggered when the CRTC CURSOR signal is activated": RI from LOW to HIGH); DCD = the
//     CH376's INT#, through S1 (set "both when the CH376 triggers an interrupt, and also when
//     the interrupt is acknowledged").
//   * S3 "Remote reset enable": the far end's DTR is connected to the CPC's reset.
//   * S2 "CH376 reset enable": the CH376 is reset with the CPC (else only by its command).
//   * The far end is a PC behind the FT231X (here: HostSerial, as the RS232C card's line).
//
// The far end's flow control: a PC program set for RTS/CTS stops while the CPC's RTS is HIGH.
// No document says what the far end does, so it follows the CPC side: while the UART has
// auto-RTS on (EFR[6], as the manual recommends), the far end waits for RTS; without it, the
// far end sends at the line rate and a slow program loses bytes (LSR overrun), as on a real
// link with no flow control.
#pragma once
#include "common.h"
#include "host_serial.h"
#include "sc16c650.h"

#include <memory>

namespace cpcse {

class AlbireoSerial {
public:
    static constexpr int XTAL = 24000000;

    bool enabled = false;
    bool s1 = true;                      // CH376 interrupt enable (to DCD)
    bool s2 = true;                      // CH376 reset enable
    bool s3 = false;                     // remote reset enable
    bool s4 = false;                     // remote interrupt enable (to DSR)
    std::shared_ptr<HostSerial> link = std::make_shared<HostSerial>();
    Sc16c650 uart;
    long long sentBytes = 0, receivedBytes = 0;

    AlbireoSerial();
    void reset();                        // the CPC's reset reaches the UART's RESET too
    bool handlesPort(int port) const { return enabled && port >= 0xfeb0 && port <= 0xfeb7; }
    void writePort(int port, int value) { uart.write(port & 7, value); }
    int readPort(int port) { return uart.read(port & 7); }

    // One microsecond: the line moves, the modem inputs are taken.
    void advanceMicrosecond(bool cursor, bool ch376Interrupt);

    bool cpuInt() const { return uart.intPin() && uart.out1High(); }
    bool nmiLevel() const { return uart.intPin() && uart.out2High(); }
    bool takeNmi();                      // a rising edge of the NMI level (the Z80's /NMI is an edge)
    bool takeReset();                    // the far end's DTR became active with S3 on
    int baud() const { return uart.baud(XTAL); }

private:
    int pollCountdown = 0;
    bool lastNmi = false;
    bool lastRemoteDtr = false;
    bool remoteRts = false, remoteDtr = false;   // the far end's lines at the last poll
    bool resetPending = false;
    int lastBaud = -1, lastLcr = -1;
    void applyLine();
};

} // namespace cpcse
