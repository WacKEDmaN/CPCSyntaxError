// CPCSyntaxError — shared definitions for the emulation core.
//
// -------------------------------------------------------- modelling conventions
//
//  * register values       -> int, masked with & 0xff / & 0xffff where the hardware wraps
//  * byte buffers          -> std::vector<uint8_t> (Bytes) or std::array<uint8_t,N>
//  * absent ROM/cart banks -> an empty Bytes
//  * device callbacks      -> a struct of std::function members
//  * maps / sets           -> std::unordered_map / std::unordered_set
//
// Numbers are kept as plain `int` in expressions and masked exactly where the
// hardware's counters wrap. Register *storage* uses the narrow integer types so
// reads are already in range.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <vector>
#include <array>
#include <deque>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <optional>
#include <algorithm>

namespace cpcse {

// A byte buffer. An empty buffer represents an absent ROM / cartridge bank slot
// (see GXMemory).
using Bytes = std::vector<uint8_t>;

// Sign-extend a byte to [-128,127].
inline int signed8(int value) { return (value & 0x80) ? (value - 0x100) : value; }

// Parity helper: returns 1 for even parity of the low 8 bits.
inline int parity8(int value) {
    value &= 0xff;
    value ^= value >> 4;
    value &= 0x0f;
    return (0x9669 >> value) & 1;
}

} // namespace cpcse
