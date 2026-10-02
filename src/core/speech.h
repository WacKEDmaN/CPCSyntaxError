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
//   LambdaSpeak 3     &FBEE / &FBFE (Michael Wessel; its manual, github.com/lambdamikel).
//                     Powers up in SSA-1 emulation (here the SP0256 again); bytes of &80
//                     and up are control bytes, &F1 entering SERIAL MODE, where its UART
//                     drives a Catalex MP3 module (core/yx5300.h). Serial mode: bytes
//                     0-254 go out on TX; &FF is an escape (&FF,&FF = 255; &FF,2 flush;
//                     &FF,3/4 RX buffer size; &FF,7/8/9 read the RX buffer; &FF,20 quit;
//                     &FF,30-33 + a value = line settings); a read gives 16 when ready, or
//                     the byte a command asked for. Its DECtalk / Epson speech modes are
//                     the Epson chip's firmware and are not emulated.
#pragma once
#include "common.h"
#include "../../third_party/sp0256/sp0256.h"
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
};

} // namespace cpcse
