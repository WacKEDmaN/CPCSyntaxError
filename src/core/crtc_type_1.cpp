// CPCSyntaxError — CRTC type 1 profile (UM6845R, "CRTC 1-A").
// The behaviour is in crtc_type_1.h, which CRTC 1-B (crtc_type_1b.cpp) also builds on.
// See docs/reference/ACCC1.11-EN.pdf §2.2.
#include "crtc_type_1.h"

namespace cpcse {

const CrtcBehaviour& crtcType1() { static CrtcType1 instance; return instance; }

} // namespace cpcse
