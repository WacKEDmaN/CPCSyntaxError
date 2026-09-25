// CPCSyntaxError — Conditional breakpoint expressions.
#pragma once
#include "common.h"

namespace cpcse {

class GX4000;

// The register/memory namespace a compiled condition evaluates against.
struct BreakpointContext {
    std::unordered_map<std::string, long long> values;
    std::function<int(int)> read;
};

// Compiles a condition once; the returned predicate evaluates it without eval.
std::function<bool(const BreakpointContext&)> compileBreakpointCondition(const std::string& source);

// Builds the register/flag namespace used by conditional breakpoints.
BreakpointContext breakpointContext(GX4000* emulator, int hits = 0, const std::unordered_map<std::string, long long>& extra = {});

} // namespace cpcse
