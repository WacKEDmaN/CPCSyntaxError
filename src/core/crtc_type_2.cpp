// CPCSyntaxError — CRTC type 2 profile.
// MC6845 (Motorola) register masks, status and normalization rules.
// Differences from HD6845S (type 0) that matter to the CPC / Shaker:
//   * No readable status register — a status read returns 0 (no VDISP bit).
//   * R0=0 is a legal value (not forced to 1): the "R0=0" rupture tricks are
//     CRTC-2 territory (ACCC — CRTC 2 : R0=0). So normaliseRegister only masks.
//   * VSYNC width is fixed at 16 lines (R3 high nibble does not program it).
//   * DISPTMG/CUDISP skew (R8 bits 4-7) exist on the MC6845 as on the HD6845S.
// The tick/VMA core routes type 2 through the type-0-like paths in crtc.cpp;
// MC6845-specific VMA'/add-line edge cases are refined separately.
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
#include "crtc.h"

namespace cpcse {

// ACCC §4.3 (p.22): as CRTC 1 — R3's high nibble and R8's skew bits are unused here.
static const int WRITE_MASK[16] = { 0xff, 0xff, 0xff, 0x0f, 0x7f, 0x1f, 0x7f, 0x7f,
    0x03, 0x1f, 0x7f, 0x1f, 0x3f, 0xff, 0x3f, 0xff };

struct CrtcType2 : CrtcBehaviour {
    CrtcType2() { id = 2; name = "MC6845"; }
    bool adjustEntryStepNeedsRasterMatch() const override { return true; }   // §12: C4=R4+1 if C9==R9
    // ACCC §13.6's closing prose (p.124), printed under the last of its three
    // chronograms and so speaking for all of them:
    //
    //   "The C0 counter never exceeds the value of R0 when R0 is updated according to
    //    the timing described in the previous diagrams."
    //
    // An R0 written onto the character the line was already going to end on therefore
    // does not cancel that end when the new value is one C0 has already passed: the line
    // ends and C0 goes to 0. It cancels it only when the new value is still AHEAD of C0,
    // which is the enlargement every row of those chronograms draws (#3f -> #7f) and
    // which checkR0Deadline covers.
    //
    // The sentence is conditional, and deliberately so -- a C0 that runs to 255 is not
    // ruled out in general, §17.1 (p.176) has one and says what the display does about
    // it. So what is protected is the end the chip had ALREADY COMMITTED TO and nothing
    // wider; comparing C0 >= R0 outright, as the ASIC's do, costs CRTC 0 its HSYNC
    // re-entrance protection (§27.1) and CRTC 2 its R.V.L.L.
    //
    // Both instructions, as on CRTC 0. §13.7.1 (p.125) names the OUTI / OUT(C),reg8
    // difference as "an internal processing phase shift between THIS CRTC [1] AND
    // CRTCs 0 AND 2", so there is nothing to separate them here; §13.4 (p.118) is this
    // chip's own R0 chapter and names no C0 overflow either.
    bool horizontalCounterAtTotal(CRTC6845& crtc) const override {
        if (crtc.horizontalTotalMatch && crtc.horizontal > (crtc.registers[0] & 0xff)) return true;
        return crtc.horizontal == crtc.registers[0];
    }
    int writeMask(int reg) const override { return (reg >= 0 && reg < 16) ? WRITE_MASK[reg] : 0; }
    int normaliseRegister(int reg, int value) const override { return value & writeMask(reg); }
    int read(CRTC6845& crtc, int operation) const override {
        // ACCC §28.1.9: "On CRTC 2, this register is used to read registers R16 and
        // R17. The value 0 is returned on all other register numbers. The register
        // number value is truncated to 5 bits." The status port is high impedance on
        // this chip (§21.3.1), so only the read port returns anything.
        // ACCC §21.3.2 (p.248): "These two CRTC's do not have a status register...
        // My CPC CRTC 2 always returns 255 read on this port. My CPC CRTC 0 randomly
        // returns 255 or 127." So &BE00 floats high here rather than reading 0.
        if (operation == 2) return 0xff;
        if (operation != 3) return 0;
        // ACCC §28.1.9 (p.294): "On CRTC 2, this register is used to read registers R16
        // and R17. The value 0 is returned on all other register numbers. The register
        // number value is truncated to 5 bits." §21.2.2's table prints one readable set
        // for CRTC 1 and 2 together and includes the cursor address there, but §28.1.9
        // is the section about telling the chips apart and states CRTC 2's set alone —
        // and a program that reads R14/R15 back non-zero here would take this chip for
        // a CRTC 1. The narrower reading wins; CRTC 1 does return R14-R17 (§28.1.9
        // agrees with §21.2.2 on that half).
        int reg = crtc.selected & 0x1f;
        if (reg == 16) return crtc.registers[16] & 0x3f;   // light pen high, bits 7-6 = 0
        if (reg == 17) return crtc.registers[17] & 0xff;   // light pen low
        return 0;
    }
    // ACCC §14.6 (p.142), the half of it that was quoted below but not implemented: on
    // this chip R3l=0 does not mean "no HSYNC" and does not mean "a HSYNC of zero" --
    // "A VALUE OF 0 IN R3 WILL GENERATE A HSYNC OF 16 usec". suppressesHsyncStartOnZeroWidth
    // below already lets the sync start; without this it started and was over at once.
    // CRTC 3 and 4 have had the 16 all along (crtc_type_3.h); this chip is the third the
    // sentence names.
    int hsyncWidth(CRTC6845& crtc) const override {
        int width = crtc.registers[3] & 0x0f;
        return width ? width : 16;
    }
    int vsyncWidth(CRTC6845&) const override { return 16; }
    // ACCC §14.6 (p.142): "When R3=0, CRTC's 0 and 1 do not produce HSYNC (and
    // therefore no interruption). On CRTC's 2, 3 and 4, it is impossible not to
    // generate HSYNC if the C0=R2 condition is satisfied. A value of 0 in R3 will
    // generate a HSYNC of 16 μsec, unless it is interrupted by modifying R3 during
    // HSYNC." (The interruption is CRTC 1's alone — see cancelsHsyncOnZeroWidth.)
    bool suppressesHsyncStartOnZeroWidth() const override { return false; }
    int hsyncBlackStartPixel() const override { return 3; }        // ACCC §14.7.1 (4th)
    int hsyncBlackStartPixelJit() const override { return 7; }     // ACCC §14.7.1 (8th)
    // ACCC §9.3.4.4 (p.65): "On CRTC 2, the HSYNC stops on the last Pixel-M2 of the
    // HSYNC and allows the visualization of an additional pixel."
    // MEASURED on SHAKER BI/C against AmSpiriT 2.0: the black ends on Pixel-M2 310,
    // as on CRTC 0, and the additional pixel follows it; 6 ended it two short (308).
    int hsyncBlackEndPixelJit() const override { return 8; }
    int displayRestorePixelAfterHsync() const override { return 0; }   // ACCC §16.2.2
    bool considersVsyncAtAnyRaster() const override { return true; }   // ACCC §16.4.3
    bool usesLastFrameLineLatch() const override { return true; }     // ACCC §10.3.3
    bool hasSeparateAdjustCounter() const override { return true; }   // ACCC §11.1
    bool vsyncStartIsGhost(CRTC6845& crtc, bool written) const override {
        // ACCC §16.4.3: "If the VSYNC condition occurs during a HSYNC from C0=R2 to
        // C0=R2+R3 (1 usec longer than the visual size of the HSYNC) then the CRTC
        // generates a GHOST VSYNC" -- the pulse, and the character C3 ends it on. Counted
        // by C3, not by C0, so the window wraps past R0 with the pulse: §15.4.4's chart
        // (p.155) has R2=50 with R3=13 ending on C0=63 and a real VSYNC at C0=0, and R3=14
        // ending ON C0=0 and "No Vsync". (Comparing C0 against R2..R2+R3 asked the line-
        // start question while C0 still read 63, and took R3=13 for a ghost.)
        // §15.4.4 (p.154) states the exception outright -- the window "will therefore
        // trigger a GHOST VSYNC, UNLESS R2=0" -- and §15.6 repeats it: "If R2 is programmed
        // with 0, the VSYNC condition is detected early enough to occur normally." Taken
        // as written: with R2=0 there is no ghost, even while an earlier, longer pulse is
        // still running over C0=0. (Narrowing it to "a pulse starting on C0=0" ghosted
        // SHAKER BR/D-F's VSYNC, and the monitor carried the half-line slip into BI/A-H,
        // where the photo and AmSpiriT show every line of the HSYNC box black.)
        if ((crtc.registers[2] & 0xff) == 0) return false;
        // The extra microsecond is the line-start case only. An R7 write that lands on
        // the character the pulse ended on is a VSYNC on the pin: SHAKER AI/D on the real
        // CRTC 2 (R2=46, R3=14, R7=C4 written on C0=3 onwards of C9=0 and 1, the PPI read
        // 5 usec later) reads off for writes on C0=46..59 and on from C0=60, on both lines.
        if (written) return crtc.hsync;
        return crtc.hsync || crtc.hsyncDroppedThisCharacter;
    }
    // ACCC §13.4 (p.114): on the MC6845 "the R8 SKEW DISP function does not exist".
    // The chip still shows 1 byte (0.5 µsec) of BORDER before C0=0, exactly as the
    // HD6845S does — the difference is that there is no skew control to work around
    // it with, so R8 bits 4-5 must not be honoured here.
    int displaySkew(CRTC6845&) const override { return 0; }
    bool replacesBorderR1WithR0() const override { return true; }   // ACCC §19.2.4
    bool anticipatesBorderByte(CRTC6845& crtc) const override {
        // ACCC §17.6.2 (p.187): "WHEN C0 REACHES R0 WITHOUT R1 HAVING BEEN REACHED,
        // CRTC's 0 and 2 send a 'BORDER ON' signal to the GATE ARRAY, which picks it up
        // immediately. These CRTC's are 'ahead' of the characters displayed by the GATE
        // ARRAY and send the BORDER signal 0.5 usec too early." The caller only asks
        // while the display is still on, so reaching R0 here IS "without R1 having been
        // reached" -- which covers R1>R0 and equally an R1 raised past C0 mid-line.
        // "Note 1: This behaviour remains true whatever the value of R0. If R0=0, then
        // the display alternates between 1 DISP ON byte, and 1 DISP OFF byte."
        if (crtc.horizontal == crtc.registers[0]) return true;
        // ACCC §18.3.2 (R6 CONFLICTS, CRTC 0 and 2): "When C4=R6=0 and C9=0 ... the
        // state of DISPLAY ENABLE changes to ON at the beginning of the CRTC
        // character and returns to OFF 0.5 usec later. In other words, on the first
        // line of the frame, there is an alternation of bytes of BORDER and
        // displayable characters (the video pointer continuing to count normally)."
        // The conflict exists only for R6=0; with R6>0 the C4=R6 test is clean.
        return crtc.registers[6] == 0 && crtc.vertical == 0 && crtc.raster == 0;
    }
    // ACCC §18.3.2: the R6=0 first-line conflict "becomes definitive" once C0 has
    // reached R1 while R6 is still 0 — before that a write of R6>0 cancels it.
    // ACCC §18.3.2: an R6 written equal to C4 on the first line of a frame (C4=C9=0) is the
    // cancellable conflict above, not the definitive border it is on every other line.
    // SHAKER AP/D ("T04 - 1ST LINE IN DISPLAY AREA: SEQUENCE R6=0/R6=8/ WHEN R1>R0") -- the
    // R6=8 cancels it, and C0 never reaches R1 to make it definitive.
    bool r6FirstLineWriteIsConflict() const override { return true; }
    bool r6ZeroConflictBecomesDefinitive(CRTC6845& crtc) const override {
        return crtc.registers[6] == 0 && crtc.vertical == 0 && crtc.raster == 0;
    }
    bool initialVerticalDisplay(CRTC6845&) const override { return true; }
    // The frame-end decision is the latched Last Line state of §12.4.1, consulted
    // through usesLastFrameLineLatch(); this plain equality is the fallback the
    // core uses outside that path.
    bool verticalTotalMatches(CRTC6845& crtc) const override {
        return crtc.vertical == (crtc.registers[4] & 0x7f);
    }
    // ---- ACCC §12.4.1 LAST LINE CONCEPT (CRTC 2) ----------------------------
    // "As on the CRTC 0, there is a concept of Last Line which irremediably arms
    //  the reset to 0 of C4 and C9 on the following line. When this Last Line
    //  state is set, it can no longer be modified."
    // It is evaluated at C0=0, or during an R4/R9 update while "Last Line
    // Management" is true. Management is true unless C4=0 and C9=0. A separate
    // "Last Previous Line" state is decided at the last HSYNC position.
    static bool atLastLine(CRTC6845& crtc) {
        return crtc.vertical == (crtc.registers[4] & 0x7f)
            && crtc.raster == crtc.maximumRaster();
    }
    // ACCC 1.11 §12.4.1 (p.96): the C0==0 evaluation alone uses the PREVIOUS R9.
    // "For instance, if C4==R4==38 and C9==R9==7 at the start of C0==0, updating R4
    // to 10 on position C0==0 will cause C4<>R4, setting the Last Line state to
    // false. Conversely, modifying R9 on C0==0 will have no immediate effect."
    static bool atLastLineAtLineStart(CRTC6845& crtc) {
        return crtc.vertical == (crtc.registers[4] & 0x7f)
            && crtc.raster == (crtc.r9Previous & 0x1f);
    }
    // The whole C0==0 evaluation, re-runnable when R4 is written on that position.
    static void evaluateAtLineStart(CRTC6845& crtc) {
        if (crtc.hsyncOnLineStart) { crtc.lastFrameLine = false; return; }   // HSYNC on C0==0
        if (atLastLineAtLineStart(crtc)) {
            crtc.lastFrameLine = !crtc.lastPreviousLine;
            crtc.lastLineManagement = false;
        } else {
            crtc.lastFrameLine = false;
            crtc.lastLineManagement = !(crtc.vertical == 0 && crtc.raster == 0);
        }
    }
    void onRegisterWritten(CRTC6845& crtc, int reg) const override {
        // ACCC §11.9 (p.92): "On CRTC 2, if the interlace mode is disabled (R8=0) while
        // the 'Interlace' line is displayed, then the 'Last Line' condition is
        // cancelled. The current line is no longer considered as an interlace line. C9
        // then continues to count to R9, and C4 is increasing if it is different from
        // R4." Leaving the adjustment here does exactly that: the next line-end takes
        // the ordinary C9/R9 path again.
        if (reg == 8 && (crtc.registers[8] & 1) == 0
            && crtc.verticalAdjust > 0 && crtc.adjustmentInterlaceExtra != 0
            && crtc.adjustLine >= (crtc.registers[5] & 0x1f)) {
            crtc.verticalAdjust = 0;
            crtc.lastFrameLine = false;
            return;
        }
        // ACCC §19.6.3 (p.218): "There is a noticeable bug on the management of the
        // additional line. If the IVM mode is activated on the first line of an odd
        // frame, then this line will become an additional line, and a new line 0 will
        // follow the old line 0... This is true whatever the value of C0 (0 to R0). ...
        // If the parity was odd when the IVM mode is activated on line 0, this line,
        // now considered as an additional line, IMMEDIATELY BECOMES ODD. Thus the C9
        // displayed as soon as R8=3 on this line will be odd (i.e. C9=1)."
        if (reg == 8 && (crtc.registers[8] & 1) != 0 && (crtc.r8Previous & 1) == 0
            && crtc.vertical == 0 && crtc.raster == 0 && (crtc.interlaceField & 1) != 0) {
            crtc.interlaceLineNow = true;
            crtc.rasterParity = 1;
            return;
        }
        if (reg != 4 && reg != 9) return;
        // On position C0==0 the line-start evaluation is simply re-run: R4's new
        // value counts, R9's does not (§12.4.1). This is the one place the state can
        // still go back to false, because the evaluation itself has not finished.
        if (crtc.horizontal == 0) {
            if (reg == 4) evaluateAtLineStart(crtc);
            return;
        }
        // "The Last Line Management state allows, if it is true, to evaluate the
        //  conditions of the Last Line state on positions C0>0 from an update of R9
        //  and/or R4. There is an exception to this rule, if the update that
        //  satisfies the Last Line condition occurs during a HSYNC (the Last Line
        //  state remains false)."
        if (!crtc.lastLineManagement || crtc.hsync) return;
        // ...and, as at C0==0, not when "the previous line was itself a last line" (p.96):
        // "a line that remains continuously a 'Last Line' cannot be recognized indefinitely
        // as such" (p.97). SHAKER CRETURN/A on the real CRTC 2 ("R3=15 ON C4=R4/C9=R9 (7),
        // THEN R9=0/7 TO FORCE LAST LINE EVAL"): the R9 pulse does not end the frame, C4
        // overflows and the next VSYNC is missed (5E,5E); ours ended it (5E,5F).
        if (atLastLine(crtc) && !crtc.lastPreviousLine) crtc.lastFrameLine = true;
    }
    void onCharacterPosition(CRTC6845& crtc, int c0) const override {
        // "During a HSYNC, a test is performed on position C0=R2+R3-1, in order to
        //  determine if line N is a last line for line N+1 (at C0=0). Thus, if
        //  C4=R4 and C9=R9, then Last Previous Line is true, otherwise it is false.
        //  Furthermore, if C4<>R4 or C9<>R9 on this last position of the HSYNC, this
        //  updates the Last Line Management state by setting it to true."
        // ACCC 1.10 §12.4.1: "This position corresponds to C0=R2+R3-1, WITHIN THE
        // LIMIT OF R0, as a HSYNC can overflow onto the following line."
        // ACCC §12.4.1 "Absence of HSYNC" (p.98): "If no HSYNC occurs during a line
        // (R2>R0), the 'Previous Last Line' state is not updated and the 'Last Line
        // Management' state cannot be re-evaluated by the mechanism normally executed
        // on the last character of the HSYNC. Thus, if a 'Last Line' condition has
        // already been activated before the suppression of the HSYNC, this state can
        // remain active indefinitely."
        // A HSYNC that runs past R0 has its last character on the next line: "C0=R2+R3-1,
        // within the limit of R0, as a HSYNC can overflow onto the following line" counts
        // C0 round R0, it does not stop at it. (R2=50, R3=15, R0=63: C0=0 of the next line,
        // where that line's own C4/C9 are compared; SHAKER CRETURN/A group 7, above.)
        int hsyncEnd = (crtc.registers[2] + (crtc.registers[3] & 0x0f) - 1) & 0xff;
        while (hsyncEnd > crtc.registers[0]) hsyncEnd -= crtc.registers[0] + 1;
        if (crtc.registers[2] <= crtc.registers[0] && c0 == hsyncEnd) {
            bool last = atLastLine(crtc);
            crtc.lastPreviousLine = last;
            if (!last) crtc.lastLineManagement = true;
        }
        if (c0 != 0) return;
        // The C0==0 evaluation sees R9 as it stood when the character opened (r9Previous,
        // snapshotted in tickCharacter); a write landing later in this same microsecond
        // is too late for it (§12.4.1).
        // "If C4=R4 and C9=R9 on position C0=0, then the Last Line state is true (the
        //  Last Line Management state is false). However, there are 2 exceptions for
        //  which the Last Line state is false: if the previous line was a last line;
        //  if a HSYNC takes place on position C0=0."
        // "If C4<>R4 or C9<>R9 on position C0=0, then the Last Line state is false.
        //  The Last Line Management state is true unless C4=0 and C9=0."
        evaluateAtLineStart(crtc);
    }
    // ACCC §19.8.3 (p.227-228): on this chip IVM does NOT double C9 — "C9 is compared
    // with R9 in a conventional way to process C4", and a separate C9.IVM drives the
    // address. "C9.IVM is reset twice during a C4 character. Once when C9.IVM reaches
    // R9 (out of parity) and once when C9 reaches R9/2", which makes its period
    // (R9/2)+1. Worked example from p.228 (C4=1, R9=6, R8=3): C9 counts 0..6 whatever
    // the parity, C9.IVM runs 0,1,2,3,0,1,2 and C9.VMA is 0,2,4,6,0,2,4 on even frames
    // and 1,3,5,7,1,3,5 on odd ones.
    bool interlaceVideo(const CRTC6845& crtc) const override { return false; }
    // §19.8.3's counter, stepped beside C9 at every line end, verbatim:
    //     If (C9 == R9) Then C9=0 ; Management of C4 ; C9.IVM=0
    //     Else If (C9==R9/2) Then C9.IVM=0 ; C9=C9+1 ...
    //          Else C9=C9+1 ; C9.IVM=C9.IVM+1
    // It used to be derived as C9 % (R9/2+1), which is the same thing only while R9 has
    // not moved since the character began: an R9 rewritten mid-character leaves the
    // counter where it was, the derivation jumps (cpcse-logic-check's MC6845 reference,
    // R9=3 written on C9=4 of an R9=7 IVM frame).
    bool advanceVertical(CRTC6845& crtc) const override {
        const int c9 = crtc.raster & 0x1f, r9 = crtc.registers[9] & 0x1f;
        const bool newFrame = crtc.updateVerticalGeneral();
        if (newFrame || c9 == r9 || c9 == (r9 >> 1)) crtc.ivmRaster = 0;
        else crtc.ivmRaster = (crtc.ivmRaster + 1) & 0x1f;
        return newFrame;
    }
    int videoRasterIvm(CRTC6845& crtc) const override {
        if (crtc.displayMode() != 3) return -1;
        // "Si IVM ON Then C9.VMA=(C9.IVM*2) or Parity".
        return ((crtc.ivmRaster * 2) | (crtc.interlaceField & 1)) & 0x1f;
    }
    bool rasterMatches(CRTC6845& crtc) const override {
        return crtc.oldInterlaceVideo()
            ? (crtc.raster | 1) == crtc.maximumRaster()
            : crtc.raster == crtc.maximumRaster();
    }
    int dispenSkew(CRTC6845&) const override { return 0; }   // no SKEW DISP on MC6845 (§13.4)
    // ACCC §14.4's table (p.134), the R3.NJIT row for this chip, gives each cell as a
    // range: 0.0625-0.1250 / 1.1250-1.1875 / 2.0625-2.1250 / 3.0625-3.1250 µsec for
    // R3l = 2 / 3 / 4 / 5. §9.3.1 (p.52): at R3l=2 "On CRTC 2 it lasts 1 or 2 pixel-M2".
    // 2/16 µsec after the boundary is inside every cell and both pages, the R3l=3 cell
    // (1/16 later than the others) included.
    int hsyncFallPhase16(int) const override { return 2; }
    bool frameAddressComesFromVmaPrime() const override { return true; }   // ACCC §13.4
    bool skipsFrameDisplayRestoreDuringHsync() const override { return true; }     // §15.5.2
    // ACCC §16.4.3 (p.171): "if the HSYNC overflows on C0=0, then C4 overflows on the
    // last line of the frame AND THE BORDER REMAINS ACTIVATED"; §15.5.2 restores the
    // display at C0=0 only "if C4 has never reached R6"; §18.2.2 (CRTC 0, 2): the R6
    // BORDER holds "until the next frame". A runaway C4 wrapping through 0 is not a new
    // frame. MEASURED on SHAKER B4/F-H (R0=3, R4=0, HSYNC on C0=0 of C9=7, so C4 runs
    // away): ours restored the display on every wrap and drew RAM where the real CRTC 2
    // and AmSpiriT show BORDER only (the caption: "SCREEN KO ON REAL CRTC 2").
    bool restoresDisplayOnRunawayWrap() const override { return false; }
    bool assignsVmaPrimeBeforeVma() const override { return true; }                // §17.4.3
    // ACCC §19.8.3 (p.227): "The specific management of assignment of VMA' with VMA when
    // C0==R1 is only processed when R8 is equal to 3. When R8 is equal to 0 or 2, this
    // assignment takes place only when C9==R9. In IVM mode, VMA' CAN BE UPDATED
    // WITHOUT C4 INCREMENTING" — C9.IVM wraps twice per C4 character and each wrap
    // takes VMA', which is what "the video address is updated 2 times for each value
    // of C4" means when R9 is odd.
    // p.228: "C9.IVM is reset twice during a C4 character. Once when C9.IVM reaches R9 (out
    // of parity) and once when C9 reaches R9/2. If R9 is even, C9 reaches R9 before C9.IVM
    // reaches R9 (out of parity) and the VMA' video pointer is not transferred."
    static bool takesVmaPrime(CRTC6845& crtc) {
        if (crtc.displayMode() == 3) return crtc.ivmRaster == ((crtc.registers[9] & 0x1f) >> 1);
        return crtc.rasterMatchesMaximum();
    }
    // ACCC §11.3 (p.86): the end of the R5 count hands the frame back to the Last Line
    // state -- "The 'latest line' state is then processed correctly and led to reset C4"
    // -- and §12.4.1 (p.94) says the same of any adjustment: "it will return to 0 once
    // the additional line handling is complete, as this process sets the 'Last Line'
    // state". So the LAST adjustment line is a last line for §20.3.3's VMA'=R12/R13,
    // which is how the frame after the R5 lines starts on R12/R13 at all: VMA' has been
    // taking VMA at every C9=R9 in between (§11.2.3). SHAKER BRETURN (module B "R5
    // STORIES") writes R12 after C0=R1 of the frame's last line; the photographed CRTC 2
    // shows the next frame from that R12, 24 adjustment lines later.
    // The line is the last one when C5+1 reaches R5 (§11.3.1) and §11.9's interlace line
    // does not follow it -- or is the interlace line itself.
    bool endsAdjustment(CRTC6845& crtc) const {
        if (crtc.verticalAdjust != 1) return false;
        if (crtc.adjustmentInterlaceExtra != 0) return true;
        return !((crtc.displayMode() & 1) && addsInterlaceLine(crtc));
    }
    // ACCC §20.3.3 (p.244): "if R1 is 0 on the last line of the frame, VMA'= VMA AT THE
    // BEGINNING OF THIS LINE", the line whose own C9 is R9. Before the counters step,
    // C9 is still the previous line's, so only the R12/R13 branch (which §17.4.3 ties to
    // the last-line state still in force) is decided here. Taking VMA'=VMA on the old
    // C9 moved a row 63 characters on whenever R1=0 survived into the line after C9=R9:
    // SHAKER AO/G (R1=0 on C0=&3E/&3F) lost its checker row on CRTC 2.
    void reloadNextRowAddressBeforeLineStart(CRTC6845& crtc) const override {
        if (frameEndsHere(crtc) || endsAdjustment(crtc)) reloadNextRowAddress(crtc);
    }
    // The frame's own last line (C4=R4/C9=R9) loads VMA' with R12/R13 only when the frame
    // ends there. When R5 lines follow, they are the frame's last lines, not this one:
    // ACCC §10.3.1.2 / §11.2.2 (p.82): "When the additional management becomes active, it
    // cancels the Last Line state", and §11.3 (p.86) gives the state back at the end of the
    // R5 count (endsAdjustment). Here the R5 count is read live (§11.4.1: "R5 management is
    // considered on each C0 position"), as the line end that opens the adjustment reads it.
    // MEASURED on SHAKER AP/J (CRTC 2, "R6=0/FF FROM C0=2 IN V.ADJ ZONE (R5=16) ...
    // PREVIOUS R6=R4+3"): the real CRTC 2 (and AmSpiriT 2.0, lite, CPCEC) show the 16
    // adjustment lines as blank RAM after the last row; ours showed the frame's first two
    // text rows there again (VMA' had taken R12/R13 one block early).
    static bool frameEndsHere(CRTC6845& crtc) {
        return crtc.lastFrameLine && (crtc.registers[5] & 0x1f) == 0;
    }
    bool takesOffsetWriteOnR1Character() const override { return true; }   // ACCC §20.3.3
    void reloadNextRowAddress(CRTC6845& crtc) const override {
        // ACCC §20.3.3: "VMA' is a transient pointer updated with VMA when C0 reaches
        // R1 (and C9=R9). It allows to move forward in the video ram when all the
        // lines of a C4 character have been displayed. When the conditions of the
        // last line of the frame are met (C4=R4/C9=R9 at the start of the line on
        // C0=0), VMA'=R12/R13 (and not VMA'=VMA)."
        // lastFrameLine is exactly that start-of-line state (§12.4.1).
        // ACCC §12.4.2 Note 1 (p.99): "This assignment of R12/R13 depends on the 'last
        // line' state when C0==R1 (and not on the equality C4==R4 and C9==R9 which
        // positions this state). This implies that changing R9 before equality C0=R1
        // does not prevent this assignment." So the R12/R13 branch is NOT gated on the
        // raster match — only the ordinary VMA'=VMA step is.
        if (frameEndsHere(crtc) || endsAdjustment(crtc)) {
            // ACCC §17.4.3 (p.185): "There is indeed a bug when updating VMA' with
            // R12/R13 at position C0=0. To update VMA' with R12/R13, the CRTC in
            // principle carries out two logical operations: VMA'=VMA' AND (R12 x 256 +
            // R13), VMA'=VMA' OR (R12 x 256 + R13). In the situation described above,
            // only the first logical operation (AND) is performed."
            if (crtc.horizontal == 0 && crtc.registers[1] == 0)
                crtc.nextRowAddress = crtc.nextRowAddress & crtc.requestedAddress & 0x3fff;
            else
                crtc.nextRowAddress = crtc.requestedAddress & 0x3fff;
            return;
        }
        if (!takesVmaPrime(crtc)) return;
        // "VMA' is a transient pointer updated with VMA" -- the MA counter itself, not the
        // row start plus C0. They part once C0 has overflowed through 255 (§17.1) and met
        // R1 a second time: MA has run 256 characters further (the fix CRTC 1 had,
        // found here by cpcse-logic-check's MC6845 reference on an R0 lowered below C0).
        crtc.nextRowAddress = crtc.maRow & 0x3fff;
    }
};

const CrtcBehaviour& crtcType2() { static CrtcType2 instance; return instance; }

} // namespace cpcse
