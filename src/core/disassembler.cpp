// CPCSyntaxError — Z80 disassembler.
#include "disassembler.h"
#include <cstdio>

namespace cpcse {

Disassembler::Disassembler() {
    r_ = { "B", "C", "D", "E", "H", "L", "(HL)", "A" };
    dd_ = { "BC", "DE", "HL", "SP" };
    cc_ = { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" };
    CB_CODES = { "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL", "BIT", "RES", "SET" };
}
std::string Disassembler::hex8(int val) { char b[8]; std::snprintf(b, sizeof(b), "%02X", val & 0xff); return b; }
std::string Disassembler::hex16(int val) { char b[8]; std::snprintf(b, sizeof(b), "%04X", val & 0xffff); return b; }

DisasmResult Disassembler::disassemble(const std::function<int(int)>& read, int pc) {
    int address = pc;
    std::vector<int> bytes;
    auto fetch = [&]() { int b = read(address); bytes.push_back(b); address = (address + 1) & 0xffff; return b; };

    int opcode = fetch();
    std::string replace; bool hasReplace = false;
    if (opcode == 0xdd || opcode == 0xfd) {
        replace = opcode == 0xdd ? "IX" : "IY"; hasReplace = true;
        opcode = fetch();
        if (opcode == 0xdd || opcode == 0xfd) return { pc, address, bytes, "???" };
    }

    auto r = [&](int offset, int index, bool useReplace, const std::string& replaceStr) -> std::string {
        index &= 0x07;
        if (!useReplace || index < 4 || index == 7) return r_[index];
        else if (index != 6) return r_[index] + replaceStr.substr(1);
        else {
            if (offset == -1) offset = fetch();
            int signed0 = offset > 127 ? offset - 256 : offset;
            std::string signStr = signed0 < 0 ? "-" : "+";
            return "(" + replaceStr + signStr + "&" + hex8(std::abs(signed0)) + ")";
        }
    };
    auto dd = [&](int index, bool useReplace, const std::string& replaceStr) -> std::string {
        index &= 0x03;
        return useReplace && index == 2 ? replaceStr : dd_[index];
    };
    auto ss = [&](int index, bool useReplace, const std::string& replaceStr) { return dd(index, useReplace, replaceStr); };
    auto qq = [&](int index, bool useReplace, const std::string& replaceStr) -> std::string { return (index & 0x03) == 3 ? std::string("AF") : dd(index, useReplace, replaceStr); };
    auto cc = [&](int index) { return cc_[index & 0x07]; };
    auto n = [&]() { return "&" + hex8(fetch()); };
    auto nn = [&]() { int lsb = fetch(); int msb = fetch(); return "&" + hex16((msb << 8) | lsb); };
    auto e = [&]() { int offset = fetch(); int signed0 = offset > 127 ? offset - 256 : offset; return "&" + hex16((address + signed0) & 0xffff); };

    auto cbCode = [&](bool useReplace, const std::string& replaceStr) -> std::string {
        int offset = !useReplace ? 0 : fetch();
        int op = fetch();
        int reg = !useReplace ? op : 6;
        std::string res;
        if (op < 0x40) res = CB_CODES[(op >> 3) & 0x07] + " " + r(offset, reg, useReplace, replaceStr);
        else res = CB_CODES[((op >> 6) & 0x03) + 7] + " " + std::to_string((op >> 3) & 0x07) + "," + r(offset, reg, useReplace, replaceStr);
        if (useReplace && (op < 0x40 || op > 0x7f) && (op & 0x07) != 6) res += "," + r(-1, op, false, "");
        return res;
    };
    auto edCode = [&]() -> std::string {
        int op = fetch();
        switch (op) {
            case 0x40: case 0x48: case 0x50: case 0x58: case 0x60: case 0x68: case 0x78: return "IN " + r(-1, op >> 3, false, "") + ",(C)";
            case 0x41: case 0x49: case 0x51: case 0x59: case 0x61: case 0x69: case 0x79: return "OUT (C)," + r(-1, op >> 3, false, "");
            case 0x42: case 0x52: case 0x62: case 0x72: return "SBC HL," + ss(op >> 4, false, "");
            case 0x43: case 0x53: case 0x63: case 0x73: return "LD (" + nn() + ")," + ss(op >> 4, false, "");
            case 0x44: case 0x4c: case 0x54: case 0x5c: case 0x64: case 0x6c: case 0x74: case 0x7c: return "NEG";
            case 0x45: case 0x55: case 0x65: case 0x75: return "RETN";
            case 0x46: case 0x4e: case 0x66: case 0x6e: return "IM 0";
            case 0x47: return "LD I,A";
            case 0x4a: case 0x5a: case 0x6a: case 0x7a: return "ADC HL," + ss(op >> 4, false, "");
            case 0x4b: case 0x5b: case 0x6b: case 0x7b: return "LD " + dd(op >> 4, false, "") + ",(" + nn() + ")";
            case 0x4d: case 0x5d: case 0x6d: case 0x7d: return "RETI";
            case 0x4f: return "LD R,A";
            case 0x56: case 0x76: return "IM 1";
            case 0x57: return "LD A,I";
            case 0x5e: case 0x7e: return "IM 2";
            case 0x5f: return "LD A,R";
            case 0x67: return "RRD";
            case 0x6f: return "RLD";
            case 0x70: return "IN (C)";
            case 0x71: return "OUT (C),0";
            case 0xa0: return "LDI"; case 0xa1: return "CPI"; case 0xa2: return "INI"; case 0xa3: return "OUTI";
            case 0xa8: return "LDD"; case 0xa9: return "CPD"; case 0xaa: return "IND"; case 0xab: return "OUTD";
            case 0xb0: return "LDIR"; case 0xb1: return "CPIR"; case 0xb2: return "INIR"; case 0xb3: return "OTIR";
            case 0xb8: return "LDDR"; case 0xb9: return "CPDR"; case 0xba: return "INDR"; case 0xbb: return "OTDR";
            default: return "???";
        }
    };

    std::string mnem = "???";
    auto rep = [&]() { return hasReplace ? replace : std::string("HL"); };
    switch (opcode) {
        case 0x00: mnem = "NOP"; break;
        case 0x01: case 0x11: case 0x21: case 0x31: mnem = "LD " + dd(opcode >> 4, hasReplace, replace) + "," + nn(); break;
        case 0x02: mnem = "LD (BC),A"; break;
        case 0x03: case 0x13: case 0x23: case 0x33: mnem = "INC " + ss(opcode >> 4, hasReplace, replace); break;
        case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c: mnem = "INC " + r(-1, opcode >> 3, hasReplace, replace); break;
        case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d: mnem = "DEC " + r(-1, opcode >> 3, hasReplace, replace); break;
        case 0x06: case 0x0e: case 0x16: case 0x1e: case 0x26: case 0x2e: case 0x36: case 0x3e: mnem = "LD " + r(-1, opcode >> 3, hasReplace, replace) + "," + n(); break;
        case 0x07: mnem = "RLCA"; break;
        case 0x08: mnem = "EX AF,AF'"; break;
        case 0x09: case 0x19: case 0x29: case 0x39: mnem = "ADD " + rep() + "," + ss(opcode >> 4, hasReplace, replace); break;
        case 0x0a: mnem = "LD A,(BC)"; break;
        case 0x0b: case 0x1b: case 0x2b: case 0x3b: mnem = "DEC " + ss(opcode >> 4, hasReplace, replace); break;
        case 0x0f: mnem = "RRCA"; break;
        case 0x10: mnem = "DJNZ " + e(); break;
        case 0x12: mnem = "LD (DE),A"; break;
        case 0x17: mnem = "RLA"; break;
        case 0x18: mnem = "JR " + e(); break;
        case 0x1a: mnem = "LD A,(DE)"; break;
        case 0x1f: mnem = "RRA"; break;
        case 0x20: mnem = "JR NZ," + e(); break;
        case 0x22: mnem = "LD (" + nn() + ")," + rep(); break;
        case 0x27: mnem = "DAA"; break;
        case 0x28: mnem = "JR Z," + e(); break;
        case 0x2a: mnem = "LD " + rep() + ",(" + nn() + ")"; break;
        case 0x2f: mnem = "CPL"; break;
        case 0x30: mnem = "JR NC," + e(); break;
        case 0x32: mnem = "LD (" + nn() + "),A"; break;
        case 0x37: mnem = "SCF"; break;
        case 0x38: mnem = "JR C," + e(); break;
        case 0x3a: mnem = "LD A,(" + nn() + ")"; break;
        case 0x3f: mnem = "CCF"; break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6f:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x77: case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
            mnem = "LD " + r(-1, opcode >> 3, hasReplace, replace) + "," + r(-1, opcode, hasReplace, replace); break;
        case 0x66: case 0x6e: mnem = "LD " + r(-1, opcode >> 3, false, "") + "," + r(-1, 6, hasReplace, replace); break;
        case 0x74: case 0x75: mnem = "LD " + r(-1, 6, hasReplace, replace) + "," + r(-1, opcode, false, ""); break;
        case 0x76: mnem = "HALT"; break;
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: mnem = "ADD A," + r(-1, opcode, hasReplace, replace); break;
        case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f: mnem = "ADC A," + r(-1, opcode, hasReplace, replace); break;
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: mnem = "SUB " + r(-1, opcode, hasReplace, replace); break;
        case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f: mnem = "SBC A," + r(-1, opcode, hasReplace, replace); break;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6: case 0xa7: mnem = "AND " + r(-1, opcode, hasReplace, replace); break;
        case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: mnem = "XOR " + r(-1, opcode, hasReplace, replace); break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7: mnem = "OR " + r(-1, opcode, hasReplace, replace); break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: mnem = "CP " + r(-1, opcode, hasReplace, replace); break;
        case 0xc0: case 0xc8: case 0xd0: case 0xd8: case 0xe0: case 0xe8: case 0xf0: case 0xf8: mnem = "RET " + cc(opcode >> 3); break;
        case 0xc1: case 0xd1: case 0xe1: case 0xf1: mnem = "POP " + qq(opcode >> 4, hasReplace, replace); break;
        case 0xc2: case 0xca: case 0xd2: case 0xda: case 0xe2: case 0xea: case 0xf2: case 0xfa: mnem = "JP " + cc(opcode >> 3) + "," + nn(); break;
        case 0xc3: mnem = "JP " + nn(); break;
        case 0xc4: case 0xcc: case 0xd4: case 0xdc: case 0xe4: case 0xec: case 0xf4: case 0xfc: mnem = "CALL " + cc(opcode >> 3) + "," + nn(); break;
        case 0xc5: case 0xd5: case 0xe5: case 0xf5: mnem = "PUSH " + qq(opcode >> 4, hasReplace, replace); break;
        case 0xc6: mnem = "ADD A," + n(); break;
        case 0xc7: case 0xcf: case 0xd7: case 0xdf: case 0xe7: case 0xef: case 0xf7: case 0xff: mnem = "RST &" + hex8(opcode & 0x38); break;
        case 0xc9: mnem = "RET"; break;
        case 0xcb: mnem = cbCode(hasReplace, replace); break;
        case 0xcd: mnem = "CALL " + nn(); break;
        case 0xce: mnem = "ADC A," + n(); break;
        case 0xd3: mnem = "OUT (" + n() + "),A"; break;
        case 0xd6: mnem = "SUB " + n(); break;
        case 0xd9: mnem = "EXX"; break;
        case 0xdb: mnem = "IN A,(" + n() + ")"; break;
        case 0xde: mnem = "SBC A," + n(); break;
        case 0xe3: mnem = "EX (SP)," + rep(); break;
        case 0xe6: mnem = "AND " + n(); break;
        case 0xe9: mnem = "JP (" + rep() + ")"; break;
        case 0xeb: mnem = "EX DE,HL"; break;
        case 0xed: mnem = edCode(); break;
        case 0xee: mnem = "XOR " + n(); break;
        case 0xf3: mnem = "DI"; break;
        case 0xf6: mnem = "OR " + n(); break;
        case 0xf9: mnem = "LD SP," + rep(); break;
        case 0xfb: mnem = "EI"; break;
        case 0xfe: mnem = "CP " + n(); break;
    }

    size_t sp = mnem.find(' ');
    if (sp != std::string::npos) {
        std::string head = mnem.substr(0, sp);
        std::string tail = mnem.substr(sp + 1);
        while (head.size() < 5) head += ' ';
        mnem = head + tail;
    }
    return { pc, address, bytes, mnem };
}

} // namespace cpcse
