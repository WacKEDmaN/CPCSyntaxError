// CPCSyntaxError — Cassette image and pulse engine.
// Decodes CDT/TZX/TAP blocks and supplies CPC/Spectrum tape timing.
#pragma once
#include "common.h"

namespace cpcse {

class AY38912;

struct TapeBlock {
    int id = 0, start = 0, length = 0, fileOffset = 0;
    Bytes cswRle;
    bool hasCswRle = false;
    int cswPulseCount = 0;
};

struct PulseEvent {
    enum Type { PULSE, LEVEL, STOP } type = PULSE;
    int cycles = 0;
    int level = 0;
    const char* reason = "";   // a literal: a long recording queues millions of these
};

struct LoopFrame { int start; int remaining; };

struct GeneralizedDefinition { int flags; std::vector<int> pulses; };

class CPCTapeDrive {
public:
    AY38912* ay;
    bool requireMotor;
    double tstateRatio;
    int headerPilotPulses = 8064;
    int dataPilotPulses = 3220;
    int initialSignalLevel = 1;
    std::string stopTapeMode = "always";
    bool tapeRelayDelay = true;
    std::string machineModel = "cpc";

    bool loaded = false;
    std::string name, format;
    Bytes data;
    std::vector<TapeBlock> blocks;
    double outputSampleRate = 0;
    double samplePhase = 0;
    std::vector<double> sampleQueue;
    int sampleReadIndex = 0;
    double lastOutputSample = 0;
    double pulseCarry = 0;         // the fraction of a cycle the last pulse rounded away
    std::string lastError;

    bool motorOn = false;
    int pulseLedCycles = 0;
    int tapeCounter = 0;
    int tapeCounterCycles = 0;
    int startupDelayCycles = 0;
    bool playing = false;
    int currentBlock = 0, fastBlockIndex = 0;
    bool tapeEnded = false;
    std::vector<PulseEvent> pulseQueue;
    int pulseQueueIndex = 0;
    int pulseCyclesLeft = 0;
    int currentPulseLevel = 0;
    int portBBit = 0;
    bool ampHigh = false;
    std::vector<LoopFrame> loopStack;
    std::deque<int> callStack;
    std::vector<LoopFrame> fastLoopStack;
    std::deque<int> fastCallStack;
    bool fastFinished = false;

    // THE DECK: where the tape is, and its buttons. The position counts the cycles the tape
    // has actually moved under the head (playing, not paused, the motor on); each block's
    // start is measured once, when the tape goes in, by decoding it. PAUSE holds the tape
    // with PLAY still down; STOP lets PLAY up.
    bool paused = false;
    long long playedCycles = 0;
    long long counterZeroCycles = 0;            // the counter's 000 (its reset button)
    std::vector<long long> blockStartCycles;    // per block, where it starts on the tape
    long long totalCycles = 0;
    double cyclesPerSecond() const { return tstateRatio * 3500000.0; }
    double positionSeconds() const { return playedCycles / cyclesPerSecond(); }
    double lengthSeconds() const { return totalCycles / cyclesPerSecond(); }
    int blockAtPosition() const;                // the block under the head (-1: no tape)
    int counter() const;                        // 000-999, a second a count, from its last reset
    void resetCounter() { counterZeroCycles = playedCycles; }
    void play();                                // PLAY down (and PAUSE up)
    void setPaused(bool on);
    void stop();                                // PLAY up
    void fastForward();                         // to the start of the next block
    void rewindBlock();                         // to this block's start, or the one before it
    void seekBlock(int index);                  // to a block's start
    void buildTimeline();

    CPCTapeDrive(AY38912* ay = nullptr, bool requireMotor = true, double tstateFrequency = 1000000);
    virtual ~CPCTapeDrive() = default;

    bool isMotorActive();
    bool isActive();
    bool isPulseActive();
    void reset();
    void eject();
    void setOutputSampleRate(double sampleRate);
    double readSample();
    void parseTzx(const Bytes& data);
    void parseTap(const Bytes& data);
    bool load(const Bytes& input, const std::string& fileName = "tape.cdt");
    bool loadAsync(const Bytes& input, const std::string& fileName = "tape.cdt");
    void prepareCompressedBlocks();
    void rewind();
    void setMotor(bool on);
    bool isMotorOn();
    int getPortBBit();
    int getEarLevel();
    void setTapeNoise(double level);
    void addPulse(double cycles, int level);
    void addLevel(int level);
    void addStop(const char* reason = "stop");
    double tstatesToCycles(double tstates);
    double msToCycles(double ms);
    void emitPilot(double tstates, int count);
    void emitSync(double t1, double t2);
    void emitData(int p, int length, double t0, double t1, int lastBits);
    void emitDirect(int p, int length, double tstatesPerSample, int bitsUsedInLastByte);
    void emitCsw(TapeBlock& block);
    struct GenParsed { std::vector<GeneralizedDefinition> definitions; int next; };
    GenParsed readGeneralizedDefinitions(int offset, int alphabetSize, int maximumPulses, int limit);
    void emitGeneralizedSymbol(const GeneralizedDefinition& definition);
    void emitGeneralized(TapeBlock& block);
    int getSelectTarget(const TapeBlock& block, int index);
    void emitPause(double ms);
    bool shouldStopOn48KBlock();
    std::string decodeBlock(int index);
    void ensurePulses();
    bool processQueueEvent(const PulseEvent& event);
    void advanceCycles(int cycles = 1);
    void jumpToBlock(int index);
    std::optional<Bytes> getBlockData(int index);
    std::optional<Bytes> getNextLoadableBlock();
    std::string getBlockDescription(int index);
    bool togglePlay();
};

// Spectrum transport: no motor relay, one tape tick per Z80 T-state.
class SpectrumTapeDrive : public CPCTapeDrive {
public:
    explicit SpectrumTapeDrive(AY38912* ay = nullptr);
    void setModel(const std::string& model);
};

} // namespace cpcse
