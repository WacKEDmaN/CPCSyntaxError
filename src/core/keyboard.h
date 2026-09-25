// CPCSyntaxError — CPC keyboard matrix.
// Maps host keys, joystick and lightgun signals to CPC matrix lines.
#pragma once
#include "common.h"

namespace cpcse {

using KeyCell = std::array<int, 2>;   // [row, bit]
using CharCell = std::array<int, 3>;  // [row, bit, shift]

std::string normalizeCpcKeyboardRegion(const std::string& region = "uk");
std::optional<CharCell> cpcCharacterMapping(const std::string& character, const std::string& code, const std::string& region = "uk");

class KeyboardMatrix {
public:
    std::array<uint8_t, 10> rows{};
    bool joystickEnabled = true;
    std::string region = "uk";
    std::unordered_set<std::string> pressedKeys;
    std::unordered_map<std::string, CharCell> pressedSymbols;
    std::unordered_set<int> directBits;
    std::unordered_map<std::string, std::string> codeToEffective;

    int keyboardJoystick = 0xff, gamepadJoystick = 0xff, lightgunJoystick = 0xff;
    // Second joystick (CPC keyboard line 6 / GX4000 dual joystick, player 2).
    int keyboardJoystick1 = 0xff, gamepadJoystick1 = 0xff;
    int selectedRow = 0;
    std::string lightgunMode = "trojan";
    bool lightgunEnabled = false;
    bool lightgunBeamHit = false;
    bool lightgunTrigger = false;
    bool gunstickTriggerPending = false;
    int gunstickReadCounter = 0;

    std::function<void(const std::string& mode)> onLightgunRead;

    KeyboardMatrix();
    void reset();
    std::string setRegion(const std::string& region = "uk");
    void setJoystickEnabled(bool enabled);
    void rebuildKeyboardState();
    void setMatrixDirect(int row, int bit, bool pressed);
    void applyDirectBits();
    bool setKey(const std::string& code, bool pressed, const std::string& character = "", int location = 0);
    void setJoystick(int bit, bool pressed, const std::string& source = "keyboard", int player = 0);
    void setLightgunEnabled(bool enabled);
    void setLightgunType(const std::string& type = "trojan");
    void setLightgunBeamHit(bool hit);
    void setLightgunTrigger(bool pressed);
    void selectRow(int row) { selectedRow = row & 0x0f; }
    int read() { return read(selectedRow); }
    int read(int row);
    // Diagnostic: every matrix row the machine reads, with what it saw. Unset in normal
    // use. The SHAKER runner hangs a key trace off it (SHAKER_KEY_TRACE) to tell a
    // program that raced past a held key from one whose key was never pressed.
    std::function<void(int row, int value)> onRowRead;
};

} // namespace cpcse
