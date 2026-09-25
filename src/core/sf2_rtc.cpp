// CPCSyntaxError — SYMBiFACE II real-time clock.
#include "sf2_rtc.h"
#include "rtc_time.h"

namespace cpcse {

static int toBcd(int value) {
    value = std::max(0, value);
    return (((value / 10) % 10) << 4) | (value % 10);
}
static int fromBcd(int value) { return (((unsigned)value >> 4) & 15) * 10 + (value & 15); }
static int clampd(int value, int mn, int mx) { return std::max(mn, std::min(mx, value)); }

Symbiface2Rtc::Symbiface2Rtc() {
    enabled = false;
    selectedRegister = 0;
    guestEpochMs = nowMs();
    hostAnchorMs = nowMs();
    reset();
}
void Symbiface2Rtc::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
    reset();
}
void Symbiface2Rtc::reset() {
    registers.fill(0xff);
    registers[10] = 0x20;
    registers[11] = 0x07;
    registers[12] = 0x00;
    registers[13] = 0x80;
    selectedRegister = 0;
    long long now = nowMs();
    guestEpochMs = now;
    hostAnchorMs = now;
    if (enabled) syncClockRegisters();
}
bool Symbiface2Rtc::handlesPort(int port) {
    if (!enabled) return false;
    port &= 0xffff;
    return port == SF2_RTC_DETECT_PORT || port == SF2_RTC_DATA_PORT || port == SF2_RTC_SELECT_PORT;
}
bool Symbiface2Rtc::handlesWritePort(int port) {
    if (!enabled) return false;
    port &= 0xffff;
    return port == SF2_RTC_DATA_PORT || port == SF2_RTC_SELECT_PORT;
}
long long Symbiface2Rtc::currentGuestMs() { return guestEpochMs + (nowMs() - hostAnchorMs); }
bool Symbiface2Rtc::binaryMode() { return (registers[11] & 0x04) != 0; }
int Symbiface2Rtc::encode(int value) { return binaryMode() ? value & 0xff : toBcd(value); }
int Symbiface2Rtc::decode(int value, bool binaryModeV) { return binaryModeV ? value & 0xff : fromBcd(value & 0xff); }
int Symbiface2Rtc::decode(int value) { return decode(value, binaryMode()); }
void Symbiface2Rtc::syncClockRegisters() {
    DateParts date = breakLocalDate(currentGuestMs());
    registers[0] = (uint8_t)encode(date.seconds);
    registers[2] = (uint8_t)encode(date.minutes);
    registers[4] = (uint8_t)encode(date.hours);
    registers[6] = (uint8_t)encode(date.day + 1);
    registers[7] = (uint8_t)encode(date.date);
    registers[8] = (uint8_t)encode(date.month + 1);
    registers[9] = (uint8_t)encode(date.fullYear % 100);
    registers[50] = (uint8_t)encode(date.fullYear / 100);
}
void Symbiface2Rtc::setClockField(int index, int rawValue, bool binaryModeV) {
    DateParts date = breakLocalDate(currentGuestMs());
    int value = decode(rawValue, binaryModeV);
    switch (index) {
        case 0: date.seconds = clampd(value, 0, 59); break;
        case 2: date.minutes = clampd(value, 0, 59); break;
        case 4: date.hours = clampd(value, 0, 23); break;
        case 6: break;
        case 7: date.date = clampd(value, 1, 31); break;
        case 8: date.month = clampd(value, 1, 12) - 1; break;
        case 9: {
            int century = date.fullYear / 100;
            date.fullYear = century * 100 + clampd(value, 0, 99);
            break;
        }
        case 50: date.fullYear = clampd(value, 0, 99) * 100 + (date.fullYear % 100); break;
        default: return;
    }
    guestEpochMs = makeLocalMs(date);
    hostAnchorMs = nowMs();
}
int Symbiface2Rtc::readPort(int port) {
    if (!handlesPort(port)) return 0xff;
    port &= 0xffff;
    if (port == SF2_RTC_DETECT_PORT) return 0;
    if (port != SF2_RTC_DATA_PORT) return 0xff;
    syncClockRegisters();
    return registers[selectedRegister & 0x3f];
}
bool Symbiface2Rtc::writePort(int port, int value) {
    if (!handlesWritePort(port)) return false;
    port &= 0xffff; value &= 0xff;
    if (port == SF2_RTC_SELECT_PORT) {
        selectedRegister = value & 0x3f;
        return true;
    }
    int index = selectedRegister & 0x3f;
    bool oldBinaryMode = binaryMode();
    registers[index] = (uint8_t)value;
    if (index == 11) {
        syncClockRegisters();
    } else if (index < 10 || index == 50) {
        setClockField(index, value, oldBinaryMode);
        syncClockRegisters();
    }
    selectedRegister = 0;
    return true;
}

} // namespace cpcse
