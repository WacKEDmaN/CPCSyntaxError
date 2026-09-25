// CPCSyntaxError — CRTC type 3 profile (ASIC 40489, CPC+ / GX4000).
// The behaviour itself is in crtc_type_3.h, which CRTC 4 (ASIC 40226) also builds
// on. See docs/reference/ACCC1.11-EN.pdf §2.2.
#include "crtc_type_3.h"

namespace cpcse {

const CrtcBehaviour& crtcType3() { static CrtcType3 instance; return instance; }

} // namespace cpcse
