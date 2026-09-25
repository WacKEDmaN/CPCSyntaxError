// CPCSyntaxError — the GATE ARRAY models, one chip each.
//
// ACCC §9 (p.44): "On the CPC, there are 5 models of identified GATE ARRAY." They are
// not interchangeable, and the compendium documents where they differ in BEHAVIOUR, not
// just in packaging. Each model states its own numbers in its own file, duplicated
// rather than shared -- the same rule the CRTC profiles follow.
//
//   40007   Ferranti ULA20RA023 / ULA16RA023. Early 464's, and a rare 6128 (MC0057A
//           motherboards, mid-1988, under a heat sink).
//   40008   Pin compatible with the 40007. Mostly 664's.
//   40010   Later 464's and 664's, and most 6128's (version 36AA).
//   40226   ASIC of the low-cost CPC. This one IS the CRTC 4 as well.
//   40489   ASIC of the CPC+ and GX4000. This one IS the CRTC 3 as well.
//
// On a classic CPC the GATE ARRAY is a separate chip from the CRTC and which one is
// fitted is a property of the machine. On the two ASIC's it is the same silicon, so
// there the CRTC type decides the model.
#pragma once

namespace cpcse {

struct GateArrayModel {
    virtual ~GateArrayModel() = default;
    virtual const char* name() const = 0;

    // ACCC §9.2.1 (p.48): "on gate arrays 40007, 40008, 40010 and ASIC 40226 (CRTC 4),
    // pixels in mode 2 are displayed one pixel earlier than for other graphics modes.
    // They are displayed in 'advance' of 1/16 µsec (0.0625 µsec)... ASIC 40489 of the
    // CPC+ is not affected by this discrepancy. Whatever the graphic mode on this
    // machine, the pixels are aligned." In Pixel-M2.
    //
    // This is the GATE ARRAY's behaviour, and the chapter indexes it by GATE ARRAY
    // model -- it used to sit on the CRTC profile, where it only happened to give the
    // right answer because the two ASIC's are each one chip.
    virtual int mode2PixelAdvance() const = 0;

    // ACCC §9.3.4.3 (p.57): "in certain situations, when bits of the data in VRAM have
    // already been used in the calculation of a colour number of the pixels of the
    // starting mode, these bits are considered to be 0 (GA 40010) or 1 (GA 40008) for
    // the calculation of the new graphic mode colour numbers." The bit the GATE ARRAY
    // shifts into its decoder on a mid-byte mode change.
    virtual int modeSwitchFillBit() const = 0;
};

const GateArrayModel* gateArrayModel40007();
const GateArrayModel* gateArrayModel40008();
const GateArrayModel* gateArrayModel40010();
const GateArrayModel* gateArrayModel40226();
const GateArrayModel* gateArrayModel40489();

} // namespace cpcse
