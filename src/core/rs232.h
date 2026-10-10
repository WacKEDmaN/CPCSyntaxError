// CPCSyntaxError — Amstrad's RS232C serial interface (and Pace's, the same hardware): a Z80
// DART for the line and an Intel 8253 for its baud rates.
//
// The card (as MAME's cpc_rs232 wires it):
//   &FADC  DART channel A data       &FBDC  8253 counter 0 -> channel A's transmit clock
//   &FADD  DART channel A control    &FBDD  8253 counter 1 -> channel A's receive clock
//   &FADE  DART channel B data       &FBDE  8253 counter 2 -> both of channel B's clocks
//   &FADF  DART channel B control    &FBDF  8253 control word
// The 8253's counters count a 2 MHz clock; the DART divides what they give by its own clock
// mode (x1, x16, x32 or x64, WR4), so a baud rate is 2 MHz / count / mode (9600 baud: 13,
// x16). Only channel A reaches the connector -- TXD, RXD, RTS, CTS, DTR, DCD and RI, to
// whatever the settings plug into it (host_serial.h). The DART's /INT is not wired to the
// CPC (MAME's card leaves it unconnected): programs poll RR0. The card's own ROM (Amstrad's
// rs232101.rom, Pace's Comstar) is an ordinary expansion ROM in a ROM slot.
//
// The DART (Zilog's Z80 DART / Z8470 data sheet): per channel the write registers
//   WR0 the register pointer (bits 0-2) and a command (bits 3-5): 2 reset ext/status
//       interrupts, 3 channel reset, 4 enable interrupt on next Rx character, 5 reset Tx
//       interrupt pending, 6 error reset, 7 return from interrupt (channel A)
//   WR1 interrupt enables  WR2 the vector (channel B)  WR3 receiver: bit 0 enable, bit 5
//       auto enables, bits 6-7 bits per character  WR4 bit 0 parity, bit 1 even, bits 2-3
//       stop bits (1, 1.5, 2), bits 6-7 the clock mode  WR5 bit 1 RTS, bit 3 Tx enable,
//       bits 5-6 bits per character, bit 7 DTR
// and the read registers RR0 (bit 0 Rx character available, 1 interrupt pending (A), 2 Tx
// buffer empty, 3 DCD, 4 RI, 5 CTS, 7 break), RR1 (bit 0 all sent, 4 parity error, 5 Rx
// overrun, 6 framing error) and RR2 (channel B: the vector). A three-character receive FIFO;
// a fourth character arriving overwrites the last and sets the overrun bit.
#pragma once
#include "common.h"
#include "host_serial.h"

#include <array>
#include <memory>

namespace cpcse {

class Intel8253 {
public:
    struct Counter {
        int mode = 0, access = 3;        // access: 1 LSB, 2 MSB, 3 LSB then MSB
        bool bcd = false;
        int count = 0;                   // the counting element
        int reload = 0;                  // the count register (0 = 65536 / 10000)
        bool loaded = false;             // a count has been written since the control word
        bool armed = false;              // mode 0/4: the terminal count is still to come
        bool writeHigh = false, readHigh = false;   // which byte the next access is
        int writeLow = 0;
        bool latched = false;
        int latch = 0;
        bool out = true;
        int pendingLoad = -1;            // a count written, taken on the next clock
    };
    std::array<Counter, 3> c;
    void reset();
    void write(int reg, int value);
    int read(int reg);
    // One input clock to every counter; returns a bit per counter whose OUT rose.
    int clock();
    int divisor(int i) const;            // the count a rate-generating counter divides by

private:
    void load(Counter& k, int value);
    int decrement(const Counter& k, int by) const;
};

class AmstradSerial {
public:
    bool enabled = false;
    // the far end; shared, so a reboot (a new machine) keeps the connection
    std::shared_ptr<HostSerial> link = std::make_shared<HostSerial>();

    void reset();
    bool handlesPort(int port) const;
    void writePort(int port, int value);
    int readPort(int port);
    void advanceMicrosecond();           // two 8253 clocks, and the DART's clocks from them

    int baud() const;                    // channel A's transmit rate, 0 if not clocked
    long long sentBytes = 0, receivedBytes = 0;

    struct Channel {
        std::array<int, 8> wr{};
        int pointer = 0;
        std::array<int, 3> fifo{};
        int fifoCount = 0;
        int lastData = 0xff;
        int rr1 = 0x01;                  // all sent
        // transmitter: the buffer, and the shift register's remaining clock edges
        bool txFull = false;
        int txData = 0;
        int txEdges = 0;                 // > 0: shifting txShift out
        int txShift = 0;
        // receiver: the character on the line and its remaining edges
        int rxEdges = 0;
        int rxShift = 0;
        bool rxIntNext = false;          // WR0 command 4
        bool txIntPending = false, rxIntPending = false, extIntPending = false;
    };
    std::array<Channel, 2> ch;
    Intel8253 pit;

private:
    int frameEdges(const Channel& c, bool tx) const;
    void clockTx(int i);
    void clockRx(int i);
    void channelReset(int i);
    void writeControl(int i, int value);
    int readControl(int i);
    void applyLine();
    int pollCountdown = 0;
    int lastBaud = -1;
};

} // namespace cpcse
