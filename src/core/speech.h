// CPCSyntaxError — the CPC's two SP0256-AL2 speech synthesisers.
//
//   Amstrad SSA-1     &FBEE (and &FAEE), SP0256 at 3.12 MHz.
//                     write: the allophone; read: bit 7 = SBY (1 idle), bit 6 = 0 when it
//                     can take the next allophone (LRQ), the rest 1.
//   dk'tronics        &FBFE, SP0256 at the bus's 4 MHz.
//                     write: the allophone (6 bits); read: bit 7 = 0 when it can take the
//                     next one (LRQ), SBY not wired, the rest 1.
// (MAME src/devices/bus/cpc/cpc_ssa1.cpp; LambdaSpeak 3's manual gives the same ready
// values for its emulations of both.)
//
// The SP0256 itself is MAME's (third_party/sp0256), run in emulated time -- one of its
// samples every 312 of its clocks -- so LRQ and SBY change when the program polls them,
// not when the host next fills its audio buffer. Its allophones live in the AL2's 2 KB
// internal ROM (General Instrument's), which is not shipped: sp0256-al2.bin in roms.
//
//   LambdaSpeak 3     &FBEE / &FBFE (Michael Wessel; its manual and firmware source,
//                     github.com/lambdamikel/LambdaSpeak3 -- GPL-3: read for the
//                     protocol, none of it copied). Bytes of &80 and up are control bytes
//                     (the README's table); below &80 are content for the current mode:
//     SSA-1 / dk'tronics emulation (&ED / &EC, &ED at power-up) and their SP0 modes
//                     (&E2 / &E1): allophones, here to the SP0256 (the board's optional
//                     SP0256-AL2; the board itself feeds the Epson chip in &ED / &EC).
//                     It puts exact bytes on the bus: SSA-1 128 idle (SBY), 0 speaking
//                     and loadable, 64 busy (LRQ); dk'tronics 0 loadable, 128 busy.
//     Epson / DECtalk (&EF / &EE): text, buffered (253 at most) and spoken at a CR
//                     (or when full); \xHH is a byte. READY is 32; while a byte is
//                     handled and while it speaks, 0. In blocking mode (&EA, the
//                     default) the board holds the Z80 (its READY line) for the whole
//                     sentence; non-blocking (&EB) lets it run, and only &DF (stop) is
//                     heard until the speech ends. Spoken by the host's voices
//                     (host_speech.h): the Epson chip's firmware is not public.
//     Serial (&F1):   its UART drives a Catalex MP3 module (core/yx5300.h): bytes
//                     0-254 go out on TX; &FF escapes (&FF,&FF = 255; &FF,2 flush;
//                     &FF,3/4 RX buffer size; &FF,7/8/9 read the RX buffer; &FF,20 quit;
//                     &FF,30-33 + a value = line settings); a read gives 16 when ready, or
//                     the byte a command asked for.
//   Getters (&CE volume, &CD voice, &CC rate ...) put value x 16 on the bus for the getter
//   delay (20 ms; &E5 10 us, &E0 50 us), then 0 for as long, then READY; the 8-bit ones
//   (&C9 version, &F2 mode, the clock) the value, 255, then 0. Command confirmations
//   ("Native Epson mode.") are spoken unless &E8 turned them off, holding the Z80 unless
//   &F4 made them non-blocking.
#pragma once
#include "common.h"
#include "../../third_party/sp0256/sp0256.h"
#include "host_speech.h"
#include "yx5300.h"

namespace cpcse {

class SpeechSynth {
public:
    enum class Kind { None, Ssa1, DkTronics, LambdaSpeak3 };
    Kind kind = Kind::None;
    bool hasRom = false;

    SpeechSynth();
    void setKind(Kind k);
    bool loadRom(const Bytes& al2);     // 2 KB; returns false on a wrong size
    void reset();
    bool handlesPort(int port) const;
    void writePort(int port, int value);
    int readPort(int port);
    void advanceMicrosecond();
    void setOutputSampleRate(double rate) { outputRate = rate; outPhase = 0; queue.clear(); readIndex = 0; }

    // Host-rate mono samples (-1..1) made by the same stretch of CPC time as the AY's.
    std::vector<float> queue;
    size_t readIndex = 0;
    float readSample() { return readIndex < queue.size() ? queue[readIndex++] : last; }
    // LambdaSpeak 3's MP3 module, and whether the board is in its serial mode.
    Yx5300 mp3;
    bool serialMode = false;

    // LambdaSpeak 3's modes, numbered as its firmware's CUR_MODE (&F2 / &CF report them).
    enum Ls3Mode { LS3_SSA1 = 0, LS3_EPSON = 1, LS3_DECTALK = 2, LS3_DK = 3, LS3_SSA1_SP0 = 5, LS3_DK_SP0 = 6, LS3_SERIAL = 9 };
    int ls3Mode = LS3_SSA1;
    // its settings (power-on values from its firmware)
    bool ls3Blocking = true, ls3Confirm = true, ls3NonBlockConfirm = false;
    int ls3Voice = 1, ls3Rate = 9, ls3Volume = 13, ls3Language = 0, ls3FlushDelay = 10, ls3Getters = 0;
    // The board holds the Z80 (blocking speech, a spoken confirmation).
    bool holdsCpu() const { return kind == Kind::LambdaSpeak3 && !steps.empty() && steps.front().hold; }
    // What it was last asked to say (for the checks, and the GUI's status).
    std::vector<SpeechSegment> lastSaid;
    std::string speechError;         // the host could not speak (no voice installed)
    // The Epson parser's and DECtalk's in-line commands, as segments.
    static std::vector<SpeechSegment> parseEpson(const std::string& text, int voice, int wpm, bool spanish);
    static std::vector<SpeechSegment> parseDecTalk(const std::string& text, int voice, int wpm, bool spanish);

private:
    Sp0256 chip;
    double chipRate = 10000.0, chipPhase = 0.0;
    double outputRate = 44100.0, outPhase = 0.0;
    float previous = 0.0f, current = 0.0f, last = 0.0f;
    // LambdaSpeak 3's serial mode.
    int escape = 0;                  // 0 none; 1 after &FF; 2 a TX byte; 3 a line setting; 4/5 a cursor
    int pendingResult = -1;          // the byte the last command put on the port
    bool directMode = true;
    std::vector<int> txBuffer, rxBuffer;
    size_t rxCursor = 0;
    void serialWrite(int value);
    void tx(int value);

    // LambdaSpeak 3's own work, one step after another: a byte on the bus for a time, or a
    // sentence spoken (the bus 0, or what a getter left there). While a step runs the board
    // is not listening -- only &DF reaches a sentence being spoken.
    struct Step {
        int bus = 0;
        long long us = 0;            // how long (a byte on the bus)
        bool hold = false;           // the Z80 is held meanwhile
        std::vector<SpeechSegment> say;   // a sentence
        std::shared_ptr<HostSpeech::Job> job;
        size_t played = 0;           // its samples heard so far
        double playPhase = 0;
        bool started = false;
    };
    std::deque<Step> steps;
    long long stepUs = 0;            // how long the front step has run
    std::string textBuffer;          // Epson / DECtalk content waiting for its CR
    int hexState = 0, hexHigh = 0;   // \xHH
    int ls3Pending = 0;              // bytes a command still takes (&F0, &DB, &DA): how many
    int ls3LastMode = LS3_SSA1;
    float ttsSample = 0.0f;
    void ls3Reset();
    void ls3Write(int value);
    int ls3Read();
    void ls3Control(int value);
    void ls3Advance();
    void ls3Speak(const std::string& text, bool hold);
    void ls3Confirmation(const std::string& text);   // spoken unless confirmations are off
    void ls3Getter(int value, bool fourBit, const std::string& name);
    long long getterUs() const { return ls3Getters == 1 ? 10 : ls3Getters == 2 ? 50 : 20000; }
    int ls3Idle();                   // the mode's READY byte
};

} // namespace cpcse
