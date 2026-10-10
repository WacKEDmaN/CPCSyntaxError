// CPCSyntaxError — cheats. See cheats.h.
#include "cheats.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace cpcse {

int CheatFinder::stored(const Bytes& ram, uint32_t address) const {
    if (address >= ram.size()) return -1;
    if (width == 1) return ram[address];
    if (address + 1 >= ram.size()) return -1;
    return ram[address] | ram[address + 1] << 8;
}

static int fromBcd(int v, int digits) {
    int out = 0, scale = 1;
    for (int d = 0; d < digits; d++) {
        const int nib = v >> (4 * d) & 15;
        if (nib > 9) return -1;
        out += nib * scale;
        scale *= 10;
    }
    return out;
}
static int toBcd(int v, int digits) {
    int out = 0;
    for (int d = 0; d < digits; d++) { out |= (v % 10) << (4 * d); v /= 10; }
    return out;
}

int CheatFinder::shown(const Bytes& ram, const Candidate& c) const {
    const int raw = stored(ram, c.address);
    if (raw < 0) return -1;
    switch (c.encoding) {
        case BINARY: return raw;
        case ONE_LESS: return raw + 1;
        default: return fromBcd(raw, width * 2);
    }
}

int CheatFinder::encode(const Candidate& c, int value) const {
    const int top = width == 1 ? 0xff : 0xffff;
    switch (c.encoding) {
        case BINARY: return std::clamp(value, 0, top);
        case ONE_LESS: return std::clamp(value - 1, 0, top);
        default: return toBcd(std::clamp(value, 0, width == 1 ? 99 : 9999), width * 2);
    }
}

void CheatFinder::start(const Bytes& ram, unsigned encodings) {
    candidates.clear();
    history.clear();
    previousHistory.clear();
    const uint32_t n = (uint32_t)ram.size() - (width == 2 ? 1 : 0);
    candidates.reserve((size_t)n * 3);
    for (uint32_t a = 0; a < n; a++)
        for (uint8_t e = 0; e < 3; e++) if (encodings & (1u << e)) candidates.push_back({ a, e });
    previous = ram;
    started = true;
}

void CheatFinder::step(const Bytes& ram) {
    history.push_back(candidates);
    previousHistory.push_back(previous);
    if (history.size() > 6) { history.erase(history.begin()); previousHistory.erase(previousHistory.begin()); }
    (void)ram;
}

void CheatFinder::keepEqual(const Bytes& ram, int value) {
    if (!started) start(ram);
    step(ram);
    std::vector<Candidate> kept;
    for (const Candidate& c : candidates) if (shown(ram, c) == value) kept.push_back(c);
    candidates.swap(kept);
    previous = ram;
}

void CheatFinder::keepChange(const Bytes& ram, Change change) {
    if (!started) { start(ram); return; }
    step(ram);
    std::vector<Candidate> kept;
    for (const Candidate& c : candidates) {
        const int now = shown(ram, c), was = shown(previous, c);
        if (now < 0 || was < 0) continue;
        const bool keep = change == CHANGED ? now != was : change == UNCHANGED ? now == was : change == INCREASED ? now > was : now < was;
        if (keep) kept.push_back(c);
    }
    candidates.swap(kept);
    previous = ram;
}

void CheatFinder::keepChangedBy(const Bytes& ram, int delta) {
    if (!started) { start(ram); return; }
    step(ram);
    std::vector<Candidate> kept;
    for (const Candidate& c : candidates) {
        const int now = shown(ram, c), was = shown(previous, c);
        if (now >= 0 && was >= 0 && now - was == delta) kept.push_back(c);
    }
    candidates.swap(kept);
    previous = ram;
}

bool CheatFinder::undo() {
    if (history.empty()) return false;
    candidates = history.back();
    previous = previousHistory.back();
    history.pop_back();
    previousHistory.pop_back();
    return true;
}

// ---------------------------------------------------------------- the list

void CheatList::poke(Bytes& ram, const Cheat& cheat) {
    for (const Poke& p : cheat.pokes) {
        if (p.address < ram.size()) ram[p.address] = (uint8_t)p.value;
        if (p.width == 2 && p.address + 1 < ram.size()) ram[p.address + 1] = (uint8_t)(p.value >> 8);
    }
}

void CheatList::apply(Bytes& ram) {
    for (const Cheat& c : cheats) if (c.enabled && c.freeze) poke(ram, c);
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
// "&1234", "#1234", "$1234", "0x1234" or decimal
static bool number(const std::string& in, long& out) {
    std::string s = trim(in);
    int base = 10;
    if (!s.empty() && (s[0] == '&' || s[0] == '#' || s[0] == '$')) { base = 16; s = s.substr(1); }
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
    if (s.empty() || s.size() > 8) return false;
    for (char c : s) if (!(base == 16 ? std::isxdigit((unsigned char)c) : std::isdigit((unsigned char)c))) return false;
    out = std::stol(s, nullptr, base);
    return true;
}

bool CheatList::parse(const std::string& text, std::vector<Cheat>& out, std::string& error) {
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    Cheat current;
    bool have = false;
    auto flush = [&]() { if (have && !current.pokes.empty()) out.push_back(current); current = Cheat{}; have = false; };
    while (std::getline(in, line)) {
        lineNo++;
        std::string t = trim(line);
        if (t.empty()) continue;
        if (t[0] == '[' && t.back() == ']') { flush(); current.name = trim(t.substr(1, t.size() - 2)); have = true; continue; }
        if (t[0] == '#') { flush(); current.name = trim(t.substr(1)); have = true; continue; }   // a name (or a comment)
        // a line of POKEs, perhaps several joined with ':' (as a BASIC line has them)
        std::vector<std::string> parts;
        { std::string part; std::istringstream ps(t); while (std::getline(ps, part, ':')) parts.push_back(part); }
        bool any = false;
        for (std::string part : parts) {
            std::string p = trim(part);
            std::string up = p;
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            if (up.rfind("REM", 0) == 0 || up.empty()) continue;
            int width = 1;
            size_t at = 0;
            if (up.rfind("POKE16", 0) == 0) { width = 2; at = 6; }
            else if (up.rfind("POKE", 0) == 0) at = 4;
            else { error = "line " + std::to_string(lineNo) + ": not a POKE: " + p; return false; }
            std::string rest = trim(p.substr(at));
            bool once = false;
            for (const char* flag : { " once", " ONCE", " w", " W" }) {
                const std::string f = flag;
                if (rest.size() > f.size() && rest.compare(rest.size() - f.size(), f.size(), f) == 0) {
                    if (f == " w" || f == " W") width = 2; else once = true;
                    rest = trim(rest.substr(0, rest.size() - f.size()));
                }
            }
            const size_t comma = rest.find(',');
            long address = 0, value = 0;
            if (comma == std::string::npos || !number(rest.substr(0, comma), address) || !number(rest.substr(comma + 1), value) || address < 0 ||
                value < 0 || value > (width == 2 ? 0xffff : 0xff)) {
                error = "line " + std::to_string(lineNo) + ": POKE wants an address and a value: " + p;
                return false;
            }
            if (!have) { current.name = "Cheat " + std::to_string(out.size() + 1); have = true; }
            current.pokes.push_back({ (uint32_t)address, (int)value, width });
            if (once) current.freeze = false;
            any = true;
        }
        (void)any;
    }
    flush();
    return true;
}

std::string CheatList::addressText(uint32_t address) {
    char b[48];
    if (address <= 0xffff) std::snprintf(b, sizeof b, "&%04X", address);
    else std::snprintf(b, sizeof b, "&%05X (page %u &%04X)", address, (address - 0x10000) / 0x4000 + 4, (address - 0x10000) % 0x4000);
    return b;
}

std::string CheatList::serialize(const std::vector<Cheat>& cheats) {
    std::ostringstream o;
    o << "# CPCSyntaxError cheats: [name] then POKE &address,value (\"once\": written once, not held;\n";
    o << "# POKE16 a 16-bit value; addresses past &FFFF are expansion RAM)\n";
    for (const Cheat& c : cheats) {
        o << "\n[" << c.name << "]\n";
        for (const Poke& p : c.pokes) {
            char b[64];
            std::snprintf(b, sizeof b, "%s &%04X,&%0*X%s\n", p.width == 2 ? "POKE16" : "POKE", p.address, p.width == 2 ? 4 : 2, p.value, c.freeze ? "" : " once");
            o << b;
        }
    }
    return o.str();
}

} // namespace cpcse
