// CPCSyntaxError — the Multiface II. See multiface.h.
#include "multiface.h"
#include "memory.h"
#include "z80.h"

#include <cstdio>
#include <cstdlib>

namespace cpcse {

bool Multiface2::setRom(const Bytes& image) {
    if (image.size() != 0x2000) return false;
    rom = image;
    return true;
}

void Multiface2::page(bool in) {
    static const bool trace = std::getenv("CPCSE_TRACE_MF2") != nullptr;
    if (trace && in != pagedIn) std::fprintf(stderr, "MF2 page %s at pc %04x\n", in ? "in" : "out", cpu ? cpu->instructionStartPc & 0xffff : 0);
    pagedIn = in;
    if (!memory) return;
    memory->overlayLow = in ? rom.data() : nullptr;
    memory->overlayRam = in ? ram.data() : nullptr;
}

void Multiface2::setEnabled(bool on) {
    if (on && rom.size() != 0x2000) on = false;
    enabled = on;
    reset();
}

void Multiface2::reset() {
    page(false);
    visible = true;
    stopPressed = false;
    calls0065 = 0;
}

void Multiface2::stop() {
    // A press while it is already stopped does nothing (MAME: "pressing stop button
    // while multiface is running has no effect").
    if (!enabled || stopPressed || !cpu) return;
    stopPressed = true;
    calls0065 = 0;
    cpu->requestNmi();               // paged in as the Z80 fetches &0066 (opcodeFetch)
}

void Multiface2::opcodeFetch(int address) {
    if (!enabled) return;
    if (address == 0x0066 && stopPressed && !pagedIn && calls0065 == 0) page(true);
    static const bool trace = std::getenv("CPCSE_TRACE_MF2") != nullptr;
    if (trace && address == 0x0065 && pagedIn) std::fprintf(stderr, "MF2 fetch &0065 (stopped %d, calls %d)\n", stopPressed, calls0065);
    if (address == 0x0065 && pagedIn && stopPressed) {
        calls0065 += 1;
        if (calls0065 == 1) visible = true;          // into the menu (&0164)
        else { visible = false; stopPressed = false; calls0065 = 0; }   // back to the program (&0C9B)
    }
}

bool Multiface2::ioWrite(int port, int value) {
    if (!enabled) return false;
    port &= 0xffff;
    value &= 0xff;
    if (port == 0xfee8) { if (visible) page(true); return true; }
    if (port == 0xfeea) { page(false); return true; }
    // What it remembers, the high byte decoded in full (MAME) -- but not while it is paged in.
    // Its ROM selects AMSDOS itself (OUT &DF07 at &011D, &0138, &02F1) and sets its own
    // mode, inks and CRTC for its menu, yet restores the program's upper ROM straight from
    // &3AAC (&0BCF) and nothing of its own ever writes there: kept while it runs, those
    // writes would bring every save and every load back with AMSDOS selected. (MAME keeps
    // them always, and its loads are not modelled.)
    if (pagedIn) return false;
    switch (port >> 8) {
        case 0x7f:
            switch (value & 0xc0) {
                case 0x00: ram[0x1fcf] = (uint8_t)value; break;                       // pen
                case 0x40:                                                         // its colour
                    // The whole byte: the ROM masks it itself (AND &BF at &0224). The border's
                    // goes into &1FD0-&1FDF, the 16 bytes its ROM searches back from &3FDF
                    // for an ink command and then clears (&01FC-&0219) -- MAME's &1FDF + pen
                    // would leave that range for any border pen but &10.
                    if (ram[0x1fcf] & 0x10) ram[0x1fd0 + (ram[0x1fcf] & 0x0f)] = (uint8_t)value;
                    else ram[0x1f90 + (ram[0x1fcf] & 0x0f)] = (uint8_t)value;
                    break;
                case 0x80: ram[0x1fef] = (uint8_t)value; break;                       // mode, ROMs
                case 0xc0: ram[0x1fff] = (uint8_t)value; break;                       // RAM
            }
            break;
        case 0xbc: ram[0x1cff] = (uint8_t)value; break;
        case 0xbd: ram[0x1db0 + (ram[0x1cff] & 0x0f)] = (uint8_t)value; break;
        case 0xf7: ram[0x17ff] = (uint8_t)value; break;
        case 0xdf: ram[0x1aac] = (uint8_t)value; break;
        default: break;
    }
    return false;
}

} // namespace cpcse
