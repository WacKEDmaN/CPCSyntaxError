// CPCSyntaxError GUI — host key → CPC key translation.
//
// The core's KeyboardMatrix keys on DOM-style KeyboardEvent.code strings
// ("KeyA", "Digit1", "ArrowUp", "ShiftLeft", …) and the CPC firmware applies
// SHIFT/CONTROL itself from the matrix, so we only need to report the physical
// key that was pressed. This maps an SDL physical scancode to the equivalent
// code string; the character argument to setKey() is left empty on purpose so
// the matrix cell (not a produced glyph) drives the emulation.
#pragma once
#include <SDL.h>
#include <string>

namespace cpcse {

inline std::string sdlScancodeToBrowserCode(SDL_Scancode sc) {
    switch (sc) {
        // letters
        case SDL_SCANCODE_A: return "KeyA"; case SDL_SCANCODE_B: return "KeyB";
        case SDL_SCANCODE_C: return "KeyC"; case SDL_SCANCODE_D: return "KeyD";
        case SDL_SCANCODE_E: return "KeyE"; case SDL_SCANCODE_F: return "KeyF";
        case SDL_SCANCODE_G: return "KeyG"; case SDL_SCANCODE_H: return "KeyH";
        case SDL_SCANCODE_I: return "KeyI"; case SDL_SCANCODE_J: return "KeyJ";
        case SDL_SCANCODE_K: return "KeyK"; case SDL_SCANCODE_L: return "KeyL";
        case SDL_SCANCODE_M: return "KeyM"; case SDL_SCANCODE_N: return "KeyN";
        case SDL_SCANCODE_O: return "KeyO"; case SDL_SCANCODE_P: return "KeyP";
        case SDL_SCANCODE_Q: return "KeyQ"; case SDL_SCANCODE_R: return "KeyR";
        case SDL_SCANCODE_S: return "KeyS"; case SDL_SCANCODE_T: return "KeyT";
        case SDL_SCANCODE_U: return "KeyU"; case SDL_SCANCODE_V: return "KeyV";
        case SDL_SCANCODE_W: return "KeyW"; case SDL_SCANCODE_X: return "KeyX";
        case SDL_SCANCODE_Y: return "KeyY"; case SDL_SCANCODE_Z: return "KeyZ";
        // number row
        case SDL_SCANCODE_1: return "Digit1"; case SDL_SCANCODE_2: return "Digit2";
        case SDL_SCANCODE_3: return "Digit3"; case SDL_SCANCODE_4: return "Digit4";
        case SDL_SCANCODE_5: return "Digit5"; case SDL_SCANCODE_6: return "Digit6";
        case SDL_SCANCODE_7: return "Digit7"; case SDL_SCANCODE_8: return "Digit8";
        case SDL_SCANCODE_9: return "Digit9"; case SDL_SCANCODE_0: return "Digit0";
        // punctuation / symbols
        case SDL_SCANCODE_MINUS: return "Minus";
        case SDL_SCANCODE_EQUALS: return "Equal";
        case SDL_SCANCODE_LEFTBRACKET: return "BracketLeft";
        case SDL_SCANCODE_RIGHTBRACKET: return "BracketRight";
        case SDL_SCANCODE_BACKSLASH: return "Backslash";
        case SDL_SCANCODE_SEMICOLON: return "Semicolon";
        case SDL_SCANCODE_APOSTROPHE: return "Quote";
        case SDL_SCANCODE_GRAVE: return "Backquote";
        case SDL_SCANCODE_COMMA: return "Comma";
        case SDL_SCANCODE_PERIOD: return "Period";
        case SDL_SCANCODE_SLASH: return "Slash";
        // control / editing
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_SPACE: return "Space";
        case SDL_SCANCODE_BACKSPACE: return "Backspace";
        case SDL_SCANCODE_TAB: return "Tab";
        case SDL_SCANCODE_ESCAPE: return "Escape";
        case SDL_SCANCODE_DELETE: return "Delete";
        case SDL_SCANCODE_INSERT: return "Insert";
        case SDL_SCANCODE_HOME: return "Home";
        case SDL_SCANCODE_END: return "End";
        case SDL_SCANCODE_CAPSLOCK: return "CapsLock";
        case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
        case SDL_SCANCODE_RSHIFT: return "ShiftRight";
        case SDL_SCANCODE_LCTRL: return "ControlLeft";
        case SDL_SCANCODE_RCTRL: return "ControlRight";
        case SDL_SCANCODE_LALT: return "AltLeft";
        case SDL_SCANCODE_RALT: return "AltRight";
        // arrows
        case SDL_SCANCODE_UP: return "ArrowUp";
        case SDL_SCANCODE_DOWN: return "ArrowDown";
        case SDL_SCANCODE_LEFT: return "ArrowLeft";
        case SDL_SCANCODE_RIGHT: return "ArrowRight";
        // numeric keypad (CPC f0–f9 / f. / ENTER)
        case SDL_SCANCODE_KP_0: return "Numpad0"; case SDL_SCANCODE_KP_1: return "Numpad1";
        case SDL_SCANCODE_KP_2: return "Numpad2"; case SDL_SCANCODE_KP_3: return "Numpad3";
        case SDL_SCANCODE_KP_4: return "Numpad4"; case SDL_SCANCODE_KP_5: return "Numpad5";
        case SDL_SCANCODE_KP_6: return "Numpad6"; case SDL_SCANCODE_KP_7: return "Numpad7";
        case SDL_SCANCODE_KP_8: return "Numpad8"; case SDL_SCANCODE_KP_9: return "Numpad9";
        case SDL_SCANCODE_KP_PERIOD: return "NumpadDecimal";
        case SDL_SCANCODE_KP_ENTER: return "NumpadEnter";
        default: return "";
    }
}

// SDL keypad codes report location 3 so the core can distinguish NumpadEnter.
inline int sdlKeyLocation(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_KP_0: case SDL_SCANCODE_KP_1: case SDL_SCANCODE_KP_2:
        case SDL_SCANCODE_KP_3: case SDL_SCANCODE_KP_4: case SDL_SCANCODE_KP_5:
        case SDL_SCANCODE_KP_6: case SDL_SCANCODE_KP_7: case SDL_SCANCODE_KP_8:
        case SDL_SCANCODE_KP_9: case SDL_SCANCODE_KP_PERIOD: case SDL_SCANCODE_KP_ENTER:
            return 3;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_LALT: return 1;
        case SDL_SCANCODE_RSHIFT: case SDL_SCANCODE_RCTRL: case SDL_SCANCODE_RALT: return 2;
        default: return 0;
    }
}

} // namespace cpcse
