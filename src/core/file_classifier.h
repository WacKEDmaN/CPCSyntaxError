// CPCSyntaxError — Assembler file-content classifier.
#pragma once
#include "common.h"
#include "dskfs.h"

namespace cpcse {

struct FileClassification {
    bool isText = false;
    std::string text;
    bool hasText = false;
    Bytes payload;
    std::optional<AmsdosHeader> header;
    std::string reason;
};

std::string suggestedFileAction(const std::string& fileName);
FileClassification decodeLikelyTextFile(const Bytes& input, const std::string& fileName = "");

} // namespace cpcse
