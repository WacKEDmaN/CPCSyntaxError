// CPCSyntaxError: the expansion connector's pins and the bus's devices. See expansion_bus.h.
#include "expansion_bus.h"

namespace cpcse {

const std::array<ExpansionPin, EXPANSION_PIN_COUNT>& expansionPins() {
    static const std::array<ExpansionPin, EXPANSION_PIN_COUNT> pins = {{
        { 1, "SOUND", false, "The AY's mixed sound output" },
        { 2, "GND", false, "Ground" },
        { 3, "A15", false, "Address bus (the last I/O cycle's)" },
        { 4, "A14", false, "Address bus (the last I/O cycle's)" },
        { 5, "A13", false, "Address bus (the last I/O cycle's)" },
        { 6, "A12", false, "Address bus (the last I/O cycle's)" },
        { 7, "A11", false, "Address bus (the last I/O cycle's)" },
        { 8, "A10", false, "Address bus (the last I/O cycle's)" },
        { 9, "A9", false, "Address bus (the last I/O cycle's)" },
        { 10, "A8", false, "Address bus (the last I/O cycle's)" },
        { 11, "A7", false, "Address bus (the last I/O cycle's)" },
        { 12, "A6", false, "Address bus (the last I/O cycle's)" },
        { 13, "A5", false, "Address bus (the last I/O cycle's)" },
        { 14, "A4", false, "Address bus (the last I/O cycle's)" },
        { 15, "A3", false, "Address bus (the last I/O cycle's)" },
        { 16, "A2", false, "Address bus (the last I/O cycle's)" },
        { 17, "A1", false, "Address bus (the last I/O cycle's)" },
        { 18, "A0", false, "Address bus (the last I/O cycle's)" },
        { 19, "D7", false, "Data bus (the last I/O cycle's)" },
        { 20, "D6", false, "Data bus (the last I/O cycle's)" },
        { 21, "D5", false, "Data bus (the last I/O cycle's)" },
        { 22, "D4", false, "Data bus (the last I/O cycle's)" },
        { 23, "D3", false, "Data bus (the last I/O cycle's)" },
        { 24, "D2", false, "Data bus (the last I/O cycle's)" },
        { 25, "D1", false, "Data bus (the last I/O cycle's)" },
        { 26, "D0", false, "Data bus (the last I/O cycle's)" },
        { 27, "+5V", false, "Power" },
        { 28, "MREQ", true, "Z80 memory request: every memory cycle" },
        { 29, "M1", true, "Z80 machine cycle 1: every opcode fetch and interrupt acknowledge" },
        { 30, "RFSH", true, "Z80 refresh: after every opcode fetch" },
        { 31, "IORQ", true, "Z80 I/O request: an IN, an OUT, or an interrupt acknowledge" },
        { 32, "RD", true, "Z80 read" },
        { 33, "WR", true, "Z80 write (memory or I/O)" },
        { 34, "HALT", true, "The Z80 is in a HALT" },
        { 35, "INT", true, "Maskable interrupt request: the Gate Array, or a board (GFX9000, OPL4, PlayCity, Albireo UART)" },
        { 36, "NMI", true, "Non-maskable interrupt: the Multiface STOP button, PlayCity CTC channel 1, the Albireo UART" },
        { 37, "BUSRQ", true, "Bus request (no emulated board asks for the bus)" },
        { 38, "BUSAK", true, "Bus acknowledge" },
        { 39, "READY", true, "Wait: the Gate Array holds the Z80 to its microsecond grid" },
        { 40, "BUS RESET", true, "The reset, from the CPC to the boards" },
        { 41, "RESET", true, "The reset" },
        { 42, "ROMEN", true, "The Gate Array reads a ROM" },
        { 43, "ROMDIS", false, "A board takes a ROM read (an expansion ROM, the Multiface, a cartridge on a classic CPC)" },
        { 44, "RAMRD", true, "The Gate Array reads RAM (the screen, twice a microsecond)" },
        { 45, "RAMDIS", false, "A board takes a RAM access (the Multiface's own RAM)" },
        { 46, "CURSOR", false, "The CRTC's CURSOR output" },
        { 47, "LPEN", false, "Light pen input: a strobe latches R16/R17" },
        { 48, "EXP", true, "Expansion present: read by the PPI, port B bit 5" },
        { 49, "GND", false, "Ground" },
        { 50, "CLOCK", false, "The 4 MHz clock" },
    }};
    return pins;
}

const char* busDeviceName(BusDevice d) {
    switch (d) {
        case BusDevice::GateArray: return "Gate Array";
        case BusDevice::Crtc: return "CRTC";
        case BusDevice::RomSelect: return "ROM select";
        case BusDevice::Ppi: return "PPI";
        case BusDevice::FdcMotor: return "FDC motor";
        case BusDevice::Fdc: return "FDC";
        case BusDevice::Printer: return "Printer port";
        case BusDevice::M4: return "M4 Board";
        case BusDevice::Multiface: return "Multiface II";
        case BusDevice::Rs232: return "RS232C";
        case BusDevice::Opl4: return "OPL4";
        case BusDevice::PlayCity: return "PlayCity";
        case BusDevice::Speech: return "Speech";
        case BusDevice::Sf2Mouse: return "SYMBiFACE II mouse";
        case BusDevice::Sf2Rtc: return "SYMBiFACE II clock";
        case BusDevice::Sf3: return "SYMBiFACE III";
        case BusDevice::Ide: return "IDE";
        case BusDevice::Albireo1: return "Albireo 1";
        case BusDevice::Albireo2: return "Albireo 2";
        case BusDevice::AlbireoUart: return "Albireo UART";
        case BusDevice::Hd20: return "HD20";
        case BusDevice::MouseInterface: return "Mouse interface";
        case BusDevice::Gfx9000: return "GFX9000";
        case BusDevice::AmdrumDac: return "Amdrum";
        case BusDevice::Count: break;
    }
    return "?";
}

} // namespace cpcse
