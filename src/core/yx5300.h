// CPCSyntaxError — a Catalex serial MP3 player (YX5300), as LambdaSpeak 3 drives it
// through its UART. 9600 baud; every message is a 10-byte frame
//     7E FF 06 cmd feedback dataH dataL [checksumH checksumL] EF
// (MD_YX5300 library's definitions). Its micro-SD card is a folder on the host, laid out
// as the module expects: numbered folders 01, 02 ... holding files whose names start with
// their number, 001xxx.mp3, 002xxx.mp3 ... (LambdaSpeak 3's manual: "a single root
// directory '01' and a couple of MP3 files named '001xxx.mp3' to '009xxx.mp3'").
// Decoding is minimp3 (CC0, third_party/minimp3), streamed from the file.
#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace cpcse {

class Yx5300 {
public:
    Yx5300();
    ~Yx5300();
    std::string card;                  // the micro-SD card's folder; empty = no card
    void reset();
    void receive(int byte);            // one byte from the CPC side's TX
    std::deque<int> transmit;          // bytes for the CPC side's RX (replies)
    // Host-rate stereo, made once per CPC microsecond like the AY's.
    void advanceMicrosecond(double outputRate);
    std::vector<float> left, right;
    size_t readIndex = 0;
    void readSample(float& l, float& r);
    bool playing() const { return state == 1; }
    std::string nowPlaying() const { return currentName; }

private:
    struct Decoder;
    std::unique_ptr<Decoder> dec;
    std::vector<int> frame;
    int state = 0;                     // 0 stopped, 1 playing, 2 paused
    int volume = 30;                   // 0..30
    int folder = 1, file = 1;          // what is (or was last) playing
    bool loopTrack = false, loopFolder = false;
    std::string currentName;
    double sourcePhase = 0.0, outPhase = 0.0;
    float prevL = 0, prevR = 0, curL = 0, curR = 0;
    void command(int cmd, int feedback, int dh, int dl);
    void reply(int cmd, int dh, int dl);
    bool play(int folderNumber, int fileNumber);
    bool playIndex(int index);         // the Nth file on the card, all folders in order
    std::vector<std::pair<int, std::string>> filesIn(int folderNumber) const;
    std::vector<int> folders() const;
    bool nextSourceFrame(float& l, float& r);
};

} // namespace cpcse
