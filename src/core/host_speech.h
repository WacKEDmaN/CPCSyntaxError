// CPCSyntaxError — text-to-speech with the host computer's own voices.
//
// LambdaSpeak 3's Epson and DECtalk modes are the Epson S1V30120's firmware (Fonix
// DECtalk 5), which is not public; the emulator speaks their text with the voices the
// host has instead (the user's choice, 2026-10-09):
//   Windows  SAPI 5, in this process, rendered into memory (any installed voice);
//   macOS    /usr/bin/say, rendered to a file;
//   Linux    espeak-ng (or espeak) if installed, rendered to a file.
// What the CPC sent never reaches a command line: the text goes through a file, and the
// programs are started with an argument list, not a shell.
//
// A request is a list of segments, each with its own voice and prosody -- what the Epson
// parser's and DECtalk's in-line commands change part-way through a sentence. It is
// rendered on a thread; the caller polls done(). The result is 16-bit mono PCM.
#pragma once
#include "common.h"

#include <atomic>
#include <mutex>

namespace cpcse {

struct SpeechPhone {
    std::string arpabet;           // DECtalk's ARPAbet ("hx", "ae", "_" a pause)
    int ms = 0;                    // <duration>, 0 = its own
    int note = 0;                  // <,pitch> 1-37 (DECtalk's sung notes), 0 = spoken
};

struct SpeechSegment {
    std::string text;              // plain text, or empty for phones
    std::vector<SpeechPhone> phones;
    int voice = 0;                 // the S1V30120's 0-8: Paul Harry Betty Ursula Dennis Kit Frank Rita Wendy
    int pitch = 0;                 // steps up (+) or down (-) from the voice's own
    int wpm = 240;                 // words a minute
    bool whisper = false;
    double gain = 1.0;
    bool spanish = false;
};

class HostSpeech {
public:
    struct Job {
        std::atomic<bool> done{false};
        std::vector<int16_t> pcm;
        int rate = 22050;
        std::string error;         // when nothing could be rendered
        std::vector<SpeechSegment> segments;   // what was asked for
    };
    // Starts rendering; the job is the caller's to poll and to drop.
    static std::shared_ptr<Job> render(std::vector<SpeechSegment> segments);
    // Whether this host has a voice at all ("" yes; else why not).
    static std::string unavailable();
    // For checks: a renderer that makes `msPerCharacter` of a 1 kHz tone per character
    // (and per phone) and keeps the segments, instead of the host's voices.
    static void useTestRenderer(bool on, int msPerCharacter = 10);
};

} // namespace cpcse
