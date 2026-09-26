// CPCSyntaxError — CRTC type 0 profile.
// HD6845S register masks, status and normalization rules.
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
#include "crtc.h"

namespace cpcse {

// ACCC §4.3 GENERAL VIEW OF THE REGISTERS (p.22) gives the used bits per chip.
// R8 on this chip is "c c d d - - i i": cursor blink 7-6, DISPTMG skew 5-4,
// interlace 1-0, with bits 3-2 unused — 0xf3, not 0x3f.
static const int WRITE_MASK[16] = { 0xff, 0xff, 0xff, 0xff, 0x7f, 0x1f, 0x7f, 0x7f,
    0xf3, 0x1f, 0x7f, 0x1f, 0x3f, 0xff, 0x3f, 0xff };

struct CrtcType0 : CrtcBehaviour {
    CrtcType0() { id = 0; name = "HD6845S"; }
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
    // Both instructions, with no exemption. §13.7.1 (p.125) puts the OUTI /
    // OUT(C),reg8 split on CRTC 1 alone -- "a difference in internal processing time
    // at the level of the Z80A instructions, as well as an internal processing phase
    // shift between THIS CRTC AND CRTCs 0 AND 2" -- and §13.3 note 3's C0 overflow,
    // the only one in the compendium, is written about that chip's OUTI. §13.2 (p.104)
    // is this chip's own R0 chapter and names neither.
    bool horizontalCounterAtTotal(CRTC6845& crtc) const override {
        if (crtc.horizontalTotalMatch && crtc.horizontal > (crtc.registers[0] & 0xff)) return true;
        return crtc.horizontal == crtc.registers[0];
    }
    int writeMask(int reg) const override { return (reg >= 0 && reg < 16) ? WRITE_MASK[reg] : 0; }
    int normaliseRegister(int reg, int value) const override {
        // R0=0 is a real, distinct CRTC0 state ("1 µsec screens", §13.2.1) — it is
        // NOT silently promoted to 1; see blocksRasterAdvance/ignoresRegisterWrite.
        return value & writeMask(reg);
    }
    // ACCC §21.2.1 (p.246): "CRTC 0: Only the 5 least significant bits of the register
    // number are considered"; §4.3/§28.1.9 (p.294): R12-R17 are the readable ones, R12,
    // R14 and R16 six bits wide. §21.3.2 (p.248): no status register -- "My CPC CRTC 0
    // randomly returns 255 or 127 on this port" (&BE00). frame-check's 176 register-read
    // cases hold this.
    int read(CRTC6845& crtc, int operation) const override {
        if (operation == 2) return 0xff;
        int reg = crtc.selected & 0x1f;
        if (reg < 12 || reg > 17) return 0;
        if (reg == 12 || reg == 14 || reg == 16) return crtc.registers[reg] & 0x3f;
        return crtc.registers[reg] & 0xff;
    }
    // §14.6 (p.142): "When R3=0, CRTC's 0 and 1 do not produce HSYNC" -- so R3l is the
    // width as it stands, 0 included.
    int hsyncWidth(CRTC6845& crtc) const override { return crtc.registers[3] & 0x0f; }
    // §14.2 (p.132) / §16.2.1 (p.160): the VSYNC length is R3h on CRTC 0, 3 and 4 --
    // "fixed at 16 for CRTC's 1 and 2 (and for CRTC's 0, 3, 4 when R3h=0)".
    int vsyncWidth(CRTC6845& crtc) const override { return (crtc.registers[3] >> 4 & 0x0f) ? (crtc.registers[3] >> 4 & 0x0f) : 16; }
    // §19.2 (p.194) SKEW-DISPTMG, R8 bits 5-4: "only available on CRTC's 0, 3 and 4".
    int displaySkew(CRTC6845& crtc) const override { return crtc.registers[8] >> 4 & 3; }
    bool honoursDisplaySkewField() const override { return true; }   // ACCC 19.1
    bool skewIsDelayLine() const override { return true; }            // ACCC §19.2.4
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
        // ...undelayed only: with a SKEW DISP delay the substituted BORDER is a whole
        // character `skew` characters later (§19.2.4, p.196), carried by the delay line.
        const int skew = crtc.registers[8] >> 4 & 3;
        if (crtc.horizontal == crtc.registers[0] && (skew == 0 || skew == 3)) return true;
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
    bool verticalTotalMatches(CRTC6845& crtc) const override { return crtc.vertical == (crtc.registers[4] & 0x7f); }   // §12.2
    // ---- ACCC §19.8.1 (p.220) COUNTING IN INTERLACE VIDEOMODE, CRTC 0 ----------
    // "When IVM mode is activated, counter C9 CONTINUES TO INCREMENT NORMALLY.
    //  However, when C9 is used in building up the VMA address, C9 is considered
    //  shifted left by 1 bit, and bit 0 represents parity... The increment management
    //  of C9 continues to be processed on the normal value of C9."
    // So C9 is NOT doubled on this chip — only the address it feeds is.
    bool interlaceVideo(const CRTC6845&) const override { return false; }
    int videoRasterIvm(CRTC6845& crtc) const override {
        if (crtc.displayMode() != 3) return -1;
        // ACCC §19.8.1 (p.220): "When R8 changes to 3, a status indicates that the
        // calculation of the value compared to 'R9 or ParityFrame' will be performed on
        // the next C0=0, after the C9/R9 test of the line. THIS IS MOST CERTAINLY DONE
        // IN THIS WAY TO PREVENT THE C9 USED FOR THE DISPLAY FROM SWITCHING MID-LINE."
        // So the doubling waits for the next line as well: p.222's chronograms give the
        // line R8 goes to 3 a C9-VMA equal to plain C9, and only the line after it
        // (C9x2)|ParityC9. ivmTestDeferred spans exactly that window.
        if (crtc.ivmTestDeferred) return -1;
        // "C9.VMA=(C9 x 2) or ParityC9", five bits: these are the RA0-RA4 pins, and
        // p.222's own chronogram runs C9-VMA up to 31 when C9 overflows. The CPC takes
        // bits 0-2 of it for the address (§20.2); videoRaster() does that fold, not this.
        // A "& 7" here put RA=1 on the pins where the chip drives 9.
        return ((crtc.raster * 2) | (crtc.rasterParity & 1)) & 0x1f;
    }
    // "If ((C9 x 2) or ParityFrame) == (R9 + ParityFrame)" — the doubled C9 carrying
    // the frame parity is what is tested against R9, not C9 itself.
    bool rasterMatches(CRTC6845& crtc) const override { return c9AtR9(crtc, crtc.maximumRaster()); }
    // The C9/R9 comparison, against a given R9 -- the register as it stands, or as it stood
    // before a write (§10.3.1's R0-position rule needs both).
    static bool c9AtR9(CRTC6845& crtc, int r9Value) {
        if (crtc.displayMode() == 3) {
            const int r9 = r9Value & 0x1f;
            // "Thus, on the line where R8 goes to 3, it is C9 (AND NOT C9.VMA) which is
            // compared to 'R9 or ParityFrame'. If C9=R9 and the parity is odd, then the
            // test C9=R9+1 is false: C9 is not reset to 0 and overflows."
            if (crtc.ivmTestDeferred) {
                const int parity = crtc.interlaceField & 1;
                return (crtc.raster & 0x1f) == ((r9 + parity) & 0x1f);
            }
            // The character ends on C9.VMA = R9 + (ParityC9 xor R9.0). §19.8.1's summary
            // formula, "((C9 x 2) or ParityFrame) == (R9 + ParityFrame)", is this for an
            // EVEN R9 -- ParityC9 is ParityFrame all frame long then -- and p.222's tables
            // (R9=6) agree with both. For an ODD R9 the summary can never be met, while
            // §19.5.2's own table (p.207) has the chip ending every character: "R9 = 7.
            // On an even frame with C4 = 0, the CRTC generates 5 even lines (0.2.4.6.8)
            // followed by 4 odd lines (1.3.5.7)", with ParityC9 = C4.0 xor ParityFrame --
            // even-parity characters end at C9.VMA 8, odd ones at 7. The detailed chapter
            // wins (it is the one that "balanced the number of lines between 2 frames").
            const int parityC9 = crtc.rasterParity & 1;
            const int vma = ((crtc.raster * 2) | parityC9) & 0x1f;
            return vma == ((r9 + (parityC9 ^ (r9 & 1))) & 0x1f);
        }
        // ACCC §19.8.1 (p.221): on the line where R8 has just LEFT IVM, and until C0
        // returns to 0, "the limit test is therefore carried out with 'C9x2+ParityFrame'
        // (C9.VMA) and R9 (WITHOUT PARITY)".
        if (crtc.ivmExitDeferred) {
            int c9vma = ((crtc.raster * 2) | (crtc.rasterParity & 1)) & 0x1f;
            return c9vma == (r9Value & 0x1f);
        }
        return crtc.raster == (r9Value & 0x1f);
    }
    // "If R9.0==1 (C9 parity switched if R9 is odd) Then ParityC9 = C4.0 xor
    //  ParityFrame" — an assignment from the new C4, not a toggle.
    void updateRasterParity(CRTC6845& crtc) const override {
        if (crtc.displayMode() != 3 || (crtc.registers[9] & 1) == 0) return;
        crtc.rasterParity = (crtc.vertical & 1) ^ (crtc.interlaceField & 1);
    }
    int dispenSkew(CRTC6845& crtc) const override { return crtc.registers[8] >> 4 & 3; }  // §19.2: DISPEN comb (real 6845)
    // §13.2.1/§13.2.4 (p.105-106): "If R0=0, then C0 never reaches 1... and C9 can no
    // longer count. It then remains frozen with the value it had before R0=0."
    bool blocksRasterAdvance(CRTC6845& crtc) const override { return crtc.registers[0] == 0; }
    void onCharacterPosition(CRTC6845& crtc, int c0) const override {
        if (c0 == 2) crtc.vsyncAuthorized = true;        // validates the next C0=0 test
        else if (c0 == 0) crtc.vsyncAuthorized = false;  // state cancelled at C0=0
        // ACCC §13.2.4 (p.107): "if the conditions of a last line are satisfied on
        // C0=0 or C0=1, then C9 will no longer be managed with R9 but with R5. GOING
        // THROUGH C0=2 DOES NOT CANCEL IT. It will stop only when C9 computed=R5 on
        // C0=0." So while the adjustment runs, neither the C0<2 last-line comparison
        // nor the C0==2 R5 test touches the state — the C9/R5 count owns the line.
        if (crtc.verticalAdjust > 0) return;
        // ACCC 1.10 §10.3.1.2: "The value of C9 is compared with R9 when C0<2 (just as
        // C4 is compared with R4) to determine a state that indicates whether the
        // line was the last on the frame."
        if (c0 < 2) {
            crtc.lastFrameLine = atLastLine(crtc);
            if (c0 == 0) crtc.verticalAdjustState = false;   // a fresh line's decision
        }
        // ACCC §13.2.1 (p.105): R5 counts "before C0=3 (C0=0, 1 or 2)", so the value is
        // taken across that whole window and whatever stands at its close is what the
        // frame end uses. With R0<2 the window closes early, at C0=0 or 1, which is the
        // same edge the R.V.L.L. additional line is built on.
        if (c0 < 3) crtc.adjustLengthLatched = crtc.registers[5] & 0x1f;
        // "If vertical adjustment is active on the last line (R5>0 on C0==2), then the
        // vertical adjustment state becomes true and cancels the last line state."
        // With R0<2 this position never arrives, so the state stays active by
        // default — which is exactly what generates the R.V.L.L. additional line.
        if (c0 == 2) {
            // §13.7.2's deferred step lands here, before the R5 test, which is what
            // makes §13.7.2.2 work: "in this position, incrementing C4 WITHOUT C9
            // RETURNING TO 0 LEAVES THE ADDITIONAL MANAGEMENT ACTIVATED" -- so with
            // R5=0 the management still runs, and C9 counts on to R5 the long way
            // round ("C9 will increment to display lines 8 to 31, until it reaches R5").
            if (crtc.r0EnlargeStepsVertical) {
                crtc.r0EnlargeStepsVertical = false;
                crtc.vertical = (crtc.vertical + 1) & 0x7f;
                if (crtc.vertical == crtc.registers[6]) crtc.vDisplay = false;
                // Which of §13.7.2's two cases this is was settled at C0<2. On the frame's
                // last line (§13.7.2.2) the management stays active. Otherwise (§13.7.2.1)
                // "Additional management is automatically disabled on the C0=2 position
                // (C4<>R4) because R5 is 0... C9/C4 counting control remains classic" --
                // it used to stay active here too, and the line end then stepped C4 a
                // second time and reset C9 (SHAKER B7, "CRTC 0 : BUG OVF C4").
                if (crtc.lastFrameLine) {
                    crtc.verticalAdjustState = true;
                    crtc.adjustEngagedWithoutR5 = true;
                } else {
                    crtc.verticalAdjustState = false;
                }
                return;
            }
            crtc.verticalAdjustState = crtc.lastFrameLine && (crtc.registers[5] & 0x1f) > 0;
            if (crtc.verticalAdjustState) crtc.lastFrameLine = false;
        }
    }
    // The C0<2 comparison asks the same "is C9 at its maximum" question the line-end
    // asks, so it has to ask it the same way. ACCC §19.8.1 (p.220) makes that test
    // "If ((C9 x 2) or ParityFrame) == (R9 + ParityFrame)" in IVM, which a raw
    // C9==R9 can never satisfy — a legal IVM frame (§19.4.1's R9 = N-2, so R9=6 for an
    // 8-line character) never reaches C9=6 at all, so the last line was never
    // recognised and the frame ran away without end.
    static bool atLastLine(CRTC6845& crtc) {
        return crtc.vertical == (crtc.registers[4] & 0x7f) && crtc.rasterMatchesMaximum();
    }
    void onRegisterWritten(CRTC6845& crtc, int reg) const override {
        // ACCC §13.7.2 (p.126): enlarging R0 out of 1 lands in the middle of the C0=0/1/2
        // sequence. "If the update of R0 takes place when C0=1 and C9=R9, THIS CAUSES AN
        // OVERFLOW OF C4 without C0 and C9 being reset to 0, EVEN IF C4 IS NOT EQUAL TO
        // R4." The C0=R0 test (against the old value of 1) has already run and started
        // the additional management, while the new R0 is in time for C0's own increment.
        // §13.7.2.1 and §13.7.2.2 both place the step itself one character later: "the
        // consequence is then just an overflow of C4 FROM C0 = 2", and "as in the
        // previous case, C4 is incremented FROM C0=2. Additional management is
        // evaluated on the C0=2 position." So the write only arms it.
        if (reg == 0 && crtc.horizontal == 1 && crtc.r0Previous == 1
            && crtc.registers[0] > 1 && crtc.rasterMatchesMaximum()) {
            crtc.r0EnlargeStepsVertical = true;
            return;
        }
        // ACCC §10.3.1 (1.11 p.77): "when R9 is modified exactly at the position C0
        // equals R0, while C9 was equal to R9 before C0 reached R0 ... C4 is still
        // incremented", and C9 increments too because it no longer matches. The write
        // has already landed, so the OLD R9 comes from r9Previous (snapshotted as this
        // character opened) and the NEW one from the register.
        if (reg != 9 || crtc.horizontal != crtc.registers[0]) return;
        // The C4 half was decided as C0 reached R0, against the OLD R9; the C9 half is
        // taken at the line end against the new one (§10.2: "R9 update is considered
        // while C0<=R0"). p.77 states the one direction -- matched before, not now: C4
        // still steps and C9 steps too. The same latch gives the other: not matched
        // before, matched now -- C9 returns to 0 and C4 does NOT step.
        const bool before = c9AtR9(crtc, crtc.r9Previous);
        const bool now = crtc.rasterMatchesMaximum();
        if (before && !now) crtc.verticalStepForced = true;
        if (!before && now) crtc.verticalStepSuppressed = true;
    }
    void onLastLineRegisterWritten(CRTC6845& crtc, int reg) const override {
        // ACCC 1.10 §10.3.1.2, two rules that only exist in the 1.10 edition.
        // ACCC §12.2 (p.94): "Conversely, if the 'Last Line' state is true at the
        // beginning of the line (C0==0), it can be overridden if R4 or R9 is modified
        // to C0==0 so that C4 becomes different from R4 (or C9 becomes different from
        // R9)." The C0==0 comparison has already run, so re-derive it from the value
        // just written; unlike the C0>1 case this write CAN clear the state.
        if (crtc.horizontal == 0) {
            if (crtc.lastFrameLine && !atLastLine(crtc)) crtc.lastFrameLine = false;
            return;
        }
        if (crtc.horizontal == 1) {
            // "If the last line state is true at position C0==0 (therefore C9==R9 and
            // C4==R4), and R9 or R4 is updated at position C0==1 with a value
            // different from C9 or C4, then vertical adjustment becomes active and
            // the current line becomes the first adjustment line."
            bool differs = (reg == 9) ? (crtc.raster != crtc.maximumRaster())
                                      : (crtc.vertical != (crtc.registers[4] & 0x7f));
            if (crtc.lastFrameLine && differs) {
                crtc.verticalAdjustState = true;
                crtc.lastFrameLine = false;
            }
            return;
        }
        // ACCC 1.11 §10.3.1.2 (p.76) corrects the 1.10 wording of this rule: "If R9 or
        // R4 is modified when C0>1 without the vertical adjustment being active on the
        // position C0==2, WHILE C4 WAS EQUAL TO R4 AND C9 WAS EQUAL TO R9, then the
        // last line state REMAINS TRUE and it cannot become false again until the next
        // comparison of C4 and C9 (when C0 will again be 0 or 1)."
        // 1.10 read as though such a write could *establish* the state ("so that
        // C4==R4 and C9==R9, then the last line state is true"); 1.11 makes clear the
        // equality had to hold already — the write can only fail to clear a state the
        // C0<2 comparison had latched. lastFrameLine carries that comparison, and
        // nothing here clears it, so the rule is satisfied by doing nothing at all.
        (void)0;
    }
    // ACCC §17.1 (p.176): "the equality between C0 and R1 is used to update the video
    // pointer when C9=R9 (last line of a character)" -- VMA' takes the MA counter as it
    // stands at C0=R1. The row start plus C0 is the same number only while MA has stayed
    // in step with C0; on a line where C0 ran on past 255 without a line end (§17.1's
    // overflow, an R0 lowered behind C0) MA is 256 characters further on, and it is MA
    // the chip latches. Found by cpcse-logic-check's HD6845S reference: an R0 written
    // below C0 on an R9=0 frame put the next row 256 characters short (3398 for 3498).
    void reloadNextRowAddress(CRTC6845& crtc) const override {
        if (crtc.rasterMatchesMaximum()) crtc.nextRowAddress = crtc.maRow & 0x3fff;
    }
    bool usesLastFrameLineLatch() const override { return true; }   // §10.3.1.2
    bool suppressesContiguousHsync() const override { return true; }  // §15.3.1
    bool midLineVsyncAddsALine() const override { return true; }      // §16.4.1
    bool delaysVsyncOneLineInIvm(CRTC6845& crtc) const override {
        // §16.5: R9 odd, IVM mode (R8=3), odd frame, odd C4.
        return crtc.displayMode() == 3 && (crtc.registers[9] & 1) != 0
            && crtc.interlaceField != 0 && (crtc.vertical & 1) != 0;
    }

    // §13.2.2 (p.106): "Each time C0=2, a state validates the update of C4=R7 on the next
    // C0=0. This state is cancelled when C0=0." (§16.4.1.2: the VSYNC is then blocked.)
    bool allowsVsyncStart(CRTC6845& crtc) const override { return crtc.vsyncAuthorized; }
    bool incrementsVerticalEachAdjustLine() const override { return false; }   // §13.2.4
    bool adjustmentCountsRasterToR5() const override { return true; }          // §11.2.2
    bool settlesAdjustLengthBeforeC0Three() const override { return true; }    // §13.2.1
    // ACCC §16.2.1 (p.161): on this chip an R7.JIT starts the black on the 5th
    // Pixel-M2 of the word of the C0 where R7=C4 -- one character later than the
    // programmed-in-advance case -- "regardless of the instruction used".
    int vsyncBlackStartPixelJit(bool) const override { return 16 + 4; }
    // §13.2.4's note (p.105): "Since C9 management takes place only once if C0 does not
    // exceed 1, updates to registers R4, R5 and R9 are no longer considered as long as
    // R0=0. On the other hand, R8 continues to be considered each time C0=0."
    bool ignoresRegisterWrite(CRTC6845& crtc, int reg, int) const override {
        return crtc.registers[0] == 0 && (reg == 4 || reg == 5 || reg == 9);
    }
    bool generatesAdditionalLastLine(CRTC6845& crtc) const override {
        // R.V.L.L. (ACCC 1.10 §13.2.7): on 2µsec rupture lines (R0==1) with R9>0, C9
        // climbs to R9 and the chip then emits ONE extra line at C4=1/C9=0 with the
        // address reload suppressed. R0==0 (1µsec screens) is the separate C9-blocking
        // case and must not come here.
        // ACCC §12.2 (p.92) states the trigger for additional lines as: "R5>0 (or
        // R5=0 with R0<2), or if the Interlace mode is activated on even frames".
        // So on this chip it is R0<2 with R5=0 — no R9 qualifier. (R0=0 never
        // reaches here; blocksRasterAdvance freezes the counters first, §13.2.1.)
        return crtc.registers[0] < 2 && (crtc.registers[5] & 0x1f) == 0;
    }
};

const CrtcBehaviour& crtcType0() { static CrtcType0 instance; return instance; }

} // namespace cpcse
