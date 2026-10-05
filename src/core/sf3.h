// CPCSyntaxError — SYMBiFACE III expansion card.
// Combines the SF3 mouse and SF3 RTC on one cartridge.
#pragma once
#include "common.h"
#include "sf3_rtc.h"

namespace cpcse {

inline constexpr int SF3_CMD_PORT = 0xfd41;
inline constexpr int SF3_DATA_PORT = 0xfd42;
inline constexpr int SF3_ECHO_PORT = 0xfd4f;

class Symbiface3 {
public:
    bool enabled = false;
    double sensitivity = 1;
    Symbiface3Rtc rtc;
    int dx = 0, dy = 0, dz = 0;
    std::array<bool, 3> buttons{ false, false, false };
    bool buttonChanged = false;
    std::deque<int> sf3Queue;
    int sf3Echo = 0;
    std::string sf3Mode; // "" == null, "mouse" or "rtc"

    Symbiface3();
    void setEnabled(bool enabled = false);
    void reset();
    bool handlesPort(int port);
    bool handlesWritePort(int port) { return handlesPort(port); }
    void setSensitivity(double value = 1);
    void move(double dx, double dy);
    void scroll(double delta);
    void button(int index, bool pressed);
    int readPort(int port);
    bool writePort(int port, int value);
};

} // namespace cpcse
