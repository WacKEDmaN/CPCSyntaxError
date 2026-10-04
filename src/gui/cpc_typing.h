// CPCSyntaxError — which CPC key types a character: letters, digits and the punctuation a
// BASIC line needs ("OUT &BC00,4:OUT &BD00,36", RUN"GAME). Shifted keys follow the CPC's
// own layout, not a PC's. Shared by the headless --type and the command API's "type".
#pragma once
#include <string>

namespace cpcse {

// The KeyboardMatrix code for `c` and whether SHIFT goes with it. false: no key types it.
// '\n' is Enter.
inline bool cpcKeyForChar(char c, std::string& code, bool& shift) {
    shift = false;
    code.clear();
    switch (c) {
        case '\n': code = "Enter"; return true;
        case '"': code = "Digit2"; shift = true; return true;      // CPC: SHIFT+2
        case '&': code = "Digit6"; shift = true; return true;      // CPC: SHIFT+6
        case '#': code = "Digit3"; shift = true; return true;
        case '$': code = "Digit4"; shift = true; return true;
        case '%': code = "Digit5"; shift = true; return true;
        case '(': code = "Digit8"; shift = true; return true;
        case ')': code = "Digit9"; shift = true; return true;
        case '_': code = "Digit0"; shift = true; return true;
        case '=': code = "Minus"; shift = true; return true;       // CPC: the "- =" key
        case '+': code = "Quote"; shift = true; return true;       // CPC: the "; +" key
        case '*': code = "Semicolon"; shift = true; return true;   // CPC: the ": *" key
        case '<': code = "Comma"; shift = true; return true;
        case '>': code = "Period"; shift = true; return true;
        case '?': code = "Slash"; shift = true; return true;
        case ',': code = "Comma"; return true;
        case '.': code = "Period"; return true;
        case ':': code = "Semicolon"; return true;                 // CPC matrix {3,5}
        case ';': code = "Quote"; return true;                     // CPC matrix {3,4}
        case '@': code = "BracketLeft"; return true;               // CPC: the "@ |" key
        case '|': code = "BracketLeft"; shift = true; return true;
        case '^': code = "Equal"; return true;                     // CPC: the "^ £" key
        case '-': code = "Minus"; return true;
        case '/': code = "Slash"; return true;
        case '\\': code = "Backslash"; return true;
        case '[': code = "BracketLeft"; return true;
        case ']': code = "BracketRight"; return true;
        case ' ': code = "Space"; return true;
        default:
            if (c >= 'A' && c <= 'Z') { code = std::string("Key") + c; shift = true; }   // as typed: "Q" is not "q"
            else if (c >= 'a' && c <= 'z') code = std::string("Key") + (char)(c - 32);
            else if (c >= '0' && c <= '9') code = std::string("Digit") + c;
            return !code.empty();
    }
}

} // namespace cpcse
