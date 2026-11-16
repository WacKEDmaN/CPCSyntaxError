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

    // THE TOKENIZER: a line of text ("10 PRINT a") as Locomotive BASIC stores it when the
    // line is typed: the length word, the line number word, the tokens, the 0 end. Checked
    // byte for byte against BASIC itself (cpcse-basic-check). Empty with `error` set when
    // the text is not a numbered line.
    Bytes tokenizeLine(const std::string& text, std::string& error) const;
    // A whole program from its text: the lines sorted by number (a later line with the same
    // number replaces an earlier one, as at the prompt), then the 0,0 end. Blank lines are
    // skipped. `error` names the first line that is not a numbered line.
    Bytes tokenizeProgram(const std::string& text, std::string& error) const;

    // A BASIC file as text, whatever its kind: an AMSDOS header (taken off; its type 0 is a
    // BASIC program, 1 a protected one), then a tokenized program (SAVE"x": the lines' length
    // and number words, each line's 0, the 0,0 end) listed as BASIC's LIST shows it, or ASCII
    // text (SAVE"x",A: CR LF lines, &1A at the end). A protected program (SAVE"x",P) is
    // decrypted first (BASIC's own key, basic.cpp). `what` names the kind found; false (and why)
    // only for a protected file that does not decrypt to a program.
    bool fileToText(const Bytes& file, std::string& text, std::string& what);
};
// True when the bytes are a tokenized program: lines of 5 bytes or more, each ending in 0,
// numbered 1-65535 upwards, then the 0,0 end (or the end of the bytes after a line).
bool isTokenizedBasic(const Bytes& b);

// Where BASIC keeps its program: it starts at &0170 (a 0 byte before it at &016F). Four words
// follow the program's end -- the start of the variables, of the arrays, of free space and
// one more -- all equal to it after a line is typed (measured on the ROMs, cpcse-basic-check):
// at &AE83 in BASIC 1.0 (the 464), at &AE66 in BASIC 1.1 (664, 6128, Plus).
constexpr int BASIC_PROGRAM_START = 0x0170;
inline int basicEndPointers(bool basic10) { return basic10 ? 0xae83 : 0xae66; }

} // namespace cpcse
