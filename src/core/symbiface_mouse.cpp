// CPCSyntaxError — CPC proportional mouse.
#include "symbiface_mouse.h"

namespace cpcse {

static int clampi(double value, int mn, int mx) { return std::max(mn, std::min(mx, (int)value)); }

SymbifaceMouse::SymbifaceMouse() { enabled = false; sensitivity = 1; protocol = "sf2"; reset(); }

void SymbifaceMouse::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
    reset();
}
void SymbifaceMouse::setSensitivity(double value) {
    sensitivity = std::isfinite(value) ? std::max(0.25, std::min(4.0, value)) : 1;
}
void SymbifaceMouse::reset() {
    dx = dy = dz = 0;
    buttons = { false, false, false };
    buttonChanged = false;
    state = 0;
    snapshot.reset();
}
bool SymbifaceMouse::handlesPort(int port) {
    if (!enabled) return false;
    port &= 0xffff;
    return port == SYMBIFACE_MOUSE_PORT;
}
void SymbifaceMouse::move(double dxIn, double dyIn) {
    if (!enabled) return;
    dx += (int)std::lround(dxIn * sensitivity);
    dy += (int)std::lround(dyIn * sensitivity);
}
void SymbifaceMouse::scroll(double delta) {
    if (!enabled) return;
    dz += (int)delta;
}
void SymbifaceMouse::button(int index, bool pressed) {
    if (!enabled || index < 0 || index > 2) return;
    bool next = pressed;
    if (buttons[index] == next) return;
    buttons[index] = next;
    buttonChanged = true;
}
void SymbifaceMouse::beginBurst() {
    MouseBurst b;
    b.dx = dx; b.dy = dy; b.dz = dz;
    b.buttons = (buttons[0] ? 1 : 0) | (buttons[1] ? 2 : 0) | (buttons[2] ? 4 : 0);
    b.buttonChanged = buttonChanged;
    snapshot = b;
    dx = dy = dz = 0;
    buttonChanged = false;
    state = 1;
}
int SymbifaceMouse::readPort(int port) {
    if (!handlesPort(port)) return 0xff;
    port &= 0xffff;
    if (state == 0) beginBurst();
    for (;;) {
        switch (state) {
            case 1:
                state = 2;
                if (snapshot->dx) return 0x40 | (clampi(snapshot->dx, -32, 31) & 0x3f);
                break;
            case 2:
                state = 3;
                if (snapshot->dy) return 0x80 | (clampi(-snapshot->dy, -32, 31) & 0x3f);
                break;
            case 3:
                state = 4;
                if (snapshot->buttonChanged) return 0xc0 | snapshot->buttons;
                break;
            case 4:
                state = 0;
                if (snapshot->dz) return 0xe0 | (clampi(snapshot->dz, -16, 15) & 0x1f);
                break;
            default:
                state = 0;
                return 0;
        }
        if (state == 0) return 0;
    }
}

} // namespace cpcse
