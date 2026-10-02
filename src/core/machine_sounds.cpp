// CPCSyntaxError — the drive's and the keyboard's mechanical noises, played from
// recordings. See the header.
#include "machine_sounds.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cpcse {

MachineSounds machineSounds;

namespace {
// AMSDOS programs the µPD765A's step rate to 12 ms a track, and the head is a stepper:
// a seek over N tracks is N separate steps, not one.
constexpr float STEP_MS = 12.0f;
constexpr size_t MAX_VOICES = 32;
constexpr size_t MAX_PENDING = 256;
uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
}

// PCM (8/16/24/32-bit) or IEEE float, any rate, any channel count, folded to mono.
bool MachineSounds::readWav(const std::string& path, Sample& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> b;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return false;
    int format = 0, channels = 0, bits = 0, rate = 0;
    const unsigned char* data = nullptr; size_t dataSize = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t size = le32(&b[at + 4]);
        const unsigned char* body = &b[at + 8];
        if (at + 8 + size > b.size()) break;
        if (std::memcmp(&b[at], "fmt ", 4) == 0 && size >= 16) {
            format = le16(body); channels = le16(body + 2); rate = (int)le32(body + 4); bits = le16(body + 14);
            if (format == 0xfffe && size >= 26) format = le16(body + 24);   // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(&b[at], "data", 4) == 0) { data = body; dataSize = size; }
        at += 8 + size + (size & 1);
    }
    if (!data || channels <= 0 || rate <= 0 || (format != 1 && format != 3)) return false;
    const int bytes = bits / 8;
    if (bytes <= 0) return false;
    const size_t frames = dataSize / ((size_t)bytes * channels);
    out.rate = rate;
    out.data.resize(frames);
    for (size_t i = 0; i < frames; i += 1) {
        float sum = 0.0f;
        for (int c = 0; c < channels; c += 1) {
            const unsigned char* p = data + (i * channels + c) * bytes;
            float v = 0.0f;
            if (format == 3 && bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
            else if (bits == 8) v = ((int)p[0] - 128) / 128.0f;
            else if (bits == 16) v = (int16_t)le16(p) / 32768.0f;
            else if (bits == 24) v = (float)((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8) / 8388608.0f;
            else if (bits == 32) v = (float)(int32_t)le32(p) / 2147483648.0f;
            sum += v;
        }
        out.data[i] = sum / channels;
    }
    return !out.data.empty();
}

const MachineSounds::Sample* MachineSounds::Bank::pick() {
    if (variants.empty()) return nullptr;
    const Sample* s = &variants[nextVariant % variants.size()];
    nextVariant += 1;
    return s;
}

void MachineSounds::loadBank(Bank& bank, const std::string& base) {
    bank.variants.clear(); bank.nextVariant = 0;
    Sample s;
    if (readWav(dir + "/" + base + ".wav", s)) { bank.variants.push_back(std::move(s)); loaded += 1; }
    for (int i = 1; i <= 16; i += 1) {
        Sample v;
        if (readWav(dir + "/" + base + std::to_string(i) + ".wav", v)) { bank.variants.push_back(std::move(v)); loaded += 1; }
    }
}

int MachineSounds::load(const std::string& folder) {
    reset();
    dir = folder; loaded = 0;
    loadBank(motorStart, "drive_motor_start");
    loadBank(motorLoop, "drive_motor");
    loadBank(motorStop, "drive_motor_stop");
    loadBank(step, "drive_step");
    loadBank(insertBank, "drive_insert");
    loadBank(ejectBank, "drive_eject");
    loadBank(keyPress, "key_press");
    loadBank(keyRelease, "key_release");
    loadBank(keySpace, "key_space_press");
    loadBank(keyReturn, "key_return_press");
    return loaded;
}

void MachineSounds::reset() {
    voices.clear(); pendingSteps.clear();
    motorOn = false; motorVoice = -1;
    clock = 0; nextStepFree = 0;
}

void MachineSounds::start(const Sample* s, float gain, bool loop) {
    if (!s) return;
    if (voices.size() >= MAX_VOICES) {
        // Drop the oldest one-shot; never the motor.
        for (size_t i = 0; i < voices.size(); i += 1)
            if (!voices[i].loop) { voices.erase(voices.begin() + (long)i); break; }
        if (voices.size() >= MAX_VOICES) return;
    }
    voices.push_back(Voice{ s, 0.0, gain, loop });
}

void MachineSounds::motor(bool on) {
    if (!driveEnabled) on = false;
    if (on == motorOn) return;
    motorOn = on;
    if (on) {
        const Sample* rise = motorStart.pick();
        start(rise);
        // The loop takes over as the run-up ends; with no run-up recording it starts at once.
        if (const Sample* loop = motorLoop.pick()) {
            Voice v{ loop, rise ? -(double)rise->data.size() * loop->rate / rise->rate : 0.0, 1.0f, true };
            voices.push_back(v);
        }
    } else {
        voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice& v) { return v.loop; }), voices.end());
        start(motorStop.pick());
    }
}

void MachineSounds::steps(int count) {
    if (!driveEnabled || count <= 0) return;
    count = std::min(count, 90);
    const long gap = (long)(STEP_MS * sampleRate / 1000.0f);
    const long at = std::max(clock, nextStepFree);
    for (int i = 0; i < count && pendingSteps.size() < MAX_PENDING; i += 1) pendingSteps.push_back(at + i * gap);
    nextStepFree = at + count * gap;
}

void MachineSounds::insert() { if (driveEnabled) start(insertBank.pick()); }
void MachineSounds::eject() { if (driveEnabled) start(ejectBank.pick()); }

void MachineSounds::key(bool pressed, Key which) {
    if (!keysEnabled) return;
    if (!pressed) { start(keyRelease.pick(), 0.8f); return; }
    Bank* bank = &keyPress;
    if (which == Key::Space && !keySpace.variants.empty()) bank = &keySpace;
    if (which == Key::Return && !keyReturn.variants.empty()) bank = &keyReturn;
    start(bank->pick());
}

bool MachineSounds::silent() const { return voices.empty() && pendingSteps.empty(); }

float MachineSounds::next() {
    while (!pendingSteps.empty() && pendingSteps.front() <= clock) {
        pendingSteps.pop_front();
        start(step.pick());
    }
    float out = 0.0f;
    for (size_t i = 0; i < voices.size();) {
        Voice& v = voices[i];
        const std::vector<float>& d = v.s->data;
        if (v.pos >= 0.0) {
            const size_t k = (size_t)v.pos;
            const double frac = v.pos - (double)k;
            const float a = d[k % d.size()];
            const float b = d[(k + 1 < d.size() || v.loop) ? (k + 1) % d.size() : k];
            out += (float)(a + (b - a) * frac) * v.gain;
        }
        v.pos += (double)v.s->rate / sampleRate;
        if (v.pos >= (double)d.size()) {
            if (v.loop) v.pos -= (double)d.size();
            else { voices.erase(voices.begin() + (long)i); continue; }
        }
        i += 1;
    }
    clock += 1;
    return std::clamp(out, -1.0f, 1.0f);
}

} // namespace cpcse
