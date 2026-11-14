// CPCSyntaxError — Locomotive BASIC decoder.
#include "basic.h"
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace cpcse {

static void putReal(Bytes& out, double v);

// A five-byte real as BASIC's own LIST prints it. Read off BASIC 1.1 in the machine:
//   6.4  0.3  0.001  0.0001234  0.00001  3.14159  123456789  100000.5
//   1234567890 -> 1.23456789E+09   1E10 -> 1E+10   2.5E-20 -> 2.5E-20   0.1234567891 -> 0.123456789
// so: at most 9 significant digits, no trailing zeros, and E notation (sign, two digits) only
// when the number does not fit in 9 digit places.
// The mantissa has 32 bits, about 9.6 digits: printed as a PC double (17 digits) 0.3 came out
// as 0.30000000004656613. The digits are the FEWEST that give the same five bytes back
// (putReal), so "From the machine" then "To the machine" leaves the program as it was; a
// value that needs a 10th digit for that gets it.
static std::string realToText(const uint8_t bytes[5]) {
    if (bytes[4] == 0) return "0";                       // exponent 0: the value 0
    double f = ((double)(bytes[3] & 0x7F) * 16777216.0) + (bytes[2] << 16) + (bytes[1] << 8) + bytes[0];
    f = 1.0 + (f / 2147483648.0);
    const double val = std::ldexp(f, bytes[4] - 129);    // the magnitude
    const bool negative = (bytes[3] & 0x80) != 0;
    std::string digits;
    int exponent = 0;                                    // val = d.ddd x 10^exponent
    for (int p = 1; p <= 10; p++) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*e", p - 1, val);
        Bytes back;
        putReal(back, std::strtod(buf, nullptr));
        digits.clear();
        const char* e = std::strchr(buf, 'e');
        for (const char* c = buf; c < e; c++) if (*c >= '0' && *c <= '9') digits += *c;
        exponent = std::atoi(e + 1);
        if (back[0] == bytes[0] && back[1] == bytes[1] && back[2] == bytes[2]
            && (back[3] & 0x7f) == (bytes[3] & 0x7f) && back[4] == bytes[4]) break;
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    const int count = (int)digits.size();
    std::string out = negative ? "-" : "";
    if (exponent >= 0 && exponent <= 8) {                // 1 to 9 digits before the point
        for (int i = 0; i <= exponent; i++) out += i < count ? digits[(size_t)i] : '0';
        if (count > exponent + 1) out += "." + digits.substr((size_t)exponent + 1);
    } else if (exponent < 0 && -exponent - 1 + count <= (count > 9 ? 10 : 9)) {   // 0.000ddd, 9 places at most (10 for a 10th digit)
        out += "0." + std::string((size_t)(-exponent - 1), '0') + digits;
    } else {
        out += digits.substr(0, 1);
        if (count > 1) out += "." + digits.substr(1);
        char e[8];
        std::snprintf(e, sizeof(e), "E%c%02d", exponent < 0 ? '-' : '+', std::abs(exponent));
        out += e;
    }
    return out;
}

BasicDecoder::BasicDecoder() {
    Keywords = {
        "AFTER","AUTO","BORDER","CALL","CAT","CHAIN","CLEAR","CLG",
        "CLOSEIN","CLOSEOUT","CLS","CONT","DATA","DEF","DEFINT","DEFREAL",
        "DEFSTR","DEG","DELETE","DIM","DRAW","DRAWR","EDIT","ELSE","END",
        "ENT","ENV","ERASE","ERROR","EVERY","FOR","GOSUB","GOTO","IF","INK",
        "INPUT","KEY","LET","LINE","LIST","LOAD","LOCATE","MEMORY","MERGE",
        "MID$","MODE","MOVE","MOVER","NEXT","NEW","ON","ON BREAK","ON ERROR GOTO",
        "ON SQ","OPENIN","OPENOUT","ORIGIN","OUT","PAPER","PEN","PLOT","PLOTR",
        "POKE","PRINT","'","RAD","RANDOMIZE","READ","RELEASE","REM","RENUM",
        "RESTORE","RESUME","RETURN","RUN","SAVE","SOUND","SPEED","STOP","SYMBOL",
        "TAG","TAGOFF","TROFF","TRON","WAIT","WEND","WHILE","WIDTH","WINDOW",
        "WRITE","ZONE","DI","EI","FILL","GRAPHICS","MASK","FRAME","CURSOR","",
        "ERL","FN","SPC","STEP","SWAP","","","TAB","THEN","TO","USING",
        ">","=",">=","<","<>","<=","+","-","*","/","^","\\","AND","MOD","OR","XOR","NOT",""
    };
    std::vector<std::string> add(256, "");
    const char* first[] = { "ABS","ASC","ATN","CHR$","CINT","COS","CREAL","EXP","FIX","FRE","INKEY","INP","INT","JOY","LEN","LOG","LOG10","LOWER$","PEEK","REMAIN","SGN","SIN","SPACE$","SQ","SQR","STR$","TAN","UNT","UPPER$","VAL" };
    for (int i = 0; i < 30; i++) add[i] = first[i];
    add[64] = "EOF"; add[65] = "ERR"; add[66] = "HIMEM"; add[67] = "INKEY$"; add[68] = "PI"; add[69] = "RND"; add[70] = "TIME"; add[71] = "XPOS"; add[72] = "YPOS"; add[73] = "DERR";
    // &FF71-&FF7F (BASIC itself stores LEFT$ as &FF75, HEX$ as &FF73: cpcse-basic-check)
    add[113] = "BIN$"; add[114] = "DEC$"; add[115] = "HEX$"; add[116] = "INSTR"; add[117] = "LEFT$"; add[118] = "MAX"; add[119] = "MIN"; add[120] = "POS"; add[121] = "RIGHT$"; add[122] = "ROUND"; add[123] = "STRING$"; add[124] = "TEST"; add[125] = "TESTR"; add[126] = "COPYCHR$"; add[127] = "VPOS";
    AdditionalKeywords = add;
}

std::string BasicDecoder::decodeLine(const std::function<int(int)>& read, int startAddr, int length) {
    std::string result;
    int charPos = startAddr;
    int endAddr = startAddr + length;
    auto fetch = [&]() -> int { if (charPos >= endAddr) return 0; int val = read(charPos); charPos++; return val; };
    auto fetchWord = [&]() -> int { int l = fetch(); int h = fetch(); return (h << 8) | l; };
    auto displayVariable = [&]() { int ch; do { ch = fetch(); if (ch == 0) break; result += (char)(ch & 0x7F); } while ((ch & 0x80) == 0 && charPos < endAddr); };

    while (charPos < endAddr) {
        int token = fetch();
        if (token == 0) break;
        switch (token) {
            case 0x01:
                if (charPos < endAddr) { int next = read(charPos); if (next == 0x97 || next == 0xC0) { continue; } }
                result += ":"; break;
            case 0x02: fetch(); fetch(); displayVariable(); result += "%"; break;
            case 0x03: fetch(); fetch(); displayVariable(); result += "$"; break;
            case 0x04: fetch(); fetch(); displayVariable(); result += "!"; break;
            case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
                fetch(); fetch(); displayVariable(); break;
            case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
                result += std::to_string(token - 0x0E); break;
            case 0x19: result += std::to_string(fetch()); break;
            case 0x1A: { int num = fetchWord(); result += std::to_string(num); break; }
            case 0x1B: {
                int num = fetchWord(); int bIgnoreDigit = 1; result += "&X";
                for (int j = 0; j < 16; j++) { int bd = (num >> (15 - j)) & 0x1; if (bd != 0) bIgnoreDigit = 0; if (bIgnoreDigit == 0) result += std::to_string(bd); }
                if (bIgnoreDigit == 1) result += "0";
                break;
            }
            case 0x1C: {
                int num = fetchWord(); char b[8]; std::snprintf(b, sizeof(b), "%X", num & 0xffff); std::string hex = b;
                if (hex != "0") { while (!hex.empty() && hex[0] == '0') hex = hex.substr(1); }
                result += "&" + hex; break;
            }
            case 0x1D: { int ptr = fetchWord(); int lsb = read((ptr + 3) & 0xffff); int msb = read((ptr + 4) & 0xffff); result += std::to_string((msb << 8) | lsb); break; }
            case 0x1E: result += std::to_string(fetchWord()); break;
            case 0x1F: {
                uint8_t real[5];
                for (uint8_t& b : real) b = (uint8_t)fetch();
                result += realToText(real); break;
            }
            case 0x22: {
                result += "\"";
                int ch;
                do { ch = fetch(); if (ch != 0) { result += (char)ch; if (ch == 0x22) break; } } while (ch != 0 && charPos < endAddr);
                break;
            }
            case 0xFF: {
                int kw = fetch();
                if (kw >= 0 && kw < (int)AdditionalKeywords.size() && !AdditionalKeywords[kw].empty()) result += AdditionalKeywords[kw];
                else { char b[8]; std::snprintf(b, sizeof(b), "[%x]", kw); result += b; }
                break;
            }
            case 0x7C: { result += "|"; fetch(); displayVariable(); break; }
            default:
                if (token >= 0x80 && token <= 0xFE) {
                    const std::string& kw = Keywords[token - 0x80];
                    if (!kw.empty()) result += kw;
                    if (token == 0xBF && charPos < endAddr) {
                        int next = read(charPos);
                        if (next == 0xFF || (next >= 0x80 && next <= 0xFE) || (next >= 0x02 && next <= 0x0D)) result += " ";
                    }
                } else if (token >= 0x20 && token <= 0x7F) {
                    result += (char)token;
                } else {
                    if (!Keywords[token & 0x7F].empty()) result += Keywords[token & 0x7F];
                }
        }
    }
    return result;
}

// ======================================================================= the tokenizer

static bool isLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }
static char upperChar(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// BASIC's five-byte real: a 32-bit mantissa (its top bit, always 1, holds the sign) and an
// exponent biased by 128; the value is 0.1mmm... x 2^(exponent-128). 0 is all zeros.
static void putReal(Bytes& out, double v) {
    if (v == 0.0 || std::isnan(v)) { for (int i = 0; i < 5; i++) out.push_back(0); return; }
    const bool negative = v < 0;
    v = std::fabs(v);
    // BASIC's range: exponent bytes 1-255, 2^-128 to just under 2^127. Typed text can say
    // more ("1E999" is infinity, and (int) of it is undefined): the largest or 0 then.
    if (std::isinf(v) || v >= 1.7014118e38) {
        out.push_back(0xff); out.push_back(0xff); out.push_back(0xff); out.push_back(negative ? 0xff : 0x7f); out.push_back(0xff);
        return;
    }
    if (v < 2.9387359e-39) { for (int i = 0; i < 5; i++) out.push_back(0); return; }
    int e = (int)std::floor(std::log2(v));
    double f = v / std::ldexp(1.0, e);                 // 1 <= f < 2
    if (f >= 2.0) { f /= 2.0; e++; }
    if (f < 1.0) { f *= 2.0; e--; }
    double m = std::floor((f - 1.0) * 2147483648.0 + 0.5);
    if (m >= 2147483648.0) { m = 0; e++; }
    const uint32_t mant = (uint32_t)m;
    out.push_back((uint8_t)(mant & 0xff));
    out.push_back((uint8_t)(mant >> 8 & 0xff));
    out.push_back((uint8_t)(mant >> 16 & 0xff));
    out.push_back((uint8_t)((mant >> 24 & 0x7f) | (negative ? 0x80 : 0)));
    out.push_back((uint8_t)(e + 129));
}

Bytes BasicDecoder::tokenizeLine(const std::string& text, std::string& error) const {
    error.clear();
    size_t i = 0;
    const size_t n = text.size();
    while (i < n && text[i] == ' ') i++;
    if (i >= n || !isDigit(text[i])) { error = "a line must start with its number"; return {}; }
    long number = 0;
    while (i < n && isDigit(text[i])) { number = number * 10 + (text[i] - '0'); if (number > 65535) break; i++; }
    if (number < 1 || number > 65535) { error = "the line number must be 1 to 65535"; return {}; }
    if (i < n && text[i] == ' ') i++;                  // the space after the number is not kept

    // the keywords: statements &80-&FE, functions &FF nn
    auto keyword = [&](const std::string& word, int& token, bool& function) {
        for (size_t k = 0; k < Keywords.size(); k++)
            if (!Keywords[k].empty() && Keywords[k] == word && isLetter(word[0])) { token = 0x80 + (int)k; function = false; return true; }
        for (size_t k = 0; k < AdditionalKeywords.size(); k++)
            if (!AdditionalKeywords[k].empty() && AdditionalKeywords[k] == word) { token = (int)k; function = true; return true; }
        return false;
    };
    auto tokenOf = [&](const char* word) {
        for (size_t k = 0; k < Keywords.size(); k++) if (Keywords[k] == word) return 0x80 + (int)k;
        return -1;
    };
    // after these, a number is a line number (and stays one across "," and "-")
    static const char* LINE_WORDS[] = { "GOTO", "GOSUB", "THEN", "ELSE", "RESTORE", "RUN", "LIST", "DELETE", "EDIT", "RENUM", "AUTO", "RESUME" };

    Bytes out;
    bool lineNumbers = false;
    while (i < n) {
        const char c = text[i];
        if (c == '"') {                                // a string, as typed
            out.push_back((uint8_t)c); i++;
            while (i < n) { out.push_back((uint8_t)text[i]); if (text[i++] == '"') break; }
            continue;
        }
        if (c == ':') { out.push_back(0x01); i++; lineNumbers = false; continue; }
        if (c == '\'') {                               // a comment: ' is ":REM", the rest as typed
            out.push_back(0x01); out.push_back(0xc0); i++;
            while (i < n) out.push_back((uint8_t)text[i++]);
            break;
        }
        if (c == '|') {                                // an RSX: its name, the last letter marked
            out.push_back(0x7c); out.push_back(0x00); i++;
            size_t start = i;
            while (i < n && (isLetter(text[i]) || isDigit(text[i]) || text[i] == '.')) i++;
            for (size_t k = start; k < i; k++) out.push_back((uint8_t)(upperChar(text[k]) | (k + 1 == i ? 0x80 : 0)));
            lineNumbers = false;
            continue;
        }
        if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(text[i + 1]))) {
            size_t start = i;
            while (i < n && isDigit(text[i])) i++;
            bool real = false;
            if (i < n && text[i] == '.') { real = true; i++; while (i < n && isDigit(text[i])) i++; }
            if (i < n && (text[i] == 'E' || text[i] == 'e')) {
                size_t j = i + 1;
                if (j < n && (text[j] == '+' || text[j] == '-')) j++;
                if (j < n && isDigit(text[j])) { real = true; i = j; while (i < n && isDigit(text[i])) i++; }
            }
            const std::string num = text.substr(start, i - start);
            const double v = std::strtod(num.c_str(), nullptr);
            if (lineNumbers && !real && v <= 65535) {
                out.push_back(0x1e); out.push_back((uint8_t)((int)v & 0xff)); out.push_back((uint8_t)((int)v >> 8));
            } else if (!real && v <= 9) out.push_back((uint8_t)(0x0e + (int)v));
            else if (!real && v <= 255) { out.push_back(0x19); out.push_back((uint8_t)v); }
            else if (!real && v <= 32767) { out.push_back(0x1a); out.push_back((uint8_t)((int)v & 0xff)); out.push_back((uint8_t)((int)v >> 8)); }
            else if (!(v < 1.7014118e38)) out.insert(out.end(), num.begin(), num.end());   // past BASIC's range: BASIC keeps the text ("1E999")
            else { out.push_back(0x1f); putReal(out, v); }
            continue;
        }
        if (c == '&') {                                // &hex, &Hhex, &Xbinary
            size_t j = i + 1;
            const bool binary = j < n && (text[j] == 'X' || text[j] == 'x');
            if (binary || (j < n && (text[j] == 'H' || text[j] == 'h'))) j++;
            long v = 0;
            size_t digits = j;
            while (j < n && (binary ? (text[j] == '0' || text[j] == '1') : std::isxdigit((unsigned char)text[j]))) {
                v = v * (binary ? 2 : 16) + (isDigit(text[j]) ? text[j] - '0' : (upperChar(text[j]) - 'A' + 10));
                j++;
            }
            if (j > digits) {
                out.push_back(binary ? 0x1b : 0x1c); out.push_back((uint8_t)(v & 0xff)); out.push_back((uint8_t)(v >> 8 & 0xff));
                i = j;
                continue;
            }
        }
        if (isLetter(c)) {
            size_t start = i;
            while (i < n && (isLetter(text[i]) || isDigit(text[i]) || text[i] == '.')) i++;
            std::string word;
            for (size_t k = start; k < i; k++) word += upperChar(text[k]);
            if (i < n && text[i] == '$') {             // CHR$, MID$ ...
                int t; bool f;
                if (keyword(word + "$", t, f)) { word += "$"; i++; }
            }
            // the phrases ON BREAK and ON SQ are one token each (ON ERROR GOTO, typed, is
            // three: ON, ERROR, GOTO)
            if (word == "ON") {
                size_t j = i;
                while (j < n && text[j] == ' ') j++;
                auto follows = [&](const char* w) {
                    size_t k = j;
                    for (const char* p = w; *p; p++, k++) {
                        if (*p == ' ') { if (k >= n || text[k] != ' ') return (size_t)0; while (k + 1 < n && text[k + 1] == ' ') k++; continue; }
                        if (k >= n || upperChar(text[k]) != *p) return (size_t)0;
                    }
                    return k;
                };
                size_t e;
                if ((e = follows("BREAK")) != 0) { out.push_back((uint8_t)tokenOf("ON BREAK")); i = e; lineNumbers = false; continue; }
                if ((e = follows("SQ")) != 0) { out.push_back((uint8_t)tokenOf("ON SQ")); i = e; lineNumbers = false; continue; }
            }
            int token; bool function;
            if (keyword(word, token, function)) {
                if (function) { out.push_back(0xff); out.push_back((uint8_t)token); lineNumbers = false; continue; }
                if (word == "ELSE") out.push_back(0x01);       // ELSE is stored after a ':'
                out.push_back((uint8_t)token);
                if (word == "REM") { while (i < n) out.push_back((uint8_t)text[i++]); break; }
                if (word == "DATA") {                          // the data, as typed, to the end of the statement
                    bool quoted = false;
                    while (i < n && (quoted || text[i] != ':')) { if (text[i] == '"') quoted = !quoted; out.push_back((uint8_t)text[i++]); }
                    continue;
                }
                lineNumbers = false;
                for (const char* w : LINE_WORDS) if (word == w) lineNumbers = true;
                continue;
            }
            // FNname: the FN token, then the name as a variable
            if (word.size() > 2 && word.compare(0, 2, "FN") == 0) {
                out.push_back((uint8_t)tokenOf("FN"));
                start += 2;
                word.erase(0, 2);
            }
            // a variable: its type from the suffix, a 2-byte slot BASIC fills when it runs,
            // the name as typed with its last character marked
            int type = 0x0d;
            if (i < n && text[i] == '%') { type = 0x02; i++; }
            else if (i < n && text[i] == '$') { type = 0x03; i++; }
            else if (i < n && text[i] == '!') { type = 0x04; i++; }
            out.push_back((uint8_t)type); out.push_back(0); out.push_back(0);
            const size_t end = start + word.size();
            for (size_t k = start; k < end; k++) out.push_back((uint8_t)(text[k] | (k + 1 == end ? 0x80 : 0)));
            lineNumbers = false;
            continue;
        }
        // operators
        auto op = [&](const char* s) { return text.compare(i, std::strlen(s), s) == 0; };
        const char* ops[] = { "<>", "<=", ">=", ">", "=", "<", "+", "-", "*", "/", "^", "\\" };
        bool done = false;
        for (const char* o : ops)
            if (op(o)) {
                out.push_back((uint8_t)tokenOf(o));
                i += std::strlen(o);
                done = true;
                if (std::strcmp(o, "-") != 0) lineNumbers = false;   // LIST 10-20: still line numbers
                break;
            }
        if (done) continue;
        if (c != ' ' && c != ',') lineNumbers = false;  // ON x GOTO 10,20: still line numbers
        out.push_back((uint8_t)c);
        i++;
    }
    out.push_back(0x00);
    const int length = (int)out.size() + 4;
    if (length > 255 + 4) { error = "the line is longer than BASIC allows"; return {}; }
    Bytes line = { (uint8_t)(length & 0xff), (uint8_t)(length >> 8), (uint8_t)(number & 0xff), (uint8_t)(number >> 8) };
    line.insert(line.end(), out.begin(), out.end());
    return line;
}

Bytes BasicDecoder::tokenizeProgram(const std::string& text, std::string& error) const {
    error.clear();
    std::map<int, Bytes> lines;
    size_t at = 0;
    int lineNo = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string s = text.substr(at, end - at);
        if (!s.empty() && s.back() == '\r') s.pop_back();
        lineNo++;
        if (s.find_first_not_of(" \t") != std::string::npos) {
            std::string why;
            Bytes b = tokenizeLine(s, why);
            if (b.empty()) { error = "line " + std::to_string(lineNo) + ": " + why; return {}; }
            lines[b[2] | b[3] << 8] = b;
        }
        at = end + 1;
    }
    Bytes out;
    for (auto& [num, b] : lines) out.insert(out.end(), b.begin(), b.end());
    out.push_back(0); out.push_back(0);
    return out;
}

std::vector<BasicLine> BasicDecoder::listProgram(const std::function<int(int)>& read, int startAddr) {
    std::vector<BasicLine> lines;
    int baspos = startAddr;
    int safeLoop = 0;
    while (safeLoop++ < 10000) {
        int lenL = read(baspos);
        int lenH = read((baspos + 1) & 0xffff);
        int linelength = (lenH << 8) | lenL;
        if (linelength == 0) break;
        int numL = read((baspos + 2) & 0xffff);
        int numH = read((baspos + 3) & 0xffff);
        int linenumber = (numH << 8) | numL;
        std::string text = decodeLine(read, (baspos + 4) & 0xffff, linelength - 4);
        lines.push_back({ baspos, linenumber, text });
        baspos = (baspos + linelength) & 0xffff;
    }
    return lines;
}

} // namespace cpcse
