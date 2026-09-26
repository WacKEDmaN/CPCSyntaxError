// CPCSyntaxError — 6845 CRTC core.
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
#include "crtc.h"
#include <stdexcept>
#include <cstdio>
#include <cstdlib>

namespace cpcse {

const CrtcBehaviour* crtcTypeFor(int type) {
    switch (type) {
        case 0: return &crtcType0();
        case 1: return &crtcType1();
        case 2: return &crtcType2();
        case 3: return &crtcType3();
        case 4: return &crtcType4();
        case 5: return &crtcType1b();   // ACCC §11.6: CRTC 1-B (UM6845R with RFD#10)
        default: return nullptr;
    }
}

CRTC6845::CRTC6845(const Crtc6845Options& options) {
    onHsync = options.onHsync ? options.onHsync : []() {};
    onVsyncStart = options.onVsyncStart ? options.onVsyncStart : []() {};
    onFrame = options.onFrame ? options.onFrame : [](int) {};
    onScanline = options.onScanline ? options.onScanline : [](int) {};
    onSelect = options.onSelect ? options.onSelect : [](int) {};
    onHsyncStart = options.onHsyncStart ? options.onHsyncStart : []() {};
    onHsyncCancelled = options.onHsyncCancelled ? options.onHsyncCancelled : []() {};
    onCharacterStart = options.onCharacterStart ? options.onCharacterStart : []() {};
    onCharacter = options.onCharacter ? options.onCharacter : []() {};
    getHorizontalScroll = options.getHorizontalScroll ? options.getHorizontalScroll : []() { return 0; };
    getSplit = options.getSplit ? options.getSplit : []() { return std::optional<CrtcSplit>(); };
    onLightgunBeam = options.onLightgunBeam ? options.onLightgunBeam : [](int, int) {};
    type = 3; behaviour = &crtcType3();
    reset();
}

void CRTC6845::setType(int type_) {
    const CrtcBehaviour* b = crtcTypeFor(type_);
    if (!b) throw std::range_error("CRTC type must be 0, 1, 2, 3, 4 or 5 (CRTC 1-B).");
    type = type_; behaviour = b;
    for (int reg = 0; reg < 16; reg += 1) registers[reg] &= (uint8_t)writeMask(reg);
    registers[0] = (uint8_t)behaviour->normaliseRegister(0, registers[0]);
}

void CRTC6845::reset() {
    bool savedLightgunEnabled = lightgunEnabled;
    std::string savedLightgunType = lightgunType;
    registers = std::array<uint8_t, 18>{ 63, 40, 46, 0x8e, 38, 0, 25, 30, 0, 7, 0, 0, 0x30, 0, 0, 0, 0, 0 };
    rawRegisters = registers;
    for (int reg = 0; reg < 16; reg += 1) registers[reg] &= (uint8_t)writeMask(reg);
    selected = 0; charClockRemainder = 0;
    horizontal = vertical = raster = vsyncCounter = verticalAdjust = 0; ivmRaster = 0;
    horizontalTotalMatch = false;
    scanlineInFrame = 0;
    hsyncCounter = 0; hsync = vsync = vblank = false; hsyncOverflow = false;
    r7Match = false; r4Match = false; r9Match = false;
    verticalAdjustActive = false; rasterMatchForced = false; lpenStrobe = false; updateReady = true;
    adjustmentInterlaceExtra = 0; adjustLine = 0; verticalStepForced = false; verticalStepSuppressed = false; adjustStateEngaged = false;
    adjustLengthLatched = registers[5] & 0x1f;
    r9Previous = registers[9]; r5Previous = registers[5]; r0Previous = registers[0]; r8Previous = registers[8]; lastWriteBlockIo = false;
    hsyncEndedThisCharacter = false; r3WrittenThisCharacter = false; r2WrittenThisCharacter = false; horizontalOverflowed = false; r7WrittenThisCharacter = false;
    r3JitPending = false; hsyncEndedJit = false;
    adjustFromFrameStart = false; adjustBlocksRegisterReload = false;
    ivmExitDeferred = false; r1ReloadPending = false; rfdActive = false; rfdIgnoresParity = false; rfdParityLocked = false; lastLineEnlarged = false; interlaceLineNow = false; r0EnlargeStepsVertical = false; adjustEngagedWithoutR5 = false; rasterParity = 0; interlaceFourthMicrosecond = -1; borderR6Status = false; ivmTestDeferred = false; parityR6 = 1; midVsyncParity = 0; r1ZeroLate = false;
    verticalMatchForced = false; vsyncGhost = false; lastLineBlocked = false;
    lastFrameLine = false; lastLineManagement = false; lastPreviousLine = false;
    addressFrozenForAdditionalLine = false; verticalAdjustState = false;
    vsyncHalfLinePending = false; vsyncFullLinePending = false; vsyncSkipsFirstLineEnd = false;
    hDisplay = registers[1] != 0;
    hDisplayInternal = hDisplay; skewHistory = 0;
    vDisplay = behaviour->initialVerticalDisplay(*this); frame = 0;
    requestedAddress = screenAddress();
    frameAddress = requestedAddress; rowAddress = frameAddress; nextRowAddress = rowAddress; maRow = rowAddress; vlc = 0;
    interlaceField = 0;
    revision = revision + 1;
    lightgunEnabled = savedLightgunEnabled; lightgunType = savedLightgunType; lightgunTriggered = false;
    // lightgunX/Y/ViewWidth/Height persist across reset (member state).
    lightgunAddress = frameAddress; lightgunLatched = false;
}

// ACCC §12.5, §13.3: a CRTC applies a register write immediately -- "the modification
// of register 4 is considered immediately at the end of the line", and §13.3's note 3
// explains the apparent latency as the microsecond at which the Z80A's I/O reaches the
// chip, not a latch inside it. That microsecond is §4.4.3's per-instruction offset,
// which the host applies before calling in (GX4000::cpcIoEffectOffset). There is
// therefore no one-character write latch here; the `delayed` flag and the pendingWrite
// slot that used to carry one were dead -- no chip profile ever asked for them.
void CRTC6845::write(int port, int value) {
    lastWritePort = port;
    if ((port & 0x0100) == 0) { selected = value & 0x1f; onSelect(value & 0xff); }
    else if (selected < 16) writeRegister(selected, value);
}

void CRTC6845::writeRegister(int reg, int value) {
    if (reg < 0 || reg >= 16) return;      // R16/R17 are the light pen's, read-only

    // CPCSE_TRACE_REG=<n> logs every write of register n with the counters it lands on,
    // to stderr, capped at CPCSE_TRACE_REG_MAX (default 400) so a per-line technique does
    // not fill the disk.
    //
    // This exists because reading the program is not enough: DSC4 drives its CRTC from
    // tables through OUTI, so a static scan for "LD BC,&BC05" finds nothing and concludes
    // -- wrongly -- that the demo never writes R5. The trace shows 199 R5 writes. It is
    // also what revealed that DSC4's CRTC-0 route rewrites R9 on EVERY line at C0=42,
    // always to C9+1, where its CRTC-1 route writes R9 twice in a whole run.
    // A harness may re-aim traceRegister/traceRegisterBudget at the moment it cares
    // about; the environment only seeds them.
    if (traceRegister == -2) {
        // CPCSE_TRACE_REG takes a COMMA LIST now, not one register: "4,7,9,12" logs all
        // four interleaved in program order. A rupture is a conversation between several
        // registers -- Pinball Dreams' CRTC-1 route rewrites R4, R9, R7 and R12/R13
        // together -- and watching them one at a time cannot show which write lands
        // between which two others.
        const char* want = std::getenv("CPCSE_TRACE_REG");
        traceRegister = -1;
        traceRegisterMask = 0;
        for (const char* c = want; c && *c; c += 1) {
            if (*c < '0' || *c > '9') continue;
            int n = std::atoi(c);
            if (n >= 0 && n < 32) traceRegisterMask |= 1u << n;
            while (c[1] >= '0' && c[1] <= '9') c += 1;
        }
        const char* cap = std::getenv("CPCSE_TRACE_REG_MAX");
        traceRegisterBudget = cap ? std::atol(cap) : 400;
        const char* from = std::getenv("CPCSE_TRACE_REG_FROM");
        traceRegisterFrom = from ? std::atol(from) : 0;
    }
    if (reg >= 0 && reg < 32 && (traceRegisterMask & (1u << reg)) != 0
        && traceRegisterBudget > 0 && frame >= traceRegisterFrom) {
        std::fprintf(stderr, "f%-6d R%-2d<-%3d  C0=%2d C4=%3d C9=%d  pc=%04X\n",
                     frame, reg, value & 0xff, horizontal, vertical, raster, tracePc & 0xffff);
        traceRegisterBudget -= 1;
    }
    int r3PreviousLow = registers[3] & 0x0f;   // ACCC §14.5.4: R3.JIT needs the old R3l
    int r8Incoming = registers[8];             // ACCC §19.5: the parity rules need the old R8
    const int skewIncoming = behaviour->dispenSkew(*this) & 3;   // ACCC §19.2.5
    if (reg >= 0 && reg < (int)rawRegisters.size()) rawRegisters[reg] = (uint8_t)(value & 0xff);
    // ACCC §13.2.1: while the vertical counters are frozen (CRTC0 with R0=0) the
    // chip no longer takes R4/R5/R9 updates into account.
    if (behaviour->ignoresRegisterWrite(*this, reg, value)) return;
    registers[reg] = (uint8_t)behaviour->normaliseRegister(reg, value);
    revision += 1;
    if (reg == 0) {
        // ACCC §13.3 note 3: on the UM6845R a BLOCK I/O write landing on the character
        // C0 has just wrapped into takes the wrap back -- "C0 will not be equal to 0 but
        // to 50" when R0 was 49. Every line start is that wrap, so the position of the
        // write is the whole condition, exactly as the chapter frames it ("if R0 is
        // modified on the last µsecond of the OUTI instruction").
        // Including a previous R0 of 0, where every character is that wrap: §13.6.2's
        // "Previous R0=0" rows read C0=1 on the OUTI's own write microsecond (the OUT's
        // reads the bold 0 and only the next one 1). An exclusion for r0Previous == 0
        // stood here unexplained; it was cancelling the interrupt being a microsecond
        // early (GateArray::interruptRaiseIn) on SHAKER B6/A's "OUTI ON C0=0,R0=0" line.
        if (lastWriteBlockIo && horizontal == 0
            && behaviour->unwrapsC0OnBlockR0Write())
            horizontal = (r0Previous + 1) & 0xff;
        horizontalTotalMatch = behaviour->horizontalCounterAtTotal(*this);
    }
    if (reg == 12 || reg == 13) {
        requestedAddress = screenAddress();
    }
    // ACCC §11.3 UPDATING R5 DURING AN ADJUSTMENT: "Whatever the CRTC, if R5 is
    // modified with C5+1 (or C9+1 on CRTC's 0, 3, 4) on line C5 (or C9), then the
    // vertical adjustment is stopped... If R5 is modified with a value less than
    // C5+1, then the counter overflows and continues to count up to 0 to reach the
    // new R5 value." CRTC 0 reads R5 live in the adjustment branch instead (§11.2.2).
    // §11.3's live R5 update. The chips on the general vertical path carry a DOWN
    // counter, so the remaining line count has to be recomputed here; CRTC 1 counts UP
    // and re-reads the limit from R5 every line, so for it the rule needs no help --
    // and writing a down-counter value into its up-counter would corrupt the count.
    if (reg == 5 && verticalAdjust > 0 && !behaviour->adjustmentCountsRasterToR5()
        && !behaviour->adjustmentLimitReadLive()) {
        int limit = (registers[5] & 0x1f) + adjustmentInterlaceExtra;
        // ACCC §11.3.3 (p.87, CRTC 3 and 4): "If R5 is modified with a value below
        // C9+1, then the line is considered the last and additional management ends...
        // Whether with R5 or R9, IT IS IMPOSSIBLE TO OVERFLOW C9." On CRTC 1 and 2 the
        // same write overflows C5 and counts all the way round instead (§11.3.1/2).
        if (behaviour->adjustmentCannotOverflow() && limit <= adjustLine) verticalAdjust = 1;
        else verticalAdjust = (limit - adjustLine) & 0x1f;
    }
    if (reg == 6 && vertical == registers[6] && behaviour->considersR6WriteImmediately()
        && !(behaviour->r6FirstLineWriteIsConflict() && vertical == 0 && raster == 0)) {
        vDisplay = false;      // ACCC §18.2.2/§18.2.3; not on CRTC 3/4 (§18.2.4)
    }
    // ACCC §17.4 (p.180): "The condition C0=R1 is considered IMMEDIATELY on a line. It
    // can occur several times on the same line if R1 is reprogrammed... If R1 is
    // updated again during the line to meet the condition C0=R1 when C9=R9 then this
    // will cause an update of the video pointer. In other words, the modification of
    // R1 during the BORDER R1 display allows for the video pointer to be updated
    // without the data being [displayed]."
    // ...except for R1=0, which §17.5 governs on its own: on CRTC 3 and 4 it is taken only
    // when written on C0=R0 ("NO BORDER, update of R1 not considered (too late)" on C0=0),
    // so an R1=0 landing ON C0=0 is not a C0=R1 there. On CRTC 0, 1 and 2 C0=0 is still in
    // time and the two rules agree.
    const bool r1ZeroTooLate = reg == 1 && registers[1] == 0 && !behaviour->acknowledgesR1ZeroNow(*this);
    if (reg == 1 && horizontal == registers[1] && !r1ZeroTooLate) {
        const bool pinBefore = hDisplay;
        behaviour->reloadNextRowAddress(*this);
        hDisplay = false;                       // "there is no longer any display"
        // ...on the chip's own signal: through §19.2's delay the pin follows `skew`
        // characters later, so only an unskewed pin drops here and now.
        if (behaviour->skewIsDelayLine()) {
            hDisplayInternal = false;
            const int skew = behaviour->dispenSkew(*this);
            if (skew != 0 && skew != 3) hDisplay = pinBefore;
        }
    }
    // ACCC §17.5 ACKNOWLEDGMENT R1=0: past the per-chip deadline the BORDER this write
    // would raise is not considered until the next line.
    if (r1ZeroTooLate) r1ZeroLate = true;
    // ACCC §19.5.3: the 3rd µsecond of the OUT now, the 4th one character later.
    if (reg == 8) {
        behaviour->onInterlaceWritten(*this, r8Incoming);
        interlaceFourthMicrosecond = r8Incoming;
    }
    // ACCC §19.2.5 DISINTEGRATION OF THE BORDER ON CRTC 0 (p.197-198), R0=R1=63: a delay
    // taken away is "considered immediately" -- R8=#00 landing on C0=0 after a +1 cancels
    // the BORDER the C0=R1 condition had reported there (19.2.5.2), and landing on C0=63
    // it leaves that character BORDER (19.2.5.1). A delay added is not: R8=#10 landing on
    // C0=63 "is not fast enough to prevent the BORDER from being activated. However, this
    // programming is considered for the following character" (19.2.5.4) -- which the pin,
    // chosen at the character's start, already does. So a lowered skew re-taps the delay
    // line for THIS character: 0 is the undelayed signal, 1 the character before it.
    if (reg == 8 && behaviour->skewIsDelayLine()) {
        const int skewBefore = skewIncoming == 3 ? 0 : skewIncoming;
        int skewNow = behaviour->dispenSkew(*this) & 3;
        if (skewNow == 3) skewNow = 0;
        if (skewNow < skewBefore)
            hDisplay = skewNow == 0 ? hDisplayInternal : ((skewHistory >> 1) & 1) != 0;
    }
    // ACCC §19.8.1: entering IVM defers the doubled-C9 comparison to the next C0=0.
    if (reg == 8 && displayMode() == 3) ivmTestDeferred = true;
    // ACCC §19.8.1 (p.221): "When R8 returns to 0, the same mechanics apply. The state
    // set with R8 is considered when C0 returns to 0. The test takes place with C9.VMA
    // (which includes parity) and R9: ParityC9 is no longer considered for R9."
    if (reg == 8 && displayMode() != 3 && displayModeOf(r8Previous) == 3) ivmExitDeferred = true;
    if (reg == 2) {
        r2WrittenThisCharacter = true;   // ACCC §14.7.1 (R2.JIT)
        if (horizontal > (registers[2] & 0xff)) traceHsync("R2 written BEHIND C0");
        // ACCC §14.7.1 (p.142): "When C0=R2 then a HSYNC is generated over a length of R3
        // characters. R2 update occurs during the 3rd µsec of the OUT(C),reg8
        // instruction... IF THE UPDATE OF R2 OCCURS WHILE C0=R2 (during the 3rd µsec of an
        // OUT(C),r8) THEN THE CRTC SENDS THE HSYNC SIGNAL LATER TO THE GATE ARRAY. This
        // reflects a delay between the execution of the instruction in Z80A and the
        // consideration by the CRTC."
        //
        // That is the R2.JIT: R2 written with the value C0 already holds still raises the
        // HSYNC. checkHsync() runs once per character, from tickCharacter, and for this
        // character it has already been and gone -- so without raising it here the HSYNC
        // is simply lost. Which is what happened: a mid-line HSYNC programmed this way
        // never appeared, and with it went the mid-line graphic-mode latch it exists to
        // carry (§9.3.4's mode splitting). The renderer already knew about the technique
        // -- r2WrittenThisCharacter feeds §14.7.1's later black-zone position -- but
        // nothing ever started the sync.
        if (horizontal == registers[2]) {
            traceHsync("R2 written at C0==R2");
            hsyncArmedBetweenCharacters = true;
            checkHsync();
            hsyncArmedBetweenCharacters = false;
        }
    }
    if (reg == 3) r3WrittenThisCharacter = true;   // ACCC §15.3.3
    // ACCC §14.5.4 (p.139) R3.JIT: R3l set to exactly the running C3l, from a value
    // that was greater, stops the HSYNC here — and the GATE ARRAY then holds its black
    // 0.25 µsec past the normal end (§9.3.4.2). Arm it for the end of this HSYNC.
    // §14.5.4 also: "The use of OUTI does not allow this technique to be used on
    // these CRTC's" -- it must be an OUT(C),r8.
    // "...WHILE R3l WAS GREATER THAN THIS VALUE" -- a write that merely restates the
    // value the counter has already passed is not the technique.
    if (reg == 3 && hsync && !lastWriteBlockIo && behaviour->supportsR3Jit()
        && (registers[3] & 0x0f) == hsyncCounter && r3PreviousLow > hsyncCounter)
        r3JitPending = true;
    // ...but "does not allow this technique" is about the JIT, not about the stop. §14.5.4.3
    // (p.141) charts the OUTI too: "5th µs OUTI (I/O R3=1)" on C0=R2+1 ends the HSYNC
    // exactly where an HSYNC programmed with R3=1 from the start ends -- black from the
    // 6th Pixel-M2 of C0=R2 to the 5th of C0=R2+1, no 0.25 µsec extension -- and Note 2
    // names the case: "in R3.NJIT (or with OUTI)". The OUTI's write lands at the start of
    // that character, so the sync ends right here, as an ordinary end. Without this the
    // write matched nothing, C3 ran on to 15 and round, and SHAKER BI/F-H drew an eight-
    // character black box where the real CPC has a one-character bar.
    if (reg == 3 && hsync && lastWriteBlockIo && behaviour->supportsR3Jit()
        && (registers[3] & 0x0f) == hsyncCounter && hsyncCounter != 0
        && r3PreviousLow > hsyncCounter) {
        hsync = false; hsyncEndedThisCharacter = true; hsyncEndedJit = false;
        hsyncEndedByBlockWrite = true;
        onHsync();
        hsyncEndedByBlockWrite = false;
    }
    // ACCC §14.5.3 (p.138), CRTC 3 and 4: the chip's own C3 starts one character AFTER
    // C0=R2 -- its first 0 sits under C0=R2+1, where CRTC 0 and 2's sits under C0=R2
    // (§14.5.1) -- because the ASIC "synchronize[s] the HSYNC with the display" (§14.5.4).
    // Here the counter starts on C0=R2 like every other chip's and the lag is applied to
    // the pin downstream (hsyncDisplayDelayCharacters, §27.6.5), which gives the same
    // HSYNC on every line the program leaves alone. A mid-HSYNC R3 write is where the
    // two part: the chip compares the new R3l with ITS counter, which reads C3 - lag.
    // A value above that and not above C3 is one the chip has still to reach, and it
    // ends the HSYNC on reaching it -- at the next character boundary of its own count,
    // which is NOW on ours, the lag then carrying the end to where the chip puts it.
    // §14.5.3's rows: OUT R3,5 on its C3=4 ends at 5 (ours reads 5 there: end now);
    // R3,4 equals its counter and overflows (ours reads 5: below, overflow); R3,6 ends
    // at 6 (ours: above, the ordinary compare). SHAKER BI/B on CRTC 3 ("R2=R3=14 / R3=1
    // ON C0io=15") is the photographed case: a one-character bar, where letting C3 run
    // round drew a seventeen-character box.
    {
        const int lag = behaviour->hsyncDisplayDelayCharacters();
        const int v = registers[3] & 0x0f;
        if (reg == 3 && hsync && lag > 0 && hsyncCounter >= lag && v <= hsyncCounter && v > hsyncCounter - lag) {
            // An ordinary end, only earlier than the R3l the GATE ARRAY armed its mode
            // countdown with: the update needs "a HSYNC of at least 2 usec" (§9.3.1),
            // and this one ran the chip's C3 -- one behind ours -- plus the character
            // it ends on: C3 usec. The same decision as §14.5.4 Note 2's OUTI end, so
            // the same route to it. SHAKER BI/B's photo keeps its text in the old mode
            // after the 1 usec bar; running the countdown on to 6 switched it.
            hsync = false; hsyncEndedThisCharacter = true; hsyncEndedJit = false;
            hsyncEndedByBlockWrite = true;
            onHsync();
            hsyncEndedByBlockWrite = false;
        }
    }
    // An R3 written on C0=R2 can START the HSYNC that R3=0 withheld there (§14.6: "When
    // R3=0, CRTC's 0 and 1 do not produce HSYNC"). It does not RESTART one already
    // running -- nothing in §14.4-§14.7 zeroes C3 or re-sends the HSYNC to the GATE ARRAY
    // on an R3 write. Doing so re-armed H06 a character late: SHAKER DH/B1 writes R3=6 on
    // the C0=R2=0 that has just raised the band's line sync, and the C-HSYNC came out
    // 1 usec late, knocking the monitor 20 px sideways and back (slanted bars on CRTC 2,
    // whose real chip draws them straight).
    if (reg == 3 && horizontal == registers[2]) {
        if (effectiveHsyncWidth() != 0) {
            if (!hsync) { hsync = true; hsyncCounter = 0; onHsyncStart(); }
        }
        else if (hsync && hsyncCounter == 0) {
            hsync = false;
            // ACCC §14.5.4 Note 1 (p.139): "Interrupting R3 with 0 using an OUTI PREVENTS
            // THE HSYNC FROM STARTING" -- §14.5.4.3's first row has no black at all. The
            // GATE ARRAY was told the sync had started at the character boundary, so it has
            // to be told it never did: no black, no C-HSYNC, no mode update, no R52 step.
            // Dropping the pin alone left its mode countdown running, and SHAKER BI/E
            // switched mode mid-line where the real CPC's text runs on untouched.
            if (lastWriteBlockIo) onHsyncCancelled();
        }
    }
    if (reg == 7) {
        // ACCC §16.2.1 (p.161): R7 written on this very character is the R7.JIT case, and
        // it moves where inside the character the GATE ARRAY opens its VSYNC black --
        // "on this chip an R7.JIT starts the black on the 5th Pixel-M2 of the word of the
        // C0 where R7=C4, one character later than the programmed-in-advance case". The
        // renderer has always asked for that (video.cpp reads r7WrittenThisCharacter to
        // pick vsyncBlackStartPixelJit) but nothing ever set the flag, so the JIT
        // position could not happen. Same shape as R2.JIT: consumer present, producer
        // missing.
        r7WrittenThisCharacter = true;
        // ACCC §16.3 (p.168), the FIRST of the two VSYNC protections: "The first consists
        // in IGNORING THE COMPARISON OF C4 WITH R7 WHEN R7 IS MODIFIED DURING VSYNC. It is
        // not possible to trigger or inhibit a VSYNC during a VSYNC. Thus, modifying the
        // value of R7 with A VALUE OF C4 REACHED DURING THE VSYNC DOES NOT CAUSE A NEW
        // VSYNC. Modifying R7 with a C4 value different from the initial C4 value does not
        // interrupt the current VSYNC."
        //
        // So while the pin is up this write neither arms the comparison nor disarms it --
        // the latch keeps whatever it had. SHAKER's DT page 3 is built on exactly this and
        // prints its own answers: with R7=#0A and the VSYNC running, switching R7 to #0A,
        // #0B or #0C -- all values C4 passes through DURING those sixteen lines -- expects
        // REP0D, i.e. no second VSYNC; switching it to #0D, which C4 only reaches after the
        // VSYNC has ended, expects REP10, i.e. a second one. Clearing the latch here made
        // the first three fire as well.
        if (vsync) { /* §16.3: the comparison is ignored for the duration */ }
        else if (vertical != registers[7]) r7Match = false;
        else if (behaviour->startsVsyncOnR7Write(*this) && !r7Match) startVsyncIfNeeded(true);
    }
    behaviour->onRegisterWritten(*this, reg);
    if (reg == 4 || reg == 9) behaviour->onLastLineRegisterWritten(*this, reg);
}

int CRTC6845::read(int port) {
    int operation = port >> 8 & 3;
    if (operation != 1 && operation != 2 && operation != 3) return 0xff;
    return behaviour->read(*this, operation);
}

void CRTC6845::setLightgunType(const std::string& type_) {
    lightgunType = (type_ == "trojan" || type_ == "gunstick" || type_ == "westphaser") ? type_ : "trojan";
    lightgunTriggered = false; lightgunLatched = false; lpenStrobe = false;
    updateTrojanLightgun();
}
void CRTC6845::setTrojanLightgunEnabled(bool enabled) {
    lightgunEnabled = enabled;
    if (!lightgunEnabled) {
        lightgunTriggered = false; lightgunLatched = false; lpenStrobe = false;
    }
}
void CRTC6845::setTrojanLightgun(double x, double y, bool trigger, int viewWidth, int viewHeight) {
    if (!lightgunEnabled) return;
    lightgunX = x; lightgunY = y;
    lightgunViewWidth = viewWidth ? viewWidth : LIGHTGUN_VIEW_WIDTH;
    lightgunViewHeight = viewHeight ? viewHeight : LIGHTGUN_VIEW_HEIGHT;
    lightgunTriggered = trigger;
    if (!trigger) lightgunLatched = false;
    updateTrojanLightgun();
}
void CRTC6845::releaseTrojanLightgun() { lightgunTriggered = false; lightgunLatched = false; updateTrojanLightgun(); }
void CRTC6845::refreshTrojanLightgun() {
    if (lightgunType != "trojan" || !lightgunTriggered) return;
    lightgunLatched = false;
    updateTrojanLightgun();
}
// The LPEN pin going low-to-high. UM6845R data sheet (LPEN High/Low): "When the LPEN
// input changes from low to high, on the next negative-going edge of CCLK the contents
// of the internal scan counter are stored in registers R16 and R17", and its Light Pen
// Strobe Timing note pins down WHICH contents -- the safe strobe position "cause[s]
// address n+2 to load into Light Pen Register", two characters ahead of the MA on the
// bus when the strobe arrived. So the register takes the chip's own counter; it is not
// a position worked back out of R1, R2, R7 and R12/R13.
void CRTC6845::strobeLightPen() {
    lightgunAddress = (maRow + 2) & 0x3fff;
    registers[16] = (uint8_t)(lightgunAddress >> 8 & 0x3f);
    registers[17] = (uint8_t)(lightgunAddress & 0xff);
    lpenStrobe = true;
}
void CRTC6845::updateTrojanLightgun() {
    if (!lightgunEnabled) return;
    // Releasing the trigger drops the pin; the strobe itself is raised by the beam
    // reaching the pen, in onLightgunBeam, because that is when a light pen fires.
    if (lightgunType != "trojan" || !lightgunTriggered) lpenStrobe = false;
}
// The HSYNC length the chip actually runs, which is R3l as each silicon reads it
// (ACCC §14.1, §14.6). There is nothing else in here: the per-demo signature
// overrides this used to carry -- RAM byte-patterns identifying Scratch, Tomate and
// Hi-Tech, which then forced R3l to 14 or 13 -- were horizontal-centring fudges from
// the era before §14.4's C-HSYNC positioning and §15.7's convergence were modelled,
// and they are not in the compendium.
int CRTC6845::effectiveHsyncWidth() { return behaviour->hsyncWidth(*this); }

// The CURSOR pin. Three comparators and a blink divider, exactly as the UM6845R data
// sheet's register descriptions state them; nothing here is CPC-specific, because the
// pin is not wired on a CPC and no chapter of the ACCC modifies it.
bool CRTC6845::cursorOutput() {
    const int mode = (registers[10] >> 5) & 3;
    if (mode == 1) return false;                        // "No Cursor"
    if (memoryAddress() != cursorAddress()) return false;
    // The RA0-RA4 PINS, five bits -- not videoRaster(), the CPC's three-bit address field,
    // which folds C9=8 onto 0 and lit the cursor on a raster the chip is not driving
    // (found by cpcse-logic-check on an HD6845S IVM frame whose C9.VMA passes 7).
    const int scanline = rasterOutput();
    // "the scan line counter (RA lines) falls within the bounds set by R10 and R11".
    // With a start above the end there is no scan line inside the bounds and the pin
    // stays low; the data sheet describes no wrap.
    if (scanline < (registers[10] & 0x1f) || scanline > (registers[11] & 0x1f)) return false;
    if (mode == 2) return (fieldCounter & 8) != 0;      // "Blink at 16 x field period"
    if (mode == 3) return (fieldCounter & 16) != 0;     // "Blink at 32 x field period"
    return true;                                        // "No Blinking" -- steady on
}

bool CRTC6845::displayOutputEnabled() {
    if (!hDisplay || !vDisplay) return false;
    return behaviour->displayEnabled(*this);
}
// R9 as the end-of-character comparison sees it. It is R9 itself: every chip states
// its own IVM test and none of them wants R9's bit 0 forced on.
//   §19.8.1 (CRTC 0):  "If ((C9 x 2) or ParityFrame) == (R9 + ParityFrame)"
//   §19.8.2 (CRTC 1):  "If (C9 and %11110) == (R9 and %11110)" -- parity masked off
//                      BOTH sides, so a forced bit 0 is discarded anyway
//   §19.8.3 (CRTC 2):  "C9 is compared with R9 in a CONVENTIONAL WAY to process C4"
//   §19.8.4 (CRTC 3/4):"If C9 >= R9"
// Forcing `maximum | 1` in interlace broke the last two whenever R9 was even, which
// §19.4.1's own recipe makes the normal case: with R9 = N-2 = 6 for an 8-line
// character, the ASIC's C9 (stepping by 2) reached 6 and was told 6 >= 7 was false,
// so every character on the even frame ran a fifth line -- 389 lines instead of 313.
int CRTC6845::maximumRaster() const {
    int maximum = registers[9] & 0x1f;
    if (behaviour->interlaceSyncAndVideo(*this)) return maximum >> 1;
    return maximum;
}
int CRTC6845::videoRaster(int verticalScroll) {
    bool asicInterlace = behaviour->interlaceSyncAndVideo(*this);
    // ACCC §19.8.1/§19.8.3: CRTC 0 and 2 drive the video pointer from a doubled C9
    // (CRTC 2 via a separate C9.IVM counter) while C9 itself counts by 1 against R9.
    int ivm = behaviour->videoRasterIvm(*this);
    int rasterVal = asicInterlace ? (raster << 1) | interlaceField
                  : ivm >= 0    ? ivm
                                : raster;
    int scrolled = rasterVal + verticalScroll;
    // The fold keeps a SCROLLED raster inside its character. It must not touch a raster
    // the chip already handed over as a finished address field: ACCC §20.2 builds the
    // pointer's bits 11-13 straight out of "bits 0 to 2 of C9", with no comparison
    // against R9. In IVM C9.VMA legitimately runs past R9 -- §19.8.1's chronograms
    // (p.222) give C9-VMA 7 on an odd frame with R9=6 -- and folding it by R9+1 turned
    // every one of those into 0.
    if (!asicInterlace && ivm < 0) {
        int wrap = oldInterlaceVideo() ? ((maximumRaster() | 1) + 1) : maximumRaster() + 1;
        if (scrolled > maximumRaster()) scrolled -= wrap;
    }
    return scrolled & 7;
}
int CRTC6845::videoRaster() { return videoRaster(getHorizontalScroll() & 7); }
int CRTC6845::rasterOutput() {
    if (behaviour->interlaceSyncAndVideo(*this)) return (raster << 1 | interlaceField) & 0x1f;
    const int ivm = behaviour->videoRasterIvm(*this);   // CRTC 0 and 2's own C9.IVM
    return (ivm >= 0 ? ivm : raster) & 0x1f;
}

void CRTC6845::tick(int tStates) {
    charClockRemainder += tStates;
    while (charClockRemainder >= 4) { charClockRemainder -= 4; tickCharacter(); }
}
void CRTC6845::updateHsync() {
    if (!hsync) return;
    behaviour->advanceHsync(*this);
}
void CRTC6845::updateHsyncStandard() {
    if (hsyncOverflow) {
        // §15.3.2: "C3 will increment up to 15, return to 0 and then back to 1."
        // The HSYNC cannot end until the counter has come back round.
        hsyncCounter = (hsyncCounter + 1) & 0x0f;
        if (hsyncCounter == 0) hsyncOverflow = false;
        return;
    }
    int width = effectiveHsyncWidth();
    // ACCC §14.5.2 (p.137): "During an R3 update during HSYNC, CRTC 1 behaves like
    // CRTC's 0 and 2, EXCEPT when R3 is updated to 0. Indeed, the processing of R3=0
    // (no HSYNC) continues to be managed during [the HSYNC]. On CRTC's 0 and 2 in this
    // condition, HSYNC continues and 0 is treated as a value to reach."
    if (width == 0 && behaviour->cancelsHsyncOnZeroWidth()) {
        bool signal = hsyncCounter != 0;
        hsyncEndedThisCharacter = true;
        hsyncEndedJit = r3JitPending; r3JitPending = false;
        hsync = false;
        if (signal) onHsync();
        return;
    }
    // ACCC §14.5.4 (p.139) R3.JIT: "then the HSYNC STOPS on CRTC's 0, 1 and 2." The
    // start-of-character comparison for this character has already run against the old,
    // larger R3l, so without this the write would look like §14.5's "R3l changed with a
    // value less than C3l" and send C3l all the way round instead of ending the sync --
    // a 20-character HSYNC where the chapter has 5. The 0.25 usec by which this end
    // falls after a start-of-character one is exactly the JIT black zone §9.3.4.2
    // describes, which hsyncEndedJit already carries to the renderer.
    if (r3JitPending) {
        hsyncCounter = (hsyncCounter + 1) & 0x0f;
        hsync = false; hsyncEndedThisCharacter = true;
        hsyncEndedJit = true; r3JitPending = false;
        onHsync();
        return;
    }
    // ACCC §14.1: "The HSYNC ends as soon as the C3L counter reaches the value of R3L,
    // at the start of the character" — an equality, so §14.5's "if R3l is changed with
    // a value less than C3l, then C3l is overflowing" falls out, as does R3l=0 meaning
    // all 16 values on the chips that treat 0 as a value to reach.
    hsyncCounter = (hsyncCounter + 1) & 0x0f;
    if (hsyncCounter == (width & 0x0f)) {
        // ACCC §15.3.1: "On the CRTC's 1, 2, 3 and 4, there is a bug if C0=R2 on
        // C0=R2+R3." §15.3.2: "the HSYNC does not end and C3 will overflow. C3 will
        // increment up to 15, return to 0 and then back to 1. At the end of the overflow
        // of C3, if C0 is still equal to R2, then the HSYNC will again continue its
        // route." This is the character the pulse would end on; the pin never drops.
        int nextC0 = horizontalTotalMatch ? 0 : (horizontal + 1) & 0xff;
        if (!behaviour->suppressesContiguousHsync() && nextC0 == (registers[2] & 0xff)) {
            hsyncOverflow = true;
            traceHsync("overflow: C0=R2 on C0=R2+R3");
            // §15.3.4 (p.151): "The CRTC 1, however, has time to generate an 'invisible'
            // end of HSYNC, then immediately reactivate the signal for the GATE ARRAY. The
            // latter then reset to 0 its internal character counter and sends a second
            // HSYNC monitor from the 2nd position." -- here, on the character §15.3.1 names
            // (C0=R2 ON C0=R2+R3l). "The CRTC 2 does not have time to generate an end of
            // HSYNC." This used to be tested one character EARLY (C3l+1 == R3l), a C0=R2
            // meeting that §15.3.1 says is not considered at all, so it never fired where
            // the overflow actually is.
            if (behaviour->restartsCHsyncOnOverflow()) {
                hsyncRestartFromOverflow = true;
                onHsyncStart();
                hsyncRestartFromOverflow = false;
                traceHsync("overflow: C-HSYNC restarted");
            }
            return;
        }
        hsync = false; hsyncEndedThisCharacter = true;
        hsyncEndedJit = r3JitPending; r3JitPending = false;   // ACCC §14.5.4 R3.JIT
        onHsync();
    }
}
// CPCSE_TRACE_HSYNC=1 logs every HSYNC the chip starts, and every position where C0
// reached R2 and one did NOT come out, with the reason. "The picture has no black bar
// where the reference has one" can be either, and the register trace cannot tell them
// apart: it shows the R2 write landing and says nothing about what the sync pin did.
// Budget with CPCSE_TRACE_HSYNC_MAX (default 400).
void CRTC6845::traceHsync(const char* what) {
    if (traceHsyncBudget == -2) {
        const char* want = std::getenv("CPCSE_TRACE_HSYNC");
        traceHsyncBudget = -1;
        if (want) {
            const char* cap = std::getenv("CPCSE_TRACE_HSYNC_MAX");
            traceHsyncBudget = cap ? std::atol(cap) : 400;
        }
    }
    if (traceHsyncBudget <= 0) return;
    // CPCSE_TRACE_HSYNC_R2=<n>: only the positions programmed with this R2. A normal
    // screen raises one HSYNC per line, so an unfiltered trace spends its whole budget
    // on the 312 ordinary ones before reaching the test that moved R2.
    static int wantR2 = -2;
    if (wantR2 == -2) { const char* w = std::getenv("CPCSE_TRACE_HSYNC_R2"); wantR2 = w ? std::atoi(w) : -1; }
    if (wantR2 >= 0 && (registers[2] & 0xff) != wantR2) return;
    std::fprintf(stderr, "HSYNC %-22s C0=%3d C4=%3d C9=%d  R0=%3d R2=%3d R3=%02X C3=%2d\n",
                 what, horizontal, vertical, raster, registers[0] & 0xff,
                 registers[2] & 0xff, registers[3] & 0xff, hsyncCounter);
    traceHsyncBudget -= 1;
}
void CRTC6845::checkHsync() {
    if (horizontal != registers[2]) return;
    if (hsync) {
        traceHsync("refused: already in one");
        // ACCC §15.3.1: "During the processing of HSYNC CRTC, an update to R2 is no
        // longer considered if the purpose of this change is to start a new HSYNC during
        // HSYNC... particularly the case when R0 is less than R3, which implies that C0
        // can pass several times on the same value (equal to R2)." Meeting R2 mid-pulse
        // does nothing to C3; only meeting it on C0=R2+R3 is the bug, and that is decided
        // in updateHsyncStandard, on the character the pulse would have ended on.
        // (SHAKER A3/S1 on CRTC 2: R0=1, R2=1, R3=13 -- an odd R3 never puts C0 on R2 at
        // the end, so the HSYNC ends after 13 characters, "C9=R9 NOT IN HSYNC".)
        return;
    }
    if (behaviour->suppressesHsyncStartOnZeroWidth() && effectiveHsyncWidth() == 0) {
        traceHsync("refused: R3l=0");
        return;
    }
    // ACCC §15.3.1/§15.3.3 (CRTC 0): "two HSYNC's cannot be contiguous if position
    // C0=R2 is encountered when C3l reaches R3l, AND R3l HAS NOT BEEN MODIFIED ON
    // THIS POSITION." The HSYNC that just ended did so on this very character, so
    // this restart is the contiguous one the chip refuses.
    if (hsyncEndedThisCharacter && behaviour->suppressesContiguousHsync()) {
        if (!r3WrittenThisCharacter) { traceHsync("refused: contiguous"); return; }
        // "if C0 is again equal to R2 but R3l is modified, then a new HSYNC-CRTC
        // begins WITHOUT C3l BEING ZEROED" — it resumes from where it left off.
        hsync = true; hsyncOverflow = false; onHsyncStart();
        traceHsync("start (R3 rewritten)");
        return;
    }
    hsync = true; hsyncCounter = 0; hsyncOverflow = false; r3JitPending = false; onHsyncStart();
    traceHsync(hsyncArmedBetweenCharacters ? "start (R2.JIT)" : "start");
}
// CPCSE_TRACE_VSYNC=1 logs every time C4 reaches R7 and a VSYNC does NOT come out, with
// the reason. A missing VSYNC is a rolling picture, and "vsync/frame = 0.815" says only
// that it happened, never which of the four gates in here turned it away.
void CRTC6845::traceVsyncRefusal(const char* why) {
    if (traceVsyncBudget == -2) {
        const char* want = std::getenv("CPCSE_TRACE_VSYNC");
        traceVsyncBudget = -1;
        if (want) {
            const char* cap = std::getenv("CPCSE_TRACE_VSYNC_MAX");
            traceVsyncBudget = cap ? std::atol(cap) : 400;
            const char* from = std::getenv("CPCSE_TRACE_VSYNC_FROM");
            traceVsyncFrom = from ? std::atol(from) : 0;
        }
    }
    if (traceVsyncBudget > 0 && frame >= traceVsyncFrom) {
        std::fprintf(stderr, "f%-6d VSYNC REFUSED (%s)  C0=%2d C4=%3d C9=%d R4=%3d R7=%3d R9=%2d\n",
                     frame, why, horizontal, vertical, raster,
                     registers[4] & 0x7f, registers[7] & 0x7f, registers[9] & 0x1f);
        traceVsyncBudget -= 1;
    }
}
// ACCC §16.5.1 (p.172) / §19.5.2 (p.207): the IVM odd-frame odd-C4 VSYNC is "delayed by
// 1 line. It occurs when C4=R7 and C9.VMA=2 on the odd C4s" -- the character's SECOND
// line, from its start. So the start armed at one line start is taken at the next one,
// before that line's own comparison. It used to be taken at the top of the very next
// character, still C0=0 of the SAME line: the pin rose one character late, not one line
// (found by cpcse-logic-check's HD6845S reference on R9=7 and R9=5 IVM frames).
// CPCSE_TRACE_GHOST=1 logs every GHOST VSYNC start (ACCC 16.4.3): the counters it began on
// and the HSYNC state that made it one. Unbudgeted -- ghosts are rare, and the VSYNC refusal
// trace's budget is spent by ordinary r7Match refusals long before a SHAKER test reaches one.
void CRTC6845::traceGhostStart() {
    static const bool on = std::getenv("CPCSE_TRACE_GHOST") != nullptr;
    if (!on) return;
    std::fprintf(stderr, "f%-6d GHOST VSYNC  C0=%3d C4=%3d C9=%2d  R2=%3d R3=%02X hsync=%d dropped=%d C3=%d\n",
                 frame, horizontal, vertical, raster, registers[2] & 0xff, registers[3] & 0xff,
                 hsync ? 1 : 0, hsyncDroppedThisCharacter ? 1 : 0, hsyncCounter);
}
void CRTC6845::startDelayedVsyncLine() {
    if (!vsyncFullLinePending) return;
    vsyncFullLinePending = false;
    if (vsync) return;
    vsyncCounter = 0; vsync = true; traceFrameVsyncs += 1; dbgCrtcVsyncStarts += 1;
    vsyncGhost = behaviour->vsyncStartIsGhost(*this);
    if (vsyncGhost) traceGhostStart();
    if (!vsyncGhost) onVsyncStart();
}
void CRTC6845::startVsyncIfNeeded(bool liveWrite) {
    if (!liveWrite && behaviour->blocksAutoVsync(*this)) {
        if (vertical == registers[7]) traceVsyncRefusal("blocksAutoVsync");
        return;
    }
    bool rasterAllows = liveWrite || raster == baseRaster()
        || behaviour->considersVsyncAtAnyRaster();      // ACCC §16.4.3 (MC6845)
    if (vertical == registers[7] && !rasterAllows) traceVsyncRefusal("raster != baseRaster");
    if (vertical == registers[7] && rasterAllows) {
        // ACCC §16.3: on CRTC 3/4 the r7Match latch (the "equality has changed"
        // protection) does not exist, so an unchanged C4=R7 re-arms every time.
        bool reentryBlocked = behaviour->hasVsyncReentrancyProtection() && r7Match;
        if (reentryBlocked) traceVsyncRefusal("r7Match latch");
        else if (vsync) traceVsyncRefusal("already in VSYNC");
        if (!reentryBlocked && !vsync) {
            if (!behaviour->allowsVsyncStart(*this)) {
                traceVsyncRefusal("allowsVsyncStart");
                // ACCC §13.2.2: this C4=R7 occurrence was never authorised (C0 did
                // not reach 2). Consume it so it does not retry; §16.3 mechanism 2
                // lifts the block as soon as the C4/R7 equality changes below.
                r7Match = true;
                return;
            }
            r7Match = true;
            // ACCC §19.3.1: "In order to increase the vertical resolution, the CRTC,
            // during VSYNC, delays the signal by half a line when C4 changes to R7
            // for the frame displayed with the even lines. When C4 reaches R7, the
            // VSYNC signal from the CRTC is generated taking C0=R0/2 as the new
            // reference." Applies on all CRTC's, on the interlaced field only.
            // ACCC §16.5: one COMPLETE line of delay in the IVM special case, which
            // takes precedence over the half-line one (CRTC 0 and 3/4 only).
            if (!liveWrite && behaviour->delaysVsyncOneLineInIvm(*this)) {
                vsyncFullLinePending = true;
                return;
            }
            // ACCC §16.5/§19.3.1: half a line on the EVEN frame of an interlace mode.
            // ACCC §19.7.2/§19.7.3: "The MID-VSYNC is generated when C4=R7 if
            // ParityFrame is even (and R8 is 3 or 1)."
            // §19.7.3's R7=0 rule -- the VSYNC decided before ParityFrame switches -- is the
            // frame-start VSYNC's alone (midVsyncParity, set in newFrame). An R7 moved later
            // in that frame to a C4 still to come uses the frame's own parity.
            const int midParity = (registers[7] & 0x7f) == 0 ? midVsyncParity : (interlaceField & 1);
            if (!liveWrite && (displayMode() & 1) != 0 && midParity == 0
                && (registers[0] >> 1) != 0) {
                vsyncHalfLinePending = true;
                return;
            }
            vsyncCounter = 0;
            // ACCC §16.4.1 (p.169, CRTC 0): "When R7=C4 with C0vs>1, the VSYNC is
            // triggered during the line. In this case, the row counter starts with 0.
            // This VSYNC line counter is initialized at the start of the next line
            // when C0=0... The total duration of the VSYNC is INCREASED... So if a
            // VSYNC is triggered during line number 1, then the VSYNC ends at the end
            // of line 17." CRTC 1 and 2 instead "count the line as if the VSYNC had
            // started when C0=0", ending at line 16 (§16.4.2/§16.4.3) — which is what
            // a plain 0 gives. Skipping the partial line's end buys CRTC 0 the extra
            // line for every R3h, including R3h=0 (16), where pre-loading the counter
            // with 0x0f instead wrapped it to 0 == (16 & 0x0f) and ended it at once.
            vsyncSkipsFirstLineEnd =
                liveWrite && horizontal > 0 && behaviour->midLineVsyncAddsALine();
            vsync = true; traceFrameVsyncs += 1; dbgCrtcVsyncStarts += 1;
            // ACCC §16.4.3: a GHOST VSYNC counts and blocks, but the pin stays low,
            // so nothing downstream (PPI status, Gate Array, renderer) sees it.
            vsyncGhost = behaviour->vsyncStartIsGhost(*this);
            if (vsyncGhost) traceGhostStart();
            if (!vsyncGhost) onVsyncStart();
        }
    } else if (vertical != registers[7] && !vsync) r7Match = false;
    // ACCC §16.3's first mechanism again, from the per-line side: "it is not possible to
    // trigger or inhibit a VSYNC during a VSYNC". C4 moving away from R7 while the pin is
    // up must not re-arm the comparison either, or a mid-VSYNC R7 write is re-armed by the
    // very next line and the protection above buys nothing.
}
// ACCC §11.9 INTERLACE ADJUSTMENT LINE (p.92): "In INTERLACE mode, a specific vertical
// adjustment management is carried out which results in an additional line on each even
// frame... This adjustment is independent of that made via R5. When the adjustment
// condition is filled, the 'Interlace' line is added after the lines possibly scheduled
// in R5." Returns true (once) when that line is due, having claimed it.
bool CRTC6845::claimInterlaceAdjustLine() {
    if (adjustmentInterlaceExtra != 0) return false;          // already spent this frame
    if ((displayMode() & 1) == 0) return false;               // R8 read on THIS line (§19.2: not under BORDER ON)
    if (!behaviour->addsInterlaceLine(*this)) return false;
    adjustmentInterlaceExtra = 1;
    return true;
}
bool CRTC6845::rasterMatchesMaximum() {
    return behaviour->rasterMatchesMaximum(*this);
}
bool CRTC6845::verticalMatchesTotal() {
    return behaviour->verticalTotalMatches(*this);
}
bool CRTC6845::reloadsStartAddressOnThisScanline() {
    // ACCC §11.6: under an RFD the reload no longer depends on C4 at all.
    if (behaviour->reloadsStartAddressAnyRow(*this)) return true;
    if (vertical != 0) return false;
    if (raster == baseRaster()) return true;
    return behaviour->reloadsStartAddressExtra(*this);
}
void CRTC6845::latchRequestedStartAddress() {
    // Per-chip: the UM6845R freezes the reload during an R1>R0 rupture; other
    // CRTCs always reload (see CrtcType1::freezesStartAddressReload).
    if (behaviour->freezesStartAddressReload(*this)) {
        // ACCC §17.4.2 (p.183, CRTC 1): the freeze is of VMA' alone. "The first line
        // character begins with the address defined by R12/R13, whatever the value of
        // R1... Indeed, the condition C0=R1 no longer occurs and the VMA' pointer is no
        // longer updated. VMA' is 'frozen' on the last known pointer when C0 reached R1
        // when C9=R9... We have therefore, when R1>R0, A FIRST LINE CHARACTER WHICH
        // CONTAINS THE POINTER DEFINED IN R12/R13 and on the following, the last
        // pointer updated in VMA'." So VMA still takes R12/R13 here; only VMA' is left
        // alone, and every line after the first reloads from it.
        frameAddress = requestedAddress & 0x3fff;
        rowAddress = frameAddress;
        maRow = rowAddress;
        return;
    }
    // ACCC §13.4: on CRTC 2 the offset only reaches VMA through VMA', which took it
    // at C0=R1 — "it is impossible to change offset if C0 does not reach R1".
    frameAddress = (behaviour->frameAddressComesFromVmaPrime() ? nextRowAddress
                                                               : requestedAddress) & 0x3fff;
    rowAddress = frameAddress;
    nextRowAddress = rowAddress;
    maRow = rowAddress;
}
void CRTC6845::newFrame() {
    // ACCC §11.6.2 (p.90): the parity status an RFD turns on is locked "until the frame
    // is finished" -- so the lock, and nothing else about the RFD, is released here.
    rfdParityLocked = false;
    adjustFromFrameStart = false; adjustBlocksRegisterReload = false;
    vertical = 0; raster = 0; verticalAdjust = 0; verticalAdjustActive = false;
    adjustStateEngaged = false;
    // r7Match is deliberately NOT cleared here. ACCC §16.3: the second VSYNC protection
    // "is to check if the equality between C4 and R7 has changed. This condition changes
    // if C4 increments or if R7 is changed" — a new frame is neither. With R7=0 and R4=0
    // "C4 is therefore 0 during the VSYNC but also after the end of the VSYNC. In this
    // context, there is no more VSYNC", and clearing the latch on every C4 wrap turned
    // that into an infinite VSYNC on CRTC 0 and 2. The ordinary case is already covered:
    // startVsyncIfNeeded clears the latch itself the moment C4 differs from R7.
    rasterMatchForced = false; additionalLinePending = false;
    fieldCounter += 1;                              // the cursor's blink divider
    // (vsyncGhost is NOT cleared here: ACCC §16.4.3's GHOST VSYNC "counts the lines as if a
    // VSYNC were taking place", and a short frame -- R9=0, 39 lines -- ends while its 16 are
    // still counting. Clearing it raised the pin half way through. It ends with the VSYNC.)
    verticalMatchForced = false;
    vsyncAuthorized = true; r0FreezeArmed = false; r0FreezeHiccup = false;
    // ACCC §15.5.2 (CRTC 2): a HSYNC on this C0=0 skips the C4=C9=C0=0 restore.
    if (!(behaviour->skipsFrameDisplayRestoreDuringHsync() && hsyncReachesLineStart()))
        vDisplay = behaviour->initialVerticalDisplay(*this);
    // ParityFrame, and there is exactly one of it. ACCC §19.5.2/§19.5.4 (CRTC 0 and 2):
    // "ParityFrame = ParityR6 when C4=C9=C0=0", and "the management of ParityR6 is
    // INDEPENDENT OF THE VALUE OF R8". §19.5.3/§19.5.5 (CRTC 1, 3 and 4): "ParityFrame
    // switch between each frame when C4=C9=C0=0... WHATEVER THE VALUE OF R8." So the
    // state runs on every frame, interlaced or not — which is what §11.6.1's RFD needs
    // it for. Everything that consumes the interlace FIELD (baseRaster, videoRasterIvm,
    // addsInterlaceLine, the MID-VSYNC) gates itself on R8 at its own call site.
    int previousField = interlaceField;
    if (behaviour->frameParityFromParityR6()) interlaceField = parityR6 & 1;
    else interlaceField ^= 1;                       // ACCC §19.5.3 / §19.8.4
    // ACCC §19.7.3: with R7=0 the ASIC settles the VSYNC before swapping ParityFrame,
    // so its MID-VSYNC decision is made on the OUTGOING frame's parity.
    midVsyncParity = (behaviour->vsyncPrecedesParitySwap() && registers[7] == 0)
        ? (previousField & 1) : (interlaceField & 1);
    // ACCC §19.5: "At the beginning of the frame, Parityc9=ParityFrame."
    rasterParity = interlaceField & 1;
    raster = baseRaster();
    r4Match = registers[4] == 0;
    r9Match = registers[9] == 0;
    latchRequestedStartAddress();
    // The ASIC CRTCs keep their own scanline index for a host that does not own it (a
    // harness driving the chip alone). In a machine -- CPC or Plus -- the monitor owns
    // that index (hostOwnsScanlineIndex) and a CRTC frame is not a monitor frame at all: a
    // rupture restarts the CRTC many times inside one displayed frame, and zeroing the
    // index here sent each of those restarts' characters to capture line 0 while the
    // monitor kept assigning the real line -- ~8 captured characters per 64-character
    // line, so the legacy renderer drew almost nothing. The per-scanline increment in
    // onScanline is already gated the same way.
    if (behaviour->tracksScanlineInFrame() && !hostOwnsScanlineIndex) scanlineInFrame = 0;
    traceFrame();
    startDelayedVsyncLine();
    startVsyncIfNeeded(); frame += 1; onFrame(frame);
}
void CRTC6845::traceLine() {
    if (traceLineBudget == -2) {
        const char* want = std::getenv("CPCSE_TRACE_LINE");
        traceLineBudget = -1;
        if (want) {
            const char* cap = std::getenv("CPCSE_TRACE_LINE_MAX");
            traceLineBudget = cap ? std::atol(cap) : 400;
            const char* from = std::getenv("CPCSE_TRACE_LINE_FROM");
            traceLineFrom = from ? std::atol(from) : 0;
        }
    }
    // CPCSE_TRACE_LINE_R4=<n>: only lines running with this R4. A rupture screen makes
    // tens of thousands of CRTC lines a frame, so an unfiltered budget is spent long
    // before the test under examination is on screen; its register signature is what
    // picks it out (SHAKER names the registers in its own caption).
    static int wantR4 = -2;
    if (wantR4 == -2) { const char* w = std::getenv("CPCSE_TRACE_LINE_R4"); wantR4 = w ? std::atoi(w) : -1; }
    if (wantR4 >= 0 && (registers[4] & 0x7f) != wantR4) return;
    if (traceLineBudget > 0 && frame >= traceLineFrom) {
        std::fprintf(stderr, "  line f%-6d n=%3d C4=%3d C9=%2d R4=%3d R5=%2d R9=%2d R8=%d r9M=%d par=%d fld=%d%s%s  h%d C3=%d%s R2=%d R3=%d R0=%d us=%ld\n",
                     frame, traceFrameLines, vertical, raster,
                     registers[4] & 0x7f, registers[5] & 0x1f, registers[9] & 0x1f,
                     registers[8] & 3, r9Match ? 1 : 0, rasterParity & 1, interlaceField & 1,
                     verticalAdjustActive ? "  ADJUST" : "",
                     vsync ? (vsyncGhost ? "  VSYNC(ghost)" : "  VSYNC") : "", hsync ? 1 : 0, hsyncCounter,
                     hsyncOverflow ? "o" : "", registers[2] & 0xff, registers[3] & 0xff,
                     registers[0] & 0xff, traceCharacters);
        traceLineBudget -= 1;
    }
}
// CPCSE_TRACE_FRAME=1 logs one line per CRTC frame -- how many lines it ran for, the
// geometry it ran with, and whether it produced a VSYNC at all. Capped by
// CPCSE_TRACE_FRAME_MAX (default 400).
//
// This exists because a register trace cannot answer the question a ruptured screen
// raises. Pinball Dreams writes R4 twice per displayed frame and the two writes only
// mean what they are supposed to mean if each lands in the right CRTC frame; reading
// "R4<-13 at C4=0" and "R4<-24 at C4=6" tells you nothing about whether that made two
// frames of 14 and 25 rows or one frame of 25. This does.
void CRTC6845::traceFrame() {
    if (traceFrameBudget == -2) {
        const char* want = std::getenv("CPCSE_TRACE_FRAME");
        traceFrameBudget = want ? 0 : -1;
        if (want) {
            const char* cap = std::getenv("CPCSE_TRACE_FRAME_MAX");
            traceFrameBudget = cap ? std::atol(cap) : 400;
            // CPCSE_TRACE_FRAME_FROM: skip to the frame that matters. A game reaches the
            // screen under test thousands of frames in, and a budget spent on the loader
            // reports a picture that is working.
            const char* from = std::getenv("CPCSE_TRACE_FRAME_FROM");
            traceFrameFrom = from ? std::atol(from) : 0;
        }
    }
    if (traceFrameBudget > 0 && frame >= traceFrameFrom) {
        std::fprintf(stderr, "FRAME lines=%3d rows=%3d R4=%3d R5=%2d R7=%3d R9=%2d vsyncs=%d\n",
                     traceFrameLines, traceFrameRows, registers[4] & 0x7f, registers[5] & 0x1f,
                     registers[7] & 0x7f, registers[9] & 0x1f, traceFrameVsyncs);
        traceFrameBudget -= 1;
    }
    traceFrameLines = 0; traceFrameRows = 0; traceFrameVsyncs = 0;
}
bool CRTC6845::updateVerticalType1() {
    if (r9Match) {
        // ACCC §19.8.2: on the parity-excluded C9/R9 match the chip does
        // "ParityC9 = ParityC9 xor (not r9.0)" and only THEN "C9 = ParityC9", so the
        // flip has to land before baseRaster() is read.
        behaviour->updateRasterParity(*this);
        raster = baseRaster();
        if (registers[9] != 0) r9Match = false;
        if (r4Match) {
            if (registers[4] != 0) r4Match = false;
            verticalAdjustActive = true;
            // ACCC §11.2.4 (p.85): "If C4=0 BEFORE the additional management, then VMA
            // is updated with R12/R13 and not VMA', and this as long as C4=1." Recorded
            // here, before C4 takes its step, exactly as the general path records it.
            adjustFromFrameStart = vertical == 0 && !adjustBlocksRegisterReload;
            if (registers[5] != 0) vertical = (vertical + 1) & 0x7f;
            // ACCC §11.3.2: the internal additional-management state is armed only when
            // R5>0 HERE. With R5 already 0 there is no state and no adjustment, which is
            // the ordinary "R5=0 adds no lines" case.
            adjustStateEngaged = (registers[5] & 0x1f) != 0;
            verticalAdjust = 0; adjustmentInterlaceExtra = 0;
        } else vertical = (vertical + 1) & 0x7f;
    } else {
        raster = (raster + rasterStep()) & 0x1f;
        // ACCC §19.8.2: the latched C9/R9 test is the per-chip one — in IVM it masks
        // parity off both sides rather than comparing against R9 + the interlace bit.
        if (behaviour->rasterMatches(*this)) r9Match = true;
    }

    if (vertical == registers[4]) r4Match = true;

    if (verticalAdjustActive) {
        // The limit is re-read from R5 on every line, which is §11.3's live update on
        // this chip: "if R5 is modified with C5+1 on line C5, then the vertical
        // adjustment is stopped" falls straight out of the comparison.
        int limit = ((registers[5] & 0x1f) + adjustmentInterlaceExtra) & 0x1f;
        // ACCC §11.3.2 (p.86): "However, if the new value of R5 is set to 0, this causes
        // a bug that deactivates (slyly) the reset of C4 for the new frame... if R5
        // becomes zero during additional management, THE STATE IS NOT DEACTIVATED, C4
        // does not return to 0 and C5 loops... Thus, if C5+1 reaches an R5>0, then the
        // additional management changes C4 to 0 before deactivating its state."
        //
        // So R5 zeroed while the state is armed holds the frame open instead of ending
        // it, and C5 goes on round. The chapter states the use outright -- "it is
        // possible to change C4 and C9 to 0 on any line with this method by modifying R5
        // with a value greater than 0 according to the value reached by C5+1" -- and it
        // is how Pinball Dreams steers its CRTC-1 rupture. Ending the frame here instead
        // made every one of its frames a line long, which walked its R4:=C4 write off the
        // row it targets until §12.3's runaway fired and the picture rolled.
        //
        // R5 that was ALREADY 0 when the adjustment opened never armed the state
        // (adjustStateEngaged), so the ordinary "R5=0 adds no lines" case is untouched.
        bool heldOpenByZeroR5 = adjustStateEngaged && (registers[5] & 0x1f) == 0;
        if (!heldOpenByZeroR5 && verticalAdjust == limit) {
            // ACCC §19.6.2 (p.217): "The additional line is added at the end of the
            // frame (AFTER THE R5 LINES if necessary) if one of the two 'Interlace'
            // modes is activated (R8=3 or 1) and if ParityFrame is even." Claimed here,
            // at the end of the R5 count, so §11.9's "it is possible to update R8 on one
            // of the lines displayed via R5" holds on this chip too.
            if (!claimInterlaceAdjustLine()) { adjustStateEngaged = false; newFrame(); return true; }
        }
        verticalAdjust = (verticalAdjust + 1) & 0x1f;
    }

    if (vertical == registers[6]) vDisplay = false;
    return false;
}
// The standard 6845 vertical counter chain (HD6845S / MC6845 / Plus ASIC). The
// UM6845R uses updateVerticalType1() instead. Returns true if a new frame began.
bool CRTC6845::updateVerticalGeneral() {
    int verticalAtLineStart = vertical;      // ACCC §19.5: did C4 move this line?
    bool c9Frozen = behaviour->blocksRasterAdvance(*this);   // ACCC §13.2.1 (R0=0)
    if (additionalLinePending) {          // the extra C4=1 line just finished (ACCC §13.2.7)
        additionalLinePending = false;    // now C4 and C9 return to 0 — and only now does
        newFrame(); return true;          // the R12/R13 address reload happen
    }
    if (c9Frozen) {
        // ACCC §13.2.6 "C4's last hiccup": the C4 increment and the C9 reset are
        // both armed on the C0=0 at which R0 went to 0. C9 is frozen, so on the
        // NEXT C0=0 only C4 moves — once — and it moves whatever R4 holds, with
        // C9 NOT returning to 0. After that nothing but C0 is managed at all.
        if (!r0FreezeArmed) {
            r0FreezeArmed = true;
            r0FreezeHiccup = rasterMatchesMaximum();
            // ACCC §13.2.4 (p.106): "On the first step of C0=0, when R0 becomes equal
            // to 0, THE VALUE OF C9 IS CALCULATED FOR THE FIRST TIME with respect to R9
            // (for example if C9 was worth 4 and R9=7, C9 goes to 5)." Only the reset
            // arm survives into the freeze: if C9 had already reached R9 the chapter
            // has C9 stay put and the C4 increment fall to the second C0=0, which is
            // the hiccup below. §13.2.1's shorter "all of the CRTC counters are frozen"
            // is the summary of the same thing one line later.
            // §13.2.3 FREEZE OF ADDITIONAL ADJUSTMENT LINE carves out the one case
            // where even that first step does not happen: "If R0 goes to 0 on C0=0 of
            // this line, then C9 REMAINS FIXED AT 0 and C4 can only go to 0 when C9 is
            // managed again (as soon as C0=1)."
            if (!r0FreezeHiccup && verticalAdjust == 0) raster = (raster + rasterStep()) & 0x1f;
            // ACCC §13.2.6 (p.109): "If C9=R9 and C4=R4 then C4=R4+1. When R0>0, C4 is
            // managed by C9/R5." — the freeze arms the additional management too, "and
            // which will remain so when C0 can once again exceed 1. It is then R5 which
            // controls the end of the additional management."
            if (r0FreezeHiccup && verticalMatchesTotal() && behaviour->adjustmentCountsRasterToR5()) {
                verticalAdjust = 1; adjustmentInterlaceExtra = 0; adjustLine = 0;
                verticalAdjustState = true;
            }
        } else if (r0FreezeHiccup) {
            r0FreezeHiccup = false;
            vertical = (vertical + 1) & 0x7f;
        }
        return false;
    }
    // ACCC §19.6.3 (p.218, CRTC 2): "If the IVM mode is activated on the first line of
    // an odd frame, then THIS LINE WILL BECOME AN ADDITIONAL LINE, and a new line 0
    // will follow the old line 0, which will extend the size of the frame by R0 usec."
    if (interlaceLineNow) { interlaceLineNow = false; newFrame(); return true; }
    r0FreezeArmed = false;   // R0 > 0 again: counters resume, re-arm for next time
    if (verticalAdjust > 0) {
        if (behaviour->adjustmentCountsRasterToR5()) {
            // ACCC §11.2.2 (CRTC 0): there is no C5. C9 keeps counting through the
            // adjustment and its limit is R5, tested at the start of the line. "In
            // order to prevent resetting C9 to 0 from leading to a loop if R5>R9+1 ...
            // As long as C4<>R4 in vertical adjustment, C9 can no longer be zeroed" —
            // hence a plain increment, with no R9 comparison in the way.
            raster = (raster + rasterStep()) & 0x1f;
            // ACCC §11.3: R5 is read live — "if R5 is modified with C9+1 on line C9,
            // then the vertical adjustment is stopped", and "if R5 is modified with a
            // value less than C9+1, then the counter overflows and continues to count
            // up to 0 to reach the new R5 value".
            int limit = (registers[5] & 0x1f) + adjustmentInterlaceExtra;
            if (raster == (limit & 0x1f)) {
                if (claimInterlaceAdjustLine()) return false;   // ACCC §11.9
                verticalAdjust = 0; newFrame(); return true;
            }
            return false;
        }
        verticalAdjust -= 1;
        adjustLine += 1;
        // ACCC §11.1 (p.81): "On CRTCs 0, 3 and 4, there is no specific C5 counter and
        // C9 is used for comparison with R5. On CRTCs 1 and 2, THERE IS A SPECIFIC
        // COUNTER C5 used in conjunction with C9 to allow management of 'characters'
        // within the adjustment lines." §11.2.5 says what that looks like on the
        // MC6845: "In additional management, C4 is incremented each time C9 reaches R9
        // (C9 then goes to 0), whatever the value of R4." So on a chip with a C5, C9
        // keeps its ordinary R9 wrap through the adjustment and C4 steps on that wrap --
        // it does not run 0..R5 with C4 stepping every line, which is what §11.2.1's
        // table (p.82) shows CRTC 1 and 2 sharing and CRTC 0 doing differently.
        // C5 itself is verticalAdjust/adjustLine here, and still counts the R5 lines.
        if (behaviour->hasSeparateAdjustCounter()) {
            // Tested on the CURRENT C9 and only then advanced, exactly as the ordinary
            // line end below does it -- C9 reaching R9 is what wraps it.
            if (rasterMatchesMaximum()) {
                rasterMatchForced = false;
                raster = baseRaster();
                vertical = (vertical + 1) & 0x7f;
                if (vertical == registers[6]) vDisplay = false;
            } else {
                raster = (raster + rasterStep()) & 0x1f;
            }
            if (verticalAdjust == 0) {
                if (claimInterlaceAdjustLine()) { verticalAdjust = 1; return false; }
                newFrame(); return true;
            }
            return false;
        }
        // ACCC §11.3.3 (CRTC 3, 4): the R5 lines end "when the number of the next additional
        // line (C9+1) reaches R5" -- counted one by one even in IVM, where §19.4.4 says R5
        // "still contains a finite number of lines without considering the Interlace mode".
        raster = (raster + (behaviour->additionalLinesCountFromZero() ? 1 : rasterStep())) & 0x1f;
        if (verticalAdjust == 0) {
            // ACCC §11.9 (p.92): the interlace adjustment line "is evaluated on the last
            // line of a frame, when C0=R0, and only if R8 contains the right value on
            // the last line. THIS LATEST LINE CAN BE ONE OF THE ADJUSTMENT LINES
            // DISPLAYED VIA R5. It is therefore possible to update R8 on one of the
            // lines displayed via R5 to activate or deactivate the treatment of the
            // interlace line." So it is decided here, at the end of the R5 count, not
            // when the adjustment opened.
            if (claimInterlaceAdjustLine()) {
                verticalAdjust = 1;
                if (behaviour->additionalLinesCountFromZero()) raster = 0;
                return false;
            }
            newFrame(); return true;
        }
        // ACCC §13.2.4 (p.103): CRTC 0 increments C4 exactly once for the whole
        // adjustment — that one step happened on entry below. Every other chip
        // keeps stepping C4 on each adjustment line, so C4 can reach R6/R7 here.
        if (behaviour->incrementsVerticalEachAdjustLine()) vertical = (vertical + 1) & 0x7f;
    // ACCC §10.3.1 (p.77): "When the last line state is true without active vertical
    // adjustment, then C4 AND C9 are reset to 0 for the next line." Both counters, and
    // on the strength of the LATCHED state -- so on CRTC 0 and 2 the row has to
    // complete here even when the live C9/R9 comparison no longer holds, which is what
    // §10.3's table (p.79) shows in its last row: with C4==R4 and C9==R9==7, writing
    // R9=0 at C0>1 still gives C9=0 and C4=0 on the next line, where CRTC 1 -- which
    // has no latch and re-derives the comparison -- overflows C9 to 8 instead.
    // Reading only rasterMatchesMaximum() here let the R9 write escape the frame end
    // that C0<2 had already decided on.
    } else if (rasterMatchesMaximum()
               || (behaviour->usesLastFrameLineLatch() && (lastFrameLine || verticalAdjustState))) {
        rasterMatchForced = false;
        // ACCC §10.3.1.2: on CRTC 0 and 2 the decision was LATCHED at C0<2 and is
        // not re-derived here — "programming R9 (or R4) when C0>1 will not prevent
        // C4 and C9 from returning to 0". CRTC 1 evaluates it as it goes (§10.3.2).
        // ACCC §10.3.1.2: "If vertical adjustment is active on the last line (R5>0 on
        // C0==2), then the vertical adjustment state becomes true AND CANCELS THE 'LAST
        // LINE' STATE." So on CRTC 0 the frame's end arrives as one of two states, not
        // one: lastFrameLine when there is no adjustment to run, verticalAdjustState
        // when there is. Reading only the first left the R5 lines unreachable — an
        // R4=36/R5=16/R9=7 frame came out 318 lines instead of 312, and lost a VSYNC
        // in four frames out of five.
        bool atVerticalTotal = behaviour->usesLastFrameLineLatch()
            ? (lastFrameLine || verticalAdjustState)
            : verticalMatchesTotal();
        verticalMatchForced = false;   // the armed C4 reset is consumed here (§13.4)
        if (atVerticalTotal) {
            // §13.2.1's deadline: a chip that settles the adjustment inside the C0<3
            // window uses R5 as it stood there, so an R5 raised later on this same line
            // adds nothing and the next line is simply a new frame.
            verticalAdjust = behaviour->settlesAdjustLengthBeforeC0Three()
                ? adjustLengthLatched : (registers[5] & 0x1f);
            // ACCC §11.1: "If one of the two interlace modes is programmed (R8=1 or
            // R8=3) then an additional adjustment line is added on the even frames,
            // AFTER any lines generated via R5." §11.9 says when that is decided, which
            // is at the end of those lines rather than here, so the R5 count opens
            // clean and claimInterlaceAdjustLine() extends it if R8 still asks.
            adjustmentInterlaceExtra = 0;
            adjustLine = 0;
            // ACCC §13.7.2.2: an additional management engaged by the R0 enlargement
            // runs even with R5=0 -- C9 counts round to R5 rather than the frame
            // ending at once. Spent here, with the state it engaged.
            const bool engagedByR0Enlarge = adjustEngagedWithoutR5;
            if (verticalAdjust == 0 && adjustEngagedWithoutR5) verticalAdjust = 1;
            adjustEngagedWithoutR5 = false;
            if (verticalAdjust == 0 && claimInterlaceAdjustLine()) verticalAdjust = 1;
            if (verticalAdjust == 0) {
                // CRTC 0: a Last Line on a >=2µsec-short line emits one extra line at
                // C4=1/C9=0 first, with NO address reload (ACCC §13.2.7).
                if (behaviour->generatesAdditionalLastLine(*this)) {
                    additionalLinePending = true;
                    vertical = 1; raster = baseRaster();
                    addressFrozenForAdditionalLine = true;
                    // The oracle scores this line as BORDER even though letting it
                    // display reproduces the reference geometry exactly — see the
                    // note in the commit; its content is not yet right.
                    vDisplay = false;
                    return false;
                }
                newFrame(); return true;
            }
            else {
                // ACCC §11.1: CRTC 3/4 hold C4 AT R4 through the adjustment; the
                // others step it once here (CRTC 0 only once in total, §11.2.2).
                // ACCC §11.2.4 (p.85, CRTC 1): "If C4=0 before the additional
                // management, then VMA is updated with R12/R13 and not VMA', and this
                // as long as C4=1 (new value of C4 in additional management). In other
                // words, the management of R1 for the update of the video pointer no
                // longer takes place. It is then possible to modify the offset on each
                // line C9 of C4=1 as one would do when C4=0." Recorded here, where the
                // entry condition is still visible; consumed by the chip profile.
                adjustFromFrameStart = vertical == 0 && !adjustBlocksRegisterReload;
                if (engagedByR0Enlarge) {
                    // ACCC §13.7.2.2: the management was engaged by the R0 enlargement,
                    // whose C4 step already landed on C0=2 -- "C4 is incremented only once,
                    // whatever the value of R5" (§13.2.4) -- and "incrementing C4 WITHOUT C9
                    // RETURNING TO 0 leaves the additional management activated... C9 will
                    // increment to display lines 8 to 31, until it reaches R5". So no second
                    // step and no reset: C9 carries on, and the count ends where it meets R5.
                    raster = (raster + rasterStep()) & 0x1f;
                    const int limit = (registers[5] & 0x1f) + adjustmentInterlaceExtra;
                    if (behaviour->adjustmentCountsRasterToR5() && raster == (limit & 0x1f)) {
                        verticalAdjust = 0; newFrame(); return true;
                    }
                } else {
                    if (behaviour->incrementsVerticalOnAdjustEntry()) vertical = (vertical + 1) & 0x7f;
                    raster = behaviour->additionalLinesCountFromZero() ? 0 : baseRaster();
                }
            }
        } else {
            // ACCC §10.3.1: an R9 written onto C9 exactly at C0=R0 resets C9 but finds the
            // C4 step already decided against -- verticalStepSuppressed (CRTC 0).
            if (!verticalStepSuppressed) {
                vertical = (vertical + 1) & 0x7f;
                if (vertical == registers[6]) vDisplay = false;
            }
            raster = baseRaster();
        }
    } else {
        raster = (raster + rasterStep()) & 0x1f;
        // ACCC §10.3.1 (CRTC 0): an R9 write landing exactly on C0=R0, on a line whose
        // C9 already matched the OLD R9, leaves the C4 increment armed even though the
        // new R9 no longer matches — so both counters step on this one line.
        if (verticalStepForced) {
            vertical = (vertical + 1) & 0x7f;
            if (vertical == registers[6]) vDisplay = false;
        }
        if (behaviour->clearsVDisplayOnRowMatch() && vertical == registers[6]) vDisplay = false;
    }
    verticalStepForced = false;
    verticalStepSuppressed = false;
    // ACCC §19.5: "ParityC9 switches to each new C4 when R9 is odd... when R9 is odd,
    // the parity of the lines depends on that of C4 and on the current parity of C9."
    // With an even R9 this never fires and ParityC9 stays the frame parity.
    if (vertical != verticalAtLineStart) behaviour->updateRasterParity(*this);
    return false;
}
// Default per-chip vertical advance = the standard chain. UM6845R overrides.
bool CrtcBehaviour::advanceVertical(CRTC6845& crtc) const { return crtc.updateVerticalGeneral(); }
// Default display-enable: gated on the R8 display-skew/blank bits. UM6845R overrides.
bool CrtcBehaviour::displayEnabled(CRTC6845& crtc) const { return (crtc.registers[8] & 0x30) != 0x30; }
// Default raster-max test: a forced match, or the per-chip rasterMatches(). UM6845R overrides.
bool CrtcBehaviour::rasterMatchesMaximum(CRTC6845& crtc) const {
    if (crtc.rasterMatchForced) return true;
    return rasterMatches(crtc);
}
// Default horizontal-total match on an R0 write: exact. ASIC catches up with >=.
bool CrtcBehaviour::horizontalCounterAtTotal(CRTC6845& crtc) const { return crtc.horizontal == crtc.registers[0]; }
void gaHsyncBlackWindow(const CrtcBehaviour* chip, bool now, bool before,
                        bool r2Jit, bool endJit, int& from, int& to) {
    from = 16; to = 16;
    if (now && before) { from = 0; }                                  // wholly inside
    else if (now && !before) from = r2Jit ? chip->hsyncBlackStartPixelJit()
                                          : chip->hsyncBlackStartPixel();
    else if (!now && before) { from = 0;
        to = endJit ? chip->hsyncBlackEndPixelJit() : chip->hsyncBlackEndPixel(); }
}
// Default classic interlace-video mode = R8&3==3. ASIC reports false (uses sync+video).
bool CrtcBehaviour::interlaceVideo(const CRTC6845& crtc) const { return crtc.displayMode() == 3; }
// ACCC §17.5.1 (p.186): on CRTC 0, 1 and 2 an R1=0 write landing on C0=0 is still
// acknowledged for this line; from C0=1 it is "not considered (too late)".
bool CrtcBehaviour::acknowledgesR1ZeroNow(CRTC6845& crtc) const { return crtc.horizontal == 0; }
// ACCC gives a ParityC9 rule for the moment of the R8 write in two sections only:
// §19.5.5 (CRTC 3 and 4) "When R8 changes to 1 or 3, Parityc9=C9.0", and §19.5.3's much
// fuller one for CRTC 1. §19.5.2 (CRTC 0) and §19.5.4 (CRTC 2) state none, and §19.8.1's
// chronograms show CRTC 0 really does not have it: on p.222's table 2 an OUT R8,3 landing
// on C9=1 of an EVEN frame is followed by C9-VMA = 4 on the next line, which is
// (C9x2)|ParityFrame with ParityC9 still 0 -- taking C9.0 there would have made it 5.
// So the default is to do nothing, and each chip that has a rule brings its own.
void CrtcBehaviour::onInterlaceWritten(CRTC6845&, int) const {}
void CrtcBehaviour::onInterlaceWrittenFourthMicrosecond(CRTC6845&, int) const {}
// ACCC §19.6.1/§19.6.3 (CRTC 0 and 2): the additional line is added when "one of the
// two Interlace modes is activated (R8=3 or 1) AND THE PARITYR6 STATE IS ODD".
bool CrtcBehaviour::addsInterlaceLine(CRTC6845& crtc) const { return (crtc.parityR6 & 1) != 0; }
// Default Hsync advance = the standard 6845 counter. ASIC overrides.
void CrtcBehaviour::advanceHsync(CRTC6845& crtc) const { crtc.updateHsyncStandard(); }
// Default next-row start-address reload at C0=R1: on a raster-max match. ASIC honours splits.
void CrtcBehaviour::reloadNextRowAddress(CRTC6845& crtc) const {
    if (crtc.rasterMatchesMaximum()) crtc.nextRowAddress = (crtc.rowAddress + crtc.horizontal) & 0x3fff;
}
void CRTC6845::updateVertical() {
    bool newFrameStarted = false;
    // ACCC §17.4.3 (CRTC 2, R1=0): the C0=R1 evaluation is processed before both the
    // last-line evaluation and the VMA=VMA' transfer. C0 is still R0 here — the wrap
    // to 0 happens in the caller straight after — so it is presented as 0 for the test.
    if (registers[1] == 0 && behaviour->assignsVmaPrimeBeforeVma()) {
        int savedHorizontal = horizontal;
        horizontal = 0;
        behaviour->reloadNextRowAddress(*this);
        horizontal = savedHorizontal;
    }
    int verticalScroll = getHorizontalScroll() & 7;
    // Keep the pre-advance row address: the R.V.L.L. additional line must NOT take
    // it (ACCC §13.2.7, "the change of address does not take place because C4=1").
    int addressBeforeAdvance = rowAddress & 0x3fff;
    rowAddress = nextRowAddress & 0x3fff;
    if (vsync && !behaviour->blocksRasterAdvance(*this)) {
        // ACCC §16.4.1: the VSYNC line counter C3h is one of the counters frozen by
        // the CRTC 0 R0=0 state — "the VSYNC is not deactivated if R3h was worth 1
        // (because C3h can no longer reach R3h)". So it must not tick while frozen.
        int width = behaviour->vsyncWidth(*this);
        // ACCC §16.4.1 (p.169): a VSYNC "triggered" mid-line has its line counter
        // "initialized at the start of the next line when C0=0", so the partial line
        // it started on is not one of the R3h lines — it is added to them.
        if (vsyncSkipsFirstLineEnd) {
            vsyncSkipsFirstLineEnd = false;
        } else {
            vsyncCounter = (vsyncCounter + 1) & 0x0f;
            if (vsyncCounter == (width & 0x0f)) { vsync = false; vsyncCounter = 0; vsyncGhost = false; }
        }
    }

    newFrameStarted = behaviour->advanceVertical(*this);
    // ACCC §19.6: ParityR6 anticipates the NEXT frame's parity and is updated
    // whenever C4 reaches R6, whatever R8 holds. When R6>R4 it is never updated, so
    // the frame parity — and with it the additional interlace line — freezes.
    if (vertical == (registers[6] & 0x7f)) parityR6 = (interlaceField & 1) ^ 1;
    if (addressFrozenForAdditionalLine) {
        // ACCC §13.2.7: "The change of address does not take place because C4=1. It
        // is on the next line frame that C4 will return to 0 (with C9)." So the
        // additional line repeats the row it followed rather than stepping on by R1,
        // and only the frame restart after it reloads R12/R13.
        addressFrozenForAdditionalLine = false;
        rowAddress = addressBeforeAdvance;
        nextRowAddress = addressBeforeAdvance;
        maRow = addressBeforeAdvance;
    }

    // ACCC §18.2.1: "The common condition for restoring the bottom display is
    // C4=C9=C0=0." The BORDER raised by the R6 condition is cleared by the COUNTERS
    // returning to zero, not by a frame *event*. That distinction matters once C4
    // has run away past R4 — §12 (p.89, and p.91 for CRTC 1) says it then counts on
    // to 127 and loops back — because the wrap through 0 produces no newFrame() to
    // restore the display. Without this, a single overshoot past R6 blanks the whole
    // following 128-step C4 cycle: Shaker A3 on CRTC 1 loses 36 of its 121 rupture
    // lines exactly that way.
    // ACCC §15.5.2: on CRTC 2 a HSYNC on that C0=0 skips the restore.
    if (!newFrameStarted && vertical == 0 && raster == baseRaster()
        && !(behaviour->skipsFrameDisplayRestoreDuringHsync() && hsyncReachesLineStart())) {
        vDisplay = behaviour->initialVerticalDisplay(*this);
    }

    if (!newFrameStarted) {
        if (reloadsStartAddressOnThisScanline()) {
            latchRequestedStartAddress();
        }
        // ACCC §17.4.2 (p.183): when R1>R0 the pointer is no longer reloaded — "we have
        // therefore, when R1>R0, A FIRST LINE CHARACTER WHICH CONTAINS THE POINTER
        // DEFINED IN R12/R13 and on the following, THE LAST POINTER UPDATED IN VMA'".
        // latchRequestedStartAddress's freeze branch carefully leaves VMA' alone for
        // exactly that reason, and this line then handed it rowAddress — which the freeze
        // branch had just set to R12/R13. So the frozen pointer survived until the first
        // frame end and was overwritten by the frame base for good: Shaker AO on CRTC 1
        // repeated the caption row on all 24 rows where the reference repeats the row at
        // VMA'+40x24. VMA' is frozen, so nothing here may assign it.
        if (!behaviour->freezesStartAddressReload(*this)) nextRowAddress = rowAddress;
        maRow = rowAddress;
        vlc = videoRaster(verticalScroll);
        if (behaviour->tracksScanlineInFrame() && !hostOwnsScanlineIndex) scanlineInFrame = (scanlineInFrame + 1) & 0x3ff;
        startDelayedVsyncLine();
        startVsyncIfNeeded();
    } else vlc = videoRaster(verticalScroll);
    updateTrojanLightgun();
    onScanline(scanlineInFrame);
}
void CRTC6845::tickCharacter() {
    traceCharacters += 1;
    // ACCC §17.4.2: the character on which C0 reached R1 has now run its course, and every
    // write made during it has landed. The reload happens only if R1 still matches -- an
    // R1.JIT on that very character took the condition away.
    if (r1ReloadPending) {
        r1ReloadPending = false;
        if (horizontal == registers[1]) behaviour->reloadNextRowAddress(*this);
    }
    onCharacterStart();
    if (horizontal == 0) { traceFrameLines += 1; traceLine(); }
    // R9 as it stood when this character opened, before any write of this microsecond
    // lands. ACCC §12.4.1 needs it for CRTC 2's C0==0 evaluation and §10.3.1 for
    // CRTC 0's "C9 was equal to R9 before C0 reached R0".
    r9Previous = registers[9];
    r5Previous = registers[5];
    r0Previous = registers[0];
    r8Previous = registers[8];
    // ACCC §19.5.3 (p.210): "these updates are performed on the 3rd and 4th µseconds of
    // the OUT(C),C instruction". The 3rd ran inside writeRegister during the previous
    // character; this is the 4th.
    if (interlaceFourthMicrosecond >= 0) {
        int previousR8 = interlaceFourthMicrosecond;
        interlaceFourthMicrosecond = -1;
        behaviour->onInterlaceWrittenFourthMicrosecond(*this, previousR8);
    }
    // ACCC §19.8.1: the deferred IVM comparison lasts until the next C0=0.
    if (horizontal == 0) ivmTestDeferred = false;
    if (horizontal == 0) ivmExitDeferred = false;
    if (horizontal == 0) lastLineEnlarged = false;   // ACCC §13.7.1.2: one line only
    // ACCC §18.3.4 (CRTC 3/4): R6=0 is tested only here, at the start of the line.
    if (horizontal == 0 && registers[6] == 0 && behaviour->testsR6ZeroAtLineStart())
        vDisplay = false;

    const bool hsyncBeforeUpdate = hsync;
    updateHsync();
    // The character now opening is the one this pulse ended on (C0=R2+R3), for the whole
    // of its microsecond -- ACCC §16.4.3's window, which a write can land in.
    hsyncDroppedThisCharacter = hsyncBeforeUpdate && !hsync;
    behaviour->onCharacterPosition(*this, horizontal);   // ACCC §13.2.1/§13.2.2 C0 staging
    // ACCC §19.3.1: the interlaced field's VSYNC is referenced to C0=R0/2, half a
    // line later than the normal C0=0, which is what orders the two fields' lines.
    // ACCC §16.5: the one-complete-line delay lands at C0=0 of the following line.
    // Both deferred VSYNC starts are taken HERE, before C0 advances, so the pin rises on
    // the character after the one the comparison names: §19.3.1 makes "C0=R0/2 the new
    // reference" and §19.7.1's period then comes out at the half line (313 half-lines).
    // Moving them down beside checkHsync, where C0 is already the character being
    // output, makes the pin rise a character earlier and DOUBLES frame-check's
    // interlace-period failures, 6 to 12 -- measured, not assumed.
    if (vsyncHalfLinePending && horizontal == (registers[0] >> 1)) {
        vsyncHalfLinePending = false;
        vsyncCounter = 0; vsync = true; traceFrameVsyncs += 1; dbgCrtcVsyncStarts += 1;
        vsyncGhost = behaviour->vsyncStartIsGhost(*this);
        if (vsyncGhost) traceGhostStart();
        if (!vsyncGhost) onVsyncStart();
    }
    // DISPTMG skew (ACCC 1.10 §19.2): on CRTC 0/3/4 R8 bits 4-5 delay the DISPEN
    // pin by 0/1/2 characters. Delaying the border->display edge makes the first
    // `skew` characters after every C0=R0 wrap show BORDER — the per-rupture-line
    // yellow comb the A2 SKEWDISP test draws (value 3 = non-output, handled in
    // displayOutputEnabled, so it must not shift the edges here).
    // The per-character DISPEN skew (border intruding once per R0-rupture line)
    // is a real-6845 behaviour (types 0 and 2, MC6845/HD6845S). The Plus ASIC
    // CRTCs (3/4) expose the R8 skew bits but do NOT produce that per-rupture
    // comb — Amspirit shows straight bars there — so skew is not delayed here.
    int dispenSkew = behaviour->dispenSkew(*this);
    if (dispenSkew == 3) dispenSkew = 0;
    // §19.2.4: on a delay-line chip the events below run on the UNSKEWED signal, and the
    // pin is that signal `skew` characters late (end of this function's display block).
    const bool skewDelayLine = behaviour->skewIsDelayLine();
    const int pinSkew = dispenSkew;
    if (skewDelayLine) { hDisplay = hDisplayInternal; dispenSkew = 0; }
    if (horizontalTotalMatch) {
        if (registers[0] != 0) horizontalTotalMatch = false;
        updateVertical();
        horizontal = 0;
        hsyncOnLineStart = hsyncReachesLineStart();
        // ACCC §17.5: a fresh line's R1=0 deadline opens HERE, as C0 wraps -- before any
        // write can land on character 0. Reset at the top of the tick that begins with
        // C0=0 instead, it wiped the "too late" of a write that had just landed ON C0=0,
        // which §17.5.2 says the ASIC does not take ("R1=0 is considered only on C0=R0"):
        // SHAKER AO/H's OUTI half lands on C0=0 and came out all BORDER on CRTC 3.
        r1ZeroLate = false;
        if (dispenSkew == 0 || behaviour->skewBordersEveryLineStart())
            hDisplay = registers[1] != 0 && dispenSkew == 0;
        else if (registers[1] == 0) hDisplay = false;   // otherwise off at C0=skew, below
        horizontalOverflowed = false;
    } else {
        horizontal = (horizontal + 1) & 0xff;
        // ACCC §17.1 (p.176): "if C0 returns to 0 because it reached 255 having
        // overflowed, this does not authorize the display."
        horizontalOverflowed = horizontal == 0;
        if (horizontal == registers[0]) horizontalTotalMatch = true;
        maRow = (maRow + 1) & 0x3fff;
    }
    vlc = videoRaster();
    if (horizontal == registers[1]) {
        if (behaviour->decidesR1ReloadAtCharacterEnd()) r1ReloadPending = true;
        else behaviour->reloadNextRowAddress(*this);
        // ACCC §18.3.2: on CRTC 0 and 2, if R6 is still 0 when C0 reaches R1 on the
        // first line of the frame, the R6 BORDER stops being cancellable.
        if (behaviour->r6ZeroConflictBecomesDefinitive(*this)) vDisplay = false;
    }
    // ACCC §17.5: R1=0 raises the BORDER only if the write met this line's deadline.
    if (registers[1] == 0 && !r1ZeroLate) hDisplay = false;
    else {
        // ACCC §17.1's overflow exception (p.176): "if C0 returns to 0 because it
        // reached 255 having overflowed, this does not authorize the display (at least
        // on the CRTC 0 but is yet to be verified on the other CRTC's)". The CPCWiki's
        // DISPTMG section contradicts that for every other chip -- "On all CRTCs,
        // HBORDER is disabled when HCC=0 and enabled when HCC=R1", its only exception
        // being CRTC 2 during a HSYNC. SHAKER cannot separate them: authorizing the
        // display on an overflow wrap moves NO screen at all in CRTC 1's modules A and
        // D, base and new built from the same tree. (An earlier run that appeared to
        // charge it 7.6 on DH/B1 was comparing against a base binary that did not
        // match its source -- the same screen measures 20.7 either way, twice in a
        // row.) So the gate is silent and the ACCC's chapter beats the wiki's summary:
        // the exception it could only claim for CRTC 0 is applied to every chip until
        // a test actually distinguishes them.
        if (horizontal == dispenSkew && !horizontalOverflowed) hDisplay = true;
        // ACCC §19.2 (p.194): with a 1 µsec delay "the BORDER is activated on the 1st
        // character after the one where C0=R1. Note: IF R1=R0, THEN THE BORDER IS
        // ACTIVATED ON C0=0"; with 2 µsec it is C0=R1+2, and on C0=1 when R1=R0. So
        // the delayed position wraps around the line rather than running off its end.
        // ACCC §19.2.4/§19.2.5 (p.196-197): with R1>R0 "the condition C0=R1 not being
        // met during the line... THE CONDITION C0=R0 THEREFORE REPLACES THE CONDITION
        // C0=R1", and "when a delay is programmed using the SKEW DISP functions, the
        // delay is counted based on the transitions of C0" — so the deferred BORDER
        // lands skew characters after C0=R0. Without a delay the substitution is the
        // half-character border byte of §17.6.2 (anticipatesBorderByte), not a whole
        // character, so it belongs to the skewed case only. "On CRTC's 1, 3 and 4, in
        // the same context... the CRTC does not send BORDER ON signal to GATE ARRAY."
        int borderBase = registers[1];
        if (dispenSkew > 0 && registers[1] > registers[0]
            && behaviour->replacesBorderR1WithR0()) borderBase = registers[0];
        int borderOn = borderBase + dispenSkew;
        if (borderBase <= registers[0] && borderOn > registers[0])
            borderOn -= registers[0] + 1;
        if (horizontal == (borderOn & 0xff)) hDisplay = false;
    }
    if (skewDelayLine) {
        hDisplayInternal = hDisplay;
        // What this character feeds the delay. §19.2.4: "The condition C0=R1 not being met
        // during the line, the BORDER signal is then sent... THE CONDITION C0=R0 THEREFORE
        // REPLACES THE CONDITION C0=R1" -- a C0=R0 reached with the display still on is a
        // BORDER character in the chip's own signal. Undelayed it reaches the GATE ARRAY
        // as §17.6.2's half byte (anticipatesBorderByte); delayed, "the 'deferred' BORDER
        // will be displayed at the beginning of the character, as it would have been on
        // the condition C0=R1" -- a whole one, `skew` characters on (p.196's rows 2 and 3:
        // C0=0 with +1, C0=1 with +2, and C0=0 still displayed with +2).
        const bool feed = hDisplay
            && !(behaviour->replacesBorderR1WithR0() && horizontal == registers[0]);
        const bool pin = pinSkew == 0 ? hDisplay : ((skewHistory >> (pinSkew - 1)) & 1) != 0;
        skewHistory = ((skewHistory << 1) | (feed ? 1 : 0)) & 3;
        hDisplay = pin;
    }
    checkHsync();
    // ACCC §15.3.3: both flags describe the character just processed and are
    // consumed by checkHsync above; a write landing after this point belongs to the
    // NEXT character, so they are cleared here and not at tickCharacter entry.
    hsyncEndedThisCharacter = false; r3WrittenThisCharacter = false; r2WrittenThisCharacter = false; r7WrittenThisCharacter = false;
    onCharacter();
    onLightgunBeam(horizontal, scanlineInFrame);
}

} // namespace cpcse
