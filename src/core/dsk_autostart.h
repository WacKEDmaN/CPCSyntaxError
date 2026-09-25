// CPCSyntaxError — Per-disk autostart registry.
// A key/value store may be supplied; otherwise nothing persists.
#pragma once
#include "common.h"

namespace cpcse {

extern const std::string DSK_AUTOSTART_STORAGE_KEY;

// Optional key/value store (JSON strings). nullptr == no persistence.
struct AutostartStorage {
    virtual ~AutostartStorage() = default;
    virtual std::string getItem(const std::string& key) = 0;
    virtual void setItem(const std::string& key, const std::string& value) = 0;
};

struct AutostartRecord { std::string fingerprint; std::string command; std::string name; int drive; long long savedAt; bool valid = false; };

std::string diskFingerprint(const Bytes& input);
AutostartRecord rememberDiskAutostart(const Bytes& input, const std::string& command, const std::string& name = "", int drive = 0, AutostartStorage* storage = nullptr);
AutostartRecord diskAutostartCommand(const Bytes& input, AutostartStorage* storage = nullptr);
bool forgetDiskAutostart(const Bytes& input, AutostartStorage* storage = nullptr);

} // namespace cpcse
