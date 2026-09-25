// CPCSyntaxError — SYMBiFACE II real-time clock.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int SF2_RTC_DETECT_PORT = 0xfd10;
inline constexpr int SF2_RTC_DATA_PORT = 0xfd14;
inline constexpr int SF2_RTC_SELECT_PORT = 0xfd15;

class Symbiface2Rtc {
public:
    bool enabled = false;
    std::array<uint8_t, 64> registers{};
    int selectedRegister = 0;
    long long guestEpochMs = 0;
    long long hostAnchorMs = 0;

    Symbiface2Rtc();
    void setEnabled(bool enabled = false);
    void reset();
    bool handlesPort(int port);
    bool handlesWritePort(int port);
    long long currentGuestMs();
    bool binaryMode();
    int encode(int value);
    int decode(int value);
    int decode(int value, bool binaryMode);
    void syncClockRegisters();
    void setClockField(int index, int rawValue, bool binaryMode);
    int readPort(int port);
    bool writePort(int port, int value);
};

} // namespace cpcse
