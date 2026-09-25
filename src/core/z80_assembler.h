// CPCSyntaxError — Z80 assembler.
// A two-pass Maxam/Maxam-1.5 compatible assembler with AMSDOS binary output.
#pragma once
#include "common.h"
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace cpcse {

// ---- result / diagnostic records --------------------------------------
struct AsmError { int lineNumber; std::string message; std::string source; };

struct AsmListingItem {
    int lineNumber;
    int address;
    std::vector<int> bytes;
    std::string source;
    std::string file;   // "" == null (no source file)
};

struct AsmRegion { int bank; int start; Bytes bytes; };

// Virtual-file-system hooks used by READ / INCBIN / WRITE. Any member may be
// left empty; the assembler checks before calling.
struct AsmFileProvider {
    std::function<std::optional<std::string>(const std::string& path, const std::string& cwd)> readText;
    std::function<std::optional<Bytes>(const std::string& path, const std::string& cwd)> readBinary;
    std::function<void(const std::string& path, const Bytes& bytes)> writeFile;
    // path, bytes, loadAddress, noHeader
    std::function<void(const std::string& path, const Bytes& bytes, int start, bool noHeader)> writeDiskFile;
    std::function<bool(const std::string& spec)> canWrite;
};

struct AsmOptions {
    int origin = 0;
    const AsmFileProvider* fileProvider = nullptr;
    std::optional<std::string> currentDir;
};

struct AsmResult {
    bool ok = false;
    std::vector<AsmError> errors;
    std::vector<AsmError> warnings;
    std::vector<AsmListingItem> listing;
    int start = 0;
    int end = 0;
    Bytes bytes;
    std::set<int> assembled;
    std::map<std::string, int> symbols;
    std::map<std::string, int> equates;
    std::vector<AsmRegion> regions;
    std::optional<int> runAddress;
    std::vector<std::string> printed;
};

// ---- the assembler ----------------------------------------------------
class Z80Assembler {
public:
    // A source line carried through expansion, tagged with the file it came
    // from so WRITE can split output at READ-include boundaries.
    struct RawLine { std::string line; std::string file; bool hasFile = false; };
    struct ExpandedLine { std::string line; int lineNumber; std::string file; };
    struct Macro { std::vector<std::string> params; std::vector<std::pair<int, std::string>> body; };

    Z80Assembler();
    void reset();

    AsmResult assemble(const std::string& source, const AsmOptions& options = {});

    // ---- primitive helpers exposed for the incremental/async paths -----
    void startAssembly(const std::string& source, const AsmOptions& options);
    void setupPass(int pass);
    void runPass(int pass);
    AsmResult finishAssembly();

private:
    // symbol tables
    std::unordered_map<std::string, int> symbols;   // address labels
    std::unordered_map<std::string, int> equs;      // EQU / DEFL values
    std::unordered_set<std::string> letMap;         // redefinable via LET
    std::vector<AsmError> errors;
    std::vector<AsmError> warnings;
    std::vector<AsmListingItem> listing;
    int origin = 0;
    int pc = 0;
    int lowest = 0xffff;
    int highest = 0;
    std::optional<int> runAddress;
    int defaultOrigin = 0;
    std::optional<int> outputAddress;
    bool codeEnabled = true;
    bool listEnabled = true;
    std::unordered_map<std::string, Macro> macros;
    std::vector<ExpandedLine> expanded;
    int localCounter = 0;
    std::vector<std::string> printed;
    std::string currentDir;        // directory of the currently-included file
    bool hasCurrentDir = false;
    bool noHeader = false;
    struct PendingWrite { std::string spec; bool noHeader; int lineNumber; std::string source; std::string file; };
    std::vector<PendingWrite> pendingWrites;
    int currentBank = 0xC0;
    std::map<std::string, int> assembledBytes;  // "<bank>:<addr>" -> byte
    std::set<int> assembled;
    const AsmFileProvider* fileProvider = nullptr;

    // ---- pipeline stages ----------------------------------------------
    int evalExprSafe(const std::string& expr);
    std::vector<RawLine> inlineReads(const std::vector<RawLine>& rawLines, std::set<std::string> seen = {}, int depth = 0);
    std::vector<RawLine> inlineReadsRaw(const std::string& text, std::set<std::string> seen, int depth, const std::string& file);
    std::optional<std::string> readFileText(const std::string& path, const std::string& cwd);
    std::optional<Bytes> readFileBinary(const std::string& path, const std::string& cwd);
    std::vector<ExpandedLine> expandSource(const std::vector<RawLine>& rawLines);
    std::vector<ExpandedLine> filterConditionals(const std::vector<ExpandedLine>& expanded);
    std::vector<AsmRegion> computeRegions();
    std::string regionFileName(const std::string& baseNoExt, const std::string& ext, int count, int bank = 0xC0);

    void assembleLine(const std::string& rawLine, int lineNumber, int pass, const std::string& file);
    void assembleStatement(std::string line, int lineNumber, int pass, const std::string& original, int startPc, const std::string& file);

    // ---- symbol helpers -----------------------------------------------
    std::string symKey(const std::string& name);
    void defineSymbol(const std::string& name, int value, int lineNumber, bool equ = false);
    std::optional<int> symbolValue(const std::string& name);
    bool isKnownOpcode(const std::string& name);
    bool isDirective(const std::string& name);

    // ---- expression + encoding ----------------------------------------
    int evalExpr(const std::string& expr, int pc, int lineNumber, int pass);
    std::vector<int> encodeInstruction(std::string inst, int pc, int lineNumber, int pass);
    std::vector<int> bytesFromTemplate(const std::string& templ, const std::vector<std::string>& captures, int pc, int lineNumber, int pass);

    void error(int lineNumber, const std::string& message, const std::string& source = "");
};

// ---- free helpers (AMSDOS output, listing) ----------------------------
std::string formatAssemblerListing(const AsmResult& result);
std::string hexDump(const Bytes& bytes, int start = 0);

struct AmsdosName { std::string base; std::string ext; std::string display; };
AmsdosName sanitizeAmsdosFilename(const std::string& name = "CPCSE.BIN");

struct AmsdosHeaderOptions {
    std::string filename = "CPCSE.BIN";
    int type = 2;
    int loadAddress = 0;
    int length = 0;
    std::optional<int> entryAddress;   // defaults to loadAddress
};
Bytes createAmsdosHeader(const AmsdosHeaderOptions& options = {});
Bytes addAmsdosHeader(const Bytes& bytes, const AmsdosHeaderOptions& options = {});

} // namespace cpcse
