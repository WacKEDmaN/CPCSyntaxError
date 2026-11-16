// CPCSyntaxError: memory as RASM source (Debugger > Export .asm). The code is disassembled from
// `from` to `to` (not included), each jump, call and DJNZ target in the range gets a label (the
// debugger's own name when it has one, else L<address>), a known name outside the range an EQU,
// and the text is assembled again with the built-in RASM: an instruction whose bytes do not
// come back the same (an undocumented duplicate, a useless prefix, a byte the disassembler
// cannot name) is written as DB instead, until the source gives back every byte.
#pragma once
#include <functional>
#include <string>
#include <unordered_map>

namespace cpcse {

struct DisasmExport {
    std::string text;
    int instructions = 0, dataBytes = 0, labels = 0;
    bool verified = false;          // RASM gave back every byte
    std::string why;                // when not
};

DisasmExport exportDisassembly(const std::function<int(int)>& read, int from, int to,
                               const std::unordered_map<int, std::string>& known);

} // namespace cpcse
