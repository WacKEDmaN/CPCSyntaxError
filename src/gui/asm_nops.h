// CPCSyntaxError: what each assembler line costs in NOPs (the assembler window's NOP column).
//
// THE TABLE. Every Z80 instruction (the base set, CB, ED, DD/FD and DD CB/FD CB) is run once
// on an emulated CPC of its own, from a known state, and timed by the machine's own clock
// (GX4000::machineCycles: the CPC's time, each instruction rounded to whole microseconds as
// the Gate Array makes it). A conditional instruction is run with its condition false and
// true, a repeating one (LDIR, DJNZ ...) for its last turn and for another turn: its cost is
// then "min/max". The disassembler names each one; the name, with numbers, labels and
// expressions made one placeholder, is the key. The table is made on first use.
//
// A LINE. Its comment is cut (';'), a label is passed over (a word in the first column that
// is not an instruction, or one ending in ':'), the rest is split at ':' (outside quotes)
// into instructions, and their costs add up. A word that is not an instruction (a directive,
// a macro) costs nothing; an instruction with operands the table does not know is "?".
#pragma once
#include <map>
#include <string>
#include <vector>

namespace cpcse {

struct NopCost { int min = 0, max = 0; };

// The instruction as a table key: "LD A,(IX+5)" -> "LD A,(IX+d)", "jr nz,loop" -> "JR NZ,n".
std::string nopKey(const std::string& instruction);
const std::map<std::string, NopCost>& nopTable();
bool isInstructionWord(const std::string& upperWord);

struct LineNops {
    int instructions = 0;          // instructions found on the line
    bool unknown = false;          // one of them is not in the table
    int min = 0, max = 0;          // their costs added up
    std::vector<std::string> parts;   // "LD A,(IX+5)  5" ... for a tooltip
};
LineNops lineNops(const std::string& line);
// "3", or "2/3" when the cost depends on a condition, "?" when unknown, "" for no instruction.
std::string nopText(const LineNops& l);

} // namespace cpcse
