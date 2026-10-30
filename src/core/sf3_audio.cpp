// CPCSyntaxError — the SYMBiFACE III's sound side (see sf3_audio.h for the sources).
#include "sf3_audio.h"
#include "host_midi.h"
#include "host_process.h"
#include "minimp3_ex.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace cpcse {

namespace fs = std::filesystem;

// The firmware this SF3 answers for: SF3_CPC_20250422.dfu, the newest in TMTLOGIC's update
// folder. AT+SEFIRMWARECHK gives a date as DDMMYYYY (SEPLAYER.BAS: "12092018").
static const char* FIRMWARE = "22042025";
static const int FIRMWARE_YMD = 20250422;

static std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
static fs::path u8(const std::string& s) { return fs::u8path(s); }

// ------------------------------------------------------------------ ffmpeg

// ffmpeg turning what minimp3 cannot into 44.1 kHz stereo 16-bit, read as it comes.
struct Sf3Audio::Ffmpeg {
    HostProcess proc;
    std::vector<unsigned char> buf = std::vector<unsigned char>(16384);
    size_t have = 0, at = 0;
    // input: a file, or "pipe:0" with `format` ("aac", "ogg") for a stream written to it
    bool start(const std::string& program, const std::string& input, const char* format) {
        std::vector<std::string> a = { program, "-hide_banner", "-nostats", "-v", "info" };
        if (format) a.insert(a.end(), { "-probesize", "4096", "-analyzeduration", "0", "-f", format });
        a.insert(a.end(), { "-i", input, "-vn", "-f", "s16le", "-ac", "2", "-ar", "44100", "pipe:1" });
        return proc.start(a, input == "pipe:0");
    }
    size_t buffered() const { return proc.available() + (have - at); }
    // 1 a frame, 0 none yet, -1 the end
    int frame(float& l, float& r) {
        if (at + 4 > have) {
            const size_t rest = have - at;
            std::memmove(buf.data(), buf.data() + at, rest);
            have = rest; at = 0;
            have += proc.read(buf.data() + have, buf.size() - have);
            if (have < 4) return proc.finished() ? -1 : 0;
        }
        l = (int16_t)(buf[at] | buf[at + 1] << 8) / 32768.0f;
        r = (int16_t)(buf[at + 2] | buf[at + 3] << 8) / 32768.0f;
        at += 4;
        return 1;
    }
    // "Duration: 00:03:25.12" from ffmpeg's report, in seconds; 0 while not known
    double seconds() const {
        const std::string e = proc.errorText();
        const size_t d = e.find("Duration: ");
        int h = 0, m = 0; double s = 0;
        if (d == std::string::npos || std::sscanf(e.c_str() + d + 10, "%d:%d:%lf", &h, &m, &s) != 3) return 0;
        return h * 3600.0 + m * 60.0 + s;
    }
};

// ------------------------------------------------------------------ a file or a sample

struct Sf3Audio::Source {
    std::vector<unsigned char> bytes;
    std::unique_ptr<Ffmpeg> ff;
    size_t fileSize = 0;
    bool mp3 = false;
    mp3dec_ex_t dec{};
    bool decOpen = false;
    std::vector<int16_t> pcm;          // WAV: the whole file; MP3: a block from the decoder
    size_t have = 0, at = 0;
    int channels = 2, hz = 44100;
    uint64_t total = 0, done = 0;      // samples (all channels)
    ~Source() { if (decOpen) mp3dec_ex_close(&dec); }

    // RIFF WAVE: PCM, 8 or 16 bits, mono or stereo.
    bool openWav() {
        auto u32 = [&](size_t i) { return (unsigned)bytes[i] | (unsigned)bytes[i + 1] << 8 | (unsigned)bytes[i + 2] << 16 | (unsigned)bytes[i + 3] << 24; };
        auto u16 = [&](size_t i) { return (unsigned)bytes[i] | (unsigned)bytes[i + 1] << 8; };
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;
        int bits = 0, format = 0;
        for (size_t i = 12; i + 8 <= bytes.size();) {
            const size_t len = u32(i + 4), body = i + 8;
            if (len > bytes.size() - body) return false;
            if (std::memcmp(&bytes[i], "fmt ", 4) == 0 && len >= 16) {
                format = (int)u16(body); channels = (int)u16(body + 2); hz = (int)u32(body + 4); bits = (int)u16(body + 14);
            } else if (std::memcmp(&bytes[i], "data", 4) == 0) {
                if (format != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2 || hz < 1000 || hz > 192000) return false;
                const size_t n = len / (size_t)(bits / 8);
                pcm.resize(n);
                for (size_t k = 0; k < n; k++)
                    pcm[k] = bits == 16 ? (int16_t)u16(body + k * 2) : (int16_t)(((int)bytes[body + k] - 128) * 256);
                have = n; total = n;
                return true;
            }
            i = body + len + (len & 1);
        }
        return false;
    }
    bool open(const std::string& file, const std::string& ffmpeg, std::string& why) {
        std::error_code ec;
        fileSize = (size_t)fs::file_size(u8(file), ec);
        if (ec) { why = "FILE NOT FOUND"; return false; }
        std::string ext = upper(u8(file).extension().string());
        static const char* viaFfmpeg[] = { ".OGG", ".AAC", ".M4A", ".MP4", ".WMA", ".FLAC", ".OPUS" };
        if (std::find(std::begin(viaFfmpeg), std::end(viaFfmpeg), ext) != std::end(viaFfmpeg)) {
            if (ffmpeg.empty()) { why = "FORMAT NOT SUPPORTED"; return false; }
            ff = std::make_unique<Ffmpeg>();
            if (!ff->start(ffmpeg, file, nullptr)) { why = "FORMAT NOT SUPPORTED"; return false; }
            hz = 44100; channels = 2;
            return true;
        }
        std::ifstream f(u8(file), std::ios::binary);
        if (!f) { why = "FILE NOT FOUND"; return false; }
        bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (ext == ".MP3") {
            if (mp3dec_ex_open_buf(&dec, bytes.data(), bytes.size(), MP3D_SEEK_TO_SAMPLE) != 0 || dec.info.hz <= 0) { why = "FILE ERROR"; return false; }
            decOpen = true; mp3 = true;
            channels = std::max(1, dec.info.channels); hz = dec.info.hz; total = dec.samples;
            pcm.resize(4096 * 2);
            return true;
        }
        if (ext == ".WAV") { if (openWav()) return true; why = "FILE ERROR"; return false; }
        why = "FORMAT NOT SUPPORTED";
        return false;
    }
    bool next(float& l, float& r) {
        if (ff) {
            const int k = ff->frame(l, r);
            if (k < 0) return false;
            if (k == 0) l = r = 0.0f;                  // ffmpeg is behind: a gap, still playing
            else done += 2;
            return true;
        }
        if (mp3 && at + (size_t)channels > have) {
            have = mp3dec_ex_read(&dec, pcm.data(), pcm.size() - pcm.size() % (size_t)channels);
            at = 0;
        }
        if (at + (size_t)channels > have) return false;
        l = pcm[at] / 32768.0f;
        r = channels > 1 ? pcm[at + 1] / 32768.0f : l;
        at += (size_t)channels; done += (uint64_t)channels;
        return true;
    }
    int percent() {
        if (ff && !total) total = (uint64_t)(ff->seconds() * 44100.0 * 2.0);
        return total ? (int)std::min<uint64_t>(100, done * 100 / total) : 0;
    }
};

// ------------------------------------------------------------------ the MP3A stream

struct Sf3Audio::StreamDecoder {
    mp3dec_t dec{};
    std::vector<int16_t> pcm = std::vector<int16_t>(MINIMP3_MAX_SAMPLES_PER_FRAME);
    int samples = 0, at = 0, channels = 2;
    StreamDecoder() { mp3dec_init(&dec); }
    void restart() { mp3dec_init(&dec); samples = at = 0; }
};

Sf3Audio::Sf3Audio() : streamDec(std::make_unique<StreamDecoder>()) { reset(); }
Sf3Audio::~Sf3Audio() = default;

void Sf3Audio::reset() {
    source.reset();
    flushStream();
    mode = Mode::Mp3a; route = 0; masterVolume = 100;
    modeReg = 0; decoderRegs.clear(); spectrumOut.clear(); filling = true;
    i2cStatus = 0xf8; i2cData = 0; i2cIndex = -1; i2cPointer = 0; i2cAddressed = false;
    tda = { 0x3c, 0x3c, 0x36, 0x36 }; tdaSwitch = 0xee;
    atLine.clear(); atOut.clear(); listOut.clear(); fault.clear();
    drive = 1; path.clear(); samples = {};
    regCommand = regData = regStatus = 0;
    dspVol[0] = dspVol[1] = 100; dspBass = dspTreble = 50;
    playingName.clear(); paused = false;
    recent.assign(1024, 0.0f); recentAt = 0;
    peak[0] = peak[1] = 0;
    beepOn = false;
}

bool Sf3Audio::playing() const { return source != nullptr; }

void Sf3Audio::flushStream() {
    stream.clear(); streamAt = 0; streamDec->restart();
    streamFf.reset();
    streamKind = 0;
    filling = true;
}

void Sf3Audio::setRoute(int r) { route = r; flushStream(); }

size_t Sf3Audio::streamRoom() const {
    if (streamKind == 2) return streamFf && streamFf->buffered() < (2u << 20) ? 65536 : 0;
    const size_t p = streamPending();
    return p < 262144 ? 262144 - p : 0;
}

void Sf3Audio::feedStream(const unsigned char* data, size_t n) {
    if (streamKind == 2) { streamFf->proc.write(data, n); return; }
    if (streamKind == 3) return;                       // nothing here plays it
    if (streamAt > 65536) { stream.erase(stream.begin(), stream.begin() + (long)streamAt); streamAt = 0; }
    n = std::min(n, streamRoom());
    stream.insert(stream.end(), data, data + n);
    if (route == 5 && streamKind == 0) tellStream();
}

// The web radio's format, from its bytes (the HTTP/ICY answer comes first, as the SF3 copies
// the socket as it is). AAC in ADTS: 12 bits of sync, layer 00, the frame's length in bits
// 30-42 (ISO 14496-3 1.A.2), and the next header just there. Ogg: "OggS".
void Sf3Audio::tellStream() {
    const unsigned char* b = stream.data() + streamAt;
    const size_t n = streamPending();
    const char* format = nullptr;
    size_t from = 0;
    for (size_t i = 0; i + 7 <= n && !format; i++) {
        if (b[i] == 0xff && (b[i + 1] & 0xf6) == 0xf0) {
            const size_t len = (size_t)(b[i + 3] & 3) << 11 | (size_t)b[i + 4] << 3 | b[i + 5] >> 5;
            if (len > 7 && i + len + 2 <= n && b[i + len] == 0xff && (b[i + len + 1] & 0xf6) == 0xf0) { format = "aac"; from = i; }
        } else if (b[i] == 'O' && b[i + 1] == 'g' && b[i + 2] == 'g' && b[i + 3] == 'S') { format = "ogg"; from = i; }
    }
    if (!format) { if (n >= 16384) streamKind = 1; return; }   // none in 16 KB: MP3
    if (ffmpeg.empty()) { streamKind = 3; fault = "FORMAT NOT SUPPORTED"; stream.clear(); streamAt = 0; return; }
    streamFf = std::make_unique<Ffmpeg>();
    if (!streamFf->start(ffmpeg, "pipe:0", format)) { streamFf.reset(); streamKind = 3; fault = "FORMAT NOT SUPPORTED"; return; }
    streamFf->proc.write(b + from, n - from);
    stream.clear(); streamAt = 0;
    streamKind = 2;
}

// ------------------------------------------------------------------ ports

int Sf3Audio::readPort(int port) {
    port &= 0xffff;
    if (isMidiPort(port)) return port == 0xf8f2 ? 0x02 : 0x00;   // ready to send, nothing came in
    switch (port) {
        case 0xff20: { if (atOut.empty()) return 0; const int v = atOut.front(); atOut.pop_front(); return v; }
        case 0xff22:
            if (!spectrumOut.empty()) { const int v = spectrumOut.front(); spectrumOut.pop_front(); return v; }
            return 0;
        case 0xff23: {
            int v = playing() && !paused ? 0x20 : 0x08;
            if (mode == Mode::Mp3a && streamPending() + 2048 <= 8192) v |= 0x80;   // room for SymAmp's 2048 bytes
            return v;                                          // bit 4 (busy) never: nothing here takes time
        }
        case 0xff24: return mode == Mode::Mp3a ? i2cStatus : 0;
        case 0xff25:
            if (mode == Mode::Mp3a) return i2cData;
            if (listOut.empty()) return 0;
            { const int v = listOut.front(); listOut.pop_front(); return v; }
        case 0xff26: return mode == Mode::Reg ? regStatus : vu(0);
        case 0xff27: return mode == Mode::Mp3a ? 0x08 : mode == Mode::Reg ? regData : vu(1);   // I2C: done
        default: return 0xff;
    }
}

static int tdaVolumeDb(int code) { code &= 0x3f; return code >= 28 ? 2 * code - 120 : -80; }   // Table 6

void Sf3Audio::writePort(int port, int value) {
    port &= 0xffff; value &= 0xff;
    if (isMidiPort(port)) { if (port == 0xf8f3) HostMidi::send(value); return; }
    switch (port) {
        case 0xff20:
            if (value == 13) { const std::string line = atLine; atLine.clear(); command(line); }
            else if (value == 10) {}
            else {
                if (atLine.empty()) atOut.clear();            // a new command: the old answer goes
                if (atLine.size() < 256) atLine.push_back((char)value);
            }
            return;
        case 0xff21: if (mode == Mode::Mp3a) flushStream(); return;
        case 0xff22:
            if (mode == Mode::Synth) { HostMidi::send(value); return; }
            if (mode != Mode::Mp3a) return;
            if (modeReg == 0x40) { if (decoderRegs.size() < 28) decoderRegs.push_back(value); return; }
            if (streamAt > 65536) { stream.erase(stream.begin(), stream.begin() + (long)streamAt); streamAt = 0; }
            if (stream.size() - streamAt < (1u << 20)) stream.push_back((unsigned char)value);
            return;
        case 0xff23:
            if (mode != Mode::Mp3a) return;
            if (value == 0x40) decoderRegs.clear();
            else if (value == 0xc0) flushStream();     // registers written: a fresh decoder
            else if (value == 0x1f) spectrum();
            modeReg = value;
            return;
        case 0xff25: i2cData = value; return;
        case 0xff26:
            if (mode != Mode::Reg) return;
            regCommand = value;
            if (value == 0x80 || value == 0x7e) {
                std::string file;
                if (value == 0x80) file = regData >= 0 && regData < 10 ? samples[(size_t)regData] : "";
                else {
                    // SP0256 allophone n: SETALK/SPnnnH.MP3, on the stick or the SD card (SE-TALK.BAS)
                    char name[32]; std::snprintf(name, sizeof name, "SP%03XH.MP3", regData & 0xff);
                    for (const std::string& root : { usbFolder, sdFolder }) {
                        if (root.empty()) continue;
                        std::error_code ec;
                        const fs::path dir = u8(root) / "SETALK";
                        if (!fs::is_directory(dir, ec)) continue;
                        for (const auto& e : fs::directory_iterator(dir, ec))
                            if (upper(e.path().filename().string()) == name) { file = e.path().u8string(); break; }
                        if (!file.empty()) break;
                    }
                }
                regStatus = !file.empty() && startFile(file) ? 0 : 2;
                if (regStatus == 2 && fault.empty()) fault = "SAMPLE NOT DEFINED";
            }
            return;
        case 0xff27:
            if (mode == Mode::Reg) { regData = value; return; }
            if (mode != Mode::Mp3a) return;
            // The TWI of the ATmega: &64 start, &44 send &FF25, &54 stop.
            if (value & 0x20) { i2cStatus = i2cIndex >= 0 ? 0x10 : 0x08; i2cIndex = 0; }
            else if (value & 0x10) { i2cIndex = -1; i2cAddressed = false; i2cStatus = 0xf8; }
            else if (i2cIndex == 0) { i2cAddressed = i2cData == 0x82; i2cStatus = i2cAddressed ? 0x18 : 0x20; i2cIndex = 1; }
            else if (i2cIndex > 0 && i2cAddressed) {
                if (i2cIndex == 1) i2cPointer = i2cData;
                else {
                    if (i2cPointer >= 0 && i2cPointer <= 3) tda[(size_t)i2cPointer] = i2cData;
                    else if (i2cPointer == 8) tdaSwitch = i2cData;
                    i2cPointer++;                              // "automatic increment" (datasheet p.6)
                }
                i2cIndex++;
                i2cStatus = 0x28;
            } else if (i2cIndex > 0) i2cStatus = 0x30;          // data not acknowledged
            return;
        default: return;
    }
}

// ------------------------------------------------------------------ AT commands

void Sf3Audio::answer(const std::string& text) {
    atOut.clear();
    for (char c : text) atOut.push_back((unsigned char)c);
    atOut.push_back(13); atOut.push_back(10);
}
void Sf3Audio::error(const std::string& why) { fault = why; answer("ERROR"); }

std::string Sf3Audio::folderNow() const {
    const std::string& root = drive == 0 ? sdFolder : usbFolder;
    if (root.empty()) return "";
    fs::path p = u8(root);
    for (const std::string& c : path) p /= u8(c);
    return p.u8string();
}

// A name in the folder now, as the card's FAT would match it: case does not count. A name with
// "/" or "\" goes down into folders; ".." never leaves the folder.
std::string Sf3Audio::findFile(const std::string& name) const {
    std::string dir = folderNow();
    if (dir.empty()) return "";
    std::vector<std::string> parts;
    std::string part;
    for (char c : name) { if (c == '/' || c == '\\') { if (!part.empty()) parts.push_back(part); part.clear(); } else part.push_back(c); }
    if (!part.empty()) parts.push_back(part);
    for (size_t i = 0; i < parts.size(); i++) {
        if (parts[i] == "." || parts[i] == "..") return "";
        std::error_code ec;
        std::string found;
        for (const auto& e : fs::directory_iterator(u8(dir), ec))
            if (upper(e.path().filename().u8string()) == upper(parts[i])) { found = e.path().u8string(); break; }
        if (found.empty()) return "";
        dir = found;
    }
    std::error_code ec;
    return fs::is_regular_file(u8(dir), ec) ? dir : "";
}

bool Sf3Audio::startFile(const std::string& file) {
    auto s = std::make_unique<Source>();
    std::string why;
    if (!s->open(file, ffmpeg, why)) { fault = why; return false; }
    sourceHz = s->hz;
    source = std::move(s);
    playingName = u8(file).filename().u8string();
    paused = false;
    fault.clear();
    return true;
}
void Sf3Audio::stopFile() { source.reset(); playingName.clear(); paused = false; }

void Sf3Audio::command(const std::string& lineIn) {
    std::string line = lineIn;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
    const size_t eq = line.find('=');
    const bool query = eq == std::string::npos && !line.empty() && line.back() == '?';
    std::string name = upper(eq != std::string::npos ? line.substr(0, eq) : query ? line.substr(0, line.size() - 1) : line);
    const std::string arg = eq != std::string::npos ? line.substr(eq + 1) : "";
    if (name == "AT") { answer("AT"); return; }
    if (name.rfind("AT+", 0) != 0) { error("UNKNOWN COMMAND"); return; }
    const std::string cmd = name.substr(3);
    auto ok = [&]() { answer(name + (eq != std::string::npos ? "=" + arg : "")); };
    auto value = [&](const std::string& v) { answer(name + "=" + v); };
    auto number = [&](int lo, int hi, int& out) {
        char* end = nullptr;
        const long v = std::strtol(arg.c_str(), &end, 10);
        if (arg.empty() || (end && *end && *end != ' ')) return false;
        out = (int)std::clamp<long>(v, lo, hi);
        return true;
    };

    if (cmd == "SEFIRMWARE" && query) { value(FIRMWARE); return; }
    if (cmd == "SEFIRMWARECHK") {
        // DDMMYYYY: "true" when this firmware is that one or newer
        const int d = arg.size() == 8 ? std::atoi(arg.substr(0, 2).c_str()) : 0, m = arg.size() == 8 ? std::atoi(arg.substr(2, 2).c_str()) : 0;
        const int y = arg.size() == 8 ? std::atoi(arg.substr(4, 4).c_str()) : 0;
        value(y * 10000 + m * 100 + d <= FIRMWARE_YMD ? "TRUE" : "FALSE");
        return;
    }
    if (cmd == "SEMODE") {
        if (query) { static const char* names[] = { "MP3A", "MP3B", "REG", "SYNTH", "FM" }; value(names[(int)mode]); return; }
        const std::string m = upper(arg);
        if (m == "MP3A") mode = Mode::Mp3a;
        else if (m == "MP3B") mode = Mode::Mp3b;
        else if (m == "REG") mode = Mode::Reg;
        else if (m == "SYNTH") mode = Mode::Synth;
        else if (m == "FM") mode = Mode::Fm;
        else { error("UNKNOWN MODE"); return; }
        stopFile();
        listOut.clear();
        ok();
        return;
    }
    if (cmd == "SEFAULTTXT" && query) { value(fault.empty() ? "NO ERROR" : fault); return; }
    if (cmd == "USBINIT") { if (usbFolder.empty()) { error("NO USB DEVICE"); return; } ok(); return; }
    if (cmd == "MSDPATH") {
        if (query) {
            std::string p = std::to_string(drive) + ":";
            for (const std::string& c : path) p += "/" + c;
            value(p);
            return;
        }
        // "1:" the stick's root, "1:/MUSIC", "0:\SAMPLE"
        if (arg.size() < 2 || (arg[0] != '0' && arg[0] != '1') || arg[1] != ':') { error("PATH NOT FOUND"); return; }
        const int newDrive = arg[0] - '0';
        std::vector<std::string> newPath;
        std::string part;
        auto push = [&]() {
            if (part == "..") { if (!newPath.empty()) newPath.pop_back(); }
            else if (!part.empty() && part != ".") newPath.push_back(part);
            part.clear();
        };
        for (size_t i = 2; i < arg.size(); i++) { if (arg[i] == '/' || arg[i] == '\\') push(); else part.push_back(arg[i]); }
        push();
        const int oldDrive = drive; const std::vector<std::string> oldPath = path;
        drive = newDrive; path.clear();
        // each part found as the FAT would find it (case does not count)
        for (const std::string& c : newPath) {
            std::error_code ec;
            std::string found;
            const std::string here = folderNow();
            if (!here.empty())
                for (const auto& e : fs::directory_iterator(u8(here), ec))
                    if (e.is_directory(ec) && upper(e.path().filename().u8string()) == upper(c)) { found = e.path().filename().u8string(); break; }
            if (found.empty()) { drive = oldDrive; path = oldPath; error("PATH NOT FOUND"); return; }
            path.push_back(found);
        }
        std::error_code ec;
        if (folderNow().empty() || !fs::is_directory(u8(folderNow()), ec)) { drive = oldDrive; path = oldPath; error("PATH NOT FOUND"); return; }
        ok();
        return;
    }
    if (cmd == "MSDFILES") {
        const std::string dir = folderNow();
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(u8(dir), ec)) { error("NO USB DEVICE"); return; }
        std::vector<std::string> dirs, files;
        for (const auto& e : fs::directory_iterator(u8(dir), ec)) {
            const std::string n = e.path().filename().u8string();
            if (e.is_directory(ec)) dirs.push_back("/" + n); else if (e.is_regular_file(ec)) files.push_back(n);
        }
        auto byName = [](const std::string& a, const std::string& b) { return upper(a) < upper(b); };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        if (!path.empty()) { dirs.insert(dirs.begin(), "/.."); dirs.insert(dirs.begin(), "/."); }   // FAT keeps both in a folder
        listOut.clear();
        bool first = true;
        for (const auto* list : { &dirs, &files })
            for (const std::string& n : *list) {
                if (!first) listOut.push_back(9);
                first = false;
                for (char c : n) if (c) listOut.push_back((unsigned char)c);
            }
        listOut.push_back(0);
        ok();
        return;
    }
    if (cmd == "MSDPLAY") {
        const std::string file = findFile(arg);
        if (file.empty()) { error("FILE NOT FOUND"); return; }
        if (!startFile(file)) { error(fault); return; }
        ok();
        return;
    }
    if (cmd == "MSDPAUSE") { if (source) paused = true; ok(); return; }
    if (cmd == "MSDSTART") { if (source) paused = false; ok(); return; }
    if (cmd == "MSDSTOP") { stopFile(); ok(); return; }
    if (cmd == "MSDSIZE" && query) { value(std::to_string(source ? source->fileSize : 0)); return; }
    if (cmd == "MSDCNT" && query) { value(std::to_string(source ? source->percent() : 0)); return; }
    if (cmd == "MSDSETSAMPLE") {
        // n@NAME: sample n (0..9) is NAME in the folder now
        const size_t at = arg.find('@');
        if (at != 1 || !std::isdigit((unsigned char)arg[0])) { error("SAMPLE NUMBER"); return; }
        const std::string file = findFile(arg.substr(2));
        if (file.empty()) { error("FILE NOT FOUND"); return; }
        samples[(size_t)(arg[0] - '0')] = file;
        ok();
        return;
    }
    int* dsp = cmd == "DSPVOLL" ? &dspVol[0] : cmd == "DSPVOLR" ? &dspVol[1] : cmd == "DSPBASS" ? &dspBass : cmd == "DSPTREBLE" ? &dspTreble : nullptr;
    if (dsp) {
        if (query) { value(std::to_string(*dsp)); return; }
        if (!number(0, 100, *dsp)) { error("VALUE"); return; }
        ok();
        return;
    }
    // The FM tuner: kept and answered, but there is no radio to hear.
    if (cmd == "FMBAND") { ok(); return; }
    if (cmd == "FMFREQ") {
        if (query) { char b[16]; std::snprintf(b, sizeof b, "%.1f", fmFreq); value(b); return; }
        fmFreq = std::atof(arg.c_str());
        ok();
        return;
    }
    if (cmd == "FMSCANUP" || cmd == "FMSCANDOWN") { ok(); return; }
    error("UNKNOWN COMMAND");
}

// ------------------------------------------------------------------ sound

bool Sf3Audio::nextFrame(float& l, float& r) {
    if (source) {
        if (paused) return false;
        if (source->next(l, r)) return true;
        stopFile();                                            // the end: &FF23 bit 3 again
        return false;
    }
    if (mode != Mode::Mp3a && route != 5) return false;
    StreamDecoder& d = *streamDec;
    if (route == 5 && streamKind != 1) {
        if (streamKind != 2) return false;             // not told yet, or not playable
        // ffmpeg's sound: half a second gathered first, and again after a gap
        if (filling && streamFf->buffered() < 44100 * 2) return false;
        filling = false;
        const int k = streamFf->frame(l, r);
        if (k == 1) { sourceHz = 44100; return true; }
        filling = true;
        return false;
    }
    if (route == 5) {
        const bool empty = d.at >= d.samples * d.channels;
        if (empty && filling && streamPending() < 16384) return false;
        filling = false;
        if (empty && streamPending() < 2048) { filling = true; return false; }   // ran dry: fill again
    }
    for (int tries = 0; d.at >= d.samples * d.channels && tries < 8; tries++) {
        const size_t pending = stream.size() - streamAt;
        if (pending == 0) return false;
        mp3dec_frame_info_t info{};
        d.samples = mp3dec_decode_frame(&d.dec, stream.data() + streamAt, (int)std::min<size_t>(pending, 1u << 20), d.pcm.data(), &info);
        d.at = 0;
        if (info.frame_bytes == 0) { d.samples = 0; return false; }   // not a whole frame yet
        streamAt += (size_t)info.frame_bytes;
        if (d.samples > 0) { d.channels = std::max(1, info.channels); sourceHz = info.hz > 0 ? info.hz : sourceHz; }
    }
    if (d.at >= d.samples * d.channels) return false;
    l = d.pcm[(size_t)d.at] / 32768.0f;
    r = d.channels > 1 ? d.pcm[(size_t)d.at + 1] / 32768.0f : l;
    d.at += d.channels;
    return true;
}

// First-order shelves for bass and treble. The TDA8425 sets its corners with the board's
// capacitors, which no document here gives: 300 Hz and 3 kHz are this emulator's own choice.
static float shelf(float x, float& state, float coeff, float gain, bool low) {
    state += coeff * (x - state);
    return low ? x + (gain - 1.0f) * state : x + (gain - 1.0f) * (x - state);
}

void Sf3Audio::advanceMicrosecond() {
    sourcePhase += (double)sourceHz / 1e6;
    while (sourcePhase >= 1.0) {
        sourcePhase -= 1.0;
        prevL = curL; prevR = curR;
        if (!nextFrame(curL, curR)) curL = curR = 0.0f;
        recent[recentAt] = (curL + curR) * 0.5f;
        recentAt = (recentAt + 1) % recent.size();
        peak[0] = std::max(std::fabs(curL), peak[0] * 0.9995f);
        peak[1] = std::max(std::fabs(curR), peak[1] * 0.9995f);
    }
    // The gains: in MP3A the TDA8425 as SymAmp set it (datasheet Tables 4-8); in the other
    // modes and in web radio (which sets no TDA8425: WEB-DK.BAS) the firmware's own settings,
    // AT+DSPVOLL/R, DSPBASS, DSPTREBLE (0..100, 50 = flat for bass and treble), put on the
    // same chip's steps.
    float gl, gr, bassDb, trebleDb;
    bool mono = false;
    if (mode == Mode::Mp3a && route != 5) {
        const bool mute = (tdaSwitch & 0x20) != 0;
        gl = mute ? 0.0f : std::pow(10.0f, tdaVolumeDb(tda[0]) / 20.0f);
        gr = mute ? 0.0f : std::pow(10.0f, tdaVolumeDb(tda[1]) / 20.0f);
        bassDb = (float)std::clamp(3 * ((tda[2] & 15) - 6), -12, 15);
        trebleDb = (float)std::clamp(3 * ((tda[3] & 15) - 6), -12, 12);
        mono = (tdaSwitch & 0x18) == 0;                        // STL = EFL = 0: forced mono
    } else {
        auto vol = [](int v) { return v <= 0 ? 0.0f : std::pow(10.0f, tdaVolumeDb(28 + v * 32 / 100) / 20.0f); };
        gl = vol(dspVol[0]); gr = vol(dspVol[1]);
        bassDb = (float)(3 * ((2 + dspBass * 8 / 100) - 6));
        trebleDb = (float)(3 * ((2 + dspTreble * 8 / 100) - 6));
    }
    const float master = masterVolume <= 0 ? 0.0f : std::pow(masterVolume / 100.0f, 2.0f);
    const float bassGain = std::pow(10.0f, bassDb / 20.0f), trebleGain = std::pow(10.0f, trebleDb / 20.0f);
    const float bassK = 1.0f - std::exp(-2.0f * 3.14159265f * 300.0f / (float)outputRate);
    const float trebleK = 1.0f - std::exp(-2.0f * 3.14159265f * 3000.0f / (float)outputRate);
    outPhase += outputRate / 1e6;
    while (outPhase >= 1.0) {
        outPhase -= 1.0;
        const float t = (float)sourcePhase;
        float l = prevL + (curL - prevL) * t, r = prevR + (curR - prevR) * t;
        if (mono) l = r = (l + r) * 0.5f;
        l = shelf(shelf(l, bassState[0], bassK, bassGain, true), trebleState[0], trebleK, trebleGain, false);
        r = shelf(shelf(r, bassState[1], bassK, bassGain, true), trebleState[1], trebleK, trebleGain, false);
        float b = 0.0f;
        if (beepOn) {
            beepPhase += beepHz / outputRate;
            if (beepPhase >= 1.0) beepPhase -= 1.0;
            b = (beepPhase < 0.5 ? 0.25f : -0.25f) * beepLevel;
        }
        left.push_back(l * gl * master + b);
        right.push_back(r * gr * master + b);
    }
    if (readIndex > 4096) {
        left.erase(left.begin(), left.begin() + (long)readIndex);
        right.erase(right.begin(), right.begin() + (long)readIndex);
        readIndex = 0;
    }
    const size_t cap = (size_t)(outputRate / 4);
    if (left.size() - readIndex > cap) readIndex = left.size() - cap;
}

void Sf3Audio::readSample(float& l, float& r) {
    if (readIndex < left.size()) { l = left[readIndex]; r = right[readIndex]; readIndex += 1; }
    else { l = r = 0.0f; }
}

// The SF3's buzzer (function 80, |BUZZ,a,b with a, b = 0..3; 0,0 = off). Its pitch is in no
// document here: 1, 2 or 3 kHz for b, a/3 of a quarter of full scale, are this emulator's choice.
void Sf3Audio::setBeep(int a, int b) {
    beepOn = a > 0 && b > 0;
    beepLevel = (float)std::clamp(a, 0, 3) / 3.0f;
    beepHz = 1000.0 * std::clamp(b, 1, 3);
}

// The VU levels 0..8 (SEPLAYER.BAS draws VU$(0..8)): 4 dB a step under full scale, this
// emulator's choice.
int Sf3Audio::vu(int side) const {
    const float p = peak[side & 1];
    if (p <= 1e-6f) return 0;
    return std::clamp(8 + (int)std::floor(20.0f * std::log10(p) / 4.0f), 0, 8);
}

// Function 31 on &FF23: 31 bytes on &FF22. SymAmp shows bytes 11, 13 ... 29 as its 10 bars,
// (byte & 31) / 2 - 3. Here the 31 bytes are the 31 third-octave bands from 20 Hz to 20 kHz
// (ISO 266; bytes 11 ... 29 are 250 Hz ... 16 kHz), 2 dB a step, 31 = full scale: the
// layout and the scale are this emulator's choice, as no document gives the card's own.
void Sf3Audio::spectrum() {
    static const double centre[31] = { 20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                       1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000 };
    const size_t n = recent.size();
    std::vector<std::complex<double>> x(n);
    for (size_t i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * (double)i / (double)(n - 1));
        x[i] = recent[(recentAt + i) % n] * w;
    }
    // radix-2 FFT, in place
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> wl = std::polar(1.0, -2.0 * 3.14159265358979 / (double)len);
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (size_t k = 0; k < len / 2; k++, w *= wl) {
                const std::complex<double> u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v; x[i + k + len / 2] = u - v;
            }
        }
    }
    const double binHz = (double)sourceHz / (double)n, full = (double)n / 4.0;   // a full-scale sine's Hann peak
    spectrumOut.clear();
    for (double fc : centre) {
        const double lo = fc / std::pow(2.0, 1.0 / 6.0), hi = fc * std::pow(2.0, 1.0 / 6.0);
        double e = 0.0;
        size_t a = (size_t)std::ceil(lo / binHz), b = (size_t)std::floor(hi / binHz);
        if (a > b) a = b = (size_t)std::lround(fc / binHz);   // a band narrower than a bin: the nearest bin
        for (size_t k = a; k <= b && k < n / 2; k++) e += std::norm(x[k]);
        const double db = e > 0.0 ? 10.0 * std::log10(e / (full * full)) : -999.0;
        spectrumOut.push_back(std::clamp((int)std::lround(31.0 + db / 2.0), 0, 31));
    }
}

} // namespace cpcse
