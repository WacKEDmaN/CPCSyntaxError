// CPCSyntaxError — shared Amstrad ASIC CRTC profile (CRTC 3 and CRTC 4).
// The 40489 (CPC+/GX4000, CRTC 3) and the 40226 (cost-reduced CPC, CRTC 4) share
// almost all of their CRTC behaviour; the ACCC calls them out together as "CRTC's
// 3 and 4" nearly everywhere. What is shared lives here; where the compendium
// names one chip and not the other, crtc_type_4.cpp overrides it.
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
#pragma once
#include "crtc.h"
#include <cstdlib>

namespace cpcse {

// ACCC §4.3 (p.22): R8 is "c c d d - - i i" on the ASIC as on the HD6845S.
inline const int ASIC_WRITE_MASK[16] = { 0xff, 0xff, 0xff, 0xff, 0x7f, 0x1f, 0x7f, 0x7f,
    0xf3, 0x1f, 0x7f, 0x1f, 0x3f, 0xff, 0x3f, 0xff };

// ASIC CRTC status registers, ACCC §21.3.4 (p.249-250). R10 = STATUS 1, R11 = STATUS 2;
// each bit reflects a live counter event. Present on both CRTC 3 and 4.
//
// The tables give every bit a "Bit Value" -- the value it takes WHILE ITS EVENT HOLDS --
// and most of them are active low: STATUS 1's C0=R0/2, C0=R1-1, C0=R2, C0=R2+R3 and
// VMA-overrun bits read 0 on their character and 1 on every other, as do STATUS 2's
// three "last char" bits and its C9=R9. (Setting every bit to 1 on its event, as this
// used to, read that column as a list of bit positions. The 1.10 edition printed the
// same values.)
inline int statusBit(int bit, bool event, int valueOnEvent) {
    return ((event ? valueOnEvent : valueOnEvent ^ 1) & 1) << bit;
}
inline int asicStatus1(CRTC6845& c) {
    int c0 = c.horizontal, r0 = c.registers[0];
    int hsync = (c.registers[3] & 0x0f) ? (c.registers[3] & 0x0f) : 16;
    int s = 0x40;                                                                 // bit6: always 1
    s |= statusBit(0, c0 == r0, 1);                                               // C0=R0
    s |= statusBit(1, c0 == (r0 >> 1), 0);                                        // C0=R0/2
    s |= statusBit(2, r0 >= c.registers[1] && c0 == ((c.registers[1] - 1) & 0xff), 0);  // C0=R1-1 (if R0>=R1)
    s |= statusBit(3, c0 == c.registers[2], 0);                                   // C0=R2
    s |= statusBit(4, c0 == ((c.registers[2] + hsync) & 0xff), 0);                // C0=R2+R3
    // Bit 5 has two rows with opposite values: "R3h>0 : C0=0..R0 on the line R3h from
    // Vsync (C4=R7)" reads 0, "R3h=0 : C0=0..R0 over 15 lines from Vsync" reads 1. It spans
    // whole lines, so C0 does not enter it -- only which VSYNC line we are on.
    int r3h = c.registers[3] >> 4 & 0x0f;
    if (r3h > 0) s |= statusBit(5, c.vsync && c.vsyncCounter == r3h - 1, 0);
    else s |= statusBit(5, c.vsync && c.vsyncCounter < 15, 1);
    // Bit 7, 0 on both rows: "on the next CRTC character, the less significant byte of the
    // video pointer will be reset to 0 (either from an overrun on the current VMA pointer,
    // or when this pointer is going to be reloaded from VMA' at the end of the line)".
    const bool lsbToZero = c0 == r0 ? (c.nextRowAddress & 0xff) == 0 : ((c.maRow + 1) & 0xff) == 0;
    s |= statusBit(7, lsbToZero, 0);
    return s & 0xff;
}
inline int asicStatus2(CRTC6845& c) {
    int c0 = c.horizontal, r0 = c.registers[0], c4 = c.vertical;
    bool c9r9 = c.raster == c.maximumRaster();       // C9=R9
    bool c0r0 = c0 == r0;                            // C0=R0
    int s = 0;                                       // bit6: always 0
    // Bit 4, "Always 1" in §21.3.4.2's table, is the comparator that ends the additional
    // lines: on these chips "there is no specific C5 counter and C9 is used for comparison
    // with R5" (§11.1), and the management "just set's C9 to 0 and compare's C9 with R5 to
    // deactivate" it (§11.2.6). With R5=0 it always matches (the table's 1). With R5>0 it
    // matches for one usec, on C0=R0 of the last additional line (C9+1=R5): §21.3.4, "several
    // bits only change state for 1 usec". MEASURED: SHAKER DU/G27 (R4=37 R5=8, bit 4 at every
    // line and C0) is red (0) on the real CRTC 3 and 4 and on AmSpiriT-lite, G22 (R5=0)
    // yellow; DU/B2 waits for "last char of screen", then counts lines while bit 4 is 0 and
    // finds R5=8 on the real CRTC 4 (ours, with the bit always 1, found 0 for R4-R7).
    const int r5 = c.registers[5] & 0x1f;
    if (r5 == 0 || (c.inAdditionalLines && c0 == r0 && ((c.raster + 1) & 0x1f) == r5)) s |= 0x10;
    // Bit 0, "Last char of screen": the end of row R4, not inside the additional lines that
    // may follow it. There C4 stays R4 (§11.2.6) and C9 counts from 0 again, so C4=R4, C9=R9
    // and C0=R0 hold once more on the last additional line, but the real ASIC keeps the bit 1.
    // MEASURED on SHAKER DU/D (real CRTC 3 and 4, "STATUS 2 BIT 0 : OK"): with R4=37 and R5=8
    // it samples every usec of the frame and wants 0 on #4BFF only (end of row R4) and 1 on the
    // 512 usec after it; ours read 0 again on #4DFF. DU/B2 counts R5 from the same 0.
    s |= statusBit(0, c4 == (c.registers[4] & 0x7f) && c9r9 && c0r0 && !c.inAdditionalLines, 0);
    s |= statusBit(1, c4 == ((c.registers[6] - 1) & 0x7f) && c9r9 && c0r0, 0);    // last char displayed
    s |= statusBit(2, c4 == ((c.registers[7] - 1) & 0x7f) && c9r9 && c0r0, 0);    // last char before Vsync
    if ((c.frame >> 4) & 1) s |= 0x08;               // bit3: timer, toggles every 16 frames
    s |= statusBit(5, c9r9, 0);                      // C9=R9 : C0=0 to R0
    s |= statusBit(7, (c9r9 && c0r0) || (c.raster == 0 && c0 < r0), 1);
    return s & 0xff;
}

struct CrtcType3 : CrtcBehaviour {
    CrtcType3() { id = 3; name = "ASIC 40489"; }
    // ACCC §12.5 (p.102): the "Rom Select" R4 exception below is the 40489's alone —
    // "Note that the CRTC 4 ASIC is not affected by this exception."
    virtual bool hasRomSelectR4Exception() const { return true; }
    int writeMask(int reg) const override { return (reg >= 0 && reg < 16) ? ASIC_WRITE_MASK[reg] : 0; }
    int normaliseRegister(int reg, int value) const override { return value & writeMask(reg); }
    int read(CRTC6845& crtc, int /*operation*/) const override {
        // ACCC §21.2.3: "For CRTC's 3 and 4, only the 3 least significant bits of the
        // selected register number are considered to read a register" — so reading
        // R4 also reads R12 (8+4) and R20 (16+4). The table is:
        //   0 R16 lightpen high   1 R17 lightpen low   2 R10 status 1
        //   3 R11 status 2        4 R12 start high     5 R13 start low
        //   6 R14 cursor high     7 R15 cursor low
        // §21.3.1 adds that the status port is a mirror of this read port on these
        // chips ("d : Mirror of BF00 port"), which is why `operation` is ignored.
        switch (crtc.selected & 7) {
            case 0: return crtc.registers[16] & 0x3f;   // bits 7,6 read as 0
            case 1: return crtc.registers[17] & 0xff;
            case 2: return asicStatus1(crtc);
            case 3: return asicStatus2(crtc);
            case 4: return crtc.registers[12] & 0x3f;
            case 5: return crtc.registers[13] & 0xff;
            case 6: return crtc.registers[14] & 0x3f;   // bits 7,6 read as 0
            default: return crtc.registers[15] & 0xff;
        }
    }
    int hsyncWidth(CRTC6845& crtc) const override { return (crtc.registers[3] & 0x0f) ? (crtc.registers[3] & 0x0f) : 16; }
    int vsyncWidth(CRTC6845& crtc) const override { return (crtc.registers[3] >> 4 & 0x0f) ? (crtc.registers[3] >> 4 & 0x0f) : 16; }
    int displaySkew(CRTC6845& crtc) const override { return crtc.registers[8] >> 4 & 3; }
    bool honoursDisplaySkewField() const override { return true; }   // ACCC 19.1
    // ACCC §19.2 (p.194): "These functions are only available on CRTC's 0, 3 AND 4.
    // They allow for the activation of the BORDER or to generate a delay on the BORDER
    // R1 [conditions]" — the 1/2 µsec delay moves the border-off to C0=skew and the
    // border-on to C0=R1+skew, on this chip as on the HD6845S.
    // NOTE: an earlier, differently-shaped implementation of this measured as a
    // regression on Shaker A2 for CRTC 3/4 and was scoped out to real 6845s. It is
    // reinstated here because §19.2 names these chips explicitly; if the Shaker gate
    // disagrees again, the disagreement is with the shape below, not with the chapter.
    int dispenSkew(CRTC6845& crtc) const override { return crtc.registers[8] >> 4 & 3; }
    // §19.2.4: no BORDER ON when R1>R0, so the skew has no border to hold at a line start.
    // SHAKER A2/A1 (R0=3, "DISPTMG SKEW VERSUS BORDER"): the real CRTC 3 shows the skewed
    // zones exactly like the unskewed one; ours cut a `skew`-wide border into every rupture.
    bool skewBordersEveryLineStart() const override { return false; }
    // §19.2: "These functions are only available on CRTC's 0, 3 and 4... generate a delay on
    // the BORDER R1 management". That is one function on all three, so the same delay line as
    // CRTC 0 (§19.2.3's "acknowledgment counter from C0"). The per-edge shift it replaces
    // lost the line's border-off when R8 went back to 0 on C0=0 or 1 (the deferred
    // "C0==skew" test then never matched): SHAKER A2/C, "OUT R8,#10/OUT R8,0 FROM C0=#39",
    // was a whole BORDER line on CRTC 3/4 where the real CRTC 3 and AmSpiriT-lite show it.
    // §19.2.4's R1>R0 rule stays this chip's own (replacesBorderR1WithR0 false).
    bool skewIsDelayLine() const override { return true; }
    // §19.2.5's "taken away immediately" is CRTC 0's. Here both R8 changes are seen from the
    // next character. MEASURED on SHAKER A2/C (OUT R8,#10 then OUT R8,0 4 usec later, from
    // C0=#37 to #3E): the real CRTC 3 and AmSpiriT-lite show C0=63 displayed on rows #38-#3B
    // and the C0=0 BORDER on rows #39-#3C. Taking the removal at once gave 3 rows from #39;
    // taking both at once gave 4 rows from #39; an R8 write moved to the 3rd usec, 4 from #3A.
    bool skewRemovedActsNow() const override { return false; }
    bool initialVerticalDisplay(CRTC6845& crtc) const override { return crtc.registers[6] != 0; }
    // ACCC §12.5 (p.102): "If R4 is updated with a value less than C4, then there is
    // overflow of the C4 counter. (unlike what happens with C9/R9)" — C4 keeps
    // counting past R4 and only matches after wrapping, so this is a plain equality.
    // The CRTC 3 exception below is the one case where R4 is compared against 0.
    bool verticalTotalMatches(CRTC6845& crtc) const override {
        if (crtc.verticalMatchForced) return true;
        return crtc.vertical == (crtc.registers[4] & 0x7f);
    }
    bool rasterMatches(CRTC6845& crtc) const override { return crtc.raster >= crtc.maximumRaster(); }
    void onRegisterWritten(CRTC6845& crtc, int reg) const override {
        // ASIC forces a raster match when R9 is written at/after the current line.
        if (reg == 9) crtc.rasterMatchForced = crtc.raster >= crtc.maximumRaster();
        if (reg != 4) return;
        // ACCC §12.5: "The modification of register 4 is considered immediately at the
        // end of the line." R4 written with the value of C4 ends the frame here.
        if (crtc.vertical == (crtc.registers[4] & 0x7f)) { crtc.verticalMatchForced = true; return; }
        // ...and a later R4 that differs takes that back: it is the value at the end of the
        // line that counts. SHAKER AU/A test 8 on the real CRTC 3 and 4 ("UPD R4=0 WHEN C4=1
        // & C9=7", the writes landing on C0=0..6 of the new frame's first line, then R4=36 on
        // C0=42): the frame runs on (VSYNC at C4=28 read as 01). The kept match ended it
        // after row 0, a row early: 00.
        crtc.verticalMatchForced = false;
        // ACCC §12.5, the CRTC 3 exception: "When R4 is set to 0 on C0=0 while C4
        // should have gone to 0, and in the case where the I/O is active on the CRTC
        // in parallel with 'Rom Select', the CRTC updates R4 before C4 goes to 0. It
        // then compares the value of C4 with 0 and can overflow if R4 was greater than
        // 0. 'Rom Select' is active during the I/O if bit 5 of the Z80A B register is
        // 0... Note that the CRTC 4 ASIC is not affected by this exception."
        if (hasRomSelectR4Exception() && crtc.horizontal == 0
            && (crtc.registers[4] & 0x7f) == 0
            && (crtc.lastWritePort & 0x2000) == 0 && crtc.vertical != 0) {
            crtc.verticalMatchForced = false;      // compared against 0: no match, C4 overflows
        }
    }
    // A lowered R0 wraps immediately on the ASIC (>=), not on the next exact tick.
    bool horizontalCounterAtTotal(CRTC6845& crtc) const override { return crtc.horizontal >= crtc.registers[0]; }
    bool considersR6WriteImmediately() const override { return false; }        // §18.2.4
    // ACCC §18.2.4: "C4 is not incremented on these 2 CRTC's during vertical
    // adjustment" — the ASIC shares that with CRTC 0, not with CRTC 1 and 2.
    bool incrementsVerticalEachAdjustLine() const override { return false; }   // §18.2.4
    bool incrementsVerticalOnAdjustEntry() const override { return false; }   // §11.1
    bool adjustmentCannotOverflow() const override { return true; }           // §11.3.3
    bool additionalLinesCountFromZero() const override { return true; }       // §19.6.4
    // ACCC §16.4.4 (p.171): "VSYNC starts when C4=R7 and C9=C0=0. If R7 is modified
    // with the value of C4 while C0>0 and/or C9>0, it will not trigger CRTC VSYNC."
    bool startsVsyncOnR7Write(CRTC6845& crtc) const override {
        return crtc.horizontal == 0 && crtc.raster == crtc.baseRaster();
    }
    // ACCC §16.3 / §16.4.4: "There is no VSYNC reentrancy protection mechanism on
    // these circuits. If the condition C4=R7 and C9=C0=0 has not changed and is
    // renewed in the absence of an active VSYNC, then a VSYNC starts again."
    bool hasVsyncReentrancyProtection() const override { return false; }
    // ACCC §19.5.5 (p.214): "When R8 changes to 1 or 3, Parityc9=C9.0 (Parityc9 is fixed
    // with the parity of the current C9). This C9 parity is managed only when R8 = 3 to
    // update C9." Stated for these two chips alone -- see CrtcBehaviour::onInterlaceWritten.
    void onInterlaceWritten(CRTC6845& crtc, int) const override {
        if (crtc.displayMode() != 0) crtc.rasterParity = crtc.raster & 1;   // §19.2: BORDER ON is non-interlace
    }
    int outCWriteOffset() const override { return 12; }          // ACCC §4.4.3: 4th usec
    bool cVsyncNeedsCrtcVsync() const override { return true; }  // ACCC §16.2.3, p.164
    int modeSwitchPixelInByte() const override { return 3; }   // ACCC §9.3.4 (CRTC 4)
    int hsyncDisplayDelayCharacters() const override { return 1; }   // ACCC §15.1
    bool vsyncPrecedesParitySwap() const override { return true; }   // ACCC §19.7.3
    // ACCC §14.7.2/§15.1: the ASIC lines its HSYNC up with the character the GATE
    // ARRAY is displaying, so the black zone starts on the boundary and "an R2.JIT
    // update carried out with an OUT(C),r8 does not cause the HSYNC black zone to
    // appear later than in the other situations since the HSYNC is deferred".
    // ACCC §14.7.2 (p.143): the 40489's chronogram is not drawn yet — "The CRTC 3
    // diagram will be added in a later version. Currently assumed to be the same as
    // CRTC 4, but with HSYNC starting on the 17TH PIXEL after the start of the displayed
    // CRTC R2-1 character." The 17th pixel is index 16, which is one whole character of
    // ASIC HSYNC delay and nothing more — so 0 here, against the 40226's 2.
    int hsyncBlackStartPixel() const override { return 0; }
    int hsyncBlackStartPixelJit() const override { return 0; }
    // ACCC §27.7.2 (p.290): the "more or less late" HSYNC end is a discrete 6845's
    // problem. This chip IS the GATE ARRAY, so its end of HSYNC arrives on the character
    // and the Z80A always samples it in time. (CRTC 4 inherits it.)
    bool hsyncEndMissesInterruptSample() const override { return false; }
    // ACCC §14.9 (p.145) ends its per-chip HSYNC chart with "CRTC 3: to come." — the
    // compendium has no figure for the 40489's black zone, so it keeps the boundary
    // it had before the chart was consulted. CRTC 4 overrides it (p.145 measures it).
    int hsyncBlackEndPixel() const override { return 0; }
    // ACCC §14.5.4 (p.139): R3.JIT "does not work on CRTC's 3 and 4, which
    // synchronize the HSYNC with the display", so the boundary does not move.
    int hsyncBlackEndPixelJit() const override { return hsyncBlackEndPixel(); }
    int displayRestorePixelAfterHsync() const override { return 0; }   // ACCC §14.9
    int vsyncBlackStartPixel() const override { return 1; }            // ACCC §16.2.1 (2nd)
    // ACCC §16.2.1: R7.JIT "does not work on CRTC's 3 and 4, whose VSYNC only starts
    // on C0=0", so there is no separate position for it.
    int vsyncBlackStartPixelJit(bool) const override { return vsyncBlackStartPixel(); }
    // ACCC §9.2.1: "ASIC 40489 of the CPC+ is not affected by this discrepancy."
    int mode2PixelAdvance() const override { return 0; }
    // ACCC §9.2.2's chart for the 40489 (p.51) puts the first pixel in the new colour
    // half a byte earlier than every other chip does: mode 0 pixel 29 and mode 1 pixel
    // 58, where the classic GATE ARRAY has 30 and 60. In Pixel-M2 from the start of
    // the character, 4 instead of 8. (The 40226 shares this profile but not this
    // number, and overrides it back.)
    int inkChangePixelInCharacter() const override { return 4; }
    // ACCC §14.5.4: R3.JIT "does not work on CRTC's 3 and 4, which synchronize the
    // HSYNC with the display" -- with either instruction.
    bool supportsR3Jit() const override { return false; }
    // ACCC §17.5.2 (p.186): on these chips only an R1=0 written on the PREVIOUS line
    // is acknowledged — every position within the line is already too late.
    bool acknowledgesR1ZeroNow(CRTC6845&) const override { return false; }
    // ...and an R1 write of ANY value is compared from the next character on: one that
    // lands on the very character C0=R1 names is no C0=R1 there (no border, no VMA'
    // update). The same deadline as R1=0 above and as R6 (§18.1: "Except on CRTC 3 and
    // 4, R6 is considered immediately on the current C0"). MEASURED on SHAKER AO/I
    // ("R1=63 ON C0=63", the second half by OUTI, whose write lands on C0=63 by §4.4.3):
    // the real 6128 Plus (CRTC 3 and 4) and AmSpiriT 2.0 repeat the same text row in both
    // halves, VMA' never updated; ours took the match and walked the pointer on, a black
    // half screen. CRTC 0 takes it, and its photo shows the pointer walking.
    bool takesR1MatchInWriteCharacter() const override { return false; }
    bool vsyncWaitsForC0LeavingZero() const override { return true; }
    bool testsR6ZeroAtLineStart() const override { return true; }     // ACCC §18.3.4
    bool delaysVsyncOneLineInIvm(CRTC6845& crtc) const override {
        // §16.5: R9 odd, IVM mode (R8=3), odd frame, odd C4.
        return crtc.displayMode() == 3 && (crtc.registers[9] & 1) != 0
            && crtc.interlaceField != 0 && (crtc.vertical & 1) != 0;
    }

    // ---- ACCC §19.8.4 (p.236) COUNTING IN IVM, CRTC 3 and 4 --------------------
    // "If C9 >= R9 Then ... C9 = ParityC9 / Else If R8<3 Then C9=C9+1 Else C9=C9+2;
    //  C9 = C9 or ParityC9".  So in IVM the counter itself steps by 2 and carries
    // ParityC9 in bit 0 — there is no second, doubled value to build the address
    // from, and R9 is programmed exactly as on a CRTC 0 ("R9 contains the number of
    // character lines of character less 2"), so it must not be halved either.
    bool interlaceVideo(const CRTC6845& crtc) const override { return crtc.displayMode() == 3; }
    bool interlaceSyncAndVideo(const CRTC6845&) const override { return false; }
    bool frameParityFromParityR6() const override { return false; }   // plain toggle (§19.8.4)
    // "Else C4++ ; If R9.0==1 (R9 is odd) Then ParityC9 = ParityC9 xor 1". On the C4
    // wrap the frame path instead does "ParityC9 = ParityFrame", which is baseRaster().
    void updateRasterParity(CRTC6845& crtc) const override {
        if (crtc.displayMode() != 3 || (crtc.registers[9] & 1) == 0) return;
        crtc.rasterParity ^= 1;
        // ...and only then "C9=ParityC9". The core reset C9 from the parity BEFORE calling
        // this (it is called once C4 has moved), so the new row opened on the old parity:
        // C4=1 of an even R9=7 IVM frame started on C9=0 where §19.8.4's example 1 has
        // "ParityC9=1 because R9 is odd... C9=ParityC9 (C9=1)".
        crtc.raster = crtc.baseRaster();
    }
    // ACCC §19.6.4 (p.218): "The additional line is added at the end of the frame
    // (after the R5 lines if necessary) if one of the two 'Interlace' modes is
    // activated (R8=3 or 1) and IF PARITYFRAME IS EVEN." ParityR6, which the default
    // tests, is CRTC 0's and CRTC 2's state (§19.5.2/§19.5.4) and does not exist here.
    bool addsInterlaceLine(CRTC6845& crtc) const override { return (crtc.interlaceField & 1) == 0; }
    bool suppressesHsyncStartOnZeroWidth() const override { return false; }
    bool tracksScanlineInFrame() const override { return true; }
    // ACCC §18.2.4 / §18.3.4: R6 is tested at the start of a character row only (C9=C0=0)
    // -- "Setting R6 to 0 when C4 and C9 are 0, but C0>0 will have no effect before the new
    // frame", so not at the next line start of the same row.
    bool clearsVDisplayOnRowMatch() const override { return false; }
    // No advanceHsync of its own: ACCC 1.11 §15.3.1 (p.149) "On the CRTC's 1, 2, 3 and 4,
    // there is a bug if C0=R2 on C0=R2+R3", and §15.3.2 "On the other CRTC's, the HSYNC does
    // not end and C3 will overflow": the standard body's overflow. This chip had a plain
    // end-on-equality instead (from the port, no citation), so an HSYNC reaching R3l on
    // C0=R2 ended and a NEW one began there, restarting the GATE ARRAY's H06. SHAKER DH/B on
    // CRTC 3 (R0=1, R2=0, R3=6) is that case: the C-HSYNC was cut at 2 usec and a second one
    // sent 4 usec later, where the real 6128 Plus and AmSpiriT 2.0 show one ordinary sync and
    // the 17 usec black block of the overflowed HSYNC.
    void reloadNextRowAddress(CRTC6845& crtc) const override {
        // ASIC honours a mid-frame screen split (ACCC §13.5); otherwise it reloads
        // the row address at the maximum-raster wrap like a real 6845.
        std::optional<CrtcSplit> split = crtc.getSplit();
        int scanline = ((crtc.vertical & 0x3f) << 3) | (crtc.raster & 7);
        // ACCC §11.1 (p.81): during a vertical adjustment on these chips "C9 is only
        // compared with R5", so the R9 wrap that normally takes VMA' is not running --
        // and §11.2.6 says the pointer moves once, BEFORE the additional lines start:
        // "the video pointer is updated before the start of the additional lines
        // (VMA'=VMA) when C0=R1". §11.2.1's table (p.82) is the picture of that: the
        // CRTC 3/4 column reads 0 on all sixteen adjustment lines while CRTC 0's steps
        // once and CRTC 1/2's steps four times.
        if (crtc.verticalAdjust > 0) return;
        // The row's last line is the one the counter itself ends the character on --
        // §19.8.4's "If C9 >= R9" -- and what is latched is the MA counter. A 3-bit
        // equality missed IVM's C9=8 (R9=7, even parity: 0,2,4,6,8) and every C9 > R9.
        // The Plus soft scroll still offsets the comparison, as it offsets the address.
        const int scroll = crtc.getHorizontalScroll() & 7;
        const bool rowEnds = scroll == 0
            ? crtc.rasterMatchesMaximum()
            : ((crtc.raster + scroll) & 7) == (crtc.maximumRaster() & 7);
        if (split && split->line && split->line == scanline) {
            crtc.nextRowAddress = split->address & 0x3fff;
        } else if (rowEnds) {
            crtc.nextRowAddress = crtc.maRow & 0x3fff;
        }
    }
};

} // namespace cpcse
