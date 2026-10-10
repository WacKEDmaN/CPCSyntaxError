// CPCSyntaxError — external development: GDB remote server, command API, file watching.
// See devserver.h.
#include "devserver.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "cpc_typing.h"
#include "devcommand.h"
#include "devnet.h"
#include "emuhost.h"
#include "gui_debugger.h"
#include "minijson.h"
#include "core/debug_symbols.h"
#include "core/emulator.h"
#include "core/keyboard.h"
#include "core/memory.h"
#include "core/png.h"
#include "core/video.h"
#include "core/z80.h"

namespace cpcse {

namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string hex2(int v) { char b[4]; std::snprintf(b, sizeof(b), "%02x", v & 0xff); return b; }
std::string hexLE16(int v) { return hex2(v) + hex2(v >> 8); }

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "414243" -> "ABC" (stops at the first pair that is not hex).
std::string hexDecode(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        int a = hexDigit(h[i]), b = hexDigit(h[i + 1]);
        if (a < 0 || b < 0) break;
        out += (char)(a << 4 | b);
    }
    return out;
}

std::string hexEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) out += hex2(c);
    return out;
}

// The emulator's machine, or null when nothing is booted.
GX4000* machine(EmuHost& host) {
    return host.booted() && host.emu && host.emu->cpu && host.emu->memory ? host.emu : nullptr;
}

// ---------------------------------------------------------------- registers
// One table for the GDB client's names (MAME's: af2 for AF', ir) and the API's.
bool getRegister(GX4000* e, std::string name, int& value) {
    Z80& z = *e->cpu;
    name = lower(name);
    for (char& c : name) if (c == '\'') c = '2';
    struct Pair { const char* n; int v; };
    const Pair regs[] = {
        { "pc", z.pc }, { "sp", z.sp }, { "af", z.a << 8 | z.f }, { "bc", z.b << 8 | z.c },
        { "de", z.d << 8 | z.e }, { "hl", z.h << 8 | z.l }, { "ix", z.ix }, { "iy", z.iy },
        { "af2", z.ap << 8 | z.fp }, { "bc2", z.bp << 8 | z.cp }, { "de2", z.dp << 8 | z.ep },
        { "hl2", z.hp << 8 | z.lp }, { "ir", z.i << 8 | z.r }, { "im", z.im },
        { "a", z.a }, { "f", z.f }, { "b", z.b }, { "c", z.c }, { "d", z.d }, { "e", z.e },
        { "h", z.h }, { "l", z.l }, { "i", z.i }, { "r", z.r },
        { "ixh", z.ix >> 8 }, { "ixl", z.ix & 0xff }, { "iyh", z.iy >> 8 }, { "iyl", z.iy & 0xff },
        { "iff1", z.iff1 ? 1 : 0 }, { "iff2", z.iff2 ? 1 : 0 }, { "halt", z.halted ? 1 : 0 },
    };
    for (const auto& p : regs) if (name == p.n) { value = p.v & 0xffff; return true; }
    return false;
}

bool setRegister(GX4000* e, std::string name, int v) {
    Z80& z = *e->cpu;
    name = lower(name);
    for (char& c : name) if (c == '\'') c = '2';
    const int w = v & 0xffff, hi = w >> 8, lo = w & 0xff;
    if (name == "pc") { z.pc = w; z.halted = false; }
    else if (name == "sp") z.sp = w;
    else if (name == "af") { z.a = hi; z.f = lo; }
    else if (name == "bc") { z.b = hi; z.c = lo; }
    else if (name == "de") { z.d = hi; z.e = lo; }
    else if (name == "hl") { z.h = hi; z.l = lo; }
    else if (name == "ix") z.ix = w;
    else if (name == "iy") z.iy = w;
    else if (name == "af2") { z.ap = hi; z.fp = lo; }
    else if (name == "bc2") { z.bp = hi; z.cp = lo; }
    else if (name == "de2") { z.dp = hi; z.ep = lo; }
    else if (name == "hl2") { z.hp = hi; z.lp = lo; }
    else if (name == "ir") { z.i = hi; z.r = lo; }
    else if (name == "im") z.im = std::clamp(w, 0, 2);
    else if (name == "a") z.a = lo;
    else if (name == "f") z.f = lo;
    else if (name == "b") z.b = lo;
    else if (name == "c") z.c = lo;
    else if (name == "d") z.d = lo;
    else if (name == "e") z.e = lo;
    else if (name == "h") z.h = lo;
    else if (name == "l") z.l = lo;
    else if (name == "i") z.i = lo;
    else if (name == "r") z.r = lo;
    else if (name == "ixh") z.ix = (z.ix & 0xff) | lo << 8;
    else if (name == "ixl") z.ix = (z.ix & 0xff00) | lo;
    else if (name == "iyh") z.iy = (z.iy & 0xff) | lo << 8;
    else if (name == "iyl") z.iy = (z.iy & 0xff00) | lo;
    else if (name == "iff1") z.iff1 = w != 0;
    else if (name == "iff2") z.iff2 = w != 0;
    else return false;
    return true;
}

// MAME's register order in its Z80 target description; GDB numbers them from 0, so the
// PC is register 11 (0x0b) -- the number DeZog reads after a Ctrl-C and in stop reports.
const char* const GDB_REGS[] = { "af", "bc", "de", "hl", "af2", "bc2", "de2", "hl2", "ix", "iy", "sp", "pc" };
constexpr int GDB_REG_COUNT = 12;

const char* const TARGET_XML =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "<architecture>z80</architecture>\n"
    "<feature name=\"mame.z80\">\n"
    "<reg name=\"af\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"bc\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"de\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"hl\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"af'\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"bc'\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"de'\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"hl'\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"ix\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"iy\" bitsize=\"16\" type=\"int\"/>\n"
    "<reg name=\"sp\" bitsize=\"16\" type=\"data_ptr\"/>\n"
    "<reg name=\"pc\" bitsize=\"16\" type=\"code_ptr\"/>\n"
    "</feature>\n"
    "</target>\n";

// ---------------------------------------------------------------- expressions
// MAME's debugger expressions, as much as a GDB client sends through "monitor print":
// numbers are hex unless written #decimal (0x and $ are hex too), registers by name,
// b@ w@ d@ read memory, + - * / % & | ^ << >> and parentheses.
class Expr {
public:
    Expr(GX4000* e, const std::string& s) : e(e), s(s) {}
    bool eval(long& out, std::string& error) {
        try {
            out = orExpr();
            space();
            if (i != s.size()) throw std::runtime_error("unexpected '" + s.substr(i) + "'");
            return true;
        } catch (const std::exception& ex) { error = ex.what(); return false; }
    }

private:
    GX4000* e;
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    struct Nest {   // "((((((..." or "------...": 200 deep at most, not the stack's limit
        Expr& x;
        explicit Nest(Expr& x) : x(x) { if (++x.depth > 200) throw std::runtime_error("nested too deeply"); }
        ~Nest() { x.depth--; }
    };
    void space() { while (i < s.size() && std::isspace((unsigned char)s[i])) i++; }
    bool take(const char* op) {
        space();
        size_t n = std::char_traits<char>::length(op);
        if (s.compare(i, n, op) != 0) return false;
        // "<" is not "<<", "&" is not "&&"
        if (n == 1 && i + 1 < s.size() && s[i + 1] == op[0] && (op[0] == '<' || op[0] == '>' || op[0] == '&' || op[0] == '|')) return false;
        i += n;
        return true;
    }
    int byte(long a) { return e->memory->readMapped((int)(a & 0xffff)); }
    long orExpr() { long v = xorExpr(); while (take("|")) v |= xorExpr(); return v; }
    long xorExpr() { long v = andExpr(); while (take("^")) v ^= andExpr(); return v; }
    long andExpr() { long v = shiftExpr(); while (take("&")) v &= shiftExpr(); return v; }
    long shiftExpr() {
        long v = addExpr();
        for (;;) {
            // A shift by a negative count or by the whole width is undefined: 0 bits out.
            if (take("<<")) { long n = addExpr(); v = n < 0 || n >= 63 ? 0 : (long)((unsigned long)v << n); }
            else if (take(">>")) { long n = addExpr(); v = n < 0 || n >= 63 ? (v < 0 ? -1 : 0) : v >> n; }
            else return v;
        }
    }
    long addExpr() {
        long v = mulExpr();
        for (;;) {
            if (take("+")) v += mulExpr();
            else if (take("-")) v -= mulExpr();
            else return v;
        }
    }
    long mulExpr() {
        long v = unary();
        for (;;) {
            if (take("*")) v *= unary();
            // x / -1 and x % -1 apart: the smallest long divided by -1 traps the CPU
            else if (take("/")) { long d = unary(); if (!d) throw std::runtime_error("division by zero"); v = d == -1 ? (long)(0UL - (unsigned long)v) : v / d; }
            else if (take("%")) { long d = unary(); if (!d) throw std::runtime_error("division by zero"); v = d == -1 ? 0 : v % d; }
            else return v;
        }
    }
    long unary() {
        Nest nest(*this);
        if (take("-")) return (long)(0UL - (unsigned long)unary());
        if (take("~")) return ~unary();
        if (take("!")) return !unary();
        return primary();
    }
    long primary() {
        Nest nest(*this);
        space();
        if (i >= s.size()) throw std::runtime_error("missing value");
        if (take("(")) { long v = orExpr(); if (!take(")")) throw std::runtime_error("missing ')'"); return v; }
        // b@ w@ d@: a byte, a word, a double word (little-endian) at an address
        if (i + 1 < s.size() && s[i + 1] == '@' && (std::tolower((unsigned char)s[i]) == 'b' ||
            std::tolower((unsigned char)s[i]) == 'w' || std::tolower((unsigned char)s[i]) == 'd')) {
            char size = (char)std::tolower((unsigned char)s[i]);
            i += 2;
            long a = unary();
            if (size == 'b') return byte(a);
            if (size == 'w') return byte(a) | byte(a + 1) << 8;
            return (long)((unsigned long)(byte(a) | byte(a + 1) << 8) | (unsigned long)(byte(a + 2) | byte(a + 3) << 8) << 16);
        }
        if (s[i] == '#') {   // decimal
            i++;
            size_t st = i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
            if (st == i) throw std::runtime_error("bad decimal number");
            return std::stol(s.substr(st, i - st));
        }
        if (s[i] == '$' || s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) {
            i += s[i] == '$' ? 1 : 2;
            size_t st = i;
            while (i < s.size() && std::isxdigit((unsigned char)s[i])) i++;
            if (st == i) throw std::runtime_error("bad hex number");
            return std::stol(s.substr(st, i - st), nullptr, 16);
        }
        // A word: a register name, else a hex number (MAME's default base).
        size_t st = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '\'')) i++;
        if (st == i) throw std::runtime_error("unexpected '" + s.substr(st, 1) + "'");
        std::string w = s.substr(st, i - st);
        int reg;
        if (getRegister(e, w, reg)) return reg;
        bool hex = std::all_of(w.begin(), w.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; });
        if (hex) return std::stol(w, nullptr, 16);
        throw std::runtime_error("unknown symbol '" + w + "'");
    }
};

// ---------------------------------------------------------------- AMSDOS header
// The 128-byte header AMSDOS puts in front of a file: checksum (bytes 0-66 summed) at 67,
// type at 18, load address at 21, entry at 26, and the file's length at 64-66 (24-25 is
// the logical length of one record, the same for a disc file but not to be relied on).
struct AmsdosInfo { bool present = false; int type = 0, load = 0, length = 0, entry = 0; };

AmsdosInfo amsdosHeader(const std::vector<uint8_t>& d) {
    AmsdosInfo h;
    if (d.size() < 128) return h;
    unsigned sum = 0;
    for (int i = 0; i < 67; i++) sum += d[(size_t)i];
    if ((sum & 0xffff) != (unsigned)(d[67] | d[68] << 8)) return h;
    bool blank = true;
    for (int i = 0; i < 67 && blank; i++) blank = d[(size_t)i] == 0;
    if (blank) return h;
    h.present = true;
    h.type = d[18];
    h.load = d[21] | d[22] << 8;
    h.length = d[64] | d[65] << 8 | d[66] << 16;
    if (!h.length) h.length = d[24] | d[25] << 8;
    h.entry = d[26] | d[27] << 8;
    return h;
}

std::string fileName(const std::string& path) { return std::filesystem::path(path).filename().string(); }

// A web page can have the browser send an HTTP request to a port on this computer, and a
// POST's body could then carry API commands or GDB packets (a cross-protocol attack: the
// page needs no permission for it). Neither server speaks HTTP, so a connection that
// opens with an HTTP request line is closed before anything in it runs.
// 1: it is one; 0: it is not; -1: too little has come to tell.
int httpRequestStart(const std::string& in) {
    static const char* const methods[] = { "GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ", "CONNECT ", "TRACE " };
    for (const char* m : methods) {
        const size_t n = std::strlen(m), k = std::min(n, in.size());
        if (in.compare(0, k, m, k) == 0) return k == n ? 1 : -1;
    }
    return 0;
}

// A JSON number as a whole number, held in range: converting a double outside the target
// type (1e300, from any client) is undefined.
long wholeNumber(double d, long lo, long hi) {
    if (!(d >= (double)lo)) return lo;   // NaN too
    if (d >= (double)hi) return hi;
    return (long)d;
}

} // namespace

// ==================================================================== state
struct DevServer::Impl {
    EmuHost& host;
    Debugger& dbg;
    DevServer& self;
    Impl(EmuHost& h, Debugger& d, DevServer& s) : host(h), dbg(d), self(s) {}

    // ---------------------------------------------------------------- GDB
    struct Gdb {
        net::Socket listener = net::NO_SOCKET, client = net::NO_SOCKET;
        int port = 0;
        std::string in, out;
        net::Socket candidate = net::NO_SOCKET;   // a new connection not yet vetted (pollGdb)
        std::string candidateIn;
        bool noAck = false;
        bool running = false;     // the client sent c (or s ran on) and waits for a stop report
        int nextBpId = 1;
    } gdb;

    // ---------------------------------------------------------------- API
    struct Waiting {
        json::Value id;
        bool hasId = false;
        std::string kind;          // stop / frames / typed
        uint64_t untilFrame = 0;
        Clock::time_point deadline;
    };
    struct ApiClient {
        net::Socket sock = net::NO_SOCKET;
        std::string in;
        std::vector<Waiting> waiting;
        bool vetted = false;       // its first bytes are not an HTTP request (httpRequestStart)
        bool closing = false;
    };
    net::Socket apiListener = net::NO_SOCKET;
    int apiPortNow = 0;
    std::vector<ApiClient> apiClients;

    // ---------------------------------------------------------------- typing
    // no code: a pause; ch: a typed character (cpcTypeKey), else the key `code` itself
    struct Key { std::string code; bool shift = false; int waitFrames = 0; char ch = 0; };
    std::deque<Key> keys;
    int keyPhase = 0;              // 0 idle, 1 held, 2 released (the gap)
    uint64_t keyUntil = 0;
    uint64_t typeFrom = 0;         // no key before this frame (the firmware is starting)
    Key keyNow;

    // ---------------------------------------------------------------- loading
    bool pendingLoad = false;
    LoadRequest pending;
    uint64_t pendingAt = 0;        // the frame it loads at
    static constexpr uint64_t FIRMWARE_FRAMES = 100;   // a CPC is at its prompt by then

    bool watching = false;
    LoadRequest watchRequest;
    bool stampKnown = false;
    std::filesystem::file_time_type stampTime{};
    uintmax_t stampSize = 0;
    Clock::time_point changedAt{}, lastCheck{};
    bool changeSeen = false;
    int reloadCount = 0;

    // ================================================================ common
    bool paused() const { return !host.booted() || host.paused; }

    // Resume without the breakpoint skip a debugger's "continue" wants: a program just
    // loaded must stop on a breakpoint at its first instruction.
    void resumeFresh() {
        GX4000* e = machine(host);
        if (!e) return;
        e->stepTarget = -1;
        e->breakpointSkipOnce = -1;
        host.stopAfter = nullptr;
        host.breakReason.clear();
        dbg.lastWatchAddress = -1;
        host.paused = false;
        host.status = "Running";
    }

    int parseAddr(const json::Value& req, const char* key, int otherwise) {
        const json::Value* v = req.find(key);
        if (!v) return otherwise;
        if (v->type == json::Value::Number) return (int)wholeNumber(v->number, -1, 0x7fffffffL);
        if (v->type != json::Value::String) return otherwise;
        int a = dbg.parseAddress(v->text);
        if (a < 0) throw std::runtime_error(std::string("'") + key + "': not an address or a label: " + v->text);
        return a;
    }
    // A count or length: decimal unless it carries a hex prefix (& # $ 0x).
    long parseCount(const json::Value& req, const char* key, long otherwise) {
        const json::Value* v = req.find(key);
        if (!v) return otherwise;
        if (v->type == json::Value::Number) return wholeNumber(v->number, -0x7fffffffL, 0x7fffffffL);
        if (v->type != json::Value::String) return otherwise;
        const std::string& t = v->text;
        if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }))
        {
            const size_t lead = std::min(t.find_first_not_of('0'), t.size());
            return t.size() - lead > 9 ? 0x7fffffffL : std::stol(t);   // past a long on Windows: as large as it gets
        }
        int a = dbg.parseAddress(t);
        if (a < 0) throw std::runtime_error(std::string("'") + key + "': not a number: " + t);
        return a;
    }

    // ================================================================ typing
    void queueText(const std::string& text) {
        for (char c : text) {
            Key k;
            k.ch = c;
            if (cpcKeyForChar(c, k.code, k.shift)) keys.push_back(k);
        }
    }

    void tickKeys() {
        KeyboardMatrix* kb = host.emu ? host.emu->keyboard : nullptr;
        if (!kb) { keys.clear(); keyPhase = 0; return; }
        const uint64_t f = host.framesRun;
        // Held 4 frames and released 4, as the headless --type does: the firmware scans
        // the keyboard once a frame and wants to see a key up before it counts it again.
        if (keyPhase == 1 && f >= keyUntil) {
            if (keyNow.ch) cpcTypeKey(*kb, keyNow.ch, false);
            else {
                kb->setKey(keyNow.code, false);
                if (keyNow.shift) kb->setKey("ShiftLeft", false);
            }
            keyPhase = 2;
            keyUntil = f + 4;
        }
        if (keyPhase == 2 && f >= keyUntil) keyPhase = 0;
        if (keyPhase == 0 && !keys.empty() && f >= typeFrom) {
            keyNow = keys.front();
            keys.pop_front();
            if (keyNow.code.empty()) {   // a pause: frames with no key
                keyPhase = 2;
                keyUntil = f + (uint64_t)keyNow.waitFrames;
                return;
            }
            if (keyNow.ch) {
                if (!cpcTypeKey(*kb, keyNow.ch, true)) return;   // not on this keyboard: skipped
            } else {
                if (keyNow.shift) kb->setKey("ShiftLeft", true);
                kb->setKey(keyNow.code, true);
            }
            keyPhase = 1;
            keyUntil = f + 4;
        }
    }

    bool typing() const { return !keys.empty() || keyPhase != 0; }

    // ================================================================ loading
    int loadSymbols(const std::string& path, std::string& error) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { error = "cannot read " + path; return -1; }
        std::stringstream ss;
        ss << f.rdbuf();
        // sjasmplus writes "name: EQU 0x00001234"; the core's reader takes "name EQU $1234".
        // A line at a time, and only short ones: std::regex recurses once a character, and
        // a long line (a binary named as the symbol file) overflowed the stack. The core's
        // reader passes over long lines too.
        static const std::regex colon("^([A-Za-z_.@?][\\w.@?]*):[ \\t]"), hex("\\b0[xX]([0-9A-Fa-f]+)");
        std::string text;
        {
            std::istringstream in(ss.str());
            for (std::string line; std::getline(in, line);) {
                if (line.size() > 512) continue;
                line = std::regex_replace(line, colon, "$1 ");
                text += std::regex_replace(line, hex, "$$$1") + "\n";
            }
        }
        ParseSymbolOptions opt;
        opt.fileName = fileName(path);
        std::vector<RemuSymbol> parsed = parseSymbolText(text, opt);
        if (parsed.empty()) { error = "no symbols found in " + path; return -1; }
        std::vector<std::pair<std::string, int>> list;
        for (const auto& s : parsed) list.emplace_back(s.name, s.address & 0xffff);
        dbg.setSymbols(std::move(list));
        return (int)parsed.size();
    }

    // A GDB client is attached and stopped: the machine is its to run. Loading still writes
    // memory and sets the PC, but leaves the start to the client's continue -- otherwise the
    // machine would run while the client believes it stopped, and miss the next stop report.
    bool gdbHolds() const { return gdb.client != net::NO_SOCKET && !gdb.running; }
    void startRunning() {
        if (gdbHolds()) { host.paused = true; return; }
        resumeFresh();
    }

    bool load(const LoadRequest& r, std::string& error) {
        if (!machine(host)) { error = "no machine is running"; return false; }
        if (r.path.empty()) { error = "no file named"; return false; }
        std::error_code ec;
        if (!std::filesystem::exists(r.path, ec)) { error = "no such file: " + r.path; return false; }
        if (!r.symbols.empty() && loadSymbols(r.symbols, error) < 0) return false;
        const std::string ext = lower(std::filesystem::path(r.path).extension().string());
        const bool disc = ext == ".dsk" || ext == ".edsk" || ext == ".hfe" || ext == ".ipf";
        const bool tape = ext == ".cdt" || ext == ".tzx" || ext == ".tap" || ext == ".wav";
        bool ok = true;
        if (ext == ".sna") {
            ok = host.loadSnapshot(r.path);
            if (ok) startRunning();
        } else if (ext == ".cpr") {
            ok = host.loadCartridgeFile(r.path);
            if (ok) startRunning();
        } else if (disc || tape) {
            if (r.reset) { host.reset(); startRunning(); }
            ok = disc ? host.loadDiskFile(r.path, std::clamp(r.unit, 0, 1)) : host.loadTapeFile(r.path);
            // PLAY pressed, as a person loading it would; the motor relay still decides
            // when the tape moves.
            if (ok && tape) host.tapePlay();
        } else {
            // Anything else is a binary for memory: with an AMSDOS header, or at an address.
            bool readOk = false;
            Bytes data = EmuHost::readFile(r.path, readOk);
            if (!readOk) { error = "cannot read " + r.path; return false; }
            AmsdosInfo h = amsdosHeader(data);
            size_t from = 0, length = data.size();
            if (h.present) { from = 128; length = std::min<size_t>(h.length ? (size_t)h.length : data.size() - 128, data.size() - 128); }
            int at = r.address >= 0 ? r.address : (h.present ? h.load : -1);
            if (at < 0) { error = fileName(r.path) + " has no AMSDOS header: say where it goes (an address)"; return false; }
            if (length > 0x10000) length = 0x10000;
            if (r.reset) {
                // The firmware's start-up would write over it: reset now, load once it is up.
                host.reset();
                startRunning();
                pending = r;
                pending.reset = false;
                pending.symbols.clear();
                pendingAt = host.framesRun + FIRMWARE_FRAMES;
                pendingLoad = true;
                host.status = "Reset; " + fileName(r.path) + " loads once the firmware has started";
                self.lastEvent = host.status;
                return true;
            }
            GX4000* e = host.emu;
            for (size_t i = 0; i < length; i++) e->memory->write((int)((at + i) & 0xffff), data[from + i]);
            const int pc = r.run == LoadRequest::RUN_ENTRY ? (h.present && r.address < 0 ? h.entry : at) : r.run;
            char b[200];
            if (r.run == LoadRequest::NO_RUN)
                std::snprintf(b, sizeof(b), "Loaded %s: %zu bytes at &%04X", fileName(r.path).c_str(), length, at & 0xffff);
            else {
                dbg.setPc(pc);
                startRunning();
                std::snprintf(b, sizeof(b), "Loaded %s: %zu bytes at &%04X, %s at &%04X", fileName(r.path).c_str(), length,
                              at & 0xffff, gdbHolds() ? "PC" : "started", pc & 0xffff);
            }
            host.status = b;
        }
        if (!ok) { error = host.status; return false; }
        self.lastEvent = host.status;
        if (!r.command.empty()) {
            // After a reset or a cartridge the firmware needs its start-up first.
            if (r.reset || ext == ".cpr") typeFrom = host.framesRun + FIRMWARE_FRAMES;
            if (paused()) startRunning();
            queueText(r.command);
            // A tape's RUN" asks "Press PLAY then any key": answer it once it is up (a key
            // typed straight after the Enter is gone before the prompt reads one).
            if (tape && r.command.back() == '\n') {
                keys.push_back(Key{ "", false, 30 });
                keys.push_back(Key{ "Space", false, 0 });
            }
        }
        return true;
    }

    void tickLoads() {
        if (pendingLoad && host.framesRun >= pendingAt) {
            pendingLoad = false;
            std::string err;
            if (!load(pending, err)) self.lastEvent = "Load failed: " + err;
        }
        if (!watching) return;
        const Clock::time_point now = Clock::now();
        if (now - lastCheck < std::chrono::milliseconds(200)) return;
        lastCheck = now;
        std::error_code ec;
        auto t = std::filesystem::last_write_time(watchRequest.path, ec);
        if (ec) return;   // being rewritten, or not built yet
        uintmax_t size = std::filesystem::file_size(watchRequest.path, ec);
        if (ec) return;
        if (!stampKnown || t != stampTime || size != stampSize) {
            stampKnown = true;
            stampTime = t;
            stampSize = size;
            changeSeen = true;
            changedAt = now;
            return;
        }
        // Unchanged for a moment: the build has finished writing it.
        if (changeSeen && now - changedAt >= std::chrono::milliseconds(300) && host.framesRun >= FIRMWARE_FRAMES) {
            changeSeen = false;
            std::string err;
            if (load(watchRequest, err)) {
                reloadCount++;
                self.lastEvent = "Reloaded " + fileName(watchRequest.path) + " (" + std::to_string(reloadCount) + ")";
                host.status = self.lastEvent;
            } else {
                self.lastEvent = "Reload failed: " + err;
            }
        }
    }

    // ================================================================ GDB server
    void gdbPacket(const std::string& data) {
        unsigned sum = 0;
        for (unsigned char c : data) sum += c;
        gdb.out += "$" + data + "#" + hex2((int)sum);
    }

    std::string stopReply() {
        GX4000* e = machine(host);
        int pc = e ? e->cpu->pc & 0xffff : 0;
        std::string r = "T05";
        if (dbg.lastWatchAddress >= 0) {
            char b[32];
            std::snprintf(b, sizeof(b), "%swatch:%04x;", dbg.lastWatchWrite ? "" : "r", dbg.lastWatchAddress & 0xffff);
            r += b;
        }
        return r + "0b:" + hexLE16(pc) + ";";
    }

    // Everything a GDB client set up, gone with it.
    void gdbForget() {
        auto& bps = dbg.breakpoints;
        bool changed = false;
        for (size_t i = bps.size(); i-- > 0;) if (bps[i].remoteId || bps[i].remoteTemp) { bps.erase(bps.begin() + (long)i); changed = true; }
        if (changed) dbg.breakpointsChanged();
        auto& wps = dbg.watchpoints;
        wps.erase(std::remove_if(wps.begin(), wps.end(), [](const GuiWatchpoint& w) { return w.remote; }), wps.end());
    }

    void gdbDisconnect(const char* why) {
        if (gdb.client == net::NO_SOCKET) return;
        net::closeSocket(gdb.client);
        gdb.client = net::NO_SOCKET;
        gdb.in.clear();
        gdb.out.clear();
        gdb.running = false;
        gdbForget();
        // As MAME does when its debugger detaches: the machine carries on.
        if (machine(host) && host.paused) dbg.run();
        self.lastEvent = std::string("GDB client ") + why;
    }

    std::string monitorCommand(const std::string& line) {
        GX4000* e = machine(host);
        if (!e) return "E01";
        std::string out;
        size_t at = 0;
        // Several commands may come in one, separated by ';'.
        while (at <= line.size()) {
            size_t semi = line.find(';', at);
            std::string cmd = line.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            at = semi == std::string::npos ? line.size() + 1 : semi + 1;
            size_t a = cmd.find_first_not_of(" \t"), b = cmd.find_last_not_of(" \t\r\n");
            if (a == std::string::npos) continue;
            cmd = cmd.substr(a, b - a + 1);
            std::string word = lower(cmd.substr(0, cmd.find_first_of(" \t")));
            std::string args = cmd.size() > word.size() ? cmd.substr(word.size()) : "";
            auto argList = [&]() {
                std::vector<std::string> v;
                std::string cur;
                int depth = 0;
                for (char c : args) {
                    if (c == '(') depth++;
                    if (c == ')') depth--;
                    if (c == ',' && depth == 0) { v.push_back(cur); cur.clear(); } else cur += c;
                }
                if (!cur.empty() || !v.empty()) v.push_back(cur);
                return v;
            };
            auto eval = [&](const std::string& text, long& v) -> bool {
                std::string err;
                if (Expr(e, text).eval(v, err)) return true;
                out += "Error: " + err + "\n";
                return false;
            };
            if (word == "print" || word == "p") {
                std::string values;
                for (const std::string& x : argList()) {
                    long v;
                    if (!eval(x, v)) return hexEncode(out);
                    char hb[24];
                    std::snprintf(hb, sizeof(hb), "%lX", (unsigned long)v & 0xffffffffUL);
                    if (!values.empty()) values += ' ';
                    values += hb;
                }
                if (!out.empty()) out += ' ';
                out += values;
            } else if (word == "bpset" || word == "bp") {
                std::vector<std::string> v = argList();
                long addr;
                if (v.empty() || !eval(v[0], addr)) return hexEncode(out.empty() ? "Error: bpset needs an address" : out);
                GuiBreakpoint bp;
                bp.address = (int)(addr & 0xffff);
                bp.remoteId = gdb.nextBpId++;
                if (v.size() > 1) {
                    std::string cond = v[1];
                    size_t c0 = cond.find_first_not_of(" \t");
                    cond = c0 == std::string::npos ? "" : cond.substr(c0);
                    if (!cond.empty() && cond != "1") {
                        dbg.setCondition(bp, cond);
                        if (!bp.error.empty()) return hexEncode("Error: condition: " + bp.error);
                    }
                }
                dbg.breakpoints.push_back(bp);
                dbg.breakpointsChanged();
                char msg[64];
                std::snprintf(msg, sizeof(msg), "Breakpoint %X set", bp.remoteId);
                out += msg;
            } else if (word == "bpclear" || word == "bpdisable" || word == "bpenable") {
                std::vector<std::string> v = argList();
                int cleared = 0;
                long id = -1;
                if (!v.empty() && !eval(v[0], id)) return hexEncode(out);
                for (size_t i = dbg.breakpoints.size(); i-- > 0;) {
                    GuiBreakpoint& bp = dbg.breakpoints[i];
                    if (!bp.remoteId || (id >= 0 && bp.remoteId != id)) continue;
                    if (word == "bpclear") dbg.breakpoints.erase(dbg.breakpoints.begin() + (long)i);
                    else bp.enabled = word == "bpenable";
                    cleared++;
                }
                dbg.breakpointsChanged();
                char msg[64];
                // One of ours already gone (deleted in the Debugger window) is cleared all the
                // same: DeZog treats anything else as a failure.
                if (id >= 0 && !cleared && (id < 1 || id >= gdb.nextBpId)) std::snprintf(msg, sizeof(msg), "Error: invalid breakpoint number %lX", id);
                else if (id >= 0) std::snprintf(msg, sizeof(msg), "Breakpoint %lX %s", id, word == "bpclear" ? "cleared" : word == "bpenable" ? "enabled" : "disabled");
                else std::snprintf(msg, sizeof(msg), "Cleared all breakpoints");
                out += msg;
            } else if (word == "bplist") {
                for (const auto& bp : dbg.breakpoints) {
                    if (!bp.remoteId) continue;
                    char msg[96];
                    std::snprintf(msg, sizeof(msg), "%c%4X : %04X%s%s\n", bp.enabled ? ' ' : 'D', bp.remoteId, bp.address & 0xffff,
                                  bp.condition.empty() ? "" : " if ", bp.condition.c_str());
                    out += msg;
                }
                if (out.empty()) out = "No breakpoints currently installed";
            } else if (word == "help") {
                out += "print <expr>[,<expr>...]   values in hex (b@ w@ d@ read memory)\n"
                       "<reg>=<expr>               set a register (af bc de hl ix iy sp pc af2 bc2 de2 hl2 ir im, a..l, i, r)\n"
                       "bpset <addr>[,<cond>]      breakpoint (condition in the emulator's syntax: A==&3F && HITS>2)\n"
                       "bpclear [<id>]             remove one, or all\n"
                       "bplist                     list them\n"
                       "reset                      reset the machine\n";
            } else if (word == "reset") {
                host.reset();
                out += "Reset";
            } else if (cmd.find('=') != std::string::npos && cmd.find("==") == std::string::npos) {
                size_t eq = cmd.find('=');
                std::string name = cmd.substr(0, eq);
                name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return std::isspace((unsigned char)c) != 0; }), name.end());
                long v;
                if (!eval(cmd.substr(eq + 1), v)) return hexEncode(out);
                if (!setRegister(e, name, (int)v)) return hexEncode("Error: unknown register " + name);
            } else {
                return hexEncode("Error: unknown command '" + word + "' (try help)");
            }
        }
        return out.empty() ? "OK" : hexEncode(out);
    }

    void gdbHandle(const std::string& p) {
        GX4000* e = machine(host);
        auto reply = [&](const std::string& r) { gdbPacket(r); };
        if (!e || p.empty()) { reply(p.empty() ? "" : "E01"); return; }
        if (p.rfind("qSupported", 0) == 0) { reply("PacketSize=4000;qXfer:features:read+;QStartNoAckMode+"); return; }
        if (p == "QStartNoAckMode") { reply("OK"); gdb.noAck = true; return; }
        if (p.rfind("qXfer:features:read:target.xml:", 0) == 0) {
            size_t comma = p.find(',', 31);
            unsigned long off = std::strtoul(p.c_str() + 31, nullptr, 16);
            unsigned long len = comma == std::string::npos ? 0xffff : std::strtoul(p.c_str() + comma + 1, nullptr, 16);
            std::string xml = TARGET_XML;
            if (off >= xml.size()) { reply("l"); return; }
            std::string part = xml.substr(off, len);
            reply((off + part.size() >= xml.size() ? "l" : "m") + part);
            return;
        }
        if (p.rfind("qXfer:", 0) == 0) { reply("E00"); return; }
        if (p == "qAttached") { reply("1"); return; }
        if (p == "qC") { reply("QC1"); return; }
        if (p == "qfThreadInfo") { reply("m1"); return; }
        if (p == "qsThreadInfo") { reply("l"); return; }
        if (p == "qSymbol::") { reply("OK"); return; }
        if (p.rfind("qRcmd,", 0) == 0) { reply(monitorCommand(hexDecode(p.substr(6)))); return; }
        if (p[0] == 'H') { reply("OK"); return; }
        if (p == "?") { reply(stopReply()); return; }
        if (p == "g") {
            std::string r;
            for (const char* n : GDB_REGS) { int v = 0; getRegister(e, n, v); r += hexLE16(v); }
            reply(r);
            return;
        }
        // A register's value: four hex digits, low byte first; -1 if any is not hex.
        auto word = [](const char* h) {
            for (int k = 0; k < 4; k++) if (hexDigit(h[k]) < 0) return -1;
            return hexDigit(h[2]) << 12 | hexDigit(h[3]) << 8 | hexDigit(h[0]) << 4 | hexDigit(h[1]);
        };
        if (p[0] == 'G') {
            for (int k = 0; k < GDB_REG_COUNT && 1 + k * 4 + 4 <= (int)p.size(); k++)
                if (word(p.c_str() + 1 + k * 4) < 0) { reply("E01"); return; }
            for (int k = 0; k < GDB_REG_COUNT && 1 + k * 4 + 4 <= (int)p.size(); k++)
                setRegister(e, GDB_REGS[k], word(p.c_str() + 1 + k * 4));
            reply("OK");
            return;
        }
        if (p[0] == 'p') {
            int n = (int)std::strtol(p.c_str() + 1, nullptr, 16);
            if (n < 0 || n >= GDB_REG_COUNT) { reply("E01"); return; }
            int v = 0;
            getRegister(e, GDB_REGS[n], v);
            reply(hexLE16(v));
            return;
        }
        if (p[0] == 'P') {
            size_t eq = p.find('=');
            int n = (int)std::strtol(p.c_str() + 1, nullptr, 16);
            if (eq == std::string::npos || n < 0 || n >= GDB_REG_COUNT || p.size() < eq + 5) { reply("E01"); return; }
            const int v = word(p.c_str() + eq + 1);
            if (v < 0) { reply("E01"); return; }
            setRegister(e, GDB_REGS[n], v);
            reply("OK");
            return;
        }
        if (p[0] == 'm') {
            char* end = nullptr;
            unsigned long a = std::strtoul(p.c_str() + 1, &end, 16);
            unsigned long n = end && *end == ',' ? std::strtoul(end + 1, nullptr, 16) : 0;
            n = std::min(n, 0x10000UL);
            std::string r;
            r.reserve(n * 2);
            for (unsigned long k = 0; k < n; k++) r += hex2(e->memory->readMapped((int)((a + k) & 0xffff)));
            reply(r);
            return;
        }
        if (p[0] == 'M') {
            char* end = nullptr;
            unsigned long a = std::strtoul(p.c_str() + 1, &end, 16);
            unsigned long n = end && *end == ',' ? std::strtoul(end + 1, &end, 16) : 0;
            if (!end || *end != ':') { reply("E01"); return; }
            const char* h = end + 1;
            // n first: a huge n made n * 2 wrap round to a small number that passed, and the
            // writes then read far past the packet
            if (n > 0x10000UL || std::strlen(h) < n * 2) { reply("E01"); return; }
            for (unsigned long k = 0; k < n * 2; k++) if (hexDigit(h[k]) < 0) { reply("E01"); return; }
            for (unsigned long k = 0; k < n; k++, h += 2)
                e->memory->write((int)((a + k) & 0xffff), hexDigit(h[0]) << 4 | hexDigit(h[1]));
            reply("OK");
            return;
        }
        if (p[0] == 'c') {
            if (p.size() > 1) dbg.setPc((int)std::strtoul(p.c_str() + 1, nullptr, 16));
            dbg.run();
            gdb.running = true;   // the reply is the stop report, whenever it comes
            return;
        }
        if (p[0] == 's') {
            if (p.size() > 1) dbg.setPc((int)std::strtoul(p.c_str() + 1, nullptr, 16));
            dbg.stepInto();
            reply(stopReply());
            return;
        }
        if ((p[0] == 'Z' || p[0] == 'z') && p.size() > 3 && p[2] == ',') {
            const bool add = p[0] == 'Z';
            const int type = p[1] - '0';
            char* end = nullptr;
            int a = (int)(std::strtoul(p.c_str() + 3, &end, 16) & 0xffff);
            int len = end && *end == ',' ? (int)std::min(std::strtoul(end + 1, nullptr, 16), 0x10000UL) : 1;
            if (type == 0 || type == 1) {
                if (add) {
                    GuiBreakpoint bp;
                    bp.address = a;
                    bp.remoteTemp = true;
                    dbg.breakpoints.push_back(bp);
                } else {
                    auto& bps = dbg.breakpoints;
                    for (size_t i = 0; i < bps.size(); i++)
                        if (bps[i].remoteTemp && (bps[i].address & 0xffff) == a) { bps.erase(bps.begin() + (long)i); break; }
                }
                dbg.breakpointsChanged();
                reply("OK");
                return;
            }
            if (type >= 2 && type <= 4) {
                const int last = std::min(a + std::max(len, 1) - 1, 0xffff);   // a range never wraps
                const bool rd = type != 2, wr = type != 3;
                if (add) {
                    GuiWatchpoint w;
                    w.start = a;
                    w.end = last;
                    w.onRead = rd;
                    w.onWrite = wr;
                    w.remote = true;
                    dbg.watchpoints.push_back(w);
                } else {
                    auto& wps = dbg.watchpoints;
                    for (size_t i = 0; i < wps.size(); i++)
                        if (wps[i].remote && wps[i].start == a && wps[i].onRead == rd && wps[i].onWrite == wr) { wps.erase(wps.begin() + (long)i); break; }
                }
                reply("OK");
                return;
            }
            reply("");
            return;
        }
        if (p == "D" || p.rfind("D;", 0) == 0) { reply("OK"); flushGdb(); gdbDisconnect("detached"); return; }
        if (p == "k") { gdbDisconnect("killed the session"); return; }
        reply("");   // not supported: the client falls back or does without
    }

    void flushGdb() {
        if (gdb.client == net::NO_SOCKET || gdb.out.empty()) return;
        if (!net::sendAll(gdb.client, gdb.out)) { gdb.out.clear(); gdbDisconnect("lost"); return; }
        gdb.out.clear();
    }

    void pollGdb() {
        if (gdb.listener == net::NO_SOCKET) return;
        // A new connection waits as the candidate until its first bytes show it is not an
        // HTTP request (httpRequestStart); only then does it take over from the client --
        // so a web page cannot even knock DeZog off.
        net::Socket c = net::acceptClient(gdb.listener);
        if (c != net::NO_SOCKET) {
            if (gdb.candidate != net::NO_SOCKET) net::closeSocket(gdb.candidate);
            gdb.candidate = c;
            gdb.candidateIn.clear();
        }
        char buf[4096];
        if (gdb.candidate != net::NO_SOCKET) {
            int n = 0;
            while (gdb.candidateIn.size() < 4096 && (n = net::receiveSome(gdb.candidate, buf, sizeof(buf))) > 0)
                gdb.candidateIn.append(buf, (size_t)n);
            const int http = n < 0 ? 1 : httpRequestStart(gdb.candidateIn);   // gone again: dropped too
            if (http > 0) {
                net::closeSocket(gdb.candidate);
                gdb.candidate = net::NO_SOCKET;
                if (n >= 0) self.lastEvent = "GDB server: refused an HTTP request (a web page?)";
            } else if (http == 0) {
                if (gdb.client != net::NO_SOCKET) gdbDisconnect("replaced by a new connection");
                gdb.client = gdb.candidate;
                gdb.in = gdb.candidateIn;
                gdb.candidate = net::NO_SOCKET;
                gdb.candidateIn.clear();
                gdb.noAck = false;
                gdb.running = false;
                // A debugger that attaches finds the machine stopped, as MAME's does.
                if (machine(host)) dbg.pause();
                self.lastEvent = "GDB client connected (port " + std::to_string(gdb.port) + ")";
                host.status = self.lastEvent;
            }
        }
        if (gdb.client == net::NO_SOCKET) return;
        for (;;) {
            int n = net::receiveSome(gdb.client, buf, sizeof(buf));
            if (n < 0) { gdbDisconnect("disconnected"); return; }
            if (n == 0) break;
            gdb.in.append(buf, (size_t)n);
            if (gdb.in.size() > (1u << 22)) { gdbDisconnect("sent 4 MB that is not a packet"); return; }
        }
        while (!gdb.in.empty() && gdb.client != net::NO_SOCKET) {
            char c0 = gdb.in[0];
            if (c0 == '+' || c0 == '-') { gdb.in.erase(0, 1); continue; }
            if (c0 == 0x03) {
                gdb.in.erase(0, 1);
                if (gdb.running) {
                    dbg.pause();
                    gdb.running = false;
                    gdbPacket(stopReply());
                }
                continue;
            }
            if (c0 != '$') { gdb.in.erase(0, 1); continue; }
            size_t hash = gdb.in.find('#');
            if (hash == std::string::npos || hash + 2 >= gdb.in.size()) break;   // the rest is still coming
            std::string data = gdb.in.substr(1, hash - 1);
            const int sumHi = hexDigit(gdb.in[hash + 1]), sumLo = hexDigit(gdb.in[hash + 2]);
            const int want = sumHi < 0 || sumLo < 0 ? -1 : sumHi << 4 | sumLo;
            gdb.in.erase(0, hash + 3);
            unsigned sum = 0;
            for (unsigned char ch : data) sum += ch;
            if (!gdb.noAck) {
                if ((int)(sum & 0xff) != want) { gdb.out += "-"; continue; }
                gdb.out += "+";
            }
            gdbHandle(data);
        }
        // A stop the client is waiting for: a breakpoint, a watchpoint, a step's end, or
        // the UI's Pause.
        if (gdb.client != net::NO_SOCKET && gdb.running && paused()) {
            gdb.running = false;
            gdbPacket(stopReply());
        }
        flushGdb();
    }

    // ================================================================ API
    json::Value regsJson(GX4000* e) {
        json::Value r = json::Value::object();
        for (const char* n : { "pc", "sp", "af", "bc", "de", "hl", "ix", "iy", "af2", "bc2", "de2", "hl2",
                               "a", "f", "b", "c", "d", "e", "h", "l", "i", "r", "im", "iff1", "iff2", "halt" }) {
            int v = 0;
            getRegister(e, n, v);
            r.set(n, v);
        }
        std::string flags = "SZ5H3PNC";
        for (int b = 0; b < 8; b++) if (!(e->cpu->f & (0x80 >> b))) flags[(size_t)b] = '-';
        r.set("flags", flags);
        return r;
    }

    json::Value statusJson() {
        json::Value r = json::Value::object();
        GX4000* e = machine(host);
        r.set("booted", e != nullptr);
        if (host.currentModel >= 0 && host.currentModel < (int)host.models.size()) r.set("model", host.models[(size_t)host.currentModel].id);
        r.set("paused", paused());
        if (e) r.set("pc", e->cpu->pc & 0xffff);
        r.set("frames", (long long)host.framesRun);
        if (!host.breakReason.empty()) r.set("reason", host.breakReason);
        r.set("status", host.status);
        r.set("typing", typing());
        if (watching) { r.set("watching", watchRequest.path); r.set("reloads", reloadCount); }
        r.set("gdb", gdb.listener != net::NO_SOCKET ? (gdb.client != net::NO_SOCKET ? "connected" : "listening") : "off");
        return r;
    }

    json::Value bpJson(size_t index, const GuiBreakpoint& bp) {
        json::Value b = json::Value::object();
        b.set("index", (int)index);
        b.set("addr", bp.address & 0xffff);
        b.set("enabled", bp.enabled);
        b.set("hits", bp.hits);
        if (!bp.condition.empty()) b.set("condition", bp.condition);
        if (!bp.error.empty()) b.set("error", bp.error);
        if (bp.remoteId || bp.remoteTemp) b.set("gdb", true);
        if (bp.fromAssembler) b.set("assembler", true);
        return b;
    }

    LoadRequest loadRequestFrom(const json::Value& req) {
        LoadRequest r;
        r.path = req.str("path");
        r.address = parseAddr(req, "addr", -1);
        const json::Value* run = req.find("run");
        if (run) {
            if (run->type == json::Value::Bool) r.run = run->boolean ? LoadRequest::RUN_ENTRY : LoadRequest::NO_RUN;
            else if (run->type == json::Value::String && (lower(run->text) == "entry" || lower(run->text) == "true" || lower(run->text) == "yes"))
                r.run = LoadRequest::RUN_ENTRY;
            else if (run->type == json::Value::String && (lower(run->text) == "false" || lower(run->text) == "no" || run->text.empty()))
                r.run = LoadRequest::NO_RUN;
            else r.run = parseAddr(req, "run", LoadRequest::NO_RUN);
        }
        r.unit = (int)parseCount(req, "unit", 0);
        r.reset = req.flag("reset");
        r.command = req.str("command");
        r.symbols = req.str("symbols");
        return r;
    }

    // One request; true when `res` is the answer now, false when it is deferred.
    bool apiHandle(ApiClient& client, const json::Value& req, json::Value& res) {
        const std::string cmd = lower(req.str("cmd"));
        GX4000* e = machine(host);
        auto need = [&]() { if (!e) throw std::runtime_error("no machine is running"); };
        auto defer = [&](const std::string& kind, uint64_t frames, long timeoutMs) {
            Waiting w;
            if (const json::Value* id = req.find("id")) { w.id = *id; w.hasId = true; }
            w.kind = kind;
            w.untilFrame = host.framesRun + frames;
            w.deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
            client.waiting.push_back(w);
            return false;
        };

        if (cmd == "ping") { res.set("name", "CPCSyntaxError"); return true; }
        if (cmd == "status") { json::Value s = statusJson(); for (auto& f : s.fields) res.set(f.first, f.second); return true; }
        if (cmd == "pause") { need(); dbg.pause(); res.set("pc", e->cpu->pc & 0xffff); return true; }
        if (cmd == "run" || cmd == "continue") {
            need();
            int a = parseAddr(req, "addr", -1);
            if (a >= 0) { dbg.setPc(a); resumeFresh(); } else dbg.run();
            return true;
        }
        if (cmd == "step") {
            need();
            long n = std::clamp(parseCount(req, "count", 1), 1L, 1000000L);
            // All but the last without a redraw each (a picture per instruction is slow);
            // a watchpoint ends it early.
            dbg.pause();
            host.breakReason.clear();
            dbg.lastWatchAddress = -1;
            for (long k = 0; k + 1 < n && host.breakReason.empty(); k++) host.stepInstruction(false);
            if (host.breakReason.empty()) dbg.stepInto();
            else { host.render(); host.status = host.breakReason; }
            res.set("pc", e->cpu->pc & 0xffff);
            res.set("regs", regsJson(e));
            return true;
        }
        if (cmd == "over" || cmd == "out" || cmd == "runto") {
            need();
            if (cmd == "over") dbg.stepOver();
            else if (cmd == "out") dbg.stepOut();
            else {
                int a = parseAddr(req, "addr", -1);
                if (a < 0) throw std::runtime_error("runto needs an address");
                dbg.runTo(a);
            }
            if (paused()) { res.set("pc", e->cpu->pc & 0xffff); return true; }   // a step over a plain instruction
            return defer("stop", 0, (long)parseCount(req, "timeout", 10000));
        }
        if (cmd == "reset") { need(); host.reset(); if (req.flag("run", true) && host.paused) resumeFresh(); return true; }
        if (cmd == "regs") { need(); res.set("regs", regsJson(e)); return true; }
        if (cmd == "setreg") {
            need();
            if (const json::Value* many = req.find("regs"); many && many->isObject()) {
                for (const auto& f : many->fields) {
                    json::Value one = json::Value::object();
                    one.set("v", f.second);
                    if (!setRegister(e, f.first, parseAddr(one, "v", 0))) throw std::runtime_error("unknown register " + f.first);
                }
            } else {
                std::string reg = req.str("reg");
                if (!setRegister(e, reg, parseAddr(req, "value", 0))) throw std::runtime_error("unknown register '" + reg + "'");
            }
            res.set("regs", regsJson(e));
            return true;
        }
        if (cmd == "read") {
            need();
            int a = parseAddr(req, "addr", -1);
            if (a < 0) throw std::runtime_error("read needs an address");
            long n = std::clamp(parseCount(req, "len", 16), 0L, 0x10000L);
            std::string hex;
            json::Value bytes = json::Value::array();
            const bool asArray = req.str("format") == "bytes";
            for (long k = 0; k < n; k++) {
                int v = e->memory->readMapped((int)((a + k) & 0xffff));
                if (asArray) bytes.push(v); else hex += hex2(v);
            }
            res.set("addr", a);
            if (asArray) res.set("bytes", bytes); else res.set("data", hex);
            return true;
        }
        if (cmd == "write") {
            need();
            int a = parseAddr(req, "addr", -1);
            if (a < 0) throw std::runtime_error("write needs an address");
            std::vector<int> bytes;
            const json::Value* d = req.find("data");
            if (!d) throw std::runtime_error("write needs data");
            if (d->type == json::Value::Array) {
                for (const auto& b : d->items) {
                    if (b.type != json::Value::Number || b.number < 0 || b.number > 255 || b.number != (int)b.number)
                        throw std::runtime_error("data: bytes are whole numbers 0-255");
                    bytes.push_back((int)b.number);
                }
            } else if (d->type != json::Value::String) {
                throw std::runtime_error("data: hex text or an array of bytes");
            } else {
                std::string h;
                for (char c : d->text) if (hexDigit(c) >= 0) h += c; else if (c != ' ' && c != ',') throw std::runtime_error("data: not hex");
                if (h.size() % 2) throw std::runtime_error("data: an odd number of hex digits");
                for (size_t k = 0; k < h.size(); k += 2) bytes.push_back(hexDigit(h[k]) << 4 | hexDigit(h[k + 1]));
            }
            for (size_t k = 0; k < bytes.size(); k++) e->memory->write((int)((a + k) & 0xffff), bytes[k]);
            res.set("written", (int)bytes.size());
            return true;
        }
        if (cmd == "load") {
            std::string err;
            if (!load(loadRequestFrom(req), err)) throw std::runtime_error(err);
            res.set("status", host.status);
            if (req.flag("wait") && typing()) return defer("typed", 0, (long)parseCount(req, "timeout", 60000));
            return true;
        }
        if (cmd == "watch") {
            LoadRequest r = loadRequestFrom(req);
            if (r.path.empty()) { self.watch(r); res.set("watching", false); return true; }
            std::error_code ec;
            if (!std::filesystem::exists(r.path, ec)) throw std::runtime_error("no such file: " + r.path);
            self.watch(r);
            res.set("watching", r.path);
            return true;
        }
        if (cmd == "unwatch") { self.watch(LoadRequest{}); return true; }
        if (cmd == "symbols") {
            std::string err;
            int n = loadSymbols(req.str("path"), err);
            if (n < 0) throw std::runtime_error(err);
            res.set("symbols", n);
            return true;
        }
        if (cmd == "type") {
            need();
            queueText(req.str("text"));
            if (req.flag("wait")) return defer("typed", 0, (long)parseCount(req, "timeout", 60000));
            return true;
        }
        if (cmd == "key") {
            need();
            std::string code = req.str("key");
            if (code.empty()) throw std::runtime_error("key needs a key code (Space, Enter, KeyA, Digit1, F1, ...)");
            keys.push_back(Key{ code, req.flag("shift") });
            return true;
        }
        if (cmd == "screenshot") {
            need();
            std::string path = req.str("path", "cpcse-shot.png");
            std::string ext = lower(std::filesystem::path(path).extension().string());
            host.render();
            bool ok;
            if (ext == ".bmp") ok = host.saveScreenshotBmp(path);
            else {
                const int w = host.video->width, h = host.video->height;
                std::vector<uint8_t> rgb((size_t)w * h * 3);
                for (size_t k = 0; k < (size_t)w * h; k++) {
                    uint32_t px = host.video->pixels[k];   // 0xAABBGGRR
                    rgb[k * 3] = (uint8_t)px; rgb[k * 3 + 1] = (uint8_t)(px >> 8); rgb[k * 3 + 2] = (uint8_t)(px >> 16);
                }
                ok = writePng(path, rgb.data(), w, h);
            }
            if (!ok) throw std::runtime_error("could not write " + path);
            res.set("path", std::filesystem::absolute(path).string());
            return true;
        }
        if (cmd == "bp") {
            std::string action = lower(req.str("action", "list"));
            auto& bps = dbg.breakpoints;
            if (action == "add") {
                int a = parseAddr(req, "addr", -1);
                if (a < 0) throw std::runtime_error("bp add needs an address");
                dbg.addBreakpoint(a, req.str("condition"));
                if (!bps.back().error.empty()) { std::string err = bps.back().error; dbg.removeBreakpoint(bps.size() - 1); throw std::runtime_error("condition: " + err); }
                res.set("breakpoint", bpJson(bps.size() - 1, bps.back()));
                return true;
            }
            if (action == "remove" || action == "delete" || action == "del") {
                int a = parseAddr(req, "addr", -1);
                if (a < 0) throw std::runtime_error("bp remove needs an address");
                int removed = 0;
                for (size_t i = bps.size(); i-- > 0;) if ((bps[i].address & 0xffff) == a && !bps[i].remoteTemp && !bps[i].remoteId) { dbg.removeBreakpoint(i); removed++; }
                if (!removed) throw std::runtime_error("no breakpoint there");
                res.set("removed", removed);
                return true;
            }
            if (action == "clear") {
                for (size_t i = bps.size(); i-- > 0;) if (!bps[i].remoteTemp && !bps[i].remoteId) dbg.removeBreakpoint(i);
                return true;
            }
            if (action != "list") throw std::runtime_error("bp: add, remove, clear or list");
            json::Value list = json::Value::array();
            for (size_t i = 0; i < bps.size(); i++) list.push(bpJson(i, bps[i]));
            res.set("breakpoints", list);
            return true;
        }
        if (cmd == "wp") {
            std::string action = lower(req.str("action", "list"));
            auto& wps = dbg.watchpoints;
            if (action == "add") {
                int a = parseAddr(req, "start", parseAddr(req, "addr", -1));
                if (a < 0) throw std::runtime_error("wp add needs an address");
                long last = req.has("end") ? parseAddr(req, "end", a) : a + std::max(1L, parseCount(req, "len", 1)) - 1;
                if (last < a) throw std::runtime_error("wp: the end is before the start");
                std::string mode = lower(req.str("mode", "w"));
                GuiWatchpoint w;
                w.start = a;
                w.end = (int)std::min(last, 0xffffL);   // a range never wraps
                w.onRead = mode.find('r') != std::string::npos;
                w.onWrite = mode.find('w') != std::string::npos;
                if (!w.onRead && !w.onWrite) throw std::runtime_error("mode: r, w or rw");
                wps.push_back(w);
                res.set("index", (int)wps.size() - 1);
                return true;
            }
            if (action == "remove" || action == "delete" || action == "del") {
                int a = parseAddr(req, "start", parseAddr(req, "addr", -1));
                size_t before = wps.size();
                wps.erase(std::remove_if(wps.begin(), wps.end(), [&](const GuiWatchpoint& w) { return !w.remote && w.start == a; }), wps.end());
                if (wps.size() == before) throw std::runtime_error("no watchpoint starts there");
                return true;
            }
            if (action == "clear") {
                wps.erase(std::remove_if(wps.begin(), wps.end(), [](const GuiWatchpoint& w) { return !w.remote; }), wps.end());
                return true;
            }
            if (action != "list") throw std::runtime_error("wp: add, remove, clear or list");
            json::Value list = json::Value::array();
            for (const auto& w : wps) {
                json::Value o = json::Value::object();
                o.set("start", w.start); o.set("end", w.end);
                o.set("mode", std::string(w.onRead ? "r" : "") + (w.onWrite ? "w" : ""));
                o.set("hits", w.hits); o.set("enabled", w.enabled);
                if (w.remote) o.set("gdb", true);
                list.push(o);
            }
            res.set("watchpoints", list);
            return true;
        }
        if (cmd == "disasm") {
            need();
            int a = parseAddr(req, "addr", e->cpu->pc & 0xffff);
            long n = std::clamp(parseCount(req, "count", 10), 1L, 1000L);
            json::Value lines = json::Value::array();
            auto read = [e](int x) { return e->memory->readMapped(x & 0xffff); };
            for (long k = 0; k < n; k++) {
                DisasmResult d = dbg.disassembler.disassemble(read, a);
                json::Value l = json::Value::object();
                l.set("addr", d.addr & 0xffff);
                std::string bytes;
                for (int b : d.bytes) bytes += hex2(b);
                l.set("bytes", bytes);
                l.set("text", d.mnem);
                auto lab = dbg.labelAt.find(d.addr & 0xffff);
                if (lab != dbg.labelAt.end()) l.set("label", lab->second);
                lines.push(l);
                a = d.next & 0xffff;
            }
            res.set("lines", lines);
            return true;
        }
        if (cmd == "wait") {
            std::string what = lower(req.str("for", "stop"));
            if (what == "stop") {
                if (paused()) { res.set("pc", e ? e->cpu->pc & 0xffff : 0); if (!host.breakReason.empty()) res.set("reason", host.breakReason); return true; }
                return defer("stop", 0, (long)parseCount(req, "n", parseCount(req, "timeout", 10000)));
            }
            if (what == "frames") {
                need();
                long n = std::max(0L, parseCount(req, "n", 50));
                if (paused()) throw std::runtime_error("the machine is paused: no frames will pass");
                return defer("frames", (uint64_t)n, (long)parseCount(req, "timeout", 600000));
            }
            if (what == "typed") {
                if (!typing()) return true;
                return defer("typed", 0, (long)parseCount(req, "timeout", 60000));
            }
            throw std::runtime_error("wait for: stop, frames or typed");
        }
        if (cmd == "frames") {   // run n frames and come back: "frames 50"
            json::Value w = req;
            w.set("cmd", "wait");
            w.set("for", "frames");
            return apiHandle(client, w, res);
        }
        if (cmd == "quit") {
            if (self.onQuit) self.onQuit();
            return true;
        }
        if (cmd == "help") {
            res.set("commands", "ping status pause run step over out runto reset regs setreg read write load watch unwatch "
                                "symbols type key screenshot bp wp disasm wait frames quit");
            return true;
        }
        throw std::runtime_error("unknown command '" + cmd + "' (try help)");
    }

    void apiReply(ApiClient& c, const json::Value& res) {
        if (c.sock == net::NO_SOCKET) return;
        if (!net::sendAll(c.sock, res.dump() + "\n")) c.closing = true;
    }

    void apiLine(ApiClient& c, const std::string& raw) {
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) return;
        json::Value res = json::Value::object();
        json::Value req;
        try {
            if (line[first] == '{') req = json::parse(line);
            else if (lower(line.substr(first, 5)) == "type " || lower(line.substr(first)) == "type") {
                // The text, exactly as written after "type " -- quotes and all: PRINT "HI"
                const std::string typed = line.size() > first + 5 ? line.substr(first + 5) : "";
                std::string text;
                for (size_t k = 0; k < typed.size(); k++) {
                    if (typed[k] == '\\' && k + 1 < typed.size() && typed[k + 1] == 'n') { text += '\n'; k++; }
                    else text += typed[k];
                }
                req = json::Value::object();
                req.set("cmd", "type");
                req.set("text", text);
            }
            else req = devCommandFromWords(devSplitWords(line));
            if (!req.isObject()) throw std::runtime_error("a request is a JSON object");
            if (const json::Value* id = req.find("id")) res.set("id", *id);
            res.set("ok", true);
            if (!apiHandle(c, req, res)) return;   // answered later
        } catch (const std::exception& ex) {
            res.set("ok", false);
            res.set("error", ex.what());
        }
        apiReply(c, res);
    }

    void pollApi() {
        if (apiListener == net::NO_SOCKET) return;
        for (;;) {
            net::Socket s = net::acceptClient(apiListener);
            if (s == net::NO_SOCKET) break;
            ApiClient c;
            c.sock = s;
            apiClients.push_back(c);
        }
        for (size_t ci = 0; ci < apiClients.size(); ci++) {
            ApiClient& c = apiClients[ci];
            char buf[4096];
            for (;;) {
                int n = net::receiveSome(c.sock, buf, sizeof(buf));
                if (n < 0) { c.closing = true; break; }
                if (n == 0) break;
                c.in.append(buf, (size_t)n);
                if (c.in.size() > (1u << 22)) { c.closing = true; break; }   // 4 MB without a newline
            }
            if (!c.vetted && !c.closing) {
                const int http = httpRequestStart(c.in);
                if (http > 0) { c.closing = true; self.lastEvent = "Command API: refused an HTTP request (a web page?)"; }
                else if (http == 0) c.vetted = true;
                else continue;   // too little to tell yet
            }
            size_t nl;
            while (!c.closing && (nl = c.in.find('\n')) != std::string::npos) {
                std::string line = c.in.substr(0, nl);
                c.in.erase(0, nl + 1);
                apiLine(apiClients[ci], line);
            }
            // Deferred answers whose moment has come.
            ApiClient& cc = apiClients[ci];
            const Clock::time_point now = Clock::now();
            for (size_t k = 0; k < cc.waiting.size();) {
                Waiting& w = cc.waiting[k];
                bool done = false, timedOut = now >= w.deadline;
                if (w.kind == "stop") done = paused();
                else if (w.kind == "frames") done = host.framesRun >= w.untilFrame;
                else if (w.kind == "typed") done = !typing();
                // Frames and keys need the machine running: a breakpoint ends the wait.
                const bool stopped = !done && w.kind != "stop" && paused();
                if (!done && !timedOut && !stopped) { k++; continue; }
                json::Value res = json::Value::object();
                if (w.hasId) res.set("id", w.id);
                res.set("ok", done);
                if (stopped) res.set("error", "the machine stopped first" + (host.breakReason.empty() ? std::string() : ": " + host.breakReason));
                else if (!done) res.set("error", "timed out waiting for " + w.kind);
                GX4000* e = machine(host);
                if (e) res.set("pc", e->cpu->pc & 0xffff);
                if (done && w.kind == "stop" && !host.breakReason.empty()) res.set("reason", host.breakReason);
                if (w.kind == "frames") res.set("frames", (long long)host.framesRun);
                cc.waiting.erase(cc.waiting.begin() + (long)k);
                apiReply(cc, res);
            }
        }
        for (size_t ci = apiClients.size(); ci-- > 0;)
            if (apiClients[ci].closing) { net::closeSocket(apiClients[ci].sock); apiClients.erase(apiClients.begin() + (long)ci); }
    }
};

// ==================================================================== DevServer
DevServer::DevServer(EmuHost& host, Debugger& debugger) : impl(std::make_unique<Impl>(host, debugger, *this)) {}

DevServer::~DevServer() {
    stopGdb();
    stopApi();
}

void DevServer::poll() {
    impl->pollGdb();
    impl->pollApi();
    impl->tickLoads();
    impl->tickKeys();
    // Breakpoints and watchpoints set just now reach the core before the frames run: the
    // shell's own attach() comes only with the UI, after them, and a client's step-over
    // breakpoint followed at once by continue would be run past.
    impl->dbg.attach();
}

bool DevServer::startGdb(int port) {
    stopGdb();
    gdbError.clear();
    impl->gdb.listener = net::listenLocal(port, gdbError);
    if (impl->gdb.listener == net::NO_SOCKET) return false;
    impl->gdb.port = net::localPort(impl->gdb.listener);   // what port 0 became
    lastEvent = "GDB server listening on port " + std::to_string(impl->gdb.port);
    return true;
}

void DevServer::stopGdb() {
    impl->gdbDisconnect("closed");
    if (impl->gdb.candidate != net::NO_SOCKET) net::closeSocket(impl->gdb.candidate);
    impl->gdb.candidate = net::NO_SOCKET;
    impl->gdb.candidateIn.clear();
    if (impl->gdb.listener != net::NO_SOCKET) net::closeSocket(impl->gdb.listener);
    impl->gdb.listener = net::NO_SOCKET;
}

bool DevServer::gdbListening() const { return impl->gdb.listener != net::NO_SOCKET; }
bool DevServer::gdbConnected() const { return impl->gdb.client != net::NO_SOCKET; }
int DevServer::gdbPort() const { return impl->gdb.port; }

bool DevServer::startApi(int port) {
    stopApi();
    apiError.clear();
    impl->apiListener = net::listenLocal(port, apiError);
    if (impl->apiListener == net::NO_SOCKET) return false;
    impl->apiPortNow = net::localPort(impl->apiListener);
    lastEvent = "Command API listening on port " + std::to_string(impl->apiPortNow);
    return true;
}

void DevServer::stopApi() {
    for (auto& c : impl->apiClients) net::closeSocket(c.sock);
    impl->apiClients.clear();
    if (impl->apiListener != net::NO_SOCKET) net::closeSocket(impl->apiListener);
    impl->apiListener = net::NO_SOCKET;
}

bool DevServer::apiListening() const { return impl->apiListener != net::NO_SOCKET; }
int DevServer::apiClients() const { return (int)impl->apiClients.size(); }
int DevServer::apiPort() const { return impl->apiPortNow; }


void DevServer::loadWhenReady(const LoadRequest& request) {
    impl->pending = request;
    impl->pendingAt = Impl::FIRMWARE_FRAMES;   // counted from the start: the command line's --load
    impl->pendingLoad = true;
}

void DevServer::watch(const LoadRequest& request) {
    impl->watching = !request.path.empty();
    impl->watchRequest = request;
    impl->stampKnown = false;     // the first look counts as a change: load it once now
    impl->changeSeen = false;
    impl->reloadCount = 0;
    impl->lastCheck = {};
    lastEvent = impl->watching ? "Watching " + fileName(request.path) : "Not watching a file";
}

const LoadRequest* DevServer::watched() const { return impl->watching ? &impl->watchRequest : nullptr; }
int DevServer::reloads() const { return impl->reloadCount; }

int DevServer::loadSymbols(const std::string& path, std::string& error) { return impl->loadSymbols(path, error); }


void DevServer::loadSettings(const std::map<std::string, std::string>& ini) {
    auto num = [&](const char* k, int d) { auto i = ini.find(k); return i == ini.end() ? d : std::atoi(i->second.c_str()); };
    gdbAtStart = num("gdb_server", 0) != 0;
    gdbPortSetting = std::clamp(num("gdb_port", 12000), 1, 65535);
    apiAtStart = num("api_server", 0) != 0;
    apiPortSetting = std::clamp(num("api_port", 6128), 1, 65535);
}

void DevServer::saveSettings(std::ostream& out) const {
    out << "gdb_server=" << (gdbAtStart ? 1 : 0) << "\n" << "gdb_port=" << gdbPortSetting << "\n"
        << "api_server=" << (apiAtStart ? 1 : 0) << "\n" << "api_port=" << apiPortSetting << "\n";
}

void DevServer::startFromSettings() {
    if (gdbAtStart && !gdbListening()) startGdb(gdbPortSetting);
    if (apiAtStart && !apiListening()) startApi(apiPortSetting);
}

// ==================================================================== command line
int DevServer::address(const std::string& text) const { return impl->dbg.parseAddress(text); }

bool applyDevArguments(DevServer& dev, int argc, char** argv, std::string& error) {
    LoadRequest load, watch;
    bool haveLoad = false, haveWatch = false, reset = false;
    std::string run, command, symbols;
    // file@addr; a Windows path keeps its colon, and an '@' that is part of a name stays.
    auto split = [](const std::string& arg, LoadRequest& r, std::string& addrText) {
        r.path = arg;
        size_t at = arg.rfind('@');
        if (at == std::string::npos || at == 0 || at + 1 >= arg.size()) return;
        const std::string tail = arg.substr(at + 1);
        if (tail.find_first_of("/\\") != std::string::npos) return;   // a folder named x@y
        r.path = arg.substr(0, at);
        addrText = tail;
    };
    std::string loadAt, watchAt;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&](const char* name, std::string& out) {
            if (i + 1 >= argc) { error = std::string(name) + " needs a value"; return false; }
            out = argv[++i];
            return true;
        };
        // "--gdb=12001": a port 1-65535; anything else is a mistake to report, not 0 (which
        // would have the system pick one nobody knows to connect to).
        auto port = [&](int def) {
            size_t eq = a.find('=');
            if (eq == std::string::npos) return def;
            const std::string digits = a.substr(eq + 1);
            if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
            const int p = std::atoi(digits.c_str());
            return p >= 1 && p <= 65535 ? p : -1;
        };
        std::string v;
        if (a == "--gdb" || a.rfind("--gdb=", 0) == 0) {
            const int p = port(dev.gdbPortSetting);
            if (p < 0) { error = "--gdb: '" + a.substr(6) + "' is not a port (1-65535)"; return false; }
            if (!dev.startGdb(p)) { error = "--gdb: " + dev.gdbError; return false; }
        } else if (a == "--api" || a.rfind("--api=", 0) == 0) {
            const int p = port(dev.apiPortSetting);
            if (p < 0) { error = "--api: '" + a.substr(6) + "' is not a port (1-65535)"; return false; }
            if (!dev.startApi(p)) { error = "--api: " + dev.apiError; return false; }
        } else if (a == "--load") {
            if (!value("--load", v)) return false;
            split(v, load, loadAt);
            haveLoad = true;
        } else if (a == "--watch") {
            if (!value("--watch", v)) return false;
            split(v, watch, watchAt);
            haveWatch = true;
        } else if (a == "--run") {
            if (!value("--run", run)) return false;
        } else if (a == "--reset") {
            reset = true;
        } else if (a == "--command") {
            if (!value("--command", v)) return false;
            command.clear();
            for (size_t k = 0; k < v.size(); k++) {
                if (v[k] == '\\' && k + 1 < v.size() && v[k + 1] == 'n') { command += '\n'; k++; }
                else command += v[k];
            }
        } else if (a == "--symbols") {
            if (!value("--symbols", symbols)) return false;
        }
    }
    // The symbols first: --run and file@addr may name a label.
    if (!symbols.empty() && dev.loadSymbols(symbols, error) < 0) { error = "--symbols: " + error; return false; }
    auto finish = [&](LoadRequest& r, const std::string& addrText, const char* flag) {
        if (!addrText.empty()) {
            r.address = dev.address(addrText);
            if (r.address < 0) { error = std::string(flag) + ": '" + addrText + "' is not an address or a label"; return false; }
        }
        if (!run.empty()) {
            std::string low = lower(run);
            if (low == "entry") r.run = LoadRequest::RUN_ENTRY;
            else {
                r.run = dev.address(run);
                if (r.run < 0) { error = "--run: 'entry', an address or a label, not '" + run + "'"; return false; }
            }
        }
        r.reset = reset;
        r.command = command;
        r.symbols = symbols;   // read again with every reload: a rebuild moves the labels
        return true;
    };
    if (haveLoad) { if (!finish(load, loadAt, "--load")) return false; load.symbols.clear(); dev.loadWhenReady(load); }
    if (haveWatch) { if (!finish(watch, watchAt, "--watch")) return false; dev.watch(watch); }
    return true;
}

} // namespace cpcse
