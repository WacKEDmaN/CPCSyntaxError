// CPCSyntaxError — shared UM6845R profile (CRTC 1-A and CRTC 1-B).
// ACCC §11.6 (p.88, p.90) splits the UM6845R in two: "The value of R5 is of little
// importance, except on certain CRTC 1's for the value #10... CRTC 1-A is defined as
// CRTC 1 which does not support RFD#10." The two are the same part number and the
// compendium found 3 of 7 machines with the extra capacity, so the behaviour lives
// here and crtc_type_1b.cpp turns RFD#10 on.
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
#pragma once
#include "crtc.h"

namespace cpcse {

// ACCC §4.3 (p.22): on CRTC 1 and 2 the R3 high nibble is unused (no programmable
// VSYNC length — §14.2), and R8 keeps only the two interlace bits, the SKEW-DISPTMG
// function being "only available on CRTC's 0, 3 and 4" (§19.2).
inline const int UM6845R_WRITE_MASK[16] = { 0xff, 0xff, 0xff, 0x0f, 0x7f, 0x1f, 0x7f, 0x7f,
    0x03, 0x1f, 0x7f, 0x1f, 0x3f, 0xff, 0x3f, 0xff };

struct CrtcType1 : CrtcBehaviour {
    CrtcType1() { id = 1; name = "UM6845R (1-A)"; }
    // ACCC §11.6: whether this particular UM6845R takes RFD#10. "The brand and
    // model of CRTC 1-B does not differ from other CRTC 1's (UM6845R)... Out of 7
    // machines tested, 3 had this additional capacity."
    virtual bool supportsRfd10() const { return false; }
    // ACCC §17.4.2 (p.183) is written for CRTC 1: an R1 written ON the C0=R1 character
    // cancels that character's reload. SHAKER AO/B ("OUT R1>R0 ON C0vs=#28", R1 was 40)
    // is exactly that case: its reference freezes VMA' on row 24, and reloading at the
    // character's start froze ours on row 25 instead (25.9, FAIL).
    bool decidesR1ReloadAtCharacterEnd() const override { return true; }
    int writeMask(int reg) const override { return (reg >= 0 && reg < 16) ? UM6845R_WRITE_MASK[reg] : 0; }
    int normaliseRegister(int reg, int value) const override { return value & writeMask(reg); }
    int read(CRTC6845& crtc, int operation) const override {
        // ACCC §21.3.1: "Only CRTC 1 has a status register present on the specific
        // port &BE00", and its layout is  x L V x x x x x  --
        //   bit 6 (L) 1 = light pen reading, 0 = R16/R17 can be read
        //   bit 5 (V) 1 = BORDER R6 is true, 0 = BORDER R6 is false
        // The remaining bits are undefined.
        if (operation == 2) {
            int status = 0;
            if (crtc.lpenStrobe) status |= 0x40;
            // ACCC §21.3.1 (p.248): "bit 5 of the Status register is updated when
            // C0=R0 according to the BORDER R6 conditions (False: C4=C9=C0=0 / True:
            // C4=R6 & C9=C0=0)... Note also that if R6 is positioned at 0 (while C4>0)
            // to generate BORDER, this state is not detected." It is a LATCH sampled
            // once a line, not a live C4>=R6 comparison — "this does not necessarily
            // mean that BORDER or CHARACTERS are displayed".
            if (crtc.borderR6Status) status |= 0x20;
            return status;
        }
        // ACCC §28.1.9 (p.294): "On CRTC 1, this register returns 0 on all registers,
        // EXCEPT FOR R14-R17 and undefined R31 (and all registers whose bits 0 to 4
        // are at 1). Values 255 and 127 were observed."
        // ACCC §21.2.2 (p.246): "Only the 5 least significant bits of the register
        // number are considered... These CRTC's can read the contents of the following
        // registers on the port located at &BF00" — and the table lists the cursor
        // address (R14/R15, r/w) and the light pen (R16/R17, r). R12/R13 are readable
        // on CRTC 0 but NOT here. The high registers read back with bits 7-6 as 0.
        int reg = crtc.selected & 0x1f;
        // UM6845R data sheet, Status Register bit 6 (LPEN REGISTER FULL): "This bit
        // goes to '0' whenever either register R16 or R17 is read by the MPU. This bit
        // goes to '1' whenever a LPEN strobe occurs." Reading the pen registers is what
        // empties them, so the read is what clears the flag -- ACCC §21.3.1 says the
        // same from the other side ("0 = R16/R17 can be read").
        if (reg == 16 || reg == 17) crtc.lpenStrobe = false;
        if (reg == 14 || reg == 16) return crtc.registers[reg] & 0x3f;
        if (reg == 15 || reg == 17) return crtc.registers[reg] & 0xff;
        // ACCC §28.1.9: "On CRTC 1, this register returns 0 on all registers, except
        // for register 31 (and all registers whose bits 0 to 4 are at 1). Values 255
        // and 127 were observed." The register number is truncated to 5 bits.
        if (reg == 0x1f) return 0xff;
        return 0;
    }
    // ACCC §14.1: R3 bits 3-0 are the HSYNC width on every chip; see the VSYNC note
    // just below for what this chip does with bits 7-4.
    int hsyncWidth(CRTC6845& crtc) const override { return crtc.registers[3] & 0x0f; }
    // ACCC §14.1: the R3 table gives bits 7-4 as Vsync duration on CRTC 0, 3 and 4
    // only — on CRTC 1 and 2 those bits are marked "x" and the VSYNC is fixed at 16
    // lines. (This replaced a register-sniffing special case that returned 15 for
    // R4=62/R9=4/R5=0, which is not in the compendium.)
    int vsyncWidth(CRTC6845&) const override { return 16; }
    bool cancelsHsyncOnZeroWidth() const override { return true; }   // ACCC §14.5.2
    // ACCC §15.3.4: this chip alone gets its "invisible" end of HSYNC in before the
    // restart, so the GATE ARRAY re-zeroes H06 and emits a SECOND monitor sync.
    bool restartsCHsyncOnOverflow() const override { return true; }
    int hsyncBlackStartPixel() const override { return 5; }   // ACCC §14.7.1 (6th)
    // ACCC §14.5.4 (p.139): "On a CRTC 1, the HSYNC starts on the 6th pixel-M2 and lasts
    // 3 pixel-M2's" when an OUT(C),r8 writes R3=0 on its first µsec -- p.141's 40010 row
    // is black on 5, 6, 7 and the 40007/8 row one pixel more (hsyncBlackEndLag).
    int r3ZeroFirstMicrosecondBlackEndPixel() const override { return 8; }
    // ACCC §14.4's table (p.134), the R3.NJIT row for this chip: 0.1250 / 1.1250 /
    // 2.1250 / 3.1250 µsec for R3l = 2 / 3 / 4 / 5, against 4.0000 for R3l=6. The
    // fraction is a flat 2/16 µsec, and it is this chip's own -- CRTC 0 and 2 read
    // 1/16 on the same page. It makes the screen shift 7 pixels per R3l step here,
    // which is the paragraph's "instead of the expected 8 pixels".
    int hsyncFallPhase16() const override { return 2; }
    // ACCC §9.3.4.3 (p.58): the zone lasts 32 Pixel-M2 from the 6th, so it ends on the
    // 6th of the character after the HSYNC. That is also the Pixel-M2 the GATE ARRAY
    // changes mode on, which is why "on CRTC 1, the reactivation of the display
    // corresponds exactly to the start of the mode change by the GA, which does not
    // allow you to take advantage of a nice mode 2 pixel of the previous graphics
    // mode" — the pixel CRTC 0, 2 and 4 show is the one this chip keeps black.
    int hsyncBlackEndPixel() const override { return 5; }
    int vsyncBlackStartPixel() const override { return 5; }   // ACCC §16.2.1 (6th)
    // ACCC §19.6.2: on this chip the additional line goes on the frame where
    // "ParityFrame is even", one frame offset from the CRTC 0/2 ParityR6 test.
    bool addsInterlaceLine(CRTC6845& crtc) const override { return (crtc.interlaceField & 1) == 0; }
    // ACCC §19.5.3 (p.209): "There are TWO parity states: ParityFrame switch between
    // each frame when C4=C9=C0=0 [and] ParityC9 switch between each C4 if R9 is even."
    // No ParityR6 on this chip -- that one belongs to CRTC 0 (§19.5.2) and CRTC 2
    // (§19.5.4). "When a frame starts, ParityFrame switch off even to odd and vice
    // versa, whatever the value of R8."
    bool frameParityFromParityR6() const override { return false; }
    // ACCC §19.8.2: "ParityC9 = ParityC9 xor (not r9.0) (inversion of parity if r9
    // is even)" — the opposite R9 parity to CRTC 0, and a toggle rather than an
    // assignment. C9 itself steps by 2 on this chip (interlaceVideo stays true).
    void updateRasterParity(CRTC6845& crtc) const override {
        if (crtc.displayMode() == 3 && (crtc.registers[9] & 1) == 0) crtc.rasterParity ^= 1;
    }
    // ACCC §19.5.3 (p.210): "When R8 is modified (R8 changes from 0 to 3 or vice versa),
    // the parity and/or bit 0 of C9 are updated according to rules which involve the
    // current parity, the parity of C9 before the change to IVM and the parity of C4 if
    // R9 is even. These updates are performed on the 3rd and 4th µseconds of the
    // OUT(C),C instruction (on R8)."
    //
    //   3rd µsecond, in either direction:
    //     ParityC9 = C9.0
    //     ParityC9 = ParityC9 xor (C4.0 and not (R9.0))
    //   4th µsecond, IVM becoming active (OUT R8,3):
    //     If (ParityFrame==EVEN) Then ParityC9 = C4.0 and (not R9.0) End If
    //     ParityFrame = ParityFrame and (ParityC9 xor (C4.0 and (not R9.0)))
    //   4th µsecond, IVM going idle (OUT R8,0):
    //     ParityFrame = ParityC9
    //
    // The two microseconds are NOT one event. §19.5.3's chronograms (p.211-212) give
    // C9 a value per microsecond across C0=0..11, and in the EVEN / C4.0=1 / R9.0=0 /
    // C9.0=1 cell -- the one labelled Test 2 (B), 6 (F) -- C9 reads
    //
    //     1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0
    //                     ^  ^
    //                     |  the 4th µsec puts it back to 1
    //                     the 3rd µsec of OUT R8,3 drops it to 0
    //
    // A single microsecond at 0, and C9 is what the video pointer counts with, so that
    // microsecond fetches a different row. Applying both rules at one instant cannot
    // produce it. So this is the 3rd µsecond only, and the 4th is below.
    void onInterlaceWritten(CRTC6845& crtc, int previousR8) const override {
        bool wasIvm = (previousR8 & 3) == 3;
        bool isIvm = (crtc.registers[8] & 3) == 3;
        if (wasIvm == isIvm) return;          // no rule stated for the other transitions
        int c4AndNotR9 = (crtc.vertical & 1) & ((crtc.registers[9] & 1) ^ 1);
        crtc.rasterParity = (crtc.raster & 1) ^ c4AndNotR9;
        crtc.raster = (crtc.raster & ~1) | (crtc.rasterParity & 1);
        relatchLastLine(crtc);
    }
    // THE ROW-END LATCH FOLLOWS C9. r9Match says "the line now running is the last of the
    // row", and it is taken one line ahead, when C9 steps. Both writes that change C9 or
    // the form of the comparison part-way through a line have to ask again: an R9 write
    // already does (onRegisterWritten), and so must §19.5.3's write-back, which rewrites
    // C9's bit 0 and switches the test between §19.8.2's two forms.
    //
    // SHAKER C2/D1 ("IVM ON/OFF ON ODD C9"), MEASURED off the per-line trace: on the frame
    // that writes R9=6 at C0=5 of a C9=7 line and then toggles IVM at C0=19/23, the
    // write-back leaves C9 = 6 -- equal to the new R9 -- but the latch still held the
    // answer from the R9 write, 7 != 6. The row ran on to C9=31 and wrapped, drawing a
    // character row four times over; the reference, and §19.8.2's "If C9 == R9" taken at
    // the line end, end it.
    void relatchLastLine(CRTC6845& crtc) const { crtc.r9Match = rasterMatches(crtc); }
    // The 4th µsecond, one character later. It carries on from the ParityC9 the 3rd
    // left -- it must not recompute it from C9, because C9 already holds it.
    void onInterlaceWrittenFourthMicrosecond(CRTC6845& crtc, int previousR8) const override {
        bool wasIvm = (previousR8 & 3) == 3;
        bool isIvm = (crtc.registers[8] & 3) == 3;
        if (wasIvm == isIvm) return;
        int c4AndNotR9 = (crtc.vertical & 1) & ((crtc.registers[9] & 1) ^ 1);
        if (isIvm) {
            if ((crtc.interlaceField & 1) == 0) crtc.rasterParity = c4AndNotR9;
            crtc.interlaceField = (crtc.interlaceField & 1) & (crtc.rasterParity ^ c4AndNotR9);
        } else {
            crtc.interlaceField = crtc.rasterParity & 1;
        }
        // C9 follows ParityC9 out of this microsecond too, and keeps it after IVM is
        // switched off again -- p.212's Test 1 (A) cell (C4.0=1 R9.0=0 C9.0=1) runs
        // C9 = 1,1,1,1,1 then 0,0,0,0 then 1,1,1, the last three being the OUT R8,0's
        // own 3rd µsecond and after. Every cell of p.211/p.212 is in the frame-check's
        // checkIvmParity, and the names in them -- 1 (A), 10 (J2), 11 (K2), 12 (L),
        // 14 (N), 15 (O), 16 (P), 25 (Y2), 26 (Z), 27 (ZA) ... -- are SHAKER module
        // C's own C4 sub-test letters, so a failing cell names a failing screen.
        crtc.raster = (crtc.raster & ~1) | (crtc.rasterParity & 1);
        relatchLastLine(crtc);
    }
    void onCharacterPosition(CRTC6845& crtc, int c0) const override {
        // ACCC §21.3.1: the status bit-5 latch is evaluated at C0=R0. R6=0 with C4>0
        // raises the BORDER but is NOT detected here ("the value 0, like in other
        // CRTC 1 registers, is handled in a special way").
        if (c0 != crtc.registers[0]) return;
        if (crtc.vertical == 0 && crtc.raster == 0) crtc.borderR6Status = false;
        else if (crtc.registers[6] != 0 && crtc.vertical == (crtc.registers[6] & 0x7f)
                 && crtc.raster == 0) crtc.borderR6Status = true;
    }
    int displaySkew(CRTC6845&) const override { return 0; }
    bool initialVerticalDisplay(CRTC6845&) const override { return true; }
    // ACCC §12.1: R4 is seven bits, and the vertical total is reached on C4 == R4.
    bool verticalTotalMatches(CRTC6845& crtc) const override { return crtc.vertical == (crtc.registers[4] & 0x7f); }
    // ACCC §19.8.2 (p.226): in IVM this chip counts C9 in two stages, and the test
    // sits between them:
    //
    //   When C0 goes to 0:
    //     C9 = C9 + not (R9.0)                      (C9 is incremented if R9 is even)
    //     If (C9 and %11110) == (R9 and %11110)     (test C9/R9 EXCLUDING PARITY)
    //     Then  C4 management ; ParityC9 = ParityC9 xor (not r9.0) ; C9 = ParityC9
    //     Else  C9 = C9 + 1 + (R9.0)
    //     End if
    //
    // Bit 0 is masked off BOTH sides, so the test works for an even R9 as well as an
    // odd one. (A `(C9|1) == R9` form can never match an even R9 at all.) The two
    // increments always total 2, but the FIRST of them lands before the comparison, so
    // with an even R9 the test is made on C9+1 and not on C9 -- which is the whole
    // point of §19.8.2's "equivalence in number of lines between the even and odd
    // frames". Testing the bare C9 instead gave both halves the same length: with
    // R9=6, C9 ran 0,2,4,6 and then 1,3,5,7 (four lines each) where the chip runs
    // 0,2,4,6 and then 1,3,5 (four, then three). With an odd R9 the term is zero and
    // nothing changes. On the match "C9 = ParityC9", which is what baseRaster()
    // returns; the caller's r9Match latch defers that to the next line boundary, which
    // is the same line the chapter resets on because the test here runs one step ahead.
    bool rasterMatches(CRTC6845& crtc) const override {
        if (!crtc.oldInterlaceVideo()) return crtc.raster == crtc.maximumRaster();
        int preIncrement = (crtc.raster + ((crtc.registers[9] & 1) ^ 1)) & 0x1f;
        return (preIncrement & 0x1e) == (crtc.maximumRaster() & 0x1e);
    }
    bool advanceVertical(CRTC6845& crtc) const override { return crtc.updateVerticalType1(); }
    bool adjustmentLimitReadLive() const override { return true; }   // ACCC §11.3
    // ACCC §13.3 note 3 (p.114): "when the OUTI instruction is used on CRTC 1 to modify
    // R0, THE COMPARISON OF C0 WITH R0 TAKES PLACE AFTER THE ASSIGNMENT of R0 with the
    // new value in some cases. Consequently, on the position where C0 should have gone
    // to 0, if R0 is modified on the last µsecond of the OUTI instruction, then C0 is
    // compared with the new value of R0, which can lead to an overflow of C0. Example:
    // If R0 was 49 and the 5th µsecond of the OUTI is at the position following C0=49,
    // and R0 is modified to 20, then C0 will not be equal to 0 but to 50." An
    // OUT(C),r8 landing on the same microsecond does not do this — its comparison ran
    // first, against the value R0 had when the character opened.
    // Note 3's effect is an UN-WRAP, not a different comparison: C0 has already gone to
    // 0, and the late assignment makes it carry on from where it was instead. The
    // example is exact -- old R0 49, C0 back at 0, and the answer is 50, which is
    // r0Previous+1. §13.6.2's chronogram is the same thing seen from outside: an OUTI
    // starting at #3c with R0=#3f is still "in time", one microsecond past the CRTC 0/2
    // deadline, because the line it should have ended goes on instead.
    //
    // Comparing C0 against r0Previous for OUT(C),r8, which is what stood here, made the
    // opposite change: it pushed CRTC 1's OUT deadline one microsecond EARLIER than
    // CRTC 0 and 2, where §13.6.1 and §13.6.2 put both at a start of #3d.
    bool unwrapsC0OnBlockR0Write() const override { return true; }
    // ACCC §13.3 Note 3 (p.114) is a CONTRAST, and the half of it that is not about OUTI
    // has to be read too. The note exists to say that "using the OUTI instruction to
    // modify R0 has unintended consequences COMPARED TO THE SAME CHANGE MADE WITH AN
    // OUT(C),R8", and the consequence it names is this one:
    //
    //   "on the position where C0 should have gone to 0, if R0 is modified on the last
    //    µsecond of the OUTI instruction, then C0 is compared with the new value of R0,
    //    WHICH CAN LEAD TO AN OVERFLOW OF C0.
    //    Example: If R0 was 49 ... and R0 is modified to 20, then C0 will not be equal
    //    to 0 but to 50 in some cases."
    //
    // The overflow belongs to OUTI. With an OUT (C),r8 landing on that same position,
    // C0 does go to 0: the line the chip had already decided to end, ends.
    //
    // That is not the same as "the comparison precedes the assignment", which would also
    // make §13.6.2's third row end its line -- and the colour of that row's marker is
    // #FFC000, the legend's "update of R0 ok (just in time)", not #E16B09's "not
    // considered". There the new R0 (#7F) is ABOVE the C0 the write lands on (#3F) and
    // the line is enlarged. The two rows differ only in which side of C0 the new R0
    // falls, so that is what decides it: a pending end is cancelled by an R0 the counter
    // has not reached yet, and is not cancelled by one it has already passed.
    //
    // Pinball Dreams' rupture is the second case exactly -- "R0<-20 at C0=42", with 42
    // the R0 it is replacing, through OUT (C),C.
    //
    // §13.6's closing prose (p.124) is the same rule stated for the chronograms as a
    // whole -- "THE C0 COUNTER NEVER EXCEEDS THE VALUE OF R0 when R0 is updated according
    // to the timing described in the previous diagrams" -- and the OUTI here is its one
    // recorded exception, so that is the only thing carved out: after a block I/O write
    // the unwrap above has already put C0 back to r0Previous+1, and comparing that for
    // equality alone is what lets it run on, which is note 3's "overflow of C0".
    bool horizontalCounterAtTotal(CRTC6845& crtc) const override {
        if (crtc.horizontalTotalMatch && !crtc.lastWriteBlockIo
            && crtc.horizontal > (crtc.registers[0] & 0xff)) return true;
        return crtc.horizontal == crtc.registers[0];
    }
    void onRegisterWritten(CRTC6845& crtc, int reg) const override {
        // ACCC §11.6 RUPTURE FOR DUMMIES: "there is a very interesting bug when R5 is
        // updated with a value different from 0 on the position C0=R0 of some C9s
        // when R5 is equal to 0... Going from R5>0 to R5=0, or from R5>0 to another
        // value of R5>0 does not trigger this bug."
        if (reg == 5) {
            // §11.6 words the trigger as "R5 is updated with a value different from 0 ON
            // THE POSITION C0=R0 of some C9s when R5 is equal to 0", and ON C0=R0 ONLY.
            // SHAKER's RFD scanner (module B, CTRL: "R5 SHAKER : MAGIC COCKTAIL", screens
            // BCTRL/A1-A3) sweeps the 0 -> 1 write across every C0 of every C9 and prints
            // the position: "UPDATE R5 FROM 0 TO 1 ON C0=#3E" (R0-1) leaves the picture
            // intact on the real CPC and on AmSpiriT, and only C0=#3F (R0) ruptures it.
            // The SHAKER 1-A/1-B identifier (module B, test O) lands its "OUT R5,#10 +
            // OUT R5,0" on C0=R0 as well, and so does DSC4's CRTC 1 route (C0=63, traced).
            // R0-1 was once accepted too, for DSC4 when its write still measured a
            // character early; it made our 1-A rupture on BCTRL/A1 where the chip does not.
            const int r0 = crtc.registers[0] & 0xff;
            const bool atR0 = crtc.horizontal == r0;
            if ((crtc.r5Previous & 0x1f) == 0 && (crtc.registers[5] & 0x1f) != 0 && atR0) {
                crtc.rfdActive = true;
                // ACCC §11.6/§11.6.2 (p.88, p.90): "The value of R5 is of little
                // importance, EXCEPT ON CERTAIN CRTC 1's FOR THE VALUE #10... On CRTC
                // 1-B, value #10 in R5 deactivates parity management in test C9=R9,
                // while other values activate this parity management (including value
                // #10 on CRTC 1-A)." So on a 1-B this RFD skips the faulty-parity
                // frame entirely and behaves as case 2 on both frames.
                // §11.6.2 makes the two directions asymmetric: an RFD that turns parity
                // ON locks it on for the rest of the frame, and an RFD#10 can only turn
                // it off while no earlier RFD in this frame has locked it.
                bool rfd10 = supportsRfd10() && (crtc.registers[5] & 0x1f) == 0x10;
                if (!rfd10) { crtc.rfdIgnoresParity = false; crtc.rfdParityLocked = true; }
                else if (!crtc.rfdParityLocked) crtc.rfdIgnoresParity = true;
            }
            return;
        }
        // ACCC §18.3.3 (p.192): "When R6 is updated with 0, the BORDER is activated as
        // long as the register value is 0... HOWEVER, IF C4=R6=0 (1st line-character of
        // a new frame) DURING THIS UPDATE, BORDER R6 BECOMES TRUE FIRST FOR ALL THE
        // REST OF THE FRAME, until the new frame (C4=C9=C0=0)." Everywhere else the
        // R6=0 border is live and cancellable (see displayEnabled).
        if (reg == 6 && crtc.registers[6] == 0 && crtc.vertical == 0 && crtc.raster == 0) {
            crtc.vDisplay = false;
            return;
        }
        // R8 is handled by onInterlaceWritten and onInterlaceWrittenFourthMicrosecond,
        // which are §19.5.3's two microseconds. A second copy of the same algebra used
        // to sit here as well, and since onRegisterWritten runs AFTER onInterlaceWritten
        // it was the copy that actually decided the outcome -- with r8Previous (R8 as of
        // the character's start) for "was IVM" instead of R8 as of the write, and with
        // both microseconds collapsed into one. The frame-check's p.211/p.212 cells
        // caught it: C9 and ParityC9 disagreed the instant the write returned, which
        // one rule applied once cannot do. §11.6.2's "IVM ON/OFF" needs nothing beyond
        // that algebra either -- leaving IVM sets ParityFrame from ParityC9, which is
        // exactly how "activate and deactivate the IVM mode with an odd R9 to set an
        // even parity" works; there is no separate freeze.
        // ACCC §13.7.1.2 (p.125), the RFD's second trigger: "When C0 reaches R0 on the
        // last line of a frame on CRTC 1, some internal end-of-frame states are updated.
        // If R0 is modified on position C0=R0 of this last line of the frame
        // (C9=R9/C4=R4/R5=0) IN ORDER TO ENLARGE IT, and the last line condition is
        // canceled (by modifying R9 and/or R4) during this enlargement, THIS PARADOX
        // WILL GENERATE AN RFD STATE, which will be triggered at the end of the line."
        // §13.6.2's chronogram footnote points at the same case.
        if (reg == 0) {
            if (crtc.horizontal == crtc.r0Previous && crtc.r4Match && crtc.r9Match
                && (crtc.registers[5] & 0x1f) == 0
                && crtc.registers[0] > crtc.r0Previous) {
                crtc.lastLineEnlarged = true;
            }
            return;
        }
        // The cancellation half of that trigger: an R4/R9 write inside the enlarged
        // tail that breaks the last-line condition arms the RFD proper.
        if (crtc.lastLineEnlarged && (reg == 4 || reg == 9)) {
            bool stillLast = reg == 4
                ? crtc.vertical == (crtc.registers[4] & 0x7f)
                : crtc.raster == crtc.registers[9] + (crtc.registers[8] & 1);
            if (!stillLast) {
                crtc.rfdActive = true;
                crtc.lastLineEnlarged = false;
            }
        }
        // ACCC §11.2.4 (p.85): "If R9 and/or R4 are modified on the last line of the
        // frame while additional lines are programmed, their value is taken into
        // account immediately to postpone the end of the frame... HOWEVER, IF C4==R4
        // AND C9==R9 IN POSITION C0==R0, AND R4 OR R9 IS MODIFIED IN THIS POSITION,
        // THEN THE ADDITIONAL MANAGEMENT REMAINS TRUE." So on that one position a
        // write cannot take back a match that already held.
        if (crtc.horizontal == crtc.registers[0] && crtc.r4Match && crtc.r9Match) {
            // "However, if R4 was modified to C0==R0 with R4>0, then VMA is not updated
            // with R12/R13 when C4=1." The R9 form of the same write does not block it:
            // "if R9 is modified to C0==R0 when C4==R4=0, then VMA continues to be
            // updated with R12/R13 when C4=1."
            if (reg == 4 && (crtc.registers[4] & 0x7f) > 0) crtc.adjustBlocksRegisterReload = true;
            return;
        }
        // ACCC §12.3 (p.95): "If R4 is updated with the value of C4 [the frame ends]...
        // If R4 is updated with a value LESS than C4, then C4 will increment to its
        // maximum value (127) before looping back. Unlike CRTC 0, putting R4=0 on the
        // last line of a screen will cause an 'overflow' of C4, THE VALUE 0 BEING
        // HANDLED AS A GENERAL CASE. If we want to put R4 to 0 so that C4 loops to 0,
        // we must do it only when C4 = 0." So this is an equality, and the runaway
        // frame it can produce is the hardware's own.
        if (reg == 4) crtc.r4Match = crtc.vertical == (crtc.registers[4] & 0x7f);
        // ACCC §10.3.2.1 (p.77) states the R9 write for THIS chip in three lines, and
        // §10.3.2.2 is titled NO EXCEPTION ("everything is pure logic, in an unspeakable
        // and tasteless simplicity"):
        //   "If R9 is changed to the CURRENT VALUE of C9, then on the following line:
        //    C9 goes to 0. C4 is incremented by 1, and goes to 0 if it was R4..."
        //   "If R9 is modified with a value LESS than C9: C9=C9+1 and C4 is unchanged
        //    (until the end of the overflow of C9)."
        //   "If R9 is modified with a value GREATER than C9, then C9=C9+1. C4 is
        //    unchanged."
        // So it is one equality against C9 and nothing else. This used to add R8's
        // interlace bit to R9 before comparing, which has no source: with R8=1 it made
        // an R9 written ONE BELOW C9 read as a match, ended the row and stepped C4 --
        // where the chapter says C9 simply counts on and overflows.
        // ...and it is the SAME comparison the chip makes at a line end, which is why
        // this asks rasterMatches rather than spelling an equality out again: outside
        // interlace that is C9==R9, and in IVM it is §19.8.2's parity-excluding "(C9 and
        // %11110) == (R9 and %11110)". Spelling it as a plain equality here left an R9
        // written under IVM unmatched -- C9=2 with R9=3 is a last line on this chip, and
        // the row ran on instead.
        else if (reg == 9) crtc.r9Match = rasterMatches(crtc);
    }
    // ACCC §18.3.3 (p.192), first clause: "when R6 is updated with 0, the BORDER is
    // activated AS LONG AS THE REGISTER VALUE IS 0" -- so on this chip the R6=0 border is
    // live and cancellable, not latched. §19.1 (p.193) says the same from R8's side: the
    // skew field can "deactivate the display AS R6=0 DOES ON CRTC 1". The clause that
    // makes it definitive when C4=R6=0 opens a frame is in onRegisterWritten above.
    bool displayEnabled(CRTC6845& crtc) const override { return crtc.registers[6] != 0; }
    // NO blocksAutoVsync override. It used to read
    //     return crtc.registers[4] == 0 && crtc.registers[5] == 0;
    // with no comment and no citation -- the only rule in this file that quoted no
    // chapter -- and nothing in the compendium supports it. ACCC §19.7 (p.219) states
    // the general case: "IN GENERAL, VSYNC OCCURS WHEN C4 IS EQUAL TO R7 ON ANY POSITION
    // OF C0 (except on CRTC's 3 and 4, which dictate that C4=C9=C0=0)." R4 and R5 are not
    // part of it. §20.3.1 (p.243) says the opposite of a block for exactly this register
    // set: with R4=R9=0 "C4 passes through the values 0 and 1 alternately ... this is the
    // triggering of the 'R5' additional line management, which generates a line despite
    // R5=0, and therefore INCREMENTS C4 DESPITE R4=0".
    //
    // A flood is not a risk that needs guarding against here: §16.3's re-entrancy latch
    // (hasVsyncReentrancyProtection / r7Match) already holds the equality until it
    // CHANGES, so C4 and R7 both sitting at 0 arm exactly one VSYNC, not one per line.
    // That latch is this chip's documented protection; the override was a second,
    // undocumented one layered on top of it.
    //
    // (Symptom that exposed it, for the record only: any program that ruptures with
    // R4=R5=0 and sets R7=0 got no VSYNC at all, so the monitor free-ran to its 353-line
    // limit -- 4 MHz / (353 x 64 x 4) = 44.3 Hz instead of 50.)
    bool rasterMatchesMaximum(CRTC6845& crtc) const override { return crtc.r9Match; }
    bool reloadsStartAddressExtra(CRTC6845& crtc) const override {
        // ACCC §13.3 (p.110) + §20.3: on the UM6845R "the offset is considered as
        // long as C4=0, whatever the value of C9, unlike all other CRTC's" — and
        // it is considered *while* C4=0, not at the moment C4 goes back to 0. The
        // caller has already established C4=0, so there is nothing further to
        // qualify: no R4/R5 test belongs here. (This is what lets CRTC 1 make 14
        // "hidden" ruptures and reach every C9 value.)
        return true;
    }
    // ACCC §11.6: while an RFD is active "VMA update via R12/R13 is activated by the
    // RFD and this state persists"; the offset can be changed "on each line of every
    // character, as if C4 was 0". So the C4=0 gate on the reload is lifted entirely.
    // ACCC §11.6 / §11.2.4 (p.85): the RFD lifts the C4=0 gate on the reload, and so
    // does an adjustment entered from C4=0 — "it is then possible to modify the offset
    // on each line C9 of C4=1 as one would do when C4=0, or on any value of C4 in RFD".
    bool reloadsStartAddressAnyRow(CRTC6845& crtc) const override {
        if (crtc.rfdActive) return true;
        return crtc.verticalAdjust > 0 && crtc.adjustFromFrameStart && crtc.vertical == 1;
    }
    void reloadNextRowAddress(CRTC6845& crtc) const override {
        // ACCC §11.6.1: under an RFD the C9=R9 test taken at C0=R1 "is carried out
        // with the current parity", so on one frame it is faulty and VMA' is never
        // updated — "the characters are repeated" — and on the next it behaves
        // normally. Once VMA really is updated with VMA', "a R12/R13 update is no
        // longer considered", which retires the RFD's R12/R13 source state.
        if (!crtc.rfdActive) {
            // CPCWiki: "when RC==(R9-1), the CURRENT MA is captured for the next
            // char-line", and ACCC §17.1 (p.176) keys that capture on "the equality
            // between C0 and R1... when C9=R9". It is the MA counter that is latched,
            // not the row start plus C0: the two only differ once MA has run on past
            // the line -- 256 characters of it, when C0 overflowed through 255 without
            // a line end (§17.1's overflow) -- and that difference is the whole
            // question on a ruptured line.
            if (crtc.rasterMatchesMaximum())
                crtc.nextRowAddress = crtc.maRow & 0x3fff;
            return;
        }
        // ACCC §11.6: the RFD "activates parity management in the C9/R9 test carried
        // out in IVM when C0 reaches R1". That test's parity is ParityFrame, which
        // §19.5.3 keeps running on every frame — one state, not the private copy the
        // RFD used to carry.
        if ((crtc.interlaceField & 1) == 0 && !crtc.rfdIgnoresParity) return;   // case 1: faulty test, row repeats
        if (!crtc.rasterMatchesMaximum()) return;
        crtc.nextRowAddress = (crtc.rowAddress + crtc.horizontal) & 0x3fff;
        crtc.rfdActive = false;                          // case 2: VMA' taken, RFD retires
    }
    bool freezesStartAddressReload(CRTC6845& crtc) const override {
        // ACCC 1.10 §17.4.2 (CRTC 1): when R1>R0 the VMA'/VMA pointer is no longer
        // reloaded with R12/R13 — it stays frozen on the last pointer latched before
        // the rupture, so every ruptured line/frame repeats the same base address.
        // Without this, a pseudo-frame (C4 wrap) during an R0 rupture follows a
        // mid-frame R12/R13 change and reads a different VRAM run each frame (striping
        // where Amspirit/real hardware show identical solid lines — Shaker A2/A1).
        // Scoped to CRTC 1: CRTC 0/3/4 drive this test through DISPTMG skew instead
        // (§17.4.1) and freezing the address there regresses vs the Amspirit oracle.
        // The R1>R0 freeze covers real R0 ruptures (Shaker A2/A3: R0=1..3) and is
        // harmless for a full-width normal frame (R0=63/R4!=0, constant R12/R13). The
        // one case it must NOT fire on is the degenerate full-width 1-line frame
        // (R0>=63 && R4==0) that Pinball Dreams' BG-logo clear uses to blank the screen:
        // there R12/R13 changes every frame and the address must still reload, else the
        // VMA pointer accumulates and paints garbage. No Shaker rupture uses R0>=63 with
        // R4==0, so this carve-out is invisible to the Shaker suite.
        return crtc.registers[1] > crtc.registers[0]
            && !(crtc.registers[0] >= 63 && crtc.registers[4] == 0);
    }
};

} // namespace cpcse
