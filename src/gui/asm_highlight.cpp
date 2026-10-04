// CPCSyntaxError GUI — Z80 / RASM source split into coloured spans. See asm_highlight.h.
#include "asm_highlight.h"

#include <cctype>
#include <unordered_set>

namespace cpcse {

namespace {

const std::unordered_set<std::string>& mnemonics() {
    static const std::unordered_set<std::string> s = {
        "ld", "push", "pop", "ex", "exx", "ldi", "ldir", "ldd", "lddr", "cpi", "cpir", "cpd", "cpdr",
        "add", "adc", "sub", "sbc", "and", "or", "xor", "cp", "inc", "dec", "daa", "cpl", "neg", "ccf", "scf",
        "nop", "halt", "di", "ei", "im", "rlca", "rla", "rrca", "rra", "rlc", "rl", "rrc", "rr", "sla", "sra",
        "sll", "sl1", "srl", "rld", "rrd", "bit", "set", "res", "jp", "jr", "djnz", "call", "ret", "reti", "retn",
        "rst", "in", "ini", "inir", "ind", "indr", "out", "outi", "otir", "outd", "otdr",
        // RASM's shorthands
        "exa", "exd", "ldix", "ldirx", "lddx", "lddrx", "outi0",
    };
    return s;
}

const std::unordered_set<std::string>& directives() {
    static const std::unordered_set<std::string> s = {
        "org", "equ", "defl", "db", "defb", "dm", "defm", "dw", "defw", "ds", "defs", "dr", "defr", "dz", "str",
        "text", "charset", "include", "read", "incbin", "incl48", "incl49", "inclz4", "incexo", "incapu",
        "inczx7", "inczx0", "incl4", "if", "ifdef", "ifndef", "ifused", "ifnused", "ifnot", "else", "elseif",
        "endif", "macro", "mend", "endm", "rept", "repeat", "rend", "until", "endr", "while", "wend",
        "switch", "case", "default", "break", "endswitch", "struct", "endstruct", "ends", "let", "assert",
        "print", "fail", "stop", "save", "write", "direct", "bank", "bankset", "run", "limit", "protect",
        "align", "nolist", "list", "buildsna", "buildcpr", "buildtape", "buildrom", "snaset", "snainit",
        "nocode", "code", "module", "endmodule", "noexport", "enoexport", "lz4", "lz48", "lz49", "lzexo",
        "lzapu", "lzx7", "lzx0", "lzsa1", "lzsa2", "lzclose", "ticker", "undef", "breakpoint", "range",
        "section", "name", "setcpc", "setcrtc", "amsdos", "hexbin", "skip", "cpcsnapshot", "binclude",
        "export", "proc", "endp",
    };
    return s;
}

const std::unordered_set<std::string>& registers() {
    static const std::unordered_set<std::string> s = {
        "a", "b", "c", "d", "e", "h", "l", "i", "r", "f", "af", "bc", "de", "hl", "ix", "iy", "sp", "pc",
        "ixh", "ixl", "iyh", "iyl", "xh", "xl", "yh", "yl", "lx", "hx", "ly", "hy",
    };
    return s;
}

bool isCondition(const std::string& w) {
    return w == "nz" || w == "z" || w == "nc" || w == "c" || w == "po" || w == "pe" || w == "p" || w == "m";
}

bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '@'; }
bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '$'; }

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

std::vector<AsmSpan> highlightAsmLine(const std::string& line, bool& inBlockComment) {
    std::vector<AsmSpan> spans;
    const int n = (int)line.size();
    auto push = [&](int start, int length, AsmToken kind) {
        if (length <= 0) return;
        if (!spans.empty() && spans.back().kind == kind && spans.back().start + spans.back().length == start) spans.back().length += length;
        else spans.push_back({ start, length, kind });
    };
    int i = 0;
    if (inBlockComment) {
        const size_t end = line.find("*/");
        if (end == std::string::npos) { push(0, n, AsmToken::Comment); return spans; }
        push(0, (int)end + 2, AsmToken::Comment);
        inBlockComment = false;
        i = (int)end + 2;
    }
    bool statementStart = true;     // the next word is an instruction, a directive or a label
    bool conditions = false;        // after JP/JR/CALL/RET: nz, z, nc, c... are conditions
    // The word after `at`, lower-cased ("" if none): to tell `name equ 5` from a macro call.
    auto nextWord = [&](int at) {
        while (at < n && (line[(size_t)at] == ' ' || line[(size_t)at] == '\t')) at++;
        if (at < n && line[(size_t)at] == '=') return std::string("=");
        int e = at;
        while (e < n && identChar(line[(size_t)e])) e++;
        return lower(line.substr((size_t)at, (size_t)(e - at)));
    };
    while (i < n) {
        const char c = line[(size_t)i];
        const char next = i + 1 < n ? line[(size_t)i + 1] : '\0';
        if (c == ' ' || c == '\t') { int s = i; while (i < n && (line[(size_t)i] == ' ' || line[(size_t)i] == '\t')) i++; push(s, i - s, AsmToken::Text); continue; }
        if (c == ';' || (c == '/' && next == '/')) { push(i, n - i, AsmToken::Comment); break; }
        if (c == '/' && next == '*') {
            const size_t end = line.find("*/", (size_t)i + 2);
            if (end == std::string::npos) { push(i, n - i, AsmToken::Comment); inBlockComment = true; break; }
            push(i, (int)end + 2 - i, AsmToken::Comment);
            i = (int)end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            int e = i + 1;
            while (e < n && line[(size_t)e] != c) { if (line[(size_t)e] == '\\' && e + 1 < n) e++; e++; }
            if (e < n) e++;                                   // the closing quote
            push(i, e - i, AsmToken::String);
            i = e;
            statementStart = false;
            continue;
        }
        if (c == ':') { push(i, 1, AsmToken::Text); i++; statementStart = true; conditions = false; continue; }
        // numbers: #4000 &4000 $4000 0x4000 4000h %1010 0b1010 12 -- and $ alone, the address
        const bool hexPrefix = (c == '#' || c == '&' || c == '$') && std::isxdigit((unsigned char)next);
        const bool binPrefix = c == '%' && (next == '0' || next == '1');
        if (std::isdigit((unsigned char)c) || hexPrefix || binPrefix || (c == '$' && !identStart(next))) {
            int e = i + 1;
            while (e < n && (std::isalnum((unsigned char)line[(size_t)e]) || line[(size_t)e] == '.')) e++;
            push(i, e - i, AsmToken::Number);
            i = e;
            statementStart = false;
            continue;
        }
        if (identStart(c)) {
            int e = i;
            while (e < n && identChar(line[(size_t)e])) e++;
            const std::string word = lower(line.substr((size_t)i, (size_t)(e - i)));
            AsmToken kind;
            if (statementStart && e < n && line[(size_t)e] == ':' && (e + 1 >= n || line[(size_t)e + 1] != ':')) {
                kind = AsmToken::Label;                       // name:
            } else if (statementStart) {
                if (mnemonics().count(word)) {
                    kind = AsmToken::Mnemonic;
                    conditions = word == "jp" || word == "jr" || word == "call" || word == "ret";
                    statementStart = false;
                } else if (directives().count(word)) {
                    kind = AsmToken::Directive;
                    statementStart = false;
                } else {
                    const std::string after = nextWord(e);
                    // a name in the first column, or one defined by EQU / = / MACRO: a label
                    if (i == 0 || after == "equ" || after == "=" || after == "defl" || after == "macro") kind = AsmToken::Label;
                    else { kind = AsmToken::Symbol; statementStart = false; }   // a macro used
                }
            } else if (word == "af" && e < n && line[(size_t)e] == '\'') {
                kind = AsmToken::Register;                    // af'
                e++;
            } else if (registers().count(word) || (conditions && isCondition(word))) {
                kind = AsmToken::Register;
            } else if (directives().count(word)) {
                kind = AsmToken::Directive;                   // `label equ`, `macro name`
            } else {
                kind = AsmToken::Symbol;
            }
            push(i, e - i, kind);
            i = e;
            continue;
        }
        push(i, 1, AsmToken::Text);                           // , ( ) + - * and the rest
        i++;
    }
    return spans;
}

const char* asmTokenKey(AsmToken t) {
    static const char* const keys[] = { "text", "mnemonic", "register", "directive", "number", "string", "comment", "label", "symbol" };
    return keys[(size_t)t];
}

const char* asmTokenLabel(AsmToken t) {
    static const char* const labels[] = { "Text and operators", "Instructions", "Registers and conditions", "Directives",
                                          "Numbers", "Strings", "Comments", "Labels (defined)", "Symbols (used)" };
    return labels[(size_t)t];
}

AsmColours defaultAsmColours() {
    AsmColours c{};
    c[(size_t)AsmToken::Text] = 0xD4D4D4;
    c[(size_t)AsmToken::Mnemonic] = 0x6CB6FF;
    c[(size_t)AsmToken::Register] = 0xF0C674;
    c[(size_t)AsmToken::Directive] = 0xC586C0;
    c[(size_t)AsmToken::Number] = 0xB5CEA8;
    c[(size_t)AsmToken::String] = 0xE6936A;
    c[(size_t)AsmToken::Comment] = 0x6A9955;
    c[(size_t)AsmToken::Label] = 0xFFC740;   // the interface's accent (kAccent)
    c[(size_t)AsmToken::Symbol] = 0x9CDCFE;
    return c;
}

} // namespace cpcse
