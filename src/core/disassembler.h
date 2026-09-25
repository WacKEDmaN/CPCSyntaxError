// CPCSyntaxError — Z80 disassembler.
#pragma once
#include "common.h"

namespace cpcse {

struct DisasmResult { int addr; int next; std::vector<int> bytes; std::string mnem; };

class Disassembler {
public:
    std::vector<std::string> r_;
    std::vector<std::string> dd_;
    std::vector<std::string> cc_;
    std::vector<std::string> CB_CODES;
    Disassembler();
    std::string hex8(int val);
    std::string hex16(int val);
    // read(addr) returns the byte at addr.
    DisasmResult disassemble(const std::function<int(int)>& read, int pc);
};

} // namespace cpcse
