// CPCSyntaxError — PlayCity (TotO / Richard Gatineau, 2013): two Yamaha YMZ294 sound
// chips and a Z80 CTC on their own ports, not on the PPI.
//
// Ports (cpcwiki "PlayCity"):
//   &F880-&F883  CTC channels 0-3
//   &F984 / &F884  right YMZ294: register select / data
//   &F988 / &F888  left YMZ294: register select / data
//   &F8FF         the expansion bus's peripheral reset
// CTC wiring: channel 0 counts the CPC's 4 MHz and its ZC/TO clocks both YMZ294s;
// channel 1 counts the CRTC's CURSOR pin and its ZC/TO drives the Z80's NMI (raster
// interrupts); channels 2 and 3 are a timer pair (2's ZC/TO clocks 3) whose interrupts
// are IM2-vectored -- "only one vector need be specified; the CTC internally generates an
// unique vector for each channel".
//
// The YMZ294 is the AY's register set without the I/O ports (R14/R15). Unprogrammed,
// "the YMZs are clocked at 4 MHz but run internally at 2 MHz" -- an Atari ST's pitch;
// programming channel 0 with time constant n gives (2 - 1/n) MHz, so n=1 is the CPC's
// 1 MHz and n=4 (1.75 MHz) a Spectrum's. That rule is CPCEC's, the only implementation
// to compare against; the cpcwiki example's "$7F,$01 = CPC AY" agrees with it.
#pragma once
#include "ay.h"
#include <array>

namespace cpcse {

class PlayCity {
public:
    bool enabled = false;
    AY38912 right{ nullptr, 4000000, 16 };
    AY38912 left{ nullptr, 4000000, 16 };

    PlayCity() { reset(); }
    void reset();
    bool handlesPort(int port) const;
    void writePort(int port, int value);
    int readPort(int port);
    // One CPC microsecond (4 T-states): the YMZs, and the CTC's timers.
    void advanceMicrosecond();
    // The CRTC's CURSOR pin, sampled once a character: channel 1 counts its rising edges.
    void cursorPin(bool level);
    void setOutputSampleRate(double rate) { right.setOutputSampleRate(rate); left.setOutputSampleRate(rate); }

    // What the CTC asks of the Z80. nmiPending is taken (and cleared) by the host;
    // an interrupt is a vector, -1 when none is waiting.
    bool takeNmi() { bool n = nmiPending; nmiPending = false; return n; }
    int takeInterruptVector();

private:
    struct Channel {
        int control = 0x03;         // after reset: "software reset" and control word
        int timeConstant = 256;
        int counter = 256;
        int prescale = 0;           // system clocks into the 16/256 prescaler
        bool expectConstant = false;
        bool running = false;
        bool interruptPending = false;
    };
    std::array<Channel, 4> ch{};
    int vectorBase = 0;
    bool nmiPending = false;
    bool cursorLast = false;
    void writeChannel(int n, int value);
    void zeroCount(int n);          // channel n reached zero: ZC/TO and the interrupt
    void countEdge(int n);          // one CLK/TRG edge into a channel in counter mode
    void setYmzClock(int timeConstant);
};

} // namespace cpcse
