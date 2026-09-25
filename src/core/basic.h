// CPCSyntaxError — Locomotive BASIC decoder.
// Converts tokenized CPC BASIC memory into readable source text.
#pragma once
#include "common.h"

namespace cpcse {

struct BasicLine { int addr; int num; std::string text; };

class BasicDecoder {
public:
    std::vector<std::string> Keywords;            // "" == null
    std::vector<std::string> AdditionalKeywords;  // 256 entries, "" == null

    BasicDecoder();
    // read(address) returns the byte at address (masked by the caller).
    std::string decodeLine(const std::function<int(int)>& read, int startAddr, int length);
    std::vector<BasicLine> listProgram(const std::function<int(int)>& read, int startAddr = 0x0170);
};

} // namespace cpcse
