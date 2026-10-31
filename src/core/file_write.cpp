// CPCSyntaxError — writing a file whole or not at all (see file_write.h).
#include "file_write.h"
#include <cstdio>
#include <filesystem>

namespace cpcse {

bool writeFileSafely(const std::string& path, const void* data, size_t size, std::string* why) {
    namespace fs = std::filesystem;
    const fs::path target(path), temp(path + ".cpcse-new");   // narrow names, as every other file here
    {
        std::FILE* f = std::fopen(temp.string().c_str(), "wb");
        if (!f) { if (why) *why = "cannot write " + path; return false; }
        const bool wrote = (size == 0 || std::fwrite(data, 1, size, f) == size);
        const bool closed = std::fclose(f) == 0;
        if (!wrote || !closed) {
            std::error_code ec;
            fs::remove(temp, ec);
            if (why) *why = "could not write all of " + path + " (disc full?); the old file is as it was";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);   // replaces the old file in one step
    if (ec) {
        fs::remove(temp, ec);
        if (why) *why = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace cpcse
