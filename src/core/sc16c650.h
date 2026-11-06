// CPCSyntaxError: the NXP SC16C650B UART (the Albireo 1.1's serial link; albireo_serial.h).
//
// From the NXP data sheet SC16C650B Rev. 04, 14 September 2009 (docs/SC16C650B.pdf):
//   registers (Table 3, Table 8): A2-A0 with LCR[7] = 0: RHR/THR, IER, ISR/FCR, LCR, MCR, LSR,
//     MSR, SPR; with LCR[7] = 1: DLL, DLM at 0 and 1; with LCR = BFh: EFR at 2, Xon1, Xon2,
//     Xoff1, Xoff2 at 4 to 7. LCR is at 3 in every set.
//   FIFOs: 32 bytes each way (§6.2). RX trigger levels 8, 16, 24, 28 (Table 11); TX trigger
//     levels 16, 8, 24, 30 (Table 12), active only while EFR[4] is set (§6.2). FCR's other
//     bits are programmed only when FCR[0] is written as 1 (Table 10).
//   interrupts (Table 13), highest first: line status 06h, RX data 04h and RX time-out 0Ch,
//     THR empty 02h, modem status 00h, Xoff or special character 10h, CTS/RTS 20h; ISR[0] = 1
//     when none; ISR[7:6] = 11 with the FIFOs on. The time-out is 4 character times (§6.6).
//   enhanced bits (Table 8 [3], EFR[4]): IER[7:4], ISR[5:4], FCR[5:4], MCR[7:5] change only
//     while EFR[4] is set.
//   hardware flow control (§6.3, EFR[7:6]): auto-CTS stops the transmitter while CTS is HIGH;
//     auto-RTS drives RTS from the RX FIFO. Table 4 gives the levels: RTS off (or Xoff sent)
//     at the trigger level, on again (or Xon sent) at 0, 7, 15, 23. (§6.3's prose says "the
//     next trigger level"; Table 4's numbers are used.)
//   software flow control (§6.4, Table 23): EFR[3:0]; Xon/Xoff characters are not stored.
//     Special character (§6.5, EFR[5]): a match with Xoff2 is stored and sets ISR[4].
//   baud (§6.7): XTAL / 16 / divisor, and / 4 more with MCR[7]. Divisor 0 has no rate in the
//     data sheet: the line stops.
//   loopback (§6.10, MCR[4]): TX goes to RX inside; MSR[4..7] are MCR's RTS, DTR, OUT1, OUT2;
//     the RTS, DTR, OUT1 and OUT2 pins go HIGH (Table 2); the TX pin stays marking.
//   MSR (Table 21): bits 4-7 are the complements of CTS, DSR, RI, DCD; bits 0, 1, 3 are set
//     by any change, bit 2 when RI goes from LOW to HIGH. Reading MSR clears 0-3.
//   reset (Tables 24, 25): IER, LCR, MCR, FCR, EFR 0; LSR 60h; ISR 01h; TX, RTS, DTR HIGH.
//
// Not in this model: DMA (RXRDY, TXRDY), Sleep mode (IER[4] is kept but changes nothing),
// IrDA (MCR[6] is kept), break (LCR[6] is kept; nothing receives a break from the far end),
// parity and framing errors (the far end gives bytes, never a bad frame). The data sheet
// does not say what changing FCR[0] does to the FIFOs; here both are cleared, as the 16C550
// family does.
#pragma once
#include "common.h"

#include <deque>
#include <functional>

namespace cpcse {

class Sc16c650 {
public:
    static constexpr int FIFO = 32;

    void reset();                        // the RESET pin (Tables 24 and 25)
    void write(int reg, int value);      // A2-A0
    int read(int reg);

    // The modem inputs as the pins carry them (true = HIGH). The MSR's change bits follow.
    void setInputs(bool ctsHigh, bool dsrHigh, bool riHigh, bool dcdHigh);
    // XTAL1 clocks (the board gives 24 per microsecond): the transmitter and receiver move.
    void advance(int clocks);

    // The serial line. transmit: a character leaves the TX pin. receive: the far end's next
    // character (-1: none), asked only when the receiver is free.
    std::function<void(uint8_t)> transmit;
    std::function<int()> receive;

    // Output pins (true = HIGH).
    bool intPin() const { return (isr() & 1) == 0; }   // INT is active HIGH (Table 2)
    bool rtsHigh() const;
    bool dtrHigh() const { return loopback() || !(mcr & 0x01); }
    bool out1High() const { return loopback() || !(mcr & 0x04); }
    bool out2High() const { return loopback() || !(mcr & 0x08); }

    int baud(int xtalHz) const;          // 0: stopped
    int wordLength() const { return 5 + (lcr & 3); }
    int stopHalves() const { return !(lcr & 4) ? 2 : (lcr & 3) == 0 ? 3 : 4; }
    int parity() const { return !(lcr & 0x08) ? 0 : (lcr & 0x20) ? ((lcr & 0x10) ? 4 : 3) : ((lcr & 0x10) ? 2 : 1); }   // 0 none, 1 odd, 2 even, 3 mark, 4 space
    bool autoRts() const { return (efr & 0x40) != 0; }
    bool loopback() const { return (mcr & 0x10) != 0; }
    bool fifoOn() const { return (fcr & 1) != 0; }
    int rxCount() const { return (int)rxFifo.size(); }
    int txCount() const { return (int)txFifo.size(); }

    // Register contents (for the debugger and checks).
    uint8_t ier = 0, fcr = 0, lcr = 0, mcr = 0, spr = 0xff, efr = 0;
    uint8_t dll = 0, dlm = 0, xon1 = 0, xon2 = 0, xoff1 = 0, xoff2 = 0;
    int isr() const;
    int lsr() const;
    int msr() const { return msrState | msrDelta; }

private:
    std::deque<uint8_t> rxFifo, txFifo, txControl;   // txControl: Xon/Xoff characters, sent first
    bool overrun = false;
    bool tsrBusy = false;
    uint8_t tsrData = 0;
    long long tsrClocks = 0;             // clocks left of the character being sent
    int rxByte = -1;                     // the character arriving
    long long rxClocks = 0;
    long long idleClocks = 0;            // since the last character in or RHR read (time-out)
    bool timeoutPending = false;
    bool thrIntPending = false;
    bool xoffIntPending = false;         // ISR[4]: an Xoff or the special character
    bool ctsRtsPending = false;          // ISR[5]
    bool txHalted = false;               // an Xoff received
    bool xoffSent = false;
    bool rtsAutoOff = false;             // auto-RTS has RTS HIGH
    int pairFirst = -1;                  // the first of two Xon/Xoff characters, held
    uint8_t lastRxRead = 0;
    uint8_t msrState = 0, msrDelta = 0;  // MSR[7:4], MSR[3:0]
    bool pinCts = true, pinDsr = true, pinRi = true, pinDcd = true;
    bool lastRtsHigh = true;

    bool enhanced() const { return lcr == 0xbf; }
    bool efr4() const { return (efr & 0x10) != 0; }
    long long charClocks() const;
    int rxTrigger() const;
    int txTrigger() const;
    int rtsOnLevel() const;
    bool txCondition() const;
    void takeRx(uint8_t byte);
    void storeRx(uint8_t byte);
    void flowAfterRx();
    void updateMsr();
    void updateRtsPin();
    void startTx();
    void writeThr(int value);
    void setFcr(int value);
};

} // namespace cpcse
