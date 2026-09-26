// CPCSyntaxError — GATE ARRAY 40007.
//
// ACCC §9 (p.44): "40007 models (version 1: Ferranti Matrix ULA20RA023, version 2:
// Ferranti Matrix ULA16RA023) and 40008 (version 2) are pin compatible." Early 464's,
// and a rare 6128 -- the MC0057A motherboards of mid-1988, where it sits under a heat
// sink. On the board it is often the one with a flat piece of metal pasted to its top.
#include "gate_array_model.h"

namespace cpcse {
namespace {

struct GateArray40007 : GateArrayModel {
    const char* name() const override { return "40007"; }

    // ACCC §9.2.1 (p.48): "on gate arrays 40007, 40008, 40010 and ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed one pixel earlier than for other graphics modes."
    int mode2PixelAdvance() const override { return 1; }

    // ACCC §9.3.4.3 (p.57) names only the 40010 and the 40008 for the shifted-in bit.
    // The 40007 is the 40008's pin-compatible sibling (the chapter pairs them in the
    // same sentence), so it takes the 40008's 1 rather than the 40010's 0. Stated here
    // as this chip's own value, not inherited.
    int modeSwitchFillBit() const override { return 1; }

    // ACCC §9 (p.46) / §9.3.4.3 (p.59): "ahead of 0.0625 usec compared to the 40010
    // when they recover the bits" -- one bit of the decoder at a switch out of MODE 2.
    int modeSwitchDecodeLead() const override { return 1; }

    // ACCC §14.5.4 (p.139/140): "The HSYNC is 1 pixel-M2 longer on the GA 40007 and
    // 40008 compared to the GA 40010."
    int hsyncBlackEndLag() const override { return 1; }
};

const GateArray40007 instance;

}  // namespace

const GateArrayModel* gateArrayModel40007() { return &instance; }

}  // namespace cpcse
