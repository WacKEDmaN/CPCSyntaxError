// CPCSyntaxError — an OPL4 sound card on the AMSDAP. See opl4.h.
#include "opl4.h"
#include "ymfm_opl.h"

namespace cpcse {

// ymfm's chip, with this card as its outside world.
class Opl4Chip : public ymfm::ymfm_interface {
public:
    explicit Opl4Chip(Opl4Card& card) : card(card), chip(*this) {}
    Opl4Card& card;
    ymfm::ymf278b chip;
    void timerExpired(uint32_t tnum) { m_engine->engine_timer_expired(tnum); }
    void ymfm_set_timer(uint32_t tnum, int32_t duration) override { card.setTimer(tnum, duration); }
    void ymfm_set_busy_end(uint32_t clocks) override { card.setBusyEnd(clocks); }
    bool ymfm_is_busy() override { return card.isBusy(); }
    void ymfm_update_irq(bool asserted) override { card.setIrq(asserted); }
    uint8_t ymfm_external_read(ymfm::access_class type, uint32_t address) override {
        return type == ymfm::ACCESS_PCM ? card.memoryRead(address) : 0;
    }
    void ymfm_external_write(ymfm::access_class type, uint32_t address, uint8_t data) override {
        if (type == ymfm::ACCESS_PCM) card.memoryWrite(address, data);
    }
};

// CPC T-states (4 MHz) to the chip's 33.8688 MHz clock
static constexpr double CLOCKS_PER_CYCLE = Opl4Card::CLOCK_HZ / 4000000.0;

Opl4Card::Opl4Card() : chip(std::make_unique<Opl4Chip>(*this)) {
    ram.assign(2048 * 1024, 0);
    reset();
}
Opl4Card::~Opl4Card() = default;

void Opl4Card::setEnabled(bool on) {
    if (enabled == on) return;
    enabled = on;
    reset();
}
void Opl4Card::setRamKiB(int kib) {
    kib = std::clamp(kib, 128, 2048);
    if ((int)ram.size() != kib * 1024) ram.assign((size_t)kib * 1024, 0);
}
void Opl4Card::reset() {
    chip->chip.reset();
    samples.clear();
    timerEnd[0] = timerEnd[1] = -1;
    busyUntil = 0;
    irq = false;
    samplePhase = 0;
    resync = true;
    schedule();
}

bool Opl4Card::handlesPort(int port) const {
    if (!enabled || (port >> 8 & 0xff) != 0xff) return false;
    const int low = port & 0xff;
    return (low >= 0xc4 && low <= 0xc7) || low == 0x7e || low == 0x7f;
}
static int chipOffset(int low) {
    switch (low) {
        case 0xc4: return 0; case 0xc5: return 1; case 0xc6: return 2; case 0xc7: return 3;
        case 0x7e: return 4; default: return 5;
    }
}
// CPCSE_TRACE_OPL4=<n>: the first n accesses, to see what a program does with the card.
static long traceLeft() { static long n = std::getenv("CPCSE_TRACE_OPL4") ? std::atol(std::getenv("CPCSE_TRACE_OPL4")) : 0; return n; }
static long traced = 0;
int Opl4Card::readPort(int port, long long cpcCycles) {
    advanceTo(cpcCycles);
    const int off = chipOffset(port & 0xff);
    const int v = (off == 0 || off == 5) ? chip->chip.read((uint32_t)off) : 0xff;
    if (traced < traceLeft()) { traced++; std::fprintf(stderr, "OPL4 IN  %04x -> %02x\n", port & 0xffff, v); }
    return v;
}
void Opl4Card::writePort(int port, int value, long long cpcCycles) {
    advanceTo(cpcCycles);
    if (traced < traceLeft()) { traced++; std::fprintf(stderr, "OPL4 OUT %04x <- %02x\n", port & 0xffff, value & 0xff); }
    chip->chip.write((uint32_t)chipOffset(port & 0xff), (uint8_t)value);
}

uint8_t Opl4Card::memoryRead(uint32_t address) const {
    address &= 0x3fffff;
    if (address < 0x200000) return address < rom.size() ? rom[address] : 0;   // no ROM: silence
    address -= 0x200000;
    return address < ram.size() ? ram[address] : 0;
}
void Opl4Card::memoryWrite(uint32_t address, uint8_t value) {
    address &= 0x3fffff;
    if (address < 0x200000) return;                                          // the ROM
    address -= 0x200000;
    if (address < ram.size()) ram[address] = value;
}

void Opl4Card::setTimer(uint32_t tnum, int32_t durationClocks) {
    if (tnum > 1) return;
    timerEnd[tnum] = durationClocks < 0 ? -1 : nowClocks + durationClocks;
    schedule();
}

void Opl4Card::schedule() {
    double next = -1;
    for (double t : timerEnd) if (t >= 0 && (next < 0 || t < next)) next = t;
    nextEventCycles = next < 0 ? LLONG_MAX : lastCycles + (long long)std::ceil((next - nowClocks) / CLOCKS_PER_CYCLE);
}

void Opl4Card::advanceTo(long long cpcCycles) {
    // Just fitted or reset: the chip starts now, not at the CPC's power-on.
    if (resync || !enabled) { lastCycles = cpcCycles; resync = !enabled; schedule(); return; }
    if (cpcCycles <= lastCycles) return;
    double target = nowClocks + (cpcCycles - lastCycles) * CLOCKS_PER_CYCLE;
    lastCycles = cpcCycles;
    ymfm::ymf278b::output_data out;
    while (nowClocks < target) {
        // the next thing to happen: a sample (every 768 clocks) or a timer
        double step = 768.0 - samplePhase;
        int due = -1;
        for (int t = 0; t < 2; t++)
            if (timerEnd[t] >= 0 && timerEnd[t] - nowClocks < step) { step = std::max(0.0, timerEnd[t] - nowClocks); due = t; }
        if (nowClocks + step > target) { samplePhase += target - nowClocks; nowClocks = target; break; }
        nowClocks += step;
        if (due >= 0) {
            timerEnd[due] = -1;
            samplePhase += step;
            chip->timerExpired((uint32_t)due);      // may re-arm itself through setTimer
            continue;
        }
        samplePhase = 0;
        chip->chip.generate(&out, 1);
        samples.push_back((int16_t)out.data[4]);
        samples.push_back((int16_t)out.data[5]);
    }
    // nobody listening (headless): keep no more than a second
    if (samples.size() > (size_t)SAMPLE_RATE * 4)
        samples.erase(samples.begin(), samples.end() - (long)SAMPLE_RATE * 2);
    schedule();
}

} // namespace cpcse
