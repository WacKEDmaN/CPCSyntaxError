// CPCSyntaxError — the Catalex YX5300 serial MP3 player. See the header.
#include "yx5300.h"
#include "minimp3_ex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace cpcse {

struct Yx5300::Decoder {
    mp3dec_ex_t mp3{};
    bool open = false;
    std::vector<mp3d_sample_t> buf;
    size_t at = 0, have = 0;
    ~Decoder() { close(); }
    void close() { if (open) mp3dec_ex_close(&mp3); open = false; buf.clear(); at = have = 0; }
};

Yx5300::Yx5300() : dec(std::make_unique<Decoder>()) {}
Yx5300::~Yx5300() = default;

void Yx5300::reset() {
    dec->close();
    frame.clear(); transmit.clear();
    state = 0; volume = 30; folder = 1; file = 1; loopTrack = loopFolder = false;
    currentName.clear();
    sourcePhase = outPhase = 0.0;
    prevL = prevR = curL = curR = 0.0f;
    left.clear(); right.clear(); readIndex = 0;
}

// A frame starts at 7E and ends at EF; the fields sit at fixed offsets from the start, so
// a frame with or without its two checksum bytes reads the same.
void Yx5300::receive(int byte) {
    byte &= 0xff;
    if (frame.empty() && byte != 0x7e) return;
    frame.push_back(byte);
    if (byte == 0xef && frame.size() >= 8) {
        if (frame[1] == 0xff) command(frame[3], frame[4], frame[5], frame[6]);
        frame.clear();
    } else if (frame.size() > 10) frame.clear();
}

void Yx5300::reply(int cmd, int dh, int dl) {
    const int sum = -(0xff + 0x06 + cmd + 0x00 + dh + dl);
    for (int b : { 0x7e, 0xff, 0x06, cmd, 0x00, dh & 0xff, dl & 0xff, (sum >> 8) & 0xff, sum & 0xff, 0xef })
        transmit.push_back(b);
}

std::vector<int> Yx5300::folders() const {
    std::vector<int> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (card.empty() || !fs::is_directory(card, ec)) return out;
    for (const auto& e : fs::directory_iterator(card, ec)) {
        if (!e.is_directory()) continue;
        const std::string n = e.path().filename().string();
        if (n.size() == 2 && isdigit((unsigned char)n[0]) && isdigit((unsigned char)n[1])) out.push_back(std::atoi(n.c_str()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::pair<int, std::string>> Yx5300::filesIn(int folderNumber) const {
    std::vector<std::pair<int, std::string>> out;
    namespace fs = std::filesystem;
    char name[8]; std::snprintf(name, sizeof name, "%02d", folderNumber);
    std::error_code ec;
    const fs::path dir = fs::path(card) / name;
    if (card.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string n = e.path().filename().string();
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext != ".mp3" || n.size() < 3) continue;
        if (!isdigit((unsigned char)n[0]) || !isdigit((unsigned char)n[1]) || !isdigit((unsigned char)n[2])) continue;
        out.push_back({ std::atoi(n.substr(0, 3).c_str()), e.path().string() });
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool Yx5300::play(int folderNumber, int fileNumber) {
    dec->close();
    state = 0;
    for (const auto& f : filesIn(folderNumber)) {
        if (f.first != fileNumber) continue;
        if (mp3dec_ex_open(&dec->mp3, f.second.c_str(), 0) != 0) break;
        dec->open = true;
        folder = folderNumber; file = fileNumber;
        currentName = std::filesystem::path(f.second).filename().string();
        state = 1;
        return true;
    }
    reply(0x40, 0, 0);                  // file not found
    return false;
}

bool Yx5300::playIndex(int index) {
    int n = 0;
    for (int fo : folders())
        for (const auto& f : filesIn(fo))
            if (++n == index) return play(fo, f.first);
    reply(0x40, 0, 0);
    return false;
}

void Yx5300::command(int cmd, int feedback, int dh, int dl) {
    const int value = (dh << 8) | dl;
    switch (cmd) {
        case 0x01: { auto fs = filesIn(folder); for (const auto& f : fs) if (f.first > file) { play(folder, f.first); break; } break; }
        case 0x02: { auto fs = filesIn(folder); for (auto it = fs.rbegin(); it != fs.rend(); ++it) if (it->first < file) { play(folder, it->first); break; } break; }
        case 0x03: loopTrack = false; playIndex(value); break;
        case 0x04: volume = std::min(30, volume + 1); break;
        case 0x05: volume = std::max(0, volume - 1); break;
        case 0x06: volume = std::clamp(dl, 0, 30); break;
        case 0x08: loopTrack = true; playIndex(value); break;
        case 0x0c: reset(); reply(0x3f, 0, 0x02); return;      // initialised, TF card present
        case 0x0d: if (state == 2) state = 1; else if (state == 0 && dec->open) state = 1; break;
        case 0x0e: if (state == 1) state = 2; break;
        case 0x0f: loopTrack = false; play(dh, dl); break;
        case 0x16: dec->close(); state = 0; break;
        case 0x17: loopFolder = true; { auto fs = filesIn(dh); if (!fs.empty()) play(dh, fs.front().first); } break;
        case 0x19: loopTrack = dl == 0; break;                  // 0 = on, 1 = off
        case 0x22: volume = std::clamp(dh, 0, 30); playIndex(dl); break;
        case 0x42: reply(0x42, 0x01, state); return;            // TF card, and play state
        case 0x43: reply(0x43, 0, volume); return;
        case 0x44: reply(0x44, 0, 0); return;
        case 0x48: { int n = 0; for (int fo : folders()) n += (int)filesIn(fo).size(); reply(0x48, n >> 8, n & 0xff); return; }
        case 0x4c: reply(0x4c, file >> 8, file & 0xff); return;
        case 0x4e: { int n = (int)filesIn(value).size(); reply(0x4e, n >> 8, n & 0xff); return; }
        case 0x4f: { int n = (int)folders().size(); reply(0x4f, 0, n); return; }
        default: break;                  // select device, sleep, wake, EQ, DAC: nothing to do here
    }
    if (feedback) reply(0x41, 0, 0);
}

// One stereo frame from the file, -1..1; false at its end.
bool Yx5300::nextSourceFrame(float& l, float& r) {
    Decoder& d = *dec;
    if (!d.open) return false;
    const int channels = std::max(1, d.mp3.info.channels);
    if (d.at + channels > d.have) {
        d.buf.resize(4096 * channels);
        d.have = mp3dec_ex_read(&d.mp3, d.buf.data(), d.buf.size());
        d.at = 0;
        if (d.have < (size_t)channels) return false;
    }
    l = d.buf[d.at] / 32768.0f;
    r = channels > 1 ? d.buf[d.at + 1] / 32768.0f : l;
    d.at += channels;
    return true;
}

void Yx5300::advanceMicrosecond(double outputRate) {
    if (state == 1 && dec->open) {
        sourcePhase += (double)dec->mp3.info.hz / 1e6;
        while (sourcePhase >= 1.0) {
            sourcePhase -= 1.0;
            prevL = curL; prevR = curR;
            if (!nextSourceFrame(curL, curR)) {
                // The track ended: tell the CPC side, and loop or move on as asked.
                reply(0x3d, 0, file & 0xff);
                curL = curR = 0.0f;
                if (loopTrack) play(folder, file);
                else if (loopFolder) {
                    auto fs = filesIn(folder);
                    auto it = std::find_if(fs.begin(), fs.end(), [&](const auto& f) { return f.first > file; });
                    play(folder, it != fs.end() ? it->first : (fs.empty() ? file : fs.front().first));
                } else { dec->close(); state = 0; }
                break;
            }
        }
    } else {
        prevL = prevR = curL = curR = 0.0f;
    }
    // The module's volume is 0..30; a quarter-power curve is near enough to its steps.
    const float gain = volume <= 0 ? 0.0f : std::pow(volume / 30.0f, 2.0f);
    outPhase += outputRate / 1e6;
    while (outPhase >= 1.0) {
        outPhase -= 1.0;
        const float t = (float)sourcePhase;
        left.push_back((prevL + (curL - prevL) * t) * gain);
        right.push_back((prevR + (curR - prevR) * t) * gain);
    }
    if (readIndex > 4096) {
        left.erase(left.begin(), left.begin() + (long)readIndex);
        right.erase(right.begin(), right.begin() + (long)readIndex);
        readIndex = 0;
    }
    const size_t cap = (size_t)(outputRate / 4);
    if (left.size() - readIndex > cap) readIndex = left.size() - cap;
}

void Yx5300::readSample(float& l, float& r) {
    if (readIndex < left.size()) { l = left[readIndex]; r = right[readIndex]; readIndex += 1; }
    else { l = r = 0.0f; }
}

} // namespace cpcse
