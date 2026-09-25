// CPCSyntaxError — Locomotive BASIC decoder.
#include "basic.h"
#include <cstdio>
#include <cstdlib>

namespace cpcse {

static std::string jsNumToStr(double val) {
    if (val == (long long)val && std::fabs(val) < 1e15) return std::to_string((long long)val);
    char buf[64];
    for (int p = 1; p <= 17; p++) {
        std::snprintf(buf, sizeof(buf), "%.*g", p, val);
        if (std::strtod(buf, nullptr) == val) break;
    }
    return buf;
}

BasicDecoder::BasicDecoder() {
    Keywords = {
        "AFTER","AUTO","BORDER","CALL","CAT","CHAIN","CLEAR","CLG",
        "CLOSEIN","CLOSEOUT","CLS","CONT","DATA","DEF","DEFINT","DEFREAL",
        "DEFSTR","DEG","DELETE","DIM","DRAW","DRAWR","EDIT","ELSE","END",
        "ENT","ENV","ERASE","ERROR","EVERY","FOR","GOSUB","GOTO","IF","INK",
        "INPUT","KEY","LET","LINE","LIST","LOAD","LOCATE","MEMORY","MERGE",
        "MID$","MODE","MOVE","MOVER","NEXT","NEW","ON","ON BREAK","ON ERROR GOTO",
        "SQ","OPENIN","OPENOUT","ORIGIN","OUT","PAPER","PEN","PLOT","PLOTR",
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
    add[112] = "BIN$"; add[113] = "DEC$"; add[114] = "HEX$"; add[115] = "INSTR"; add[116] = "LEFT$"; add[117] = "MAX"; add[118] = "MIN"; add[119] = "POS"; add[120] = "RIGHT$"; add[121] = "ROUND"; add[122] = "STRING$"; add[123] = "TEST"; add[124] = "TESTR"; add[125] = "COPYCHR$"; add[126] = "VPOS";
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
                int b0 = fetch(), b1 = fetch(), b2 = fetch(), b3 = fetch(), b4 = fetch();
                double f = ((double)(b3 & 0x7F) * 16777216.0) + (b2 << 16) + (b1 << 8) + b0;
                f = 1.0 + (f / 2147483648.0);
                if (b3 & 0x80) f = -f;
                int exp = b4 - 129;
                double val = f * std::pow(2.0, exp);
                std::string e = jsNumToStr(val);
                if (e.find("e-") != std::string::npos) {
                    char buf[64]; std::snprintf(buf, sizeof(buf), "%.9f", val); e = buf;
                    while (!e.empty() && e.back() == '0') e.pop_back();
                }
                result += e; break;
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
