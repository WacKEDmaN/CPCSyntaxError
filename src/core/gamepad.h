// CPCSyntaxError — Gamepad input adapter.
// Maps host gamepads onto CPC and console joystick matrix signals.
// Headless: no navigator.getGamepads; update() operates on a supplied list.
#pragma once
#include "common.h"

namespace cpcse {

class KeyboardMatrix;

struct Gamepad {
    std::vector<bool> buttons;
    std::vector<double> axes;
};

class GamepadController {
public:
    KeyboardMatrix* keyboard;
    explicit GamepadController(KeyboardMatrix* keyboard) : keyboard(keyboard) {}
    bool update(const std::vector<Gamepad>& gamepads = {});
};

} // namespace cpcse
