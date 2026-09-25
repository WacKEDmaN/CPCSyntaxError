// CPCSyntaxError — Z80A CPU core.
#include "z80.h"

namespace cpcse {

// Flag bits.
static const int S = 0x80, Z = 0x40, Y = 0x20, H = 0x10, X = 0x08, PV = 0x04, N = 0x02, C = 0x01;

Z80::Z80(Z80Memory* memory, Z80Ports ports) : memory(memory), ports(std::move(ports)) { reset(); }

void Z80::reset() {
    a = f = b = c = d = e = h = l = 0;
    ap = fp = bp = cp = dp = ep = hp = lp = 0;
    ix = iy = 0; sp = 0xffff; pc = 0; i = r = 0;
    iff1 = iff2 = false; im = 0; halted = false; eiDelay = 0;
    pendingInterrupt = -1; pendingNmi = false; tStates = 0;
    inInstruction = false; busOffset = 0; waitStates = 0;
    instructionStartPc = pc;
}

void Z80::applyMemoryWait(int address, bool write) {
    if (!inInstruction) return;
    int delay = memory->contend(address & 0xffff, busOffset + waitStates, write);
    waitStates += std::max(0, delay | 0);
}

void Z80::notifyAccess(const AccessEvent& event) {
    if (!accessObserver) return;
    AccessEvent e = event;
    e.pc = instructionStartPc & 0xffff;
    // debugger observers must never break CPU execution
    accessObserver(e);
}

int Z80::busRead(int address, int cycles, const std::string& accessType) {
    address &= 0xffff;
    if (inInstruction) { const int aligned = busCycleStart(); alignmentWaits += aligned - busOffset; busOffset = aligned; }
    applyMemoryWait(address, false);
    if (inInstruction) memory->accessTiming(address, busOffset + waitStates + cycles, false);
    int value = memory->read(address) & 0xff;
    if (accessType != "execute") {
        AccessEvent ev; ev.type = accessType; ev.address = address; ev.value = value;
        notifyAccess(ev);
    }
    if (inInstruction) { busOffset += cycles; lastBusAccessEnd = busOffset; }
    return value;
}

void Z80::busWrite(int address, int value, int cycles) {
    address &= 0xffff; value &= 0xff;
    if (inInstruction) { const int aligned = busCycleStart(); alignmentWaits += aligned - busOffset; busOffset = aligned; }
    applyMemoryWait(address, true);
    if (inInstruction) memory->accessTiming(address, busOffset + waitStates + cycles, true);
    int previous = memory->read(address) & 0xff;
    memory->write(address, value);
    AccessEvent ev; ev.type = "write"; ev.address = address; ev.value = value; ev.previous = previous;
    notifyAccess(ev);
    if (inInstruction) { busOffset += cycles; lastBusAccessEnd = busOffset; }
}

int Z80::fetchOpcode() {
    int value = busRead(pc, 4, "execute");
    pc = pc + 1 & 0xffff;
    r = (r & 0x80) | (r + 1 & 0x7f);
    return value;
}

int Z80::portRead(int address, const std::string& kind) {
    address &= 0xffff;
    if (inInstruction) { const int aligned = busCycleStart(); alignmentWaits += aligned - busOffset; busOffset = aligned; }
    int delay = ports.contend ? ports.contend(address, busOffset + waitStates) : 0;
    waitStates += std::max(0, delay | 0);
    int sampleOffset = busOffset + waitStates + 3;
    int value = ports.read ? ports.read(address, sampleOffset, kind) : 0xff;
    AccessEvent ev; ev.type = "in"; ev.port = address; ev.address = address; ev.value = value & 0xff;
    notifyAccess(ev);
    if (inInstruction) { busOffset += 4; lastBusAccessEnd = busOffset; }
    return value & 0xff;
}

void Z80::portWrite(int address, int value, const std::string& kind) {
    address &= 0xffff;
    if (inInstruction) { const int aligned = busCycleStart(); alignmentWaits += aligned - busOffset; busOffset = aligned; }
    int delay = ports.contend ? ports.contend(address, busOffset + waitStates) : 0;
    waitStates += std::max(0, delay | 0);
    int sampleOffset = busOffset + waitStates + 3;
    if (ports.write) ports.write(address, value & 0xff, sampleOffset, kind);
    AccessEvent ev; ev.type = "out"; ev.port = address; ev.address = address; ev.value = value & 0xff;
    notifyAccess(ev);
    if (inInstruction) { busOffset += 4; lastBusAccessEnd = busOffset; }
}

void Z80::push(int value, int internalCycles) {
    // Every instruction that pushes spends one internal T-state before the first
    // write: PUSH rr and RST are 11 T-states for a 4-T fetch and two 3-T writes,
    // CALL nn is 17 for a fetch, two operand reads and those two writes. On a CPC
    // that idle T-state is what puts the high byte's write in the NEXT microsecond
    // (ACCC §8.3, p.45: PUSH's high byte first appears on the 3rd microsecond).
    // An interrupt or NMI passes 0: its idle T-state is already inside the longer
    // acknowledge M-cycle, and does not come after it.
    busInternal(internalCycles);
    sp = sp - 1 & 0xffff; write(sp, value >> 8);
    sp = sp - 1 & 0xffff; write(sp, value);
}
int Z80::pop() {
    int low = read(sp); sp = sp + 1 & 0xffff;
    int high = read(sp); sp = sp + 1 & 0xffff;
    return low | high << 8;
}

int Z80::interrupt() {
    if (halted) pc = pc + 1 & 0xffff;
    halted = false; iff1 = iff2 = false;
    r = (r & 0x80) | (r + 1 & 0x7f);
    if (inInstruction) busOffset = 7; // interrupt acknowledge/internal cycle
    push(pc, 0);
    if (im == 2) pc = readWord(i << 8 | pendingInterrupt);
    else if (im == 0) pc = ports.im0Address ? ports.im0Address(pendingInterrupt) : (pendingInterrupt & 0x38);
    else pc = 0x38;
    pendingInterrupt = -1; if (ports.acknowledge) ports.acknowledge();
    lastWasInterrupt = true;   // ACCC §27.4: one more usec than the T-states give
    return im == 2 ? 19 : 13;
}

int Z80::nmi() {
    if (halted) pc = pc + 1 & 0xffff;
    halted = false; iff2 = iff1; iff1 = false;
    r = (r & 0x80) | (r + 1 & 0x7f);
    if (inInstruction) busOffset = 5;
    push(pc, 0); pc = 0x66; pendingNmi = false; return 11;
}

int Z80::pair(int code, int* index) {
    if (code == 0) return bc();
    if (code == 1) return de();
    if (code == 2) return index ? *index : hl();
    return sp;
}
void Z80::setPair(int code, int value, int* index) {
    value &= 0xffff;
    if (code == 0) setBc(value);
    else if (code == 1) setDe(value);
    else if (code == 2) { if (index) *index = value; else setHl(value); }
    else sp = value;
}
int Z80::indexAddress(int* index, int internalCycles) {
    if (!displacementSet) { displacement = signed8(fetch()); displacementSet = true; busInternal(internalCycles); }
    return (*index + displacement) & 0xffff;
}
int Z80::reg(int code, int* index, bool indexedRegisters) {
    switch (code & 7) {
        case 0: return b; case 1: return c; case 2: return d; case 3: return e;
        case 4: return index && indexedRegisters ? *index >> 8 : h;
        case 5: return index && indexedRegisters ? *index & 0xff : l;
        case 6: return read(index ? indexAddress(index) : hl());
        default: return a;
    }
}
void Z80::setReg(int code, int value, int* index, bool indexedRegisters) {
    value &= 0xff;
    switch (code & 7) {
        case 0: b = value; break; case 1: c = value; break; case 2: d = value; break; case 3: e = value; break;
        case 4: if (index && indexedRegisters) *index = (*index & 0xff) | value << 8; else h = value; break;
        case 5: if (index && indexedRegisters) *index = (*index & 0xff00) | value; else l = value; break;
        case 6: write(index ? indexAddress(index) : hl(), value); break;
        default: a = value;
    }
}
bool Z80::condition(int code) {
    switch (code & 7) {
        case 0: return !(f & Z); case 1: return !!(f & Z);
        case 2: return !(f & C); case 3: return !!(f & C);
        case 4: return !(f & PV); case 5: return !!(f & PV);
        case 6: return !(f & S); default: return !!(f & S);
    }
}
int Z80::szp(int value) { value &= 0xff; return (value & S) | (value == 0 ? Z : 0) | (value & (Y | X)) | (parity8(value) ? PV : 0); }
int Z80::szxy(int value) { value &= 0xff; return (value & S) | (value == 0 ? Z : 0) | (value & (Y | X)); }

int Z80::inc8(int value) {
    int result = value + 1 & 0xff;
    f = (f & C) | szxy(result) | ((value & 0x0f) == 0x0f ? H : 0) | (value == 0x7f ? PV : 0);
    return result;
}
int Z80::dec8(int value) {
    int result = value - 1 & 0xff;
    f = (f & C) | szxy(result) | N | ((value & 0x0f) ? 0 : H) | (value == 0x80 ? PV : 0);
    return result;
}
void Z80::add8(int value, int carry) {
    int a0 = a, sum = a0 + value + carry, result = sum & 0xff; a = result;
    f = szxy(result) | ((a0 ^ value ^ result) & H) | ((~(a0 ^ value) & (a0 ^ result) & S) ? PV : 0) | (sum > 0xff ? C : 0);
}
void Z80::sub8(int value, int carry, bool compare) {
    int a0 = a, diff = a0 - value - carry, result = diff & 0xff; if (!compare) a = result;
    f = szxy(result) | N | ((a0 ^ value ^ result) & H) | (((a0 ^ value) & (a0 ^ result) & S) ? PV : 0) | (diff < 0 ? C : 0);
    if (compare) f = (f & ~(Y | X)) | (value & (Y | X));
}
void Z80::logic(int op, int value) {
    if (op == 4) { a &= value; f = szp(a) | H; }
    else if (op == 5) { a ^= value; f = szp(a); }
    else { a |= value; f = szp(a); }
}
int Z80::add16(int lhs, int rhs) {
    int sum = lhs + rhs, result = sum & 0xffff;
    f = (f & (S | Z | PV)) | ((result >> 8) & (Y | X)) | (((lhs ^ rhs ^ result) & 0x1000) ? H : 0) | (sum > 0xffff ? C : 0);
    return result;
}
int Z80::adc16(int lhs, int rhs, bool subtract) {
    int carry = f & C, value = subtract ? -rhs - carry : rhs + carry, sum = lhs + value, result = sum & 0xffff;
    int overflow = subtract ? (lhs ^ rhs) & (lhs ^ result) : ~(lhs ^ rhs) & (lhs ^ result);
    f = ((result >> 8) & S) | (result == 0 ? Z : 0) | ((result >> 8) & (Y | X)) | (((lhs ^ rhs ^ result) & 0x1000) ? H : 0)
        | ((overflow & 0x8000) ? PV : 0) | (subtract ? N : 0) | ((sum < 0 || sum > 0xffff) ? C : 0);
    return result;
}
int Z80::rotate(int value, int type) {
    int result, carry;
    switch (type) {
        case 0: carry = value >> 7; result = value << 1 | carry; break;              // RLC
        case 1: carry = value & 1; result = value >> 1 | carry << 7; break;           // RRC
        case 2: carry = value >> 7; result = value << 1 | (f & C); break;             // RL
        case 3: carry = value & 1; result = value >> 1 | (f & C) << 7; break;         // RR
        case 4: carry = value >> 7; result = value << 1; break;                       // SLA
        case 5: carry = value & 1; result = value >> 1 | (value & 0x80); break;       // SRA
        case 6: carry = value >> 7; result = value << 1 | 1; break;                   // SLL (undocumented)
        default: carry = value & 1; result = value >> 1;                              // SRL
    }
    result &= 0xff; f = szp(result) | carry; return result;
}
void Z80::bit(int bitn, int value, int xySource) {
    int mask = 1 << bitn, set = value & mask;
    f = (f & C) | H | (set ? 0 : (Z | PV)) | ((bitn == 7 && set) ? S : 0) | (xySource & (Y | X));
}
void Z80::daa() {
    int savedFlags = f, correction = ((a & 0x0f) > 9 || (savedFlags & H)) ? 0x06 : 0;
    if ((savedFlags & C) || a > 0x99) { savedFlags |= C; correction |= 0x60; }
    if (savedFlags & N) sub8(correction); else add8(correction);
    f = (f & 0xfa) | (savedFlags & C) | (parity8(a) ? PV : 0);
}

int Z80::executeCB(int opcode, int* index, int address) {
    int group = opcode >> 6, code = opcode & 7, bitn = opcode >> 3 & 7;
    int value = index ? read(address) : code == 6 ? read(hl()) : reg(code);
    // The read-modify-write M-cycle is 4 T-states, not 3: RLC (HL) is 15 T for two 4-T
    // fetches, that read and a 3-T write.
    if (index || code == 6) busInternal(1);
    if (group == 0) {
        int result = rotate(value, bitn);
        if (index) { write(address, result); if (code != 6) setReg(code, result); }
        else if (code == 6) write(hl(), result); else setReg(code, result);
        return index ? 23 : code == 6 ? 15 : 8;
    }
    if (group == 1) {
        bit(bitn, value, index ? address >> 8 : code == 6 ? hl() >> 8 : value);
        return index ? 20 : code == 6 ? 12 : 8;
    }
    int result = group == 2 ? value & ~(1 << bitn) : value | 1 << bitn;
    if (index) { write(address, result); if (code != 6) setReg(code, result); }
    else if (code == 6) write(hl(), result); else setReg(code, result);
    return index ? 23 : code == 6 ? 15 : 8;
}

int Z80::executeED(int opcode) {
    if (opcode == 0xff && ports.edff && ports.edff(*this)) return 8;
    int rr = opcode >> 4 & 3;
    if ((opcode & 0xc7) == 0x40) { int value = portRead(bc(), "in-c"); if ((opcode >> 3 & 7) != 6) setReg(opcode >> 3 & 7, value); f = (f & C) | szp(value); return 12; }
    if ((opcode & 0xc7) == 0x41) { portWrite(bc(), (opcode >> 3 & 7) == 6 ? 0 : reg(opcode >> 3 & 7), "out-c"); return 12; }
    if ((opcode & 0xcf) == 0x42) { setHl(adc16(hl(), pair(rr), true)); return 15; }
    if ((opcode & 0xcf) == 0x4a) { setHl(adc16(hl(), pair(rr))); return 15; }
    if ((opcode & 0xcf) == 0x43) { writeWord(fetchWord(), pair(rr)); return 20; }
    if ((opcode & 0xcf) == 0x4b) { setPair(rr, readWord(fetchWord())); return 20; }
    if ((opcode & 0xc7) == 0x44) { int value = a; a = 0; sub8(value); return 8; }
    if ((opcode & 0xc7) == 0x45) { pc = pop(); iff1 = iff2; return 14; }
    if ((opcode & 0xc7) == 0x46) { static const int table[8] = { 0, 0, 1, 2, 0, 0, 1, 2 }; im = table[opcode >> 3 & 7]; return 8; }
    switch (opcode) {
        case 0x47: i = a; return 9; case 0x4f: r = a; return 9;
        case 0x57: a = i; f = (f & C) | (a & (S | Y | X)) | (a == 0 ? Z : 0) | (iff2 ? PV : 0); return 9;
        case 0x5f: a = r; f = (f & C) | (a & (S | Y | X)) | (a == 0 ? Z : 0) | (iff2 ? PV : 0); return 9;
        // RRD/RLD are 4,4,3,4,3: four idle T-states for the nibble rotate sit between
        // the read and the write.
        case 0x67: { int memory0 = read(hl()), aLow = a & 0x0f; busInternal(4); write(hl(), aLow << 4 | memory0 >> 4); a = (a & 0xf0) | (memory0 & 0x0f); f = (f & C) | szp(a); return 18; }
        case 0x6f: { int memory0 = read(hl()), aLow = a & 0x0f; busInternal(4); write(hl(), (memory0 << 4 & 0xf0) | aLow); a = (a & 0xf0) | memory0 >> 4; f = (f & C) | szp(a); return 18; }
        default: return blockInstruction(opcode);
    }
}

int Z80::blockInstruction(int opcode) {
    static const int validList[16] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa8, 0xa9, 0xaa, 0xab,
        0xb0, 0xb1, 0xb2, 0xb3, 0xb8, 0xb9, 0xba, 0xbb };
    bool valid = false; for (int v : validList) if (v == opcode) { valid = true; break; }
    if (!valid) {
        if (onUnwiredEd) onUnwiredEd(opcode, pc);   // SSM screenshot detection
        return 8; // all other undocumented ED bytes are 8T NOPs
    }
    int operation = opcode & 0x1f;
    bool repeat = !!(opcode & 0x10), decrement = !!(opcode & 0x08); int direction = decrement ? -1 : 1;
    if (operation == 0x00 || operation == 0x08 || operation == 0x10 || operation == 0x18) { // LDI/LDD/LDIR/LDDR
        // LDI/LDD are 4,4,3,3 plus 2 idle T-states after the write (16 T); the repeating
        // forms idle 5 more when they loop (21 T).
        int value = read(hl()); write(de(), value); busInternal(2); setHl(hl() + direction & 0xffff); setDe(de() + direction & 0xffff); setBc(bc() - 1 & 0xffff);
        int sum = a + value;
        f = (f & (S | Z | C)) | (bc() ? PV : 0) | (sum & X) | (sum << 4 & Y);
        if (repeat) lastBranchTaken = bc() != 0;
        if (repeat && bc()) { busInternal(5); pc = pc - 2 & 0xffff; return 21; } return 16;
    }
    if (operation == 0x01 || operation == 0x09 || operation == 0x11 || operation == 0x19) { // CPI/CPD/CPIR/CPDR
        // CPI/CPD are 4,4,3 plus 5 idle T-states (16 T); the repeating forms idle 5 more.
        int value = read(hl()), result = a - value & 0xff; busInternal(5); setHl(hl() + direction & 0xffff); setBc(bc() - 1 & 0xffff);
        bool half = (a & 0x0f) < (value & 0x0f); int adjusted = result - (half ? 1 : 0) & 0xff;
        f = (f & C) | N | (result & S) | (result == 0 ? Z : 0) | (half ? H : 0) | (bc() ? PV : 0) | (adjusted & X) | (adjusted << 4 & Y);
        if (repeat) lastBranchTaken = (bc() != 0 && result != 0);
        if (repeat && bc() && result != 0) { busInternal(5); pc = pc - 2 & 0xffff; return 21; } return 16;
    }
    bool input = operation == 0x02 || operation == 0x0a || operation == 0x12 || operation == 0x1a; // INI/IND/INIR/INDR
    int value, flagSum;
    // INI/OUTI and friends are 16 T: two 4-T fetches, ONE idle T-state, then the port
    // cycle (4 T) and the memory cycle (3 T) in whichever order the direction implies.
    busInternal(1);
    if (input) {
        value = portRead(bc(), "block-in");
        write(hl(), value);
        b = b - 1 & 0xff;
        setHl(hl() + direction & 0xffff);
        flagSum = value + c + direction & 0xff;
    } else {
        value = read(hl());
        b = b - 1 & 0xff;
        portWrite(bc(), value, "block-out");
        setHl(hl() + direction & 0xffff);
        flagSum = value + l & 0xff;
    }
    // ACCC §24.5 (p.256) OUTI/OUTD AND STATUS REGISTER: "The official documentation is
    // incorrect regarding the N and C bits of the Z80A F register for the OUTI and OUTD
    // instructions... When the sum of the value sent (in HL) to the circuit and the
    // register L (post processing (incremented/decremented)) is greater than 255, then
    // N=C=1. This potentially allows to test the end of a table without a counter."
    // flagSum is already that post-increment sum masked to 8 bits, so `flagSum < value`
    // IS "greater than 255". The compendium states only this case, so the usual
    // bit-7-of-the-byte rule for N is kept for the other one and N is forced here.
    bool sumOverflowed = flagSum < value;
    f = szxy(b)
        | ((value & 0x80) || (!input && sumOverflowed) ? N : 0)
        | (sumOverflowed ? H | C : 0)
        | (parity8((flagSum & 0x07) ^ b) ? PV : 0);
    if (repeat) lastBranchTaken = b != 0;
    if (repeat && b) { busInternal(5); pc = pc - 2 & 0xffff; return 21; } return 16;
}

int Z80::executeBase(int opcode, int* index) {
    bool indexed = index != nullptr;
    auto pairL = [&](int code) { return pair(code, index); };
    auto setPairL = [&](int code, int value) { setPair(code, value, index); };
    if (opcode >= 0x40 && opcode <= 0x7f) {
        if (opcode == 0x76) { halted = true; pc = pc - 1 & 0xffff; return 4; }
        int destination = opcode >> 3 & 7, source = opcode & 7; bool memoryOp = destination == 6 || source == 6;
        setReg(destination, reg(source, index, !memoryOp), index, !memoryOp);
        return indexed && memoryOp ? 19 : memoryOp ? 7 : 4;
    }
    if (opcode >= 0x80 && opcode <= 0xbf) {
        int operation = opcode >> 3 & 7, source = opcode & 7; bool memoryOp = source == 6; int value = reg(source, index, !memoryOp);
        if (operation < 2) add8(value, operation == 1 ? f & C : 0);
        else if (operation < 4) sub8(value, operation == 3 ? f & C : 0);
        else if (operation < 7) logic(operation, value); else sub8(value, 0, true);
        return indexed && memoryOp ? 19 : memoryOp ? 7 : 4;
    }
    // INC/DEC of a memory operand read in a 4-T M-cycle and write in a 3-T one:
    // INC (HL) is 11 T, INC (IX+d) 23. The idle T-state sits between them.
    if ((opcode & 0xc7) == 0x04) { int code = opcode >> 3 & 7; bool memoryOp = code == 6; int value = reg(code, index, !memoryOp); if (memoryOp) busInternal(1); setReg(code, inc8(value), index, !memoryOp); return indexed && memoryOp ? 23 : memoryOp ? 11 : 4; }
    if ((opcode & 0xc7) == 0x05) { int code = opcode >> 3 & 7; bool memoryOp = code == 6; int value = reg(code, index, !memoryOp); if (memoryOp) busInternal(1); setReg(code, dec8(value), index, !memoryOp); return indexed && memoryOp ? 23 : memoryOp ? 11 : 4; }
    if ((opcode & 0xc7) == 0x06) {
        int code = opcode >> 3 & 7; bool memoryOp = code == 6;
        // LD (IX+d),n is 4,4,3,3,2,3: n is read straight after the displacement and the
        // idle pair follows both, which is why it is 19 T and not 4,4,3,5,3,3 = 22.
        if (indexed && memoryOp) { int address = indexAddress(index, 0), value = fetch(); busInternal(2); write(address, value); return 19; }
        setReg(code, fetch(), index, !memoryOp); return memoryOp ? 10 : 7;
    }
    if ((opcode & 0xcf) == 0x01) { setPairL(opcode >> 4 & 3, fetchWord()); return indexed && (opcode >> 4 & 3) == 2 ? 14 : 10; }
    if ((opcode & 0xcf) == 0x03) { int code = opcode >> 4 & 3; setPairL(code, pairL(code) + 1); return indexed && code == 2 ? 10 : 6; }
    if ((opcode & 0xcf) == 0x0b) { int code = opcode >> 4 & 3; setPairL(code, pairL(code) - 1); return indexed && code == 2 ? 10 : 6; }
    if ((opcode & 0xcf) == 0x09) { int code = opcode >> 4 & 3; setPairL(2, add16(pairL(2), pairL(code))); return indexed ? 15 : 11; }
    if ((opcode & 0xe7) == 0x02) { int address = opcode & 0x10 ? de() : bc(); if (opcode & 0x08) a = read(address); else write(address, a); return 7; }
    switch (opcode) {
        case 0x00: return 4;
        case 0x07: { int cc = a >> 7; a = (a << 1 | cc) & 0xff; f = (f & (S | Z | PV)) | (a & (Y | X)) | cc; return 4; }
        case 0x0f: { int cc = a & 1; a = a >> 1 | cc << 7; f = (f & (S | Z | PV)) | (a & (Y | X)) | cc; return 4; }
        case 0x17: { int cc = a >> 7, old = f & C; a = (a << 1 | old) & 0xff; f = (f & (S | Z | PV)) | (a & (Y | X)) | cc; return 4; }
        case 0x1f: { int cc = a & 1, old = f & C; a = a >> 1 | old << 7; f = (f & (S | Z | PV)) | (a & (Y | X)) | cc; return 4; }
        case 0x08: std::swap(a, ap); std::swap(f, fp); return 4;
        case 0x10: { b = b - 1 & 0xff; int displacement0 = signed8(fetch()); lastBranchTaken = b != 0; if (b) { pc = pc + displacement0 & 0xffff; return 13; } return 8; }
        case 0x18: { int displacement0 = signed8(fetch()); pc = pc + displacement0 & 0xffff; return 12; }
        case 0x20: case 0x28: case 0x30: case 0x38: { int displacement0 = signed8(fetch()); if (condition(opcode >> 3 & 3)) { pc = pc + displacement0 & 0xffff; return 12; } return 7; }
        case 0x22: { int address = fetchWord(); writeWord(address, pairL(2)); return indexed ? 20 : 16; }
        case 0x2a: { int address = fetchWord(); setPairL(2, readWord(address)); return indexed ? 20 : 16; }
        case 0x27: daa(); return 4;
        case 0x2f: a ^= 0xff; f = (f & (S | Z | PV | C)) | N | H | (a & (Y | X)); return 4;
        case 0x32: write(fetchWord(), a); return 13;
        case 0x3a: a = read(fetchWord()); return 13;
        case 0x37: f = (f & (S | Z | PV)) | (a & (Y | X)) | C; return 4;
        case 0x3f: { int old = f & C; f = (f & (S | Z | PV)) | (a & (Y | X)) | (old ? H : C); return 4; }
        case 0xc3: pc = fetchWord(); return 10;
        case 0xc9: pc = pop(); return 10;
        case 0xcd: { int address = fetchWord(); push(pc); pc = address; return 17; }
        case 0xd3: { int port = a << 8 | fetch(); portWrite(port, a, "out-n"); return 11; }
        case 0xdb: { int port = a << 8 | fetch(); a = portRead(port, "in-n"); return 11; }
        case 0xd9: std::swap(b, bp); std::swap(c, cp); std::swap(d, dp); std::swap(e, ep); std::swap(h, hp); std::swap(l, lp); return 4;
        // EX (SP),HL is 4,3,4,3,5: the second read carries one idle T-state, and the
        // LAST write two -- and it writes the HIGH byte first, so the low byte of the
        // pair is the byte that reaches memory last.
        case 0xe3: {
            int low = read(sp), high = read(sp + 1 & 0xffff);
            busInternal(1);
            int old = pairL(2);
            write(sp + 1 & 0xffff, old >> 8); write(sp, old & 0xff);
            busInternal(2);
            setPairL(2, low | high << 8); return indexed ? 23 : 19;
        }
        case 0xe9: pc = pairL(2); return indexed ? 8 : 4;
        case 0xeb: { int value = de(); setDe(hl()); setHl(value); return 4; }
        case 0xf3: iff1 = iff2 = false; eiDelay = 0; return 4;
        case 0xf9: sp = pairL(2); return indexed ? 10 : 6;
        case 0xfb: iff1 = iff2 = true; eiDelay = 2; return 4;
        case 0xcb: return executeCB(fetchOpcode());
        case 0xed: { int extended = fetchOpcode(); lastEdOpcode = extended; return executeED(extended); }
        default: break;
    }
    if ((opcode & 0xc7) == 0xc0) { lastBranchTaken = condition(opcode >> 3 & 7); if (lastBranchTaken) { pc = pop(); return 11; } return 5; }
    if ((opcode & 0xc7) == 0xc2) { int address = fetchWord(); if (condition(opcode >> 3 & 7)) pc = address; return 10; }
    if ((opcode & 0xc7) == 0xc4) { int address = fetchWord(); if (condition(opcode >> 3 & 7)) { push(pc); pc = address; return 17; } return 10; }
    if ((opcode & 0xc7) == 0xc6) {
        int operation = opcode >> 3 & 7, value = fetch();
        if (operation < 2) add8(value, operation == 1 ? f & C : 0);
        else if (operation < 4) sub8(value, operation == 3 ? f & C : 0);
        else if (operation < 7) logic(operation, value); else sub8(value, 0, true);
        return 7;
    }
    if ((opcode & 0xc7) == 0xc7) { push(pc); pc = opcode & 0x38; return 11; }
    if ((opcode & 0xcf) == 0xc1) { int code = opcode >> 4 & 3, value = pop(); if (code == 3) setAf(value); else setPairL(code, value); return 10; }
    if ((opcode & 0xcf) == 0xc5) { int code = opcode >> 4 & 3; push(code == 3 ? af() : pairL(code)); return 11; }
    return 4; // remaining undocumented bytes behave as NOPs on an NMOS Z80
}

bool Z80::indexedTimingIncludesPrefix(int opcode) {
    if (opcode >= 0x40 && opcode <= 0x7f && opcode != 0x76) return (opcode >> 3 & 7) == 6 || (opcode & 7) == 6;
    if (opcode >= 0x80 && opcode <= 0xbf) return (opcode & 7) == 6;
    if ((opcode & 0xc7) == 0x04 || (opcode & 0xc7) == 0x05 || (opcode & 0xc7) == 0x06) return (opcode >> 3 & 7) == 6;
    if ((opcode & 0xcf) == 0x01 || (opcode & 0xcf) == 0x03 || (opcode & 0xcf) == 0x0b) return (opcode >> 4 & 3) == 2;
    if ((opcode & 0xcf) == 0x09) return true;
    return opcode == 0x22 || opcode == 0x2a || opcode == 0xe3 || opcode == 0xe9 || opcode == 0xf9;
}

int Z80::step() {
    int cycles = 0;
    instructionStartPc = pc & 0xffff;
    inInstruction = true;
    busOffset = 0;
    lastBusAccessEnd = 0;
    alignmentWaits = 0;
    waitStates = 0;
    lastOpcode = -1; lastIndex = false; lastPrefixCount = 0; lastEdOpcode = -1; lastBranchTaken = -1;
    lastWasInterrupt = false;
    if (pendingNmi) cycles = nmi();
    else if (pendingInterrupt != -1 && iff1 && eiDelay == 0) cycles = interrupt();
    else if (halted) { busRead(pc, 4, "execute"); r = (r & 0x80) | (r + 1 & 0x7f); cycles = 4; }
    else {
        displacementSet = false;
        int opcode = fetchOpcode(); int* index = nullptr; int prefixCount = 0;
        while (opcode == 0xdd || opcode == 0xfd) { index = opcode == 0xdd ? &ix : &iy; prefixCount += 1; opcode = fetchOpcode(); }
        lastOpcode = opcode; lastIndex = index != nullptr; lastPrefixCount = prefixCount;
        if (index && opcode == 0xcb) {
            // DD CB d op is 4,4,3,5,4,3 T-states: the displacement read has no idle
            // after it, and the two idle T-states belong to the OPCODE fetch that
            // follows (its M-cycle is 5 T long, not 3).
            int address = indexAddress(index, 0);
            int cbOpcode = fetch();
            busInternal(2);
            cycles = executeCB(cbOpcode, index, address) + (prefixCount - 1) * 4;
        } else {
            cycles = executeBase(opcode, index);
            if (index) cycles += (prefixCount - (indexedTimingIncludesPrefix(opcode) ? 1 : 0)) * 4;
        }
    }
    if (eiDelay > 0) eiDelay -= 1;
    cycles += waitStates;
    inInstruction = false;
    tStates += cycles; return cycles;
}

int Z80::run(int tStatesTarget) { int elapsed = 0; while (elapsed < tStatesTarget) elapsed += step(); return elapsed; }

} // namespace cpcse
