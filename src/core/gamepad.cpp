// CPCSyntaxError — Gamepad input adapter.
#include "gamepad.h"
#include "keyboard.h"

namespace cpcse {

bool GamepadController::update(const std::vector<Gamepad>& gamepads) {
    // Up to two pads: index 0 -> CPC joystick 0, index 1 -> joystick 1 (GX4000
    // dual). Button/axis indices follow the standard gamepad layout:
    // 0/1 = fire buttons, 12-15 = d-pad up/down/left/right, axes 0/1 = left stick.
    bool any = false;
    for (int player = 0; player < 2; player += 1) {
        const Gamepad* pad = player < (int)gamepads.size() ? &gamepads[player] : nullptr;
        if (!pad) { for (int bit = 0; bit < 6; bit += 1) keyboard->setJoystick(bit, false, "gamepad", player); continue; }
        any = true;
        auto pressed = [&](int i) { return i >= 0 && i < (int)pad->buttons.size() && pad->buttons[i]; };
        auto axis = [&](int i) { return i >= 0 && i < (int)pad->axes.size() ? pad->axes[i] : 0.0; };
        keyboard->setJoystick(0, pressed(12) || axis(1) < -0.5, "gamepad", player); // up
        keyboard->setJoystick(1, pressed(13) || axis(1) > 0.5, "gamepad", player);  // down
        keyboard->setJoystick(2, pressed(14) || axis(0) < -0.5, "gamepad", player); // left
        keyboard->setJoystick(3, pressed(15) || axis(0) > 0.5, "gamepad", player);  // right
        keyboard->setJoystick(4, pressed(0), "gamepad", player);                    // fire 1
        keyboard->setJoystick(5, pressed(1), "gamepad", player);                    // fire 2
    }
    return any;
}

} // namespace cpcse
