// CPCSyntaxError — SYMBiFACE III expansion card.
#include "sf3.h"

namespace cpcse {

static int clampi(double value, int mn, int mx) { return std::max(mn, std::min(mx, (int)value)); }

Symbiface3::Symbiface3() { enabled = false; sensitivity = 1; reset(); }
void Symbiface3::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
    reset();
    rtc.setEnabled(next);
}
void Symbiface3::reset() {
    dx = dy = dz = 0;
    buttons = { false, false, false };
    buttonChanged = false;
    sf3Queue.clear();
    sf3Echo = 0;
    sf3Mode = "";
    rtc.reset();
}
bool Symbiface3::handlesPort(int port) {
    if (!enabled) return false;
    port &= 0xffff;
    return port == SF3_CMD_PORT || port == SF3_DATA_PORT || port == SF3_ECHO_PORT;
}
void Symbiface3::setSensitivity(double value) {
    sensitivity = std::isfinite(value) ? std::max(0.25, std::min(4.0, value)) : 1;
}
void Symbiface3::move(double dxIn, double dyIn) {
    if (!enabled) return;
    dx += (int)std::lround(dxIn * sensitivity);
    dy += (int)std::lround(dyIn * sensitivity);
}
void Symbiface3::scroll(double delta) {
    if (!enabled) return;
    dz += (int)delta;
}
void Symbiface3::button(int index, bool pressed) {
    if (!enabled || index < 0 || index > 2) return;
    bool next = pressed;
    if (buttons[index] == next) return;
    buttons[index] = next;
    buttonChanged = true;
}
int Symbiface3::readPort(int port) {
    if (!handlesPort(port)) return 0xff;
    port &= 0xffff;
    if (port == SF3_ECHO_PORT) return sf3Echo & 0xff;
    if (port == SF3_CMD_PORT) return 0;
    if (sf3Mode == "mouse") { if (!sf3Queue.empty()) { int v = sf3Queue.front(); sf3Queue.pop_front(); return v; } return 0; }
    return rtc.readPort(port);
}
bool Symbiface3::writePort(int port, int value) {
    if (!handlesWritePort(port)) return false;
    port &= 0xffff; value &= 0xff;
    if (port == SF3_ECHO_PORT) { sf3Echo = value; rtc.writePort(port, value); return true; }
    if (port == SF3_CMD_PORT) {
        if (value == 20) {
            sf3Mode = "mouse";
            sf3Queue.clear();
            sf3Queue.push_back(clampi(dx, -128, 127) & 0xff);
            sf3Queue.push_back(clampi(dy, -128, 127) & 0xff);
            sf3Queue.push_back((buttons[0] ? 1 : 0) | (buttons[1] ? 2 : 0) | (buttons[2] ? 4 : 0));
            sf3Queue.push_back(clampi(dz, -128, 127) & 0xff);
            dx = dy = dz = 0;
            buttonChanged = false;
        } else {
            sf3Mode = value == 0 ? "" : "rtc";
            rtc.writePort(port, value);
        }
        return true;
    }
    rtc.writePort(port, value);
    return true;
}

} // namespace cpcse
