// CPCSyntaxError: see disasm_export.h.
#include "disasm_export.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "core/disassembler.h"
#include "rasm_bridge.h"

extern "C" {
// The same two result structures as gui_assembler.cpp takes from rasm.h.
struct s_debug_error { char* filename; int line; char* msg; int lenmsg, lenfilename; };
struct s_debug_symbol { char* name; int v; };
struct s_rasm_info {
    struct s_debug_error* error; int nberror, maxerror, warnerr;
    struct s_debug_symbol* symbol; int nbsymbol, maxsymbol;
    int run, start;
    unsigned char* emuram; int lenram;
    unsigned char* emurom; int lenrom;
};
}

namespace cpcse {

namespace {

struct Ins { int addr = 0; std::vector<int> bytes; std::string mnem; bool data = false; int target = -1; };

std::string hex2(int v) { char b[8]; std::snprintf(b, sizeof b, "#%02X", v & 0xff); return b; }
std::string hex4(int v) { char b[8]; std::snprintf(b, sizeof b, "#%04X", v & 0xffff); return b; }

// A label RASM takes: letters, digits, '_' and '.', starting with a letter (others become '_').
std::string labelName(const std::string& in) {
    std::string out;
    for (char c : in) out += (std::isalnum((unsigned char)c) || c == '_' || c == '.') ? c : '_';
    if (out.empty() || !std::isalpha((unsigned char)out[0])) out = "L_" + out;
    return out;
}

// The disassembler's text in RASM's spelling: '#' for hex ('&' is AND in RASM), the index
// registers' halves as XH XL YH YL.
std::string rasmText(std::string m) {
    for (char& c : m) if (c == '&') c = '#';
    const std::pair<const char*, const char*> halves[] = { { "HX", "XH" }, { "LX", "XL" }, { "HY", "YH" }, { "LY", "YL" } };
    for (const auto& h : halves) {
        for (size_t p = 0; (p = m.find(h.first, p)) != std::string::npos; p += 2) {
            const bool before = p == 0 || !std::isalnum((unsigned char)m[p - 1]);
            const bool after = p + 2 >= m.size() || !std::isalnum((unsigned char)m[p + 2]);
            if (before && after) m.replace(p, 2, h.second);
        }
    }
    return m;
}

} // namespace

DisasmExport exportDisassembly(const std::function<int(int)>& read, int from, int to, const std::unordered_map<int, std::string>& known) {
    DisasmExport out;
    from &= 0xffff;
    if (to <= from || to > 0x10000) { out.why = "the range is empty"; return out; }
    // the instructions, one after the other
    Disassembler dis;
    std::vector<Ins> list;
    for (int a = from; a < to;) {
        const DisasmResult d = dis.disassemble(read, a);
        Ins in;
        in.addr = a;
        in.bytes = d.bytes;
        in.mnem = d.mnem;
        if (in.bytes.empty()) in.bytes = { read(a) & 0xff };
        if (a + (int)in.bytes.size() > to) { in.bytes.resize((size_t)(to - a)); in.data = true; }   // cut by the end
        if (in.mnem.find("???") != std::string::npos) in.data = true;
        // encodings an assembler writes another way: data from the start (the check below
        // finds any other)
        if (!in.data && in.bytes.size() >= 2) {
            const int p = in.bytes[0], op = in.bytes[1];
            if ((p == 0xdd || p == 0xfd) && in.mnem.find("IX") == std::string::npos && in.mnem.find("IY") == std::string::npos &&
                in.mnem.find("HX") == std::string::npos && in.mnem.find("LX") == std::string::npos &&
                in.mnem.find("HY") == std::string::npos && in.mnem.find("LY") == std::string::npos)
                in.data = true;                                      // a prefix with no index register to change
            if ((p == 0xdd || p == 0xfd) && op == 0xcb && in.bytes.size() >= 4 && (in.bytes[3] & 7) != 6) in.data = true;   // DD CB d op, r
            if (p == 0xed) {
                static const std::set<int> documented = {
                    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4d, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x56, 0x57,
                    0x58, 0x59, 0x5a, 0x5b, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x67, 0x68, 0x69, 0x6a, 0x6f, 0x72, 0x73, 0x78, 0x79, 0x7a, 0x7b,
                    0xa0, 0xa1, 0xa2, 0xa3, 0xa8, 0xa9, 0xaa, 0xab, 0xb0, 0xb1, 0xb2, 0xb3, 0xb8, 0xb9, 0xba, 0xbb };
                if (!documented.count(op)) in.data = true;           // duplicates, ED 63/6B (LD (nn),HL is 22 / 2A), IN (C)
            }
        }
        // a jump or call target: the last operand, when it is a number
        const std::string mn = in.mnem.substr(0, in.mnem.find(' '));
        if (!in.data && (mn == "JP" || mn == "JR" || mn == "CALL" || mn == "DJNZ")) {
            const size_t amp = in.mnem.rfind('&');
            if (amp != std::string::npos && in.mnem.find('(') == std::string::npos) in.target = (int)std::strtol(in.mnem.c_str() + amp + 1, nullptr, 16);
        }
        list.push_back(in);
        a += (int)in.bytes.size();
    }
    std::set<int> starts;
    for (const Ins& in : list) starts.insert(in.addr);
    // up to 1000 times: write, assemble, and make the first instruction that differs data
    for (int pass = 0; pass < 1000; pass++) {
        std::map<int, std::string> labels;                   // in the range: at an instruction start
        std::map<std::string, int> equs;                      // outside it, by a name the debugger knows
        std::set<std::string> used;
        auto unique = [&](std::string n) { std::string u = n; for (int k = 2; used.count(u); k++) u = n + "_" + std::to_string(k); used.insert(u); return u; };
        for (const Ins& in : list) if (!in.data) if (auto k = known.find(in.addr); k != known.end()) labels[in.addr] = unique(labelName(k->second));
        for (const Ins& in : list) {
            if (in.data || in.target < 0) continue;
            if (in.target >= from && in.target < to && starts.count(in.target)) {
                if (!labels.count(in.target)) { char b[16]; std::snprintf(b, sizeof b, "L%04X", in.target); labels[in.target] = unique(b); }
            } else if (auto k = known.find(in.target); k != known.end() && (in.target < from || in.target >= to)) {
                bool have = false;
                for (const auto& e : equs) have = have || e.second == in.target;
                if (!have) equs[unique(labelName(k->second))] = in.target;
            }
        }
        auto nameOf = [&](int target) -> std::string {
            if (auto l = labels.find(target); l != labels.end()) return l->second;
            for (const auto& e : equs) if (e.second == target) return e.first;
            return "";
        };
        std::string text;
        char head[128];
        std::snprintf(head, sizeof head, "; exported by CPCSyntaxError: &%04X to &%04X\n", from, to - 1);
        text += head;
        for (const auto& e : equs) text += e.first + " equ " + hex4(e.second) + "\n";
        text += "\n    org " + hex4(from) + "\n";
        std::vector<int> lineOf(list.size(), 0);
        int instructions = 0, dataBytes = 0;
        for (size_t i = 0; i < list.size(); i++) {
            const Ins& in = list[i];
            // (a jump into what became data still needs its label: the label goes before the DB)
            if (auto l = labels.find(in.addr); l != labels.end()) text += l->second + ":\n";
            std::string line;
            if (in.data) {
                line = "    db ";
                for (size_t k = 0; k < in.bytes.size(); k++) line += (k ? "," : "") + hex2(in.bytes[k]);
                dataBytes += (int)in.bytes.size();
            } else {
                std::string m = in.mnem;
                if (in.target >= 0) {
                    const std::string n = nameOf(in.target);
                    if (!n.empty()) m = m.substr(0, m.rfind('&')) + n;
                }
                line = "    " + rasmText(m);
                instructions++;
            }
            char addr[24];
            std::snprintf(addr, sizeof addr, " ; &%04X", in.addr);
            text += line + std::string(line.size() < 32 ? 32 - line.size() : 1, ' ') + addr + "\n";
        }
        out.text = text;
        out.instructions = instructions;
        out.dataBytes = dataBytes;
        out.labels = (int)labels.size() + (int)equs.size();
        // assembled again: the first byte that differs makes its instruction data
        unsigned char* bin = nullptr;
        int binLength = 0, fatal = 0;
        s_rasm_info* info = nullptr;
        const int r = rasm_bridge_assemble(text.c_str(), (int)text.size(), RASM_SYNTAX_RASM, &bin, &binLength, &info, &fatal);
        std::vector<unsigned char> got(bin, bin + std::max(0, binLength));
        std::string firstError;
        if (info && info->nberror > 0 && info->error[0].msg)
            firstError = "line " + std::to_string(info->error[0].line) + ": " +
                         std::string(info->error[0].msg, (size_t)std::max(0, info->error[0].lenmsg));
        rasm_bridge_free(bin, info);
        if (r != 0 || fatal) {
            out.why = "RASM did not assemble the export";
            if (!firstError.empty()) out.why += " (" + firstError + ")";
            return out;
        }
        int differ = -1;
        for (int k = 0; k < to - from; k++) if (k >= (int)got.size() || got[(size_t)k] != (read(from + k) & 0xff)) { differ = from + k; break; }
        if (differ < 0 && (int)got.size() == to - from) { out.verified = true; return out; }
        if (differ < 0) differ = to - 1;                      // longer than the range: the last one
        bool changed = false;
        for (Ins& in : list)
            if (!in.data && differ >= in.addr && differ < in.addr + (int)in.bytes.size()) { in.data = true; changed = true; break; }
        if (!changed) { out.why = "RASM's bytes differ where the export has data"; return out; }
    }
    out.why = "too many differences";
    return out;
}

} // namespace cpcse
