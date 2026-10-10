// CPCSyntaxError — AY-3-8912 sound generator.
// Tone, noise, envelope, mixer and CPC keyboard-port access.
#pragma once
#include "common.h"

namespace cpcse {

class KeyboardMatrix;

extern const double AY_LOG_VOLUME[16];

class AY38912 {
public:
    KeyboardMatrix* keyboard;
    int clockRate;
    int tStatesPerTick;
    std::array<uint8_t, 16> registers{};
    double tapeNoise = 0;
    double beeperNoise = 0;

    int selected = 0;
    // The address the bus last gave was this chip's: DA7-DA4 0000 (GI's data sheet, "Address
    // Decoding"). Any other high nibble leaves the latch alone and the chip off the bus.
    bool active = true;
    std::array<uint16_t, 3> toneCounter{};
    std::array<uint8_t, 3> toneState{};
    int noiseCounter = 0, noiseState = 1;
    int lfsr = 1;              // the noise generator's 17-bit shift register
    int envelopeCounter = 0, envelopeStep = 0, envelopeVolume = 0;
    bool envelopeHolding = false;
    bool envelopeAttack = false, envelopeAlternate = false, envelopeHold = false, envelopeContinue = false;
    int ayTStateRemainder = 0;
    double outputSamplePhase = 0;
    double outputSampleRate = 0;
    std::vector<std::array<double, 2>> sampleQueue;
    int sampleReadIndex = 0;
    std::array<double, 2> lastOutputSample{ 0, 0 };

    explicit AY38912(KeyboardMatrix* keyboard, int clockRate = 4000000, int tStatesPerTick = 32);
    void reset();
    void setOutputSampleRate(double sampleRate);
    void advanceTStates(int tStates);
    std::array<double, 2> readSample();
    void select(int value) { active = (value & 0xf0) == 0; if (active) selected = value & 0x0f; }
    void write(int value);
    void writeRegister(int index, int value);
    int read();
    int tonePeriod(int channel);
    int noisePeriod();
    int envelopePeriod();
    void reloadEnvelope();
    void tickEnvelope();
    void tick();
    std::array<double, 2> level();
};

} // namespace cpcse
