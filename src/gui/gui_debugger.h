// CPCSyntaxError GUI — debugger control (no drawing).
//
// Breakpoints (PC, optionally with a condition in the core's breakpoint-expression
// language: "A==&3F && HITS>2", "[&BE80]==1", "WORD(SP)==&0038"), memory watchpoints (read / write
// over an address range), stepping (into, over, out, run to an address) and the symbol
// table the assembler hands over. It wires itself into the GX4000's existing debugger
// hooks every frame, so a reboot or a cartridge swap never leaves it detached.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/breakpoints.h"
#include "core/disassembler.h"

namespace cpcse {

class EmuHost;

struct GuiBreakpoint {
    int address = 0;
    bool enabled = true;
    std::string condition;                 // empty = always
    std::string error;                     // the condition's parse error, if any
    std::function<bool(const BreakpointContext&)> compiled;
    int hits = 0;
    bool fromAssembler = false;            // a RASM BRK label; replaced on every assembly
};

struct GuiWatchpoint {
    int start = 0, end = 0;                // inclusive
    bool onRead = false, onWrite = true;
    bool enabled = true;
    int hits = 0;
};

class Debugger {
public:
    explicit Debugger(EmuHost& host);

    std::vector<GuiBreakpoint> breakpoints;
    std::vector<GuiWatchpoint> watchpoints;

    // Called once per UI frame: hands the enabled breakpoints and watchpoints to the core.
    void attach();

    bool hasBreakpoint(int address) const;
    void toggleBreakpoint(int address);
    void addBreakpoint(int address, const std::string& condition = "", bool fromAssembler = false);
    void setCondition(GuiBreakpoint& bp, const std::string& condition);
    void removeBreakpoint(size_t index);
    void clearAssemblerBreakpoints();
    void breakpointsChanged() { pcSetDirty = true; }   // after editing `breakpoints` directly

    // Execution control. All of them leave the machine paused or running through the
    // host's one `paused` flag.
    bool running() const;
    void run();                            // continue (skips a breakpoint at the current PC)
    void pause();
    void stepInto();
    void stepOver();                       // CALL / RST / DJNZ / block repeats run to the next instruction
    void stepOut();                        // run until a RET takes SP above where it is now
    void runTo(int address);
    void setPc(int address);

    // Symbols (from the assembler). Addresses may carry several names; the first wins
    // in the disassembly.
    std::vector<std::pair<std::string, int>> symbols;
    std::unordered_map<int, std::string> labelAt;
    void setSymbols(std::vector<std::pair<std::string, int>> list);
    // "&4000", "#4000", "$4000", "0x4000", "4000h", "16384" or a symbol name. -1 if unknown.
    int parseAddress(const std::string& text) const;

    // Disassembler shared by the views.
    Disassembler disassembler;
    int instructionLength(int address);

private:
    EmuHost& host;
    std::unordered_set<int> pcSet;         // enabled breakpoint addresses, handed to the core
    bool pcSetDirty = true;
    void rebuildPcSet();
    void resumeFrom();                     // common part of the run-like commands
};

} // namespace cpcse
