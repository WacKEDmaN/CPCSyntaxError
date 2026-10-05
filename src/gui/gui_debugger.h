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
    // Set by an external debugger (devserver.h): its id (0 = the UI's own), and whether
    // it is one of the GDB client's temporary stepping breakpoints (Z1).
    int remoteId = 0;
    bool remoteTemp = false;
};

struct GuiWatchpoint {
    int start = 0, end = 0;                // inclusive
    bool onRead = false, onWrite = true;
    bool enabled = true;
    int hits = 0;
    bool remote = false;                   // set by an external debugger
};

class Debugger {
public:
    explicit Debugger(EmuHost& host);

    std::vector<GuiBreakpoint> breakpoints;
    std::vector<GuiWatchpoint> watchpoints;
    // The access that last stopped the machine on a watchpoint (-1: none since the last
    // resume), for an external debugger's stop report.
    int lastWatchAddress = -1;
    bool lastWatchWrite = false;

    // Called once per UI frame: hands the enabled breakpoints and watchpoints to the core.
    void attach();

    void toggleBreakpoint(int address);
    void addBreakpoint(int address, const std::string& condition = "", bool fromAssembler = false);
    void setCondition(GuiBreakpoint& bp, const std::string& condition);
    void removeBreakpoint(size_t index);
    void clearAssemblerBreakpoints();
    void breakpointsChanged() { pcSetDirty = true; }   // after editing `breakpoints` directly

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
