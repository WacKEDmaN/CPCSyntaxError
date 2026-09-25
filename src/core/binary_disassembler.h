// CPCSyntaxError — Binary-to-assembly converter.
// Produces editable Z80 source with control-flow labels and separate data blocks.
#pragma once
#include "common.h"
#include "disassembler.h"
#include "dskfs.h"

namespace cpcse {

struct PreparedBinary { Bytes bytes; int origin; std::optional<AmsdosHeader> header; };
PreparedBinary prepareBinaryForDisassembly(const Bytes& input, int fallbackOrigin = 0x4000);

struct BinaryDisasmResult {
    std::string source;
    int origin = 0;
    int execAddress = 0;
    std::optional<AmsdosHeader> header;
    int byteLength = 0;
    Bytes binary;
    std::unordered_map<int, std::string> labels;
    int codeBytes = 0;
    int dataBytes = 0;
    int truncated = 0;
};

struct DisassembleOptions { std::optional<int> origin; std::string fileName; Disassembler* disassembler = nullptr; };
BinaryDisasmResult disassembleBinaryToSource(const Bytes& input, const DisassembleOptions& options = {});

} // namespace cpcse
