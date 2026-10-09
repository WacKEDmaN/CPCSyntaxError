// CPCSyntaxError — text-to-speech with the host's voices. See host_speech.h.
#include "host_speech.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <sapi.h>
#else
#  include <cstdlib>
#  include <cerrno>
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace cpcse {

namespace {

const int RATE = 22050;
std::atomic<bool> testRenderer{false};
std::atomic<int> testMsPerCharacter{10};

// The S1V30120's nine voices (in its own order, which the Epson parser's :-)N uses) as
// the host can give them: a man's or a woman's voice, and how far from its usual pitch.
struct VoiceLook { bool female; int pitch; bool whisper; };
const VoiceLook LOOKS[9] = {
    { false, 0, false },   // 0 Perfect Paul
    { false, -5, false },  // 1 Huge Harry
    { true, 0, false },    // 2 Beautiful Betty
    { true, -3, false },   // 3 Uppity Ursula
    { false, 2, false },   // 4 Doctor Dennis
    { true, 8, false },    // 5 Kit the Kid
    { false, -2, false },  // 6 Frail Frank
    { true, -5, false },   // 7 Rough Rita
    { true, 2, true },     // 8 Whispering Wendy
};
const VoiceLook& look(int voice) { return LOOKS[voice < 0 || voice > 8 ? 0 : voice]; }

int clampi(long v, int lo, int hi) { return (int)std::max<long>(lo, std::min<long>(hi, v)); }

// Whispered: the voice's own loudness carried by noise, with a little of the voice left.
void whisperRange(std::vector<int16_t>& pcm, size_t from, size_t to) {
    uint32_t seed = 0x2545f491u;
    double env = 0, lp = 0;
    for (size_t i = from; i < to && i < pcm.size(); i++) {
        const double x = pcm[i] / 32768.0;
        env = env * 0.995 + std::fabs(x) * 0.005;
        seed = seed * 1664525u + 1013904223u;
        const double n = ((seed >> 9) / 4194304.0) - 1.0;   // -1..1
        lp = lp * 0.6 + n * 0.4;
        const double y = 0.25 * x + (n - lp) * env * 2.2;
        pcm[i] = (int16_t)clampi(std::lround(y * 32767.0), -32768, 32767);
    }
}

void applyGain(std::vector<int16_t>& pcm, size_t from, size_t to, double gain) {
    if (std::fabs(gain - 1.0) < 1e-9) return;
    for (size_t i = from; i < to && i < pcm.size(); i++) pcm[i] = (int16_t)clampi(std::lround(pcm[i] * gain), -32768, 32767);
}

#ifdef _WIN32
// DECtalk's ARPAbet, as SAPI's American English phone set spells it ("" = nothing).
std::string sapiPhone(const std::string& p) {
    static const char* same[] = { "aa", "ae", "ah", "ao", "aw", "ax", "ay", "b", "ch", "d", "dh", "eh", "er", "ey", "f", "g",
                                  "ih", "iy", "jh", "k", "l", "m", "n", "ow", "oy", "p", "r", "s", "sh", "t", "th", "uh",
                                  "uw", "v", "w", "y", "z", "zh" };
    for (const char* s : same) if (p == s) return p;
    if (p == "hx") return "h";
    if (p == "ix") return "ih";
    if (p == "el") return "ax l";
    if (p == "en") return "ax n";
    if (p == "nx") return "ng";
    if (p == "rr") return "er";
    if (p == "rx") return "r";
    if (p == "lx" || p == "ll") return "l";
    if (p == "dx") return "d";
    if (p == "tx") return "t";
    if (p == "yx") return "y";
    if (p == "yu") return "y uw";
    if (p == "ar") return "aa r";
    if (p == "or") return "ao r";
    if (p == "ir") return "ih r";
    if (p == "ur") return "uh r";
    return "";
}

// Words a minute as SAPI's -10..10 (0 is about 180; every 10 is three times as fast).
int sapiRate(int wpm) { return clampi(std::lround(10.0 * std::log(std::max(30, wpm) / 180.0) / std::log(3.0)), -10, 10); }

// A sung note (1-37, DECtalk's semitones) as SAPI's pitch -10..10.
int notePitch(int note) { return clampi(std::lround((note - 16) / 1.5), -10, 10); }

#endif

void renderTest(HostSpeech::Job& job) {
    const int ms = testMsPerCharacter;
    for (const SpeechSegment& s : job.segments) {
        const size_t units = s.text.size() + s.phones.size();
        const size_t n = units * (size_t)ms * RATE / 1000;
        for (size_t i = 0; i < n; i++) job.pcm.push_back((int16_t)(std::sin(6.283185307179586 * 1000.0 * i / RATE) * 8000 * s.gain));
    }
}

#ifndef _WIN32
// A RIFF WAVE file's 16-bit PCM, as mono at RATE.
bool readWav(const std::string& path, std::vector<int16_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) return false;
    int channels = 1, rate = RATE, bits = 16;
    size_t at = 12;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    auto u16 = [&](size_t o) { return (int)(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
    while (at + 8 <= d.size()) {
        const uint32_t size = u32(at + 4);
        if (std::memcmp(d.data() + at, "fmt ", 4) == 0 && at + 24 <= d.size()) { channels = u16(at + 10); rate = (int)u32(at + 12); bits = u16(at + 22); }
        if (std::memcmp(d.data() + at, "data", 4) == 0) { pcm = d.data() + at + 8; pcmBytes = std::min<size_t>(size, d.size() - at - 8); break; }
        at += 8 + size + (size & 1);
    }
    if (!pcm || bits != 16 || channels < 1 || rate < 1000) return false;
    const size_t frames = pcmBytes / (2 * (size_t)channels);
    std::vector<int16_t> mono(frames);
    for (size_t i = 0; i < frames; i++) mono[i] = (int16_t)(pcm[i * 2 * channels] | pcm[i * 2 * channels + 1] << 8);
    if (rate == RATE) { out.insert(out.end(), mono.begin(), mono.end()); return true; }
    const size_t n = (size_t)((double)frames * RATE / rate);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i * rate / RATE;
        const size_t k = (size_t)t;
        const double a = k < frames ? mono[k] : 0, b = k + 1 < frames ? mono[k + 1] : a;
        out.push_back((int16_t)std::lround(a + (b - a) * (t - k)));
    }
    return true;
}

#endif

#ifdef _WIN32
// SPDFID_WaveFormatEx (the Windows SDK's sapi.h); MinGW's libsapi does not carry it.
const GUID FMT_WAVEFORMATEX = { 0xC31ADBAE, 0x527F, 0x4FF5, { 0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C } };

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(0, n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string xmlEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else if (c < 32) o += ' ';
        else if (c >= 0x80) { o += (char)(0xc0 | c >> 6); o += (char)(0x80 | (c & 0x3f)); }   // Latin-1 to UTF-8
        else o += (char)c;
    }
    return o;
}

std::string sapiXml(const SpeechSegment& s) {
    const VoiceLook& v = look(s.voice);
    std::string x = std::string("<voice required=\"Language=") + (s.spanish ? "C0A" : "409") + "\" optional=\"Gender=" +
                    (v.female ? "Female" : "Male") + "\">";
    x += "<volume level=\"100\"/>";
    x += "<rate absspeed=\"" + std::to_string(sapiRate(s.wpm)) + "\"/>";
    const int pitch = clampi(v.pitch + 2 * s.pitch, -10, 10);
    if (!s.text.empty()) {
        x += "<pitch absmiddle=\"" + std::to_string(pitch) + "\">" + xmlEscape(s.text) + "</pitch>";
    }
    for (const SpeechPhone& p : s.phones) {
        if (p.arpabet == "_") { x += "<silence msec=\"" + std::to_string(p.ms ? p.ms : 150) + "\"/>"; continue; }
        const std::string sym = sapiPhone(p.arpabet);
        if (sym.empty()) continue;
        const int pp = p.note ? clampi(notePitch(p.note) + 2 * s.pitch, -10, 10) : pitch;
        const int rate = p.ms ? clampi(std::lround(10.0 * std::log(110.0 / std::max(20, p.ms)) / std::log(3.0)), -10, 10) : sapiRate(s.wpm);
        x += "<pitch absmiddle=\"" + std::to_string(pp) + "\"><rate absspeed=\"" + std::to_string(rate) + "\"><pron sym=\"" + sym + "\"/></rate></pitch>";
    }
    x += "</voice>";
    return x;
}

void renderHost(HostSpeech::Job& job) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISpVoice* voice = nullptr;
    ISpStream* stream = nullptr;
    IStream* memory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void**)&voice)) ||
        FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, (void**)&stream)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory))) {
        job.error = "Windows speech (SAPI) is not available";
    } else {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = 1; wfx.nSamplesPerSec = RATE;
        wfx.wBitsPerSample = 16; wfx.nBlockAlign = 2; wfx.nAvgBytesPerSec = RATE * 2;
        if (FAILED(stream->SetBaseStream(memory, FMT_WAVEFORMATEX, &wfx)) || FAILED(voice->SetOutput(stream, TRUE))) {
            job.error = "Windows speech could not render to memory";
        } else {
            std::vector<size_t> ends;
            for (const SpeechSegment& s : job.segments) {
                voice->Speak(widen(sapiXml(s)).c_str(), SPF_IS_XML, nullptr);
                STATSTG st{};
                memory->Stat(&st, STATFLAG_NONAME);
                ends.push_back((size_t)(st.cbSize.QuadPart / 2));
            }
            HGLOBAL h = nullptr;
            GetHGlobalFromStream(memory, &h);
            const size_t samples = ends.empty() ? 0 : ends.back();
            if (h && samples) {
                const int16_t* p = (const int16_t*)GlobalLock(h);
                if (p) job.pcm.assign(p, p + samples);
                GlobalUnlock(h);
            }
            size_t from = 0;
            for (size_t i = 0; i < job.segments.size() && i < ends.size(); i++) {
                const SpeechSegment& s = job.segments[i];
                if (s.whisper || look(s.voice).whisper) whisperRange(job.pcm, from, ends[i]);
                applyGain(job.pcm, from, ends[i], s.gain);
                from = ends[i];
            }
        }
    }
    if (memory) memory->Release();
    if (stream) stream->Release();
    if (voice) voice->Release();
    if (SUCCEEDED(init)) CoUninitialize();
}

#else   // macOS and Linux: a program renders each segment to a WAV file

bool onPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::string all = path ? path : "/usr/bin:/bin:/usr/local/bin";
    size_t at = 0;
    while (at <= all.size()) {
        size_t end = all.find(':', at);
        if (end == std::string::npos) end = all.size();
        const std::string full = all.substr(at, end - at) + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return true;
        at = end + 1;
    }
    return false;
}

std::string synthesiser() {
#if defined(__APPLE__)
    if (access("/usr/bin/say", X_OK) == 0) return "/usr/bin/say";
#endif
    if (onPath("espeak-ng")) return "espeak-ng";
    if (onPath("espeak")) return "espeak";
    return "";
}

bool run(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int r = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (r != 0) return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string tempPath(const char* suffix) {
    const char* dir = std::getenv("TMPDIR");
    std::string t = std::string(dir && *dir ? dir : "/tmp") + "/cpcse-speech-XXXXXX";
    std::vector<char> b(t.begin(), t.end());
    b.push_back('\0');
    const int fd = mkstemp(b.data());
    if (fd < 0) return "";
    close(fd);
    const std::string base(b.data());
    unlink(base.c_str());
    return base + suffix;
}

void renderHost(HostSpeech::Job& job) {
    const std::string program = synthesiser();
    if (program.empty()) { job.error = "no speech program (install espeak-ng)"; return; }
    for (const SpeechSegment& s : job.segments) {
        std::string text = s.text;
        if (text.empty()) for (const SpeechPhone& p : s.phones) if (p.arpabet != "_") text += p.arpabet;   // no phone input here
        if (text.empty()) continue;
        const std::string in = tempPath(".txt"), out = tempPath(".wav");
        if (in.empty() || out.empty()) continue;
        const VoiceLook& v = look(s.voice);
        { std::ofstream f(in, std::ios::binary); f << text; }
        std::vector<std::string> args;
        if (program == "/usr/bin/say") {
            // [[pbas]] sets the base pitch (about 46 is usual)
            std::ofstream f(in, std::ios::binary);
            f << "[[pbas " << clampi(46 + 2 * (v.pitch + 2 * s.pitch), 20, 80) << "]] " << text;
            f.close();
            args = { program, "-v", s.spanish ? (v.female ? "Monica" : "Jorge") : (v.female ? "Samantha" : "Fred"),
                     "-r", std::to_string(clampi(s.wpm, 80, 500)), "--file-format=WAVE", "--data-format=LEI16@22050",
                     "-o", out, "-f", in };
        } else {
            const std::string variant = v.female ? "+f" + std::to_string(1 + s.voice % 4) : "+m" + std::to_string(1 + s.voice % 7);
            args = { program, "-v", std::string(s.spanish ? "es" : "en") + variant, "-s", std::to_string(clampi(s.wpm, 80, 450)),
                     "-p", std::to_string(clampi(50 + 4 * (v.pitch + 2 * s.pitch), 0, 99)), "-w", out, "-f", in };
        }
        const size_t from = job.pcm.size();
        if (run(args)) readWav(out, job.pcm);
        if (s.whisper || v.whisper) whisperRange(job.pcm, from, job.pcm.size());
        applyGain(job.pcm, from, job.pcm.size(), s.gain);
        unlink(in.c_str());
        unlink(out.c_str());
    }
    if (job.pcm.empty() && job.error.empty()) job.error = program + " gave no sound";
}
#endif

} // namespace

std::string HostSpeech::unavailable() {
    if (testRenderer) return "";
#ifdef _WIN32
    return "";
#else
    return synthesiser().empty() ? "no speech program (install espeak-ng)" : "";
#endif
}

void HostSpeech::useTestRenderer(bool on, int msPerCharacter) {
    testRenderer = on;
    testMsPerCharacter = std::max(1, msPerCharacter);
}

std::shared_ptr<HostSpeech::Job> HostSpeech::render(std::vector<SpeechSegment> segments) {
    auto job = std::make_shared<Job>();
    job->segments = std::move(segments);
    job->rate = RATE;
    if (testRenderer) { renderTest(*job); job->done = true; return job; }
    std::thread([job] { renderHost(*job); job->done = true; }).detach();
    return job;
}

} // namespace cpcse
