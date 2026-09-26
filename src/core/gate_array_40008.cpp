// CPCSyntaxError — GATE ARRAY 40008.
//
// ACCC §9 (p.44): pin compatible with the 40007 (version 2 of that part). Mostly found
// in 664's, alongside the 40010.
#include "gate_array_model.h"

namespace cpcse {
namespace {

struct GateArray40008 : GateArrayModel {
    const char* name() const override { return "40008"; }

    // ACCC §9.2.1 (p.48): "on gate arrays 40007, 40008, 40010 and ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed one pixel earlier than for other graphics modes."
    int mode2PixelAdvance() const override { return 1; }

    // ACCC §9.3.4.3 (p.57): the bits already consumed by the starting mode "are
    // considered to be 0 (GA 40010) or 1 (GA 40008)". This is the 40008, so 1 -- and
    // this is the one behaviour that separates it from the 40010 on screen.
    int modeSwitchFillBit() const override { return 1; }

    // ACCC §9 (p.46) / §9.3.4.3 (p.59): "ahead of 0.0625 usec compared to the 40010
    // when they recover the bits" -- one bit of the decoder at a switch out of MODE 2.
    int modeSwitchDecodeLead() const override { return 1; }

    // ACCC §14.5.4 (p.139/140): "The HSYNC is 1 pixel-M2 longer on the GA 40007 and
    // 40008 compared to the GA 40010."
    int hsyncBlackEndLag() const override { return 1; }
};

const GateArray40008 instance;

}  // namespace

const GateArrayModel* gateArrayModel40008() { return &instance; }

}  // namespace cpcse
