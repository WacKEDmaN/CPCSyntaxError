// CPCSyntaxError — Z80 syntax tools.
// Tokenizes and highlights assembler source, directives, labels and errors.
#pragma once
#include "common.h"

namespace cpcse {

std::string highlightZ80Assembly(const std::string& source);
extern const std::string z80SyntaxCss;

} // namespace cpcse
