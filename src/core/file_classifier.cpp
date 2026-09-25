// CPCSyntaxError — Assembler file-content classifier.
#include "file_classifier.h"
#include <regex>

namespace cpcse {

static std::string trimStr(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }

std::string suggestedFileAction(const std::string& fileName) {
    static const std::regex TEXT_EXTENSIONS("\\.(?:asm|txt|z80|s|inc|src|mac)$", std::regex::icase);
    return std::regex_search(trimStr(fileName), TEXT_EXTENSIONS) ? "load" : "disassemble";
}

static void payloadFromFile(const Bytes& input, Bytes& payload, std::optional<AmsdosHeader>& header) {
    Bytes source = input;
    header = hasAmsdosHeader(source) ? parseAmsdosHeader(source) : std::nullopt;
    payload = header ? Bytes(source.begin() + 128, source.end()) : source;
    if (header) {
        int declared = header->fullLength ? header->fullLength : header->logicalLength ? header->logicalLength : header->length;
        if (declared > 0 && declared < (int)payload.size()) payload = Bytes(payload.begin(), payload.begin() + declared);
    }
    while (!payload.empty() && payload.back() == 0x1a) payload.pop_back();
}

static bool isValidUtf8(const Bytes& b) {
    size_t i = 0;
    while (i < b.size()) {
        uint8_t c = b[i];
        int need;
        if (c < 0x80) need = 0;
        else if ((c & 0xe0) == 0xc0) need = 1;
        else if ((c & 0xf0) == 0xe0) need = 2;
        else if ((c & 0xf8) == 0xf0) need = 3;
        else return false;
        if (i + need >= b.size() && need > 0) return false;
        for (int k = 1; k <= need; k++) if ((b[i + k] & 0xc0) != 0x80) return false;
        i += need + 1;
    }
    return true;
}

FileClassification decodeLikelyTextFile(const Bytes& input, const std::string& fileName) {
    FileClassification out;
    payloadFromFile(input, out.payload, out.header);
    const Bytes& payload = out.payload;
    bool extensionHint = suggestedFileAction(fileName) == "load";
    if (payload.empty()) { out.isText = extensionHint; out.hasText = true; out.text = ""; out.reason = extensionHint ? "empty text file" : "empty binary file"; return out; }

    int controls = 0, highBytes = 0;
    for (uint8_t byte : payload) {
        if (byte == 0) { out.isText = false; out.hasText = false; out.reason = "contains NUL bytes"; return out; }
        if (byte >= 0x80) highBytes += 1;
        else if (byte < 0x20 && byte != 0x09 && byte != 0x0a && byte != 0x0c && byte != 0x0d) controls += 1;
    }
    if (controls > 0) { out.isText = false; out.hasText = false; out.reason = "contains binary control bytes"; return out; }

    std::string encoding = "UTF-8";
    bool decoded = false;
    if (isValidUtf8(payload)) { out.text = std::string(payload.begin(), payload.end()); decoded = true; }
    else {
        if ((double)highBytes / payload.size() > 0.15) { out.isText = false; out.hasText = false; out.reason = "invalid UTF-8 / too many high-bit bytes"; return out; }
        // Windows-1252 always decodes; represent as the original bytes.
        out.text = std::string(payload.begin(), payload.end()); encoding = "Windows-1252"; decoded = true;
    }
    (void)decoded;
    // decoded-text control-character check (bytes are already free of control codes above)
    out.isText = true; out.hasText = true;
    out.reason = encoding + " text" + (extensionHint ? " with source extension" : " detected by content");
    return out;
}

} // namespace cpcse
