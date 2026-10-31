// CPCSyntaxError — writing a file whole or not at all.
#pragma once
#include <cstddef>
#include <string>

namespace cpcse {

// Writes a file whole or not at all: to a temporary file beside it, then renamed over it,
// so a failed write (a full disc) leaves the old file as it was instead of cut short.
// false (and `why`) when it could not.
bool writeFileSafely(const std::string& path, const void* data, size_t size, std::string* why = nullptr);

} // namespace cpcse
