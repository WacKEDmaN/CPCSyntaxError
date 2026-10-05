// CPCSyntaxError — Debug symbol and REMU metadata support.
#include "debug_symbols.h"
#include <regex>

namespace cpcse {

// std::regex (libstdc++) matches a repetition by recursing once a character: a line of a few
// thousand characters overflows the stack (measured: 5000 crash the program). No symbol
// line or REMU record is long, so a longer one -- a binary given as a symbol file -- is
// passed over before any expression sees it.
static constexpr size_t MAX_SYMBOL_LINE = 512;

static std::string trimStr(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }
static std::string upperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string lowerStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::tolower((unsigned char)c); return r; }

std::optional<int> parseNumeric(const std::string& valueIn, std::optional<int> fallback) {
    std::string raw = trimStr(valueIn);
    if (raw.empty()) return fallback;
    long number;
    try {
        if (std::regex_match(raw, std::regex("^&[0-9a-f]+$", std::regex::icase))) number = std::stol(raw.substr(1), nullptr, 16);
        else if (std::regex_match(raw, std::regex("^\\$[0-9a-f]+$", std::regex::icase))) number = std::stol(raw.substr(1), nullptr, 16);
        else if (std::regex_match(raw, std::regex("^#[0-9a-f]+$", std::regex::icase))) number = std::stol(raw.substr(1), nullptr, 16);
        else if (std::regex_match(raw, std::regex("^0x[0-9a-f]+$", std::regex::icase))) number = std::stol(raw.substr(2), nullptr, 16);
        else if (std::regex_match(raw, std::regex("^[0-9a-f]+h$", std::regex::icase))) number = std::stol(raw.substr(0, raw.size() - 1), nullptr, 16);
        else if (std::regex_match(raw, std::regex("^-?[0-9]+$"))) number = std::stol(raw, nullptr, 10);
        else return fallback;
    } catch (...) { return fallback; }
    return (int)number;
}
static std::string cleanName(const std::string& name, const std::string& fallback = "SYMBOL") {
    std::string value = trimStr(name.empty() ? fallback : name);
    value = std::regex_replace(value, std::regex("[^A-Za-z0-9_.$@?]"), "_");
    return value.empty() ? fallback : value;
}
static std::vector<std::string> splitRemuRecords(const std::string& text) {
    std::vector<std::string> out; std::string cur;
    for (char c : text) { if (c == ';') { std::string t = trimStr(cur); if (!t.empty()) out.push_back(t); cur.clear(); } else cur += c; }
    std::string t = trimStr(cur); if (!t.empty()) out.push_back(t);
    return out;
}

RemuData parseRemu(const std::string& text) {
    std::vector<std::string> records = splitRemuRecords(text);
    RemuData result; result.text = text; result.records = records;
    for (const std::string& raw : records) {
        if (raw.size() > MAX_SYMBOL_LINE) { result.unknown.push_back(raw.substr(0, 64)); continue; }
        std::smatch head;
        if (!std::regex_search(raw, head, std::regex("^([A-Za-z]+)\\s*(.*)$"))) { result.unknown.push_back(raw); continue; }
        std::string tag = lowerStr(head[1].str());
        std::string rest = trimStr(head[2].str());
        if (tag == "label" || tag == "romlabel") {
            std::smatch m;
            if (!std::regex_search(rest, m, std::regex("^(\\S+)\\s+([^\\s,]+)\\s+([^\\s,]+)"))) { result.unknown.push_back(raw); continue; }
            std::optional<int> address = parseNumeric(m[2].str());
            std::optional<int> bank = parseNumeric(m[3].str());
            if (!address || !bank) { result.unknown.push_back(raw); continue; }
            RemuSymbol s; s.name = cleanName(m[1].str()); s.address = *address & 0xffff; s.bank = bank; s.kind = tag == "romlabel" ? "rom" : "ram"; s.source = "snapshot";
            result.symbols.push_back(s);
            continue;
        }
        if (tag == "alias") {
            std::smatch m;
            if (!std::regex_search(rest, m, std::regex("^(\\S+)\\s+([^\\s,]+)"))) { result.unknown.push_back(raw); continue; }
            std::optional<int> value = parseNumeric(m[2].str());
            if (!value) { result.unknown.push_back(raw); continue; }
            RemuSymbol s; s.name = cleanName(m[1].str()); s.address = *value & 0xffff; s.bank = std::nullopt; s.kind = "alias"; s.source = "snapshot";
            result.symbols.push_back(s);
            continue;
        }
        if (tag == "comz" || tag == "romcomz") {
            std::smatch m;
            if (!std::regex_search(rest, m, std::regex("^([^\\s,]+)\\s+([^\\s,]+)\\s*(.*)$"))) { result.unknown.push_back(raw); continue; }
            std::optional<int> address = parseNumeric(m[1].str());
            std::optional<int> bank = parseNumeric(m[2].str());
            if (!address || !bank) { result.unknown.push_back(raw); continue; }
            RemuComment c; c.address = *address & 0xffff; c.bank = bank; c.text = m[3].str(); c.kind = tag == "romcomz" ? "rom" : "ram"; c.source = "snapshot";
            result.comments.push_back(c);
            continue;
        }
        if (tag == "brk" || tag == "rombrk") {
            std::vector<std::string> fields; { std::regex re("[\\s,]+"); std::sregex_token_iterator it(rest.begin(), rest.end(), re, -1), end; for (; it != end; ++it) { std::string f = *it; if (!f.empty()) fields.push_back(f); } }
            std::optional<int> address = fields.size() > 0 ? parseNumeric(fields[0]) : std::nullopt;
            std::optional<int> bank = fields.size() > 1 ? parseNumeric(fields[1]) : std::nullopt;
            if (!address || !bank) { result.unknown.push_back(raw); continue; }
            RemuBreakpoint b; b.address = *address & 0xffff; b.bank = bank; b.kind = tag == "rombrk" ? "rom" : "ram"; b.source = "snapshot";
            result.breakpoints.push_back(b);
            continue;
        }
        if (tag == "acebreak") {
            std::vector<std::string> parts; { std::string cur; for (char c : rest) { if (c == ',') { std::string t = trimStr(cur); if (!t.empty()) parts.push_back(t); cur.clear(); } else cur += c; } std::string t = trimStr(cur); if (!t.empty()) parts.push_back(t); }
            std::unordered_set<std::string> flags;
            std::unordered_map<std::string, std::string> attrs;
            for (const std::string& part : parts) {
                size_t eq = part.find('=');
                if (eq != std::string::npos && eq > 0) attrs[lowerStr(trimStr(part.substr(0, eq)))] = trimStr(part.substr(eq + 1));
                else flags.insert(upperStr(part));
            }
            auto attr = [&](const std::string& k) -> std::string { auto it = attrs.find(k); return it != attrs.end() ? it->second : ""; };
            std::optional<int> address = parseNumeric(attr("addr"));
            int mask = parseNumeric(attr("mask"), 0xffff).value_or(0xffff);
            int size = parseNumeric(attr("size"), 1).value_or(1);
            int value = parseNumeric(attr("value"), 0).value_or(0);
            int valueMask = parseNumeric(attr("valmask"), 0).value_or(0);
            int addrVal = address ? (*address & 0xffff) : 0;
            std::string nm = attr("name");
            if (address && flags.count("STOP") && flags.count("EXEC") && mask == 0xffff && size == 1 && valueMask == 0) {
                RemuBreakpoint b; b.address = addrVal; b.mask = mask & 0xffff; b.size = size; b.value = value; b.valueMask = valueMask; b.name = nm; b.source = "snapshot"; b.ace = true; b.bank = std::nullopt; b.kind = "logical";
                result.breakpoints.push_back(b);
            } else if (address && flags.count("STOP") && flags.count("MEM") && mask == 0xffff && size >= 1) {
                RemuWatchpoint w; w.address = addrVal; w.mask = mask & 0xffff; w.size = size; w.value = value; w.valueMask = valueMask; w.name = nm; w.source = "snapshot"; w.ace = true;
                w.type = (flags.count("W") && !flags.count("R")) ? "write" : (flags.count("R") && !flags.count("W")) ? "read" : "access";
                w.end = (addrVal + std::max(1, size) - 1) & 0xffff;
                result.watchpoints.push_back(w);
            } else {
                result.unknown.push_back(raw);
            }
            continue;
        }
        result.unknown.push_back(raw);
    }
    return result;
}

static std::optional<int> parseSymbolNumeric(const std::string& valueIn) {
    std::string raw = trimStr(valueIn);
    if (std::regex_match(raw, std::regex("^[0-9a-f]{1,8}$", std::regex::icase))) { try { return (int)std::stol(raw, nullptr, 16); } catch (...) { return std::nullopt; } }
    return parseNumeric(raw);
}
static std::optional<int> parseBankToken(const std::string& token) {
    std::string raw = trimStr(std::regex_replace(token, std::regex("^\\[|\\]$"), ""));
    if (std::regex_match(raw, std::regex("^C[0-9A-F]$", std::regex::icase))) { try { return (int)std::stol(raw.substr(1), nullptr, 16); } catch (...) { return std::nullopt; } }
    if (std::regex_match(raw, std::regex("^ROM[0-9A-F]{1,2}$", std::regex::icase))) { try { return (int)std::stol(raw.substr(3), nullptr, 16); } catch (...) { return std::nullopt; } }
    return parseNumeric(raw);
}

std::vector<RemuSymbol> parseSymbolText(const std::string& text, const ParseSymbolOptions& options) {
    std::vector<RemuSymbol> symbols;
    auto add = [&](const std::string& name, std::optional<int> address, std::optional<int> bank, const std::string& kind) {
        if (!address) return;
        RemuSymbol s; s.name = cleanName(name); s.address = *address & 0xffff; s.bank = bank; s.kind = kind; s.source = options.source; s.fileName = options.fileName;
        symbols.push_back(s);
    };
    const std::string& input = text;
    if (std::regex_search(input, std::regex("\\b(?:label|romlabel|alias|comz|romcomz|brk|rombrk|acebreak)\\b", std::regex::icase)) && input.find(';') != std::string::npos) {
        RemuData r = parseRemu(input);
        std::vector<RemuSymbol> out;
        for (RemuSymbol s : r.symbols) { s.source = options.source; s.fileName = options.fileName; out.push_back(s); }
        return out;
    }
    std::vector<std::string> lines; { std::string cur; for (char c : input) { if (c == '\n') { std::string l = cur; if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); cur.clear(); } else cur += c; } lines.push_back(cur); }
    for (const std::string& original : lines) {
        if (original.size() > MAX_SYMBOL_LINE) continue;
        std::string line = std::regex_replace(original, std::regex(";.*"), "");
        line = std::regex_replace(line, std::regex("//.*$"), "");
        line = trimStr(line);
        if (line.empty()) continue;
        std::smatch m;
        if (std::regex_search(line, m, std::regex("^([A-Za-z0-9\\[\\]]+)\\s*:\\s*([&$#]?[0-9A-Fa-f]+)\\s+([A-Za-z_.$@?][\\w.$@?]*)"))) {
            std::string bankToken = m[1].str();
            std::optional<int> bank = parseBankToken(bankToken);
            std::string kind = std::regex_search(bankToken, std::regex("^ROM", std::regex::icase)) ? "rom" : "ram";
            add(m[3].str(), parseSymbolNumeric(m[2].str()), bank, kind); continue;
        }
        if (std::regex_search(line, m, std::regex("^([0-9A-Fa-f]{4,8})\\s+([A-Za-z_.$@?][\\w.$@?]*)\\b"))) {
            std::string name = m[2].str();
            if (!std::regex_search(name, std::regex("^(?:l__|s__|\\.)", std::regex::icase))) { try { add(name, (int)std::stol(m[1].str(), nullptr, 16), std::nullopt, "logical"); } catch (...) {} }
            continue;
        }
        if (std::regex_search(line, m, std::regex("^([A-Za-z_.$@?][\\w.$@?]*)\\s+(?:EQU\\s+|=\\s*)?([&$#]?[0-9A-Fa-f]+H?)\\b", std::regex::icase))) {
            add(m[1].str(), parseSymbolNumeric(m[2].str()), std::nullopt, "logical"); continue;
        }
        if (std::regex_search(line, m, std::regex("^([A-Za-z_.$@?][\\w.$@?]*)\\s*=\\s*([&$#]?[0-9A-Fa-f]+H?)\\b", std::regex::icase))) {
            add(m[1].str(), parseSymbolNumeric(m[2].str()), std::nullopt, "logical"); continue;
        }
        if (std::regex_search(line, m, std::regex("^([&$#]?[0-9A-Fa-f]+H?)\\s+([&$#]?[0-9A-Fa-f]+H?)\\s+([A-Za-z_.$@?][\\w.$@?]*)"))) {
            add(m[3].str(), parseSymbolNumeric(m[2].str()), parseBankToken(m[1].str()), "ram"); continue;
        }
    }
    return symbols;
}

static std::string remuRecord(const std::string& record) {
    return std::regex_replace(trimStr(record), std::regex(";+\\s*$"), "") + ";";
}
std::string buildRemuText(const DebugMetadata& metadata, const BuildRemuOptions& options) {
    std::vector<std::string> records = metadata.remu.records;
    std::unordered_set<std::string> seen;
    for (const std::string& r : records) seen.insert(lowerStr(trimStr(r)));
    auto add = [&](const std::string& record) {
        std::string value = std::regex_replace(trimStr(record), std::regex(";+\\s*$"), "");
        if (value.empty() || seen.count(lowerStr(value))) return;
        records.push_back(value); seen.insert(lowerStr(value));
    };
    for (const RemuSymbol& symbol : options.symbols) {
        if (symbol.source == "snapshot") continue;
        if (symbol.kind == "alias" || !symbol.bank) { add("alias " + cleanName(symbol.name) + " " + std::to_string(symbol.address & 0xffff)); continue; }
        std::string tag = symbol.kind == "rom" ? "romlabel" : "label";
        add(tag + " " + cleanName(symbol.name) + " " + std::to_string(symbol.address & 0xffff) + " " + std::to_string(*symbol.bank));
    }
    for (const RemuBreakpoint& bp : options.breakpoints) {
        if (bp.source == "snapshot") continue;
        if (bp.kind == "rom") add("rombrk " + std::to_string(bp.address & 0xffff) + " " + std::to_string(bp.bank ? *bp.bank : 256));
        else if (bp.bank) add("brk " + std::to_string(bp.address & 0xffff) + " " + std::to_string(*bp.bank));
        else add("acebreak EXEC,RW,STOP,addr=" + std::to_string(bp.address & 0xffff) + ",mask=65535,size=1,value=0,valmask=0,name=" + cleanName(bp.name.empty() ? "cpcse_break" : bp.name));
    }
    for (const RemuWatchpoint& wp : options.watchpoints) {
        if (wp.source == "snapshot" || (wp.type != "read" && wp.type != "write" && wp.type != "access")) continue;
        std::string rw = wp.type == "read" ? "R" : wp.type == "write" ? "W" : "RW";
        int size = ((wp.end ? wp.end : wp.address) - wp.address + 1) & 0xffff; if (!size) size = 1;
        add("acebreak MEM," + rw + ",STOP,addr=" + std::to_string(wp.address & 0xffff) + ",mask=65535,size=" + std::to_string(size) + ",value=0,valmask=0,name=" + cleanName(wp.name.empty() ? "cpcse_watch" : wp.name));
    }
    std::string out; for (const std::string& r : records) out += remuRecord(r);
    return out;
}

} // namespace cpcse
