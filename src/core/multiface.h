// CPCSyntaxError — Romantic Robot's Multiface II.
//
// What the CPC sees (MAME's mface2.cpp, and the card's own ROM read where MAME leaves it
// open):
//   - an 8K ROM at &0000-&1FFF and 8K of RAM at &2000-&3FFF, over the CPC's own memory
//     (ROMDIS / RAMDIS) while the card is paged in;
//   - the STOP button: the card pages itself in and pulses /NMI, so the Z80's NMI at
//     &0066 runs the card's ROM (its handler saves SP at &3EFE and the registers);
//   - OUT &FEE8 pages it in, OUT &FEEA out -- the first only while the card is visible;
//   - it hides itself: its ROM CALLs &0065 (a lone RET) on the way in (&0164) and on the
//     way back (&0C9B), and the card watches that opcode fetch. The first after a STOP
//     makes it visible -- the menu pages itself in and out -- the second hides it and arms
//     the button again, so a program cannot find the card by paging it in. The way back
//     loads BC with &FEEA before that second call and pages out from its RAM afterwards,
//     so paging out works while the card is hidden. After a reset it is visible.
//   - the registers a program cannot read back -- the Gate Array's pen, inks, mode and
//     ROM configuration, its RAM configuration, the CRTC's, the PPI's control and the
//     upper ROM number -- copied into its RAM as they are written, where its ROM finds
//     them to restore the machine (MAME's offsets: &1FCF pen, &1F90+ inks, &1FDF border,
//     &1FEF mode/ROMs, &1FFF RAM, &1CFF CRTC select, &1DB0+ CRTC registers, &17FF PPI,
//     &1AAC ROM select).
#pragma once
#include "common.h"

namespace cpcse {

class GXMemory;
class Z80;

class Multiface2 {
public:
    bool enabled = false;
    Bytes rom;                       // 8K (not shipped: Romantic Robot's)
    std::array<uint8_t, 0x2000> ram{};
    bool pagedIn = false, visible = true, stopPressed = false;
    int calls0065 = 0;               // since the last STOP

    void attach(GXMemory* m, Z80* c) { memory = m; cpu = c; }
    bool setRom(const Bytes& image); // false: not an 8K ROM
    void setEnabled(bool on);
    void reset();                    // the CPC's reset: visible, paged out, button armed
    void stop();                     // the STOP button
    // Every I/O write: true when it was the card's own port (nothing else takes it).
    bool ioWrite(int port, int value);
    // An opcode fetch below &0080 (Z80::onLowM1).
    void opcodeFetch(int address);

private:
    GXMemory* memory = nullptr;
    Z80* cpu = nullptr;
    void page(bool in);
};

} // namespace cpcse
