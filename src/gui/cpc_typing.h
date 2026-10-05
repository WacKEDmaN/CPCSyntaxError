// CPCSyntaxError — which CPC key types a character: letters, digits and the punctuation a
// BASIC line needs ("OUT &BC00,4:OUT &BD00,36", RUN"GAME). Shifted keys follow the CPC's
// own layout, not a PC's. Shared by the headless --type and the command API's "type".
#pragma once
#include <string>

#include "core/keyboard.h"

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
        case '`': code = "Backslash"; shift = true; return true;   // the "\ `" key (no ~ on a CPC)
        case '~': return false;   // keyboard.cpp's host map sends a PC's ~ to that key, as `
        // The "[ {" key is matrix {2,1}: no PC key code of its own (AltRight reaches it, but
        // is also joystick fire 2), so these two go only through the character map
        // (cpcTypeKey); BracketLeft is the "@ |" key.
        case '[': code = "IntlBracket"; return true;
        case '{': code = "IntlBracket"; shift = true; return true;
        case ']': code = "BracketRight"; return true;
        case '}': code = "BracketRight"; shift = true; return true;
        case '!': code = "Digit1"; shift = true; return true;
        case '\'': code = "Digit7"; shift = true; return true;
        case ' ': code = "Space"; return true;
        default:
            if (c >= 'A' && c <= 'Z') { code = std::string("Key") + c; shift = true; }   // as typed: "Q" is not "q"
            else if (c >= 'a' && c <= 'z') code = std::string("Key") + (char)(c - 32);
            else if (c >= '0' && c <= '9') code = std::string("Digit") + c;
            return !code.empty();
    }
}

// Presses (down) or lets go of the key that types `c` on this keyboard. A character the
// keyboard's own map knows (keyboard.cpp: the CPC's symbols, letters with their SHIFT, the
// French and Spanish layouts) goes through it -- the key it names, SHIFT only when it says
// so; the rest (Enter, Space, the - = key) is the key cpcKeyForChar names. false: no key on
// this keyboard types it.
inline bool cpcTypeKey(KeyboardMatrix& kb, char c, bool down) {
    std::string code;
    bool shift = false;
    if (!cpcKeyForChar(c, code, shift)) return false;
    const std::string ch(1, c);
    if (c != '\n' && c != ' ') {
        if (cpcCharacterMapping(ch, code, kb.region)) { kb.setKey(code, down, ch); return true; }
        if (normalizeCpcKeyboardRegion(kb.region) != "uk") return false;   // not on this keyboard
    }
    if (down) {
        if (shift) kb.setKey("ShiftLeft", true);
        kb.setKey(code, true);
    } else {
        kb.setKey(code, false);
        if (shift) kb.setKey("ShiftLeft", false);
    }
    return true;
}

} // namespace cpcse
