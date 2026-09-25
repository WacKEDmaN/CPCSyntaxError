// CPCSyntaxError — CPC Gate Array.
#include "gate_array.h"
#include <cstdio>
#include <cstdlib>
#include "asic.h"
#include "memory.h"

namespace cpcse {

GateArray::GateArray(GXMemory* memory, PlusAsic* asic, bool plusHardware)
    : memory(memory), asic(asic), plusHardware(plusHardware) { reset(); }

void GateArray::reset() { interruptCounter = 0; interruptSyncCount = 0; interruptRaiseIn = 0; modeLatchDelay = 0; modeLatchJustArmed = false;
    cVsyncGatedByCrtc = false; crtcVsyncPin = false;
    suppressNextR52Increment = false;
    v26 = -1; monitorSyncStarted = false;
    h06Pixels = 0; hsyncPinFallIn = -1; cHsyncWidth16 = 64; cHsyncFallArmed = false;
    sigGaHsync = sigGaVsync = cblackHsync = cblackVsync = vsyncGa = cHsyncStarted = cHsyncEnded = false;
    hsyncBlackRemaining = 0; }

// ACCC §9: the GATE ARRAY's port is &7Fxx -- A15=0, A14=1. Everything decoded here is
// this chip's: the pen select, the 17 inks, the graphic mode, the ROM configuration and
// the R52 reset. It used to be PlusAsic::writeGateArray(), which put a 464's registers
// inside the CPC+ extension chip.
void GateArray::write(int port, int value) {
    value &= 0xff;
    if ((port & 0xc000) != 0x4000) {
        // Not this chip's port. The upper-cartridge select is decoded outside it.
        if ((port & 0x2000) == 0 && memory) memory->selectUpperCartridgeFromPort(value);
        return;
    }
    // CPC+ only, and the one clause here that is genuinely the ASIC's: with the ASIC
    // unlocked, %101xxxxx pages the lower cartridge or maps the ASIC RAM.
    if (asic && asic->plusCartridgePageWrite(value)) return;

    if ((value & 0x80) == 0) {
        // %0x: select a pen (bit 6 clear) or set the selected pen's ink (bit 6 set).
        // 16 is the BORDER, §9.1's 17th colour.
        if ((value & 0x40) == 0) paletteIndex = std::min(value & 0x1f, 16);
        else {
            gaPalette[paletteIndex] = (uint8_t)(value & 0x1f);
            if (asic) asic->setGateArrayInk(paletteIndex, value & 0x1f);
        }
        return;
    }
    if ((value & 0x40) == 0) {
        // %10: RMR. Bits 0-1 are the graphic mode, taken at the next HSYNC-GA (§9.3.1),
        // bits 2-3 the ROM configuration, bit 4 the R52 reset.
        romConfig = value;
        newMode = value & 3;
        if (memory) memory->setGateArrayConfig(value);
        if (value & 0x10) resetInterruptCounter();
        return;
    }
    // %11: the RAM configuration, on the 6128 and above.
    if (memory) memory->setRamConfig(value, port);
}

void GateArray::latchMode(int switchPixel) {
    if (mode == newMode) return;
    // ACCC §9.3.4: remember what the byte under this chip's decoder was being read in,
    // and where inside it the new mode takes over. "Given that mode update occurs
    // during the processing of the byte read in ram, the algorithm for determining the
    // PEN by the GA switches during processing."
    modeBefore = mode;
    modeSwitchPixel = switchPixel;
    mode = newMode;
    if (asic) asic->bumpVideoRevision();
}

void GateArray::onHsyncStart(int hsyncWidth, int hsyncDelay, bool betweenCharacters, int fallPhase16) {
    // ACCC §16.2.3, "If HSYNC-CRTC Transition OFF -> ON": H06=0, CBLACK_HSYNC=true.
    // H06 restarts even if a C-HSYNC pulse is still running, which is what lets a
    // second HSYNC inside one line move and stretch the pulse the monitor locks to.
    // Like the mode latch below, this is raised from the END of the character C0=R2,
    // whose own onCharacter() follows immediately -- unless the HSYNC came from an
    // R2.JIT write, which lands between two characters and has none to swallow.
    // The rise is raised from the END of the character C0=R2, so the 16 Pixel-M2 of
    // that character still have to run before this counter reaches zero -- unless the
    // HSYNC came from an R2.JIT write, which lands BETWEEN two characters and has none
    // of them left to run.
    // ...and ACCC 15.1 (p.146): "THE ASIC's (CRTC's 3 and 4) manage a HSYNC consistent
    // with the C0 value displayed, DELAYING THE DISPLAY OF THE HSYNC BY 1 usec." That is
    // the WHOLE HSYNC, "over a length R3 usec" -- both edges of the pulse the monitor
    // locks to, not just the black. H06 is what times that pulse, so the delay goes on
    // its start; the pin-driven end, for an R3l too short to reach H06==6, is delayed the
    // same microsecond by armCHsyncFall.
    //
    // It used to be dropped here, because adding it alone moves the CRTC 3/4 picture a
    // character left. The other half of the chapter is why: the CM14, and the CTM Amstrad
    // calibrated for the CRTC 4 machine, hold a back porch a microsecond longer and
    // cancel it (monitor_model.h, GX4000::monitorCalibration16). Both halves are modelled
    // now, so the pairing the machine shipped with is centred and a mixed pairing shifts,
    // which is what the chapter describes.
    h06Pixels = (betweenCharacters ? 0 : -PIXELS_PER_CHARACTER)
              - hsyncDelay * PIXELS_PER_CHARACTER;
    // ACCC 15.3.4 (p.151): a second HSYNC arriving while this chip still OWES the end of
    // the first one does not swallow that end. The chapter draws two bars on one CRTC 1
    // line and labels both "Monitor Sync": the chip "has time to generate an 'invisible'
    // end of HSYNC, then immediately reactivate the signal for the GATE ARRAY", which
    // "reset to 0 its internal character counter and SENDS A SECOND HSYNC MONITOR".
    //
    // The armed end used to be thrown away here, so the first pulse ran on into the new
    // H06 window and the two syncs came out as ONE. MEASURED on SHAKER D1/A's own pin
    // ("1 CSYNC 4us (R2=#2E) VS 2xCSYNC 2us (+R2=#33)"), which is the test for exactly
    // this: the CRTC's pin falls 17 Pixel-M2 into the pulse and rises again at 33, while
    // the end 14.4 arms lands at 34 -- one Pixel-M2 too late, every line. CRTC 1 emitted
    // a single 114 Pixel-M2 pulse where CRTC 0, whose residue puts that end at 33, emits
    // the two 32 Pixel-M2 pulses the test is named after. A one-Pixel-M2 race decided
    // whether the monitor saw one sync or two.
    if (hsyncPinFallIn > 0) endCHsync();
    hsyncPinFallIn = -1;
    cblackHsync = true;
    // ACCC §14.3 (p.133): "WHEN THE GATE ARRAY RECEIVES A HSYNC SIGNAL FROM THE CRTC, IT
    // DISPLAYS BLACK COLOR FOR 2 usec (approximately because it is possible to reduce this
    // period) and then generates a C-HSYNC signal for the monitor whose maximum duration
    // is 4 usec (during this period he stops displaying colors). If the value programmed
    // in R3 is greater than 6, the GATE ARRAY will display black color again for the
    // remaining period."
    //
    // Read as a whole that is 2 + 4 + max(0, R3-6), which comes to R3 for the ordinary
    // R3 >= 6 -- and to TWO MICROSECONDS for everything below it. The 2 usec are the GATE
    // ARRAY's own and do not shorten with R3. Ours ended the black with the CRTC's pin
    // instead, so a short HSYNC blacked only R3 characters, which is most of what SHAKER's
    // AR test (R3 = 0, 1, 2 across its zones) is looking at.
    // 2 + 4 + max(0, R3-6) comes to R3 for R3 >= 6, and to 2 below it -- so the whole
    // rule is max(2, R3l), COUNTED BY THE GATE ARRAY. It does not follow the CRTC's pin
    // at either end, and that is the half that matters: §27.1's re-entrance bug lets
    // CRTC's 1 to 4 hold HSYNC almost indefinitely ("the creation of an infinite HSYNC"),
    // and an R0 rupture does exactly that. Blacking while the pin is high turned SHAKER's
    // A2 and A3 on CRTC 1 -- R0-rupture tests, both comfortably passing -- into 32% of
    // every row black, 86240 pixels against the reference's 1159.
    hsyncBlackRemaining = hsyncWidth > 2 ? hsyncWidth : 2;
    hsyncFallPhase16 = fallPhase16;
    // ACCC §9.3.1 (p.52): "A period, which can be called HSYNC-GA, has a maximum
    // duration capped at 6 µsec and cannot exceed the value defined in R3. For all
    // CPCs without exception (CRTC's 0 to 4) a HSYNC of at least 2 µsec is required
    // for the graphic mode update." §9.3.4 restates the floor from the other side:
    // "as long as this change of mode precedes a HSYNC programmed with a length
    // R3>1", and the R3.JIT case fixes it at "a non-display time of 2.25 µsec".
    if (hsyncWidth < 2) { modeLatchDelay = 0; modeLatchJustArmed = false; return; }
    // The mode is taken at the END of HSYNC-GA. §9.3.2's chronogram (R2=46, R3=14)
    // shows the last OUT whose 3rd µsec lands on C0=51 winning, i.e. 6 µsec after
    // the HSYNC opened on C0=46 — the 6 µsec cap, not R3's 14. §9.3.1: "HSYNC is
    // considered more quickly by the GATE ARRAY on CRTC's 0, 1 and 2, than on the
    // ASIC's of CRTC's 3 and 4. ASIC's delay HSYNC by 1 µsec", and §9.3.3's
    // chronogram duly moves that boundary to C0=52.
    // onHsyncStart() is raised from the end of the character C0=R2, whose own
    // onCharacter() follows immediately and is swallowed by modeLatchJustArmed, so
    // the countdown reaching 0 lands one character earlier than the µsec count.
    modeLatchDelay = std::min(6, hsyncWidth) - 1 + hsyncDelay;
    modeLatchJustArmed = !betweenCharacters;
}

void GateArray::onHsyncStoppedByJit(int elapsedUsec) {
    // ACCC §9.3.4.1 (p.54): "the R3.JIT technique DOES NOT SHORTEN THE PERIOD REQUIRED
    // FOR THE GRAPHICS MODE UPDATE... if this technique is used on the 2nd µsec of the
    // HSYNC, the change of graphics mode does not take place. The change of MODE is
    // considered only on an R3.JIT during the 3rd µsec. This implies a non-display time
    // of 2.25 µsec for a mode update."
    // So a JIT stop always ends the countdown: below the 3rd µsec it throws the pending
    // update away, and from the 3rd it takes it there and then rather than at the
    // microsecond the programmed R3 would have reached.
    if (modeLatchDelay <= 0) return;                  // already taken, or never armed
    modeLatchDelay = 0; modeLatchJustArmed = false;
    if (elapsedUsec >= 3) latchMode(modeSwitchPixelInByte);
}

// ACCC §14.5.4 Note 1: an HSYNC the CRTC withdrew on its first microsecond, before any
// of it reached the screen. Undo what onHsyncStart armed; no end is signalled, so R52 and
// V26 do not count it.
void GateArray::cancelHsyncStart() {
    cblackHsync = false;
    hsyncBlackRemaining = 0;
    hsyncPinFallIn = -1;
    cHsyncFallArmed = false;
    h06Pixels = CHSYNC_FALL_PIXELS;          // past the pulse: H06 raises nothing
    modeLatchDelay = 0; modeLatchJustArmed = false;
}

// An HSYNC cut short by an OUTI's R3 write (ACCC §14.5.4 Note 2, "R3.NJIT (or with
// OUTI)"): an ORDINARY end, just earlier than the R3 the countdown was armed with, so the
// pending mode update is decided as for an HSYNC programmed that long -- taken once it
// has run 2 µsec (the JIT's §9.3.4.1 "2.25 µsec" is that plus its quarter), thrown away
// below. §14.5.4.3 marks "Mode Update" on the OUTI R3=2 row and not on the R3=1 one, and
// SHAKER BI/F (R3=1) and BI/G (R3=2) on a real CPC agree.
void GateArray::onHsyncCutShort(int elapsedUsec) {
    if (modeLatchDelay <= 0) return;
    modeLatchDelay = 0; modeLatchJustArmed = false;
    if (elapsedUsec >= 2) latchMode(modeSwitchPixelInByte);
}

// ACCC §16.2.2 (p.165): this chip runs at 16 MHz and clocks the CRTC down to one
// character per 16 ticks. H06 is a Pixel-M2 counter, so the C-HSYNC edges land where
// the chapter puts them rather than on the CRTC's character grid.
void GateArray::clockPixel() {
    h06Pixels += 1;
    if (h06Pixels == CHSYNC_RISE_PIXELS) { sigGaHsync = true; cHsyncStarted = true; }
    else if (h06Pixels == CHSYNC_FALL_PIXELS) endCHsync();

    // §16.2.2's other end condition, reached first whenever R3l < 6: the CRTC's HSYNC
    // pin falls part-way through the character AFTER the one on which the CRTC raised
    // its end, by §14.4's per-chip residue.
    if (hsyncPinFallIn > 0) {
        hsyncPinFallIn -= 1;
        if (hsyncPinFallIn == 0) { hsyncPinFallIn = -1; endCHsync(); }
    }
    if (onPixel) onPixel();
}

// The pulse ends, and its width -- in Pixel-M2, measured, not assembled from whole
// characters plus a residue -- goes to whoever is driving a sweep off it.
void GateArray::endCHsync() {
    if (!sigGaHsync) return;
    sigGaHsync = false;
    cHsyncEnded = true;
    if (tipTraceBudget > 0) {
        std::fprintf(stderr, "CHSYNC end h06=%4d width=%3d\n",
                     h06Pixels, h06Pixels - CHSYNC_RISE_PIXELS);
        tipTraceBudget -= 1;
    }
    cHsyncWidth16 = h06Pixels - CHSYNC_RISE_PIXELS;
    if (cHsyncWidth16 < 0) cHsyncWidth16 = 0;
    if (cHsyncWidth16 > 64) cHsyncWidth16 = 64;
}

void GateArray::onCharacter() {
    // §27.6.1: the INT an HSYNC end asked for comes up one microsecond after it.
    if (interruptRaiseIn > 0 && --interruptRaiseIn == 0)
        asic->raiseGateArrayInterrupt(cpu, !plusHardware);
    // §14.3's max(2, R3l) characters of black, counted down by the GATE ARRAY itself.
    if (hsyncBlackRemaining > 0 && --hsyncBlackRemaining == 0) cblackHsync = false;
    // ACCC §16.2.3, "If CRTC Transition Character": H06++, and SIG_GA_HSYNC is HIGH
    // from H06==2 to H06==6 -- the GATE ARRAY's own 4 µsec horizontal pulse, which it
    // times itself because it cannot see C3l. A programmed HSYNC shorter than the
    // threshold never raises it at all (§16.2.2: "active ... if R3l>=2"), and one
    // longer than 6 is cut off here rather than at the CRTC's end.
    // One CRTC character is 16 of this chip's own clock ticks (§16.2.2). Everything
    // H06 times happens in there, at Pixel-M2 resolution.
    for (int i = 0; i < PIXELS_PER_CHARACTER; i += 1) clockPixel();
    if (modeLatchDelay <= 0) return;
    if (modeLatchJustArmed) { modeLatchJustArmed = false; return; }
    modeLatchDelay -= 1;
    // ACCC §9.3.4: the update lands part-way through the byte the GA is processing,
    // not at its start, so the switch point travels with the latch.
    if (modeLatchDelay == 0) latchMode(modeSwitchPixelInByte);
}

void GateArray::onVsyncStart(bool hsyncActive, bool gatedByCrtcVsync) {
    interruptSyncCount = (hsyncActive && interruptCounter > 50) ? 1 : 2;
    // ACCC §16.2.3, "If VSYNC-CRTC Transition OFF -> ON": V26=0, VSYNC_GA=true,
    // CBLACK_VSYNC=true. From here the CRTC's VSYNC pin is not consulted again --
    // "the CRTC VSYNC signal state is no longer used thereafter, except for CRTC's 3
    // and 4" (§16.2.2) -- the GATE ARRAY runs the whole thing off V26.
    v26 = 0;
    vsyncGa = true;
    cblackVsync = true;
    sigGaVsync = false;                 // rises when V26 reaches 2, not here
    // §16.2: the C-VSYNC runs from the end of the 2nd to the end of the 6th CRTC HSYNC.
    // On CRTC 3 and 4 it also needs the CRTC's VSYNC pin (onCrtcVsyncPin), which is how
    // p.164's R3h cases come out: R3h=2 gives a signal from the end of the HSYNC of line
    // C9=1 to the next C0=0 (11 usec in its example), R3h=3 a line more ("increases the
    // duration of the signal by R0 usec"), R3h=1 none -- the pin is down by V26=2. §16.4.4
    // (p.171) "at least 3 lines for the C-VSYNC monitor signal to be generated" is the same
    // fact seen from the monitor: under §16.2.4's 11-12 usec a CTM cannot anchor on it,
    // and the monitor model applies that threshold itself.
    cVsyncGatedByCrtc = gatedByCrtcVsync;
    crtcVsyncPin = true;
}

void GateArray::onCrtcVsyncPin(bool high) {
    crtcVsyncPin = high;
    if (cVsyncGatedByCrtc && !high) sigGaVsync = false;   // "stops when C0 returns to 0"
}

// The C-HSYNC half of "HSYNC-CRTC Transition ON -> OFF" on its own: the pin falls this
// chip's residue past the end of the character, exactly as onHsync would arm it, for a
// CRTC whose HSYNC end the rest of this chip only hears about late.
void GateArray::armCHsyncFall() {
    hsyncPinFallIn = PIXELS_PER_CHARACTER + hsyncFallPhase16;
    cHsyncFallArmed = true;
}

void GateArray::onHsync(Z80* cpu) {
    // ACCC §16.2.3, "If HSYNC-CRTC Transition ON -> OFF": the GATE ARRAY drops its own
    // horizontal signal and the HSYNC black with it, then services V26. So C-HSYNC
    // ends at whichever comes first, H06==6 or the CRTC's own end.
    // §14.4: the pin falls this chip's own number of Pixel-M2 past the end of the
    // character the CRTC raised this on -- and this runs BEFORE that character's 16
    // ticks, so the wait is a whole character plus the residue. With a residue of 0
    // (the ASIC's) that is the character boundary exactly, which is where the old
    // character-granular model put it.
    if (cHsyncFallArmed) cHsyncFallArmed = false;      // the host armed it on time already
    else hsyncPinFallIn = PIXELS_PER_CHARACTER + hsyncFallPhase16;
    // The CRTC's pin dropping does not end the black: §14.3 gives the GATE ARRAY its own
    // count, and onCharacter below is what runs it out. A pin that drops early leaves the
    // rest of the 2 usec standing; one that never drops does not extend it.
    if (hsyncBlackRemaining <= 0) cblackHsync = false;
    if (v26 >= 0) {                     // ACCC §16.1: V26 counts the ends of HSYNCs
        v26 += 1;
        if (v26 == 2) {                                   // C-VSYNC begins...
            if (!cVsyncGatedByCrtc || crtcVsyncPin) { sigGaVsync = true; monitorSyncStarted = true; }
        }                                                 // ...if the pin allows it (CRTC 3/4)
        else if (v26 == 6) sigGaVsync = false;
        if (v26 >= 26) { cblackVsync = false; vsyncGa = false; v26 = -1; }   // §16.2.1
    }
    // ACCC §27: an RMR reset on the HSYNC's last microsecond beats this increment.
    if (suppressNextR52Increment) suppressNextR52Increment = false;
    else interruptCounter += 1;
    // The INT itself is raised by onCharacter a microsecond later (interruptRaiseIn).
    if (cpu) this->cpu = cpu;
    if (interruptSyncCount == 0) {
        if (interruptCounter >= 52) {
            interruptCounter = 0;
            if (!asic->rasterInterruptEnabled()) interruptRaiseIn = 2;
            asic->clearShadowInterrupt();
        }
    } else {
        interruptSyncCount -= 1;
        if (interruptSyncCount == 0) {
            if (interruptCounter >= 32 && !asic->rasterInterruptEnabled()) interruptRaiseIn = 2;
            interruptCounter = 0;
        }
    }
}

} // namespace cpcse
