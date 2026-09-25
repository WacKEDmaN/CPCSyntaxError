// CPCSyntaxError — SYMBiFACE III real-time clock.
#include "sf3_rtc.h"
#include "rtc_time.h"

namespace cpcse {

static const int CMD_TIMSET = 100, CMD_TIMGET = 101, CMD_DATSET = 110, CMD_DATGET = 111;
static int clampd(int value, int mn, int mx) { return std::max(mn, std::min(mx, value)); }
static int at(const std::deque<int>& v, int i) { return i >= 0 && i < (int)v.size() ? v[i] : 0; }

Symbiface3Rtc::Symbiface3Rtc() { enabled = false; reset(); }
void Symbiface3Rtc::setEnabled(bool enabled_) {
    bool next = enabled_;
    if (enabled == next) return;
    enabled = next;
    reset();
}
void Symbiface3Rtc::reset() {
    echo = 0;
    readQueue.clear();
    writeBuffer.clear();
    long long now = nowMs();
    guestEpochMs = now;
    hostAnchorMs = now;
}
long long Symbiface3Rtc::currentGuestMs() { return guestEpochMs + (nowMs() - hostAnchorMs); }
bool Symbiface3Rtc::handlesPort(int port) {
    if (!enabled) return false;
    port &= 0xffff;
    return port == SF3_RTC_CMD_PORT || port == SF3_RTC_DATA_PORT || port == SF3_RTC_ECHO_PORT;
}
std::array<int, 3> Symbiface3Rtc::timeFields() {
    DateParts d = breakLocalDate(currentGuestMs());
    return { d.hours & 0xff, d.minutes & 0xff, d.seconds & 0xff };
}
std::array<int, 3> Symbiface3Rtc::dateFields() {
    DateParts d = breakLocalDate(currentGuestMs());
    return { d.date & 0xff, (d.month + 1) & 0xff, std::max(0, d.fullYear - 2000) & 0xff };
}
void Symbiface3Rtc::setTime(const std::deque<int>& payload) {
    DateParts d = breakLocalDate(currentGuestMs());
    d.hours = clampd(at(payload, 0), 0, 23);
    d.minutes = clampd(at(payload, 1), 0, 59);
    d.seconds = clampd(at(payload, 2), 0, 59);
    guestEpochMs = makeLocalMs(d);
    hostAnchorMs = nowMs();
}
void Symbiface3Rtc::setDate(const std::deque<int>& payload) {
    DateParts d = breakLocalDate(currentGuestMs());
    d.date = clampd(at(payload, 0), 1, 31);
    d.month = clampd(at(payload, 1), 1, 12) - 1;
    d.fullYear = 2000 + clampd(at(payload, 2), 0, 99);
    guestEpochMs = makeLocalMs(d);
    hostAnchorMs = nowMs();
}
void Symbiface3Rtc::issueCommand(int cmd) {
    switch (cmd) {
        case 0: readQueue.clear(); writeBuffer.clear(); break;
        case CMD_TIMSET: setTime(writeBuffer); writeBuffer.clear(); break;
        case CMD_DATSET: setDate(writeBuffer); writeBuffer.clear(); break;
        case CMD_TIMGET: { auto f = timeFields(); readQueue.assign(f.begin(), f.end()); break; }
        case CMD_DATGET: { auto f = dateFields(); readQueue.assign(f.begin(), f.end()); break; }
        default: readQueue.clear(); writeBuffer.clear();
    }
}
int Symbiface3Rtc::readPort(int port) {
    if (!handlesPort(port)) return 0xff;
    port &= 0xffff;
    if (port == SF3_RTC_ECHO_PORT) return echo & 0xff;
    if (port == SF3_RTC_CMD_PORT) return 0;
    if (!readQueue.empty()) { int v = readQueue.front(); readQueue.pop_front(); return v; }
    return 0;
}
bool Symbiface3Rtc::writePort(int port, int value) {
    if (!handlesWritePort(port)) return false;
    port &= 0xffff; value &= 0xff;
    if (port == SF3_RTC_ECHO_PORT) { echo = value; return true; }
    if (port == SF3_RTC_CMD_PORT) { issueCommand(value); return true; }
    writeBuffer.push_back(value);
    if ((int)writeBuffer.size() > 4) writeBuffer.pop_front();
    return true;
}

} // namespace cpcse
