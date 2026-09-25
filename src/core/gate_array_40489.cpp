// CPCSyntaxError — ASIC 40489.
//
// ACCC §9 (p.44): "the ASIC 40489 (CRTC 3) is used on CPC + and GX 4000." As with the
// 40226, the same silicon is the CRTC, so the CRTC type chooses this model.
#include "gate_array_model.h"

namespace cpcse {
namespace {

struct GateArray40489 : GateArrayModel {
    const char* name() const override { return "40489"; }

    // ACCC §9.2.1 (p.48): "ASIC 40489 of the CPC+ IS NOT AFFECTED by this discrepancy.
    // Whatever the graphic mode on this machine, the pixels are aligned." The only one
    // of the five that does not run mode 2 a Pixel-M2 early.
    int mode2PixelAdvance() const override { return 0; }

    // ACCC §9.3.4.3 (p.57) gives the shifted-in bit for the 40010 and the 40008 only;
    // nothing for this ASIC. It keeps the 40010's 0 rather than an invented value.
    int modeSwitchFillBit() const override { return 0; }
};

const GateArray40489 instance;

}  // namespace

const GateArrayModel* gateArrayModel40489() { return &instance; }

}  // namespace cpcse
