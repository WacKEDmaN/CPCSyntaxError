// CPCSyntaxError — what the TUBE does with the RGB the GATE ARRAY drives.
//
// This is the monitor's video stage and nothing else. It is handed the R, G and B the
// GATE ARRAY (or, on a Plus, the ASIC) is putting on pins 1, 2 and 3 of the DIN6 and it
// answers with what the screen shows. It must not look an ink up: which colour a pen is
// is the GATE ARRAY's question (GateArray::rgbForHardwareColour, ACCC §9.1) and it used
// to be answered TWICE, with a private 32-entry copy of that chip's table living here.
//
// A COLOUR SET (CTM 640/644, CM14) shows the RGB it is given: three guns, three signals,
// nothing to transform.
//
// A GREEN SET (GT 64/65) has ONE gun, and it is not fed by a TV luminance matrix. It is
// fed by the CPC's own LUM pin, which the machine builds itself: "These signals are mixed
// using resistors R131, R132 and R133, as well as R195, and amplified by transistor Q102.
// The resulting output signal is the LUM signal, which serves as a video signal for the
// green screen" (The Anatomy of the CPCs, p.92). The GT 65 takes it linearly -- "analogue
// voltage input which is linear between 0.8V (Black) and 1.75V (Peak white)" (Amstrad's
// own Arnold/Plus design spec, which gives the MM12 the same input).
//
// The weights of that resistor sum are fixed by a documented fact: the firmware colour
// number is 3*R + 9*G + 1*B, each gun 0/50/100% -> 0/1/2, and "on a green screen ... the
// colours (in the software/firmware colours) are in order of INCREASING INTENSITY"
// (CPCWiki, gate array). Every one of the 27 colours is a distinct step, in that order,
// only if green outweighs red and red outweighs blue in base 3 -- G:R:B = 9:3:1, which
// makes a colour's green-screen level simply its firmware number over 26.
//
// The TV matrix that stood here (0.299R + 0.587G + 0.114B) put 13 of the 351 colour
// pairs out of that order -- bright magenta (8) brighter than green (9) -- and drew
// firmware 7 and 12, and 20 and 21, as the same shade. Measured, not argued: see
// cpcse-ga-check's green-screen order case.
//
// The level is LINEAR in the gun voltages, so it is formed on the voltages, not on the
// displayed RGB this table holds. That RGB already carries the tube's own response -- the
// GATE ARRAY's 50% level is drawn as 0x63, not 0x80 -- so it is taken back out with the
// curve that 0x63 implies, the sum is formed, and the SAME curve is put back on the way to
// the phosphor. No new constant: the curve is whatever the colour table already says a
// CPC tube does with half a volt.
//
// That single curve replaces a second, separate green model: a 32-entry table of greens,
// inherited from an earlier version of this code, that was used for GATE ARRAY inks
// while the curve was used for everything else. The two disagreed by up to 72 of 255 per
// channel on the same tube (measured), the table was not monotonic in luminance, and it
// put BLACK at green 88 of 255 -- a tube whose gun is off is black.
//
// NOT SOURCED, and marked as such: the phosphor's own colour at full drive. No datasheet
// I have gives the GT's phosphor, so the endpoints below are the earlier ones and are kept
// rather than replaced with something equally invented. The SHAPE -- one luminance ramp
// on one chromaticity -- is the physical part.
#include "monitor_palette.h"
#include <cmath>

namespace cpcse {

const std::string MONITOR_MODE_COLOUR = "colour";
const std::string MONITOR_MODE_GREEN = "green";
const std::string MONITOR_MODE_MONO = "mono";

static std::string toLower(const std::string& s) { std::string r = s; for (auto& ch : r) ch = (char)std::tolower((unsigned char)ch); return r; }

std::string normaliseMonitorMode(const std::string& mode) {
    std::string value = toLower(mode);
    if (value == MONITOR_MODE_GREEN) return MONITOR_MODE_GREEN;
    if (value == MONITOR_MODE_MONO || value == "bw" || value == "grey" || value == "gray") return MONITOR_MODE_MONO;
    return MONITOR_MODE_COLOUR;
}

// The tube's response, as the GATE ARRAY's colour table implies it: a 50% drive is shown
// at 0x63/0xFF, so displayed = drive ^ gamma with gamma = ln(0x63/255) / ln(0.5).
static double tubeGamma() {
    static const double g = std::log(0x63 / 255.0) / std::log(0.5);
    return g;
}
// The CPC's LUM pin for an RGB the GATE ARRAY (or the Plus ASIC) is driving, as a displayed
// level 0..1 -- see the header comment: G:R:B = 9:3:1 on the linear gun drives.
static double cpcLumDisplayed(int rgb) {
    auto drive = [](int c) { return c <= 0 ? 0.0 : std::pow(c / 255.0, 1.0 / tubeGamma()); };
    const double r = drive((rgb >> 16) & 0xff), g = drive((rgb >> 8) & 0xff), b = drive(rgb & 0xff);
    const double lum = (9.0 * g + 3.0 * r + 1.0 * b) / 13.0;
    return std::pow(lum, tubeGamma());
}

// The luminance matrix of a television's video stage. Kept for callers that ARE a
// television (the ZX side); the CPC's own green and mono sets use cpcLumDisplayed.
int rgbToLuma(int rgb) {
    int r = (rgb >> 16) & 0xff;
    int g = (rgb >> 8) & 0xff;
    int b = rgb & 0xff;
    long v = std::lround(r * 0.299 + g * 0.587 + b * 0.114);
    return (int)std::max(0L, std::min(255L, v));
}

static int mix(int a, int b, double t) { return (int)std::lround(a + (b - a) * std::max(0.0, std::min(1.0, t))); }
static int pack(int r, int g, int b) { return (r << 16) | (g << 8) | b; }

int monitorTransformRgb(int rgb, const std::string& mode) {
    std::string m = normaliseMonitorMode(mode);
    if (m == MONITOR_MODE_COLOUR) return rgb & 0xffffff;
    double y = cpcLumDisplayed(rgb);
    if (m == MONITOR_MODE_GREEN) {
        // The one gun, driven by the luminance, shown in the phosphor's colour.
        return pack(mix(0x00, 0x6c, y), mix(0x10, 0xd7, y), mix(0x00, 0x61, y));
    }
    int v = (int)std::lround(y * 255);
    return pack(v, v, v);
}

int monitorLumLevel(int rgb) { return (int)std::lround(cpcLumDisplayed(rgb) * 255.0); }

// The ZX side's entry point. A Spectrum is not wired to the CPC's LUM network, so its
// green and mono transforms keep the television matrix they always had.
std::array<int, 3> monitorTransformTuple(const std::array<int, 3>& rgb, const std::string& mode) {
    const int packedIn = (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
    std::string m = normaliseMonitorMode(mode);
    int packed = packedIn & 0xffffff;
    if (m != MONITOR_MODE_COLOUR) {
        double y = rgbToLuma(packedIn) / 255.0;
        packed = m == MONITOR_MODE_GREEN
            ? pack(mix(0x00, 0x6c, y), mix(0x10, 0xd7, y), mix(0x00, 0x61, y))
            : pack((int)std::lround(y * 255), (int)std::lround(y * 255), (int)std::lround(y * 255));
    }
    return { (packed >> 16) & 0xff, (packed >> 8) & 0xff, packed & 0xff };
}

} // namespace cpcse
