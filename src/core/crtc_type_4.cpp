// CPCSyntaxError — CRTC type 4 profile (ASIC 40226, "Pre-ASIC" cost-reduced CPC).
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
//
// The 40226 shares the CRTC core of the CPC+'s 40489 (crtc_type_3.h), which is why
// the compendium almost always writes "CRTC's 3 and 4". It is NOT the same chip
// though, and until this profile existed CRTC 4 was aliased onto CRTC 3 and took the
// 40489's behaviour wherever the two differ. Everything below is a place where the
// compendium names one and not the other.
#include "crtc_type_3.h"

namespace cpcse {

struct CrtcType4 : CrtcType3 {
    CrtcType4() { id = 4; name = "ASIC 40226"; }

    // ACCC §9.1 (p.48): "on gate arrays 40007, 40008, 40010 AND ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed one pixel earlier than for other graphics modes...
    // They are displayed in 'advance' of 1/16 µsec (0.0625 µsec)... ASIC 40489 of the
    // CPC+ is not affected by this discrepancy. Whatever the graphic mode on this
    // machine, the pixels are aligned." So the 40226 keeps the classic machines'
    // mode-2 skew and the 40489 alone drops it.
    int mode2PixelAdvance() const override { return 1; }

    // ACCC §12.5 (p.102): the 40489 has an R4 peculiarity when an OUT lands on C0=0
    // "in parallel with 'Rom Select'" (Z80A B register bit 5 clear) — it compares C4
    // against the new R4 before C4 goes to 0, and overflows. "Note that the CRTC 4
    // ASIC is not affected by this exception."
    bool hasRomSelectR4Exception() const override { return false; }

    // ACCC §9.3.4.1 (p.54): "On CRTC's 0, 1 and 2, the GATE ARRAY switches the mode
    // update to the 6th pixel mode 2 (i.e. the 3rd pixel mode 1, the 2nd pixel mode
    // 0). On the CRTC 4, the GATE ARRAY switches the mode change to the 4th pixel
    // mode 2 (i.e. the 2nd pixel mode 1, the middle of the 1st pixel mode 0)." The
    // chapter gives no figure for CRTC 3, so the shared profile keeps this value for
    // it too — it is stated here because here it is documented.
    int modeSwitchPixelInByte() const override { return 3; }   // 0-based: the 4th

    // ACCC §9.3.4.1 (p.54): "On CRTC's 0, 2 AND 4, when the display is reactivated by
    // the GATE ARRAY (R3=2), 1 Pixel-M2 is however displayed in the previous graphics
    // mode" (CRTC 3 is the one the chapter leaves "to be verified") — which is the gap
    // between the two numbers below, 3 - 2, without needing to be asked for.
    //
    // ACCC §14.7.2 (p.143) states this one outright, and is the clearest source for it:
    // "On this CRTC, the display stop linked to the HSYNC starts from the 19TH MODE 2
    // PIXEL AFTER THE START OF THE DISPLAYED CRTC R2-1 CHARACTER." A character is 16
    // Pixel-M2 and the ASIC's HSYNC is one character late (§9.3.1,
    // hsyncDisplayDelayCharacters), so the 19th pixel — index 18 — is 16 + 2, and the 2
    // is this hook. The same section gives the 40489 the 17th pixel instead, which is
    // 16 + 0 and is why crtc_type_3.h holds 0.
    //
    // §14.9's chronogram (p.145, R2=10) draws it on the same Pixel-M2 grid as the other
    // chips and agrees exactly: with R2=10 the displayed R2-1 character opens at
    // Pixel-M2 144, this chip's zone runs 162..193 and the picture returns at 194, while
    // CRTC 0's runs 148..179. 162 - 148 = 14 = one character of ASIC delay plus this 2,
    // less CRTC 0's 4. (§9.3.4.2's per-chip charts read one Pixel-M2 tighter, but they
    // are separate figures with their own origins and that section states its own half
    // Pixel-M2 tolerance; §14.9 puts every chip on one grid and §14.7.2 puts a number
    // on it in words.) §9.3.4.3's CRTC 4 chart (p.57) agrees too: the mode switch lands
    // on the Pixel-M2 after the picture comes back, at the 4th of that byte.
    int hsyncBlackStartPixel() const override { return 2; }
    int hsyncBlackEndPixel() const override { return 2; }
    int hsyncBlackEndPixelJit() const override { return 2; }   // §14.5.4: no R3.JIT here
    // ACCC §16.2.3 (p.165): the C-HSYNC rises "1 or 2 Pixel-M2 before the end of the 2 us",
    // by an amount that "depend[s] on the type of CRTC". This part takes the 2. Measured
    // against AmSpiriT: every other chip's picture aligns best one pixel over, this one's
    // two, which is the only place the chapter's "or 2" has to go.
    int cHsyncRiseAdvance16() const override { return 2; }
    // ACCC §14.7.2 (p.143): "An R2.JIT update carried out with an OUT(C),r8 does not
    // cause the HSYNC black zone to appear later than in the other situations since
    // the HSYNC is deferred" — so JIT and NJIT share the one position.
    int hsyncBlackStartPixelJit() const override { return 2; }

    // ACCC §9.2.2 (p.49): "Updating the colour of an ink is applied to the same
    // position regardless of the graphics mode on CRTCs 0, 1, 2, and 3. On CRTC 4,
    // however, the color change 0.0625 µsec (1 pixel mode 2) earlier in mode 2 than in
    // the other three graphics modes." In mode 2 the pixel stream runs one Pixel-M2
    // ahead, so a character's leading pixel is painted in the PREVIOUS character's
    // slot; on every other chip an ink written on that boundary has not happened yet
    // at that instant, and on this one it has, because the colour change moves back
    // with the pixels.
    bool inkChangeFollowsMode2Advance() const override { return true; }

    // ACCC §9.2.2's CRTC 4 chart (p.50, lower block) has the first pixel in the new
    // colour at mode 0 pixel 30 / mode 1 pixel 60 / mode 2 pixel 120 -- the second BYTE
    // of the character, exactly as on the classic GATE ARRAY. The 40489's half-byte
    // earlier position (crtc_type_3.h) is the 40489's alone, and this profile inherits
    // from it, so it has to be given back.
    int inkChangePixelInCharacter() const override { return 8; }
};

const CrtcBehaviour& crtcType4() { static CrtcType4 instance; return instance; }

} // namespace cpcse
