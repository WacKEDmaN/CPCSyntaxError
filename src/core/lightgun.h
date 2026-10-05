// CPCSyntaxError — CPC lightgun geometry and protocol helpers.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int LIGHTGUN_VIEW_WIDTH = 768;
inline constexpr int LIGHTGUN_VIEW_HEIGHT = 544;

bool westphaserBeamHit(double beamX, double beamY, double aimX, double aimY);
bool gunstickBeamAtAim(double beamX, double beamY, double aimX, double aimY, int characterWidth = 16);

} // namespace cpcse
