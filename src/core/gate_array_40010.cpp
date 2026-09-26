// CPCSyntaxError — GATE ARRAY 40010.
//
// ACCC §9 (p.44): the later 464's and 664's, and most 6128's, which carry version 36AA
// (Matrix LSI HSG3130). The commonest of the five, and this emulator's default for a
// classic machine.
#include "gate_array_model.h"

namespace cpcse {
namespace {

struct GateArray40010 : GateArrayModel {
    const char* name() const override { return "40010"; }

    // ACCC §9.2.1 (p.48): "on gate arrays 40007, 40008, 40010 and ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed one pixel earlier than for other graphics modes."
    int mode2PixelAdvance() const override { return 1; }

    // ACCC §9.3.4.3 (p.57): the bits already consumed by the starting mode "are
    // considered to be 0 (GA 40010) or 1 (GA 40008)". This is the 40010, so 0.
    int modeSwitchFillBit() const override { return 0; }

    // ACCC §9 (p.46), §9.3.4.3 (p.59) and §14.5.4 (p.139) measure the 40007/40008
    // AGAINST this chip: it is the reference, so neither the decoder lead nor the longer
    // HSYNC black.
    int modeSwitchDecodeLead() const override { return 0; }
    int hsyncBlackEndLag() const override { return 0; }
};

const GateArray40010 instance;

}  // namespace

const GateArrayModel* gateArrayModel40010() { return &instance; }

}  // namespace cpcse
