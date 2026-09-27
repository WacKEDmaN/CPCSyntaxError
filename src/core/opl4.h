// CPCSyntaxError — an OPL4 sound card (Yamaha YMF278B) on the AMSDAP.
//
// The AMSDAP (Prodatron & TMTLOGIC) puts MSX I/O cards on the CPC at &FFxx, so a
// MoonSound-style OPL4 card answers where it does on the MSX:
//   &FFC4 / &FFC5  FM bank 0 address / data     (read &FFC4: status)
//   &FFC6 / &FFC7  FM bank 1 address / data
//   &FF7E / &FF7F  wavetable address / data     (read &FF7F: wavetable data)
// clocked at 33.8688 MHz: 18 FM channels (the OPL3) and 24 wavetable channels, out at
// clock/768 = 44.1 kHz. Wave memory is 22 bits: the YRW801 sample ROM at 0..&1FFFFF,
// sample RAM from &200000 (MoonSound: 128K, expandable; 2 MB on later cards).
//
// The chip itself is ymfm's YMF278B (third_party/ymfm, Aaron Giles, BSD-3). This file is
// the card around it: ports, memory, timers, IRQ, and time.
#pragma once
#include "common.h"

namespace cpcse {

class Opl4Chip;   // ymfm's, kept out of this header

class Opl4Card {
public:
    Opl4Card();
    ~Opl4Card();
    static constexpr double CLOCK_HZ = 33868800.0;
    static constexpr int SAMPLE_RATE = 44100;           // CLOCK_HZ / 768

    bool enabled = false;
    Bytes rom;                  // YRW801 (2 MB), empty = none fitted
    Bytes ram;                  // sample RAM
    void setEnabled(bool on);
    void setRamKiB(int kib);
    void reset();

    bool handlesPort(int port) const;
    int readPort(int port, long long cpcCycles);
    void writePort(int port, int value, long long cpcCycles);
    // Brings the chip up to this moment of CPC time: samples made, timers run.
    void advanceTo(long long cpcCycles);
    // Cheap enough for every instruction: only does work when a timer is due.
    void tick(long long cpcCycles) { if (enabled && cpcCycles >= nextEventCycles) advanceTo(cpcCycles); }
    bool intAsserted() const { return enabled && irq; }

    // The card's output (the chip's DO2: FM and wavetable mixed), stereo, 44.1 kHz, as
    // time made it; the host takes and clears it.
    std::vector<int16_t> samples;   // L, R, L, R ...

    // --- called by ymfm (through Opl4Chip)
    uint8_t memoryRead(uint32_t address) const;
    void memoryWrite(uint32_t address, uint8_t value);
    void setTimer(uint32_t tnum, int32_t durationClocks);
    void setBusyEnd(uint32_t clocks) { busyUntil = nowClocks + clocks; }
    bool isBusy() const { return nowClocks < busyUntil; }
    void setIrq(bool asserted) { irq = asserted; }

private:
    std::unique_ptr<Opl4Chip> chip;
    double nowClocks = 0;           // the chip's clock, in its own cycles
    long long lastCycles = 0;       // CPC time it was brought to
    double samplePhase = 0;         // chip clocks towards the next sample
    double busyUntil = 0;
    double timerEnd[2] = { -1, -1 };
    long long nextEventCycles = 0;
    bool irq = false;
    bool resync = true;             // the next advance only sets the clock
    void schedule();
};

} // namespace cpcse
