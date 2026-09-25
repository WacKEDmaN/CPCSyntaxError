// CPCSyntaxError — Z80 assembler.
// Two-pass Maxam/Maxam-1.5 compatible assembler with AMSDOS binary output.
#include "z80_assembler.h"
#include <regex>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace cpcse {

// ==================================================================
//  Opcode / value tables (mnemonic string -> hex template).
//  The N / NN tokens in a mnemonic become XX / XXXX placeholders in
//  the template. opcodes[i] pairs with values[i]; the assembler only
//  iterates min(len(opcodes), len(values)).
// ==================================================================
static const char* OPCODES[] = {
  "BRK", "BREAK", "NOP", "LD BC,N", "LD (BC),A", "INC BC", "INC B", "DEC B", "LD B,N", "RLCA", "EX AF,AF'",
  "EX AF,AF", "ADD HL,BC", "LD A,(BC)", "DEC BC", "INC C", "DEC C", "LD C,N", "RRCA", "DJNZ $+N", "LD DE,NN",
  "LD (DE),A", "INC DE", "INC D", "DEC D", "LD D,N", "RLA", "JR $+N", "ADD HL,DE", "LD A,(DE)", "DEC DE",
  "INC E", "DEC E", "LD E,N", "RRA", "JR NZ,$+N", "LD HL,NN", "LD (NN),HL", "INC HL", "INC H", "DEC H",
  "LD H,N", "DAA", "JR Z,$+N", "ADD HL,HL", "LD HL,(NN)", "DEC HL", "INC L", "DEC L", "LD L,N", "CPL",
  "JR NC,$+N", "LD SP,NN", "LD (NN),A", "INC SP", "INC (HL)", "DEC (HL)", "LD (HL),N", "SCF", "JR C,$+N",
  "ADD HL,SP", "LD A,(NN)", "DEC SP", "INC A", "DEC A", "LD A,N", "CCF", "LD B,B", "LD B,C", "LD B,D",
  "LD B,E", "LD B,H", "LD B,L", "LD B,(HL)", "LD B,A", "LD C,B", "LD C,C", "LD C,D", "LD C,E", "LD C,H",
  "LD C,L", "LD C,(HL)", "LD C,A", "LD D,B", "LD D,C", "LD D,D", "LD D,E", "LD D,H", "LD D,L", "LD D,(HL)",
  "LD D,A", "LD E,B", "LD E,C", "LD E,D", "LD E,E", "LD E,H", "LD E,L", "LD E,(HL)", "LD E,A", "LD H,B",
  "LD H,C", "LD H,D", "LD H,E", "LD H,H", "LD H,L", "LD H,(HL)", "LD H,A", "LD L,B", "LD L,C", "LD L,D",
  "LD L,E", "LD L,H", "LD L,L", "LD L,(HL)", "LD L,A", "LD (HL),B", "LD (HL),C", "LD (HL),D", "LD (HL),E",
  "LD (HL),H", "LD (HL),L", "HALT", "LD (HL),A", "LD A,B", "LD A,C", "LD A,D", "LD A,E", "LD A,H", "LD A,L",
  "LD A,(HL)", "LD A,A", "ADD A,B", "ADD B", "ADD A,C", "ADD C", "ADD A,D", "ADD D", "ADD A,E", "ADD E",
  "ADD A,H", "ADD H", "ADD A,L", "ADD L", "ADD A,(HL)", "ADD (HL)", "ADD A,A", "ADD A", "ADC A,B", "ADC B",
  "ADC A,C", "ADC C", "ADC A,D", "ADC D", "ADC A,E", "ADC E", "ADC A,H", "ADC H", "ADC A,L", "ADC L",
  "ADC A,(HL)", "ADC (HL)", "ADC A,A", "ADC A", "SUB B", "SUB C", "SUB D", "SUB E", "SUB H", "SUB L",
  "SUB (HL)", "SUB A", "SBC B", "SBC C", "SBC D", "SBC E", "SBC H", "SBC L", "SBC (HL)", "SBC A", "AND B",
  "AND C", "AND D", "AND E", "AND H", "AND L", "AND (HL)", "AND A", "XOR B", "XOR C", "XOR D", "XOR E",
  "XOR H", "XOR L", "XOR (HL)", "XOR A", "OR B", "OR C", "OR D", "OR E", "OR H", "OR L", "OR (HL)", "OR A",
  "CP B", "CP C", "CP D", "CP E", "CP H", "CP L", "CP (HL)", "CP A", "RET NZ", "POP BC", "JP NZ,$+3",
  "JP $+3", "CALL NZ,NN", "PUSH BC", "ADD A,N", "RST 0", "RET Z", "RET", "JP Z,$+3", "RLC B", "RLC C",
  "RLC D", "RLC E", "RLC H", "RLC L", "RLC (HL)", "RLC A", "RRC B", "RRC C", "RRC D", "RRC E", "RRC H",
  "RRC L", "RRC (HL)", "RRC A", "RL B", "RL C", "RL D", "RL E", "RL H", "RL L", "RL (HL)", "RL A", "RR B",
  "RR C", "RR D", "RR E", "RR H", "RR L", "RR (HL)", "RR A", "SLA B", "SLA C", "SLA D", "SLA E", "SLA H",
  "SLA L", "SLA (HL)", "SLA A", "SRA B", "SRA C", "SRA D", "SRA E", "SRA H", "SRA L", "SRA (HL)", "SRA A",
  "SRL B", "SRL C", "SRL D", "SRL E", "SRL H", "SRL L", "SRL (HL)", "SRL A", "BIT 0,B", "BIT 0,C", "BIT 0,D",
  "BIT 0,E", "BIT 0,H", "BIT 0,L", "BIT 0,(HL)", "BIT 0,A", "BIT 1,B", "BIT 1,C", "BIT 1,D", "BIT 1,E",
  "BIT 1,H", "BIT 1,L", "BIT 1,(HL)", "BIT 1,A", "BIT 2,B", "BIT 2,C", "BIT 2,D", "BIT 2,E", "BIT 2,H",
  "BIT 2,L", "BIT 2,(HL)", "BIT 2,A", "BIT 3,B", "BIT 3,C", "BIT 3,D", "BIT 3,E", "BIT 3,H", "BIT 3,L",
  "BIT 3,(HL)", "BIT 3,A", "BIT 4,B", "BIT 4,C", "BIT 4,D", "BIT 4,E", "BIT 4,H", "BIT 4,L", "BIT 4,(HL)",
  "BIT 4,A", "BIT 5,B", "BIT 5,C", "BIT 5,D", "BIT 5,E", "BIT 5,H", "BIT 5,L", "BIT 5,(HL)", "BIT 5,A",
  "BIT 6,B", "BIT 6,C", "BIT 6,D", "BIT 6,E", "BIT 6,H", "BIT 6,L", "BIT 6,(HL)", "BIT 6,A", "BIT 7,B",
  "BIT 7,C", "BIT 7,D", "BIT 7,E", "BIT 7,H", "BIT 7,L", "BIT 7,(HL)", "BIT 7,A", "RES 0,B", "RES 0,C",
  "RES 0,D", "RES 0,E", "RES 0,H", "RES 0,L", "RES 0,(HL)", "RES 0,A", "RES 1,B", "RES 1,C", "RES 1,D",
  "RES 1,E", "RES 1,H", "RES 1,L", "RES 1,(HL)", "RES 1,A", "RES 2,B", "RES 2,C", "RES 2,D", "RES 2,E",
  "RES 2,H", "RES 2,L", "RES 2,(HL)", "RES 2,A", "RES 3,B", "RES 3,C", "RES 3,D", "RES 3,E", "RES 3,H",
  "RES 3,L", "RES 3,(HL)", "RES 3,A", "RES 4,B", "RES 4,C", "RES 4,D", "RES 4,E", "RES 4,H", "RES 4,L",
  "RES 4,(HL)", "RES 4,A", "RES 5,B", "RES 5,C", "RES 5,D", "RES 5,E", "RES 5,H", "RES 5,L", "RES 5,(HL)",
  "RES 5,A", "RES 6,B", "RES 6,C", "RES 6,D", "RES 6,E", "RES 6,H", "RES 6,L", "RES 6,(HL)", "RES 6,A",
  "RES 7,B", "RES 7,C", "RES 7,D", "RES 7,E", "RES 7,H", "RES 7,L", "RES 7,(HL)", "RES 7,A", "SET 0,B",
  "SET 0,C", "SET 0,D", "SET 0,E", "SET 0,H", "SET 0,L", "SET 0,(HL)", "SET 0,A", "SET 1,B", "SET 1,C",
  "SET 1,D", "SET 1,E", "SET 1,H", "SET 1,L", "SET 1,(HL)", "SET 1,A", "SET 2,B", "SET 2,C", "SET 2,D",
  "SET 2,E", "SET 2,H", "SET 2,L", "SET 2,(HL)", "SET 2,A", "SET 3,B", "SET 3,C", "SET 3,D", "SET 3,E",
  "SET 3,H", "SET 3,L", "SET 3,(HL)", "SET 3,A", "SET 4,B", "SET 4,C", "SET 4,D", "SET 4,E", "SET 4,H",
  "SET 4,L", "SET 4,(HL)", "SET 4,A", "SET 5,B", "SET 5,C", "SET 5,D", "SET 5,E", "SET 5,H", "SET 5,L",
  "SET 5,(HL)", "SET 5,A", "SET 6,B", "SET 6,C", "SET 6,D", "SET 6,E", "SET 6,H", "SET 6,L", "SET 6,(HL)",
  "SET 6,A", "SET 7,B", "SET 7,C", "SET 7,D", "SET 7,E", "SET 7,H", "SET 7,L", "SET 7,(HL)", "SET 7,A",
  "CALL Z,NN", "CALL NN", "ADC A,N", "RST &8", "RET NC", "POP DE", "JP NC,$+3", "OUT (N),A", "CALL NC,NN",
  "PUSH DE", "SUB N", "RST &10", "RET C", "EXX", "JP C,$+3", "IN A,(N)", "CALL C,NN", "ADD IX,BC",
  "ADD IX,DE", "LD IX,NN", "LD (NN),IX", "INC IX", "ADD IX,IX", "LD IX,(NN)", "DEC IX", "INC (IX+N)",
  "DEC (IX+N)", "LD (IX+N),N", "ADD IX,SP", "LD B,(IX+N)", "LD C,(IX+N)", "LD D,(IX+N)", "LD E,(IX+N)",
  "LD H,(IX+N)", "LD L,(IX+N)", "LD (IX+N),B", "LD (IX+N),C", "LD (IX+N),D", "LD (IX+N),E", "LD (IX+N),H",
  "LD (IX+N),L", "LD (IX+N),A", "LD A,(IX+N)", "ADD A,(IX+N)", "ADC A,(IX+N)", "SUB (IX+N)", "SBC A,(IX+N)",
  "AND (IX+N)", "XOR (IX+N)", "OR (IX+N)", "CP (IX+N)", "RLC (IX+N)", "RRC (IX+N)", "RL (IX+N)", "RR (IX+N)",
  "SLA (IX+N)", "SRA (IX+N)", "BIT 0,(IX+N)", "BIT 1,(IX+N)", "BIT 2,(IX+N)", "BIT 3,(IX+N)", "BIT 4,(IX+N)",
  "BIT 5,(IX+N)", "BIT 6,(IX+N)", "BIT 7,(IX+N)", "RES 0,(IX+N)", "RES 1,(IX+N)", "RES 2,(IX+N)",
  "RES 3,(IX+N)", "RES 4,(IX+N)", "RES 5,(IX+N)", "RES 6,(IX+N)", "RES 7,(IX+N)", "SET 0,(IX+N)",
  "SET 1,(IX+N)", "SET 2,(IX+N)", "SET 3,(IX+N)", "SET 4,(IX+N)", "SET 5,(IX+N)", "SET 6,(IX+N)",
  "SET 7,(IX+N)", "POP IX", "EX (SP),IX", "PUSH IX", "JP (IX)", "LD SP,IX", "SBC A,N", "RST &18", "RET PO",
  "POP HL", "JP PO,$+3", "EX (SP),HL", "CALL PO,NN", "PUSH HL", "AND N", "RST &20", "RET PE", "JP (HL)",
  "JP PE,$+3", "EX DE,HL", "EX HL,DE", "CALL PE,NN", "IN B,(C)", "OUT (C),B", "SBC HL,BC", "LD (NN),BC",
  "NEG", "RETN", "IM 0", "LD I,A", "IN C,(C)", "OUT (C),C", "ADC HL,BC", "LD BC,(NN)", "RETI", "IN D,(C)",
  "OUT (C),D", "SBC HL,DE", "LD (NN),DE", "IM 1", "LD A,I", "IN E,(C)", "OUT (C),E", "ADC HL,DE",
  "LD DE,(NN)", "IM 2", "IN H,(C)", "OUT (C),H", "SBC HL,HL", "RRD", "IN L,(C)", "OUT (C),L", "ADC HL,HL",
  "RLD", "SBC HL,SP", "LD (NN),SP", "IN A,(C)", "OUT (C),A", "ADC HL,SP", "LD SP,(NN)", "LDI", "CPI", "INI",
  "OUTI", "LDD", "CPD", "IND", "OUTD", "LDIR", "CPIR", "INIR", "OTIR", "LDDR", "CPDR", "INDR", "OTDR",
  "XOR N", "RST &28", "RET P", "POP AF", "JP P,$+3", "DI", "CALL P,NN", "PUSH AF", "OR N", "RST &30", "RET M",
  "LD SP,HL", "JP M,$+3", "EI", "CALL M,NN", "ADD IY,BC", "ADD IY,DE", "LD IY,NN", "LD (NN),IY", "INC IY",
  "ADD IY,IY", "LD IY,(NN)", "DEC IY", "INC (IY+N)", "DEC (IY+N)", "LD (IY+N),N", "ADD IY,SP", "LD B,(IY+N)",
  "LD C,(IY+N)", "LD D,(IY+N)", "LD E,(IY+N)", "LD H,(IY+N)", "LD L,(IY+N)", "LD (IY+N),B", "LD (IY+N),C",
  "LD (IY+N),D", "LD (IY+N),E", "LD (IY+N),H", "LD (IY+N),L", "LD (IY+N),A", "LD A,(IY+N)", "ADD A,(IY+N)",
  "ADC A,(IY+N)", "SUB (IY+N)", "SBC A,(IY+N)", "AND (IY+N)", "XOR (IY+N)", "OR (IY+N)", "CP (IY+N)",
  "RLC (IY+N)", "RRC (IY+N)", "RL (IY+N)", "RR (IY+N)", "SLA (IY+N)", "SRA (IY+N)", "BIT 0,(IY+N)",
  "BIT 1,(IY+N)", "BIT 2,(IY+N)", "BIT 3,(IY+N)", "BIT 4,(IY+N)", "BIT 5,(IY+N)", "BIT 6,(IY+N)",
  "BIT 7,(IY+N)", "RES 0,(IY+N)", "RES 1,(IY+N)", "RES 2,(IY+N)", "RES 3,(IY+N)", "RES 4,(IY+N)",
  "RES 5,(IY+N)", "RES 6,(IY+N)", "RES 7,(IY+N)", "SET 0,(IY+N)", "SET 1,(IY+N)", "SET 2,(IY+N)",
  "SET 3,(IY+N)", "SET 4,(IY+N)", "SET 5,(IY+N)", "SET 6,(IY+N)", "SET 7,(IY+N)", "POP IY", "EX (SP),IY",
  "PUSH IY", "JP (IY)", "LD SP,IY", "LD PC,IY", "CP N", "RST &38", "BREAKPOINT",
};
static const int OPCODES_count = 714;

static const char* OP_VALUES[] = {
  "F7", "EDFF", "00", "01XXXX", "02", "03", "04", "05", "06XX", "07", "08", "08", "09", "0A", "0B", "0C",
  "0D", "0EXX", "0F", "10XX", "11XXXX", "12", "13", "14", "15", "16XX", "17", "18XX", "19", "1A", "1B", "1C",
  "1D", "1EXX", "1F", "20XX", "21XXXX", "22XXXX", "23", "24", "25", "26XX", "27", "28XX", "29", "2AXXXX",
  "2B", "2C", "2D", "2EXX", "2F", "30XX", "31XXXX", "32XXXX", "33", "34", "35", "36XX", "37", "38XX", "39",
  "3AXXXX", "3B", "3C", "3D", "3EXX", "3F", "40", "41", "42", "43", "44", "45", "46", "47", "48", "49", "4A",
  "4B", "4C", "4D", "4E", "4F", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59", "5A", "5B", "5C",
  "5D", "5E", "5F", "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "6A", "6B", "6C", "6D", "6E",
  "6F", "70", "71", "72", "73", "74", "75", "76", "77", "78", "79", "7A", "7B", "7C", "7D", "7E", "7F", "80",
  "80", "81", "81", "82", "82", "83", "83", "84", "84", "85", "85", "86", "86", "87", "87", "88", "88", "89",
  "89", "8A", "8A", "8B", "8B", "8C", "8C", "8D", "8D", "8E", "8E", "8F", "8F", "90", "91", "92", "93", "94",
  "95", "96", "97", "98", "99", "9A", "9B", "9C", "9D", "9E", "9F", "A0", "A1", "A2", "A3", "A4", "A5", "A6",
  "A7", "A8", "A9", "AA", "AB", "AC", "AD", "AE", "AF", "B0", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8",
  "B9", "BA", "BB", "BC", "BD", "BE", "BF", "C0", "C1", "C2E300", "C3E600", "C4XXXX", "C5", "C6XX", "C7",
  "C8", "C9", "CAF200", "CB00", "CB01", "CB02", "CB03", "CB04", "CB05", "CB06", "CB07", "CB08", "CB09",
  "CB0A", "CB0B", "CB0C", "CB0D", "CB0E", "CB0F", "CB10", "CB11", "CB12", "CB13", "CB14", "CB15", "CB16",
  "CB17", "CB18", "CB19", "CB1A", "CB1B", "CB1C", "CB1D", "CB1E", "CB1F", "CB20", "CB21", "CB22", "CB23",
  "CB24", "CB25", "CB26", "CB27", "CB28", "CB29", "CB2A", "CB2B", "CB2C", "CB2D", "CB2E", "CB2F", "CB38",
  "CB39", "CB3A", "CB3B", "CB3C", "CB3D", "CB3E", "CB3F", "CB40", "CB41", "CB42", "CB43", "CB44", "CB45",
  "CB46", "CB47", "CB48", "CB49", "CB4A", "CB4B", "CB4C", "CB4D", "CB4E", "CB4F", "CB50", "CB51", "CB52",
  "CB53", "CB54", "CB55", "CB56", "CB57", "CB58", "CB59", "CB5A", "CB5B", "CB5C", "CB5D", "CB5E", "CB5F",
  "CB60", "CB61", "CB62", "CB63", "CB64", "CB65", "CB66", "CB67", "CB68", "CB69", "CB6A", "CB6B", "CB6C",
  "CB6D", "CB6E", "CB6F", "CB70", "CB71", "CB72", "CB73", "CB74", "CB75", "CB76", "CB77", "CB78", "CB79",
  "CB7A", "CB7B", "CB7C", "CB7D", "CB7E", "CB7F", "CB80", "CB81", "CB82", "CB83", "CB84", "CB85", "CB86",
  "CB87", "CB88", "CB89", "CB8A", "CB8B", "CB8C", "CB8D", "CB8E", "CB8F", "CB90", "CB91", "CB92", "CB93",
  "CB94", "CB95", "CB96", "CB97", "CB98", "CB99", "CB9A", "CB9B", "CB9C", "CB9D", "CB9E", "CB9F", "CBA0",
  "CBA1", "CBA2", "CBA3", "CBA4", "CBA5", "CBA6", "CBA7", "CBA8", "CBA9", "CBAA", "CBAB", "CBAC", "CBAD",
  "CBAE", "CBAF", "CBB0", "CBB1", "CBB2", "CBB3", "CBB4", "CBB5", "CBB6", "CBB7", "CBB8", "CBB9", "CBBA",
  "CBBB", "CBBC", "CBBD", "CBBE", "CBBF", "CBC0", "CBC1", "CBC2", "CBC3", "CBC4", "CBC5", "CBC6", "CBC7",
  "CBC8", "CBC9", "CBCA", "CBCB", "CBCC", "CBCD", "CBCE", "CBCF", "CBD0", "CBD1", "CBD2", "CBD3", "CBD4",
  "CBD5", "CBD6", "CBD7", "CBD8", "CBD9", "CBDA", "CBDB", "CBDC", "CBDD", "CBDE", "CBDF", "CBE0", "CBE1",
  "CBE2", "CBE3", "CBE4", "CBE5", "CBE6", "CBE7", "CBE8", "CBE9", "CBEA", "CBEB", "CBEC", "CBED", "CBEE",
  "CBEF", "CBF0", "CBF1", "CBF2", "CBF3", "CBF4", "CBF5", "CBF6", "CBF7", "CBF8", "CBF9", "CBFA", "CBFB",
  "CBFC", "CBFD", "CBFE", "CBFF", "CCXXXX", "CDXXXX", "CEXX", "CF", "D0", "D1", "D2F002", "D3XX", "D4XXXX",
  "D5", "D6XX", "D7", "D8", "D9", "DAFE02", "DBXX", "DCXXXX", "DD09", "DD19", "DD21XXXX", "DD22XXXX", "DD23",
  "DD29", "DD2AXXXX", "DD2B", "DD34XX", "DD35XX", "DD36XXXX", "DD39", "DD46XX", "DD4EXX", "DD56XX", "DD5EXX",
  "DD66XX", "DD6EXX", "DD70XX", "DD71XX", "DD72XX", "DD73XX", "DD74XX", "DD75XX", "DD77XX", "DD7EXX",
  "DD86XX", "DD8EXX", "DD96XX", "DD9EXX", "DDA6XX", "DDAEXX", "DDB6XX", "DDBEXX", "DDCBXX06", "DDCBXX0E",
  "DDCBXX16", "DDCBXX1E", "DDCBXX26", "DDCBXX2E", "DDCBXX46", "DDCBXX4E", "DDCBXX56", "DDCBXX5E", "DDCBXX66",
  "DDCBXX6E", "DDCBXX76", "DDCBXX7E", "DDCBXX86", "DDCBXX8E", "DDCBXX96", "DDCBXX9E", "DDCBXXA6", "DDCBXXAE",
  "DDCBXXB6", "DDCBXXBE", "DDCBXXC6", "DDCBXXCE", "DDCBXXD6", "DDCBXXDE", "DDCBXXE6", "DDCBXXEE", "DDCBXXF6",
  "DDCBXXFE", "DDE1", "DDE3", "DDE5", "DDE9", "DDF9", "DEXX", "DF", "E0", "E1", "E2F103", "E3", "E4XXXX",
  "E5", "E6XX", "E7", "E8", "E9", "EAFE03", "EB", "EB", "ECXXXX", "ED40", "ED41", "ED42", "ED43XXXX", "ED44",
  "ED45", "ED46", "ED47", "ED48", "ED49", "ED4A", "ED4BXXXX", "ED4D", "ED50", "ED51", "ED52", "ED53XXXX",
  "ED56", "ED57", "ED58", "ED59", "ED5A", "ED5BXXXX", "ED5E", "ED60", "ED61", "ED62", "ED67", "ED68", "ED69",
  "ED6A", "ED6F", "ED72", "ED73XXXX", "ED78", "ED79", "ED7A", "ED7BXXXX", "EDA0", "EDA1", "EDA2", "EDA3",
  "EDA8", "EDA9", "EDAA", "EDAB", "EDB0", "EDB1", "EDB2", "EDB3", "EDB8", "EDB9", "EDBA", "EDBB", "EEXX",
  "EF", "F0", "F1", "F28204", "F3", "F4XXXX", "F5", "F6XX", "F7", "F8", "F9", "FA8F04", "FB", "FCXXXX",
  "FD09", "FD19", "FD21XXXX", "FD22XXXX", "FD23", "FD29", "FD2AXXXX", "FD2B", "FD34XX", "FD35XX", "FD36XXXX",
  "FD39", "FD46XX", "FD4EXX", "FD56XX", "FD5EXX", "FD66XX", "FD6EXX", "FD70XX", "FD71XX", "FD72XX", "FD73XX",
  "FD74XX", "FD75XX", "FD77XX", "FD7EXX", "FD86XX", "FD8EXX", "FD96XX", "FD9EXX", "FDA6XX", "FDAEXX",
  "FDB6XX", "FDBEXX", "FDCBXX06", "FDCBXX0E", "FDCBXX16", "FDCBXX1E", "FDCBXX26", "FDCBXX2E", "FDCBXX46",
  "FDCBXX4E", "FDCBXX56", "FDCBXX5E", "FDCBXX66", "FDCBXX6E", "FDCBXX76", "FDCBXX7E", "FDCBXX86", "FDCBXX8E",
  "FDCBXX96", "FDCBXX9E", "FDCBXXA6", "FDCBXXAE", "FDCBXXB6", "FDCBXXBE", "FDCBXXC6", "FDCBXXCE", "FDCBXXD6",
  "FDCBXXDE", "FDCBXXE6", "FDCBXXEE", "FDCBXXF6", "FDCBXXFE", "FDE1", "FDE3", "FDE5", "FDE9", "FDF9", "FDF9",
  "FEXX", "FF",
};
static const int OP_VALUES_count = 713;

static const char* UNDOC_OPCODES[] = {
  "ADD IXH", "ADD IXL", "ADC IXH", "ADC IXL", "SBC IXH", "SBC IXL", "ADD IYH", "ADD IYL", "ADC IYH",
  "ADC IYL", "SBC IYH", "SBC IYL", "LD R,A", "LD A,R", "LD PC,IXH", "LD PC,IX", "SLL B", "SLL C", "SLL D",
  "SLL E", "SLL H", "SLL L", "SLL (HL)", "SLL A", "INC IXH", "DEC IXH", "LD IXH,N", "INC IXL", "DEC IXL",
  "LD IXL,N", "LD B,IXH", "LD B,IXL", "LD C,IXH", "LD C,IXL", "LD D,IXH", "LD D,IXL", "LD E,IXH", "LD E,IXL",
  "LD IXH,B", "LD IXH,C", "LD IXH,D", "LD IXH,E", "LD IXH,IXH", "LD IXH,IXL", "LD IXH,A", "LD IXL,B",
  "LD IXL,C", "LD IXL,D", "LD IXL,E", "LD IXL,IXH", "LD IXL,IXL", "LD IXL,A", "LD A,IXH", "LD A,IXL",
  "ADD A,IXH", "ADD A,IXL", "ADC A,IXH", "ADC A,IXL", "SUB IXH", "SUB IXL", "SBC A,IXH", "SBC A,IXL",
  "AND IXH", "AND IXL", "XOR IXH", "XOR IXL", "OR IXH", "OR IXL", "CP IXH", "CP IXL", "INC IYH", "DEC IYH",
  "LD IYH,N", "INC IYL", "DEC IYL", "LD IYL,N", "LD B,IYH", "LD B,IYL", "LD C,IYH", "LD C,IYL", "LD D,IYH",
  "LD D,IYL", "LD E,IYH", "LD E,IYL", "LD IYH,B", "LD IYH,C", "LD IYH,D", "LD IYH,E", "LD IYH,IYH",
  "LD IYH,IYL", "LD IYH,A", "LD IYL,B", "LD IYL,C", "LD IYL,D", "LD IYL,E", "LD IYL,IYH", "LD IYL,IYL",
  "LD IYL,A", "LD A,IYH", "LD A,IYL", "ADD A,IYH", "ADD A,IYL", "ADC A,IYH", "ADC A,IYL", "SUB IYH",
  "SUB IYL", "SBC A,IYH", "SBC A,IYL", "AND IYH", "AND IYL", "XOR IYH", "XOR IYL", "OR IYH", "OR IYL",
  "CP IYH", "CP IYL",
};
static const int UNDOC_OPCODES_count = 116;

static const char* UNDOC_VALUES[] = {
  "DD84", "DD85", "DD8C", "DD8D", "DD9C", "DD9D", "FD84", "FD85", "FD8C", "FD8D", "FD9C", "FD9D", "ED4F",
  "ED5F", "DDE9", "DDE9", "CB30", "CB31", "CB32", "CB33", "CB34", "CB35", "CB36", "CB37", "DD24", "DD25",
  "DD26XX", "DD2C", "DD2D", "DD2EXX", "DD44", "DD45", "DD4C", "DD4D", "DD54", "DD55", "DD5C", "DD5D", "DD60",
  "DD61", "DD62", "DD63", "DD64", "DD65", "DD67", "DD68", "DD69", "DD6A", "DD6B", "DD6C", "DD6D", "DD6F",
  "DD7C", "DD7D", "DD84", "DD85", "DD8C", "DD8D", "DD94", "DD95", "DD9C", "DD9D", "DDA4", "DDA5", "DDAC",
  "DDAD", "DDB4", "DDB5", "DDBC", "DDBD", "FD24", "FD25", "FD26XX", "FD2C", "FD2D", "FD2EXX", "FD44", "FD45",
  "FD4C", "FD4D", "FD54", "FD55", "FD5C", "FD5D", "FD60", "FD61", "FD62", "FD63", "FD64", "FD65", "FD67",
  "FD68", "FD69", "FD6A", "FD6B", "FD6C", "FD6D", "FD6F", "FD7C", "FD7D", "FD84", "FD85", "FD8C", "FD8D",
  "FD94", "FD95", "FD9C", "FD9D", "FDA4", "FDA5", "FDAC", "FDAD", "FDB4", "FDB5", "FDBC", "FDBD",
};
static const int UNDOC_VALUES_count = 116;

// ==================================================================
//  Small string / regex utility helpers.
// ==================================================================
namespace {

std::string toUpper(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return r;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

bool reTest(const std::string& s, const std::regex& re) { return std::regex_search(s, re); }

// Replace using a per-match callback.
std::string reReplaceCb(const std::string& s, const std::regex& re,
                        const std::function<std::string(const std::smatch&)>& fn) {
    std::string out;
    auto it = std::sregex_iterator(s.begin(), s.end(), re);
    auto end = std::sregex_iterator();
    size_t last = 0;
    for (; it != end; ++it) {
        const std::smatch& m = *it;
        size_t pos = (size_t)m.position();
        out.append(s, last, pos - last);
        out += fn(m);
        last = pos + (size_t)m.length();
    }
    out.append(s, last, s.size() - last);
    return out;
}

std::string escapeRegex(const std::string& value) {
    static const std::string special = ".*+?^${}()|[]\\";
    std::string out;
    for (char c : value) {
        if (special.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

// ---- normalizeInstruction ----------------------------------------
std::string normalizeInstruction(const std::string& value) {
    static const std::regex reTab("\t");
    static const std::regex reWs("\\s+");
    static const std::regex reComma("\\s*,\\s*");
    static const std::regex reParenOpen("\\(\\s*");
    static const std::regex reParenClose("\\s*\\)");
    static const std::regex reIxMinus("\\((IX|IY)-");
    std::string s = toUpper(value);
    s = std::regex_replace(s, reTab, " ");
    s = std::regex_replace(s, reWs, " ");
    s = std::regex_replace(s, reComma, ",");
    s = std::regex_replace(s, reParenOpen, "(");
    s = std::regex_replace(s, reParenClose, ")");
    s = std::regex_replace(s, reIxMinus, "($1+-");
    return trim(s);
}

// A quote only opens a string if it is NOT directly after a word character
// (keeps AF' / BC' register apostrophes out of the string logic).
bool isStringQuoteStart(char ch, char prev, bool hasPrev) {
    if (ch != '"' && ch != '\'') return false;
    if (ch == '"') return true;
    if (!hasPrev) return true;
    return !((prev >= 'A' && prev <= 'Z') || (prev >= 'a' && prev <= 'z') ||
             (prev >= '0' && prev <= '9') || prev == '_' || prev == '.');
}

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    int depth = 0;
    for (size_t i = 0; i < value.size(); i++) {
        char ch = value[i];
        char prev = i > 0 ? value[i - 1] : 0;
        if (quote) {
            cur += ch;
            if (ch == quote && prev != '\\') quote = 0;
            continue;
        }
        if (isStringQuoteStart(ch, prev, i > 0)) { quote = ch; cur += ch; continue; }
        if (ch == '(') { depth += 1; cur += ch; continue; }
        if (ch == ')') { depth = std::max(0, depth - 1); cur += ch; continue; }
        if (ch == ',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
        cur += ch;
    }
    bool endsComma = !value.empty() && value.back() == ',';
    if (!trim(cur).empty() || endsComma) out.push_back(trim(cur));
    return out;
}

std::string stripComment(const std::string& line) {
    std::string out;
    char quote = 0;
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        char prev = i > 0 ? line[i - 1] : 0;
        if (quote) {
            out += ch;
            if (ch == quote && prev != '\\') quote = 0;
            continue;
        }
        if (isStringQuoteStart(ch, prev, i > 0)) { quote = ch; out += ch; continue; }
        if (ch == ';') break;
        out += ch;
    }
    return out;
}

std::string cleanLine(const std::string& line) {
    std::string s = stripComment(line);
    std::string r;
    for (char c : s) if (c != '\r') r += c;
    return trim(r);
}

// Returns the unquoted string, or nullopt if the token is not a quoted string.
std::optional<std::string> unquoteString(const std::string& token) {
    std::string t = trim(token);
    bool dq = t.size() >= 2 && t.front() == '"' && t.back() == '"';
    bool sq = t.size() >= 2 && t.front() == '\'' && t.back() == '\'';
    if (!(dq || sq)) return std::nullopt;
    std::string inner = t.substr(1, t.size() - 2);
    // \n \r \t escapes
    std::string out;
    for (size_t i = 0; i < inner.size(); i++) {
        if (inner[i] == '\\' && i + 1 < inner.size()) {
            char n = inner[i + 1];
            if (n == 'n') { out += '\n'; i++; continue; }
            if (n == 'r') { out += '\r'; i++; continue; }
            if (n == 't') { out += '\t'; i++; continue; }
        }
        out += inner[i];
    }
    return out;
}

// ---- ECMAScript-style 32-bit coercions for the expression evaluator ----
int32_t toInt32(double d) {
    if (!std::isfinite(d)) return 0;
    double m = std::fmod(std::trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}
uint32_t toUint32(double d) { return (uint32_t)toInt32(d); }

struct EToken { int type; double value; std::string op; }; // type 0=num, 1=op

std::vector<EToken> tokenizeExpression(const std::string& expr,
                                       const std::function<std::optional<double>(const std::string&)>& resolve) {
    std::vector<EToken> tokens;
    const std::string& s = expr;
    size_t i = 0, n = s.size();
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto isHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    auto isWordStart = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '$'; };
    auto isWordCh = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$'; };
    while (i < n) {
        char ch = s[i];
        if (ch == ' ' || ch == '\t') { i++; continue; }
        if (ch == '\'') {
            size_t close = s.find('\'', i + 1);
            if (close == std::string::npos) throw std::runtime_error("Unterminated character literal");
            std::string inner = s.substr(i + 1, close - (i + 1));
            tokens.push_back({ 0, inner.size() ? (double)(unsigned char)inner[0] : 0.0, "" });
            i = close + 1; continue;
        }
        if (ch == '"') throw std::runtime_error("String not allowed inside expression");
        if (ch == '&' || ch == '#') {
            size_t j = i + 1;
            if (j < n && s[j] == '#') j++;
            size_t h = j;
            while (h < n && isHex(s[h])) h++;
            if (h > j) { tokens.push_back({ 0, (double)std::strtol(s.substr(j, h - j).c_str(), nullptr, 16), "" }); i = h; continue; }
            if (ch == '&') { tokens.push_back({ 1, 0, "&" }); i++; continue; }
            throw std::runtime_error("Bad hex literal");
        }
        if (ch == '%') {
            size_t b = i + 1;
            while (b < n && (s[b] == '0' || s[b] == '1')) b++;
            if (b > i + 1) { tokens.push_back({ 0, (double)std::strtol(s.substr(i + 1, b - (i + 1)).c_str(), nullptr, 2), "" }); i = b; continue; }
            throw std::runtime_error("Bad binary literal");
        }
        if (ch == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            size_t h = i + 2;
            while (h < n && isHex(s[h])) h++;
            if (h > i + 2) { tokens.push_back({ 0, (double)std::strtol(s.substr(i + 2, h - (i + 2)).c_str(), nullptr, 16), "" }); i = h; continue; }
        }
        if (isDigit(ch)) {
            // A label may start with a digit (e.g. 14khz). If leading digits are
            // followed by a word char and the whole word resolves, use that symbol.
            size_t w = i;
            while (w < n && (isWordCh(s[w]) && s[w] != '$')) w++;
            std::string word = s.substr(i, w - i);
            bool hasAlpha = false;
            for (size_t k = 1; k < word.size(); k++) { char c = word[k]; if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.') { hasAlpha = true; break; } }
            if (word.size() > 1 && hasAlpha) {
                auto sv = resolve(word);
                if (sv.has_value()) { tokens.push_back({ 0, *sv, "" }); i += word.size(); continue; }
            }
            size_t d = i;
            while (d < n && isDigit(s[d])) d++;
            std::string digits = s.substr(i, d - i);
            char after = d < n ? s[d] : 0;
            if (after == 'H' || after == 'h') {
                tokens.push_back({ 0, (double)std::strtol(digits.c_str(), nullptr, 16), "" });
                i = d + 1; continue;
            }
            tokens.push_back({ 0, (double)std::strtol(digits.c_str(), nullptr, 10), "" });
            i = d; continue;
        }
        if (isWordStart(ch)) {
            size_t j = i;
            while (j < n && isWordCh(s[j])) j++;
            std::string word = s.substr(i, j - i);
            std::string up = toUpper(word);
            if (up == "AND") { tokens.push_back({ 1, 0, "&" }); i = j; continue; }
            if (up == "OR") { tokens.push_back({ 1, 0, "|" }); i = j; continue; }
            if (up == "XOR") { tokens.push_back({ 1, 0, "^" }); i = j; continue; }
            if (up == "MOD") { tokens.push_back({ 1, 0, "%" }); i = j; continue; }
            if (up == "SHL") { tokens.push_back({ 1, 0, "<<" }); i = j; continue; }
            if (up == "SHR") { tokens.push_back({ 1, 0, ">>" }); i = j; continue; }
            if (up == "NOT") { tokens.push_back({ 1, 0, "~" }); i = j; continue; }
            auto v = resolve(word);
            if (!v.has_value()) throw std::runtime_error("Unknown symbol: " + word);
            tokens.push_back({ 0, *v, "" });
            i = j; continue;
        }
        if (ch == '<' && i + 1 < n && s[i + 1] == '<') { tokens.push_back({ 1, 0, "<<" }); i += 2; continue; }
        if (ch == '>' && i + 1 < n && s[i + 1] == '>') { tokens.push_back({ 1, 0, ">>" }); i += 2; continue; }
        if (std::string("+-*/%&|^~()<>=!").find(ch) != std::string::npos) { tokens.push_back({ 1, 0, std::string(1, ch) }); i++; continue; }
        throw std::runtime_error(std::string("Bad character in expression: ") + ch);
    }
    return tokens;
}

double applyOp(double a, const std::string& op, double b) {
    if (op == "+") return a + b;
    if (op == "-") return a - b;
    if (op == "*") return a * b;
    if (op == "/") return std::trunc(a / b);
    if (op == "%") { double r = std::fmod(a, b); return std::fmod(r + b, b); } // MOD (non-negative)
    if (op == "&") return (double)(toInt32(a) & toInt32(b));
    if (op == "|") return (double)(toInt32(a) | toInt32(b));
    if (op == "^") return (double)(toInt32(a) ^ toInt32(b));
    if (op == "<<") return (double)(toInt32(a) << (toUint32(b) & 31));
    if (op == ">>") return (double)(toInt32(a) >> (toUint32(b) & 31));
    throw std::runtime_error("Unknown operator: " + op);
}

// Strict left-to-right (no precedence) via a single-level RPN conversion.
double evalTokensLeftToRight(const std::vector<EToken>& tokens) {
    std::vector<EToken> rpn;
    std::vector<EToken> stack;
    for (const auto& t : tokens) {
        if (t.type == 0) { rpn.push_back(t); continue; }
        if (t.op == "(") { stack.push_back(t); continue; }
        if (t.op == ")") {
            while (!stack.empty() && stack.back().op != "(") { rpn.push_back(stack.back()); stack.pop_back(); }
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        if (t.op == "~") { rpn.push_back({ 2, 0, "~" }); continue; } // unary marker
        while (!stack.empty() && stack.back().op != "(") { rpn.push_back(stack.back()); stack.pop_back(); }
        stack.push_back(t);
    }
    while (!stack.empty()) { rpn.push_back(stack.back()); stack.pop_back(); }

    std::vector<double> estack;
    for (const auto& t : rpn) {
        if (t.type == 0) { estack.push_back(t.value); continue; }
        if (t.type == 2) {
            double v = estack.empty() ? 0 : estack.back(); if (!estack.empty()) estack.pop_back();
            estack.push_back((double)(~toInt32(v)));
            continue;
        }
        double b = estack.empty() ? 0 : estack.back(); if (!estack.empty()) estack.pop_back();
        double a = estack.empty() ? 0 : estack.back(); if (!estack.empty()) estack.pop_back();
        estack.push_back(applyOp(a, t.op, b));
    }
    return estack.empty() ? 0 : estack[0];
}

// ---- condition-code table ----------------------------------------
struct CondEntry { int jp; int call; int ret; int jr; }; // jr == -1 => none
const std::unordered_map<std::string, CondEntry>& conditions() {
    static const std::unordered_map<std::string, CondEntry> m = {
        { "NZ", { 0xc2, 0xc4, 0xc0, 0x20 } },
        { "Z",  { 0xca, 0xcc, 0xc8, 0x28 } },
        { "NC", { 0xd2, 0xd4, 0xd0, 0x30 } },
        { "C",  { 0xda, 0xdc, 0xd8, 0x38 } },
        { "PO", { 0xe2, 0xe4, 0xe0, -1 } },
        { "PE", { 0xea, 0xec, 0xe8, -1 } },
        { "P",  { 0xf2, 0xf4, 0xf0, -1 } },
        { "M",  { 0xfa, 0xfc, 0xf8, -1 } },
    };
    return m;
}
const CondEntry* findCond(const std::string& c) {
    auto& m = conditions();
    auto it = m.find(c);
    return it == m.end() ? nullptr : &it->second;
}

const std::unordered_set<std::string>& directiveSet() {
    static const std::unordered_set<std::string> s = {
        "ORG","AORG","RUN","ENT","END","STOP","PRINT","ALIGN","BRK","BREAK",
        "DB","DEFB","DEFM","DM","TEXT","BYTE","DW","DEFW","WORD","DS","DEFS","RMEM","STR",
        "EQU","DE","DEFL","LET","DEFINE","LIST","NOLIST","CODE","NOCODE","NOHEADER",
        "IF","IFDEF","IFNDEF","IFNOT","ELSE","ELSEIF","ENDIF","MACRO","MEND","ENDM",
        "REPEAT","REND","WHILE","WEND","INCBIN","READ","WRITE","LIMIT","BANK"
    };
    return s;
}

const std::unordered_set<std::string>& opmnemonicSet() {
    static const std::unordered_set<std::string> s = []() {
        std::unordered_set<std::string> set;
        auto addLeading = [&](const char* op) {
            std::string m;
            for (const char* p = op; *p; p++) { char c = *p; if (c >= 'A' && c <= 'Z') m += c; else break; }
            if (!m.empty()) set.insert(m);
        };
        for (int i = 0; i < OPCODES_count; i++) addLeading(OPCODES[i]);
        for (int i = 0; i < UNDOC_OPCODES_count; i++) addLeading(UNDOC_OPCODES[i]);
        for (auto& kv : conditions()) set.insert(kv.first);
        return set;
    }();
    return s;
}

bool isOpcodeToken(const std::string& tok) {
    std::string k = toUpper(trim(tok));
    if (k.empty()) return false;
    return opmnemonicSet().count(k) != 0;
}

std::vector<std::string> splitStatements(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    char quote = 0;
    static const std::regex reBareIdent("^[A-Za-z_.][A-Za-z0-9_.]*$");
    auto isBareIdent = [&](const std::string& s) { return std::regex_search(trim(s), reBareIdent); };
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        char prev = i > 0 ? line[i - 1] : 0;
        if (quote) {
            cur += ch;
            if (ch == quote && prev != '\\') quote = 0;
            continue;
        }
        if (isStringQuoteStart(ch, prev, i > 0)) { quote = ch; cur += ch; continue; }
        if (ch == '(') { depth += 1; cur += ch; continue; }
        if (ch == ')') { depth = std::max(0, depth - 1); cur += ch; continue; }
        if (ch == ':' && depth == 0 && (!isBareIdent(cur) || isOpcodeToken(cur))) {
            if (!trim(cur).empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += ch;
    }
    if (!trim(cur).empty()) out.push_back(cur);
    if (out.empty()) out.push_back(line);
    return out;
}

bool hasAmsdosHeaderBytes(const Bytes& data) {
    if (data.size() < 128) return false;
    int checksum = 0;
    for (int i = 0; i < 67; i++) checksum = (checksum + data[i]) & 0xffff;
    return checksum == (data[67] | (data[68] << 8));
}

std::string dirOfSpec(const std::string& spec, const std::string& base) {
    std::string s;
    for (char c : spec) s += (c == '\\') ? '/' : c;
    size_t slash = s.rfind('/');
    if (slash != std::string::npos) {
        std::string d = s.substr(0, slash);
        if (!d.empty()) return d;
        return (!s.empty() && s[0] == '/') ? "/" : "";
    }
    return base;
}

// ---- compiled instruction pattern --------------------------------
struct Pattern {
    std::string pattern;
    std::regex regex;
    std::vector<std::string> captures;
    std::string templ;
};

std::optional<Pattern> compilePattern(const std::string& patternIn, const std::string& templ) {
    std::string p = normalizeInstruction(patternIn);
    if (p.empty() || p.find("$+") != std::string::npos) return std::nullopt;
    std::string regex = "^";
    std::vector<std::string> captures;
    size_t i = 0, n = p.size();
    auto isWord = [&](int idx) -> bool {
        if (idx < 0 || idx >= (int)n) return false;
        char c = p[idx];
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    while (i < n) {
        if (i + 1 < n && p[i] == 'N' && p[i + 1] == 'N' && !isWord((int)i - 1) && !isWord((int)i + 2)) {
            regex += "(.+)"; captures.push_back("expr"); i += 2; continue;
        }
        if (p[i] == 'N' && !isWord((int)i - 1) && !isWord((int)i + 1)) {
            regex += "(.+)"; captures.push_back("expr"); i += 1; continue;
        }
        char ch = p[i++];
        regex += (ch == ' ') ? std::string("\\s+") : escapeRegex(std::string(1, ch));
    }
    regex += "$";
    try {
        Pattern e;
        e.pattern = p;
        e.regex = std::regex(regex);
        e.captures = captures;
        e.templ = templ;
        return e;
    } catch (...) { return std::nullopt; }
}

const std::vector<Pattern>& rawPatterns() {
    static const std::vector<Pattern> pats = []() {
        std::vector<Pattern> v;
        int nb = std::min(OPCODES_count, OP_VALUES_count);
        for (int i = 0; i < nb; i++) { auto e = compilePattern(OPCODES[i], OP_VALUES[i]); if (e) v.push_back(std::move(*e)); }
        int nu = std::min(UNDOC_OPCODES_count, UNDOC_VALUES_count);
        for (int i = 0; i < nu; i++) { auto e = compilePattern(UNDOC_OPCODES[i], UNDOC_VALUES[i]); if (e) v.push_back(std::move(*e)); }
        std::stable_sort(v.begin(), v.end(), [](const Pattern& a, const Pattern& b) {
            if (a.captures.size() != b.captures.size()) return a.captures.size() < b.captures.size();
            return b.pattern.size() < a.pattern.size();
        });
        return v;
    }();
    return pats;
}

std::string hex4Upper(int v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", v & 0xffff);
    return buf;
}
std::string padStart(const std::string& s, size_t len, char c) {
    if (s.size() >= len) return s;
    return std::string(len - s.size(), c) + s;
}
std::string padEnd(const std::string& s, size_t len, char c) {
    if (s.size() >= len) return s;
    return s + std::string(len - s.size(), c);
}

} // anonymous namespace

// ==================================================================
//  Z80Assembler
// ==================================================================
Z80Assembler::Z80Assembler() { reset(); }

void Z80Assembler::reset() {
    symbols.clear();
    equs.clear();
    letMap.clear();
    errors.clear();
    warnings.clear();
    listing.clear();
    origin = 0;
    pc = 0;
    lowest = 0xffff;
    highest = 0;
    runAddress.reset();
    defaultOrigin = 0;
    outputAddress.reset();
    codeEnabled = true;
    listEnabled = true;
    macros.clear();
    expanded.clear();
    localCounter = 0;
    printed.clear();
    currentDir.clear();
    hasCurrentDir = false;
    noHeader = false;
    pendingWrites.clear();
    currentBank = 0xC0;
    assembledBytes.clear();
    assembled.clear();
    fileProvider = nullptr;
}

AsmResult Z80Assembler::assemble(const std::string& source, const AsmOptions& options) {
    startAssembly(source, options);
    setupPass(1); runPass(1);
    setupPass(2); runPass(2);
    return finishAssembly();
}

void Z80Assembler::startAssembly(const std::string& source, const AsmOptions& options) {
    reset();
    defaultOrigin = options.origin & 0xffff;
    fileProvider = options.fileProvider;
    if (options.currentDir.has_value()) { currentDir = *options.currentDir; hasCurrentDir = true; }
    std::vector<RawLine> raw;
    {
        std::string cur;
        for (size_t i = 0; i <= source.size(); i++) {
            if (i == source.size() || source[i] == '\n') { raw.push_back({ cur, "", false }); cur.clear(); }
            else cur += source[i];
        }
        // The loop above adds one trailing element unconditionally: a final "\n"
        // yields a trailing "" element, no trailing "\n" does not add one.
        if (!source.empty() && source.back() != '\n' && !raw.empty()) {
            // last element already the tail (no extra), fine.
        }
    }
    if (fileProvider) raw = inlineReads(raw);
    expanded = expandSource(raw);
    expanded = filterConditionals(expanded);
}

void Z80Assembler::setupPass(int pass) {
    pc = defaultOrigin;
    origin = defaultOrigin;
    lowest = 0xffff;
    highest = 0;
    codeEnabled = true;
    listEnabled = true;
    currentBank = 0xC0;
    if (pass == 2) { listing.clear(); assembledBytes.clear(); }
}

void Z80Assembler::runPass(int pass) {
    for (const auto& item : expanded) assembleLine(item.line, item.lineNumber, pass, item.file);
}

std::vector<AsmRegion> Z80Assembler::computeRegions() {
    struct Key { int bank; int addr; };
    std::vector<Key> keys;
    for (const auto& kv : assembledBytes) {
        size_t colon = kv.first.find(':');
        int bank = std::stoi(kv.first.substr(0, colon));
        int addr = std::stoi(kv.first.substr(colon + 1));
        keys.push_back({ bank, addr });
    }
    std::sort(keys.begin(), keys.end(), [](const Key& x, const Key& y) {
        if (x.bank != y.bank) return x.bank < y.bank;
        return x.addr < y.addr;
    });
    std::vector<AsmRegion> regions;
    for (const auto& k : keys) {
        int lastEnd = -1;
        if (!regions.empty()) lastEnd = regions.back().start + (int)regions.back().bytes.size();
        if (regions.empty() || k.bank != regions.back().bank || k.addr != lastEnd)
            regions.push_back({ k.bank, k.addr, {} });
        auto it = assembledBytes.find(std::to_string(k.bank) + ":" + std::to_string(k.addr));
        regions.back().bytes.push_back(it != assembledBytes.end() ? (uint8_t)it->second : 0);
    }
    return regions;
}

std::string Z80Assembler::regionFileName(const std::string& baseNoExt, const std::string& ext, int count, int bank) {
    int bankDigit = ((bank == 0 ? 0xC0 : bank)) & 7;
    if (bankDigit != 0) {
        char h[4]; std::snprintf(h, sizeof(h), "%X", bankDigit);
        std::string tag = std::string("BC") + h;
        return count == 0 ? baseNoExt + "." + tag : baseNoExt + "." + tag + std::to_string(count);
    }
    std::string e = ext.empty() ? "bin" : ext;
    if (count == 0) return baseNoExt + "." + e;
    return baseNoExt + "." + e.substr(0, std::min<size_t>(2, e.size())) + std::to_string(count);
}

AsmResult Z80Assembler::finishAssembly() {
    int start = (lowest == 0xffff) ? origin : lowest;
    int end = highest;
    Bytes bytes(std::max(0, end - start), 0);
    assembled.clear();
    for (const auto& kv : assembledBytes) {
        size_t colon = kv.first.find(':');
        int addr = std::stoi(kv.first.substr(colon + 1));
        assembled.insert(addr & 0xffff);
        int idx = (addr - start) & 0xffff;
        if (idx >= 0 && idx < (int)bytes.size()) bytes[idx] = (uint8_t)kv.second;
    }
    std::vector<AsmRegion> regions = computeRegions();
    std::vector<AsmRegion> writeRegions = regions;
    if (writeRegions.empty()) writeRegions.push_back({ 0xC0, start, bytes });

    if (fileProvider) {
        static const std::regex reDisk("^[abAB]:");
        static const std::regex reDiskSplit("^([abAB]:)(.*)$", std::regex::icase);
        static const std::regex reBackslash("\\\\");
        for (const auto& w : pendingWrites) {
            try {
                std::string spec = w.spec;
                bool isDisk = std::regex_search(spec, reDisk);
                std::string filePart = spec;
                std::string dirPrefix;
                if (isDisk) {
                    std::smatch dm;
                    if (std::regex_search(spec, dm, reDiskSplit)) { dirPrefix = dm[1].str(); filePart = dm[2].matched ? dm[2].str() : ""; }
                }
                std::string norm = std::regex_replace(filePart, reBackslash, "/");
                std::vector<std::string> parts;
                {
                    std::string cur;
                    for (char c : norm) { if (c == '/') { parts.push_back(cur); cur.clear(); } else cur += c; }
                    parts.push_back(cur);
                }
                std::string fname = parts.empty() ? "" : parts.back();
                if (!parts.empty()) parts.pop_back();
                if (fname.empty()) fname = "output.bin";
                std::string dir;
                for (const auto& p : parts) { if (!p.empty()) { if (!dir.empty()) dir += "/"; dir += p; } }
                size_t dot = fname.rfind('.');
                std::string baseNoExt = (dot != std::string::npos && dot > 0) ? fname.substr(0, dot) : fname;
                std::string ext = (dot != std::string::npos && dot > 0) ? fname.substr(dot + 1) : "bin";
                std::unordered_map<int, int> bankCounts;
                for (const auto& region : writeRegions) {
                    int bank = region.bank ? region.bank : 0xC0;
                    int bankDigit = bank & 7;
                    int count = bankCounts.count(bankDigit) ? bankCounts[bankDigit] : 0;
                    bankCounts[bankDigit] = count + 1;
                    std::string regionName = regionFileName(baseNoExt, ext, count, bank);
                    std::string regionPath = dir.empty() ? regionName : dir + "/" + regionName;
                    Bytes regionBytes = region.bytes;
                    if (isDisk) {
                        if (fileProvider->writeDiskFile) fileProvider->writeDiskFile(dirPrefix + regionPath, regionBytes, region.start, w.noHeader);
                    } else if (fileProvider->writeFile) {
                        fileProvider->writeFile(regionPath, regionBytes);
                    }
                }
            } catch (const std::exception& ex) {
                error(w.lineNumber, std::string("WRITE failed — ") + ex.what(), w.source);
            }
        }
    }

    AsmResult r;
    r.ok = errors.empty();
    r.errors = errors;
    r.warnings = warnings;
    r.listing = listing;
    r.start = start;
    r.end = end;
    r.bytes = bytes;
    r.assembled = assembled;
    for (auto& kv : symbols) r.symbols[kv.first] = kv.second;
    for (auto& kv : equs) r.equates[kv.first] = kv.second;
    r.regions = writeRegions;
    r.runAddress = runAddress;
    r.printed = printed;
    return r;
}

int Z80Assembler::evalExprSafe(const std::string& expr) {
    try { return evalExpr(expr, pc, 0, 2); }
    catch (...) { return 0; }
}

// ---- READ inlining -----------------------------------------------
std::vector<Z80Assembler::RawLine> Z80Assembler::inlineReadsRaw(const std::string& text, std::set<std::string> seen, int depth, const std::string& file) {
    static const std::regex reRead("^READ\\s+(.+)$", std::regex::icase);
    std::vector<RawLine> out;
    // split text on \n
    std::vector<std::string> lines;
    { std::string cur; for (size_t i = 0; i <= text.size(); i++) { if (i == text.size() || text[i] == '\n') { lines.push_back(cur); cur.clear(); } else cur += text[i]; } if (!text.empty() && text.back() != '\n') {} }
    for (const auto& rawLine : lines) {
        std::string clean = cleanLine(rawLine);
        std::smatch m;
        if (!std::regex_search(clean, m, reRead)) { out.push_back({ rawLine, file, !file.empty() }); continue; }
        auto uq = unquoteString(m[1].str());
        std::string fileSpec = uq.has_value() ? *uq : trim(m[1].str());
        std::string key = toUpper((hasCurrentDir ? currentDir : "") + "|" + fileSpec);
        if (seen.count(key)) continue;
        auto nested = readFileText(fileSpec, currentDir);
        if (!nested.has_value()) { error(0, "READ file not found: " + fileSpec, rawLine); continue; }
        std::set<std::string> nextSeen = seen; nextSeen.insert(key);
        std::string prevDir = currentDir; bool prevHas = hasCurrentDir;
        currentDir = dirOfSpec(fileSpec, prevDir); hasCurrentDir = true;
        auto included = inlineReadsRaw(*nested, nextSeen, depth + 1, fileSpec);
        currentDir = prevDir; hasCurrentDir = prevHas;
        for (auto& l : included) out.push_back(l);
    }
    return out;
}

std::vector<Z80Assembler::RawLine> Z80Assembler::inlineReads(const std::vector<RawLine>& rawLines, std::set<std::string> seen, int depth) {
    if (depth > 16) return rawLines;
    static const std::regex reRead("^READ\\s+(.+)$", std::regex::icase);
    std::vector<RawLine> out;
    for (const auto& rawLine : rawLines) {
        std::string lineStr = rawLine.line;
        std::string file = rawLine.hasFile ? rawLine.file : "";
        std::string clean = cleanLine(lineStr);
        std::smatch m;
        if (!std::regex_search(clean, m, reRead)) { out.push_back({ lineStr, file, !file.empty() }); continue; }
        auto uq = unquoteString(m[1].str());
        std::string fileSpec = uq.has_value() ? *uq : trim(m[1].str());
        std::string key = toUpper((hasCurrentDir ? currentDir : "") + "|" + fileSpec);
        if (seen.count(key)) { error(0, "Recursive READ of '" + fileSpec + "'", lineStr); continue; }
        auto text = readFileText(fileSpec, currentDir);
        if (!text.has_value()) { error(0, "READ file not found: " + fileSpec, lineStr); continue; }
        std::set<std::string> nextSeen = seen; nextSeen.insert(key);
        std::string prevDir = currentDir; bool prevHas = hasCurrentDir;
        currentDir = dirOfSpec(fileSpec, prevDir); hasCurrentDir = true;
        auto included = inlineReadsRaw(*text, nextSeen, depth + 1, fileSpec);
        currentDir = prevDir; hasCurrentDir = prevHas;
        for (auto& l : included) out.push_back(l);
    }
    return out;
}

std::optional<std::string> Z80Assembler::readFileText(const std::string& path, const std::string& cwd) {
    if (!fileProvider || !fileProvider->readText) return std::nullopt;
    try { return fileProvider->readText(path, cwd); }
    catch (...) { return std::nullopt; }
}

std::optional<Bytes> Z80Assembler::readFileBinary(const std::string& path, const std::string& cwd) {
    if (!fileProvider || !fileProvider->readBinary) return std::nullopt;
    try { return fileProvider->readBinary(path, cwd); }
    catch (...) { return std::nullopt; }
}

// ---- macro / repeat / while expansion ----------------------------
std::vector<Z80Assembler::ExpandedLine> Z80Assembler::expandSource(const std::vector<RawLine>& rawLines) {
    std::vector<ExpandedLine> out;
    static const std::regex reCR("\r");
    static const std::regex reMacroDef("^(?:([A-Za-z_.][A-Za-z0-9_.]*)\\s+)?MACRO\\b", std::regex::icase);
    static const std::regex reMacroStrip("^([A-Za-z_.][A-Za-z0-9_.]*\\s+)?MACRO\\b", std::regex::icase);
    static const std::regex reIdentLead("^([A-Za-z_.][A-Za-z0-9_.]*)");
    static const std::regex reMendEnd("^(MEND|ENDM)\\b", std::regex::icase);
    static const std::regex reFirstTok("^([A-Za-z_.][A-Za-z0-9_.]*)");
    static const std::regex reRepeat("^REPEAT\\s+(.+)$", std::regex::icase);
    static const std::regex reRend("^REND\\b", std::regex::icase);
    static const std::regex reWhile("^WHILE\\s+(.+)$", std::regex::icase);
    static const std::regex reWend("^WEND\\b", std::regex::icase);
    static const std::regex reLocal("@([A-Za-z0-9_.]+)");

    auto stripCR = [&](const std::string& s) { return std::regex_replace(s, reCR, ""); };
    auto push = [&](const std::string& line, int ln, const std::string& file) { out.push_back({ line, ln, file }); };

    size_t i = 0;
    while (i < rawLines.size()) {
        int lineNumber = (int)i + 1;
        std::string original = stripCR(rawLines[i].line);
        std::string curFile = rawLines[i].hasFile ? rawLines[i].file : "";
        std::string clean = cleanLine(original);
        if (clean.empty()) { push(original, lineNumber, curFile); i += 1; continue; }

        std::smatch macroDef;
        if (std::regex_search(clean, macroDef, reMacroDef)) {
            std::string restAfter = trim(std::regex_replace(clean, reMacroStrip, ""));
            std::string name = macroDef[1].matched ? macroDef[1].str() : "";
            std::vector<std::string> params;
            if (!restAfter.empty()) {
                if (name.empty()) {
                    std::smatch nm;
                    if (std::regex_search(restAfter, nm, reIdentLead)) { name = nm[1].str(); }
                    restAfter = trim(std::regex_replace(restAfter, reIdentLead, ""));
                }
                if (!restAfter.empty()) { for (auto& x : splitCsv(restAfter)) params.push_back(toUpper(trim(x))); }
            }
            if (!name.empty()) {
                std::vector<std::pair<int, std::string>> body;
                i += 1;
                while (i < rawLines.size()) {
                    std::string bl = stripCR(rawLines[i].line);
                    std::string bc = cleanLine(bl);
                    if (std::regex_search(bc, reMendEnd)) { i += 1; break; }
                    body.push_back({ (int)i + 1, bl });
                    i += 1;
                }
                macros[toUpper(name)] = { params, body };
                continue;
            }
            push(original, lineNumber, curFile); i += 1; continue;
        }

        std::smatch firstTok;
        if (std::regex_search(clean, firstTok, reFirstTok) && macros.count(toUpper(firstTok[1].str()))) {
            const Macro& macro = macros[toUpper(firstTok[1].str())];
            std::string rest = trim(clean.substr(firstTok[1].str().size()));
            std::vector<std::string> args;
            if (!rest.empty()) { for (auto& x : splitCsv(rest)) args.push_back(trim(x)); }
            std::vector<std::pair<std::string, std::string>> argMap;
            for (size_t idx = 0; idx < macro.params.size(); idx++) argMap.push_back({ macro.params[idx], idx < args.size() ? args[idx] : "0" });
            int lid = ++localCounter;
            for (const auto& bl : macro.body) {
                std::string sub = bl.second;
                for (const auto& pv : argMap) {
                    try {
                        std::regex re("\\b" + pv.first + "\\b", std::regex::icase);
                        std::string val = pv.second;
                        sub = reReplaceCb(sub, re, [&](const std::smatch&) { return val; });
                    } catch (...) {}
                }
                sub = reReplaceCb(sub, reLocal, [&](const std::smatch& mm) { return "@L" + std::to_string(lid) + "_" + mm[1].str(); });
                push(sub, bl.first, curFile);
            }
            i += 1; continue;
        }

        std::smatch rm;
        if (std::regex_search(clean, rm, reRepeat)) {
            int count = evalExprSafe(rm[1].str());
            std::vector<std::pair<int, std::string>> body;
            i += 1;
            while (i < rawLines.size()) {
                std::string bl = stripCR(rawLines[i].line);
                if (std::regex_search(cleanLine(bl), reRend)) { i += 1; break; }
                body.push_back({ (int)i + 1, bl });
                i += 1;
            }
            int lid = ++localCounter;
            for (int c = 0; c < count; c++) {
                for (const auto& b : body)
                    push(reReplaceCb(b.second, reLocal, [&](const std::smatch& mm) { return "@L" + std::to_string(lid) + "_" + mm[1].str(); }), b.first, curFile);
            }
            continue;
        }

        std::smatch wm;
        if (std::regex_search(clean, wm, reWhile)) {
            std::vector<std::pair<int, std::string>> body;
            i += 1;
            while (i < rawLines.size()) {
                std::string bl = stripCR(rawLines[i].line);
                if (std::regex_search(cleanLine(bl), reWend)) { i += 1; break; }
                body.push_back({ (int)i + 1, bl });
                i += 1;
            }
            int lid = ++localCounter;
            int guard = 0;
            std::string cond = wm[1].str();
            while (evalExprSafe(cond) != 0 && guard < 100000) {
                for (const auto& b : body)
                    push(reReplaceCb(b.second, reLocal, [&](const std::smatch& mm) { return "@L" + std::to_string(lid) + "_" + mm[1].str(); }), b.first, curFile);
                guard += 1;
            }
            continue;
        }

        push(original, lineNumber, curFile);
        i += 1;
    }
    return out;
}

// ---- conditional filtering ---------------------------------------
std::vector<Z80Assembler::ExpandedLine> Z80Assembler::filterConditionals(const std::vector<ExpandedLine>& expandedIn) {
    struct Frame { bool parentActive; bool hadTrue; bool elseSeen; };
    std::vector<ExpandedLine> out;
    std::vector<Frame> stack;
    std::unordered_set<std::string> names;
    std::unordered_map<std::string, int> seen;

    auto rootActive = [&]() -> bool { for (auto& s : stack) if (!s.hadTrue) return false; return true; };

    static const std::regex reLabelPrefix("^([A-Za-z0-9_.][A-Za-z0-9_.]*):\\s*(.*)$");
    static const std::regex reEqu("^([A-Za-z0-9_.][A-Za-z0-9_.]*)\\s+(?:EQU|DEFL|DE\\s+FL|=)\\s+(.+)$", std::regex::icase);
    static const std::regex reLet("^LET\\s+([A-Za-z0-9_.][A-Za-z0-9_.]*)\\s*=\\s*(.+)$", std::regex::icase);
    static const std::regex reBare("^([A-Za-z0-9_.][A-Za-z0-9_.]*)$");

    auto collectDefined = [&](const std::string& l) {
        std::string rest = l;
        std::smatch m;
        while (std::regex_search(rest, m, reLabelPrefix)) { names.insert(toUpper(m[1].str())); rest = trim(m[2].str()); }
        if (rest.empty()) return;
        std::smatch eq;
        if (std::regex_search(rest, eq, reEqu)) {
            names.insert(toUpper(eq[1].str()));
            try { seen[toUpper(eq[1].str())] = evalExpr(eq[2].str(), pc, 0, 2) & 0xffff; } catch (...) {}
            return;
        }
        if (std::regex_search(rest, eq, reLet)) {
            names.insert(toUpper(eq[1].str()));
            try { seen[toUpper(eq[1].str())] = evalExpr(eq[2].str(), pc, 0, 2) & 0xffff; } catch (...) {}
            return;
        }
        std::smatch bare;
        if (std::regex_search(rest, bare, reBare) && !isKnownOpcode(bare[1].str()) && !isDirective(bare[1].str()))
            names.insert(toUpper(bare[1].str()));
    };

    auto evalCondExpr = [&](const std::string& expr) -> int {
        std::vector<EToken> tokens;
        bool ok = true;
        try {
            tokens = tokenizeExpression(expr, [&](const std::string& name) -> std::optional<double> {
                if (name == "$") return (double)pc;
                std::string up = toUpper(name);
                auto it = seen.find(up);
                if (it != seen.end()) return (double)it->second;
                auto v = symbolValue(name);
                if (!v.has_value()) return (double)0;
                return (double)*v;
            });
        } catch (...) { ok = false; }
        if (!ok) return 0;
        try { return toInt32(evalTokensLeftToRight(tokens)) & 0xffff; }
        catch (...) { return 0; }
    };

    static const std::regex reIfdef("^IFDEF\\b", std::regex::icase);
    static const std::regex reIfndef("^IFNDEF\\b", std::regex::icase);
    static const std::regex reIfnot("^IFNOT\\b", std::regex::icase);
    static const std::regex reIf("^IF\\s+", std::regex::icase);
    static const std::regex reElseif("^ELSEIF\\b", std::regex::icase);
    static const std::regex reElse("^ELSE\\b", std::regex::icase);
    static const std::regex reEndif("^ENDIF\\b", std::regex::icase);

    for (const auto& item : expandedIn) {
        std::string l = cleanLine(item.line);
        if (l.empty()) { out.push_back(item); continue; }
        if (reTest(l, reIfdef)) {
            std::string sym = toUpper(trim(std::regex_replace(l, reIfdef, "")));
            bool cond = names.count(sym) != 0;
            stack.push_back({ rootActive(), rootActive() && cond, false });
            continue;
        }
        if (reTest(l, reIfndef)) {
            std::string sym = toUpper(trim(std::regex_replace(l, reIfndef, "")));
            bool cond = names.count(sym) == 0;
            stack.push_back({ rootActive(), rootActive() && cond, false });
            continue;
        }
        if (reTest(l, reIfnot)) {
            bool cond = evalCondExpr(trim(std::regex_replace(l, reIfnot, ""))) == 0;
            stack.push_back({ rootActive(), rootActive() && cond, false });
            continue;
        }
        if (reTest(l, reIf)) {
            bool cond = evalCondExpr(trim(std::regex_replace(l, reIf, ""))) != 0;
            stack.push_back({ rootActive(), rootActive() && cond, false });
            continue;
        }
        if (reTest(l, reElseif)) {
            if (!stack.empty()) {
                Frame& top = stack.back();
                if (!top.elseSeen && !top.hadTrue && top.parentActive)
                    top.hadTrue = evalCondExpr(trim(std::regex_replace(l, reElseif, ""))) != 0;
            }
            continue;
        }
        if (reTest(l, reElse)) {
            if (!stack.empty()) {
                Frame& top = stack.back();
                if (!top.elseSeen && top.parentActive) top.hadTrue = !top.hadTrue;
                top.elseSeen = true;
            }
            continue;
        }
        if (reTest(l, reEndif)) { if (!stack.empty()) stack.pop_back(); continue; }
        if (!rootActive()) { collectDefined(l); continue; }
        collectDefined(l);
        out.push_back(item);
    }
    return out;
}

// ---- per-line assembly -------------------------------------------
void Z80Assembler::assembleLine(const std::string& rawLine, int lineNumber, int pass, const std::string& file) {
    std::string original;
    for (char c : rawLine) if (c != '\r') original += c;
    std::string line = cleanLine(original);
    if (line.empty()) {
        if (pass == 2 && listEnabled) listing.push_back({ lineNumber, pc, {}, original, file });
        return;
    }
    std::vector<std::string> statements = splitStatements(line);
    int startPc = pc;
    for (const auto& stmt : statements) {
        assembleStatement(trim(stmt), lineNumber, pass, original, startPc, file);
        startPc = pc;
    }
}

void Z80Assembler::assembleStatement(std::string line, int lineNumber, int pass, const std::string& original, int startPc, const std::string& file) {
    std::vector<int> emitted;
    auto emit = [&](int value) {
        emitted.push_back(value & 0xff);
        if (pass == 2) assembledBytes[std::to_string(currentBank) + ":" + std::to_string(pc & 0xffff)] = value & 0xff;
        pc = (pc + 1) & 0xffff;
        if (!emitted.empty()) {
            lowest = std::min(lowest, startPc);
            highest = std::max(highest, startPc + (int)emitted.size());
        }
    };
    auto emitWord = [&](int value) { emit(value); emit((int)((uint32_t)value >> 8)); };

    static const std::regex reBareLine("^([A-Za-z0-9_.][A-Za-z0-9_.]*)$");
    static const std::regex reLabelColon("^([A-Za-z0-9_.][A-Za-z0-9_.]*):\\s*(.*)$");
    static const std::regex reEqu("^([A-Za-z0-9_.][A-Za-z0-9_.]*)\\s+(?:EQU|DEFL|=|DE\\s+FL)\\s+(.+)$", std::regex::icase);
    static const std::regex reLet("^LET\\s+([A-Za-z0-9_.][A-Za-z0-9_.]*)\\s*=\\s*(.+)$", std::regex::icase);
    static const std::regex reLoose("^([A-Za-z0-9_.][A-Za-z0-9_.]*)\\s+(\\S.*)$");

    std::smatch m;
    if (std::regex_search(line, m, reBareLine) && !isKnownOpcode(m[1].str()) && !isDirective(m[1].str())) {
        if (pass == 1) defineSymbol(m[1].str(), pc, lineNumber);
        if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
        return;
    }

    while (std::regex_search(line, m, reLabelColon)) {
        if (pass == 1) defineSymbol(m[1].str(), pc, lineNumber);
        line = trim(m[2].str());
        if (line.empty()) {
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
            return;
        }
    }

    if (std::regex_search(line, m, reEqu)) {
        int value = evalExpr(m[2].str(), pc, lineNumber, pass) & 0xffff;
        if (pass == 1) defineSymbol(m[1].str(), value, lineNumber, true);
        if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
        return;
    }
    if (std::regex_search(line, m, reLet)) {
        int value = evalExpr(m[2].str(), pc, lineNumber, pass) & 0xffff;
        if (pass == 1) { letMap.insert(symKey(m[1].str())); defineSymbol(m[1].str(), value, lineNumber, true); }
        if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
        return;
    }

    if (std::regex_search(line, m, reLoose) && !isKnownOpcode(m[1].str()) && !isDirective(m[1].str())) {
        if (pass == 1) defineSymbol(m[1].str(), pc, lineNumber);
        line = trim(m[2].str());
    }

    std::string upper = normalizeInstruction(line);
    try {
        static const std::regex reList("^LIST\\b", std::regex::icase);
        static const std::regex reNolist("^NOLIST\\b", std::regex::icase);
        static const std::regex reCode("^CODE\\b", std::regex::icase);
        static const std::regex reNocode("^NOCODE\\b", std::regex::icase);
        static const std::regex reNoheader("^NOHEADER\\b", std::regex::icase);
        static const std::regex reOrg("^(ORG|AORG)\\s+", std::regex::icase);
        static const std::regex reBank("^BANK\\s+", std::regex::icase);
        static const std::regex reBankVal("^(?:C|0X)?([0-9A-F]+)$");
        static const std::regex reRunEnt("^(RUN|ENT)\\s+", std::regex::icase);
        static const std::regex reEnd("^END\\b", std::regex::icase);
        static const std::regex reStop("^STOP\\b", std::regex::icase);
        static const std::regex rePrint("^PRINT\\s+", std::regex::icase);
        static const std::regex reAlign("^ALIGN\\s+", std::regex::icase);
        static const std::regex reStr("^STR\\s+", std::regex::icase);
        static const std::regex reDb("^(DB|DEFB|DEFM|DM|TEXT|BYTE)\\b", std::regex::icase);
        static const std::regex reDw("^(DW|DEFW|WORD)\\b", std::regex::icase);
        static const std::regex reDs("^(DS|DEFS|RMEM)\\b", std::regex::icase);
        static const std::regex reIncbin("^INCBIN\\b", std::regex::icase);
        static const std::regex reWrite("^WRITE\\b", std::regex::icase);
        static const std::regex reWriteDirect("^DIRECT\\b", std::regex::icase);
        static const std::regex reNoops("^(DEFINE|LIMIT|BANK|READ)\\b", std::regex::icase);
        static const std::regex rePrintDollar("\\$([A-Za-z0-9_.]+)");
        static const std::regex rePrintAmp("&([A-Za-z0-9_.]+)");
        static const std::regex reDbQuote("\"([^\"]*)\"");
        static const std::regex reQuoteTest("\"");

        if (reTest(line, reList)) { listEnabled = true; return; }
        if (reTest(line, reNolist)) { listEnabled = false; return; }
        if (reTest(line, reCode)) { codeEnabled = true; return; }
        if (reTest(line, reNocode)) { codeEnabled = false; return; }
        if (reTest(line, reNoheader)) { noHeader = true; return; }

        if (reTest(line, reOrg)) {
            std::vector<std::string> parts = splitCsv(std::regex_replace(line, reOrg, ""));
            pc = evalExpr(parts[0], pc, lineNumber, pass) & 0xffff;
            if (parts.size() > 1 && !parts[1].empty()) outputAddress = evalExpr(parts[1], pc, lineNumber, pass) & 0xffff;
            if (pass == 1 && lowest == 0xffff) origin = pc;
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, pc, {}, original, file });
            return;
        }
        if (reTest(line, reBank)) {
            std::string raw = toUpper(trim(std::regex_replace(line, reBank, "")));
            std::smatch bm;
            int val = -1;
            if (std::regex_search(raw, bm, reBankVal)) val = (int)std::strtol(bm[1].str().c_str(), nullptr, 16);
            if (val < 0 || val > 7) error(lineNumber, "BANK expects C0, C4, C5, C6 or C7 — got \"" + raw + "\"", original);
            else currentBank = 0xC0 | val;
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
            return;
        }
        if (reTest(line, reRunEnt)) {
            std::string rest = std::regex_replace(line, reRunEnt, "");
            std::string first = rest.substr(0, rest.find(','));
            runAddress = evalExpr(first, pc, lineNumber, pass) & 0xffff;
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
            return;
        }
        if (reTest(line, reEnd)) { return; }
        if (reTest(line, reStop)) { error(lineNumber, "Assembly stopped", original); return; }

        if (reTest(line, rePrint)) {
            std::string rest = trim(std::regex_replace(line, rePrint, ""));
            for (const auto& part : splitCsv(rest)) {
                auto str = unquoteString(part);
                if (str.has_value()) {
                    std::string cooked = reReplaceCb(*str, rePrintDollar, [&](const std::smatch& mm) {
                        auto v = symbolValue(mm[1].str());
                        return !v.has_value() ? std::string("?") : hex4Upper(*v & 0xffff);
                    });
                    cooked = reReplaceCb(cooked, rePrintAmp, [&](const std::smatch& mm) {
                        auto v = symbolValue(mm[1].str());
                        return !v.has_value() ? std::string("?") : std::to_string(*v & 0xffff);
                    });
                    printed.push_back(cooked);
                } else {
                    printed.push_back(std::to_string(evalExpr(part, pc, lineNumber, pass)));
                }
            }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
            return;
        }
        if (reTest(line, reAlign)) {
            std::vector<std::string> parts = splitCsv(trim(std::regex_replace(line, reAlign, "")));
            int boundary = std::max(1, evalExpr(parts.empty() || parts[0].empty() ? "1" : parts[0], pc, lineNumber, pass) & 0xffff);
            int fill = (parts.size() > 1 && !parts[1].empty()) ? (evalExpr(parts[1], pc, lineNumber, pass) & 0xff) : 0;
            if (codeEnabled) while ((pc & (boundary - 1)) != 0) emit(fill);
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reStr)) {
            std::string rest = trim(std::regex_replace(line, reStr, ""));
            if (codeEnabled) {
                for (const auto& part : splitCsv(rest)) {
                    auto str = unquoteString(part);
                    if (str.has_value() && !str->empty()) {
                        for (size_t k = 0; k + 1 < str->size(); k++) emit((unsigned char)(*str)[k]);
                        emit(((unsigned char)(*str)[str->size() - 1] & 0x7f) | 0x80);
                    }
                }
            }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reDb)) {
            std::string rest = trim(std::regex_replace(line, reDb, ""));
            if (codeEnabled) {
                for (const auto& part : splitCsv(rest)) {
                    if (trim(part).empty()) continue;
                    auto str = unquoteString(part);
                    if (str.has_value()) { for (char ch : *str) emit((unsigned char)ch); }
                    else if (reTest(part, reQuoteTest)) {
                        std::string exprStr = reReplaceCb(part, reDbQuote, [&](const std::smatch& mm) {
                            std::string inner = mm[1].str();
                            return std::to_string(inner.size() ? (unsigned char)inner[inner.size() - 1] : 0);
                        });
                        emit(evalExpr(exprStr, pc, lineNumber, pass));
                    } else {
                        emit(evalExpr(part, pc, lineNumber, pass));
                    }
                }
            }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reDw)) {
            std::string rest = trim(std::regex_replace(line, reDw, ""));
            if (codeEnabled) { for (const auto& part : splitCsv(rest)) emitWord(evalExpr(part, pc, lineNumber, pass)); }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reDs)) {
            std::vector<std::string> parts = splitCsv(trim(std::regex_replace(line, reDs, "")));
            if (codeEnabled) {
                for (size_t k = 0; k < parts.size(); k += 2) {
                    int count = evalExpr(parts[k].empty() ? "0" : parts[k], pc, lineNumber, pass);
                    int fill = (k + 1 < parts.size() && !parts[k + 1].empty()) ? evalExpr(parts[k + 1], pc, lineNumber, pass) : 0;
                    for (int j = 0; j < count; j++) emit(fill & 0xff);
                }
            }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reIncbin)) {
            std::string rest = trim(std::regex_replace(line, reIncbin, ""));
            std::vector<std::string> parts = splitCsv(rest);
            auto uq = parts.empty() ? std::optional<std::string>() : unquoteString(parts[0]);
            std::string fileSpec = uq.has_value() ? *uq : (parts.empty() ? "" : trim(parts[0]));
            auto binOpt = readFileBinary(fileSpec, currentDir);
            if (!binOpt.has_value()) {
                if (pass == 2) error(lineNumber, "INCBIN file not found: " + fileSpec, original);
            } else {
                Bytes bin = *binOpt;
                if (bin.size() >= 128 && hasAmsdosHeaderBytes(bin)) bin.erase(bin.begin(), bin.begin() + 128);
                int offset = (parts.size() > 1 && !parts[1].empty()) ? evalExpr(parts[1], pc, lineNumber, pass) : 0;
                int size = (parts.size() > 2 && !parts[2].empty()) ? evalExpr(parts[2], pc, lineNumber, pass) : ((int)bin.size() - offset);
                offset = std::max(0, std::min((int)bin.size(), offset));
                size = std::max(0, std::min((int)bin.size() - offset, size));
                if (codeEnabled) for (int k = offset; k < offset + size; k++) emit(bin[k] & 0xff);
            }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reWrite)) {
            std::string rest = trim(std::regex_replace(line, reWrite, ""));
            bool direct = reTest(rest, reWriteDirect);
            std::string spec = trim(direct ? std::regex_replace(rest, reWriteDirect, "") : rest);
            auto uq = unquoteString(spec);
            std::string unq = uq.has_value() ? *uq : trim(spec);
            if (pass == 2 && fileProvider) {
                if (fileProvider->canWrite && !fileProvider->canWrite(unq)) {
                    std::string drive = unq.substr(0, unq.find(':'));
                    error(lineNumber, "WRITE failed — drive '" + drive + ":' not available", original);
                } else {
                    pendingWrites.push_back({ unq, noHeader, lineNumber, original, file });
                }
            }
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
            return;
        }
        if (reTest(line, reNoops)) {
            if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, {}, original, file });
            return;
        }

        std::vector<int> bytes = encodeInstruction(upper, startPc, lineNumber, pass);
        if (codeEnabled) { for (int b : bytes) emit(b); }
        if (pass == 2 && listEnabled) listing.push_back({ lineNumber, startPc, emitted, original, file });
    } catch (const std::exception& ex) {
        if (pass == 2) error(lineNumber, ex.what(), original);
    }

    if (!emitted.empty()) {
        lowest = std::min(lowest, startPc);
        highest = std::max(highest, startPc + (int)emitted.size());
    }
}

// ---- symbol helpers ----------------------------------------------
std::string Z80Assembler::symKey(const std::string& name) {
    size_t i = 0;
    while (i < name.size() && name[i] == '.') i++;
    return toUpper(name.substr(i));
}

void Z80Assembler::defineSymbol(const std::string& name, int value, int lineNumber, bool equ) {
    std::string key = symKey(name);
    bool have = false; int cur = 0;
    if (symbols.count(key)) { have = true; cur = symbols[key]; }
    else if (equs.count(key)) { have = true; cur = equs[key]; }
    if (have && cur != (value & 0xffff)) {
        if (!letMap.count(key)) { error(lineNumber, "Symbol already defined: " + name); return; }
    }
    if (equ) equs[key] = value & 0xffff; else symbols[key] = value & 0xffff;
}

std::optional<int> Z80Assembler::symbolValue(const std::string& name) {
    std::string key = symKey(name);
    auto s = symbols.find(key);
    if (s != symbols.end()) return s->second;
    auto e = equs.find(key);
    if (e != equs.end()) return e->second;
    return std::nullopt;
}

bool Z80Assembler::isKnownOpcode(const std::string& name) {
    std::string key = toUpper(name);
    if (findCond(key)) return true;
    return opmnemonicSet().count(key) != 0;
}

bool Z80Assembler::isDirective(const std::string& name) {
    return directiveSet().count(toUpper(name)) != 0;
}

// ---- expression evaluator ----------------------------------------
int Z80Assembler::evalExpr(const std::string& expr, int pcArg, int lineNumber, int pass) {
    std::string s = trim(expr);
    auto q = unquoteString(s);
    if (q.has_value() && q->size() == 1) return (unsigned char)(*q)[0];
    std::vector<EToken> tokens = tokenizeExpression(s, [&](const std::string& name) -> std::optional<double> {
        if (name == "$") return (double)pcArg;
        auto v = symbolValue(name);
        if (!v.has_value()) {
            if (pass == 1) return (double)0;
            throw std::runtime_error("Unknown symbol: " + name);
        }
        return (double)*v;
    });
    double value = evalTokensLeftToRight(tokens);
    if (!std::isfinite(value)) throw std::runtime_error("Bad expression: " + expr);
    return (int)toInt32(value);
}

// ---- instruction encoder -----------------------------------------
std::vector<int> Z80Assembler::encodeInstruction(std::string inst, int pcArg, int lineNumber, int pass) {
    static const std::regex reAndA("^AND A,");
    static const std::regex reOrA("^OR A,");
    static const std::regex reXorA("^XOR A,");
    static const std::regex reLdPcHl("^LD PC,HL$");
    static const std::regex reLdPcIx("^LD PC,IX$");
    static const std::regex reLdPcIy("^LD PC,IY$");
    static const std::regex reHX("\\bHX\\b"), reIXH("\\bIXH\\b"), reLX("\\bLX\\b"), reIXL("\\bIXL\\b");
    static const std::regex reHY("\\bHY\\b"), reIYH("\\bIYH\\b"), reLY("\\bLY\\b"), reIYL("\\bIYL\\b");
    static const std::regex reSbcReg("^SBC A,([ABCDEHL])$");
    static const std::regex reSbcHl("^SBC A,\\(HL\\)$");

    inst = normalizeInstruction(inst);
    inst = std::regex_replace(inst, reAndA, "AND ");
    inst = std::regex_replace(inst, reOrA, "OR ");
    inst = std::regex_replace(inst, reXorA, "XOR ");
    inst = std::regex_replace(inst, reLdPcHl, "JP (HL)");
    inst = std::regex_replace(inst, reLdPcIx, "JP (IX)");
    inst = std::regex_replace(inst, reLdPcIy, "JP (IY)");
    inst = std::regex_replace(inst, reHX, "IXH");
    inst = std::regex_replace(inst, reIXH, "IXH");
    inst = std::regex_replace(inst, reLX, "IXL");
    inst = std::regex_replace(inst, reIXL, "IXL");
    inst = std::regex_replace(inst, reHY, "IYH");
    inst = std::regex_replace(inst, reIYH, "IYH");
    inst = std::regex_replace(inst, reLY, "IYL");
    inst = std::regex_replace(inst, reIYL, "IYL");
    inst = std::regex_replace(inst, reSbcReg, "SBC $1");
    inst = std::regex_replace(inst, reSbcHl, "SBC (HL)");

    std::smatch m;
    static const std::regex reJR("^JR(?:\\s+([A-Z]+),)?(.+)$");
    if (std::regex_search(inst, m, reJR)) {
        std::string cond = m[1].matched ? m[1].str() : "";
        std::string target = trim(std::regex_replace(m[2].str(), std::regex("^,"), ""));
        int op;
        if (cond.empty()) op = 0x18;
        else { auto c = findCond(cond); if (!c || c->jr < 0) throw std::runtime_error("Invalid JR condition: " + cond); op = c->jr; }
        int to = evalExpr(target, pcArg, lineNumber, pass);
        int disp = (int8_t)((to - (pcArg + 2)) & 0xff);
        if (pass == 2 && to != (int)((pcArg + 2 + disp) & 0xffff)) throw std::runtime_error("JR target out of range: " + target);
        return { op, disp & 0xff };
    }
    static const std::regex reDJNZ("^DJNZ\\s+(.+)$");
    if (std::regex_search(inst, m, reDJNZ)) {
        int to = evalExpr(m[1].str(), pcArg, lineNumber, pass);
        int disp = (int8_t)((to - (pcArg + 2)) & 0xff);
        if (pass == 2 && to != (int)((pcArg + 2 + disp) & 0xffff)) throw std::runtime_error("DJNZ target out of range: " + m[1].str());
        return { 0x10, disp & 0xff };
    }
    static const std::regex reJP("^JP(?:\\s+([A-Z]+),)?(.+)$");
    static const std::regex reJPparen("^JP\\s*\\(");
    if (std::regex_search(inst, m, reJP) && !reTest(inst, reJPparen)) {
        std::string cond = m[1].matched ? m[1].str() : "";
        int op;
        if (cond.empty()) op = 0xc3;
        else { auto c = findCond(cond); if (!c) throw std::runtime_error("Invalid JP condition: " + cond); op = c->jp; }
        int value = evalExpr(m[2].str(), pcArg, lineNumber, pass);
        return { op, value & 0xff, (int)((value >> 8) & 0xff) };
    }
    static const std::regex reCALL("^CALL(?:\\s+([A-Z]+),)?(.+)$");
    if (std::regex_search(inst, m, reCALL)) {
        std::string cond = m[1].matched ? m[1].str() : "";
        int op;
        if (cond.empty()) op = 0xcd;
        else { auto c = findCond(cond); if (!c) throw std::runtime_error("Invalid CALL condition: " + cond); op = c->call; }
        int value = evalExpr(m[2].str(), pcArg, lineNumber, pass);
        return { op, value & 0xff, (int)((value >> 8) & 0xff) };
    }
    static const std::regex reRST("^RST\\s+(.+)$");
    if (std::regex_search(inst, m, reRST)) {
        std::vector<std::string> parts = splitCsv(m[1].str());
        int value = evalExpr(parts[0], pcArg, lineNumber, pass) & 0x38;
        if (parts.size() > 1 && !parts[1].empty()) {
            int nn = evalExpr(parts[1], pcArg + 1, lineNumber, pass);
            return { 0xc7 | value, nn & 0xff, (int)((nn >> 8) & 0xff) };
        }
        return { 0xc7 | value };
    }
    static const std::regex reRET("^RET\\s+");
    if (reTest(inst, reRET)) {
        std::string cond = trim(std::regex_replace(inst, reRET, ""));
        auto c = findCond(cond);
        if (!c) throw std::runtime_error("Invalid RET condition: " + cond);
        return { c->ret };
    }
    static const std::regex reIM("^IM\\s+([012])$", std::regex::icase);
    if (std::regex_search(inst, m, reIM)) {
        int n = m[1].str()[0] - '0';
        return { 0xed, n == 0 ? 0x46 : n == 1 ? 0x56 : 0x5e };
    }

    // Bare-immediate ADD (Maxam "ADD n" == "ADD A,n").
    static const std::regex reAddComma("^ADD\\s*,");
    static const std::regex reAddRest("^ADD\\s+(.+)$");
    static const std::regex reAddPrefix("^ADD\\s+");
    if (!reTest(inst, reAddComma)) {
        std::smatch am;
        if (std::regex_search(inst, am, reAddRest)) {
            std::string op = toUpper(trim(am[1].str()));
            bool regForm = std::regex_search(op, std::regex("^[ABCDEHL](,|$)")) || std::regex_search(op, std::regex("^\\("))
                || std::regex_search(op, std::regex("^(HL|IX|IY|SP)(,|$)")) || std::regex_search(op, std::regex("^(IXH|IXL|IYH|IYL)$"));
            if (!regForm) inst = std::regex_replace(inst, reAddPrefix, "ADD A,");
        }
    }
    static const std::regex reInC("^IN\\s+\\(C\\)$");
    if (reTest(inst, reInC)) return { 0xED, 0x70 };

    static const std::regex reOutC("^OUT\\s+\\(C\\),(.+)$");
    if (std::regex_search(inst, m, reOutC)) {
        std::string op = toUpper(trim(m[1].str()));
        if (!std::regex_search(op, std::regex("^[ABCDEHL]$"))) {
            evalExpr(m[1].str(), pcArg, lineNumber, pass);
            return { 0xED, 0x71 };
        }
    }

    for (const auto& entry : rawPatterns()) {
        std::smatch match;
        if (!std::regex_match(inst, match, entry.regex)) continue;
        std::vector<std::string> captures;
        for (size_t k = 1; k < match.size(); k++) captures.push_back(trim(match[k].str()));
        return bytesFromTemplate(entry.templ, captures, pcArg, lineNumber, pass);
    }
    throw std::runtime_error("Unknown instruction: " + inst);
}

std::vector<int> Z80Assembler::bytesFromTemplate(const std::string& templ, const std::vector<std::string>& captures, int pcArg, int lineNumber, int pass) {
    std::vector<std::string> parts;
    for (size_t i = 0; i + 1 < templ.size(); i += 2) parts.push_back(templ.substr(i, 2));
    std::vector<int> placeholderIndexes;
    for (size_t i = 0; i < parts.size(); i++) if (parts[i] == "XX") placeholderIndexes.push_back((int)i);
    std::vector<int> bytes;
    for (const auto& p : parts) bytes.push_back(p == "XX" ? 0 : (int)std::strtol(p.c_str(), nullptr, 16));
    if (placeholderIndexes.empty()) return bytes;
    std::vector<int> values;
    for (const auto& c : captures) values.push_back(evalExpr(c, pcArg, lineNumber, pass));
    if (placeholderIndexes.size() == 2 && values.size() == 1) {
        int v = values[0];
        bytes[placeholderIndexes[0]] = v & 0xff;
        bytes[placeholderIndexes[1]] = (int)((uint32_t)v >> 8) & 0xff;
        return bytes;
    }
    if (placeholderIndexes.size() == values.size()) {
        for (size_t i = 0; i < values.size(); i++) bytes[placeholderIndexes[i]] = values[i] & 0xff;
        return bytes;
    }
    if (placeholderIndexes.size() > values.size()) {
        int vi = 0;
        for (int pi = 0; pi < (int)placeholderIndexes.size(); pi++) {
            int idx = placeholderIndexes[pi];
            int v = values.empty() ? 0 : values[std::min(vi, (int)values.size() - 1)];
            bytes[idx] = v & 0xff;
            if (pi + 1 < (int)placeholderIndexes.size() && (int)values.size() <= vi + 1) {
                bytes[placeholderIndexes[++pi]] = (int)((uint32_t)v >> 8) & 0xff;
            }
            vi += 1;
        }
        return bytes;
    }
    return bytes;
}

void Z80Assembler::error(int lineNumber, const std::string& message, const std::string& source) {
    errors.push_back({ lineNumber, message, source });
}

// ==================================================================
//  Free helpers
// ==================================================================
std::string formatAssemblerListing(const AsmResult& result) {
    std::vector<std::string> lines;
    for (const auto& item : result.listing) {
        std::string addr = item.bytes.size() ? padStart(hex4Upper(item.address), 4, '0') : "    ";
        std::string hex;
        for (size_t i = 0; i < item.bytes.size(); i++) {
            if (i) hex += " ";
            char b[4]; std::snprintf(b, sizeof(b), "%02X", item.bytes[i] & 0xff); hex += b;
        }
        hex = padEnd(hex, 18, ' ');
        lines.push_back(addr + "  " + hex + "  " + item.source);
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) { if (i) out += "\n"; out += lines[i]; }
    return out;
}

std::string hexDump(const Bytes& bytes, int start) {
    std::vector<std::string> out;
    for (size_t i = 0; i < bytes.size(); i += 16) {
        std::string line = padStart(hex4Upper(start + (int)i), 4, '0') + ": ";
        for (size_t j = i; j < i + 16 && j < bytes.size(); j++) {
            if (j > i) line += " ";
            char b[4]; std::snprintf(b, sizeof(b), "%02X", bytes[j] & 0xff); line += b;
        }
        out.push_back(line);
    }
    std::string res;
    for (size_t i = 0; i < out.size(); i++) { if (i) res += "\n"; res += out[i]; }
    return res;
}

AmsdosName sanitizeAmsdosFilename(const std::string& name) {
    std::string raw = toUpper(trim(name.empty() ? "CPCSE.BIN" : name));
    std::vector<std::string> parts;
    { std::string cur; for (char c : raw) { if (c == '.') { parts.push_back(cur); cur.clear(); } else cur += c; } parts.push_back(cur); }
    static const std::regex reBad("[^A-Z0-9_$!#&+\\-@^{}~]");
    auto clean = [&](const std::string& value, const std::string& fallback) {
        return std::regex_replace(value.empty() ? fallback : value, reBad, "_");
    };
    std::string b = clean(parts.empty() ? "" : parts[0], "CPCSE");
    b = b.substr(0, std::min<size_t>(8, b.size()));
    std::string base = padEnd(b, 8, ' ');
    std::string extIn = parts.size() > 1 ? parts.back() : "BIN";
    std::string e = clean(extIn, "BIN");
    e = e.substr(0, std::min<size_t>(3, e.size()));
    std::string ext = padEnd(e, 3, ' ');
    std::string bt = trim(base); if (bt.empty()) bt = "CPCSE";
    std::string et = trim(ext); if (et.empty()) et = "BIN";
    return { base, ext, bt + "." + et };
}

Bytes createAmsdosHeader(const AmsdosHeaderOptions& options) {
    Bytes header(128, 0);
    AmsdosName nm = sanitizeAmsdosFilename(options.filename);
    int entryAddress = options.entryAddress.has_value() ? *options.entryAddress : options.loadAddress;
    int length = options.length;
    int loadAddress = options.loadAddress;
    int type = options.type;
    header[0] = 0;
    for (int i = 0; i < 8; i++) header[1 + i] = (uint8_t)(nm.base[i] & 0xff);
    for (int i = 0; i < 3; i++) header[9 + i] = (uint8_t)(nm.ext[i] & 0xff);
    header[16] = 0;
    header[17] = 0;
    header[18] = (uint8_t)(type & 0xff);
    header[19] = (uint8_t)(length & 0xff); header[20] = (uint8_t)((length >> 8) & 0xff);
    header[21] = (uint8_t)(loadAddress & 0xff); header[22] = (uint8_t)((loadAddress >> 8) & 0xff);
    header[23] = 0xff;
    header[24] = (uint8_t)(length & 0xff); header[25] = (uint8_t)((length >> 8) & 0xff);
    header[26] = (uint8_t)(entryAddress & 0xff); header[27] = (uint8_t)((entryAddress >> 8) & 0xff);
    header[64] = (uint8_t)(length & 0xff); header[65] = (uint8_t)((length >> 8) & 0xff); header[66] = (uint8_t)((length >> 16) & 0xff);
    int sum = 0;
    for (int i = 0; i <= 66; i++) sum = (sum + header[i]) & 0xffff;
    header[67] = (uint8_t)(sum & 0xff); header[68] = (uint8_t)((sum >> 8) & 0xff);
    return header;
}

Bytes addAmsdosHeader(const Bytes& bytes, const AmsdosHeaderOptions& options) {
    AmsdosHeaderOptions opt = options;
    opt.length = (int)bytes.size();
    Bytes header = createAmsdosHeader(opt);
    Bytes out;
    out.reserve(header.size() + bytes.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

} // namespace cpcse
