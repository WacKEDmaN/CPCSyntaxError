// CPCSyntaxError — the CTM 640/644 monitor, modelled as a chip.
//
// It has ONE synchronisation input: pin 4 of the DIN6, carrying the composite C-SYNC
// the GATE ARRAY generates from the CRTC's two sync pins (ACCC §16.2.2/§16.2.3). A
// monitor cannot read a CRTC register, so nothing in here may either -- the line
// period, the sync pulse width and the vertical sync are all recovered from that one
// pin, as the CTM's own parts recover them: §16.2.2 names a Sanyo LA7800 separating
// horizontal from vertical and an LA7830 driving the vertical deflection.
//
// It used to be handed crtc->registers[0] for the line period, effectiveHsyncWidth()
// for the pulse width, and registers[4]/[12] for a "suppress the width correction"
// heuristic, while its vertical sync came from a direct call on the GATE ARRAY's V26
// counter. None of those wires exist on a real set.
#pragma once
#include "common.h"
#include "monitor_model.h"
#include <array>

namespace cpcse {

// The horizontal flywheel's acceptance window, in 1/16ths of a character. A sync that
// falls outside it is not the line sync and cannot retrigger the sweep.
inline constexpr int MONITOR_HSYNC_MIN = 62 * 16;
inline constexpr int MONITOR_HSYNC_MAX = 66 * 16;
inline constexpr int MONITOR_HSYNC_MID = 64 * 16;
// The back porch: how far the visible line sits after the sync the monitor locks to.
// That sync is the GATE ARRAY's C-HSYNC, which §16.2.3's H06 raises 2 characters after
// the CRTC's own HSYNC -- so this is 2 characters shorter than when the renderer was
// (wrongly) referenced to the CRTC pin. Measured: the picture moved left by exactly
// 32 pixels when the reference moved, and these 2 characters put it back.
//
// This is the WHOLE-CHARACTER part only. §16.2.3 (p.165) puts the edge the monitor
// locks to a Pixel-M2 or two off the character grid, so the real back porch is not a
// whole number of characters; that part is per-CRTC and is added where the line is
// placed (CrtcBehaviour::cHsyncRiseAdvance16, RasterLine::cHsyncRiseAdvance16).
inline constexpr int MONITOR_CROP_LEFT = 13 * 16;
inline constexpr int MONITOR_VSYNC_MID = 312 * 2;
inline constexpr int MONITOR_VSYNC_MIN = 296 * 2;
inline constexpr int MONITOR_VSYNC_MAX = 352 * 2;

// The vertical sync separator is an integrator, not an edge detector. §16.2.2's C-SYNC
// is an XNOR, so during the vertical pulse the pin sits ACTIVE for whole lines and the
// line syncs only notch it; an integrator rides straight over those notches and crosses
// its threshold part-way into the pulse, while a 4 µsec line sync never gets near it.
inline constexpr int MONITOR_VSEP_CHARGE = 1;    // per active Pixel-M2
inline constexpr int MONITOR_VSEP_DRAIN = 2;    // per inactive Pixel-M2
// WHERE THE THRESHOLD IS, and it is measured: ACCC §16.2.4 (p.166) "TOLERANCES" --
// "according to measurements made on SEVERAL CTMs, THE DURATION OF THE SIGNAL EMITTED FOR
// THE MONITOR MUST BE GREATER THAN 11-12 uSECONDS. Below this value, THE MONITOR CAN NO
// LONGER 'ANCHOR' THE IMAGE, regardless of the adjustment made with the potentiometer on
// the back of the monitor."
//
// With CHARGE 1 per active Pixel-M2 that is a threshold of 11-12 x 16 = 176-192, and a
// 4 µsec line tip peaks at 64 and drains straight back, nowhere near it. The figure is a
// property of the SET, so it comes from the monitor model (vsyncAnchorMinimumMicroseconds)
// and this is only the fallback for a chip with no set plugged in.
inline constexpr int MONITOR_VSEP_TRIGGER = 12 * 16;
inline constexpr int MONITOR_VSEP_MAX = 1024;

// A sync pulse longer than this is not a line sync, so its width must not be taken as
// one: 4 µsec is the C-HSYNC's own cap (§16.2.2), which is 64 Pixel-M2, and this leaves
// margin above it.
inline constexpr int MONITOR_HPULSE_MAX = 128;

// ...and the floor, which ACCC §9.3.1 (p.52) states outright:
//
//   "If the HSYNC-GA is programmed to 2 usec (via the CRTC's R3l programming), then A
//    VERY SHORT C-HSYNC SIGNAL IS PRODUCED. On CRTC 0 and 1, IT LASTS 2 OR 3 PIXEL-M2
//    (1 pixel-M2=0.0625 usec). On CRTC 2 it lasts 1 or 2 pixel-M2. HOWEVER, THIS SIGNAL
//    IS MUCH TOO SHORT TO BE DETECTED BY THE MONITOR. This setting does not prompt the
//    monitor to give up its guts and ALLOWS GRAPHICS MODE TO BE CHANGED MID-LINE WITHOUT
//    AFFECTING HORIZONTAL SYNCHRONIZATION."
//
// The compendium fixes the cases either side rather than the threshold itself. §14.4's
// table (p.134) gives the C-HSYNC duration per R3l: R3l=2 is 0.1250-0.1875 usec on
// CRTC 1 -- 2-3 Pixel-M2, the one it calls too short -- and R3l=3 is 1.0625-1.1875 usec,
// 17-19 Pixel-M2, an ordinary line sync. Half a character sits between them with room on
// both sides.
inline constexpr int MONITOR_HPULSE_MIN = 4;

// ---- a SECOND sync inside the line: what the phase discriminator does with it ---------
//
// ACCC §15.3.4 (p.151) draws two bars on one CRTC 1 line and labels both "Monitor Sync":
// the chip "has time to generate an 'invisible' end of HSYNC, then immediately reactivate
// the signal for the GATE ARRAY", which "reset to 0 its internal character counter and
// sends a second HSYNC monitor". The compendium stops at the pin; what the SET does with
// the second pulse is measured off SHAKER D3's references, at the panel's left edge:
//
//   * the doubled lines move by a SUB-CHARACTER amount: +13 Pixel-M2 with R3=6, +10 with
//     R3=5, +6 with R3=4;
//   * the amount does NOT depend on how far away the second sync is -- A8, A9, A10 and A11
//     put it 12, 11, 8 and 7 characters out and all four move by the same +13. That is a
//     SATURATED discriminator: past its ramp the pull is the ramp's end value;
//   * the sign follows the side: a second sync EARLIER in the line pulls the sweep earlier
//     so the picture starts later (edge +13); one LATER pulls the other way (A1, -23);
//   * far enough out there is no response at all (A7, 33 characters: nothing moves).
//
// The magnitude tracks the pulse the detector integrates, which this chip measures off
// the pin already -- and the response is ASYMMETRIC, stronger for a sync that arrives
// after the line sync than for one before it.
//
// MEASURED ACROSS THE WHOLE D3 FAMILY, 2026-09-22 (tools/monitor_response.py: the second
// sync's distance and width come off the RECORDED PIN, the displacement off the reference
// images). All four R3 groups, saturated:
//
//     pulse width (Pixel-M2)     18    34    50    64
//     second sync LATER          -4   -10   -17   -23
//     second sync EARLIER         -     +6   +10   +13
//
// The late column is (width - 9) * 5/12, rounded: 3.75, 10.4, 17.1, 22.9. The early one
// is four sevenths of it: 2.1, 5.9, 9.8, 13.1. The 9 Pixel-M2 is a threshold the
// discriminator does not respond below -- a 2 usec C-HSYNC (32 Pixel-M2 at most, and
// 2-3 on CRTC 0 and 1 per 9.3.1) barely moves the picture at all, which is what makes
// mid-line mode switching possible.
//
// The old model was the same shape read off the A, B and C groups only -- width x 7/20
// and 4/20 -- which gave 22/17/11/6 and 12/10/6, i.e. a Pixel-M2 out on three of the
// eight cells. These reproduce all eight exactly.
inline constexpr int MONITOR_PHASE_DEAD16 = 9;      // no response below this pulse width
inline int monitorPhasePullLate16(int pulseWidth16) {
    const int effective = pulseWidth16 - MONITOR_PHASE_DEAD16;
    return effective <= 0 ? 0 : (effective * 5 + 6) / 12;
}
inline int monitorPhasePullEarly16(int pulseWidth16) {
    return (monitorPhasePullLate16(pulseWidth16) * 4 + 3) / 7;
}
// ...and a second pulse that starts within 6 usec of the line sync is not an interloper
// at all but part of the same sync complex: §9.3.1 caps HSYNC-GA at 6 usec, and SHAKER's
// D1/A ("CSYNC4 VS 2 X CSYNC2") drives two 2 usec pulses 80 Pixel-M2 apart that the
// reference draws exactly where one sync would put the line. Counting that pair as an
// interloper moved the whole of D1/A 6 Pixel-M2 left, 3.5 -> 16.4 on CRTC 0 and 2. D3's
// nearest interloper is 112 out and does move.
inline constexpr int MONITOR_PHASE_DEADZONE = 6 * 16;
inline constexpr int MONITOR_PHASE_TIPS = 8;         // tips a window can hold
// THE RAMP'S SHAPE, measured the same way (tools/monitor_response.py). The response is
// flat over most of the half line and collapses at its end, which is where a second sync
// stops belonging to this line's sweep and starts belonging to the next one's:
//
//     distance (characters)      7   12   13   19   29   30   31
//     response (of saturation)   1    1    1    1  .43  .22    0
//
// Straight through the last three: zero at 31.0 characters, full at 26.4. The screens in
// that collapse -- D3/A4, A5, A6, B4, B5, B6, C4, C5, C6 and their D relatives -- were
// ALL at zero before, because the ramp was a hard window 16 characters wide: the chip
// saturates at 19 characters and we gave nothing, which is 23 Pixel-M2 of error on one
// band of every one of them.
//
// 31 characters is just inside the half line (32), as a phase discriminator's
// characteristic must be: past the half line the tip is nearer the NEXT line's sync, and
// it is that sweep it pulls.
inline constexpr int MONITOR_PHASE_WINDOW = 496;     // 31.0 characters: no response at all
inline constexpr int MONITOR_PHASE_FLAT = 422;       // 26.4 characters: saturated to here
// What the taper leaves of a saturated pull at this distance.
inline int monitorPhaseTaper(int pull, int distance16) {
    if (distance16 <= MONITOR_PHASE_FLAT) return pull;
    if (distance16 >= MONITOR_PHASE_WINDOW) return 0;
    const int span = MONITOR_PHASE_WINDOW - MONITOR_PHASE_FLAT;
    return (pull * (MONITOR_PHASE_WINDOW - distance16) + span / 2) / span;
}
inline constexpr int MONITOR_PHASE_NO_TIP = 1 << 24;

// The plain state object returned by save()/consumed by restore().
struct MonitorRendererState {
    int character = 0;
    int pixel = 0;
    int hsyncLimit = MONITOR_HSYNC_MID;
    int hsyncCount = 0;
    int hsyncMatch = 0;
    int lineOffset = 0;
    int vsyncLimit = MONITOR_VSYNC_MID;
    int vsyncCount = 0;
    int vsyncMatch = 0;
    bool vsyncPulse = false;
    bool verticalFrameEdge = false;
    int verticalOffset = 0;
    int verticalPhase = 0;
    int lastVsyncPeriod = 0;
    int verticalHalfLine = 0;
    int lockedLines = MONITOR_VSYNC_MID / 2;
    bool csyncLast = false;
    int pulseWidth = 0;
    int preTipMatch = 0;
    bool tipRetriggered = false;
    int lastPulseWidth = 4;
    long long lineSyncAt = -1;
    long long tipStartAt = 0;
    int measuredPeriod = MONITOR_HSYNC_MID;
    int sinceLastSync = 0;
    int separator = 0;
    bool vsyncSeparated = false;
    int curN = 0;
    std::array<int, MONITOR_PHASE_TIPS> curErr{};
    bool pendActive = false;
    int pendSince = 0, pendN = 0;
    std::array<int, MONITOR_PHASE_TIPS> pendErr{};
    int pendStable = 0, pendPhase = 0;
    std::array<long long, MONITOR_PHASE_TIPS> curAt{}, pendAt{};
    long long pixelClock = 0, lastLineSyncAt = -1;
    long long fieldPeriodSum = 0;
    int fieldPeriodCount = 0;
};

class CtmMonitor {
public:
    // WHICH SET THIS IS (monitor_model.h). A monitor model is not a rendering option: ACCC
    // 15.1 (p.146) makes the back porch a property of the set, and 16.2.2 gives the colour
    // and green families different vertical deflection parts. nullptr = a CTM 640/644, the
    // reference calibration.
    const MonitorModel* model = nullptr;
    // 15.1's factory calibration as Pixel-M2 of back porch: 0 on a CTM 640/644, one
    // microsecond on the CM14 and on the CTM Amstrad calibrated for the CRTC 4 machine.
    int calibration16() const { return model ? model->calibrationMicroseconds * 16 : 0; }
    // §16.2.4's anchor minimum, in Pixel-M2 of integrator charge: how long the signal has
    // to last before this set can hold the picture vertically.
    int vsepTrigger() const {
        return model ? model->vsyncAnchorMinimumMicroseconds * 16 : MONITOR_VSEP_TRIGGER;
    }
    int character = 0;
    int pixel = 0;
    int hsyncLimit = MONITOR_HSYNC_MID;
    int hsyncCount = 0;
    int hsyncMatch = 0;
    int lineOffset = 0;
    // CPCSE_TRACE_TIP, aimed at one SHAKER screen by the runner: log this many line-end
    // pulls. What the flywheel is doing with the syncs it is being given is not visible
    // from the outside, and a line carrying two of them (SHAKER D3) is decided here.
    long pullTraceBudget = 0;
    int vsyncLimit = MONITOR_VSYNC_MID;
    int vsyncCount = 0;
    int vsyncMatch = 0;
    bool vsyncPulse = false;
    bool verticalFrameEdge = false;
    int verticalOffset = 0;
    // THE VERTICAL FLYWHEEL HAS INERTIA. §16.2.2's vertical deflection is an LA7830
    // driving a coil, not a counter that restarts on an edge: it free-runs at whatever
    // period it has been pulled to and keeps that period across a field whose drive is
    // a little short or a little long.
    //
    // That is the only way the CPC's interlace can reach the screen. ACCC §19.7.1 wants
    // each field to occupy "32 µs more (with R0=63)" -- half a line -- and the CRTC does
    // deliver exactly that, alternating a whole extra line (§19.6.2) against a MID-VSYNC
    // half a line earlier. But the GATE ARRAY re-times C-VSYNC to the end of the 2nd
    // HSYNC (§16.2.3), so what arrives here is quantised back to whole lines: 156 then
    // 157, never 156.5. A flywheel running at the AVERAGE of two fields runs at 156.5,
    // and its phase against the line grid then alternates by half a line all by itself.
    //
    // vsyncLimit is that averaged period (odd under interlace, even without it), so
    // accumulating it and keeping bit 0 is the half line -- and it costs nothing when
    // the signal is not interlaced, because an even period never changes the parity.
    int verticalPhase = 0;          // half lines swept since reset, modulo nothing
    int lastVsyncPeriod = 0;        // the previous field's measured period, for the mean
    int verticalHalfLine = 0;       // 0 or 1: where this field sits between two lines
    // The period the vertical deflection is actually RUNNING at, in whole lines, which
    // is not the same question as vsyncLimit above. vsyncLimit is a two-field mean and
    // is meant to be pulled about by half a line; this is the rate the oscillator has
    // locked to, and it only moves when a new rate arrives twice running.
    //
    // A single odd field must not move it. SHAKER's module C drives a repeating cycle of
    // 312, 345, 312 and 311 line frames on CRTC 1 as it switches IVM on and off; a mean
    // over two fields reads 329 in the middle of that and would place the picture
    // seventeen lines out on frames that are perfectly ordinary. A deflection does not
    // do that -- §16.2.4 has the monitor "anchor" the image and gives the V-Hold
    // potentiometer, not the drive, the last word on how far it can be pulled.
    int lockedFieldLines() const { return lockedLines; }
    int lockedLines = MONITOR_VSYNC_MID / 2;   // 312: the rate a CTM is built for
    // ---- recovered from the pin, never from a register --------------------------
    bool csyncLast = false;         // the pin's level on the previous character
    int pulseWidth = 0;             // characters the pin has been active for
    // The flywheel state as it stood BEFORE the tip now running retriggered it, so a tip
    // that turns out to be too short to be a line sync can be taken back. Restoring is
    // the only shape that leaves an ordinary sync bit-identical: deferring the retrigger
    // until the tip is long enough instead moves it seven Pixel-M2 later in TIME, and the
    // sweep's own line frequently ends inside that window and reads the previous line's
    // reference. Measured, when it did: 41 screens eight pixels one way, 18 the other.
    int preTipMatch = 0;
    bool tipRetriggered = false;
    // THE DISCRIMINATOR'S WINDOW STRADDLES THE FLYBACK. The sweep ends right ON the line
    // sync, so a sync that lands a Pixel-M2 late falls into the next sweep -- and on the
    // first line of a doubled block that left the interloper alone in its sweep, where the
    // re-lock below took it for the line sync and jumped the whole gap onto it (measured
    // by replaying SHAKER D3/A11's real pin through this chip: 100 Pixel-M2 out where the
    // reference wants 13). So each flyback's decision is DEFERRED by the ramp's width and
    // takes in the tips that arrive in that time too. For a line with one sync this is
    // behaviour-neutral: the pull only ever adjusts a counter, and adjusting it
    // MONITOR_PHASE_WINDOW Pixel-M2 later moves the next flyback by exactly as much.
    int curN = 0;                                                       // tips for the next flyback
    std::array<int, MONITOR_PHASE_TIPS> curErr{};                       // ...and where each fell
    bool pendActive = false;                                            // a flyback awaiting its decision
    int pendSince = 0, pendN = 0;
    std::array<int, MONITOR_PHASE_TIPS> pendErr{};
    int pendStable = 0, pendPhase = 0;                                  // the one-sync decision, taken as it was
    bool tipInWindow = false;                                           // hsyncMatch set inside it
    int preCurN = 0, prePendN = 0;                                      // undo for a too-short tip
    // THE LINE PERIOD, MEASURED BETWEEN LINE SYNCS. It used to be counted by sinceLastSync,
    // reset only when a gap happened to land inside the acceptance window -- and at the end
    // of every vertical pulse §16.2.2's XNOR flips the pin's polarity, one gap lands outside
    // it, and the counter never re-acquired. From the first field on it ran up through
    // signed overflow on EVERY screen and measuredPeriod stayed frozen at its first value.
    // On a screen whose line is not 64 usec that is visible: the bench's R0=62 frame zig-
    // zagged up to 19 Pixel-M2 between adjacent lines as the sweep, still running 64
    // characters, was dragged back to a 63-character line every other line.
    //
    // Now: each window's line sync -- the tip decide() takes as nearest -- is timestamped,
    // the period is the distance between consecutive ones, and the field's accepted periods
    // are AVERAGED into the rate latched at the vertical sync. A flywheel integrates; the
    // last period before the vertical is whatever a rupture near the frame edge made it,
    // and latching that instead is what the first attempt at this fix did, at +57.9.
    std::array<long long, MONITOR_PHASE_TIPS> curAt{}, pendAt{};        // when each tip fell
    long long pixelClock = 0;                                           // Pixel-M2 since reset
    long long lastLineSyncAt = -1;                                      // the previous line sync
    long long fieldPeriodSum = 0;                                       // this field's accepted periods
    int fieldPeriodCount = 0;
    // THE WIDTH OF THE PULSE THE SWEEP LOCKED TO -- not merely the last short pulse on
    // the pin. 14.4's correction is half the shortfall of the sync THIS LINE'S SWEEP
    // STARTED ON, and on a line carrying two pulses of different widths those are
    // different pulses: SHAKER DH/B1's band is a 49 Pixel-M2 line sync with a 64 Pixel-M2
    // interloper six characters later, and the reference moves it (64-49)/2 = 7 Pixel-M2
    // where taking the last pulse moves it nothing.
    //
    // A flywheel cannot retrigger twice in a line, so the line sync is the first tip at
    // least MONITOR_HSYNC_MIN after the one it last accepted. On every other screen the
    // two tips have the SAME width (measured: all 44 D3 subtests) and this changes
    // nothing.
    int lastPulseWidth = 4;
    long long lineSyncAt = -1;      // when the sweep last accepted a line sync
    long long tipStartAt = 0;       // ...and when the tip now running began
    // Armed by the host when a chosen SHAKER screen is being captured; see the LINEOFF
    // trace in clockPixel(). 0 = off, >0 = that many lines still to print.
    long traceLineOffsetBudget = 0;
    // A RE-LOCK TAKEN DURING THE LINE NOW BEING SWEPT, in Pixel-M2. decide() snaps the
    // sweep onto a sync that landed outside the gentle pull-in range, but it runs
    // MONITOR_PHASE_WINDOW into the line that began at the flyback -- by which time that
    // line is already being drawn from the OLD phase. A real sweep retriggered by that
    // sync starts the line AT it, so this line moves by the snap too, not only the next.
    // The host adds it to the line in progress; cleared at every flyback.
    int lineSnap16 = 0;
    // THE LAST DECISION, for diagnostics: which branch decide() took, the phase it acted on
    // and how far it moved the sweep, with a sequence number so the host can tell a new one.
    // The host files it on the line being swept, so a drawn row can be tied to the decision
    // that placed it (CPCSE_TRACE_ROW) -- traces keyed to "the field after the shot" were
    // the wrong field on screens that alternate by frame.
    int decisionPhase = 0, decisionMove = 0, decisionTips = 0;
    char decisionBranch = '-';
    long decisionSeq = 0;
    int measuredPeriod = MONITOR_HSYNC_MID;   // interval between accepted line syncs
    int sinceLastSync = 0;          // ...as it is being counted
    int separator = 0;              // the vertical integrator
    bool vsyncSeparated = false;    // it has already fired for this pulse

    CtmMonitor() { reset(); }
    void reset();
    // Sample the C-SYNC pin for ONE PIXEL-M2 (0.0625 µsec), the rate the GATE ARRAY
    // clocks everything at. Active-low on the wire; `csyncActive` is true when the
    // signal is LOW. Returns true at the end of a horizontal sweep.
    //
    // It used to be sampled once per CRTC character, which cannot see a C-SYNC edge
    // that falls part-way through one -- and §14.4 says every edge below R3l=6 does.
    bool clockPixel(bool csyncActive);
    bool consumeVerticalFrameEdge();
    MonitorRendererState save() const;
    void restore(const MonitorRendererState& state);

private:
    static void addTip(int err, long long when, std::array<int, MONITOR_PHASE_TIPS>& at,
                       std::array<long long, MONITOR_PHASE_TIPS>& times, int& n);
    void decide();      // the deferred decision for the last flyback
    void hSync();       // a sync pulse the flywheel accepted as the line sync
    void vSync();       // the separator has picked a vertical sync out of the stream
};

} // namespace cpcse
