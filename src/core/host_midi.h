// CPCSyntaxError — MIDI out to the host computer.
//
// LambdaSpeak 3's UART at 31250 baud is MIDI ("BAUD mode 6, which configures it for 31250
// BAUDs as required for MIDI", its README). The bytes it sends are cut into MIDI messages
// here (running status, System Exclusive, real-time bytes between) and sent to the host:
//   Windows  the MIDI mapper through winmm (the GS wavetable synth, or the device chosen)
//   macOS    Apple's DLS software synthesizer (AudioToolbox), out through the default output
//   Linux    an ALSA sequencer port "CPCSyntaxError", connected to the first synthesizer found
//            (FluidSynth, TiMidity, a sound card's synth); libasound is loaded when the first
//            byte comes, so the program runs without it. With none, error() says why.
#pragma once
#include "common.h"

namespace cpcse {

class HostMidi {
public:
    // One byte as it leaves the UART.
    static void send(int byte);
    // The messages as cut, for a check (1-3 bytes, or a whole SysEx F0 ... F7); while a sink
    // is set nothing reaches the host.
    static void setSink(std::function<void(const std::vector<int>&)> sink);
    static std::string error();
    static void close();
};

} // namespace cpcse
