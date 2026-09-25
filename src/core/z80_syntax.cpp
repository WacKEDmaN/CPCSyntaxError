// CPCSyntaxError — Z80 syntax tools.
#include "z80_syntax.h"

namespace cpcse {

static const std::unordered_set<std::string> DIRECTIVES = {
    "EQU","ORG","AORG","DF","DQ","DT","RUN","ENT","WRITE","DIRECT",
    "ADD_LOADER","CREATE_DSK","MAKEDISK","READ","INCLUDE","INCBIN",
    "NOHEADER","NOLIST","LIST","IF","IFDEF","ELSE","ENDIF","REPEAT",
    "REPT","REND","ENDM","LIMIT","BANK","ALIGN","DEFINE","LET"
};
static const std::unordered_set<std::string> DATA_DIRECTIVES = { "DS","DB","DW","DEFB","DEFM","DEFS","DEFW" };
static const std::unordered_set<std::string> REGISTERS = {
    "A","B","C","D","E","H","L","AF","AF'","BC","DE","HL","F",
    "I","M","IX","IY","IXL","IYL","IXH","IYH","YH","YL","XL","XH",
    "HY","LX","HX","PC","NZ","NC","P","PO","PE","M","Z","R","SP"
};
static const std::unordered_set<std::string> OPCODES = {
    "ADC","ADD","AND","BIT","CALL","CCF","CP","CPD","CPDR","CPI",
    "CPIR","CPL","DAA","DEC","DI","DJNZ","EI","EX","EXX","HALT",
    "IM","IN","INC","IND","INDR","INI","INIR","JP","JR","LD","LDD",
    "LDDR","LDI","LDIR","NEG","NOP","OR","OTDR","OTIR","OUT","OUTD",
    "OUTI","POP","PUSH","RES","RET","RETI","RETN","RL","RLA","RLC",
    "RLCA","RLD","RR","RRA","RRC","RRCA","RRD","RST","SBC","SCF",
    "SET","SLA","SLL","SRA","SRL","SUB","XOR","BRK","BREAK"
};

static std::string escapeHtml(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '&') out += "&amp;";
        else if (ch == '<') out += "&lt;";
        else if (ch == '>') out += "&gt;";
        else if (ch == '"') out += "&quot;";
        else if (ch == '\'') out += "&#39;";
        else out += ch;
    }
    return out;
}
static std::string span(const std::string& cls, const std::string& text) { return "<span class=\"" + cls + "\">" + escapeHtml(text) + "</span>"; }
static bool isHex(char ch) { return std::isxdigit((unsigned char)ch) != 0; }
static bool isSpace(char ch) { return std::isspace((unsigned char)ch) != 0; }
static bool isIdentStart(char ch) { return std::isalpha((unsigned char)ch) || ch == '_' || ch == '.'; }
static bool isIdent(char ch) { return std::isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == '\'' || ch == '$'; }
static bool isOp(char ch) { return std::string("+-*/%^|&~!=<>").find(ch) != std::string::npos; }
static std::string upperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static bool endsWith(const std::string& s, char c) { return !s.empty() && s.back() == c; }

static std::string highlightLine(const std::string& line) {
    int i = 0, n = (int)line.size();
    std::string out;
    auto at = [&](int k) { return k >= 0 && k < n ? line[k] : '\0'; };
    while (i < n) {
        char ch = line[i];
        if (ch == ';') { out += span("z80-comment", line.substr(i)); break; }
        if (isSpace(ch)) { int j = i; while (j < n && isSpace(line[j])) j++; out += escapeHtml(line.substr(i, j - i)); i = j; continue; }
        if (ch == '"' || ch == '\'') {
            char quote = ch; int j = i + 1;
            while (j < n && line[j] != quote) j++;
            if (j < n) j++;
            bool closed = j <= n && j - 1 >= 0 && at(j - 1) == quote;
            out += span(closed ? (quote == '"' ? "z80-string" : "z80-char") : "z80-error", line.substr(i, j - i));
            i = j; continue;
        }
        if (ch == '&' && isHex(at(i + 1))) { int j = i + 1; while (j < n && isHex(line[j])) j++; out += span("z80-number", line.substr(i, j - i)); i = j; continue; }
        if (ch == '#' && isHex(at(i + 1))) { int j = i + 1; while (j < n && isHex(line[j])) j++; out += span("z80-number", line.substr(i, j - i)); i = j; continue; }
        if (ch == '%' && (at(i + 1) == '0' || at(i + 1) == '1')) { int j = i + 1; while (j < n && (line[j] == '0' || line[j] == '1')) j++; out += span("z80-number", line.substr(i, j - i)); i = j; continue; }
        if (std::isdigit((unsigned char)ch)) {
            int j = i; while (j < n && isHex(line[j])) j++;
            if (std::toupper((unsigned char)at(j)) == 'H') j++;
            out += span("z80-number", line.substr(i, j - i)); i = j; continue;
        }
        if (isIdentStart(ch)) {
            int j = i; while (j < n && isIdent(line[j])) j++;
            if (at(j) == ':') j++;
            std::string token = line.substr(i, j - i);
            std::string bare = endsWith(token, ':') ? token.substr(0, token.size() - 1) : token;
            std::string upper = upperStr(bare);
            if (endsWith(token, ':') || (!token.empty() && token[0] == '.')) out += span("z80-label", token);
            else if (DATA_DIRECTIVES.count(upper)) out += span("z80-data", token);
            else if (DIRECTIVES.count(upper)) out += span("z80-directive", token);
            else if (OPCODES.count(upper)) out += span("z80-opcode", token);
            else if (REGISTERS.count(upper)) out += span("z80-register", token);
            else if (upper == "BREAKPOINT") out += span("z80-error", token);
            else out += span("z80-ident", token);
            i = j; continue;
        }
        if (isOp(ch) || std::string("(),[]").find(ch) != std::string::npos) { out += span("z80-operator", std::string(1, ch)); i += 1; continue; }
        out += escapeHtml(std::string(1, ch)); i += 1;
    }
    return out;
}

std::string highlightZ80Assembly(const std::string& source) {
    std::string text; for (char c : source) if (c != '\r') text += c;
    std::vector<std::string> lines; { std::string cur; for (char c : text) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; } lines.push_back(cur); }
    std::string out; for (size_t i = 0; i < lines.size(); i++) { out += highlightLine(lines[i]); if (i + 1 < lines.size()) out += "\n"; }
    return out;
}

const std::string z80SyntaxCss =
    "\n.z80-comment{color:var(--muted);font-style:italic;}"
    "\n.z80-opcode{color:var(--accent);font-weight:800;}"
    "\n.z80-register{color:var(--accent2);font-weight:700;}"
    "\n.z80-number{color:var(--amber-hot);font-weight:700;}"
    "\n.z80-string,.z80-char{color:var(--accent-hot);}"
    "\n.z80-label{color:var(--accent2);font-weight:800;text-decoration:underline;text-decoration-thickness:1px;text-underline-offset:2px;}"
    "\n.z80-directive{color:var(--amber);font-weight:800;}"
    "\n.z80-data{color:var(--cyan);font-weight:800;}"
    "\n.z80-operator{color:var(--muted);font-weight:700;}"
    "\n.z80-ident{color:var(--text);}"
    "\n.z80-error{color:#ff8f7a;background:rgba(217,72,54,.18);font-weight:800;}\n";

} // namespace cpcse
