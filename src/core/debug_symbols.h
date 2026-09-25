// CPCSyntaxError — Debug symbol and REMU metadata support.
// Parses bank-aware symbols, comments, aliases, breakpoints and ACE records.
#pragma once
#include "common.h"

namespace cpcse {

std::optional<int> parseNumeric(const std::string& value, std::optional<int> fallback = std::nullopt);

struct RemuSymbol {
    std::string name;
    int address = 0;
    std::optional<int> bank;
    std::string kind;
    std::string source;
    std::string fileName;
};
struct RemuComment {
    int address = 0;
    std::optional<int> bank;
    std::string text;
    std::string kind;
    std::string source;
};
struct RemuBreakpoint {
    int address = 0;
    std::optional<int> bank;
    std::string kind;
    std::string source;
    // ACE record fields (present when ace == true)
    int mask = 0, size = 0, value = 0, valueMask = 0;
    std::string name;
    bool ace = false;
};
struct RemuWatchpoint {
    int address = 0;
    std::optional<int> bank;
    std::string kind;
    std::string source;
    int mask = 0, size = 0, value = 0, valueMask = 0;
    std::string name;
    bool ace = false;
    std::string type;
    int end = 0;
};
struct RemuData {
    std::string text;
    std::vector<std::string> records;
    std::vector<RemuSymbol> symbols;
    std::vector<RemuComment> comments;
    std::vector<RemuBreakpoint> breakpoints;
    std::vector<RemuWatchpoint> watchpoints;
    std::vector<std::string> unknown;
};

// Preserved (unrecognised) snapshot chunk and installed snapshot ROMs.
struct PreservedChunk { std::string id; Bytes payload; };
struct DebugRoms { Bytes lower; bool hasLower = false; std::unordered_map<int, Bytes> upper; };
struct DebugMetadata { RemuData remu; DebugRoms roms; std::vector<PreservedChunk> preservedChunks; };

RemuData parseRemu(const std::string& text);

struct SymbolIdentity { std::string kind; int bank = 0; };

struct ParseSymbolOptions { std::string source = "import"; std::string fileName; };
std::vector<RemuSymbol> parseSymbolText(const std::string& text, const ParseSymbolOptions& options = {});

struct SymbolLookup { RemuSymbol entry; int offset; bool found = false; };

class DebugSymbolTable {
public:
    std::vector<RemuSymbol> entries;
    std::vector<RemuComment> comments;

    DebugSymbolTable() {}
    DebugSymbolTable(const std::vector<RemuSymbol>& entries, const std::vector<RemuComment>& comments = {});
    void clear();
    void clear(const std::string& source);
    void addMany(const std::vector<RemuSymbol>& entries);
    void addComments(const std::vector<RemuComment>& comments);
    SymbolLookup lookup(int address, const SymbolIdentity* identity = nullptr, bool nearest = true);
    const RemuComment* commentAt(int address, const SymbolIdentity* identity = nullptr);
    const RemuSymbol* resolve(const std::string& name, const SymbolIdentity* identity = nullptr);
    std::unordered_map<std::string, int> contextValues(const SymbolIdentity* identity = nullptr);
};

struct BuildRemuOptions {
    std::vector<RemuSymbol> symbols;
    std::vector<RemuBreakpoint> breakpoints;
    std::vector<RemuWatchpoint> watchpoints;
};
std::string buildRemuText(const DebugMetadata& metadata, const BuildRemuOptions& options = {});

} // namespace cpcse
