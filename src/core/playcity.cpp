// CPCSyntaxError — PlayCity: two YMZ294 and a Z80 CTC. See the header.
#include "playcity.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cpcse {

namespace {
// Z80 CTC control word bits (Zilog Z80 CTC technical manual).
constexpr int CTC_INT_ENABLE = 0x80;
constexpr int CTC_COUNTER_MODE = 0x40;    // else timer mode
constexpr int CTC_PRESCALE_256 = 0x20;    // timer mode: 256, else 16
constexpr int CTC_TIME_CONSTANT = 0x04;   // a time constant byte follows
constexpr int CTC_SOFT_RESET = 0x02;
constexpr int CTC_CONTROL = 0x01;         // a control word, else (channel 0) the vector
}

void PlayCity::reset() {
    right.reset(); left.reset();
    for (Channel& c : ch) c = Channel{};
    vectorBase = 0; nmiPending = false; cursorLast = false;
    setYmzClock(0);
}

bool PlayCity::handlesPort(int port) const {
    if (!enabled) return false;
    port &= 0xffff;
    return (port >= 0xf880 && port <= 0xf883) || port == 0xf884 || port == 0xf888
        || port == 0xf984 || port == 0xf988;
}

// The YMZ294s' clock from CTC channel 0's time constant: 0 = never programmed (2 MHz),
// else (2 - 1/n) MHz. Our AY ticks once per `tStatesPerTick` T-states of a 4 MHz CPC at
// 1 MHz / 8, so the period is 32 / MHz.
void PlayCity::setYmzClock(int timeConstant) {
    const double mhz = timeConstant <= 0 ? 2.0 : 2.0 - 1.0 / timeConstant;
    const int period = std::max(1, (int)std::lround(32.0 / mhz));
    right.tStatesPerTick = period;
    left.tStatesPerTick = period;
}

void PlayCity::writeChannel(int n, int value) {
    Channel& c = ch[n];
    if (c.expectConstant) {
        c.expectConstant = false;
        c.timeConstant = value ? value : 256;
        c.counter = c.timeConstant;
        c.prescale = 0;
        c.running = true;
        if (n == 0) setYmzClock(c.timeConstant);
        return;
    }
    if (!(value & CTC_CONTROL)) {
        if (n == 0) vectorBase = value & 0xf8;     // bits 2-1 are the channel's own
        return;
    }
    c.control = value;
    if (value & CTC_SOFT_RESET) { c.running = false; c.interruptPending = false; }
    if (value & CTC_TIME_CONSTANT) c.expectConstant = true;
    if (!(value & CTC_INT_ENABLE)) c.interruptPending = false;
}

void PlayCity::writePort(int port, int value) {
    port &= 0xffff; value &= 0xff;
    // CPCSE_TRACE_PLAYCITY=1: every write the card takes, and how much sound each chip has
    // made so far -- "the card is silent" is either no writes or no samples.
    static const bool trace = std::getenv("CPCSE_TRACE_PLAYCITY") != nullptr;
    if (trace) std::fprintf(stderr, "PLAYCITY %04X <- %02X  queued R %zu L %zu  tick %d\n", port, value,
                            right.sampleQueue.size(), left.sampleQueue.size(), right.tStatesPerTick);
    if (port >= 0xf880 && port <= 0xf883) writeChannel(port - 0xf880, value);
    else if (port == 0xf984) right.select(value);
    else if (port == 0xf884) { if (right.selected < 14) right.write(value); }
    else if (port == 0xf988) left.select(value);
    else if (port == 0xf888) { if (left.selected < 14) left.write(value); }
}

int PlayCity::readPort(int port) {
    port &= 0xffff;
    if (port >= 0xf880 && port <= 0xf883) return ch[port - 0xf880].counter & 0xff;   // the down-counter
    if (port == 0xf884) return right.selected < 14 ? right.registers[right.selected] : 0xff;
    if (port == 0xf888) return left.selected < 14 ? left.registers[left.selected] : 0xff;
    return 0xff;
}

void PlayCity::zeroCount(int n) {
    Channel& c = ch[n];
    c.counter = c.timeConstant;
    if (c.control & CTC_INT_ENABLE) c.interruptPending = true;
    // ZC/TO: channel 1's is the NMI, channel 2's clocks channel 3. Channel 0's is the
    // YMZs' clock, which setYmzClock already stands for; channel 3 has no ZC/TO pin.
    if (n == 1) nmiPending = true;
    if (n == 2) countEdge(3);
}

void PlayCity::countEdge(int n) {
    Channel& c = ch[n];
    if (!c.running) return;
    if (c.control & CTC_COUNTER_MODE) {
        if (--c.counter <= 0) zeroCount(n);
    }
}

void PlayCity::advanceMicrosecond() {
    if (!enabled) return;
    right.advanceTStates(4);
    left.advanceTStates(4);
    // The timers: every channel in timer mode counts the 4 MHz system clock through its
    // prescaler. (Channel 0 in counter mode is the YMZ clock and is not run cycle by cycle.)
    for (int n = 0; n < 4; n += 1) {
        Channel& c = ch[n];
        if (!c.running || (c.control & CTC_COUNTER_MODE)) continue;
        const int prescale = (c.control & CTC_PRESCALE_256) ? 256 : 16;
        c.prescale += 4;
        while (c.prescale >= prescale) {
            c.prescale -= prescale;
            if (--c.counter <= 0) zeroCount(n);
        }
    }
}

void PlayCity::cursorPin(bool level) {
    if (!enabled) return;
    if (level && !cursorLast) countEdge(1);
    cursorLast = level;
}

int PlayCity::takeInterruptVector() {
    // The CTC's own priority: channel 0 highest.
    for (int n = 0; n < 4; n += 1)
        if (ch[n].interruptPending) { ch[n].interruptPending = false; return vectorBase | (n << 1); }
    return -1;
}

} // namespace cpcse
