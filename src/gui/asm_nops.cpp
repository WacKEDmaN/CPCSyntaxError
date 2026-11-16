// CPCSyntaxError: see asm_nops.h.
#include "asm_nops.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "core/disassembler.h"
#include "core/emulator.h"
#include "core/memory.h"
#include "core/z80.h"

namespace cpcse {

namespace {

std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

const std::set<std::string>& registers() {
    static const std::set<std::string> r = { "A", "B", "C", "D", "E", "H", "L", "I", "R", "F", "AF", "AF'", "BC", "DE", "HL", "SP",
                                             "IX", "IY", "IXH", "IXL", "IYH", "IYL" };
    return r;
}
// the index registers' halves, by every name assemblers give them
std::string halfRegister(const std::string& o) {
    if (o == "HX" || o == "XH" || o == "IXH") return "IXH";
    if (o == "LX" || o == "XL" || o == "IXL") return "IXL";
    if (o == "HY" || o == "YH" || o == "IYH") return "IYH";
    if (o == "LY" || o == "YL" || o == "IYL") return "IYL";
    return "";
}

// The operands at the top level of the text (commas inside brackets or quotes stay).
std::vector<std::string> splitOperands(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    for (char c : s) {
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '(') depth++;
        if (c == ')') depth = std::max(0, depth - 1);
        if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.empty() || !out.empty()) out.push_back(cur);
    return out;
}

// The measured table: every opcode run on a CPC of its own (see asm_nops.h).
std::map<std::string, NopCost> buildTable() {
    std::map<std::string, NopCost> table;
    std::map<std::string, int> shortest;                  // each key's shortest encoding so far
    GX4000 machine;
    Disassembler dis;
    std::vector<std::vector<int>> codes;
    for (int op = 0; op < 256; op++) {
        if (op == 0xcb || op == 0xdd || op == 0xed || op == 0xfd) continue;
        codes.push_back({ op, 0, 0 });
    }
    for (int op = 0; op < 256; op++) codes.push_back({ 0xcb, op });
    for (int op = 0; op < 256; op++) codes.push_back({ 0xed, op, 0, 0 });
    for (int pre : { 0xdd, 0xfd }) {
        for (int op = 0; op < 256; op++) {
            if (op == 0xdd || op == 0xed || op == 0xfd) continue;
            if (op == 0xcb) { for (int op2 = 0; op2 < 256; op2++) codes.push_back({ pre, 0xcb, 0, op2 }); continue; }
            codes.push_back({ pre, op, 0, 0 });
        }
    }
    // One run: the code at &4000, a known state; flags, B and C as given. NOPs = usec.
    auto run = [&](const std::vector<int>& code, int f, int b, int c) {
        for (int i = 0; i < 8; i++) machine.memory->write(0x4000 + i, i < (int)code.size() ? code[(size_t)i] : 0);
        machine.memory->write(0x8000, 0x55);           // (HL), (IX+0): not A, so CPIR runs on
        Z80& z = *machine.cpu;
        z.pc = 0x4000; z.sp = 0xc000; z.iff1 = z.iff2 = false; z.halted = false;
        z.a = 0; z.f = f; z.b = b; z.c = c; z.d = 0x90; z.e = 0; z.h = 0x80; z.l = 0;
        z.ix = 0x8000; z.iy = 0x8000;
        const long long before = machine.machineCycles;
        machine.stepInstruction();
        return (int)((machine.machineCycles - before + 3) / 4);
    };
    for (const std::vector<int>& code : codes) {
        const DisasmResult d = dis.disassemble([&](int a) { const int i = a - 0x4000; return i >= 0 && i < (int)code.size() ? code[(size_t)i] : 0; }, 0x4000);
        if (d.mnem.find("???") != std::string::npos) continue;
        // a DD or FD before an instruction that has no HL to change: the prefix's own cost on
        // the plain instruction, not an instruction of its own
        // ("EX DE,HL" and "EXX" have an X of their own: the index registers by their names)
        if (code[0] == 0xdd || code[0] == 0xfd) {
            const std::string k = nopKey(d.mnem);
            bool index = false;
            for (const char* r : { "IX", "IY" }) index = index || k.find(r) != std::string::npos;
            if (!index) continue;
        }
        const std::string key = nopKey(d.mnem);
        const int length = d.next - 0x4000;               // the encoding's bytes
        NopCost cost;
        if (code[0] == 0x76) cost = { 1, 1 };            // HALT: one NOP at a time
        else {
            // the condition false and true, the last turn and another one (BC 1 / 2, B 1 / 2)
            const int a = run(code, 0x00, 0x00, 0x01), b = run(code, 0xff, 0x02, 0x02), c = run(code, 0x00, 0x01, 0x01);
            cost = { std::min({ a, b, c }), std::max({ a, b, c }) };
        }
        // Two encodings of one instruction (LD HL,(nn): 2A, and ED 6B): an assembler writes the
        // shortest, so its cost is the one. (Same length, as ED 4C beside ED 44 NEG: the first.)
        auto it = shortest.find(key);
        if (it == shortest.end() || length < it->second) { shortest[key] = length; table[key] = cost; }
    }
    return table;
}

const std::set<std::string>& mnemonics() {
    static const std::set<std::string> m = [] {
        std::set<std::string> s;
        for (const auto& kv : nopTable()) s.insert(kv.first.substr(0, kv.first.find(' ')));
        s.insert("SL1"); s.insert("SLS"); s.insert("EXA");
        return s;
    }();
    return m;
}

// The table's entry for one instruction, with the other spellings assemblers allow.
bool lookUp(const std::string& instruction, NopCost& out) {
    const std::map<std::string, NopCost>& t = nopTable();
    std::string key = nopKey(instruction);
    auto find = [&](const std::string& k) { auto it = t.find(k); if (it == t.end()) return false; out = it->second; return true; };
    if (find(key)) return true;
    const size_t sp = key.find(' ');
    std::string mn = key.substr(0, sp), ops = sp == std::string::npos ? "" : key.substr(sp + 1);
    if (mn == "SL1" || mn == "SLS") return find("SLL" + (ops.empty() ? "" : " " + ops));
    if (mn == "EXA" || key == "EX AF,AF") return find("EX AF,AF'");
    const std::vector<std::string> o = splitOperands(ops);
    if ((mn == "ADD" || mn == "ADC" || mn == "SBC") && o.size() == 1) return find(mn + " A," + o[0]);
    if ((mn == "SUB" || mn == "AND" || mn == "OR" || mn == "XOR" || mn == "CP") && o.size() == 2 && o[0] == "A") return find(mn + " " + o[1]);
    if (mn == "JP" && o.size() == 1 && (o[0] == "HL" || o[0] == "IX" || o[0] == "IY")) return find("JP (" + o[0] + ")");
    if (key == "IN F,(C)") return find("IN (C)");
    return false;
}

} // namespace

std::string nopKey(const std::string& instructionIn) {
    std::string s = upper(trim(instructionIn));
    // the mnemonic, then the operands with every space out
    size_t sp = 0;
    while (sp < s.size() && !std::isspace((unsigned char)s[sp])) sp++;
    const std::string mn = s.substr(0, sp);
    std::string rest;
    for (size_t i = sp; i < s.size(); i++) if (!std::isspace((unsigned char)s[i])) rest += s[i];
    if (rest.empty()) return mn;
    std::string key = mn + " ";
    const std::vector<std::string> ops = splitOperands(rest);
    static const std::set<std::string> conditions = { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" };
    const bool branch = mn == "JP" || mn == "JR" || mn == "CALL" || mn == "RET";
    for (size_t k = 0; k < ops.size(); k++) {
        const std::string& o = ops[k];
        std::string c;
        if (!halfRegister(o).empty()) c = halfRegister(o);
        else if (registers().count(o)) c = o;
        else if (k == 0 && branch && conditions.count(o)) c = o;
        else if (o.size() >= 2 && o.front() == '(' && o.back() == ')') {
            const std::string in = o.substr(1, o.size() - 2);
            if (in == "BC" || in == "DE" || in == "HL" || in == "SP" || in == "C") c = "(" + in + ")";
            else if (in == "IX" || in.rfind("IX+", 0) == 0 || in.rfind("IX-", 0) == 0) c = "(IX+d)";
            else if (in == "IY" || in.rfind("IY+", 0) == 0 || in.rfind("IY-", 0) == 0) c = "(IY+d)";
            else c = "(n)";
        } else c = "n";                                  // a number, a label, an expression
        key += (k ? "," : "") + c;
    }
    return key;
}

const std::map<std::string, NopCost>& nopTable() {
    static const std::map<std::string, NopCost> table = buildTable();
    return table;
}

bool isInstructionWord(const std::string& upperWord) { return mnemonics().count(upperWord) != 0; }

LineNops lineNops(const std::string& lineIn) {
    LineNops out;
    // "AF'" is a register, not the start of a quoted character: kept apart from the quotes
    std::string line = lineIn;
    for (size_t p = 0; (p = upper(line).find("AF'", p)) != std::string::npos; p += 3) line[p + 2] = '\x01';
    // the comment off
    char quote = 0;
    for (size_t i = 0; i < line.size(); i++) {
        if (quote) { if (line[i] == quote) quote = 0; continue; }
        if (line[i] == '"' || line[i] == '\'') quote = line[i];
        else if (line[i] == ';') { line.resize(i); break; }
    }
    // a label in the first column: a word that is not an instruction
    if (!line.empty() && !std::isspace((unsigned char)line[0])) {
        size_t e = 0;
        while (e < line.size() && !std::isspace((unsigned char)line[e]) && line[e] != ':') e++;
        if (!isInstructionWord(upper(line.substr(0, e)))) line = line.substr(e);
    }
    // the instructions, split at ':' outside quotes
    std::vector<std::string> statements;
    std::string cur;
    quote = 0;
    for (char c : line) {
        if (quote) { cur += c; if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == ':') { statements.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    statements.push_back(cur);
    for (std::string st : statements) {
        for (char& c : st) if (c == '\x01') c = '\'';
        st = trim(st);
        if (st.empty()) continue;
        size_t e = 0;
        while (e < st.size() && !std::isspace((unsigned char)st[e])) e++;
        if (!isInstructionWord(upper(st.substr(0, e)))) continue;   // a directive, a macro, a label
        out.instructions++;
        NopCost c;
        if (lookUp(st, c)) {
            out.min += c.min; out.max += c.max;
            out.parts.push_back(st + "    " + (c.min == c.max ? std::to_string(c.min) : std::to_string(c.min) + "/" + std::to_string(c.max)));
        } else {
            out.unknown = true;
            out.parts.push_back(st + "    ? (operands not known)");
        }
    }
    return out;
}

std::string nopText(const LineNops& l) {
    if (l.instructions == 0) return "";
    if (l.unknown) return "?";
    return l.min == l.max ? std::to_string(l.min) : std::to_string(l.min) + "/" + std::to_string(l.max);
}

} // namespace cpcse
