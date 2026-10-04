// CPCSyntaxError GUI — Z80 / RASM source split into coloured spans for the assembler's
// editor. A highlighter, not a parser: it reads one line at a time (plus whether the line
// starts inside a /* */ comment) and never fails -- what it does not know stays plain text.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cpcse {

enum class AsmToken : uint8_t {
    Text,          // operators, punctuation, anything else
    Mnemonic,      // ld, jp, djnz ...
    Register,      // a, hl, ix, af' ... and the conditions nz, z, nc, c, po, pe, p, m
    Directive,     // org, db, include, macro, if ... (RASM's)
    Number,        // #4000 &4000 $4000 0x4000 4000h %1010 0b1010 12
    String,        // "text" 'c'
    Comment,       // ; ...   // ...   /* ... */
    Label,         // a label being defined: start, .local, @macro:
    Symbol,        // a name used: call print_char
    Count
};

struct AsmSpan { int start = 0, length = 0; AsmToken kind = AsmToken::Text; };

// The spans of one line, in order, covering it without gaps. `inBlockComment` says whether
// the line starts inside /* */, and is left saying whether the next one does.
std::vector<AsmSpan> highlightAsmLine(const std::string& line, bool& inBlockComment);

// The token classes' names, for the colour settings and the ini ("mnemonic", "number"...).
const char* asmTokenKey(AsmToken t);
const char* asmTokenLabel(AsmToken t);

// The colours (0xRRGGBB), and the defaults: a dark-theme set in the spirit of the editor's.
using AsmColours = std::array<uint32_t, (size_t)AsmToken::Count>;
AsmColours defaultAsmColours();

} // namespace cpcse
