// CPCSyntaxError — CPC lightgun geometry and protocol helpers.
#include "lightgun.h"

namespace cpcse {

static bool isFinite(double v) { return std::isfinite(v); }

std::optional<Point> lightgunCanvasPoint(double clientX, double clientY, const Rect& rect,
    int width, int height, double curvature) {
    if (!(rect.width > 0) || !(rect.height > 0) || !(width > 0) || !(height > 0)) return std::nullopt;
    double scale = std::min(rect.width / width, rect.height / height);
    if (!(scale > 0)) return std::nullopt;
    double contentWidth = width * scale, contentHeight = height * scale;
    double offsetX = (rect.width - contentWidth) / 2;
    double offsetY = (rect.height - contentHeight) / 2;
    double x = (clientX - rect.left - offsetX) / scale;
    double y = (clientY - rect.top - offsetY) / scale;
    if (x < 0 || x >= width || y < 0 || y >= height) return std::nullopt;
    double amount = std::max(0.0, isFinite(curvature) ? curvature : 0.0);
    if (amount > 0) {
        double nx = x / width * 2 - 1;
        double ny = y / height * 2 - 1;
        double factor = 1 + (nx * nx + ny * ny) * amount;
        x = (nx * factor * 0.5 + 0.5) * width;
        y = (ny * factor * 0.5 + 0.5) * height;
        if (x < 0 || x >= width || y < 0 || y >= height) return std::nullopt;
    }
    return Point{ x, y };
}

int trojanLightPenAddress(const uint8_t* r, double x, double y, int width, int height) {
    int xCell = (int)std::trunc((x - width / 2.0 + 8) / 16);
    int yCell = (int)std::trunc((y - height / 2.0 + 8) / 16);
    int start = ((r[12]) << 8) | (r[13]);
    int raw = start + xCell - (r[2]) + 65
        + (yCell - (r[7]) + 40) * (r[1]);
    return (((r[12]) & 0x30) << 8) | (raw & 0x03ff);
}

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

bool gunstickSensorHit(bool triggered, bool bright) { return triggered && bright; }

} // namespace cpcse
