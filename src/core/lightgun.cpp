// CPCSyntaxError — CPC lightgun geometry and protocol helpers.
#include "lightgun.h"

namespace cpcse {

static bool isFinite(double v) { return std::isfinite(v); }

bool westphaserBeamHit(double beamX, double beamY, double aimX, double aimY) {
    return beamX > aimX && beamY > aimY;
}

bool gunstickBeamAtAim(double beamX, double beamY, double aimX, double aimY, int characterWidth) {
    double bx = beamX, by = beamY, ax = aimX, ay = aimY;
    int width = std::max(1, characterWidth ? characterWidth : 16);
    if (!(isFinite(bx) && isFinite(by) && isFinite(ax) && isFinite(ay))) return false;
    double targetLine = std::floor(ay / 2) * 2;
    return by == targetLine && bx >= ax && bx < ax + width;
}


} // namespace cpcse
