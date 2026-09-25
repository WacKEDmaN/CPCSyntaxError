// CPCSyntaxError -- the monitor SETS, one model each.
//
// The CPC was sold with more than one display and they are not interchangeable. ACCC
// §16.2.2 (p.163) names the silicon inside them:
//
//   "To my knowledge, the CTM 640/644 (COLOR SCREENS) contain a Sanyo LA7830 (or NEC
//    uPC1378H) component to manage the VERTICAL DEFLECTION, and a Sanyo LA7800 to
//    SEPARATE the horizontal and vertical synchronization signals. The GT 64/65
//    (MONOCHROME SCREENS) contain an LA1385 (or NEC uPC1031H2) to manage the vertical
//    deflection."
//
// So a green set is not a colour set with the colour turned down: it is a different
// vertical deflection part. And §15.1 (p.146) gives a difference that is visible on any
// screen, in whole microseconds:
//
//   "The GATE ARRAY is faster to manage HSYNC than to display characters read by CRTC's
//    0, 1 and 2... THE ASIC's (CRTC's 3 and 4) manage a HSYNC consistent with the C0
//    value displayed, DELAYING THE DISPLAY OF THE HSYNC BY 1 uSEC.
//    Because of this discrepancy, THE CALIBRATION OF A CM14 MONITOR (464+/6128+) IS
//    DIFFERENT FROM THAT OF A CTM 640/644 (464/664/6128). The CRTC 4 does not come with a
//    CM14, but it behaves like a CPC+ at the HSYNC signal. AMSTRAD CALIBRATED THE CTM
//    DELIVERED WITH THIS CPC so that the frame is centered on screen. CONNECTED TO THE
//    CTM 640/644 OF ANOTHER CPC (with a CRTC 0, 1, 2), THE IMAGE IS SHIFTED TO THE LEFT
//    BECAUSE HSYNC OCCURS 1 uSEC LATER...
//    Conversely, plugging a CRTC 0, 1, or 2 CPC into a CM14 monitor or the CTM of a
//    CRTC 4 should cause A SHIFT TO THE RIGHT of the image."
//
// That microsecond belongs to the PAIRING of machine and monitor, which is exactly what a
// monitor model is for: the ASIC's C-HSYNC leaves the GATE ARRAY a microsecond late (it
// is the GATE ARRAY that counts H06, §16.2.3), and the set it shipped with has a back
// porch a microsecond longer to absorb it. Put the two together and the picture is
// centred, which is why every reference photograph and every emulator screenshot of a
// Plus is centred. Pair them wrongly and §15.1's shift appears, in whichever direction
// the chapter says.
//
// Nothing here is invented. A field is added only when a chapter or a datasheet states
// it; where the compendium is silent the sets share a value and say so. This is a data
// table rather than a behaviour class because there is no per-set LOGIC -- the sets
// differ in constants, and the one part whose behaviour they do not share (the vertical
// deflection amplifier) is not characterised anywhere I can read.
#pragma once
#include "common.h"

namespace cpcse {

struct MonitorModel {
    const char* id;                  // what the .ini, the CLI and the GUI store
    const char* name;                // what the badge on the front says
    // What the tube is. One of monitor_palette.h's MONITOR_MODE_* values, so selecting a
    // set IS selecting the palette transform -- a GT 64 is green because it is a green
    // set, not because a tint box was ticked.
    const char* phosphor;
    // §15.1 (p.146): the horizontal calibration Amstrad set at the factory, in
    // MICROSECONDS, relative to a CTM 640/644 -- i.e. how much longer this set's back
    // porch is. The CM14 shipped with the 464+/6128+, and the CTM shipped with the
    // CRTC 4 machine, both absorb the ASIC's later HSYNC; every other set does not.
    int calibrationMicroseconds;
    // §16.2.2's parts. Recorded because the two families genuinely differ here and the
    // difference is not to be silently assumed away; the GUI shows them, and when the
    // vertical flywheel is given per-set constants this is where they will hang.
    const char* verticalDeflection;
    const char* syncSeparator;
    // ACCC §16.2.4 (p.166): "according to measurements made on SEVERAL CTMs, the duration
    // of the signal emitted for the monitor must be GREATER THAN 11-12 useconds. Below
    // this value, the monitor can no longer 'anchor' the image, regardless of the
    // adjustment made with the potentiometer on the back of the monitor." The chapter
    // measured CTMs; the green sets were not measured, so they carry the same figure
    // rather than a made-up one.
    int vsyncAnchorMinimumMicroseconds;
};

// The sets, by id. "ctm644" is the default and the reference calibration.
const MonitorModel* monitorModelFor(const std::string& id);
// Every set, for the GUI's list and the CLI's help.
const std::vector<const MonitorModel*>& monitorModels();
// What came in the box: the pairing that leaves §15.1's microsecond cancelled. A Plus or
// a GX4000 shipped with a CM14; a CRTC 4 machine shipped with a CTM calibrated like one;
// everything else with a CTM 640/644.
const MonitorModel* monitorModelShippedWith(bool plusHardware, int crtcType);

} // namespace cpcse
