// CPCSyntaxError — Locomotive BASIC tokenizer.
// Converts ASCII CPC BASIC source into the tokenized format used in RAM/DSK.
#pragma once
#include "common.h"

namespace cpcse {

// Locomotive BASIC keyword table (token &80+ offsets). "" == null slot.
extern const std::vector<std::string> BASIC_KEYWORDS;

struct BasicTokenInfo { int lineNum; std::string original; std::vector<int> tokenized; int length; };
struct BasicTokenizeResult { Bytes binary; std::vector<BasicTokenInfo> tokenInfo; };

BasicTokenizeResult tokenizeBasic(const std::string& text);
Bytes makeBasicAmsdosHeader(const std::string& filename, int dataLen);

} // namespace cpcse
