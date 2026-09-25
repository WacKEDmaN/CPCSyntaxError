// CPCSyntaxError — Binary-to-assembly converter.
#include "binary_disassembler.h"
#include <regex>
#include <deque>
#include <cstdio>

namespace cpcse {

static std::string hex8(int value) { char b[8]; std::snprintf(b, sizeof(b), "%02X", value & 0xff); return b; }
static std::string hex16(int value) { char b[8]; std::snprintf(b, sizeof(b), "%04X", value & 0xffff); return b; }

static const std::unordered_set<int> NONCANONICAL_ED = {
    0x4c, 0x54, 0x5c, 0x64, 0x6c, 0x74, 0x7c,
    0x55, 0x65, 0x75, 0x5d, 0x6d, 0x7d,
    0x4e, 0x66, 0x6e, 0x76, 0x7e, 0x63, 0x6b,
};

PreparedBinary prepareBinaryForDisassembly(const Bytes& input, int fallbackOrigin) {
    Bytes source = input;
    std::optional<AmsdosHeader> header = hasAmsdosHeader(source) ? parseAmsdosHeader(source) : std::nullopt;
    Bytes bytes = header ? Bytes(source.begin() + 128, source.end()) : source;
    if (header) {
        int declared = header->fullLength ? header->fullLength : header->logicalLength ? header->logicalLength : header->length;
        if (declared > 0 && declared < (int)bytes.size()) bytes = Bytes(bytes.begin(), bytes.begin() + declared);
    }
    int origin = header ? header->loadAddress : (fallbackOrigin & 0xffff);
    return { bytes, origin, header };
}

struct BranchTarget { std::string opcode; std::string condition; int address; bool valid = false; };
static std::string upperStr(const std::string& s) { std::string r = s; for (auto& c : r) c = (char)std::toupper((unsigned char)c); return r; }
static std::string trimStr(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }

static BranchTarget branchTarget(const std::string& mnemonic) {
    std::smatch m;
    static const std::regex re("^(CALL|JP|JR|DJNZ)\\s+(?:(NZ|Z|NC|C|PO|PE|P|M)\\s*,\\s*)?&([0-9A-F]{4})$", std::regex::icase);
    std::string subject = trimStr(mnemonic);
    if (!std::regex_search(subject, m, re)) return {};
    BranchTarget t; t.opcode = upperStr(m[1].str()); t.condition = m[2].matched ? upperStr(m[2].str()) : ""; t.address = (int)std::stol(m[3].str(), nullptr, 16); t.valid = true;
    return t;
}
static std::string replaceBranchTarget(const std::string& mnemonic, const std::string& label) {
    BranchTarget target = branchTarget(mnemonic);
    if (!target.valid) return mnemonic;
    return target.opcode + " " + (target.condition.empty() ? "" : target.condition + ",") + label;
}
static bool stableMnemonic(const DisasmResult& decoded, const std::vector<int>& raw, std::string& out) {
    std::string mnemonic = (!decoded.mnem.empty() && decoded.mnem != "???") ? decoded.mnem : "";
    bool has = !mnemonic.empty();
    if (has && raw[0] == 0xed && raw.size() >= 2 && NONCANONICAL_ED.count(raw[1])) has = false;
    if (has && (raw[0] == 0xdd || raw[0] == 0xfd)) {
        int prefixCount = 0;
        while (prefixCount < (int)raw.size() && (raw[prefixCount] == 0xdd || raw[prefixCount] == 0xfd)) prefixCount += 1;
        std::string indexName = raw[0] == 0xdd ? "IX" : "IY";
        bool indexedCbWithRegisterCopy = prefixCount == 1 && raw[1] == 0xcb && raw.size() >= 4 && (raw[3] & 7) != 6;
        if (prefixCount != 1 || mnemonic.find(indexName) == std::string::npos || indexedCbWithRegisterCopy) has = false;
    }
    out = has ? mnemonic : "";
    return has;
}
static bool isReturnInstruction(const std::string& mnemonic) {
    return std::regex_match(trimStr(mnemonic), std::regex("^(?:RET|RETI|RETN)$", std::regex::icase));
}
static std::vector<int> jumpTableSeeds(const Bytes& data, int origin) {
    std::vector<int> seeds;
    for (int start = 0; start + 8 < (int)data.size(); start += 1) {
        int at = start;
        std::vector<std::pair<int, int>> entries;
        while (at + 2 < (int)data.size() && data[at] == 0xc3) {
            int target = data[at + 1] | (data[at + 2] << 8);
            if (target < origin || target >= origin + (int)data.size()) break;
            entries.push_back({ at, target - origin });
            at += 3;
        }
        if ((int)entries.size() >= 3) {
            for (auto& item : entries) { seeds.push_back(item.first); seeds.push_back(item.second); }
            start = at - 1;
        }
    }
    return seeds;
}
static std::vector<std::string> dataLines(const Bytes& data, int start, int end, int origin) {
    std::vector<std::string> lines;
    int at = start;
    while (at < end) {
        int same = at + 1;
        while (same < end && data[same] == data[at]) same += 1;
        int repeat = same - at;
        if (repeat >= 8) {
            lines.push_back("    DS " + std::to_string(repeat) + ",&" + hex8(data[at]) + "                 ; &" + hex16(origin + at) + "-&" + hex16(origin + same - 1));
            at = same;
            continue;
        }
        int chunkEnd = std::min(end, at + 16);
        std::string db;
        for (int k = at; k < chunkEnd; k++) { if (k > at) db += ","; db += "&" + hex8(data[k]); }
        lines.push_back("    DB " + db + " ; &" + hex16(origin + at));
        at = chunkEnd;
    }
    return lines;
}

struct DisasmInstruction { int address; std::vector<int> bytes; std::string mnemonic; bool hasMnemonic; BranchTarget target; };

BinaryDisasmResult disassembleBinaryToSource(const Bytes& input, const DisassembleOptions& options) {
    PreparedBinary prepared = prepareBinaryForDisassembly(input, options.origin.value_or(0x4000));
    const Bytes& bytes = prepared.bytes;
    int origin = prepared.origin;
    Disassembler localDis;
    Disassembler* disassembler = options.disassembler ? options.disassembler : &localDis;
    std::string fileName = options.fileName.empty() ? "BINARY.BIN" : options.fileName;
    int maxLength = std::min((int)bytes.size(), 0x10000 - origin);
    Bytes data(bytes.begin(), bytes.begin() + maxLength);
    auto memRead = [&](int address) { int offset = (address & 0xffff) - origin; return offset >= 0 && offset < (int)data.size() ? data[offset] : 0; };
    auto inRange = [&](int address) { return address >= origin && address < origin + (int)data.size(); };
    auto toOffset = [&](int address) { return inRange(address) ? address - origin : -1; };

    std::unordered_map<int, DisasmInstruction> instructions;
    std::vector<int> owners(std::max(1, (int)data.size()), -1);
    std::deque<int> queue;
    std::unordered_set<int> queued;
    auto enqueue = [&](int offset) { if (offset < 0 || offset >= (int)data.size() || queued.count(offset)) return; queued.insert(offset); queue.push_back(offset); };
    enqueue(0);
    int execOffset = prepared.header ? toOffset(prepared.header->execAddress) : -1;
    if (execOffset >= 0) enqueue(execOffset);
    for (int seed : jumpTableSeeds(data, origin)) enqueue(seed);

    while (!queue.empty()) {
        int offset = queue.front(); queue.pop_front();
        while (offset >= 0 && offset < (int)data.size()) {
            if (instructions.count(offset)) break;
            if (owners[offset] != -1 && owners[offset] != offset) break;
            int address = origin + offset;
            DisasmResult decoded = disassembler->disassemble(memRead, address & 0xffff);
            int consumed = std::max(1, (int)decoded.bytes.size());
            if (consumed > (int)data.size() - offset) {
                std::vector<int> raw(data.begin() + offset, data.end());
                instructions[offset] = { address, raw, "", false, {} };
                for (int i = 0; i < (int)raw.size(); i += 1) owners[offset + i] = offset;
                break;
            }
            bool overlaps = false;
            for (int i = 0; i < consumed; i += 1) if (owners[offset + i] != -1 && owners[offset + i] != offset) { overlaps = true; break; }
            if (overlaps) break;
            std::vector<int> raw(data.begin() + offset, data.begin() + offset + consumed);
            std::string mnemonic; bool hasMnem = stableMnemonic(decoded, raw, mnemonic);
            BranchTarget target = hasMnem ? branchTarget(mnemonic) : BranchTarget{};
            instructions[offset] = { address, raw, mnemonic, hasMnem, target };
            for (int i = 0; i < consumed; i += 1) owners[offset + i] = offset;
            int next = offset + consumed;
            if (target.valid) {
                int targetOffset = toOffset(target.address);
                if (targetOffset >= 0) enqueue(targetOffset);
                bool unconditionalJump = (target.opcode == "JP" || target.opcode == "JR") && target.condition.empty();
                if (unconditionalJump) break;
            }
            if (hasMnem && (std::regex_search(mnemonic, std::regex("^JP\\s+\\(", std::regex::icase)) || isReturnInstruction(mnemonic))) break;
            offset = next;
        }
    }

    std::unordered_map<int, std::string> labels;
    if (instructions.count(0)) labels[origin] = "START";
    if (execOffset > 0 && instructions.count(execOffset)) labels[prepared.header->execAddress] = "ENTRY";
    for (auto& kv : instructions) {
        const DisasmInstruction& instruction = kv.second;
        if (!instruction.target.valid || !inRange(instruction.target.address)) continue;
        int targetOffset = instruction.target.address - origin;
        if (!instructions.count(targetOffset)) continue;
        if (!labels.count(instruction.target.address)) labels[instruction.target.address] = "L" + hex16(instruction.target.address);
    }

    std::vector<std::string> lines;
    lines.push_back("; Disassembled by CPCSyntaxError from " + fileName);
    if (prepared.header) lines.push_back("; AMSDOS " + prepared.header->typeName + " \xc2\xb7 load &" + hex16(origin) + " \xc2\xb7 exec &" + hex16(prepared.header->execAddress) + " \xc2\xb7 " + std::to_string(data.size()) + " bytes");
    else lines.push_back("; Raw binary \xc2\xb7 origin &" + hex16(origin) + " \xc2\xb7 " + std::to_string(data.size()) + " bytes");
    lines.push_back("; Recursive control-flow analysis: unreachable bytes are emitted as DB/DS data.");
    if ((int)bytes.size() > (int)data.size()) lines.push_back("; Note: " + std::to_string(bytes.size() - data.size()) + " byte(s) beyond the 64K Z80 address space were omitted.");
    lines.push_back("");
    lines.push_back("ORG &" + hex16(origin));
    if (execOffset >= 0 && instructions.count(execOffset)) lines.push_back("RUN &" + hex16(prepared.header->execAddress));
    lines.push_back("");

    int offset = 0;
    while (offset < (int)data.size()) {
        auto it = instructions.find(offset);
        if (it == instructions.end()) {
            int end = offset + 1;
            while (end < (int)data.size() && !instructions.count(end)) end += 1;
            for (auto& l : dataLines(data, offset, end, origin)) lines.push_back(l);
            lines.push_back("");
            offset = end;
            continue;
        }
        const DisasmInstruction& instruction = it->second;
        auto lit = labels.find(instruction.address);
        if (lit != labels.end()) lines.push_back(lit->second + ":");
        std::string byteComment; for (size_t k = 0; k < instruction.bytes.size(); k++) { if (k) byteComment += " "; byteComment += hex8(instruction.bytes[k]); }
        if (!instruction.hasMnemonic) {
            std::string db; for (size_t k = 0; k < instruction.bytes.size(); k++) { if (k) db += ","; db += "&" + hex8(instruction.bytes[k]); }
            lines.push_back("    DB " + db + " ; " + byteComment);
        } else {
            std::string mnemonic = instruction.mnemonic;
            if (instruction.target.valid) { auto tl = labels.find(instruction.target.address); if (tl != labels.end()) mnemonic = replaceBranchTarget(instruction.mnemonic, tl->second); }
            std::string padded = mnemonic; while (padded.size() < 24) padded += ' ';
            lines.push_back("    " + padded + " ; " + byteComment);
        }
        offset += (int)instruction.bytes.size();
    }
    lines.push_back("");

    int codeBytes = 0; for (auto& kv : instructions) codeBytes += (int)kv.second.bytes.size();

    BinaryDisasmResult result;
    { std::string src; for (size_t i = 0; i < lines.size(); i++) { src += lines[i]; if (i + 1 < lines.size()) src += "\n"; } result.source = src; }
    result.origin = origin;
    result.execAddress = prepared.header ? prepared.header->execAddress : origin;
    result.header = prepared.header;
    result.byteLength = (int)data.size();
    result.binary = data;
    result.labels = labels;
    result.codeBytes = codeBytes;
    result.dataBytes = (int)data.size() - codeBytes;
    result.truncated = (int)bytes.size() - (int)data.size();
    return result;
}

} // namespace cpcse
