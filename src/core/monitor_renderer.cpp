// CPCSyntaxError — the CTM 640/644 monitor, modelled as a chip. See the header.
#include "monitor_renderer.h"
#include <cstdio>
#include <cstdlib>

namespace cpcse {

void CtmMonitor::reset() {
    character = 0;
    pixel = 0;
    hsyncLimit = MONITOR_HSYNC_MID;
    hsyncCount = 0;
    hsyncMatch = 0;
    lineOffset = 0;
    vsyncLimit = MONITOR_VSYNC_MID;
    vsyncCount = 0;
    vsyncMatch = 0;
    vsyncPulse = false;
    verticalFrameEdge = false;
    verticalOffset = 0;
    verticalPhase = 0;
    lastVsyncPeriod = 0;
    verticalHalfLine = 0;
    lockedLines = MONITOR_VSYNC_MID / 2;
    csyncLast = false;
    pulseWidth = 0;
    preTipMatch = 0;
    tipRetriggered = false;
    lastPulseWidth = 4;
    lineSyncAt = -1; tipStartAt = 0;
    afcCharge = 0;
    lineSnap16 = 0;
    measuredPeriod = MONITOR_HSYNC_MID;
    sinceLastSync = 0;
    separator = 0;
    vsyncSeparated = false;
    curN = 0; curErr.fill(0); curWid.fill(0); pendWid.fill(0); widthSlot = -1; widthSlotPend = false;
    pendActive = false;
    pendSince = 0; pendN = 0; pendErr.fill(0);
    pendStable = 0; pendPhase = 0;
    preCurN = 0; prePendN = 0;
    curAt.fill(0); pendAt.fill(0);
    pixelClock = 0; lastLineSyncAt = -1;
    fieldPeriodSum = 0; fieldPeriodCount = 0;
}

// One tip into a discriminator window -- only recorded; decide() reads the window whole,
// because which tip is the line sync is not known until they have all arrived.
void CtmMonitor::addTip(int err, long long when, std::array<int, MONITOR_PHASE_TIPS>& at,
                        std::array<long long, MONITOR_PHASE_TIPS>& times, int& n) {
    if (n >= MONITOR_PHASE_TIPS) return;
    at[n] = err;
    times[n] = when;
    n += 1;
}

// The decision for the last flyback, MONITOR_PHASE_WINDOW Pixel-M2 after it. A window
// that carried a second sync inside the ramp is settled by the discriminator, gently --
// it is a disturbance, not a loss of lock, and must not reach the re-lock. Anything else
// is decided exactly as it always was, on the snapshot taken at the flyback itself.
void CtmMonitor::decide() {
    pendActive = false;
    // Diagnostics: what this decision does to the sweep, filed on the line by the host.
    const int countBefore = hsyncCount;
    struct Record {
        CtmMonitor& m; const int& before;
        ~Record() { m.decisionMove = m.hsyncCount - before; m.decisionSeq += 1; }
    } record{*this, countBefore};
    decisionTips = pendN; decisionPhase = pendPhase; decisionBranch = '-';
    decisionPull = 0; decisionWidth = lastPulseWidth; decisionSlow = 0;
    // A tip that arrived inside the window recorded its position BEFORE the pull below
    // moved the counter; where the flyback used to pull first, it recorded it after. Carry
    // it along with the pull so the next flyback sees the same distance either way.
    const int before = hsyncCount;
    struct Carry {
        CtmMonitor& m; const int& b; bool tip;
        ~Carry() { if (tip) { m.hsyncMatch += m.hsyncCount - b; m.preTipMatch += m.hsyncCount - b; } }
    } carry{*this, before, tipInWindow};
    tipInWindow = false;
    // The tip nearest the expected sync is the line sync. Every other one outside the
    // dead zone and inside the ramp adds the ramp's saturated end value, signed by its
    // side of the line sync and weighted by the side's own gain.
    int nearest = MONITOR_PHASE_NO_TIP, farPull = 0, flatPull = 0;
    int best = 0;
    for (int i = 1; i < pendN; i += 1)
        if (std::abs(pendErr[i]) < std::abs(pendErr[best])) best = i;
    bool regularSync = false;             // this line's sync follows the last by a line
    if (pendN >= 1) {
        // This window's line sync: time it, and the line period is the distance from the
        // last one. Only a period the flywheel could follow counts toward the field's rate.
        const long long at = pendAt[best];
        if (lastLineSyncAt >= 0) {
            const long long period = at - lastLineSyncAt;
            if (period >= MONITOR_HSYNC_MIN && period <= MONITOR_HSYNC_MAX) {
                fieldPeriodSum += period;
                fieldPeriodCount += 1;
                regularSync = true;
            }
        }
        lastLineSyncAt = at;
    }
    // The LA7800's AFC filter is 4.7k x 1uF -- 4.7 ms, about 73 line periods (see
    // MONITOR_AFC_LINES; the 56k in docs/LA7800.pdf p.2 goes to the oscillator's control
    // pin, not the filter). A loop that slow barely responds to the four lines of a
    // vertical pulse, and it must not respond at all: §16.2.2's C-SYNC is
    // an XNOR, so inside the vertical pulse the line information is INVERTED and the
    // edges the separator hands over are irregular. MEASURED off the recorded pin at
    // SHAKER B0/B: 382 of 392 intervals are exactly 1024 Pixel-M2, and every one of the
    // ragged ones (1600, 1664, 512, and the 960-wide serrations) is in the vertical
    // region. So the sync NEVER MOVES during the picture -- the phase disturbance this
    // loop was reacting to was manufactured here, out of the vertical pulse, and cost 52
    // lines of reacquisition afterwards.
    //
    // A television coasts: the flywheel keeps running and the AFC ignores the interval.
    if (vsyncSeparated) { decisionBranch = 'v'; return; }
    // EXACTLY TWO tips, which is what the response was measured on.
    //
    // SHAKER's AR family puts THREE on a line -- measured off its own pin at 0, +17 and
    // +37 characters, with the first two sometimes only 2 Pixel-M2 wide (9.3.1's "very
    // short C-HSYNC... much too short to be detected by the monitor") -- and its
    // references show the picture NOT displaced at all, on all six screens. How a
    // discriminator sums two interlopers is not something the references measure: taking
    // the taper at face value on those lines put every AR screen 1.5 to 4.2 worse.
    //
    // So the measured rule applies where it was measured. Three or more tips get no pull,
    // which is also what the old 16-character window did to them by accident.
    if (pendN == 2) {
        nearest = pendErr[best];
        for (int i = 0; i < pendN; i += 1) {
            if (i == best) continue;
            const int apart = pendErr[i] - nearest;
            if (std::abs(apart) <= MONITOR_PHASE_DEADZONE) continue;    // same sync complex
            if (std::abs(apart) >= MONITOR_PHASE_WINDOW) continue;      // the next line's
            // The saturated pull for this pulse, tapered by where in the half line the
            // tip fell -- both measured, see the header.
            // ...and THIS pulse's width, not the line sync's. It used to take lastPulseWidth,
            // the width of the sync the sweep locked to: harmless on D3, whose interloper is a
            // full C-HSYNC too, but SHAKER AR on CRTC 3/4 puts a line sync of 64 Pixel-M2 next
            // to an R3l=2 pulse a few Pixel-M2 wide, and pulled the band 13 px at the full
            // pulse's strength -- where AmSpiriT and the photograph show it lined up.
            const int width = pendWid[i] > 0 ? pendWid[i] : lastPulseWidth;
            decisionWidth = pendWid[i];                                  // diagnostics: the interloper's
            const int pull = monitorPhaseTaper(apart > 0 ? monitorPhasePullLate16(width)
                                                         : monitorPhasePullEarly16(width),
                                               std::abs(apart), apart < 0);
            farPull += apart > 0 ? pull : -pull;
            if (std::abs(apart) <= (apart < 0 ? MONITOR_PHASE_FLAT_EARLY : MONITOR_PHASE_FLAT))
                flatPull += apart > 0 ? pull : -pull;
        }
    }
    if (farPull != 0) {
        decisionBranch = 'd';                   // discriminator: a second sync on the line
        // The discriminator's pull has TWO parts on the real set, and the photographs of
        // SHAKER D3 show both (measured row by row, ruler = the panel's width): a fast one,
        // 16 px within two lines on D3/A4 and 14 on D3/B8 -- the response this loop has
        // always drawn -- and then a slow drift that is still going when the 64-line band
        // ends, 45 px in all on A4 and 25 on B8. The slow part is the AFC filter's
        // capacitor charging: the LA7800's 4.7k x 1uF (MONITOR_AFC_LINES) sets its time
        // constant, and the datasheet gives no loop gain, so how far it goes is read off
        // those two photographs (MONITOR_AFC_SLOW_GAIN: the fast target plus 2x it through
        // the RC reaches 2.16x the fast target at 64 lines, which is 45 on A4 and 22 on B8;
        // D3/A11 ends at 29 on the photo, 28 here).
        //
        // ONLY for a second sync on the flat of the ramp. SHAKER AT and AY put theirs 26-31
        // characters away, on the taper, and their photographs show the fast part alone: the
        // block jumps ~16 px in five lines and then holds (AY/H even eases back 4). D3's sit
        // 7-19 characters away, on the flat, and all of them keep drifting.
        //
        // ...and ONLY while the line's own sync is in lock. The capacitor's drift is the loop
        // following a bias on a sync it is tracking; SHAKER AR moves R2 itself, its line
        // sync lands ~20 characters from where the oscillator expects it (pendPhase -317 on
        // AR/A1), and its photographs have no fast jump and only a slow +10 px. D3/A11's
        // reads -100 and its photograph drifts. MONITOR_AFC_LOCK16 sits between those two
        // measured points; AR's own response (slow only) is not modelled yet.
        const bool inLock = std::abs(pendPhase) <= MONITOR_AFC_LOCK16;
        afcCharge += MONITOR_AFC_SLOW_GAIN * (inLock ? flatPull : 0) - afcCharge / MONITOR_AFC_LINES;
        const int phase = nearest + farPull + afcCharge / MONITOR_AFC_LINES;
        decisionPull = farPull; decisionSlow = afcCharge / MONITOR_AFC_LINES;
        if (phase < 0) hsyncCount += phase < -5 ? 3 : 1;
        else if (phase > 0) hsyncCount -= phase > 5 ? 3 : 1;
        return;
    }
    afcCharge = 0;                              // the band is over; the lock takes it back
    if (pendStable > 0) {
        decisionBranch = 'l';                   // locked: gentle pull, or a re-lock snap
        const int phase = pendPhase;
        // A horizontal AFC has a PULL-IN RANGE, and outside it the sweep does not crawl:
        // it re-locks. Inside a character the loop is the gentle one it always was, a
        // Pixel-M2 or three or five a line, because that is the filter that keeps a noisy
        // or deliberately-abused sync from jittering the picture.
        //
        // Beyond a character it has to snap, and Pinball Dreams is the demonstration.
        // It ends its scoreboard with ONE 46 usec line (R0=45) and then runs the
        // playfield at R0=63 again. R2 never moves, so the content sits the same
        // distance behind its HSYNC on every one of those lines and a tube draws them
        // all in the same place -- the sweep is retriggered by the sync, so the jolt
        // costs the short line and nothing after it. Crawling back at five Pixel-M2 a
        // line instead put the playfield's first rows at monitor characters 17, 17, 17,
        // 18, 18, 18, 19, 19, 19, 19, 19, 19, 20 where every one of them belongs at 20:
        // a staircase three characters deep under the scoreboard, which is exactly what
        // the picture showed.
        const int step = phase < 0 ? -phase : phase;
        if (step > 16) { hsyncCount -= phase; lineSnap16 -= phase; }   // re-lock, this line too
        else if (phase < 0) hsyncCount += phase < -5 ? 3 : 1;
        else if (phase > 0) hsyncCount -= phase > 5 ? 3 : 1;
    // Out of lock: pull 5 Pixel-M2 a line, and ALWAYS THE SAME WAY. That looks like a
    // sign bug and is not one -- MEASURED, 2026-09-21, do not "fix" it again. pendPhase on
    // the screens that reach this branch runs about -700 on a 1024 Pixel-M2 line, i.e.
    // PAST THE HALF PERIOD, and a phase detector compares two edges on a circle: -700
    // early IS 324 late. Wrapped to the shorter way round, the error is positive and this
    // unconditional subtraction is the correct direction. Making it "signed" on the raw
    // value fixed B0/B by 3.2 and cost B0/A, B0/C, B0/D +15..20 each, DH/A5 and DH/A7
    // PASS -> FAIL, and +150 over the CRTC 1 suite; adding the wrap on top made the whole
    // change a no-op (+0.4 on B0/B, 0.0 elsewhere), which is the proof that the original
    // direction was right all along.
    //
    // ...EXCEPT when the syncs are a regular train again. The re-lock above takes an
    // early sync on the line it arrives; a train that has come back to where it was is
    // the same event from the other side, and it lands in the other half of the line.
    // Crawling back from there took SHAKER DT/B2 ("VSYNC STRETCHING", one sync 20
    // characters early for a line) 64 lines -- a diagonal tear down half the screen where
    // the real CPC shows one thin line. A sync one line period after the last one is the
    // train, and the sweep re-locks onto it at once, whichever half it fell in.
    } else if (pendStable > -1024 && regularSync) {
        decisionBranch = 'r';                   // re-lock onto a regular train
        const int wrapped = pendPhase < -(hsyncLimit >> 1) ? pendPhase + hsyncLimit : pendPhase;
        hsyncCount -= wrapped; lineSnap16 -= wrapped;
    } else if (pendStable > -1024) { decisionBranch = 'c'; hsyncCount -= 5; }   // crawl
    else decisionBranch = 'n';                  // no line sync at all: coast
}

void CtmMonitor::hSync() {
    hsyncMatch = hsyncCount + 1;
}

void CtmMonitor::vSync() {
    // CPCSE_VSYNC_TRACE=1 logs EVERY vertical sync the separator picks out of the pin,
    // with the gap in lines since the previous one and the count since the last one the
    // flywheel accepted. The two numbers are different questions and both matter: the
    // gap is what the machine is generating, the cumulative count is what the deflection
    // is being asked to follow.
    //
    // This is what a "the picture rolls" report has to be turned into before it can be
    // worked on. A histogram of the gaps says immediately whether the program is driving
    // a frame rate the monitor cannot follow (which a CTM's flywheel will pull to over a
    // wide range -- MONITOR_VSYNC_MIN..MAX here) or whether it has stopped producing a
    // frame at all. Pinball Dreams turned out to be the second: gaps of 245 and 1600
    // lines in the same second, not a shifted rate.
    static const bool traceVsync = std::getenv("CPCSE_VSYNC_TRACE") != nullptr;
    if (traceVsync) {
        static int since = 0;
        if (vsyncCount < since) since = 0;           // the flywheel accepted one and reset
        std::fprintf(stderr, "CVSYNC gap=%d cum=%d\n", (vsyncCount - since) / 2, vsyncCount / 2);
        since = vsyncCount < MONITOR_VSYNC_MIN ? vsyncCount : 0;
    }
    // ACCC §16.1: "As with HSYNC's, it is possible to generate several VSYNC's during
    // a frame, but the monitor will only be able to lock onto one VSYNC signal". The
    // vertical oscillator is retriggerable only once its minimum period has elapsed;
    // a pulse arriving before that -- a rupture's mid-screen VSYNC, or a second C4==R7
    // from an R7 switched during the sync -- is ignored by the flywheel.
    if (vsyncCount < MONITOR_VSYNC_MIN) {
        if (traceVsync)
            std::fprintf(stderr, "vSync REFUSED period=%d (min %d)\n",
                         vsyncCount, MONITOR_VSYNC_MIN);
        return;
    }
    // The period the deflection is pulled to is the MEAN OF TWO FIELDS, not this one.
    // An interlaced drive arrives here as 156 then 157 lines (see the header); their
    // mean is the 156.5 the CRTC actually generated, and a period with an odd number of
    // half lines is what makes alternate fields sit half a line apart. A progressive
    // drive averages two equal fields and is unchanged.
    int measured = std::min(vsyncCount, MONITOR_VSYNC_MAX);
    // The rate the oscillator is LOCKED to moves only when a new one arrives twice
    // running. One odd field is inside the pull-in range and moves nothing: a CTM fed
    // 312, 345, 312, 311 -- which is what SHAKER's module C drives while it switches IVM
    // on and off -- stays locked at 312 throughout, and each odd field simply lengthens
    // or shortens its own blanking. A signal that really has changed rate arrives at the
    // new one every field and takes the lock on its second.
    if (measured == lastVsyncPeriod) lockedLines = (measured + 1) >> 1;
    vsyncLimit = lastVsyncPeriod > 0 ? (measured + lastVsyncPeriod + 1) >> 1 : measured;
    lastVsyncPeriod = measured;
    if (traceVsync)
        std::fprintf(stderr, "vSync hsyncCount=%d measured=%d limit=%d\n",
                     hsyncCount, measured, vsyncLimit);
    vsyncCount = 0;
    // ...and "once its minimum period has elapsed" is measured from the deflection's own
    // last retrace (vsyncMatch), not from the last pulse the separator passed
    // (vsyncCount, above). A pulse that is a proper 312 lines after its predecessor can
    // still land inside the ramp's hold-off when the ramp itself is out of phase -- after
    // a free-run cut, say, or when a program moves its VSYNC -- and a triggered ramp
    // simply does not see it. LATCHING it instead fired the retrace at the minimum period
    // every field: the deflection ran at 296 lines against a 312-line signal and walked
    // back into lock 16 lines a field. SHAKER A7 (CRTC 0) and DH/B2 were both shot during
    // that walk, 16 and 32 lines off. Ignored, the next pulse lands inside the window
    // (the ramp free-runs to MAX meanwhile) and retriggers it at once.
    vsyncPulse = vsyncMatch >= MONITOR_VSYNC_MIN;
    // The line period is latched here, once per field, from what the sweep has been
    // measuring off the pin -- where the CRTC's R0 used to be read.
    // ...the field's MEAN line period, which a flywheel integrates to, not the last one.
    if (fieldPeriodCount > 0)
        measuredPeriod = (int)((fieldPeriodSum + fieldPeriodCount / 2) / fieldPeriodCount);
    fieldPeriodSum = 0;
    fieldPeriodCount = 0;
    hsyncLimit = std::max(MONITOR_HSYNC_MIN, std::min(MONITOR_HSYNC_MAX, measuredPeriod));
}

bool CtmMonitor::clockPixel(bool csyncActive) {
    // ---- sync separator ---------------------------------------------------------
    // The vertical half first: an integrator, not an edge detector.
    separator += csyncActive ? MONITOR_VSEP_CHARGE : -MONITOR_VSEP_DRAIN;
    if (separator > MONITOR_VSEP_MAX) separator = MONITOR_VSEP_MAX;
    if (separator < 0) separator = 0;
    if (separator >= vsepTrigger()) {
        if (!vsyncSeparated) { vsyncSeparated = true; vSync(); }
    } else if (separator == 0) {
        vsyncSeparated = false;
    }

    // The horizontal half. §16.2.2: "Unlike an AND-type C-SYNC signal, a C-SYNC XNOR
    // allows the HSYNC signal to coexist during the VSYNC period. THE SIGNAL STATE FOR
    // THE HSYNC INFORMATION IN THIS SITUATION IS THEN REVERSED." So inside the vertical
    // pulse the line sync is the INACTIVE notch, not the active tip, and its leading
    // edge is the one the sweep must take. Reading the active edge in both cases put
    // the line reference 4 µsec late for the four lines of every field and cost +2023
    // SHAKER error on CRTC 0 alone.
    if (pendActive) pendSince += 1;
    bool tipStart = vsyncSeparated ? (!csyncActive && csyncLast)
                                   : (csyncActive && !csyncLast);
    if (csyncActive != vsyncSeparated) {         // inside a sync tip, either polarity
        pulseWidth += 1;
    } else {
        // The tip has ENDED, so its width is finally known. It has to be latched here
        // rather than while counting: the sweep reaches the end of its line one or two
        // characters after the tip opens, so a width read mid-tip is always 1 or 2 and
        // put every picture 24 pixels out. Only a tip short enough to BE a line sync
        // counts -- the vertical pulse runs for lines. §14.4/§15.7: a narrower sync
        // moves the picture right.
        // ...and a tip the monitor cannot detect (§9.3.1, MONITOR_HPULSE_MIN) has no
        // width either. §14.4's width correction is half the shortfall against a 4 usec
        // C-HSYNC, so a two-Pixel-M2 tip taken as a sync moves the line 31 pixels right --
        // which is exactly what SHAKER's AR lines showed (off=31 on the lines that use
        // R3l=2 to change graphic mode mid-line, off=0 on the rest).
        // ...and it is the width of the pulse the SWEEP LOCKED TO, which a flywheel can
        // only do once a line: the first tip at least a line period after the one it last
        // accepted. See the header -- on DH/B1's band the line sync is 49 Pixel-M2 wide
        // and the interloper six characters later is 64, and 14.4's correction belongs to
        // the 49.
        if (pulseWidth >= MONITOR_HPULSE_MIN && pulseWidth <= MONITOR_HPULSE_MAX
            && (lineSyncAt < 0 || tipStartAt - lineSyncAt >= MONITOR_HSYNC_MIN)) {
            lastPulseWidth = pulseWidth;
            lineSyncAt = tipStartAt;
        }
        // ACCC §9.3.1: a C-HSYNC of two or three Pixel-M2 "is much too short to be
        // detected by the monitor", which is the whole point of the R3l=2 setting -- it
        // "allows graphics mode to be changed mid-line WITHOUT AFFECTING HORIZONTAL
        // SYNCHRONIZATION". The width is only known now that the tip has ended, so the
        // retrigger it caused is taken back rather than withheld.
        if (tipRetriggered && pulseWidth > 0 && pulseWidth < MONITOR_HPULSE_MIN) {
            hsyncMatch = preTipMatch;
            // ...and the discriminator never saw it either.
            curN = preCurN; pendN = prePendN;
        }
        if (widthSlot >= 0 && pulseWidth > 0) {
            (widthSlotPend ? pendWid : curWid)[widthSlot] = pulseWidth;
            widthSlot = -1;
        }
        tipRetriggered = false;
        pulseWidth = 0;
    }
    if (tipStart) {
        // Everything the tip is about to disturb, kept so a tip too short to be a line
        // sync can be undone above.
        preTipMatch = hsyncMatch;
        tipStartAt = pixelClock;
        tipRetriggered = true;
        preCurN = curN; prePendN = pendN;
        // A tip inside the last flyback's deferred window belongs to THAT decision, late
        // by pendSince; any other belongs to the next flyback, early by however far it
        // still has to run.
        if (pendActive) {
            addTip(pendSince, pixelClock, pendErr, pendAt, pendN); tipInWindow = true;
            widthSlot = pendN - 1; widthSlotPend = true;
            if (widthSlot >= 0) pendWid[widthSlot] = 0;
        } else {
            addTip(hsyncCount + 1 - MONITOR_HSYNC_MAX, pixelClock, curErr, curAt, curN);
            widthSlot = curN - 1; widthSlotPend = false;
            if (widthSlot >= 0) curWid[widthSlot] = 0;
        }
        // The line period, measured on the pin where R0 used to be read. Only an
        // interval the flywheel would accept as a line counts: a second sync inside
        // one line (ACCC §15.3, SHAKER's D3) is not the period, and does not restart
        // the measurement either.

        hSync();
    }
    if (pendActive && pendSince >= MONITOR_PHASE_WINDOW) decide();
    csyncLast = csyncActive;
    pixelClock += 1;
    sinceLastSync = lastLineSyncAt >= 0 ? (int)std::min<long long>(pixelClock - lastLineSyncAt, 1 << 30) : 0;

    // ---- horizontal sweep -------------------------------------------------------
    pixel += 1;
    character = pixel >> 4;
    hsyncCount += 1;
    if (hsyncCount < MONITOR_HSYNC_MAX) return false;

    // §14.4 (p.134): "When R3l drops, excluding R3.JIT, the image is shifted to the
    // right of half a unit", and the page works the arithmetic: with R3l=5 on CRTC 1
    // the pulse is 3.1250 µsec against 4.0000 for R3l=6, so "4-3.1250 = 0.875/2 =
    // 0.4375 or 7 Pixels (graphi mode 2) INSTEAD OF THE EXPECTED 8 PIXELS." So the
    // shift is HALF the shortfall, and the pulse is not a whole number of characters:
    // taking it as one gave 8 where this chip gives 7, at every R3l below 6.
    // ...and the shortfall is the one THIS SET MEASURED ON ITS OWN PIN, not the GATE
    // ARRAY's live H06 counter. A monitor cannot read that counter; it has one input
    // (§16.2.2) and lastPulseWidth above is what it recovers from it.
    //
    // The two disagree exactly where it matters. §16.2.3 restarts H06 on a second HSYNC
    // inside one line even while a C-HSYNC pulse is still running, so on any line that
    // moves R2 the counter is read mid-restart and comes back 0 while the pulse that
    // actually reached the monitor was the full 64 Pixel-M2. That put a 32 Pixel-M2
    // correction -- TWO CHARACTERS -- on every such line, which is precisely the
    // displacement SHAKER's AR zone showed on all five chips.
    // MEASURED: taking the width of the tip the sweep locked to instead of simply the
    // last one -- which reads better on paper, and is what §15.3's two-C-SYNC lines
    // would seem to want -- costs +133.5 on module D / CRTC 1 across 23 screens. D3 is
    // built of lines carrying two syncs and it is the LAST tip that sets where the
    // picture lands. Left as the last tip.
    lineSnap16 = 0;                     // a new line: nothing re-locked in it yet
    int widthCorrection = (64 - std::min(lastPulseWidth, 64)) / 2;
    lineOffset = (hsyncCount & 15)
        + ((MONITOR_HSYNC_MID - hsyncLimit) >> 1)
        + widthCorrection;
    // CPCSE_TRACE_LINEOFF=1 breaks the per-line horizontal origin into the three things
    // that make it, because they answer different questions and the sum answers none of
    // them: the sub-character phase the sweep happens to be at, the correction for a
    // line period that is not 64 usec, and §14.4's half-the-shortfall for a C-HSYNC
    // shorter than 4 usec. A picture displaced by whole characters after an R2 or R3
    // move is one of these three or none of them, and only this says which.
    // traceLineOffsetBudget is armed by the HOST, which is the only thing that knows
    // which SHAKER screen is being captured -- the env var alone fires from boot and the
    // budget is spent long before a screen 500 seconds into module B. The runner arms it
    // on SHAKER_DUMP_CODE, exactly as it arms the CRTC's own per-line trace.
    if (traceLineOffsetBudget != 0 || std::getenv("CPCSE_TRACE_LINEOFF")) {
        static long budget = -1;
        if (budget == -1) {
            const char* cap = std::getenv("CPCSE_TRACE_LINEOFF_MAX");
            budget = cap ? std::atol(cap) : 400;
        }
        if (traceLineOffsetBudget > 0) budget = traceLineOffsetBudget;
        // Only the lines that are NOT nominal: a 64 usec period with a full 4 usec
        // C-HSYNC contributes nothing, and printing those buries the ones that do.
        bool nominal = (hsyncCount & 15) == 0 && hsyncLimit == MONITOR_HSYNC_MID
                       && widthCorrection == 0;
        if (budget > 0 && !nominal) {
            std::fprintf(stderr, "LINEOFF %4d = phase %2d + period %3d + width %2d   "
                                 "(hsyncLimit %4d, measured pulse %d)\n",
                         lineOffset, hsyncCount & 15,
                         (MONITOR_HSYNC_MID - hsyncLimit) >> 1, widthCorrection,
                         hsyncLimit, lastPulseWidth);
            budget -= 1;
            if (traceLineOffsetBudget > 0) traceLineOffsetBudget -= 1;
        }
    }

    int pulsePeriod = hsyncCount - hsyncMatch;
    int stable = hsyncLimit - pulsePeriod * 2;
    if (pullTraceBudget > 0) {
        std::fprintf(stderr, "PULL vsep=%d sep=%4d vsyncCount=%4d  "
                             "hsyncCount=%5d hsyncMatch=%5d pulsePeriod=%4d stable=%5d "
                             "phase=%5d lastPulseWidth=%3d\n",
                     vsyncSeparated ? 1 : 0, separator, vsyncCount,
                     hsyncCount, hsyncMatch, pulsePeriod, stable,
                     hsyncMatch - MONITOR_HSYNC_MAX, lastPulseWidth);
        pullTraceBudget -= 1;
    }
    // The decision this flyback would take is snapshotted here, and taken
    // MONITOR_PHASE_WINDOW Pixel-M2 later (decide()) once the tips just past the flyback
    // have had the chance to arrive. A window still open from the last flyback is closed
    // first -- it cannot be, at a line of 62 characters or more, but it must never leak.
    if (pendActive) decide();
    tipInWindow = false;
    pendActive = true;
    pendSince = 0;
    pendN = curN; pendErr = curErr; pendAt = curAt; pendWid = curWid;
    if (widthSlot >= 0 && !widthSlotPend) widthSlotPend = true;          // still open: now in pend*
    else if (widthSlotPend) widthSlot = -1;                              // its window has closed
    pendStable = stable;
    pendPhase = hsyncMatch - MONITOR_HSYNC_MAX;
    curN = 0;

    hsyncMatch -= hsyncLimit;
    preTipMatch -= hsyncLimit;       // the pre-tip reference rides the wrap too
    hsyncCount -= hsyncLimit;
    character = 0;
    pixel = 0;
    vsyncCount += 2;
    vsyncMatch += 2;
    if ((vsyncPulse && vsyncMatch >= MONITOR_VSYNC_MIN)
        || vsyncMatch > MONITOR_VSYNC_MAX) {
        verticalFrameEdge = true;
        vsyncPulse = false;
        vsyncMatch = 0;
        // The deflection has swept one more period. Keeping the running total and
        // reading bit 0 gives the half line this field sits at: with an odd period it
        // alternates 0, 1, 0, 1 -- interlace -- and with an even one it never moves.
        //
        // The `& ~1` that used to sit on verticalOffset quantised the origin to TWO
        // scanlines, because the field's sub-line position had nowhere to live and a
        // field one line longer than its neighbour came out two lines displaced. It has
        // somewhere to live now.
        // ...but only while the drive really is interlaced. A period of whole lines puts
        // the retrace where the sync is and nowhere else; there is no half line for it to
        // remember. As a running total the parity was a memory: ONE odd-mean field -- a
        // glitch, a ghost VSYNC, a rate change of a single line -- flipped it for good, and
        // every progressive field after it was drawn half a line down. On CRTC 2, SHAKER
        // BR/D-F's one such field moved every later screen of module B by a row (BI/A-H's
        // HSYNC box came out interleaved; B0, B9, D1 scored a row off AmSpiriT).
        if ((vsyncLimit & 1) == 0) verticalPhase = 0;
        verticalPhase += vsyncLimit;
        verticalHalfLine = verticalPhase & 1;
        verticalOffset = (MONITOR_VSYNC_MID - vsyncLimit) >> 1;
    }
    return true;
}

bool CtmMonitor::consumeVerticalFrameEdge() {
    bool edge = verticalFrameEdge;
    verticalFrameEdge = false;
    return edge;
}



} // namespace cpcse
