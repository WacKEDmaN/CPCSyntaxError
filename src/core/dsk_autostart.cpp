// CPCSyntaxError — Per-disk autostart registry.
#include "dsk_autostart.h"
#include <chrono>
#include <map>
#include <stdexcept>

namespace cpcse {

const std::string DSK_AUTOSTART_STORAGE_KEY = "cpcse_dsk_autostart_v1";

static long long nowMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
static std::string trimStr(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }
static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "%08x", v); return b; }

// Math.imul: 32-bit signed multiply, low 32 bits.
static uint32_t imul(uint32_t a, uint32_t b) { return a * b; }

static std::string fallbackHash(const Bytes& bytes) {
    uint32_t a = 0x811c9dc5u, b = 0x9e3779b9u;
    for (size_t i = 0; i < bytes.size(); i += 1) {
        a = imul(a ^ bytes[i], 0x01000193u);
        b = imul(b ^ (uint32_t)(bytes[i] + (uint32_t)i), 0x85ebca6bu);
    }
    return "fnv-" + hex8(a) + hex8(b) + "-" + std::to_string(bytes.size());
}

std::string diskFingerprint(const Bytes& input) {
    // Headless: no crypto.subtle, so the deterministic fallback hash is used.
    return fallbackHash(input);
}

// --- minimal JSON for the registry: { "<fp>": {command,name,drive,savedAt} } ---
static std::string jsonEscape(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    o += "\"";
    return o;
}
struct Rec { std::string command, name; int drive; long long savedAt; };
static std::string serializeRegistry(const std::map<std::string, Rec>& reg) {
    std::string out = "{"; bool first = true;
    for (auto& kv : reg) {
        if (!first) out += ",";
        first = false;
        out += jsonEscape(kv.first) + ":{\"command\":" + jsonEscape(kv.second.command) + ",\"name\":" + jsonEscape(kv.second.name)
            + ",\"drive\":" + std::to_string(kv.second.drive) + ",\"savedAt\":" + std::to_string(kv.second.savedAt) + "}";
    }
    out += "}";
    return out;
}
// Tolerant parser for the above shape.
static std::map<std::string, Rec> parseRegistry(const std::string& text) {
    std::map<std::string, Rec> reg;
    size_t i = 0; auto skip = [&]() { while (i < text.size() && std::isspace((unsigned char)text[i])) i++; };
    auto parseStr = [&](std::string& out) -> bool {
        skip(); if (i >= text.size() || text[i] != '"') return false; i++;
        out.clear();
        while (i < text.size() && text[i] != '"') {
            char c = text[i++];
            if (c == '\\' && i < text.size()) { char e = text[i++]; out += (e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e); }
            else out += c;
        }
        if (i < text.size()) i++;
        return true;
    };
    auto parseNum = [&]() -> long long { skip(); size_t s = i; while (i < text.size() && (std::isdigit((unsigned char)text[i]) || text[i] == '-')) i++; try { return std::stoll(text.substr(s, i - s)); } catch (...) { return 0; } };
    skip(); if (i >= text.size() || text[i] != '{') return reg; i++;
    skip(); if (i < text.size() && text[i] == '}') return reg;
    while (i < text.size()) {
        std::string key; if (!parseStr(key)) break;
        skip(); if (i < text.size() && text[i] == ':') i++;
        skip(); if (i >= text.size() || text[i] != '{') break; i++;
        Rec r{ "", "", 0, 0 };
        while (i < text.size() && text[i] != '}') {
            std::string field; if (!parseStr(field)) break;
            skip(); if (i < text.size() && text[i] == ':') i++;
            if (field == "command") parseStr(r.command);
            else if (field == "name") parseStr(r.name);
            else if (field == "drive") r.drive = (int)parseNum();
            else if (field == "savedAt") r.savedAt = parseNum();
            skip(); if (i < text.size() && text[i] == ',') i++;
        }
        if (i < text.size() && text[i] == '}') i++;
        reg[key] = r;
        skip(); if (i < text.size() && text[i] == ',') { i++; continue; }
        break;
    }
    return reg;
}

static std::map<std::string, Rec> readRegistry(AutostartStorage* storage) {
    if (!storage) return {};
    return parseRegistry(storage->getItem(DSK_AUTOSTART_STORAGE_KEY));
}
static bool writeRegistry(const std::map<std::string, Rec>& reg, AutostartStorage* storage) {
    if (!storage) return false;
    storage->setItem(DSK_AUTOSTART_STORAGE_KEY, serializeRegistry(reg));
    return true;
}

AutostartRecord rememberDiskAutostart(const Bytes& input, const std::string& command, const std::string& name, int drive, AutostartStorage* storage) {
    std::string value = trimStr(command);
    if (value.empty()) throw std::runtime_error("Autostart command is empty");
    std::string fingerprint = diskFingerprint(input);
    std::map<std::string, Rec> registry = readRegistry(storage);
    Rec r; r.command = value.substr(0, 500); r.name = name.substr(0, std::min((size_t)255, name.size())); r.drive = drive == 1 ? 1 : 0; r.savedAt = nowMs();
    registry[fingerprint] = r;
    writeRegistry(registry, storage);
    return { fingerprint, r.command, r.name, r.drive, r.savedAt, true };
}
AutostartRecord diskAutostartCommand(const Bytes& input, AutostartStorage* storage) {
    std::string fingerprint = diskFingerprint(input);
    std::map<std::string, Rec> registry = readRegistry(storage);
    auto it = registry.find(fingerprint);
    if (it != registry.end() && !trimStr(it->second.command).empty())
        return { fingerprint, trimStr(it->second.command), it->second.name, it->second.drive, it->second.savedAt, true };
    return {};
}
bool forgetDiskAutostart(const Bytes& input, AutostartStorage* storage) {
    std::string fingerprint = diskFingerprint(input);
    std::map<std::string, Rec> registry = readRegistry(storage);
    bool existed = registry.count(fingerprint) != 0;
    if (existed) { registry.erase(fingerprint); writeRegistry(registry, storage); }
    return existed;
}

} // namespace cpcse
