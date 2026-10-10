// CPCSyntaxError — Cassette image and pulse engine.
// Decodes CDT/TZX/TAP blocks and supplies CPC/Spectrum tape timing.
#pragma once
#include "common.h"

namespace cpcse {

class AY38912;

struct TapeBlock {
    int id = 0, start = 0, length = 0, fileOffset = 0;
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
    // A standard speed block's pilot (TZX 1.20, block &10: 8063 pulses before a flag byte
    // below 128, 3223 otherwise -- the 8064/3220 of older revisions were corrected).
    int headerPilotPulses = 8063;
    int dataPilotPulses = 3223;
    // "An emulator should put the 'current pulse level' to 'low' when starting to play a
    // TZX file, either from the start or from a certain position" (the TZX format).
    int initialSignalLevel = 0;
    // Block &2A stops the tape "ONLY if the machine is an 48K Spectrum" (the TZX format).
    std::string stopTapeMode = "spectrum";
    bool tapeRelayDelay = true;
    std::string machineModel = "cpc";

    bool loaded = false;
    std::string name, format;
    Bytes data;
    std::vector<TapeBlock> blocks;
    double pulseCarry = 0;         // the fraction of a cycle the last pulse rounded away
    std::string lastError;

    bool motorOn = false;
    int tapeCounter = 0;
    int tapeCounterCycles = 0;
    int startupDelayCycles = 0;
    bool playing = false;
    int currentBlock = 0;
    bool tapeEnded = false;
    std::vector<PulseEvent> pulseQueue;
    int pulseQueueIndex = 0;
    int pulseCyclesLeft = 0;
    int currentPulseLevel = 0;
    int portBBit = 0;
    bool ampHigh = false;
    std::vector<LoopFrame> loopStack;
    std::deque<int> callStack;

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

    // RECORDING (REC down with PLAY): the CPC's cassette write line (PPI port C bit 5) onto
    // the tape. While the motor turns, each change of the line ends a pulse; a run of pulses
    // with a silence of RECORD_GAP_US after it is one block, a CSW recording (TZX &18) at
    // RECORD_RATE -- 4 us a sample, so a 1000-baud pulse is one byte of it -- with the silence
    // as its pause. The recording overwrites the tape from the block under the head (or goes
    // after its end), as a deck's erase head would; STOP puts it into the tape image.
    static constexpr int RECORD_RATE = 250000;
    static constexpr long long RECORD_GAP_US = 100000;
    bool recording = false;
    bool modified = false;           // recorded onto since it went in: to be saved
    int writeLevel = 0;              // the line, as the PPI drives it
    long long recordClock = 0;       // microseconds the tape has moved while recording
    long long lastEdge = -1;         // the last change of the line (-1: none yet)
    bool blockOpen = false;          // a block's first edge has come and no silence since
    std::vector<uint32_t> recordPulses;   // the block being recorded
    Bytes recorded;                  // the finished blocks, as TZX
    size_t recordCut = 0;            // where in the image the recording starts
    long long recordFrom = 0;        // and where on the tape (cycles)
    int lastBlockPause = -1;         // offset of the last finished block's pause field in `recorded`
    void newBlank(const std::string& fileName);   // an empty tape
    bool startRecording();           // REC (with PLAY); false: this tape cannot be recorded on
    void stopRecording();            // the recording into the tape (STOP, EJECT, the motor kept on)
    void setWriteLevel(int level);
    void endRecordedBlock();

    CPCTapeDrive(AY38912* ay = nullptr, bool requireMotor = true, double tstateFrequency = 1000000);
    virtual ~CPCTapeDrive() = default;

    bool isMotorActive();
    bool isActive();
    void reset();
    void eject();
    void parseTzx(const Bytes& data);
    void parseTap(const Bytes& data);
    bool load(const Bytes& input, const std::string& fileName = "tape.cdt");
    void rewind();
    void setMotor(bool on);
    int getPortBBit();
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
    std::string getBlockDescription(int index);
    bool togglePlay();
};

} // namespace cpcse
