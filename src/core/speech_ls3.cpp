// CPCSyntaxError — LambdaSpeak 3's own modes: the board's control bytes, its Epson and
// DECtalk speech (with the host's voices), its getters and its clock. See speech.h.
#include "speech.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>

namespace cpcse {

namespace {

const int VERSION = 54;              // the firmware this follows
const size_t FLUSH_AT = 253;         // SPEECH_BUFFER_FLUSH_AT

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::tm hostTime() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return tmv;
}

// DECtalk's ARPAbet codes: two letters first.
const char* TWO[] = { "aa", "ae", "ah", "ao", "aw", "ax", "ay", "ch", "dh", "dx", "eh", "el", "en", "er", "ey", "hx", "ih",
                      "ix", "iy", "jh", "lx", "ll", "nx", "ow", "oy", "rr", "rx", "sh", "th", "tx", "uh", "uw", "yu", "yx",
                      "zh", "ar", "or", "ir", "ur" };

void parsePhones(const std::string& s, std::vector<SpeechPhone>& out) {
    size_t i = 0;
    while (i < s.size()) {
        const char c = (char)std::tolower((unsigned char)s[i]);
        if (c == '_') {
            SpeechPhone p; p.arpabet = "_"; out.push_back(p); i++;
        } else if (std::isalpha((unsigned char)c)) {
            SpeechPhone p;
            const std::string two = lower(s.substr(i, 2));
            bool matched = false;
            for (const char* t : TWO) if (two == t) { p.arpabet = two; i += 2; matched = true; break; }
            if (!matched) { p.arpabet = std::string(1, c); i += 1; }
            out.push_back(p);
        } else if (c == '<') {
            // <duration[,note]> belongs to the phone before it
            const size_t end = s.find('>', i);
            const std::string inside = s.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
            i = end == std::string::npos ? s.size() : end + 1;
            if (!out.empty()) {
                int ms = 0, note = 0;
                if (std::sscanf(inside.c_str(), "%d,%d", &ms, &note) >= 1) { out.back().ms = std::max(0, std::min(ms, 5000)); out.back().note = std::max(0, std::min(note, 37)); }
            }
        } else {
            i++;                                    // stress marks, spaces, punctuation
        }
    }
}

bool hasWords(const std::string& s) {
    for (unsigned char c : s) if (std::isalnum(c)) return true;
    return false;
}

} // namespace

// The Epson parser (the Emic 2 manual's, as LambdaSpeak's ENGLISH.BAS uses it):
//   :-)N voice N (0-8)   ## whisper on/off   /\ pitch up   \/ pitch down
//   >> faster   << slower
std::vector<SpeechSegment> SpeechSynth::parseEpson(const std::string& text, int voice, int wpm, bool spanish) {
    std::vector<SpeechSegment> out;
    SpeechSegment cur;
    cur.voice = std::max(0, std::min(voice, 8));
    cur.wpm = wpm;
    cur.spanish = spanish;
    auto flush = [&]() {
        if (hasWords(cur.text)) out.push_back(cur);
        cur.text.clear();
    };
    for (size_t i = 0; i < text.size();) {
        auto at = [&](const char* p) { return text.compare(i, std::strlen(p), p) == 0; };
        if (at(":-)") && i + 3 < text.size() && std::isdigit((unsigned char)text[i + 3])) {
            flush(); cur.voice = std::min(8, text[i + 3] - '0'); i += 4;
        } else if (at("##")) { flush(); cur.whisper = !cur.whisper; i += 2; }
        else if (at("/\\")) { flush(); cur.pitch += 1; i += 2; }
        else if (at("\\/")) { flush(); cur.pitch -= 1; i += 2; }
        else if (at(">>")) { flush(); cur.wpm = std::min(600, (int)std::lround(cur.wpm * 1.15)); i += 2; }
        else if (at("<<")) { flush(); cur.wpm = std::max(75, (int)std::lround(cur.wpm / 1.15)); i += 2; }
        else { cur.text += text[i]; i += 1; }
    }
    flush();
    return out;
}

// DECtalk's in-line commands (the DECtalk 5.01 guide): [:np] [:nh] ... or [:name x] the
// voice, [:nN] by number (as LambdaSpeak sends it), [:rate N] words a minute, [:dv ap N]
// the average pitch in Hz; [:phoneme ... on] (or [:phone arpa speak on]) makes [...] a
// string of ARPAbet phones, each with an optional <duration,note>. Other commands are
// DECtalk's own and have no host equivalent; they are passed over.
std::vector<SpeechSegment> SpeechSynth::parseDecTalk(const std::string& text, int voice, int wpm, bool spanish) {
    std::vector<SpeechSegment> out;
    SpeechSegment cur;
    cur.voice = std::max(0, std::min(voice, 8));
    cur.wpm = wpm;
    cur.spanish = spanish;
    bool phonemes = false;
    auto flush = [&]() {
        if (hasWords(cur.text) || !cur.phones.empty()) out.push_back(cur);
        cur.text.clear();
        cur.phones.clear();
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '[') { cur.text += text[i++]; continue; }
        const size_t end = text.find(']', i);
        const std::string inside = text.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
        i = end == std::string::npos ? text.size() : end + 1;
        if (!inside.empty() && inside[0] == ':') {
            std::string c = lower(inside.substr(1));
            while (!c.empty() && c.back() == ' ') c.pop_back();
            static const char letters[] = "phbudkfrw";          // the S1V30120's order
            int v = -1;
            if (c.size() == 2 && c[0] == 'n') {
                if (std::isdigit((unsigned char)c[1])) v = c[1] - '0';
                else if (const char* p = std::strchr(letters, c[1])) v = (int)(p - letters);
            } else if (c.rfind("name ", 0) == 0 && c.size() > 5) {
                if (const char* p = std::strchr(letters, c[5])) v = (int)(p - letters);
            }
            if (v >= 0 && v <= 8) { flush(); cur.voice = v; continue; }
            int n = 0;
            if (std::sscanf(c.c_str(), "rate %d", &n) == 1) { flush(); cur.wpm = std::max(75, std::min(n, 600)); continue; }
            if (std::sscanf(c.c_str(), "dv ap %d", &n) == 1 && n > 0) { flush(); cur.pitch = (int)std::lround(6.0 * std::log2(n / 122.0)); continue; }
            if (c.rfind("phone", 0) == 0) {
                const bool on = c.find(" on") != std::string::npos, off = c.find(" off") != std::string::npos;
                if (on) phonemes = true;
                else if (off) phonemes = false;
            }
            continue;
        }
        if (phonemes) {
            if (hasWords(cur.text)) flush();
            cur.text.clear();
            parsePhones(inside, cur.phones);
        } else cur.text += inside;
    }
    flush();
    return out;
}

// ------------------------------------------------------------------ the board

void SpeechSynth::ls3Reset() {
    ls3Mode = ls3LastMode = LS3_SSA1;
    ls3Blocking = true; ls3Confirm = true; ls3NonBlockConfirm = false;
    ls3Voice = 1; ls3Rate = 9; ls3Volume = 13; ls3Language = 0; ls3FlushDelay = 10; ls3Getters = 0;
    steps.clear(); stepUs = 0;
    textBuffer.clear(); hexState = 0; ls3Pending = 0;
    ttsSample = 0.0f;
    serialMode = false; escape = 0; pendingResult = -1; directMode = true;
    txBuffer.clear(); rxBuffer.clear(); rxCursor = 0;
    chip.reset();
}

int SpeechSynth::ls3Idle() {
    switch (ls3Mode) {
        case LS3_EPSON: case LS3_DECTALK: return 32;
        case LS3_SERIAL: return 16;
        case LS3_SSA1: case LS3_SSA1_SP0:
            return (chip.sby_r() ? 0x80 : 0) | (chip.lrq_r() ? 0 : 0x40);   // 128 idle, 0 speaking, 64 full
        case LS3_DK: return chip.lrq_r() ? 0 : 0x80;
        case LS3_DK_SP0: return chip.lrq_r() ? 0x7f : 0xff;                 // the dk'tronics card's own bit 7
    }
    return 0xff;
}

int SpeechSynth::ls3Read() {
    if (!steps.empty()) return steps.front().bus;
    if (ls3Mode == LS3_SERIAL && pendingResult >= 0) { const int r = pendingResult; pendingResult = -1; return r; }
    return ls3Idle();
}

void SpeechSynth::ls3Speak(const std::string& text, bool hold) {
    const int wpm = 15 + 25 * ls3Rate;
    const bool spanish = ls3Language == 1;
    std::vector<SpeechSegment> say = ls3Mode == LS3_DECTALK ? parseDecTalk(text, ls3Voice - 1, wpm, spanish)
                                                            : parseEpson(text, ls3Voice - 1, wpm, spanish);
    for (SpeechSegment& s : say) s.gain = ls3Volume / 13.0;
    Step st;
    st.bus = 0;
    st.hold = hold;
    st.say = std::move(say);
    if (st.say.empty()) { st.us = 10; st.say.clear(); }   // nothing to say: a moment's work
    steps.push_back(std::move(st));
}

void SpeechSynth::ls3Confirmation(const std::string& text) {
    if (ls3Confirm) ls3Speak(text, !ls3NonBlockConfirm);
}

void SpeechSynth::ls3Getter(int value, bool fourBit, const std::string& name) {
    Step a; a.us = getterUs();
    if (fourBit) {
        a.bus = (value << 4) & 0xff;
        steps.push_back(a);
        if (ls3Confirm) {
            // the value stays on the bus while it is said (cpc_input)
            ls3Speak(name + " " + std::to_string(value) + ".", false);
            steps.back().bus = a.bus;
        }
        Step z; z.bus = 0; z.us = getterUs();
        steps.push_back(z);
    } else {
        a.bus = value & 0xff;
        steps.push_back(a);
        Step b; b.bus = 255; b.us = getterUs(); steps.push_back(b);
        Step z; z.bus = 0; z.us = 10; steps.push_back(z);
    }
}

void SpeechSynth::ls3Control(int v) {
    textBuffer.clear();                          // proceed(): what was not yet said is dropped
    hexState = 0;
    const bool sp0 = ls3Mode == LS3_SSA1_SP0 || ls3Mode == LS3_DK_SP0;
    const bool native = ls3Mode == LS3_EPSON || ls3Mode == LS3_DECTALK;
    auto mode = [&](int m, const char* say) { ls3Mode = ls3LastMode = m; ls3Confirmation(say); };
    switch (v) {
        case 0xff: { ls3Reset(); Step boot; boot.us = 50000; steps.push_back(boot); return; }
        case 0xf0: ls3Pending = 1; return;      // the next byte only lights the LEDs
        case 0xf1:
            ls3LastMode = ls3Mode;
            ls3Mode = LS3_SERIAL;
            serialMode = true; escape = 0; pendingResult = -1;
            return;
        case 0xf2:
            ls3Getter((ls3Mode & 15) | ls3Blocking << 4 | ls3NonBlockConfirm << 5 | ls3Language << 6 | ls3Confirm << 7, false, "");
            return;
        case 0xf4: ls3Confirmation("Use non-blocking confirmations."); ls3NonBlockConfirm = true; return;
        case 0xf3: ls3NonBlockConfirm = false; ls3Confirmation("Use blocking confirmations."); return;
        case 0xef: mode(LS3_EPSON, "Native Epson mode."); return;
        case 0xee: mode(LS3_DECTALK, "Native DecTalk mode."); return;
        case 0xed: mode(LS3_SSA1, "S-S-A 1 mode."); return;
        case 0xec: mode(LS3_DK, "DeeKay Tronics mode."); return;
        case 0xe2: mode(LS3_SSA1_SP0, "S-S-A 1 vintage mode."); return;
        case 0xe1: mode(LS3_DK_SP0, "DeeKay Tronics vintage mode."); return;
        case 0xc8: ls3Speak("LambdaSpeak 3, by Michael Wessel, with Dr. Stefan Stumpferl. Emulated by C-P-C Syntax Error.", !ls3NonBlockConfirm); return;
        case 0xc7: ls3Speak("I'm sorry Dave. I'm afraid I can't do that.", !ls3NonBlockConfirm); return;
        case 0xc6: ls3Speak("Daisy, Daisy, give me your answer do. I'm half crazy, all for the love of you.", !ls3NonBlockConfirm); return;
        case 0xc3: {
            static const char* names[] = { "S-S-A 1 mode.", "Epson mode.", "DecTalk mode.", "DeeKay Tronics mode.", "", "S-S-A 1 vintage mode.", "DeeKay Tronics vintage mode." };
            if (ls3Mode >= 0 && ls3Mode <= 6 && names[ls3Mode][0]) ls3Speak(names[ls3Mode], !ls3NonBlockConfirm);
            return;
        }
        default: break;
    }
    if (sp0) return;                             // the SP0 modes know only the commands above
    switch (v) {
        case 0xeb: ls3Blocking = false; ls3Confirmation("Non-Blocking mode."); return;
        case 0xea: ls3Blocking = true; ls3Confirmation("Blocking mode."); return;
        case 0xe9: ls3Confirm = true; ls3Confirmation("Confirmations on."); return;
        case 0xe8: ls3Confirm = false; return;
        case 0xe7: ls3Language = 0; ls3Confirmation("English mode."); return;
        case 0xe6: ls3Language = 1; ls3Confirmation("Castilian spanish mode."); return;
        case 0xe5: ls3Getters = 1; ls3Confirmation("Fast getters."); return;
        case 0xe0: ls3Getters = 2; ls3Confirmation("Medium getters."); return;
        case 0xe4: ls3Getters = 0; ls3Confirmation("Slow getters."); return;
        case 0xdf: return;                       // stop: nothing is being said
        case 0xde: if (native && !textBuffer.empty()) ls3Speak(textBuffer, ls3Blocking); return;
        case 0xdd: case 0xdc: {
            static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
            static const char* months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                            "September", "October", "November", "December" };
            const std::tm t = hostTime();
            char b[96];
            if (v == 0xdd) std::snprintf(b, sizeof b, "It is %d o clock, %d minutes, and %d seconds.", t.tm_hour, t.tm_min, t.tm_sec);
            else std::snprintf(b, sizeof b, "It is %s, %s %d 20%02d.", days[t.tm_wday], months[t.tm_mon], t.tm_mday, t.tm_year % 100);
            ls3Speak(b, !ls3NonBlockConfirm);
            return;
        }
        case 0xdb: ls3Pending = 3; return;       // set time: hours, minutes, seconds (the host's clock stays)
        case 0xda: ls3Pending = 4; return;       // set date: year, month, date, weekday
        case 0xd2: if (native) ls3Getter(21, false, ""); return;   // no thermometer here: a room's 21 C
        case 0xd1: ls3Speak("Temperature is 21 degrees celsius.", !ls3NonBlockConfirm); return;
        case 0xcf: ls3Getter((ls3Mode & 3) | ls3Blocking << 2 | ls3Language << 3, true, "Current mode"); return;
        case 0xce: ls3Getter(ls3Volume, true, "Current volume"); return;
        case 0xcd: ls3Getter(ls3Voice, true, "Current voice"); return;
        case 0xcc: ls3Getter(ls3Rate, true, "Current speak rate"); return;
        case 0xcb: ls3Getter(ls3Language, true, "Current language"); return;
        case 0xca: ls3Getter(ls3FlushDelay, true, "Current flush delay"); return;
        case 0xc9: ls3Getter(VERSION, false, ""); return;
        default: break;
    }
    if (v >= 0xd3 && v <= 0xd9) {
        if (!native) return;
        const std::tm t = hostTime();
        const int values[] = { t.tm_hour, t.tm_min, t.tm_sec, t.tm_year % 100, t.tm_mon + 1, t.tm_mday, t.tm_wday == 0 ? 7 : t.tm_wday };
        ls3Getter(values[v - 0xd3], false, "");
        return;
    }
    if (v >= 0xb0 && v <= 0xbf) { ls3Voice = v == 0xb0 ? 1 : std::min(9, v - 0xb0); ls3Confirmation("Voice set to " + std::to_string(ls3Voice) + "."); return; }
    if (v >= 0xa0 && v <= 0xaf) { ls3Volume = v == 0xa0 ? 13 : v - 0xa0; ls3Confirmation("Volume set to " + std::to_string(ls3Volume) + "."); return; }
    if (v >= 0x90 && v <= 0x9f) { ls3Rate = v == 0x90 ? 9 : v - 0x90; ls3Confirmation("Speech rate set to " + std::to_string(ls3Rate) + "."); return; }
    if (v >= 0x80 && v <= 0x8f) {
        ls3FlushDelay = v == 0x80 ? 10 : v - 0x80;
        ls3Confirmation("Flush buffer after " + std::to_string(10 + ls3FlushDelay * 10) + " milliseconds.");
        return;
    }
    // &FE-&F5 (the EEPROM's PCM sampler), &E3 (Amdrum), &C5 / &C2 (test programs): not here.
}

void SpeechSynth::ls3Write(int v) {
    v &= 0xff;
    if (!steps.empty()) {
        // Busy: the board is not reading the bus -- except for &DF while it speaks.
        Step& s = steps.front();
        if (v == 0xdf && !s.say.empty()) { steps.clear(); stepUs = 0; ttsSample = 0.0f; }
        return;
    }
    if (ls3Pending > 0) { ls3Pending -= 1; return; }
    if (ls3Mode == LS3_SERIAL) {
        serialWrite(v);
        if (!serialMode) ls3Mode = ls3LastMode;   // &FF,&14 quits back
        return;
    }
    if (v >= 0x80) { ls3Control(v); return; }
    switch (ls3Mode) {
        case LS3_SSA1: case LS3_SSA1_SP0: if (hasRom) chip.ald_w((uint8_t)v); return;
        case LS3_DK: case LS3_DK_SP0: if (hasRom) chip.ald_w((uint8_t)(v & 0x3f)); return;
        default: break;
    }
    // Epson / DECtalk text, with \xHH
    if (hexState == 0 && v == '\\') { hexState = 1; return; }
    if (hexState == 1) {
        hexState = 0;
        if (v == 'x') { hexState = 2; return; }
        textBuffer += '\\';                      // not an escape after all
    } else if (hexState == 2 || hexState == 3) {
        int d = v >= '0' && v <= '9' ? v - '0' : v >= 'A' && v <= 'F' ? v - 'A' : -1;   // (the firmware's 'A' = 0)
        if (d < 0) { hexState = 0; return; }
        if (hexState == 2) { hexHigh = d; hexState = 3; return; }
        hexState = 0;
        v = hexHigh * 16 + d;
    }
    textBuffer += (char)v;
    if (textBuffer.size() >= FLUSH_AT || v == 13) {
        std::string say = textBuffer;
        say.pop_back();                          // speak_buffer: buffer[length-1] = 0
        textBuffer.clear();
        ls3Speak(say, ls3Blocking);
    }
}

void SpeechSynth::ls3Advance() {
    if (steps.empty()) { ttsSample = 0.0f; return; }
    Step& s = steps.front();
    bool finished = false;
    if (!s.say.empty()) {
        if (!s.started) {
            s.started = true;
            lastSaid = s.say;
            s.job = HostSpeech::render(s.say);
        }
        if (s.job && s.job->done) {
            if (s.job->pcm.empty()) {
                // No voice on this host: the board still takes the time a sentence takes.
                if (!s.job->error.empty()) speechError = s.job->error;
                size_t chars = 0;
                for (const SpeechSegment& g : s.say) chars += g.text.size() + g.phones.size();
                stepUs += 1;
                finished = stepUs >= (long long)chars * 60000;
                ttsSample = 0.0f;
            } else {
                s.playPhase += s.job->rate / 1e6;
                while (s.playPhase >= 1.0) { s.playPhase -= 1.0; s.played += 1; }
                ttsSample = s.played < s.job->pcm.size() ? s.job->pcm[s.played] / 32768.0f : 0.0f;
                finished = s.played >= s.job->pcm.size();
            }
        }
    } else {
        stepUs += 1;
        finished = stepUs >= s.us;
    }
    if (finished) { steps.pop_front(); stepUs = 0; ttsSample = 0.0f; }
}

} // namespace cpcse
