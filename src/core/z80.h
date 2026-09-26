// CPCSyntaxError — Z80A CPU core.
// Executes base, indexed, bit, extended, interrupt and block instructions.
#pragma once
#include "common.h"

namespace cpcse {

class Z80;

// The Z80 announces every bus access to an optional observer (the debugger).
// Mirrors the plain object literals passed to notifyAccess().
struct AccessEvent {
    int pc = 0;
    std::string type;   // 'read' | 'write' | 'in' | 'out'
    int address = 0;
    int value = 0;
    int previous = -1;  // write only
    int port = -1;      // in/out only
};

// The Z80's `memory` collaborator: the machine's memory (GXMemory, or the ZX memory).
// The optional hooks (contend, accessTiming) default to doing nothing.
struct Z80Memory {
    virtual ~Z80Memory() = default;
    virtual int read(int address) = 0;
    virtual void write(int address, int value) = 0;
    virtual int contend(int address, int instructionOffset, bool write) { return 0; }
    virtual void accessTiming(int address, int instructionOffset, bool write) {}
};

// The Z80's `ports` collaborator — an object literal in CPCSyntaxError. Each optional
// method becomes a std::function slot; an empty slot models an absent method.
struct Z80Ports {
    std::function<int(int port, int timingOffset, const std::string& kind)> read;
    std::function<void(int port, int value, int timingOffset, const std::string& kind)> write;
    std::function<int(int address, int instructionOffset)> contend;
    std::function<int(int vector)> im0Address;
    std::function<void()> acknowledge;
    std::function<bool(Z80&)> edff;
};

class Z80 {
public:
    // Collaborators.
    Z80Memory* memory = nullptr;
    Z80Ports ports;

    // --- registers -------------------------------------------------------
    int a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0;
    int ap = 0, fp = 0, bp = 0, cp = 0, dp = 0, ep = 0, hp = 0, lp = 0;
    int ix = 0, iy = 0, sp = 0xffff, pc = 0, i = 0, r = 0;
    bool iff1 = false, iff2 = false;
    int im = 0;
    bool halted = false;
    int eiDelay = 0;

    // pendingInterrupt: -1 = none. Holds the vector byte otherwise.
    int pendingInterrupt = -1;
    bool pendingNmi = false;
    long long tStates = 0;

    // Bus-timing bookkeeping for the active instruction.
    //
    // ACCC §29 (p.290): "an instruction like set n,(ix+n') officially lasts 23 cycles-T
    // without being stretched. On CPC the alignment caused by the Gate Array lengthens
    // this instruction to 27 or 28 cycles-T." The Gate Array holds the Z80A's READY
    // line so that every M-cycle STARTS on a microsecond boundary; that is why every
    // CPC instruction is a whole number of microseconds, and it is also what decides
    // which microsecond a memory write lands in (chapter 8). Hosts that are not a CPC
    // -- the ZX Spectrum core shares this Z80 -- leave this false and get the plain
    // T-state positions their own contention model expects.
    bool alignBusToMicroseconds = false;
    bool inInstruction = false;
    int busOffset = 0;
    // The T-state, from the start of the instruction, at which the last REAL bus cycle
    // ended -- an opcode fetch, a memory read or write, or an I/O access. It is NOT moved
    // by busInternal(), so comparing it with the instruction's own length says whether the
    // instruction finished on the bus or trailed off in internal cycles. ACCC §27.7.2
    // needs exactly that distinction: the GATE ARRAY only drives WAIT to align a bus
    // cycle, so only an instruction that ends on one has the Gate Array's own cycles on
    // its last cycle T.
    int lastBusAccessEnd = 0;
    int waitStates = 0;
    // T-states the GATE ARRAY's WAIT inserted this instruction to start its bus cycles on
    // a microsecond (alignBusToMicroseconds). The Z80A's own last T-state is its T-state
    // count plus these; ACCC §27.3.1's "real end" of an instruction is that moment.
    int alignmentWaits = 0;
    int instructionStartPc = 0;

    // Per-instruction identity used by GX4000.cpcInstructionCycles(); -1 until set.
    int lastOpcode = -1;      // base opcode after DD/FD prefixes (-1 = none)
    // ACCC §27.4 (p.286): "The Z80A RST #38 instruction lasts 4 usec when called by
    // code. WHEN AN INTERRUPT OCCURS, THE CALL IN #38 LASTS 5 USEC." The extra
    // microsecond is the interrupt-acknowledge cycle, which the plain T-state count
    // does not carry; the host adds it when this is set.
    bool lastWasInterrupt = false;
    int lastEdOpcode = -1;    // ED second byte (null -> -1)
    bool lastIndex = false;   // truthy when a DD/FD prefix was in force
    int lastPrefixCount = 0;
    int lastBranchTaken = -1; // -1 = none, 0 = not taken, 1 = taken

    // Displacement of the active indexed operand (displacementSet says it was fetched).
    bool displacementSet = false;
    int displacement = 0;

    // Optional debugger observer.
    std::function<void(const AccessEvent&)> accessObserver;

    // Optional hook fired for every UNWIRED (undocumented, NOP-behaving) ED
    // opcode: (edSuffixByte, pcAfterInstruction). Used to implement SSM
    // (ScreenShot Management): Shaker emits `ED LL ED HH` pairs as neutral ED
    // opcodes, and this lets a host detect them without touching the hot path
    // of documented instructions. Empty by default (one null check on the rare
    // unwired-ED path only).
    std::function<void(int, int)> onUnwiredEd;

    Z80(Z80Memory* memory, Z80Ports ports);
    explicit Z80(Z80Memory* memory) : Z80(memory, Z80Ports{}) {}

    void reset();

    // 16-bit pair access.
    int af() const { return a << 8 | f; }
    void setAf(int v) { a = (v >> 8) & 0xff; f = v & 0xff; }
    int bc() const { return b << 8 | c; }
    void setBc(int v) { b = (v >> 8) & 0xff; c = v & 0xff; }
    int de() const { return d << 8 | e; }
    void setDe(int v) { d = (v >> 8) & 0xff; e = v & 0xff; }
    int hl() const { return h << 8 | l; }
    void setHl(int v) { h = (v >> 8) & 0xff; l = v & 0xff; }

    void applyMemoryWait(int address, bool write = false);
    void notifyAccess(const AccessEvent& event);
    // accessType is a literal ("read", "execute"); only an attached observer sees it.
    int busRead(int address, int cycles = 3, const char* accessType = "read");
    void busWrite(int address, int value, int cycles = 3);
    // T-states the instruction spends on its own with the bus idle. They sit BETWEEN
    // M-cycles, so they push every later access along -- which on a CPC can push it
    // into the next microsecond (ACCC §8.3: PUSH's 5-T M1 is why its first write is on
    // the 3rd microsecond and not the 2nd).
    void busInternal(int cycles) { if (inInstruction) busOffset += cycles; }
    // The start of the next M-cycle, in T-states from the start of the instruction.
    int busCycleStart() {
        return alignBusToMicroseconds ? ((busOffset + 3) & ~3) : busOffset;
    }
    int read(int address) { return busRead(address, 3, "read"); }
    void write(int address, int value) { busWrite(address, value, 3); }
    int readWord(int address) { return read(address) | read(address + 1) << 8; }
    void writeWord(int address, int value) { write(address, value); write(address + 1, value >> 8); }
    int fetch() { int value = busRead(pc, 3, "execute"); pc = (pc + 1) & 0xffff; return value; }
    int fetchOpcode();
    int fetchWord() { int low = fetch(), high = fetch(); return low | high << 8; }
    int portRead(int address, const std::string& kind = "generic");
    void portWrite(int address, int value, const std::string& kind = "generic");
    void push(int value, int internalCycles = 1);
    int pop();

    void requestInterrupt(int vector = 0xff) { pendingInterrupt = vector & 0xff; }
    void requestNmi() { pendingNmi = true; }
    int interrupt();
    int nmi();

    int step();
    int run(int tStates);

private:
    int pair(int code, int* index = nullptr);
    void setPair(int code, int value, int* index = nullptr);
    // A DD/FD instruction spends 5 internal T-states computing IX+d after it has read
    // the displacement, before it touches the operand: LD (IX+d),r is 19 T for a 4-T
    // prefix fetch, a 4-T opcode fetch, a 3-T displacement read and a 3-T write. Two
    // groups do it differently and pass their own count: LD (IX+d),n reads n first and
    // idles 2, and the DD CB group carries its idle T-states in its opcode fetch.
    int indexAddress(int* index, int internalCycles = 5);
    int reg(int code, int* index = nullptr, bool indexedRegisters = true);
    void setReg(int code, int value, int* index = nullptr, bool indexedRegisters = true);
    bool condition(int code);
    int szp(int value);
    int szxy(int value);

    int inc8(int value);
    int dec8(int value);
    void add8(int value, int carry = 0);
    void sub8(int value, int carry = 0, bool compare = false);
    void logic(int op, int value);
    int add16(int lhs, int rhs);
    int adc16(int lhs, int rhs, bool subtract = false);
    int rotate(int value, int type);
    void bit(int bit, int value, int xySource);
    void daa();

    int executeCB(int opcode, int* index = nullptr, int address = -1);
    int executeED(int opcode);
    int blockInstruction(int opcode);
    int executeBase(int opcode, int* index = nullptr);
    bool indexedTimingIncludesPrefix(int opcode);
};

} // namespace cpcse
