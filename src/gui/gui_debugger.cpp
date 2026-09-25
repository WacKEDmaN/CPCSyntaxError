// CPCSyntaxError GUI — debugger control. See gui_debugger.h.
#include "gui_debugger.h"

#include <algorithm>
#include <any>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "emuhost.h"
#include "core/emulator.h"
#include "core/memory.h"
#include "core/z80.h"

namespace cpcse {

Debugger::Debugger(EmuHost& h) : host(h) {}

void Debugger::rebuildPcSet() {
    pcSet.clear();
    for (const auto& bp : breakpoints) if (bp.enabled) pcSet.insert(bp.address & 0xffff);
    pcSetDirty = false;
}

void Debugger::attach() {
    GX4000* e = host.emu;
    if (!e || !e->memory || !e->cpu) return;
    if (pcSetDirty) rebuildPcSet();
    e->breakpoints = &pcSet;
    e->breakpointPredicate = [this, e](int pc) {
        bool stop = false;
        for (auto& bp : breakpoints) {
            if (!bp.enabled || (bp.address & 0xffff) != pc) continue;
            bool hit = true;
            if (bp.compiled) {
                BreakpointContext ctx = breakpointContext(e, bp.hits + 1);
                ctx.read = [e](int a) { return e->memory->readMapped(a & 0xffff); };   // never trips a watchpoint
                try { hit = bp.compiled(ctx); }
                catch (const std::exception& ex) { bp.error = ex.what(); hit = true; }
            }
            if (hit) {
                bp.hits++;
                stop = true;
                char b[96]; std::snprintf(b, sizeof(b), "Breakpoint at &%04X%s", pc, bp.condition.empty() ? "" : " (condition met)");
                host.breakReason = b;
            }
        }
        return stop;
    };
    bool anyWatch = false;
    for (const auto& w : watchpoints) if (w.enabled && (w.onRead || w.onWrite)) anyWatch = true;
    host.watchArmed = anyWatch;
    e->memory->watchHook = [this, e](int address, int value, bool write) {
        if (e->watchpointPending.has_value()) return;
        for (auto& w : watchpoints) {
            if (!w.enabled || address < w.start || address > w.end) continue;
            if (write ? !w.onWrite : !w.onRead) continue;
            w.hits++;
            char b[128];
            int at = host.instructionPc >= 0 ? host.instructionPc : (e->cpu->pc & 0xffff);
            std::snprintf(b, sizeof(b), "Watchpoint: %s &%04X %s &%02X (instruction at &%04X)",
                          write ? "write" : "read", address, write ? "<-" : "->", value & 0xff, at);
            host.breakReason = b;
            e->watchpointPending = std::string(b);
            return;
        }
    };
}

bool Debugger::hasBreakpoint(int address) const {
    for (const auto& bp : breakpoints) if ((bp.address & 0xffff) == (address & 0xffff)) return true;
    return false;
}

void Debugger::addBreakpoint(int address, const std::string& condition, bool fromAssembler) {
    GuiBreakpoint bp;
    bp.address = address & 0xffff;
    bp.fromAssembler = fromAssembler;
    setCondition(bp, condition);
    breakpoints.push_back(bp);
    pcSetDirty = true;
}

void Debugger::toggleBreakpoint(int address) {
    for (size_t i = 0; i < breakpoints.size(); i++)
        if ((breakpoints[i].address & 0xffff) == (address & 0xffff)) { removeBreakpoint(i); return; }
    addBreakpoint(address);
}

void Debugger::setCondition(GuiBreakpoint& bp, const std::string& condition) {
    bp.condition = condition;
    bp.error.clear();
    bp.compiled = nullptr;
    bool blank = std::all_of(condition.begin(), condition.end(), [](char c) { return std::isspace((unsigned char)c); });
    if (blank) return;
    try { bp.compiled = compileBreakpointCondition(condition); }
    catch (const std::exception& ex) { bp.error = ex.what(); }
}

void Debugger::removeBreakpoint(size_t index) {
    if (index < breakpoints.size()) breakpoints.erase(breakpoints.begin() + index);
    pcSetDirty = true;
}

void Debugger::clearAssemblerBreakpoints() {
    breakpoints.erase(std::remove_if(breakpoints.begin(), breakpoints.end(),
                                     [](const GuiBreakpoint& b) { return b.fromAssembler; }), breakpoints.end());
    pcSetDirty = true;
}

// ------------------------------------------------------------------ execution
bool Debugger::running() const { return host.booted() && !host.paused; }

void Debugger::resumeFrom() {
    if (!host.booted() || !host.emu) return;
    // The breakpoint under the PC has already stopped us once; let it go this time.
    host.emu->breakpointSkipOnce = host.emu->cpu->pc & 0xffff;
    host.breakReason.clear();
    host.paused = false;
    host.status = "Running";
}

void Debugger::run() {
    if (!host.booted()) return;
    host.emu->stepTarget = -1;
    host.stopAfter = nullptr;
    resumeFrom();
}

void Debugger::pause() {
    if (!host.booted()) return;
    host.paused = true;
    host.emu->stepTarget = -1;       // a pending step-over/run-to must not fire later
    host.stopAfter = nullptr;
    char b[64]; std::snprintf(b, sizeof(b), "Paused at &%04X", host.emu->cpu->pc & 0xffff);
    host.status = b;
}

void Debugger::stepInto() {
    if (!host.booted()) return;
    host.paused = true;
    host.breakReason.clear();
    host.stepInstruction();
    char b[64]; std::snprintf(b, sizeof(b), "Stepped to &%04X", host.emu->cpu->pc & 0xffff);
    host.status = host.breakReason.empty() ? std::string(b) : host.breakReason;
}

int Debugger::instructionLength(int address) {
    GX4000* e = host.emu;
    auto read = [e](int a) { return e->memory->readMapped(a & 0xffff); };
    DisasmResult d = disassembler.disassemble(read, address & 0xffff);
    return ((d.next - d.addr) & 0xffff) ? ((d.next - d.addr) & 0xffff) : 1;
}

void Debugger::stepOver() {
    if (!host.booted()) return;
    GX4000* e = host.emu;
    int pc = e->cpu->pc & 0xffff;
    int op = e->memory->readMapped(pc), op2 = e->memory->readMapped((pc + 1) & 0xffff);
    bool over =
        op == 0xcd || (op & 0xc7) == 0xc4 ||        // CALL nn / CALL cc,nn
        (op & 0xc7) == 0xc7 ||                      // RST
        op == 0x10 ||                               // DJNZ
        op == 0x76 ||                               // HALT
        (op == 0xed && (op2 & 0xf4) == 0xb0);       // LDIR/CPIR/INIR/OTIR and the D forms
    if (!over) { stepInto(); return; }
    e->stepTarget = (pc + instructionLength(pc)) & 0xffff;
    host.stopAfter = nullptr;
    resumeFrom();
}

void Debugger::stepOut() {
    if (!host.booted()) return;
    GX4000* e = host.emu;
    const int startSp = e->cpu->sp & 0xffff;
    e->stepTarget = -1;
    host.stopAfter = [e, startSp](int previousPc) {
        int op = e->memory->readMapped(previousPc);
        bool ret = op == 0xc9 || (op & 0xc7) == 0xc0 ||
                   (op == 0xed && (e->memory->readMapped((previousPc + 1) & 0xffff) & 0xc7) == 0x45);
        return ret && (e->cpu->sp & 0xffff) > startSp;
    };
    resumeFrom();
}

void Debugger::runTo(int address) {
    if (!host.booted()) return;
    host.emu->stepTarget = address & 0xffff;
    host.stopAfter = nullptr;
    resumeFrom();
}

void Debugger::setPc(int address) {
    if (!host.booted()) return;
    host.emu->cpu->pc = address & 0xffff;
    host.emu->cpu->halted = false;
}

// ------------------------------------------------------------------ symbols
void Debugger::setSymbols(std::vector<std::pair<std::string, int>> list) {
    symbols = std::move(list);
    std::sort(symbols.begin(), symbols.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first; });
    labelAt.clear();
    for (const auto& s : symbols) labelAt.emplace(s.second & 0xffff, s.first);
}

int Debugger::parseAddress(const std::string& raw) const {
    std::string t;
    for (char c : raw) if (!std::isspace((unsigned char)c)) t += c;
    if (t.empty()) return -1;
    for (const auto& s : symbols) {
        if (s.first.size() != t.size()) continue;
        bool same = true;
        for (size_t i = 0; i < t.size() && same; i++) same = std::toupper((unsigned char)s.first[i]) == std::toupper((unsigned char)t[i]);
        if (same) return s.second & 0xffff;
    }
    std::string digits = t; int base = 10;
    if (t[0] == '&' || t[0] == '#' || t[0] == '$') { digits = t.substr(1); base = 16; }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { digits = t.substr(2); base = 16; }
    else if (t.back() == 'h' || t.back() == 'H') { digits = t.substr(0, t.size() - 1); base = 16; }
    if (digits.empty()) return -1;
    char* end = nullptr;
    long v = std::strtol(digits.c_str(), &end, base);
    if (!end || *end) {
        // Bare hex such as "BB5A" is what people type into a debugger.
        v = std::strtol(digits.c_str(), &end, 16);
        if (!end || *end) return -1;
    }
    return (int)(v & 0xffff);
}

} // namespace cpcse
