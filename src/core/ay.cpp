// CPCSyntaxError — AY-3-8912 sound generator.
#include "ay.h"
#include "keyboard.h"

namespace cpcse {

const double AY_LOG_VOLUME[16] = {
    0, 0.291348135, 0.377409018, 0.405813687, 0.599450675,
    0.741130694, 1.12748913, 2.08514534, 2.36758984, 4.09155413,
    5.64934768, 6.96772717, 8.8908217, 10.8194095, 12.9095903, 15
};
static double VOLUME[16];
static const int MASKS[16] = { 0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff, 0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff };
static bool volumeInit = [] { for (int i = 0; i < 16; i++) VOLUME[i] = AY_LOG_VOLUME[i] / 15.0; return true; }();

AY38912::AY38912(KeyboardMatrix* keyboard, int clockRate, int tStatesPerTick)
    : keyboard(keyboard), clockRate(clockRate), tStatesPerTick(tStatesPerTick) { reset(); }

void AY38912::reset() {
    registers.fill(0); registers[7] = 0x3f; registers[11] = 0x0d; registers[13] = 0x18;
    selected = 0; active = true;
    toneCounter.fill(0); toneState.fill(0);
    noiseCounter = 0; noiseState = 1;
    lfsr = 1;
    envelopeCounter = 0; envelopeStep = 0; envelopeVolume = 0; envelopeHolding = false;
    ayTStateRemainder = 0; outputSamplePhase = 0;
    // outputSampleRate persists across reset.
    sampleQueue.clear(); sampleReadIndex = 0; lastOutputSample = { 0, 0 };
    tapeNoise = 0;
    beeperNoise = 0;
    reloadEnvelope();
}

void AY38912::setOutputSampleRate(double sampleRate) {
    outputSampleRate = std::max(0.0, std::isfinite(sampleRate) ? sampleRate : 0.0);
    outputSamplePhase = 0; sampleQueue.clear(); sampleReadIndex = 0;
}

void AY38912::advanceTStates(int tStates) {
    ayTStateRemainder += tStates;
    while (ayTStateRemainder >= tStatesPerTick) { ayTStateRemainder -= tStatesPerTick; tick(); }
    if (!outputSampleRate) return;
    outputSamplePhase += tStates * outputSampleRate / clockRate;
    while (outputSamplePhase >= 1) {
        outputSamplePhase -= 1; lastOutputSample = level(); sampleQueue.push_back(lastOutputSample);
    }
    int maximum = (int)std::ceil(outputSampleRate / 4);
    if ((int)sampleQueue.size() - sampleReadIndex > maximum) sampleReadIndex = (int)sampleQueue.size() - maximum;
    if (sampleReadIndex > 4096) { sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + sampleReadIndex); sampleReadIndex = 0; }
}
std::array<double, 2> AY38912::readSample() {
    if (sampleReadIndex < (int)sampleQueue.size()) return sampleQueue[sampleReadIndex++];
    return lastOutputSample;
}
void AY38912::write(int value) {
    if (!active) return;
    int reg = selected;
    registers[reg] = (uint8_t)(value & MASKS[reg]);
    if (reg <= 5) {
        int channel = reg >> 1;
        toneCounter[channel] = (uint16_t)std::min((int)toneCounter[channel], tonePeriod(channel) - 1);
    } else if (reg == 6) {
        noiseCounter = std::min(noiseCounter, noisePeriod() - 1);
    } else if (reg == 11 || reg == 12) {
        envelopeCounter = std::min(envelopeCounter, envelopePeriod() - 1);
    }
    if (reg == 13) reloadEnvelope();
}
void AY38912::writeRegister(int index, int value) {
    const int savedSelected = selected;
    const bool savedActive = active;
    select(index); write(value); selected = savedSelected; active = savedActive;
}
int AY38912::read() {
    if (!active) return 0xff;                    // off the bus: DA7-DA0 high impedance
    // The I/O ports (Kevin Thacker's measurements, "AY-3-8912 Additional information"):
    // as input, a read is the pins alone; as output, the output register ANDed with them.
    // Port B (R15) is inside the 8912 with no pins, so its inputs read high.
    if (selected == 14) {
        const int kb = keyboard ? keyboard->read() : 0xff;
        return registers[7] & 0x40 ? registers[14] & kb : kb;
    }
    if (selected == 15) return registers[7] & 0x80 ? registers[15] : 0xff;
    return registers[selected];
}
int AY38912::tonePeriod(int channel) { return std::max(1, registers[channel * 2] | (registers[channel * 2 + 1] & 0x0f) << 8); }
// Period 0 is period 1 for the tone and noise counters, but half of 1 for the envelope's
// (MAME's ay8910.cpp, from the die).
int AY38912::noisePeriod() { return std::max(1, registers[6] & 0x1f) * 2; }
int AY38912::envelopePeriod() { int period = registers[11] | registers[12] << 8; return period ? period * 2 : 1; }
void AY38912::reloadEnvelope() {
    int shape = registers[13] & 0x0f;
    envelopeCounter = 0; envelopeStep = 0; envelopeHolding = false;
    envelopeAttack = !!(shape & 4);
    envelopeAlternate = !!(shape & 2); envelopeHold = !!(shape & 1);
    envelopeContinue = !!(shape & 8);
    envelopeVolume = envelopeAttack ? 0 : 0x0f;
}
void AY38912::tickEnvelope() {
    if (envelopeHolding) return;
    envelopeStep += 1;
    if (envelopeStep < 16) { envelopeVolume = envelopeAttack ? envelopeStep : 15 - envelopeStep; return; }
    if (!envelopeContinue) { envelopeHolding = true; envelopeVolume = 0; return; }
    if (envelopeHold) {
        envelopeVolume = envelopeAlternate ? (envelopeAttack ? 0 : 15) : (envelopeAttack ? 15 : 0);
        envelopeHolding = true; return;
    }
    if (envelopeAlternate) envelopeAttack = !envelopeAttack;
    envelopeStep = 0; envelopeVolume = envelopeAttack ? 0 : 15;
}
void AY38912::tick() {
    for (int channel = 0; channel < 3; channel += 1) {
        toneCounter[channel] = (uint16_t)(toneCounter[channel] + 1);
        if (toneCounter[channel] >= tonePeriod(channel)) { toneCounter[channel] = 0; toneState[channel] ^= 1; }
    }
    noiseCounter += 1;
    if (noiseCounter >= noisePeriod()) {
        noiseCounter = 0;
        // A 17-bit shift register: bit 0 is the output, bits 0 and 3 XORed go in at bit 16,
        // so the noise repeats every 131071 steps (MAME's ay8910.cpp, from the die).
        lfsr ^= ((lfsr & 1) ^ (lfsr >> 3 & 1)) << 17;
        lfsr >>= 1;
        noiseState = lfsr & 1;
    }
    envelopeCounter += 1;
    if (envelopeCounter >= envelopePeriod()) { envelopeCounter = 0; tickEnvelope(); }
}
std::array<double, 2> AY38912::level() {
    double chA = 0, chB = 0, chC = 0;
    for (int channel = 0; channel < 3; channel += 1) {
        int mixer = registers[7];
        bool toneOk = (mixer & (1 << channel)) || toneState[channel];
        bool noiseOk = (mixer & (8 << channel)) || noiseState;
        if (!toneOk || !noiseOk) continue;
        int volume = registers[8 + channel];
        double lvl = VOLUME[volume & 0x10 ? envelopeVolume : volume & 0x0f];
        if (channel == 0) chA = lvl;
        else if (channel == 1) chB = lvl;
        else chC = lvl;
    }
    double mixNoise = tapeNoise + beeperNoise;
    double left = chA + 0.75 * chB + mixNoise;
    double right = 0.75 * chB + chC + mixNoise;
    return { left, right };
}
} // namespace cpcse
