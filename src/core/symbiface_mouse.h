// CPCSyntaxError — CPC proportional mouse.
// SYMBiFACE II / Cyboard PS/2 protocol.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int SYMBIFACE_MOUSE_PORT = 0xfd10;

struct MouseBurst { int dx = 0, dy = 0, dz = 0, buttons = 0; bool buttonChanged = false; };

class SymbifaceMouse {
public:
    bool enabled = false;
    double sensitivity = 1;
    std::string protocol = "sf2";
    int dx = 0, dy = 0, dz = 0;
    std::array<bool, 3> buttons{ false, false, false };
    bool buttonChanged = false;
    int state = 0;
    std::optional<MouseBurst> snapshot;

    SymbifaceMouse();
    void setEnabled(bool enabled = false);
    void setSensitivity(double value = 1);
    void reset();
    bool handlesPort(int port);
    bool handlesWritePort(int port) { (void)port; return false; }
    void move(double dx, double dy);
    void scroll(double delta);
    void button(int index, bool pressed);
    void beginBurst();
    int readPort(int port);
    bool writePort(int port, int value) { (void)port; (void)value; return false; }
};

} // namespace cpcse
