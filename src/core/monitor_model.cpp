// CPCSyntaxError -- the monitor sets. See the header for the two chapters this is built
// from; there is nothing here that they do not state.
#include "monitor_model.h"
#include "monitor_palette.h"

namespace cpcse {

// ACCC §16.2.2 (p.163) names these parts and no others. The green sets' sync separator
// is NOT named there -- only their vertical deflection amplifier is -- so it says so
// rather than assuming the colour set's part; behaviourally they share this build's
// horizontal model, because a TV front end is a TV front end and nothing measured says
// otherwise.
static const char* VERTICAL_COLOUR = "Sanyo LA7830 (or NEC uPC1378H)";
static const char* VERTICAL_GREEN = "Sanyo LA1385 (or NEC uPC1031H2)";
static const char* SEPARATOR_COLOUR = "Sanyo LA7800";
static const char* SEPARATOR_GREEN = "not named by ACCC 16.2.2";

// §16.2.4 (p.166) measured "several CTMs" and no green sets, so they carry the CTM's
// figure rather than an invented one.
static const int ANCHOR_USEC = 12;

static const MonitorModel MODELS[] = {
    // The reference calibration: the sets that came with a CRTC 0, 1 or 2.
    { "ctm644", "CTM 644 (colour)", "colour", 0,
      VERTICAL_COLOUR, SEPARATOR_COLOUR, ANCHOR_USEC },
    { "ctm640", "CTM 640 (colour)", "colour", 0,
      VERTICAL_COLOUR, SEPARATOR_COLOUR, ANCHOR_USEC },
    // §15.1: the 464+/6128+ set, calibrated a microsecond over so that the ASIC's later
    // HSYNC lands the frame centred.
    { "cm14", "CM14 (464+/6128+ colour)", "colour", 1,
      VERTICAL_COLOUR, SEPARATOR_COLOUR, ANCHOR_USEC },
    // §15.1: "The CRTC 4 does not come with a CM14, but it behaves like a CPC+ at the
    // HSYNC signal. AMSTRAD CALIBRATED THE CTM DELIVERED WITH THIS CPC so that the frame
    // is centered on screen." Same tube as a CTM 644, same microsecond as a CM14.
    { "ctm644-crtc4", "CTM 644 (CRTC 4 calibration)", "colour", 1,
      VERTICAL_COLOUR, SEPARATOR_COLOUR, ANCHOR_USEC },
    // §16.2.2's monochrome sets. A different vertical deflection part, and no colour
    // circuitry at all -- the DIN's R, G and B arrive at one gun.
    { "gt65", "GT 65 (green)", "green", 0,
      VERTICAL_GREEN, SEPARATOR_GREEN, ANCHOR_USEC },
    { "gt64", "GT 64 (green)", "green", 0,
      VERTICAL_GREEN, SEPARATOR_GREEN, ANCHOR_USEC },
    // Amstrad's Arnold (464 Plus / 6128 Plus) design spec: "The MM12 monochrome incorporates
    // a 12" paper white tube, similar to that used on the PCW9512. The input will be the
    // same as the earlier GTM65 versions" -- the LUM pin, linear 0.8V black to 1.75V peak
    // white. Paper white, so the grey tube. Its deflection parts are not named anywhere I
    // have, and neither is a 15.1 calibration: it is left at a CTM's, which would put a
    // Plus a character left on it if Amstrad did not calibrate it like the CM14.
    { "mm12", "MM12 (paper white)", "mono", 0,
      "not named", "not named", ANCHOR_USEC },
};

const std::vector<const MonitorModel*>& monitorModels() {
    static std::vector<const MonitorModel*> all;
    if (all.empty())
        for (const MonitorModel& m : MODELS) all.push_back(&m);
    return all;
}

const MonitorModel* monitorModelFor(const std::string& id) {
    std::string want = id;
    for (auto& ch : want) ch = (char)std::tolower((unsigned char)ch);
    for (const MonitorModel* m : monitorModels()) if (want == m->id) return m;
    // A tube name on its own is a fair thing to ask for, and is what the older setting
    // stored: "green" is a GT 65, "colour" the default set.
    if (want == MONITOR_MODE_GREEN) return monitorModelFor("gt65");
    return &MODELS[0];
}

const MonitorModel* monitorModelShippedWith(bool plusHardware, int crtcType) {
    // §15.1 pairs the microsecond with the MACHINE, not with the tube: a Plus got a CM14,
    // and the CRTC 4 machine got a CTM that Amstrad had calibrated the same way.
    if (crtcType == 4) return monitorModelFor("ctm644-crtc4");
    if (plusHardware || crtcType == 3) return monitorModelFor("cm14");
    return monitorModelFor("ctm644");
}

} // namespace cpcse
