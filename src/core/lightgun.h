// CPCSyntaxError — CPC lightgun geometry and protocol helpers.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int LIGHTGUN_VIEW_WIDTH = 768;
inline constexpr int LIGHTGUN_VIEW_HEIGHT = 544;
inline constexpr int LIGHTGUN_CROP_LEFT = 15 * 16;

struct Rect { double left = 0, top = 0, width = 0, height = 0; };
struct Point { double x = 0, y = 0; };

// Returns a canvas point, or nullopt when there is none.
std::optional<Point> lightgunCanvasPoint(double clientX, double clientY, const Rect& rect,
    int width = LIGHTGUN_VIEW_WIDTH, int height = LIGHTGUN_VIEW_HEIGHT, double curvature = 0);

// registers: pointer to at least 18 CRTC register bytes.
int trojanLightPenAddress(const uint8_t* registers, double x, double y,
    int width = LIGHTGUN_VIEW_WIDTH, int height = LIGHTGUN_VIEW_HEIGHT);

bool westphaserBeamHit(double beamX, double beamY, double aimX, double aimY);
bool gunstickBeamAtAim(double beamX, double beamY, double aimX, double aimY, int characterWidth = 16);
bool gunstickSensorHit(bool triggered, bool bright);

} // namespace cpcse
