// CPCSyntaxError — 6845 CRTC core.
// Runs character, raster, sync, address and type-specific display timing.
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
#pragma once
#include "common.h"
#include "lightgun.h"

namespace cpcse {

class CRTC6845;
struct RasterLine; // defined in raster.h; the CRTC only stores accessors.

// getSplit() result: { line, address } or null.
struct CrtcSplit { int line; int address; };

// ACCC §14.7.1 / §9.3.4.3: WHERE INSIDE A CHARACTER THE GATE ARRAY'S HSYNC BLACK SITS.
// A half-open range of Pixel-M2, 16..16 meaning "no black in this character". The zone
// does not begin or end on a character boundary, which is why it cannot be expressed as
// a per-character flag and why both renderers have to ask the same question here rather
// than each deciding for itself -- the legacy compositor answered it for no character at
// all, so every mid-line HSYNC lost its black.
//
//   now/before : the chip's HSYNC pin for this character and the one before it, already
//                delayed by hsyncDisplayDelayCharacters (§9.3.1: the ASIC's are 1 usec
//                behind CRTC 0, 1 and 2).
//   r2Jit      : R2 was written on this very character, which moves the start (§14.7.1).
//   endJit     : the HSYNC ended through an R3.JIT, which holds the black 0.25 usec
//                longer (§9.3.4.4/§14.5.4).
//   endLag     : the GATE ARRAY model's own addition to the end, in Pixel-M2
//                (GateArrayModel::hsyncBlackEndLag, §14.5.4 p.139/140).
//   cutFirstUs : an OUT(C),r8 wrote R3=0 on the HSYNC's first usec, so the pin was up
//                for part of this character only (§14.5.4, CrtcBehaviour::
//                r3ZeroFirstMicrosecondBlackEndPixel).
struct CrtcBehaviour;
void gaHsyncBlackWindow(const CrtcBehaviour* chip, bool now, bool before,
                        bool r2Jit, bool endJit, int endLag, bool cutFirstUs,
                        int& from, int& to);

// A CRTC silicon-type profile (crtc_type_*). Methods receive the
// live CRTC so they can consult its registers and counters.
struct CrtcBehaviour {
    int id;
    std::string name;
    virtual ~CrtcBehaviour() = default;
    virtual int writeMask(int reg) const = 0;
    virtual int normaliseRegister(int reg, int value) const = 0;
    virtual int read(CRTC6845& crtc, int operation) const = 0;
    virtual int hsyncWidth(CRTC6845& crtc) const = 0;
    virtual int vsyncWidth(CRTC6845& crtc) const = 0;
    virtual int displaySkew(CRTC6845& crtc) const = 0;
    // ACCC §19.1's register table: the SKEW DISP (Sd) field in R8 bits 5-4 exists on
    // CRTC 0, 3 and 4 only -- "these functions are only available on CRTC's 0, 3 and 4"
    // (§19.2). Const, so displayMode() can consult it.
    virtual bool honoursDisplaySkewField() const { return false; }
    virtual bool initialVerticalDisplay(CRTC6845& crtc) const = 0;
    virtual bool verticalTotalMatches(CRTC6845& crtc) const = 0;
    virtual bool rasterMatches(CRTC6845& crtc) const = 0;
    // Per-chip vertical counter/row state machine, advanced once per scanline.
    // Returns true if a new frame started. Default = the standard 6845 chain
    // (HD6845S/MC6845/ASIC); the UM6845R (type 1) overrides it with its own
    // r4Match/r9Match latch behaviour. Each silicon variant owns its counters.
    virtual bool advanceVertical(CRTC6845& crtc) const;
    // Per-chip register-write side effects (crtc.writeRegister). The UM6845R
    // latches r4Match/r9Match on writes to R4/R9; the Plus ASIC forces a raster
    // match on R9. Default: none.
    virtual void onRegisterWritten(CRTC6845& crtc, int reg) const {}
    // Per-chip R4/R9 write handling that needs the C0 position (ACCC 1.10 §10.3.1.2).
    virtual void onLastLineRegisterWritten(CRTC6845& crtc, int reg) const {}
    // Per-chip "row is displayable" test (crtc.displayOutputEnabled, after the
    // hDisplay/vDisplay guard). UM6845R gates on R6!=0; others on R8 skew bits.
    virtual bool displayEnabled(CRTC6845& crtc) const;
    // UM6845R suppresses the automatic frame-start Vsync when R4==0 && R5==0
    // (a degenerate 1-line frame). Default: never blocks.
    virtual bool blocksAutoVsync(CRTC6845& crtc) const { return false; }
    // Per-chip "raster reached its maximum" test. Default = forced match OR
    // rasterMatches(); the UM6845R uses its latched r9Match instead.
    virtual bool rasterMatchesMaximum(CRTC6845& crtc) const;
    // UM6845R reloads the start address on an extra scanline case (R5==0 ||
    // R4!=0) beyond the standard raster==base test. Default: no extra case.
    virtual bool reloadsStartAddressExtra(CRTC6845& crtc) const { return false; }
    // ACCC §11.6: an RFD lifts the C4=0 condition on the R12/R13 reload entirely.
    virtual bool reloadsStartAddressAnyRow(CRTC6845& crtc) const { return false; }
    // ACCC §13.4 (p.118, CRTC 2): "R12/R13 content is transferred to CRTC-VMA' when C0
    // reaches R1... CRTC-VMA' is transferred to CRTC-VMA at the beginning of the frame.
    // This implies that it is impossible to change offset if C0 does not reach R1."
    // So on the MC6845 the frame start takes VMA', not R12/R13 directly.
    virtual bool frameAddressComesFromVmaPrime() const { return false; }
    // UM6845R freezes the VMA'/start-address reload during an R1>R0 rupture
    // (ACCC §17.4.2). Default: always reloads.
    virtual bool freezesStartAddressReload(CRTC6845& crtc) const { return false; }

    // --- Plus ASIC (AMS40226, type 3/4) per-chip behaviour ---------------------
    // On an R0 write the ASIC catches up with >= (a lowered R0 wraps immediately);
    // real 6845s need an exact == on the next tick. Default: exact match.
    virtual bool horizontalCounterAtTotal(CRTC6845& crtc) const;
    // ACCC §13.3 note 3 (p.114): "when the OUTI instruction is used on CRTC 1 to modify
    // R0, THE COMPARISON OF C0 WITH R0 TAKES PLACE AFTER THE ASSIGNMENT of R0 with the
    // new value in some cases. Consequently, on the position where C0 should have gone
    // to 0, if R0 is modified on the last µsecond of the OUTI instruction, then C0 is
    // compared with the new value of R0, WHICH CAN LEAD TO AN OVERFLOW OF C0. Example:
    // If R0 was 49 and the 5th µsecond of the OUTI is at the position following C0=49,
    // and R0 is modified to 20, then C0 will not be equal to 0 but to 50." The UM6845R
    // alone; "an OUT(C),r8 landing on the same microsecond does not do this".
    virtual bool unwrapsC0OnBlockR0Write() const { return false; }
    // Writing R7 restarts Vsync immediately on real 6845s. ACCC §16.4.4 (p.171):
    // on CRTC 3/4 "VSYNC starts when C4=R7 and C9=C0=0. If R7 is modified with the
    // value of C4 while C0>0 and/or C9>0, it will not trigger CRTC VSYNC" — so the
    // ASIC honours the write only in the one character where both counters are 0.
    virtual bool startsVsyncOnR7Write(CRTC6845& crtc) const { return true; }
    // ACCC §16.3: the SECOND VSYNC protection mechanism — "check if the equality
    // between C4 and R7 has changed" — is the r7Match latch. "This second mechanism
    // was not renewed by the designers of CRTC's 3 and 4 for AMSTRAD. Even if the
    // equality C4=R7 has not changed (C4=R7=0 for example), a new VSYNC starts
    // immediately as soon as the current VSYNC ends."
    virtual bool hasVsyncReentrancyProtection() const { return true; }
    // ACCC §4.4.3 (p.26): the microsecond of an OUT (C),r8 at which the CRTC takes the
    // write, as a T-state offset from the instruction's start. "The CRTC misses the
    // I/O on the 3rd usec and retrieves it on the 4th" on the ASIC's of CRTC 3 and 4;
    // the real 6845s take it on the 3rd. Every other I/O form is the same on all five.
    virtual int outCWriteOffset() const { return 8; }        // 3rd usec
    // ACCC §16.2.3 (p.166): "If V26==2: SIG_GA_VSYNC=HIGH (and CRTC-VSYNC (on CRTC 3/4))",
    // and p.164: "On CRTCs 3 and 4, the VSYNC signal from the CRTC must be active for the
    // synchronization signal to be sent by the ASIC to the monitor... The C-VSYNC signal
    // starts at the end of the HSYNC-CRTC of line C9=1 and then stops when C0 returns to
    // 0." So on these chips the C-VSYNC is the V26 window AND the CRTC's VSYNC pin; on
    // the others the GATE ARRAY runs it off V26 alone ("the CRTC VSYNC signal state is no
    // longer used thereafter, except for CRTC's 3 and 4", §16.2.2).
    virtual bool cVsyncNeedsCrtcVsync() const { return false; }
    // ACCC §9.3.4 (p.54): where inside the byte the GATE ARRAY switches graphic mode.
    // "On CRTC's 0, 1 and 2, the GATE ARRAY switches the mode update to the 6th pixel
    // mode 2 (i.e. the 3rd pixel mode 1, the 2nd pixel mode 0). On the CRTC 4, the
    // GATE ARRAY switches the mode change to the 4th pixel mode 2 (i.e. the 2nd pixel
    // mode 1, the middle of the 1st pixel mode 0)." 0-based Pixel-M2 index.
    virtual int modeSwitchPixelInByte() const { return 5; }
    // ACCC §9.3.4.3 (p.58): "On CRTC's 0 and 2, the display is restored 1 Pixel-M2
    // before the GATE ARRAY changes the graphics mode... The first Pixel-M2 obtained
    // when the display is activated is only displayed for CRTC's 0, 2 and 4. CRTC 1
    // does not display this pixel." That pixel is NOT a hook: it is the gap between
    // hsyncBlackEndPixel() and modeSwitchPixelInByte() above, which comes out at 1 on
    // CRTC 0, 2 and 4 and 0 on CRTC 1 without being asked for.
    // "Classic" interlace-video mode (R8&3==3) — real 6845s double-scan; the ASIC
    // uses interlaceSyncAndVideo() instead, so it reports false here.
    virtual bool interlaceVideo(const CRTC6845& crtc) const;
    // ASIC-only interlace sync+video (raster doubling). Default: off.
    virtual bool interlaceSyncAndVideo(const CRTC6845& crtc) const { return false; }
    // ACCC §19.8.1 (CRTC 0) and §19.8.3 (p.227, CRTC 2): "In 'Interlace' mode, C9
    // is compared with
    // R9 in a conventional way to process C4. ANOTHER COUNTER, C9.IVM, is used for
    // displaying and managing video pointer updating... C9.VMA=(C9.IVM*2) or Parity."
    // On both chips C9 itself keeps counting by 1; only the address doubles. Returns the
    // C9.VMA to use, or -1 when the chip has no separate counter / IVM is off.
    virtual int videoRasterIvm(CRTC6845& crtc) const { return -1; }
    // ParityC9's per-C4-character update, which is different on every chip that
    // has one. ACCC §19.8.1 (p.220) CRTC 0: "If R9.0==1 (C9 parity switched if R9
    // is odd) Then ParityC9=C4.0 xor ParityFrame" — an assignment, not a toggle.
    // ACCC §19.8.2 (p.226) CRTC 1: "ParityC9 = ParityC9 xor (not r9.0)" — a toggle,
    // and on the opposite R9 parity. Called whenever C4 moves.
    virtual void updateRasterParity(CRTC6845& crtc) const {}
    // ACCC §19.6: which parity state gates the additional interlace line. CRTC 0 and
    // 2 add it when ParityR6 is odd (§19.6.1/§19.6.3); CRTC 1 when ParityFrame is
    // even (§19.6.2). The two are the same thing one frame apart.
    virtual bool addsInterlaceLine(CRTC6845& crtc) const;
    // ACCC §19.7.3 (p.219, CRTC 3/4): "In the particular case where R7=0... the
    // management of the VSYNC HAS PRIORITY over the assignment of ParityFrame. Thus,
    // if R7=0, the C4/R7 comparison is processed before ParityFrame switch. If
    // ParityFrame was odd, then there will be no MID-VSYNC." On CRTC 0, 1 and 2 it is
    // the other way round — "ParityFrame management takes priority" (§19.7.2).
    virtual bool vsyncPrecedesParitySwap() const { return false; }
    // ACCC §19.6/§19.5: CRTC 0, 1 and 2 take each frame's ParityFrame from ParityR6,
    // which freezes when C4 cannot reach R6. §19.8.4's ASIC algorithm has no ParityR6
    // at all — on the C4 wrap it simply does "ParityFrame = ParityFrame xor 1".
    virtual bool frameParityFromParityR6() const { return true; }
    // Per-chip Hsync counter advance (updateHsync body). Default: standard 6845.
    virtual void advanceHsync(CRTC6845& crtc) const;
    // Real 6845s suppress an Hsync start when the width nibble is 0; ASIC starts anyway.
    virtual bool suppressesHsyncStartOnZeroWidth() const { return true; }
    // ACCC §14.5.2: only CRTC 1 lets an R3l written as 0 DURING an HSYNC cancel it;
    // "on CRTC's 0 and 2 in this condition, HSYNC continues and 0 is treated as a
    // value to reach", and the ASIC starts an HSYNC on 0 at all (16 characters).
    virtual bool cancelsHsyncOnZeroWidth() const { return false; }
    // ACCC §15.1 (p.146): "The GATE ARRAY is faster to manage HSYNC than to display
    // characters read by CRTC's 0, 1 and 2. The HSYNC starts earlier and is visible on
    // the character preceding the one pointed by R2... THE ASIC's (CRTC's 3 and 4)
    // manage a HSYNC consistent with the C0 value displayed, delaying the display of
    // the HSYNC by 1 μsec." Connected to the same CTM, that puts the ASIC's picture
    // one character to the left.
    virtual int hsyncDisplayDelayCharacters() const { return 0; }
    // ACCC §15.1 (p.146): "The GATE ARRAY is faster to manage HSYNC than to display
    // characters read by CRTC's 0, 1 and 2. THE HSYNC STARTS EARLIER AND IS 'VISIBLE' ON
    // THE CHARACTER PRECEDING THE ONE POINTED BY R2, which has not yet been displayed...
    // If R2 is 10, on a CPC with CRTC (0, 1 and 2) then HSYNC starts from C0 displayed=9
    // (R2-1) over a length of R3 usec."
    //
    // So the black the GATE ARRAY puts up is drawn one character EARLIER than the
    // character the CRTC raised HSYNC on. The per-chip half of this is already carried by
    // hsyncDisplayDelayCharacters above -- the ASIC's "manage a HSYNC consistent with the
    // C0 value displayed, delaying the display of the HSYNC by 1 usec", which is this
    // lead cancelled -- so the lead itself is the same on every part.
    //
    // Measured against the chapter before it was changed: with the first visible character
    // at C0=R2+14 (§15.1's other rule, which displayOrigin already implements), our black
    // began on the character C0=R2 rather than R2-1.
    virtual int hsyncBlackCharacterLead() const { return 1; }
    // ACCC §14.4 (p.134): this chip's HSYNC pin does not FALL on a character boundary,
    // and where it falls differs per chip. The page tabulates the C-HSYNC durations the
    // GATE ARRAY ends up driving, in µsec, and they are not whole microseconds: with
    // R3l=5 the pulse is 3.1250 µsec on CRTC 1 and 3.0625 on CRTC 0 and 2, against
    // exactly 4.0000 for R3l=6 where the GATE ARRAY's own H06 count ends it instead.
    // Longshot spells out what that costs: "4-3.1250 = 0.875/2 = 0.4375 or 7 Pixels
    // (graphi mode 2) INSTEAD OF THE EXPECTED 8 PIXELS."
    //
    // So this is the residue, in 1/16 µsec, between the character boundary and the
    // moment the GATE ARRAY sees this chip's HSYNC drop. Each chip states its own; the
    // chapter tabulates CRTC 0, 1 and 2 only, so the ASIC's keep 0 until it does.
    virtual int hsyncFallPhase16() const { return 0; }
    // ACCC §27.7.2 (p.290), the same phase shift seen by the Z80A rather than by the
    // monitor. "CRTCs are not reliable concerning the management of their HSYNC signal.
    // In particular, the CRTC 1 ... the end of the HSYNC signal can occur more or less
    // late on a 1/16th MHZ scale." The INT the GATE ARRAY raises off that edge is late by
    // the same fraction, so it misses the Z80A's sampling point when the edge falls on an
    // instruction's own end, and "the Z80A can therefore generate additional instruction
    // before generating its interruption."
    //
    // It is a property of a DISCRETE 6845 driving the GATE ARRAY across a board. On the
    // Plus and the GX4000 the ASIC contains both halves and hands the edge over on its
    // own grid, so there is no residue and no missed sample. SHAKER module D's DI test
    // prints exactly that split in its own expected values -- "CRTC 3+4:#58 / OTHERS:#59"
    // under the columns "HSYNC OFF on 0/16Mhz" and "1-2/16Mhz".
    virtual bool hsyncEndMissesInterruptSample() const { return true; }
    // ACCC §16.2.3 (p.165), the other end of the same phase shift. "The GATE ARRAY clocks
    // the CRTC via a clock signal CLK whose period is 5 x 0.0625 usec high and 11 x
    // 0.0625 usec low. When the CRTC signals a start or an end of HSYNC, THERE ARE DELAYS
    // OF 1 OR 2 PIXEL-M2 before and after this clock signal, WHICH DEPEND ON THE TYPE OF
    // CRTC... These shifts between the active edges of the signals lead to a phase shift
    // between the start of the HSYNC signal received by the GATE ARRAY and the start of
    // the count to activate the C-HSYNC signal after 2 usec." And then the value:
    //
    //   "The C-HSYNC signal becomes active (low state) 1 OR 2 PIXEL-M2 BEFORE THE END OF
    //    THE 2 us if R3l>=2."
    //
    // The monitor's horizontal reference is that edge, so this is not a detail of the
    // sync's shape: everything measured from it moves with it. Pixel-M2 BEFORE the 2 usec
    // boundary, so 1 puts the edge at 31 rather than 32.
    virtual int cHsyncRiseAdvance16() const { return 1; }
    // ACCC §14.7.1 (p.142): when R2 was programmed BEFORE C0 reached R2, "the HSYNC
    // black zone does not start exactly on a character boundary, and not at the same
    // position according to the CRTC's": CRTC 0 "from the start of the 5th [Pixel-M2],
    // the display of half of the 4th", CRTC 1 "the 6th", CRTC 2 "the 4th... half of the
    // 3rd". 0-based index within the character the GATE ARRAY is displaying.
    virtual int hsyncBlackStartPixel() const { return 4; }
    // ACCC §14.7.1, the R2.JIT case (R2 updated by an OUT(C),r8 exactly on C0=R2):
    // "CRTC 0, 1: the non-display starts from the start of the 9th mode 2 pixel after
    // the start of the displayed CRTC R2-1 character... CRTC 2: from the start of the
    // 8th". The ASIC defers its HSYNC anyway, so R2.JIT moves nothing there (§14.7.2).
    virtual int hsyncBlackStartPixelJit() const { return 8; }
    // Where the per-line HSYNC black zone ENDS: the 0-based Pixel-M2, within the
    // character that follows the HSYNC, at which the picture comes back. Fixed per
    // chip and unaffected by R2.JIT, because §9.3.4.3 (p.58) gives the zone's total
    // length for each case and both JIT and NJIT land on the same pixel:
    //   CRTC 0  NJIT 32 Pixel-M2 (2 µsec) from 4  -> 4      JIT 28 from 8 -> 4
    //   CRTC 1  NJIT 32 from 5                    -> 5      JIT 29 from 8 -> 5
    //   CRTC 2  NJIT 33 (2.0625 µsec) from 3      -> 4      JIT 29 from 7 -> 4
    // §14.9's chronogram (p.145, R2=10 R3l=2) confirms CRTC 0 exactly: the bar runs
    // Pixel-M2 148..179 and the picture returns on 180, four pixels into the
    // character after the two the HSYNC covered. CRTC 4's row of the same chart
    // restarts on Pixel-M2 162 of its own (deferred) zone, i.e. 2.
    // NOT the same quantity as displayRestorePixelAfterHsync() below, which is the
    // end of the VSYNC's 26-line blanking.
    virtual int hsyncBlackEndPixel() const { return 4; }
    // ACCC §9.3.4.4 (p.65) CRTC 0, 1, 2: the same boundary after an R3.JIT, which
    // "can delay the end of a HSYNC by 0.25 µsec". Its chart has the picture back on
    // the 8th Pixel-M2 of the character, and "on CRTC 2, the HSYNC stops ON the last
    // Pixel-M2 of the HSYNC and allows the visualization of an additional pixel", so
    // the MC6845 gets it back one earlier. §14.5.4: the technique "does not work on
    // CRTC's 3 and 4, which synchronize the HSYNC with the display" — those return
    // their plain boundary. §14.5.4's "the use of OUTI does not allow this technique to
    // be used on these CRTC's" is honoured through lastWriteBlockIo, so only an
    // OUT(C),r8 arms it.
    virtual int hsyncBlackEndPixelJit() const { return 7; }
    // ACCC §14.5.4 (p.139): "On the CRTC's 0 and 1, the first µsecond of HSYNC is
    // special, because interrupting it with the value 0 interrupts the HSYNC
    // prematurely", and p.141's chart for CRTC 1 ("3rd µs OUT (C),r8 (I/O R3=0)" on
    // C0=R2) blackens only Pixel-M2 5 to 7 of that character. The 0-based Pixel-M2 the
    // picture returns on, or -1 where the chip is not modelled this way (then the write
    // simply withholds the HSYNC).
    virtual int r3ZeroFirstMicrosecondBlackEndPixel() const { return -1; }

    // ACCC §14.9 (p.144) / §16.2.1 (p.162), the end of the VSYNC black: "When a VSYNC
    // ends (at the end of the 26th HSYNC), the black color stops 1 PixelM2 after the
    // end of an HSYNC for CRTC's 0 and 1. It stops at the same time for CRTC's 2, 3
    // and 4."
    virtual int displayRestorePixelAfterHsync() const { return 1; }
    // ACCC §16.2.1 (p.161): with R7 programmed before C4=R7, "the display of the black
    // color starts from the 5th pixel of the VMA word which precedes C4=R7" on CRTC 0
    // and 2, "the 6th" on CRTC 1, "the 2nd" on CRTC 4.
    virtual int vsyncBlackStartPixel() const { return 4; }
    // ACCC §16.2.1 (p.161), the R7.JIT case — R7 written with the current C4, so the
    // VSYNC starts on this very character. "On CRTC 0: the display of the black color
    // begins on the 5th pixelM2 of the VMA word OF C0 FOR WHICH R7=C4... and this
    // regardless of the instruction used (OUT (C),reg8 or OUTI). The display of the
    // VSYNC and the activation of the CSYNC signal are DELAYED BY 1 µsec because R7 is
    // not modified fast enough to be considered immediately" — hence 16+4. "On CRTC's 1
    // and 2: if R7 becomes equal to C4 with OUT (C),reg8, the display of the black
    // color begins on the 9th pixelM2 of the word pointed to by VMA WHICH PRECEDES the
    // position of C0 on which R7=C4... if R7 becomes equal to C4 via OUTI: on the 5th."
    // §16.2.1 also says R7.JIT "does not work on CRTC's 3 and 4, whose VSYNC only
    // starts on C0=0", so those keep their plain position.
    virtual int vsyncBlackStartPixelJit(bool blockIo) const { return blockIo ? 4 : 8; }
    // ACCC §9.2.1 (p.48): "on gate arrays 40007, 40008, 40010 and ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed ONE PIXEL EARLIER than for other graphics modes...
    // In other words, the BORDER stops 1 pixel earlier on a line in mode 2 and starts 1
    // pixel earlier when C0=R1... ASIC 40489 of the CPC+ is not affected by this
    // discrepancy. Whatever the graphic mode on this machine, the pixels are aligned."
    // In Pixel-M2. (Type 4 is mapped onto type 3 here, so the 40226 that DOES advance
    // shares the 40489's profile; a distinct type-4 profile would return 1.)
    virtual int mode2PixelAdvance() const { return 1; }
    // ACCC §9.2.2's charts (p.50/51): an ink written by an OUT does NOT take effect on
    // a character boundary. With the write landing on the instruction's 3rd microsecond
    // (§4.4.3), the first pixel in the new colour is, counting Pixel-M2 from the start
    // of the character the GATE ARRAY is displaying:
    //
    //   CRTC 0/1/2 (40007/8/10)   mode 0 pixel 30, mode 1 pixel 60 -> 8    mode 2: 9
    //   CRTC 4     (ASIC 40226)   mode 0 pixel 30, mode 1 pixel 60 -> 8    mode 2: 8
    //   CRTC 3     (ASIC 40489)   mode 0 pixel 29, mode 1 pixel 58 -> 4    mode 2: 5
    //
    // So the classic GATE ARRAY and the 40226 change colour at the SECOND BYTE of the
    // character, and the 40489 half a byte earlier. The mode-2 column is the one extra
    // Pixel-M2 of the entry below, not a separate number.
    virtual int inkChangePixelInCharacter() const { return 8; }
    // ACCC §9.2.2 (p.49): "Updating the colour of an ink is applied to the same
    // position regardless of the graphics mode on CRTCs 0, 1, 2, and 3. On CRTC 4,
    // however, the color change 0.0625 usec (1 pixel mode 2) EARLIER in mode 2 than
    // in the other three graphics modes" — so on the 40226 alone the ink boundary
    // travels with the mode-2 pixel stream instead of staying put.
    virtual bool inkChangeFollowsMode2Advance() const { return false; }
    // ACCC §17.5 ACKNOWLEDGMENT R1=0 (p.186): an OUT R1,0 has a deadline within the
    // line after which the BORDER it would raise is "not considered (too late)". On
    // CRTC 0, 1 and 2 a write whose I/O lands at C0=0 is still in time and one at C0>=1
    // is not; on CRTC 3 and 4 only a write from the PREVIOUS line makes it, so nothing
    // landing during this line is acknowledged. Returns whether now is still in time.
    virtual bool acknowledgesR1ZeroNow(CRTC6845& crtc) const;
    // ACCC §19.5: what an R8 write does to ParityC9 and ParityFrame. §19.5.3 (p.210)
    // says plainly that there are TWO of these moments -- "these updates are performed
    // on the 3rd AND 4th µseconds of the OUT(C),C instruction (on R8)" -- so the hook
    // is a pair. This one is the 3rd µsecond, called from writeRegister with the value
    // R8 held before the write; a chip whose rule is not split in time uses only this.
    virtual void onInterlaceWritten(CRTC6845& crtc, int previousR8) const;
    // ...and this one is the 4th, run one character later from tickCharacter. It is a
    // separate microsecond of the chip's life and not a bookkeeping detail: C9 drives
    // the video pointer, so whatever the 3rd µsecond leaves in C9 is what the character
    // between the two rules fetches with. §19.5.3's chronograms (p.211-212) draw that
    // one-microsecond value explicitly -- see crtc_type_1.h.
    virtual void onInterlaceWrittenFourthMicrosecond(CRTC6845& crtc, int previousR8) const;
    // ACCC §18.3.4 (p.192, CRTC 3/4): "the management of R6=0 does not exist during the
    // line and is tested only [at C0=0]. Setting R6 to 0 when C4 and C9 are 0, but C0>0
    // will have no effect before the new frame." Real 6845s have no such test.
    virtual bool testsR6ZeroAtLineStart() const { return false; }
    // ACCC §15.5.2 (p.156, CRTC 2): "The condition for restoring the background display
    // occurs when C0=0 (and C4 has never reached R6)... However, during HSYNC, this C0=0
    // test is not performed. In this condition, BORDER is not disabled." That is the
    // VERTICAL restore -- §18.2.1's C4=C9=C0=0 -- not the per-line one: SHAKER D3/A2,
    // A3, B3 and C3 run a HSYNC through C0=0 on 64 mid-frame lines (R2=59..61, R3=5..6)
    // and the real CRTC 2 (cpc/CPC portal photos) displays every one of them. p.157's
    // three examples are what the vertical reading predicts: R2=50/R3=15 and R2=0/R3=6
    // put a HSYNC on C0=0 of every line, so C4 overflows (Note 1, p.156) and comes back
    // round to 0 under a HSYNC that forbids the restore -- "the display is off"; with
    // R3=14 the HSYNC ends on C0=0 and "the display is on".
    virtual bool skipsFrameDisplayRestoreDuringHsync() const { return false; }
    // ACCC §16.4.1: a VSYNC triggered mid-line by an R7 write does not consume that
    // partial line on CRTC 0, so the VSYNC runs one line longer than on CRTC 1 and 2.
    virtual bool midLineVsyncAddsALine() const { return false; }
    // ACCC §17.4.3 (p.184, CRTC 2): with R1=0 "the frame's last-line state evaluation
    // takes place when C0=0 but it occurs AFTER processing the C0=R1 evaluation...
    // The VMA'=VMA or R12/R13 assignment (depending on status) therefore takes place
    // BEFORE VMA=VMA' assignment on position C0=0."
    virtual bool assignsVmaPrimeBeforeVma() const { return false; }
    // ACCC §17.4.2 (p.183, CRTC 1): "if the R1 update takes place EXACTLY WHEN C0=R1
    // (R1.JIT), then the condition C0=R1 is no longer true and VMA' is not updated. If the
    // modification of R1 occurs when C0=R1+1 (C0>R1), then the condition C0=R1 is
    // satisfied and VMA' is updated." So on this chip the C0=R1 reload is decided at the
    // END of that character, after any write inside it -- not the instant C0 arrives.
    virtual bool decidesR1ReloadAtCharacterEnd() const { return false; }
    // ACCC §18.3.2 (p.191, CRTC 0 and 2): the C4=R6=C9=0 conflict is cancellable on
    // the first line — "when the conflict is cancelled (R6 updated with a value > 0 on
    // the first line of the frame), the BORDER does not remain activated as on the
    // other lines. In this situation however, IF R6 IS 0 WHEN C0=R1, THE BORDER
    // BECOMES DEFINITIVE." (And "if we prevent C0=R1 on the line C4=C9=0 (for example
    // R1=R0+1), and R6 is no longer equal to 0, then the BORDER is deactivated on the
    // following line.") True while the conflict is still live at C0=R1.
    virtual bool r6ZeroConflictBecomesDefinitive(CRTC6845& crtc) const { return false; }
    // ACCC §15.3: "During the processing of HSYNC CRTC, an update to R2 is no longer
    // considered if the purpose of this change is to start a new HSYNC during HSYNC."
    // On CRTC 0 that simply blocks it -- "two HSYNC's cannot be contiguous". On CRTC
    // 1, 2, 3 and 4 "there is a bug if C0=R2 on C0=R2+R3": §15.3.2 INFINITE HSYNC
    // spells out the consequence -- "the HSYNC does not end and C3 will overflow. C3
    // will increment up to 15, return to 0 and then back to 1", repeating for as long
    // as C0 keeps landing on R2.
    virtual bool suppressesContiguousHsync() const { return false; }
    // ACCC §15.3.4 (p.151). When C0 is equal to R2 again on the position C0=R2+R3l the
    // C3l counter overflows instead of zeroing -- and what the GATE ARRAY does about it
    // splits the chips:
    //
    //   "The CRTC 1, however, has time to generate an 'invisible' end of HSYNC, then
    //    immediately reactivate the signal for the GATE ARRAY. THE LATTER THEN RESET TO 0
    //    ITS INTERNAL CHARACTER COUNTER AND SENDS A SECOND HSYNC MONITOR FROM THE 2ND
    //    POSITION (position C0vs=23 in the example above). The transition from HSYNC
    //    OFF/ON is fast enough to not be visible.
    //    The CRTC 2 does not have time to generate an end of HSYNC and the GATE ARRAY
    //    continues to display the black color until the HSYNC of the CRTC occurs."
    //
    // §15.3.5's chronogram draws CRTC 3 and 4 with the single long pulse as well, so the
    // second monitor sync belongs to the UM6845R alone. That second pulse is what SHAKER
    // D3 ("2XCSYNC RELATIVE") measures, and it is the largest test in the suite.
    virtual bool restartsCHsyncOnOverflow() const { return false; }
    // ASIC tracks a per-frame scanline counter (splits/DMA); real 6845s do not.
    virtual bool tracksScanlineInFrame() const { return false; }
    // In the plain raster-step branch, real 6845s clear vDisplay at C4=R6; ASIC does not.
    virtual bool clearsVDisplayOnRowMatch() const { return true; }
    // Per-chip next-row start-address reload at C0=R1 (ASIC honours screen splits).
    virtual void reloadNextRowAddress(CRTC6845& crtc) const;
    // DISPEN comb skew (R8 bits 4-5) — a real-6845 (0/2) artefact; 0 on UM6845R/ASIC.
    virtual int dispenSkew(CRTC6845& crtc) const { return 0; }
    // ACCC §19.2.3: the skew delays the BORDER R1 management -- the border-off of a new
    // line lands on C0=skew. Whether there is a border there to take off depends on the
    // chip. CRTC 0 always has one when R1>R0 (§17.6.2/§19.2.4: "the condition C0=R0
    // therefore replaces the condition C0=R1"), so the first `skew` characters of every
    // line are BORDER. §19.2.4's note: "On CRTC's 1, 3 and 4, in the same context (R1>R0),
    // the condition C0=R1 is not met, and the CRTC does NOT send BORDER ON signal to GATE
    // ARRAY" -- so on the ASIC a line start only keeps a border that is already up.
    virtual bool skewBordersEveryLineStart() const { return true; }
    // ACCC §19.2.4 (p.196): "when a delay is programmed using the SKEW DISP functions, THE
    // DELAY IS COUNTED BASED ON THE TRANSITIONS OF C0" -- the DISPTMG pin is the chip's
    // own display signal passed through a skew-character delay. A chip that says so runs
    // the display events unskewed on hDisplayInternal and drives hDisplay from the delay.
    virtual bool skewIsDelayLine() const { return false; }
    // ACCC 1.10 §13.2.7 (R.V.L.L.): CRTC 0 needs a rupture of at least 2 µsec for the
    // C9 counter to behave; when a "Last Line" lands on such a short line, the chip
    // emits ONE EXTRA line with C4=1 / C9=0 and the R12/R13 address reload is skipped
    // (because C4!=0) — C4/C9 only return to 0 on the following line-frame. That makes
    // the C9 increments over these short lines one FEWER than on CRTC 1/2/3/4.
    virtual bool generatesAdditionalLastLine(CRTC6845& crtc) const { return false; }
    // ACCC §13.2.4 (p.103): "The vertical adjustment management has 2 particularities
    // on the CRTC 0: C4 is incremented only once, whatever the value of R5. C4
    // returns to 0 once the adjustment is complete, whatever the value of R4."
    // Entering the adjustment already steps C4 once on every chip; the other chips
    // additionally step it on each adjustment line, CRTC 0 does not.
    virtual bool incrementsVerticalEachAdjustLine() const { return true; }
    // ACCC §11.1 (p.81): "On CRTCs 0, 3 and 4, there is no specific C5 counter and C9
    // is used for comparison with R5. On CRTCs 1 and 2, there is a specific counter C5
    // used in conjunction with C9." A chip with a C5 keeps C9 wrapping at R9 through
    // the adjustment and steps C4 on that wrap (§11.2.5), instead of running C9 all the
    // way up to R5. CRTC 1 owns a whole vertical path of its own and already does this;
    // this flag is how the MC6845 gets it on the shared path.
    virtual bool hasSeparateAdjustCounter() const { return false; }
    // ACCC §13.2.1 (p.105) and §11.3.1 (p.86): on CRTC 0 the adjustment is settled in
    // the same C0<3 window that decides the Last Line. "If R5 is programmed on the last
    // frame line with a value greater than 0 BEFORE C0=3 (C0=0, 1 or 2), then R5
    // additional lines will be considered... if R5 becomes greater than 0 WHEN C0>2,
    // then no additional line will be added to the frame." So the entry reads R5 as it
    // stood at the deadline, not as it stands when the line happens to end. (§11.3's
    // live R5 update DURING an adjustment is a separate rule and still applies.)
    virtual bool settlesAdjustLengthBeforeC0Three() const { return false; }
    // ACCC §14.5.4 (p.139): "If R3l is modified via an OUT(C),r8 with the value of C3l
    // when C0 is at position which corresponds to C3l while R3l was greater than this
    // value, then THE HSYNC STOPS on CRTC's 0, 1 and 2... Whether with OUT(C),r8 or
    // OUTI, THIS TECHNIQUE DOES NOT WORK ON CRTC'S 3 AND 4, which synchronize the HSYNC
    // with the display."
    virtual bool supportsR3Jit() const { return true; }
    // ACCC §11.2.2 (p.82): "On CRTC 0, HITACHI engineers saved a C5 counter to use C9
    // instead. In additional management, C9 is compared with R9 and R5. The new limit
    // of C9 is no longer R9 at the end of the line, but R5 at the beginning of the
    // line... it will end when C9 calculated for the new line == R5." Every other chip
    // has a real C5 and simply counts R5 lines.
    virtual bool adjustmentCountsRasterToR5() const { return false; }
    // ACCC §11.3.3 (p.87): on CRTC 3 and 4 an R5 written below C9+1 during the
    // adjustment ends it on this line rather than overflowing the counter round to it —
    // "it is impossible to overflow C9" on these chips, with either R5 or R9.
    virtual bool adjustmentCannotOverflow() const { return false; }
    // ACCC §19.6.4 (CRTC 3, 4): the additional interlace line "does not consider parity
    // states as on other CRTC's. C9 will always be 0" -- also after R5 lines, whose own
    // count starts from 0 too (R5 "still contains a finite number of lines").
    virtual bool additionalLinesCountFromZero() const { return false; }
    // The UM6845R counts its adjustment lines UP and compares against R5 live on
    // every line (CRTC6845::updateVerticalType1), so §11.3's mid-adjustment R5 write
    // needs no fix-up; the chips on the general path carry a down counter and do.
    virtual bool adjustmentLimitReadLive() const { return false; }
    // ACCC §11.1 / §11.2 (the R4=10 R5=16 R9=3 tables on p.80 show this outright):
    //   CRTC 0    C4 is incremented ONCE and sits at R4+1 for the whole adjustment
    //   CRTC 1,2  C4 incrementseach time C9=R9 while C5 has not reached R5
    //   CRTC 3,4  "C4 does not increment and is equal to R4"
    // So the ASIC does not even take the single step the others take on entry.
    virtual bool incrementsVerticalOnAdjustEntry() const { return true; }
    // ACCC §13.2.2 FREEZE OF VSYNC: on CRTC 0 a state set at C0=2 validates the
    // C4=R7 test for the NEXT C0=0 and is cancelled at C0=0. With R0<2, C0 never
    // reaches 2, so THAT C4=R7 occurrence is blocked (not vsync in general — the
    // block lifts when the C4/R7 equality changes, §16.3 mechanism 2).
    virtual bool allowsVsyncStart(CRTC6845& crtc) const { return true; }
    // ACCC §16.4.3: on the MC6845 "VSYNC is considered on all values of C0 and C9
    // when C4=R7" — the other chips only test it at the start of a character row.
    virtual bool considersVsyncAtAnyRaster() const { return false; }
    // ACCC §16.4.3: if that condition lands inside the HSYNC (C0 = R2 .. R2+R3, one
    // µsec longer than the visible HSYNC) the MC6845 makes a GHOST VSYNC — it counts
    // the lines as if a VSYNC were running, and so blocks a new one, but never
    // enables the VSYNC pin. Default: no chip-level ghosting.
    virtual bool vsyncStartIsGhost(CRTC6845& crtc) const { return false; }
    // ACCC §16.5 DELAYED VSYNC. Every chip delays VSYNC by half a line on the even
    // frame of an interlace mode (§16.5.1-4, and §19.3.1). CRTC 0 and CRTC 3/4 have
    // a SECOND case the others do not: "a delay of one complete line in the
    // particular case where R9 is odd in IVM mode (R8=3) on an odd frame and an odd
    // C4". CRTC 1 lacks it and, per §16.5.2, "does not correctly synchronize a frame
    // in IVM mode when R7 is odd on an image composed of characters with an odd
    // number of lines (R9 even)".
    virtual bool delaysVsyncOneLineInIvm(CRTC6845& crtc) const { return false; }
    virtual void onCharacterPosition(CRTC6845& crtc, int c0) const {}
    // ACCC §10.3.1.2 (CRTC 0; §10.3.3 says CRTC 2 works the same way): the "last
    // frame line" is a LATCHED STATE, not a test made at the end of the line.
    // "If C4=R4 and C9=R9 when C0<2, then the last frame line state is triggered.
    //  If C4<>R4 or C9<>R9 when C0<2 then the state is disarmed.
    //  The state is no longer evaluated when C0>1.
    //  If the state is armed when C0>1, C4 will be reset to 0 on the next line
    //  (after the additional line(s) if R5>0) and C9 will change to 0.
    //  Programming R9 (or R4) when C0>1 will not prevent C4 and C9 returning to 0."
    // CRTC 1 has no such latch -- §10.3.2 is pure logic evaluated as it goes.
    virtual bool usesLastFrameLineLatch() const { return false; }
    // ACCC 1.10 §13.2.1: with R0=0 CRTC 0 never reaches C0=1, so C9 management is
    // never re-authorised: C9 (and with it every counter) FREEZES...
    virtual bool blocksRasterAdvance(CRTC6845& crtc) const { return false; }
    // ...and, as a direct consequence of C9 being managed only once, "updates to
    // registers R4, R5 and R9 are no longer considered as long as R0=0" (R8 still is).
    virtual bool ignoresRegisterWrite(CRTC6845& crtc, int reg, int value) const { return false; }
    // ACCC §17.6.2 (CRTC 0 and 2 only): "R1>R0 AND C0=R0 — the consideration of the
    // BORDER is anticipated. 1 byte of BORDER is generated (for exactly 0.5 µsec)
    // before C0 goes to 0." Half a character, so it is expressed as a byte mask
    // rather than through the whole-character display enable.
    virtual bool anticipatesBorderByte(CRTC6845& crtc) const { return false; }
    // ACCC §19.2.4 (p.196): on CRTC 0 and 2, when R1>R0 "the condition C0=R0
    // therefore replaces the condition C0=R1" for the BORDER — which is what makes
    // the SKEW DISP delay measurable from C0=R0 there. The other chips send no
    // BORDER ON at all in that context.
    virtual bool replacesBorderR1WithR0() const { return false; }
    // ACCC §18.2.4 (CRTC 3, 4): "The R6 test is done at the beginning of the line
    // only. The update of R6 during the line is therefore not considered." The real
    // 6845s take C4=R6 immediately, whatever C0 is (§18.2.2, §18.2.3).
    virtual bool considersR6WriteImmediately() const { return true; }
    // ACCC §18.3.2 (CRTC 0, 2): "EXCEPT FOR THE FIRST LINE OF A FRAME (C4=C9=0), positioning
    // R6 with C4 causes the immediate and definitive activation of the BORDER until the
    // next frame." On that first line R6=0 is the CONFLICT instead -- "the condition
    // C4=R6=0=BORDER is cancellable on the first line", definitive only "if R6 is 0 when
    // C0=R1" (r6ZeroConflictBecomesDefinitive). CRTC 1 has the opposite rule (§18.2.3:
    // C4=R6 when C4=0 is definitive), so this is per chip.
    virtual bool r6FirstLineWriteIsConflict() const { return false; }
};

const CrtcBehaviour& crtcType0();
const CrtcBehaviour& crtcType1();
const CrtcBehaviour& crtcType2();
const CrtcBehaviour& crtcType3();
const CrtcBehaviour& crtcType4();
const CrtcBehaviour& crtcType1b();   // ACCC §11.6: the UM6845R that takes RFD#10
const CrtcBehaviour* crtcTypeFor(int type); // nullptr for unknown

// Constructor options (the object literal passed by the emulator).
struct Crtc6845Options {
    std::function<void()> onHsync;
    std::function<void()> onHsyncStart;
    std::function<void()> onHsyncCancelled;   // ACCC §14.5.4 Note 1: an OUTI R3=0 on C0=R2
    std::function<void()> onCharacterStart;
    std::function<void()> onCharacter;
    std::function<void()> onVsyncStart;
    std::function<void(int frame)> onFrame;
    std::function<void(int scanline)> onScanline;
    std::function<void(int value)> onSelect;
    std::function<void(int horizontal, int scanline)> onLightgunBeam;
    std::function<int()> getHorizontalScroll;
    std::function<std::optional<CrtcSplit>()> getSplit;
};

class CRTC6845 {
public:
    // Callbacks.
    std::function<void()> onHsync, onHsyncStart, onHsyncCancelled, onCharacterStart, onCharacter, onVsyncStart;
    std::function<void(int)> onFrame, onScanline, onSelect;
    std::function<void(int, int)> onLightgunBeam;
    std::function<int()> getHorizontalScroll;
    std::function<std::optional<CrtcSplit>()> getSplit;

    // Extra accessors wired by the emulator after construction (consumed by
    // the video renderer; the CRTC only stores them).
    std::function<const std::vector<std::shared_ptr<RasterLine>>&()> getRasterFrame;
    std::function<const std::vector<std::shared_ptr<RasterLine>>&()> getPreviousRasterFrame;
    std::function<std::array<uint8_t, 18>()> getFrameRegisters;
    std::function<bool()> usesPhysicalRasterFrame;
    std::function<int()> getPhysicalFrameOriginY;


    int type = 3;
    const CrtcBehaviour* behaviour = nullptr;

    std::array<uint8_t, 18> registers{};
    std::array<uint8_t, 18> rawRegisters{};

    int selected = 0, charClockRemainder = 0;
    int horizontal = 0, vertical = 0, raster = 0, vsyncCounter = 0, verticalAdjust = 0;
    bool horizontalTotalMatch = false;
    int scanlineInFrame = 0;
    int hsyncCounter = 0;
    bool hsync = false, vsync = false, vblank = false;
    bool r7Match = false, r4Match = false, r9Match = false;
    bool additionalLinePending = false;   // CRTC0 R.V.L.L. extra C4=1 line (ACCC §13.2.7)
    bool vsyncAuthorized = true;         // ACCC §13.2.2: set at C0=2, cancelled at C0=0
    bool r0FreezeArmed = false;          // ACCC §13.2.6: R0=0 freeze has been entered
    bool r0FreezeHiccup = false;         //   ... with C9=R9, so C4 still gets one step
    bool verticalAdjustActive = false, rasterMatchForced = false, lpenStrobe = false, updateReady = true;
    bool verticalMatchForced = false;    // ACCC §13.4: MC6845 latches the C4 reset at C0=0
    bool vsyncGhost = false;             // ACCC §16.4.3: counting a VSYNC with the pin off
    bool lastLineBlocked = false;        // ACCC §15.6: HSYNC on C0=0 cancels the Last Line
    bool lastFrameLine = false;          // ACCC §10.3.1.2: armed/disarmed at C0<2
    bool lastLineManagement = false;     // ACCC §12.4.1 (CRTC 2): may re-evaluate mid-line
    // ACCC §12.4.1/§15.6: "a HSYNC takes place on position C0=0" -- the pin on character 0
    // itself, latched when C0 wraps. The line-start evaluation runs a character later,
    // after updateHsync has already moved on to character 1, so reading hsync there
    // missed a HSYNC that ends on C0=1 (p.153, R2=50/R3=15).
    bool hsyncOnLineStart = false;
    // Valid inside the character on which C0 wraps, before checkHsync: the pulse still
    // running into character 0, or the one R2=0 is about to start there.
    bool hsyncReachesLineStart() const { return hsync || (registers[2] & 0xff) == 0; }
    bool lastPreviousLine = false;       //   ... and whether line N-1 was already a last line
    // ACCC 1.11 §12.4.1 (p.96, CRTC 2): "At the beginning of a line (C0==0), the
    // comparison uses the updated value of R4, but THE PREVIOUS VALUE OF R9 (an
    // update of R9 on C0==0 occurs too late for this evaluation)." Snapshotted as
    // the C0==0 character opens, before any write of that microsecond lands.
    int r9Previous = 0;
    // ACCC §10.3.1 (1.11 p.77, CRTC 0): "There is a special case in this counting
    // logic when R9 is modified exactly at the position C0 equals R0, while C9 was
    // equal to R9 before C0 reached R0. In this case, C4 is still incremented. This
    // impacts the results of C4 and C9, as it can result in a situation where both C4
    // and C9 are incremented simultaneously."
    bool verticalStepForced = false;
    bool verticalStepSuppressed = false;   // ACCC §10.3.1: the mirror of verticalStepForced
    // ACCC §11.1: the interlace even-frame extra adjustment line, 0 or 1. Held apart
    // from R5 so §11.3's live R5 updates can be applied without losing it.
    int adjustmentInterlaceExtra = 0;
    // ACCC §11.3.2 (p.86, CRTC 1): "CRTC 1 activates an internal additional management
    // state if R5>0 when C4 should return to 0 at the end of the frame (C4=R4, C9=R9)."
    // It is that state, and it matters because R5 going to 0 while it is set does NOT
    // clear it -- see updateVerticalType1.
    bool adjustStateEngaged = false;
    // ACCC §11.3: C5 — the adjustment line already counted. On CRTC 0 the role is
    // played by C9 (§11.2.2) and this stays at 0.
    int adjustLine = 0;
    // R5 as it stood inside §13.2.1's C0<3 deadline, for the chips that settle the
    // adjustment there (settlesAdjustLengthBeforeC0Three).
    int adjustLengthLatched = 0;
    // R5 as it stood when this character opened, for §11.6's "R5 was equal to 0"
    // trigger test (the write has already landed by the time the hook runs).
    int r5Previous = 0;
    // ACCC §19.6/§19.5 (p.213, p.217): ParityR6 — "becomes odd when C4 reaches R6 on
    // an even frame, and even when C4 reaches R6 on an odd frame, because it is used
    // to anticipate the parity of the following [frame]... If R6>R4, ParityR6 state
    // is no longer updated and this has the effect of FREEZING the parity of the
    // frame and the addition of the additional line." At each frame start
    // ParityFrame = ParityR6, so a C4 that never reaches R6 stops the alternation.
    // Its management is independent of R8.
    int parityR6 = 1;
    // ACCC §19.7: the ParityFrame value the MID-VSYNC decision uses this frame. Equal
    // to interlaceField except in §19.7.3's R7=0 case on the ASIC.
    int midVsyncParity = 0;
    // ACCC §11.6 R.F.D. (CRTC 1): "there is a state that allows CRTC 1 to accept
    // consideration of R12/R13 at the start of the line regardless of the value of
    // C4." Set by writing R5 non-zero at C0=R0 while R5 was 0; cleared as soon as VMA
    // is updated with VMA' (the C9=R9 test at C0=R1 succeeding).
    // ACCC §19.5: ParityC9 — the low bit C9 carries into the video pointer in IVM.
    int rasterParity = 0;
    // ACCC §19.8.3 (CRTC 2): C9.IVM, the counter that feeds the address in IVM while C9
    // itself counts "in a conventional way". Its "management... takes place all the time".
    int ivmRaster = 0;
    // ACCC §19.5.3 (p.210): the R8 write's second rule lands on the 4th µsecond of the
    // OUT, one character after the 3rd. Holds the R8 value from before the write until
    // then; -1 when nothing is pending.
    int interlaceFourthMicrosecond = -1;
    // ACCC §19.8.1 (p.220): "When R8 changes to 3, a status indicates that the
    // calculation of the value compared to 'R9 or ParityFrame' will be performed on
    // the NEXT C0=0, after the C9/R9 test of the line. This is most certainly done
    // in this way to prevent the C9 used for the display from switching mid-line."
    // So on the line R8 goes to 3, it is plain C9 that is compared, not C9.VMA.
    bool ivmTestDeferred = false;
    // ACCC §19.8.1 (p.221): the mirror of the above when R8 LEAVES IVM -- until C0
    // returns to 0 the limit test is C9.VMA against R9, with parity no longer added
    // to R9.
    bool ivmExitDeferred = false;
    // ACCC §21.3.1: CRTC 1's status bit 5 — a BORDER R6 latch sampled at C0=R0.
    bool borderR6Status = false;
    bool rfdActive = false;
    // §17.4.2: C0 reached R1 on this character and the reload waits for its end.
    bool r1ReloadPending = false;
    // ACCC §11.6/§11.6.2: an RFD armed with R5=#10 on a CRTC 1-B, which "deactivates
    // parity management in test C9=R9" for the rest of that RFD.
    bool rfdIgnoresParity = false;
    // ACCC §11.6.2 (p.90): "When the IVM state which authorizes the consideration of
    // parity in the test C9=R9 becomes active with a RFD, it is no longer possible to
    // deactivate it until the frame is finished. If a RFD activates this status, an
    // RFD#10 can no longer deactivate it. If a RFD #10 (CRTC 1-B) deactivates this
    // status, a RFD 'not #10' allows it to be reactivated." Set by an RFD that turns
    // parity ON, cleared at the frame start; while set, an RFD#10 cannot turn it off.
    bool rfdParityLocked = false;
    // ACCC §13.7.1.2: R0 was enlarged at C0=R0 on a last line, so an R4/R9 write in
    // the tail that cancels the last-line condition raises an RFD.
    bool lastLineEnlarged = false;
    // ACCC §19.6.3 (p.218, CRTC 2): IVM switched on during the frame's first line on an
    // odd frame turns that line into the interlace additional line, so a fresh line 0
    // follows it. Consumed at the end of the line.
    bool interlaceLineNow = false;
    // ACCC §13.7.2 (p.126, CRTC 0): an R0 written at C0=1 while C9=R9, enlarging R0
    // out of 1, steps C4 at the C0=2 that the enlargement has just made reachable --
    // and leaves the additional management engaged there even with R5=0. Armed by
    // the write, spent at C0=2.
    bool r0EnlargeStepsVertical = false;
    bool adjustEngagedWithoutR5 = false;
    // ACCC §15.3.1/§15.3.3: whether the HSYNC ended on the character now being
    // processed, and whether R3 was written on it — CRTC 0's contiguous-HSYNC rule
    // turns on both.
    bool hsyncEndedThisCharacter = false;
    // The pulse ended as this character opened, held until the next one opens (so a
    // register write landing during it still sees it) -- ACCC §16.4.3's C0=R2+R3.
    bool hsyncDroppedThisCharacter = false;
    bool r3WrittenThisCharacter = false;
    // ACCC §14.7.1: R2 written on this very character — the R2.JIT case, which moves
    // the HSYNC black zone later within it.
    bool r2WrittenThisCharacter = false;
    // ACCC §16.2.1: R7 written on this very character — the R7.JIT case.
    bool r7WrittenThisCharacter = false;
    // ACCC §14.7.1: this HSYNC was raised by an R2.JIT, i.e. from a register write
    // between two characters rather than from the per-character comparison.
    bool hsyncArmedBetweenCharacters = false;
    // ACCC §14.5.4 (p.139) R3.JIT: "If R3l is modified via an OUT(C),r8 with the value
    // of C3l when C0 is at position which corresponds to C3l while R3l was greater than
    // this value, then the HSYNC stops on CRTC's 0, 1 and 2." The HSYNC's own end is
    // already the C3l==R3l test; what R3.JIT adds is that the GATE ARRAY holds its
    // black "0.25 µsec after its normal end" (§9.3.4.2). Armed on the write, latched
    // when the HSYNC ends, and read by the renderer on the character after.
    // ACCC §13.3 note 3 (p.114): whether the I/O that is writing a CRTC register came
    // from a block instruction (OUTI/OUTD), whose R0 write on CRTC 1 is compared
    // against C0 AFTER the assignment. Set by the host beside the write.
    bool lastWriteBlockIo = false;
    // R0 as it stood when this character opened, for the same rule.
    uint8_t r0Previous = 63;
    // R8 as it stood when this character opened, for §19.5.3's IVM-transition parity.
    uint8_t r8Previous = 0;
    bool r3JitPending = false;
    bool hsyncEndedJit = false;
    // ACCC §14.5.4: the HSYNC was cut on its first usec by an OUT(C),r8 R3=0, after its
    // character had been sampled with the pin still down. Read by the next capture.
    bool hsyncCutFirstMicrosecond = false;
    // An ordinary end made early by an R3 write, lasting hsyncCounter usec: §14.5.4 Note 2's
    // OUTI on CRTC 0-2, and §14.5.3's R3 = C3 on CRTC 3/4 (the chip's C3 lags ours by one).
    bool hsyncEndedByBlockWrite = false;
    // ACCC §11.2.4 (p.85, CRTC 1): the vertical adjustment was entered from C4=0, so
    // "VMA is updated with R12/R13 and not VMA'... as long as C4=1". The second flag is
    // its exception: "if R4 was modified to C0==R0 with R4>0, then VMA is not updated
    // with R12/R13 when C4=1" — armed by that write, read when the adjustment opens.
    bool adjustFromFrameStart = false;
    bool adjustBlocksRegisterReload = false;
    // ACCC §17.5: an R1=0 write that missed this line's acknowledgment deadline. The
    // BORDER it would have raised is held off until the next line.
    bool r1ZeroLate = false;
    // ACCC §17.1: C0 came back to 0 by overflowing 255, not by matching R0 — the
    // display is NOT re-authorised in that case.
    bool horizontalOverflowed = false;
    // The BC value of the I/O that wrote a register. ACCC §12.5 needs bit 5 of B
    // ("Rom Select" active when it is 0) for the CRTC 3 R4 exception.
    int lastWritePort = 0xffff;
    bool vsyncHalfLinePending = false;   // ACCC §19.3.1: interlace half-line VSYNC delay
    bool vsyncFullLinePending = false;   // ACCC §16.5: IVM one-complete-line VSYNC delay
    bool vsyncSkipsFirstLineEnd = false; // ACCC §16.4.1: mid-line VSYNC start (CRTC 0)
    // ACCC §16.2.1: the GATE ARRAY blanks its output to black for the 26 lines its
    // V26 counter spans. Mirrored here by the host so the renderers can see it.
    bool gateArrayBlanking = false;
    bool hsyncOverflow = false;
    // ACCC §15.3.4: set while the restart being signalled is the C3l-overflow one, so
    // the GATE ARRAY can put the "invisible" end out before it.
    bool hsyncRestartFromOverflow = false;          // ACCC §15.3.2: C3 is running its 16 values
    bool addressFrozenForAdditionalLine = false;   // ACCC §13.2.7: R.V.L.L. line keeps its row
    // ACCC 1.10 §10.3.1.2 (CRTC 0): vertical adjustment is a STATE, decided at C0==2.
    // "If vertical adjustment is active on the last line (R5>0 on C0==2), then the
    // vertical adjustment state becomes true and cancels the last line state" — and
    // with R0<2 that state "is active by default until C0 reaches 2", which is what
    // produces the R.V.L.L. additional line.
    bool verticalAdjustState = false;
    // Set when the host drives scanlineInFrame itself (the classic monitor assigns
    // it from its own flywheel line). The ASIC otherwise self-increments it, and
    // two writers leave a stray index one past the end of the frame.
    bool hostOwnsScanlineIndex = false;
    // CPCSE_TRACE_REG: which register to log writes of, and how many are left to log.
    // Members rather than statics so a harness can aim the budget at the screen it
    // cares about -- a per-line technique burns 400 writes long before the test you
    // want appears. The environment seeds them; the host may overwrite at any time.
    int traceRegister = -2;              // -2 = not yet seeded from the environment
    long traceRegisterBudget = 0;
    // The VSYNC *pin* as the rest of the machine sees it (PPI status, Gate Array
    // interrupt sync, renderer frame origin). A ghost VSYNC counts internally but
    // never drives the pin, so always read the pin through here, not `vsync`.
    bool vsyncPinActive() const { return vsync && !vsyncGhost; }
    // Which of the current character's two bytes carry video: 0 = none, 3 = both,
    // 1 = the first byte only (the anticipated BORDER byte of ACCC §17.6.2).
    int displayOutputBytes() {
        if (!displayOutputEnabled()) return 0;
        return behaviour->anticipatesBorderByte(*this) ? 1 : 3;
    }
    bool hDisplay = false, vDisplay = false;
    // §19.2's delay line (skewIsDelayLine): the undelayed horizontal display signal, and
    // what it fed the delay on the last two characters (bit 0 = the previous one).
    bool hDisplayInternal = false;
    int skewHistory = 0;
    int frame = 0;
    int requestedAddress = 0, frameAddress = 0, rowAddress = 0, nextRowAddress = 0, maRow = 0, vlc = 0;
    int interlaceField = 0;
    int revision = 0;

    bool lightgunEnabled = false;
    std::string lightgunType = "trojan";
    bool lightgunTriggered = false;
    double lightgunX = -1, lightgunY = -1;
    int lightgunViewWidth = LIGHTGUN_VIEW_WIDTH, lightgunViewHeight = LIGHTGUN_VIEW_HEIGHT;
    int lightgunAddress = 0;
    bool lightgunLatched = false;

    explicit CRTC6845(const Crtc6845Options& options = {});

    void setType(int type);
    int writeMask(int reg) { return behaviour->writeMask(reg); }
    void reset();
    void write(int port, int value);
    void writeRegister(int reg, int value);
    int read(int port);
    void setLightgunType(const std::string& type = "trojan");
    void setTrojanLightgunEnabled(bool enabled);
    void setTrojanLightgun(double x, double y, bool trigger, int viewWidth = LIGHTGUN_VIEW_WIDTH, int viewHeight = LIGHTGUN_VIEW_HEIGHT);
    void releaseTrojanLightgun();
    void refreshTrojanLightgun();
    void updateTrojanLightgun();
    // The LPEN pin's low-to-high edge: latches MA+2 into R16/R17 and raises the status
    // register's "light pen register full" bit.
    void strobeLightPen();
    int effectiveHsyncWidth();

    // ACCC §19.2 (p.194): the SKEW DISP field's value 3 is the BORDER ON function, and
    // "IF THE BORDER ON FUNCTION IS ACTIVATED, THE INTERLACE FUNCTION ON THE 2 LEAST
    // SIGNIFICANT BITS IS NOT CONSIDERED. (This point requires further investigation)."
    // The author's caveat is about his confidence, not about what he observed, so the
    // rule is honoured -- and only on the chips that have the field at all, which is
    // what honoursDisplaySkewField() reports (CRTC 1 and 2 have no Sd, §19.1).
    int displayMode() const { return displayModeOf(registers[8]); }
    // The same for any R8 value (the previous one, say). Every reader of R8's interlace
    // bits goes through here, or BORDER ON leaves half the chip interlaced.
    int displayModeOf(int r8) const {
        if (behaviour->honoursDisplaySkewField() && (r8 >> 4 & 3) == 3) return 0;
        return r8 & 3;
    }
    bool displayOutputEnabled();
    int oldInterlaceVideo() const { return behaviour->interlaceVideo(*this) ? 1 : 0; }
    int rasterStep() const { return oldInterlaceVideo() ? 2 : 1; }
    // ACCC §19.5 (p.214): C9's parity in IVM is ParityC9, not the frame parity.
    // "At the beginning of the frame, Parityc9=ParityFrame... ParityC9 switches to
    // each new C4 when R9 is odd (as on CRTC 0)... when R9 is odd, the parity of the
    // lines depends on that of C4 and on the current parity of C9." With an even R9
    // it never diverges from the frame parity, which is the classic behaviour.
    int baseRaster() const { return oldInterlaceVideo() ? rasterParity & 1 : 0; }
    int maximumRaster() const;
    int rasterHeight() const { return oldInterlaceVideo() ? (maximumRaster() >> 1) + 1 : maximumRaster() + 1; }
    int videoRaster();
    int videoRaster(int verticalScroll);
    // The RA0-RA4 pins. UM6845R data sheet: "These signals are active-high outputs and
    // are used to select each raster scan within an individual character row." Five
    // bits, and nothing else on them -- videoRaster() above is a different quantity,
    // the CPC's ADDRESS FIELD, which §20.2 builds from "bits 0 to 2 of C9" with the
    // ASIC's soft scroll added and folded back inside the character. Those belong to
    // the machine around the chip; the pin is just the counter, doubled with the
    // frame's parity in IVM (ACCC §19.8.2's C9.VMA).
    int rasterOutput();
    int screenAddress() const { return (((registers[12] & 0x3f) << 8) | registers[13]) & 0x3fff; }
    // R14/R15, the character the CURSOR pin marks. UM6845R data sheet: "These registers
    // together comprise a 14-bit register containing the memory address of the current
    // cursor position."
    int cursorAddress() const { return (((registers[14] & 0x3f) << 8) | registers[15]) & 0x3fff; }
    // The CURSOR output pin. UM6845R data sheet (Cursor Position High/Low): "When the
    // video display scan line counter (MA lines) matches the contents of this register,
    // and when the scan line counter (RA lines) falls within the bounds set by R10 and
    // R11, then the CURSOR output becomes active." R10 bits 6 and 5 give the mode --
    // 00 no blinking, 01 no cursor, 10 blink at 16x the field period, 11 at 32x -- and
    // R8 bit 5's one-CCLK delay does not exist on this chip, whose R8 is two bits wide.
    // The CPC does not connect this pin; the chip drives it all the same.
    bool cursorOutput();
    // Fields since power-on, for the cursor's blink. The data sheet gives the PERIOD in
    // field times and not the phase, so it is a 50% duty taken from the counter's high
    // bit: 8 fields on and 8 off at 16x, 16 and 16 at 32x.
    long fieldCounter = 0;
    int memoryAddress() const { return maRow & 0x3fff; }
    int memoryAddressBase() const { return rowAddress & 0x3fff; }

    // CPCSE_TRACE_FRAME=1: one stderr line per CRTC frame (see crtc.cpp::traceFrame).
    void traceFrame();
    // CPCSE_TRACE_LINE=1: one stderr line per CRTC LINE. The frame trace says a ruptured
    // frame ran N lines; only this says WHICH of them the chip spent where.
    void traceLine();
    long traceLineBudget = -2, traceLineFrom = 0;
    long traceCharacters = 0;   // characters (usec) since power-on, for the line trace
    long traceFrameBudget = -2;     // -2 = environment not read yet, -1 = off
    long traceFrameFrom = 0;        // CPCSE_TRACE_FRAME_FROM: skip to the frame that matters
    unsigned traceRegisterMask = 0; // CPCSE_TRACE_REG as a comma list -> bit per register
    long traceRegisterFrom = 0;     // CPCSE_TRACE_REG_FROM: skip to the frame that matters
    int tracePc = -1;               // PC of the instruction doing the current I/O write
    int traceFrameLines = 0, traceFrameRows = 0, traceFrameVsyncs = 0;

    void tick(int tStates);
    void updateHsync();
    void updateHsyncStandard();  // the standard 6845 Hsync body (default advanceHsync)
    void checkHsync();
    long traceHsyncBudget = -2;
    void traceHsync(const char* what);
    void startVsyncIfNeeded(bool liveWrite = false);
    void traceVsyncRefusal(const char* why);   // CPCSE_TRACE_VSYNC=1
    void traceGhostStart();                    // CPCSE_TRACE_GHOST=1
    long traceVsyncBudget = -2, traceVsyncFrom = 0;
    // Every CRTC VSYNC start. Compared against the GATE ARRAY C-VSYNC count in --diag:
    // the CRTC can raise VSYNC and the GA still emit nothing, because C-VSYNC only begins
    // when V26 reaches 2 (ACCC 16.2.3) and V26 restarts at every CRTC VSYNC.
    long dbgCrtcVsyncStarts = 0;
    bool rasterMatchesMaximum();
    bool claimInterlaceAdjustLine();   // ACCC §11.9
    void startDelayedVsyncLine();      // ACCC §16.5.1
    bool verticalMatchesTotal();
    bool reloadsStartAddressOnThisScanline();
    void latchRequestedStartAddress();
    void newFrame();
    bool updateVerticalType1();
    bool updateVerticalGeneral();
    void updateVertical();
    void tickCharacter();
};

} // namespace cpcse
