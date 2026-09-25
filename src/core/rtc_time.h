// CPCSyntaxError — local-time helpers for the SYMBiFACE RTC ports.
// Local civil time helpers used by the RTC modules.
#pragma once
#include "common.h"
#include <chrono>
#include <ctime>

namespace cpcse {

inline long long nowMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Broken-down local time.
struct DateParts {
    int seconds, minutes, hours, day /*0=Sunday*/, date /*1..31*/, month /*0..11*/, fullYear;
};

inline DateParts breakLocalDate(long long ms) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    std::tm* tmp = std::localtime(&t); if (tmp) tmv = *tmp;
#endif
    DateParts d;
    d.seconds = tmv.tm_sec; d.minutes = tmv.tm_min; d.hours = tmv.tm_hour;
    d.day = tmv.tm_wday; d.date = tmv.tm_mday; d.month = tmv.tm_mon; d.fullYear = tmv.tm_year + 1900;
    return d;
}

// Rebuild an epoch-ms timestamp from local broken-down fields.
inline long long makeLocalMs(const DateParts& d) {
    std::tm tmv{};
    tmv.tm_sec = d.seconds; tmv.tm_min = d.minutes; tmv.tm_hour = d.hours;
    tmv.tm_mday = d.date; tmv.tm_mon = d.month; tmv.tm_year = d.fullYear - 1900;
    tmv.tm_isdst = -1;
    std::time_t t = std::mktime(&tmv);
    return (long long)t * 1000;
}

} // namespace cpcse
