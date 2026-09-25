// CPCSyntaxError — CRTC type 1-B profile (UM6845R that takes RFD#10).
// Technical information sourced from the "Amstrad CPC CRTC Compendium" by Longshot
// (CC BY-NC-ND). See docs/reference/ACCC1.11-EN.pdf §2.2.
//
// ACCC §11.6 (p.88): "The value of R5 is of little importance, except on certain
// CRTC 1's for the value #10. This CRTC will be identified as 1-B while waiting to
// find out if the difference noticed really comes from the CRTC... CRTC 1-A is
// defined as CRTC 1 which does not support RFD#10."
//
// §11.6.2 (p.90) is blunt about how little else separates them: "The brand and model
// of CRTC 1-B does not differ from other CRTC 1's (UM6845R). This does not seem
// related to the batch number: the RFD#10 operates for example on a 6128
// UM6845R-8804T, but not on a 6128 with UM6845R-8802T. Out of 7 machines tested, 3
// had this additional capacity." So this profile is CRTC 1 with one flag flipped, and
// CRTC 1-A stays the default because a program cannot count on the capacity.
#include "crtc_type_1.h"

namespace cpcse {

struct CrtcType1B : CrtcType1 {
    CrtcType1B() { id = 5; name = "UM6845R (1-B)"; }
    // "On CRTC 1-B, value #10 in R5 deactivates parity management in test C9=R9,
    // while other values activate this parity management (including value #10 on
    // CRTC 1-A)." An RFD armed with R5=#10 therefore skips §11.6.1's case 1 — the
    // frame whose faulty C9=R9 test repeats the characters — and behaves as case 2
    // on every frame, without needing the "IVM ON/OFF" trick of §11.6.2 to fix the
    // parity first.
    bool supportsRfd10() const override { return true; }
};

const CrtcBehaviour& crtcType1b() { static CrtcType1B instance; return instance; }

} // namespace cpcse
