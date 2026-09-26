// CPCSyntaxError — ASIC 40226.
//
// ACCC §9 (p.44): "the ASIC 40226 is used on the low-cost CPC (CRTC 4)." It is not a
// separate GATE ARRAY sitting next to a CRTC -- the same silicon is the CRTC 4, which
// is why this model is chosen by the CRTC type rather than by the machine.
#include "gate_array_model.h"

namespace cpcse {
namespace {

struct GateArray40226 : GateArrayModel {
    const char* name() const override { return "40226"; }

    // ACCC §9.2.1 (p.48): the chapter names this ASIC alongside the three discrete
    // GATE ARRAY's -- "on gate arrays 40007, 40008, 40010 AND ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed one pixel earlier".
    int mode2PixelAdvance() const override { return 1; }

    // ACCC §9.3.4.3 (p.57) gives the shifted-in bit for the 40010 and the 40008 only.
    // Nothing is stated for this ASIC, so it keeps the commonest value rather than an
    // invented one; revisit if the chapter ever measures it.
    int modeSwitchFillBit() const override { return 0; }

    // ACCC §9 (p.46), §9.3.4.3 (p.59) and §14.5.4 (p.139) set the 40007/40008 apart from
    // the 40010 alone; nothing puts this ASIC with them, so it keeps the 40010's timing.
    int modeSwitchDecodeLead() const override { return 0; }
    int hsyncBlackEndLag() const override { return 0; }
};

const GateArray40226 instance;

}  // namespace

const GateArrayModel* gateArrayModel40226() { return &instance; }

}  // namespace cpcse
