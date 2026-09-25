// CPCSyntaxError — SYMBiFACE III real-time clock.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int SF3_RTC_CMD_PORT = 0xfd41;
inline constexpr int SF3_RTC_DATA_PORT = 0xfd42;
inline constexpr int SF3_RTC_ECHO_PORT = 0xfd4f;

class Symbiface3Rtc {
public:
    bool enabled = false;
    int echo = 0;
    std::deque<int> readQueue;
    std::deque<int> writeBuffer;
    long long guestEpochMs = 0;
    long long hostAnchorMs = 0;

    Symbiface3Rtc();
    void setEnabled(bool enabled = false);
    void reset();
    long long currentGuestMs();
    bool handlesPort(int port);
    bool handlesWritePort(int port) { return handlesPort(port); }
    std::array<int, 3> timeFields();
    std::array<int, 3> dateFields();
    void setTime(const std::deque<int>& payload);
    void setDate(const std::deque<int>& payload);
    void issueCommand(int cmd);
    int readPort(int port);
    bool writePort(int port, int value);
};

} // namespace cpcse
