// CPCSyntaxError — Locomotive BASIC tokenizer.
#include "basictokenizer.h"
#include <map>

namespace cpcse {

const std::vector<std::string> BASIC_KEYWORDS = {
    "AFTER", "AUTO", "BORDER", "CALL", "CAT", "CHAIN", "CLEAR", "CLG",
    "CLOSEIN", "CLOSEOUT", "CLS", "CONT", "DATA", "DEF", "DEFINT",
    "DEFREAL", "DEFSTR", "DEG", "DELETE", "DIM", "DRAW", "DRAWR", "EDIT",
    "ELSE", "END", "ENT", "ENV", "ERASE", "ERROR", "EVERY", "FOR",
    "GOSUB", "GOTO", "IF", "INK", "INPUT", "KEY", "LET", "LINE", "LIST",
    "LOAD", "LOCATE", "MEMORY", "MERGE", "MID$", "MODE", "MOVE", "MOVER",
    "NEXT", "NEW", "ON", "", "", "SQ", "OPENIN",
    "OPENOUT", "ORIGIN", "OUT", "PAPER", "PEN", "PLOT", "PLOTR", "POKE",
    "PRINT", "'", "RAD", "RANDOMIZE", "READ", "RELEASE", "REM", "RENUM",
    "RESTORE", "RESUME", "RETURN", "RUN", "SAVE", "SOUND", "SPEED", "STOP",
    "SYMBOL", "TAG", "TAGOFF", "TROFF", "TRON", "WAIT", "WEND", "WHILE",
    "WIDTH", "WINDOW", "WRITE", "ZONE", "DI", "EI", "FILL", "GRAPHICS",
    "MASK", "FRAME", "CURSOR", "", "ERL", "FN", "SPC", "STEP", "SWAP",
    "", "", "TAB", "THEN", "TO", "USING",
    ">", "=", ">=", "<", "<>", "<=",
    "+", "-", "*", "/", "^", "\\", "AND", "MOD", "OR", "XOR", "NOT",
    ""
};

static const std::vector<std::pair<int, std::string>>& functions() {
    static const std::vector<std::pair<int, std::string>> f = {
        {0x00,"ABS"},{0x01,"ASC"},{0x02,"ATN"},{0x03,"CHR$"},{0x04,"CINT"},
        {0x05,"COS"},{0x06,"CREAL"},{0x07,"EXP"},{0x08,"FIX"},{0x09,"FRE"},
        {0x0A,"INKEY"},{0x0B,"INP"},{0x0C,"INT"},{0x0D,"JOY"},{0x0E,"LEN"},
        {0x0F,"LOG"},{0x10,"LOG10"},{0x11,"LOWER$"},{0x12,"PEEK"},{0x13,"REMAIN"},
        {0x14,"SGN"},{0x15,"SIN"},{0x16,"SPACE$"},{0x17,"SQ"},{0x18,"SQR"},
        {0x19,"STR$"},{0x1A,"TAN"},{0x1B,"UNT"},{0x1C,"UPPER$"},{0x1D,"VAL"},
        {0x40,"EOF"},{0x41,"ERR"},{0x42,"HIMEM"},{0x43,"INKEY$"},{0x44,"PI"},
        {0x45,"RND"},{0x46,"TIME"},{0x47,"XPOS"},{0x48,"YPOS"},{0x49,"DERR"},
        {0x74,"INSTR"},{0x75,"LEFT$"},{0x76,"MAX"},{0x77,"MIN"},{0x78,"POS"},
        {0x79,"RIGHT$"},{0x7A,"ROUND"},{0x7B,"STRING$"},{0x7C,"TEST"},
        {0x7D,"TESTR"},{0x7E,"COPYCHR$"},{0x7F,"VPOS"}
    };
    return f;
}

struct BasicTokenMaps {
    std::unordered_map<std::string, int> kwMap;
    std::unordered_map<std::string, int> fnMap;
    std::vector<std::string> sortedKw;
    std::vector<std::string> sortedFn;
};
static const BasicTokenMaps& buildBasicTokenMaps() {
    static BasicTokenMaps maps = [] {
        BasicTokenMaps m;
        for (int i = 0; i < (int)BASIC_KEYWORDS.size(); i++) if (!BASIC_KEYWORDS[i].empty()) { if (!m.kwMap.count(BASIC_KEYWORDS[i])) m.sortedKw.push_back(BASIC_KEYWORDS[i]); m.kwMap[BASIC_KEYWORDS[i]] = 0x80 + i; }
        for (auto& kv : functions()) { m.fnMap[kv.second] = kv.first; m.sortedFn.push_back(kv.second); }
        std::stable_sort(m.sortedKw.begin(), m.sortedKw.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        std::stable_sort(m.sortedFn.begin(), m.sortedFn.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        return m;
    }();
    return maps;
}

static std::string upperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string trimStr(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }

static std::vector<int> encodeBasicFloat(double f) {
    if (f == 0.0) return { 0x1F, 0, 0, 0, 0, 0 };
    int sign = f < 0 ? 1 : 0;
    f = std::fabs(f);
    int exp = (int)std::floor(std::log(f) / std::log(2.0)) + 1;
    long long mm = (long long)std::floor((f / std::pow(2.0, exp)) * std::pow(2.0, 32));
    unsigned m = (unsigned)mm & 0x7FFFFFFFu;
    if (sign == 1) m |= 0x80000000u;
    return { 0x1F, (int)(m & 0xFF), (int)((m >> 8) & 0xFF), (int)((m >> 16) & 0xFF), (int)((m >> 24) & 0xFF), (exp + 129) & 0xFF };
}
static std::vector<int> encodeBasicNumber(std::string s, bool asLineRef) {
    s = trimStr(s);
    auto isHexStr = [](const std::string& t) { if (t.size() < 2 || t[0] != '&') return false; for (size_t i = 1; i < t.size(); i++) if (!std::isxdigit((unsigned char)t[i])) return false; return true; };
    if (isHexStr(s)) { int val = (int)std::stol(s.substr(1), nullptr, 16) & 0xFFFF; return { 0x1C, val & 0xFF, (val >> 8) & 0xFF }; }
    if (s.size() >= 3 && s[0] == '&' && (s[1] == 'X' || s[1] == 'x')) { bool bin = true; for (size_t i = 2; i < s.size(); i++) if (s[i] != '0' && s[i] != '1') { bin = false; break; } if (bin) { int val = (int)std::stol(s.substr(2), nullptr, 2) & 0xFFFF; return { 0x1B, val & 0xFF, (val >> 8) & 0xFF }; } }
    if (s.find('.') != std::string::npos || upperStr(s).find('E') != std::string::npos) {
        try { return encodeBasicFloat(std::stod(s)); } catch (...) {}
    }
    try {
        int v = (int)std::stol(s, nullptr, 10);
        if (asLineRef) return { 0x1E, v & 0xFF, (v >> 8) & 0xFF };
        if (v >= 0 && v <= 9) return { 0x0E + v };
        if (v >= 0 && v <= 255) return { 0x19, v };
        if (v >= 0 && v <= 65535) return { 0x1A, v & 0xFF, (v >> 8) & 0xFF };
        return encodeBasicFloat(v);
    } catch (...) {
        std::vector<int> bytes; for (char c : s) bytes.push_back((unsigned char)c); return bytes;
    }
}

static std::vector<int> tokenizeLine(const std::string& text) {
    const BasicTokenMaps& maps = buildBasicTokenMaps();
    const auto& kwMap = maps.kwMap;
    const auto& fnMap = maps.fnMap;
    const auto& sortedKw = maps.sortedKw;
    const auto& sortedFn = maps.sortedFn;
    std::vector<int> out;
    int i = 0, n = (int)text.size();
    bool inStr = false, inRem = false;
    std::string upper = upperStr(text);
    bool expectLinenum = false, afterOnGoto = false;
    int lastTok = -1;
    auto kw = [&](const std::string& k) { auto it = kwMap.find(k); return it != kwMap.end() ? it->second : 0; };
    std::vector<int> LINENUM_KW = { kw("GOTO"), kw("GOSUB"), kw("RESTORE"), kw("LIST"), kw("RUN") };
    int THEN_TOK = kw("THEN"), ON_TOK = kw("ON");
    auto ch = [&](int k) { return k >= 0 && k < n ? text[k] : '\0'; };
    while (i < n) {
        char c = text[i];
        if (inStr) { out.push_back((unsigned char)c); if (c == '"') inStr = false; i++; continue; }
        if (inRem) { out.push_back((unsigned char)c); i++; continue; }
        if (c == '"') { out.push_back((unsigned char)c); inStr = true; i++; continue; }
        if (c == ':') { out.push_back(0x01); expectLinenum = false; afterOnGoto = false; i++; continue; }
        if (c == ',') { out.push_back(0x2C); i++; continue; }
        if (c == '|') {
            out.push_back(0x7C); i++;
            while (i < n && text[i] == ' ') i++;
            int j = i;
            while (j < n) { char u = (char)std::toupper((unsigned char)text[j]); if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_' || u == '.') j++; else break; }
            std::string name = upperStr(text.substr(i, j - i));
            if (!name.empty()) { out.push_back(0x00); for (int k = 0; k < (int)name.size(); k++) { int code = (unsigned char)name[k]; out.push_back(k == (int)name.size() - 1 ? (code | 0x80) : code); } }
            i = j; expectLinenum = false; continue;
        }
        if (std::isdigit((unsigned char)c) || (c == '&' && i + 1 < n && (std::isxdigit((unsigned char)ch(i + 1)) || std::toupper((unsigned char)ch(i + 1)) == 'X'))) {
            bool useLineRef = expectLinenum; int j;
            if (c == '&') {
                j = i + 1;
                if (j < n && std::toupper((unsigned char)text[j]) == 'X') { j++; while (j < n && (text[j] == '0' || text[j] == '1')) j++; }
                else { while (j < n && std::isxdigit((unsigned char)text[j])) j++; }
                auto encoded = encodeBasicNumber(text.substr(i, j - i), false);
                out.insert(out.end(), encoded.begin(), encoded.end());
            } else {
                j = i;
                while (j < n && (std::isdigit((unsigned char)text[j]) || text[j] == '.')) j++;
                if (j < n && std::toupper((unsigned char)text[j]) == 'E') { j++; if (j < n && (text[j] == '+' || text[j] == '-')) j++; while (j < n && std::isdigit((unsigned char)text[j])) j++; }
                std::string numStr = text.substr(i, j - i);
                bool isFloat = numStr.find('.') != std::string::npos || upperStr(numStr).find('E') != std::string::npos;
                auto encoded = encodeBasicNumber(numStr, useLineRef && !isFloat);
                out.insert(out.end(), encoded.begin(), encoded.end());
            }
            if (!afterOnGoto) expectLinenum = false;
            i = j; continue;
        }
        if (c == ' ') { out.push_back(0x20); i++; continue; }
        { char u = (char)std::toupper((unsigned char)c);
          if ((u >= 'A' && u <= 'Z') || u == '\'') {
            std::string ru = upper.substr(i);
            std::string matchedKw;
            for (const std::string& k : sortedKw) {
                if (ru.compare(0, k.size(), k) == 0) {
                    int ep = i + (int)k.size();
                    char lc = k.back();
                    if ((std::isalpha((unsigned char)lc)) || lc == '$') {
                        if (ep < n) { char nc = (char)std::toupper((unsigned char)text[ep]); if ((nc >= 'A' && nc <= 'Z') || (nc >= '0' && nc <= '9') || nc == '$' || nc == '%' || nc == '!' || nc == '_') continue; }
                    }
                    matchedKw = k; break;
                }
            }
            if (!matchedKw.empty()) {
                int tok = kwMap.at(matchedKw);
                if (matchedKw == "ELSE") out.push_back(0x01);
                out.push_back(tok);
                i += (int)matchedKw.size();
                if (matchedKw == "REM" || matchedKw == "'") { inRem = true; continue; }
                expectLinenum = std::find(LINENUM_KW.begin(), LINENUM_KW.end(), tok) != LINENUM_KW.end();
                if (tok == THEN_TOK) { int j = i; while (j < n && text[j] == ' ') j++; expectLinenum = j < n && std::isdigit((unsigned char)text[j]); }
                afterOnGoto = (tok == kw("GOTO") || tok == kw("GOSUB")) && lastTok != -1 && lastTok == ON_TOK;
                lastTok = tok;
                continue;
            }
            std::string matchedFn;
            for (const std::string& fn : sortedFn) {
                if (ru.compare(0, fn.size(), fn) == 0) {
                    int ep = i + (int)fn.size();
                    char lc = fn.back();
                    if (std::isalpha((unsigned char)lc)) {
                        if (ep < n) { char nc = (char)std::toupper((unsigned char)text[ep]); if ((nc >= 'A' && nc <= 'Z') || (nc >= '0' && nc <= '9') || nc == '$' || nc == '%' || nc == '!' || nc == '_') continue; }
                    }
                    matchedFn = fn; break;
                }
            }
            if (!matchedFn.empty()) { out.push_back(0xFF); out.push_back(fnMap.at(matchedFn)); i += (int)matchedFn.size(); expectLinenum = false; continue; }
            int j = i;
            while (j < n) { char cc = text[j]; if (std::isalnum((unsigned char)cc) || cc == '_') j++; else break; }
            std::string vname = text.substr(i, j - i);
            if (j < n && (text[j] == '$' || text[j] == '%' || text[j] == '!')) { vname += text[j]; j++; }
            int tb; std::string core;
            char last = vname.empty() ? '\0' : vname.back();
            if (last == '$') { tb = 0x03; core = vname.substr(0, vname.size() - 1); }
            else if (last == '%') { tb = 0x02; core = vname.substr(0, vname.size() - 1); }
            else if (last == '!') { tb = 0x04; core = vname.substr(0, vname.size() - 1); }
            else { tb = 0x0D; core = vname; }
            out.push_back(tb); out.push_back(0x00); out.push_back(0x00);
            for (int k = 0; k < (int)core.size(); k++) { int code = (unsigned char)core[k]; out.push_back(k == (int)core.size() - 1 ? (code | 0x80) : code); }
            expectLinenum = false; lastTok = -1; i = j; continue;
          }
        }
        if (c == '>' || c == '<' || c == '=' || c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '\\') {
            std::string two = upper.substr(i, std::min(2, n - i));
            std::string one = upper.substr(i, 1);
            std::string matchedOp;
            if (kwMap.count(two)) matchedOp = two;
            else if (kwMap.count(one)) matchedOp = one;
            if (!matchedOp.empty()) { out.push_back(kwMap.at(matchedOp)); i += (int)matchedOp.size(); expectLinenum = false; continue; }
            if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) out.push_back((unsigned char)c);
            i++; continue;
        }
        if ((unsigned char)c >= 0x20 && (unsigned char)c <= 0x7E) out.push_back((unsigned char)c);
        i++;
    }
    return out;
}

BasicTokenizeResult tokenizeBasic(const std::string& text) {
    struct LineData { int num; std::string txt; std::string original; };
    std::vector<LineData> lines;
    std::vector<std::string> rawLines; { std::string cur; for (char c : text) { if (c == '\n') { std::string l = cur; if (!l.empty() && l.back() == '\r') l.pop_back(); rawLines.push_back(l); cur.clear(); } else cur += c; } rawLines.push_back(cur); }
    for (std::string line : rawLines) {
        line = trimStr(line);
        if (line.empty()) continue;
        // /^(\d+)\s?(.*)/
        int p = 0; while (p < (int)line.size() && std::isdigit((unsigned char)line[p])) p++;
        if (p == 0) continue;
        int num = (int)std::stol(line.substr(0, p), nullptr, 10);
        int rest = p; if (rest < (int)line.size() && std::isspace((unsigned char)line[rest])) rest++;
        lines.push_back({ num, line.substr(rest), line });
    }
    std::stable_sort(lines.begin(), lines.end(), [](const LineData& a, const LineData& b) { return a.num < b.num; });

    std::vector<int> body;
    BasicTokenizeResult result;
    for (const LineData& lineData : lines) {
        std::vector<int> content = tokenizeLine(lineData.txt);
        int recLen = 2 + 2 + (int)content.size() + 1;
        result.tokenInfo.push_back({ lineData.num, lineData.original, content, recLen });
        body.push_back(recLen & 0xFF);
        body.push_back((recLen >> 8) & 0xFF);
        body.push_back(lineData.num & 0xFF);
        body.push_back((lineData.num >> 8) & 0xFF);
        body.insert(body.end(), content.begin(), content.end());
        body.push_back(0x00);
    }
    body.push_back(0x00); body.push_back(0x00);
    result.binary.assign(body.begin(), body.end());
    return result;
}

Bytes makeBasicAmsdosHeader(const std::string& filename, int dataLen) {
    Bytes h(128, 0);
    std::string base, ext;
    size_t dot = filename.find('.');
    base = upperStr(filename.substr(0, dot == std::string::npos ? filename.size() : dot)).substr(0, 8);
    ext = dot != std::string::npos ? upperStr(filename.substr(dot + 1)).substr(0, 3) : "BAS";
    while (base.size() < 8) base += ' ';
    while (ext.size() < 3) ext += ' ';
    h[0x00] = 0x00;
    for (int i = 0; i < 8; i++) h[0x01 + i] = (uint8_t)base[i];
    for (int i = 0; i < 3; i++) h[0x09 + i] = (uint8_t)ext[i];
    h[0x12] = 0x00;
    h[0x18] = (uint8_t)(dataLen & 0xFF); h[0x19] = (uint8_t)((dataLen >> 8) & 0xFF); h[0x1A] = 0x00;
    h[0x40] = (uint8_t)(dataLen & 0xFF); h[0x41] = (uint8_t)((dataLen >> 8) & 0xFF); h[0x42] = 0x00;
    int chk = 0; for (int i = 0; i < 67; i++) chk += h[i]; chk &= 0xFFFF;
    h[0x43] = (uint8_t)(chk & 0xFF); h[0x44] = (uint8_t)((chk >> 8) & 0xFF);
    return h;
}

} // namespace cpcse
