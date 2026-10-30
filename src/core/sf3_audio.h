// CPCSyntaxError — the SYMBiFACE III's sound side: its VS1053 decoder as the CPC sees it.
//
// The SF3 answers at &FF20-&FF27 as TMTLOGIC's SE-ONE card ("SE-ONE init is complete, set
// enable SEONE MP3A mode" in the RSF3 boot log, RSF3 SF3 SERIAL DATA FTDI.pdf p.19), which
// in its MP3A mode is an MP3MSX card moved to the CPC's 16-bit ports. Its modes:
//
//   MP3A   the MP3MSX interface SymAmp drives (Prodatron's App-SymAmp-MP3.asm, "MP3
//          ROUTINEN (CPC)", port base &FF):
//            &FF21  reset          &FF22  data: MP3 bytes in, spectrum bytes out
//            &FF23  mode/status    &FF24  I2C status   &FF25  I2C data   &FF27  I2C control
//          &FF23 written: &40 = the next 28 bytes on &FF22 are the decoder's registers,
//          &C0 = write them, &00 = audio mode, &1F (31) = read the spectrum analyser (31
//          bytes on &FF22). Read: bit 4 = busy, bit 7 = "new data is required" (SymAmp then
//          sends 2048 bytes).
//          The I2C master is an ATmega's TWI: control &64 start, &44 send the byte on
//          &FF25, &54 stop; bit 3 of &FF27 = done, &FF24 = the TWI status (&08 start, &10
//          repeated start, &18 address acknowledged, &28 data acknowledged). On the bus is a
//          Philips TDA8425 at &82 (volume, bass, treble, switches: its datasheet, Oct 1988).
//   MP3B   the SE-ONE's USB player, run by AT commands on &FF20 (SEPLAYER.BAS): files from
//          the USB stick (drive 1:) or the SD card (drive 0:), listed on &FF25.
//          &FF23 bit 3 = nothing plays, bit 5 = a file plays; &FF26 / &FF27 = VU left /
//          right, 0..8.
//   REG    samples (SAMPLER.BAS, SE-TALK.BAS): &FF26 = 0, &FF27 = n, &FF26 = &80 plays sample
//          n (AT+MSDSETSAMPLE); &FF26 = &7E says SP0256 allophone n (SETALK/SPnnnH.MP3).
//          &FF26 read: 0 done, 1 busy, 2 error.
//   SYNTH  &FF22 bytes are MIDI to the VS1053's real-time synth (SE-SYNTH.BAS).
//
// AT commands: the text, then 13, on &FF20. The answer, read from &FF20 (0 = nothing yet), is
// the command again with "?" made "=value", then 13 10 ("AT+MSDSIZE=1234"); an answer that
// starts with "E" is an error, AT+SEFAULTTXT? says which. A new command drops what was not read.
//
// The SF3's function 40 picks where its audio goes; 6 is "midi" (EMR-MIDI.BAS): the EMR MIDI
// interface at &F8F0-&F8F3 as EMR's own code drives it (EMRMCB.BIN): &F8F2 status, bit 0 =
// a byte came in, bit 1 = ready to send; &F8F3 data. MIDI goes to the host's MIDI out.
//
// Decoding is minimp3 (MP3) and a RIFF reader (WAV). The VS1053's other formats (AAC and
// AAC+, Ogg Vorbis, WMA, and here M4A/MP4, FLAC, Opus too) go to ffmpeg when it is on this
// computer (`ffmpeg`, host_process.h); without it they answer "FORMAT NOT SUPPORTED", as do
// MIDI files. A web radio stream's format is told from its own bytes (two ADTS AAC headers
// a frame apart, or "OggS"); anything else is MP3.
#pragma once
#include <array>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace cpcse {

class Sf3Audio {
public:
    Sf3Audio();
    ~Sf3Audio();
    std::string usbFolder;             // drive 1: (the USB stick); empty = none
    std::string sdFolder;              // drive 0: (the SD card); empty = none
    std::string ffmpeg;                // ffmpeg's program; empty = not on this computer

    enum class Mode { Mp3a, Mp3b, Reg, Synth, Fm };
    Mode mode = Mode::Mp3a;            // SF3_CPC.INI "AUDIO = SYMAMP" (Quick start manual p.8)
    int route = 0;                     // the SF3's function 40
    int masterVolume = 100;            // the SF3's function 41, 0..100

    void reset();
    static bool isPort(int port) { port &= 0xffff; return port >= 0xff20 && port <= 0xff27; }
    bool isMidiPort(int port) const { port &= 0xffff; return route == 6 && port >= 0xf8f0 && port <= 0xf8f3; }
    int readPort(int port);
    void writePort(int port, int value);

    void setOutputSampleRate(double rate) { outputRate = rate > 0 ? rate : 44100.0; }
    void advanceMicrosecond();
    void readSample(float& l, float& r);
    void setBeep(int a, int b);
    // Web radio (the SF3's route 5, WEB-DK.BAS): the SF3 copies a socket's data into the
    // decoder (WiFi function 21). Its ARM buffers it; here 16 KB fill before the sound
    // starts, and again after the stream ran dry (this emulator's choice).
    size_t streamRoom() const;
    void feedStream(const unsigned char* data, size_t n);
    void setRoute(int r);
    // The web radio stream's format, once told: 0 not yet, 1 MP3, 2 ffmpeg, 3 not playable.
    int streamKind = 0;

    // What a window or a check can see.
    bool playing() const;
    std::string nowPlaying() const { return playingName; }
    std::string lastFault() const { return fault; }
    int tdaVolume(int side) const { return tda[side & 1]; }
    size_t streamPending() const { return stream.size() - streamAt; }

private:
    struct Source;
    std::unique_ptr<Source> source;    // the file or sample playing (MP3B, REG)
    struct StreamDecoder;
    std::unique_ptr<StreamDecoder> streamDec;
    struct Ffmpeg;
    std::unique_ptr<Ffmpeg> streamFf;  // the web radio stream through ffmpeg
    void flushStream();
    void tellStream();
    double outputRate = 44100.0;

    // MP3A
    std::vector<unsigned char> stream; // MP3 bytes not yet decoded
    size_t streamAt = 0;
    int modeReg = 0;
    bool filling = true;               // web radio: waiting for the start buffer
    std::vector<int> decoderRegs;      // the 28 bytes after &40
    std::deque<int> spectrumOut;
    int i2cStatus = 0xf8, i2cData = 0, i2cIndex = -1, i2cPointer = 0;
    bool i2cAddressed = false;
    std::array<int, 4> tda{ 0x3c, 0x3c, 0x36, 0x36 };   // VL VR BA TR as SymAmp writes them
    int tdaSwitch = 0xee;              // S1: mute after power-on (datasheet Table 5)

    // AT commands (all modes)
    std::string atLine;
    std::deque<int> atOut;
    std::deque<int> listOut;           // AT+MSDFILES, read on &FF25 in MP3B
    std::string fault;
    int drive = 1;
    std::vector<std::string> path;     // in the drive's folder
    std::array<std::string, 10> samples;
    int regCommand = 0, regData = 0, regStatus = 0;
    int dspVol[2] = { 100, 100 }, dspBass = 50, dspTreble = 50;
    double fmFreq = 87.5;
    std::string playingName;
    bool paused = false;
    void command(const std::string& line);
    void answer(const std::string& text);
    void error(const std::string& why);
    std::string folderNow() const;
    std::string findFile(const std::string& name) const;
    bool startFile(const std::string& file);
    void stopFile();

    // the buzzer
    bool beepOn = false;
    float beepLevel = 0;
    double beepHz = 1000, beepPhase = 0;

    // output
    double sourcePhase = 0.0, outPhase = 0.0;
    float prevL = 0, prevR = 0, curL = 0, curR = 0;
    int sourceHz = 44100;
    std::vector<float> left, right;
    size_t readIndex = 0;
    std::vector<float> recent;         // mono, at the source rate, for the analyser and the VU
    size_t recentAt = 0;
    float peak[2] = { 0, 0 };
    float bassState[2] = { 0, 0 }, trebleState[2] = { 0, 0 };
    bool nextFrame(float& l, float& r);
    void spectrum();
    int vu(int side) const;
};

} // namespace cpcse
