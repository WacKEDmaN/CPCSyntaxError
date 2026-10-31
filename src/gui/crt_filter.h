// CPCSyntaxError — the advanced CRT filter: the picture as a PAL signal (its colour spread over
// its neighbours, scanlines, brightness/contrast/gamma), then the tube's glass (bulge,
// rounded corners, phosphor mask, bleed and glow, a tint, rolling lines). Two GPU stages,
// either of which can be off; each setting is an integer, as the settings file keeps it.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <string>

namespace cpcse {

struct CrtFilterSettings {
    bool enabled = false;
    bool signal = true;          // stage 1: the PAL decode and scanlines
    bool screen = true;          // stage 2: the tube's glass, mask and glow

    // stage 1, in thousandths (1000 = as it is)
    int blur = 100;              // luma spread to the pixels either side
    int scanlineShade = 800;     // the brightness of the line between two scanlines
    int oddLinesPhase = 1125;    // PAL: the odd lines' colour phase error (1000 = none)
    int oddLinesOffset = 1125;   // ...and their colour amplitude
    int saturation = 1000;
    int contrast = 1000;
    int brightness = 1000;
    int gamma = 2200;
    int hue = 1000;              // the colour's tint towards red (1000 = none)

    // stage 2
    int scale = 1;               // the mask and lines' size, in screen pixels
    int everyNLines = 4;         // dark lines: this many screen lines apart...
    int linesDarkened = 3;       // ...this many of them dark...
    int darkness = 10;           // ...by this much (%)
    bool columns = false;        // the lines and stripes run up and down
    int bulge = 8;               // %
    int corners = 17;            // rounded corners, % of the shorter side
    int cornerDot = 2;           // a lit dot in each dark corner, this big
    int bleed = 12;              // % of each pixel's light into its neighbours
    int glow = 5;                // % of bright light spreading further
    int mask = 3;                // the phosphor mask's strength, %
    int maskShape = 3;           // 0 stripes, 1 stripes with gaps, 2 slots, 3 dots
    int maskWidth = 3;           // one phosphor's width, in pattern pixels
    uint32_t tintColour = 0xffb000;   // RRGGBB
    int tint = 6;                // %
    int roll = -1;               // the dark lines move this far each frame
    int rollingBar = 0;          // a bright bar rolling down the screen, %

    // Keys "crtfilter..." after `prefix` ("" for the CPC's screen, "gfx9000." for the GFX9000's).
    void load(const std::map<std::string, std::string>& ini, const std::string& prefix = "");
    void save(std::ostream& out, const std::string& prefix = "") const;
};

// HOW ONE SCREEN IS SHOWN: its scaling, the CRT effect (a cheap pattern, bloom and blur over
// the picture) and the advanced CRT filter. The CPC's monitor and the GFX9000's each have one.
struct ScreenLook {
    bool integerScale = false;
    bool maintainAspect = true;
    bool crtEffect = false;      // the tube's pattern + bloom overlay
    float crtScanline = 0.5f;
    float crtBloom = 0.2f;
    float crtBlur = 0.0f;        // the beam's soft spot (softenRender): 0 sharp .. 1
    // THE TUBE'S PATTERN: "auto" (as the set: a colour tube has a slot mask (its phosphors
    // are stripes behind slots), and a green or grey tube shows scanlines), "slot", "soft",
    // "scanlines" or "none". crtMask is the mask's strength (0..1), crtMaskSize the width of
    // one phosphor stripe in screen pixels (the tube's pitch is not printed in the manuals).
    std::string crtPattern = "auto";
    float crtMask = 0.5f;
    int crtMaskSize = 1;
    bool crtMaskTurned = false;          // the mask turned 90 degrees: stripes across
    // The advanced CRT filter, drawn by the GPU. While it is on, the CRT effect stands aside.
    CrtFilterSettings crtFilter;
    bool crtFilterActive() const { return crtFilter.enabled && (crtFilter.signal || crtFilter.screen); }
    bool slotMask(bool colourTube) const { return crtPattern == "slot" || (crtPattern == "auto" && colourTube); }
    bool scanlines(bool colourTube) const { return crtPattern == "scanlines" || (crtPattern == "auto" && !colourTube); }
    // Keys after `prefix`: the CPC's screen keeps the names it always had ("" prefix).
    // load() returns false when the settings file holds none of this screen's keys.
    bool load(const std::map<std::string, std::string>& ini, const std::string& prefix);
    void save(std::ostream& out, const std::string& prefix) const;
};

// The settings with a slider, in the order they are shown: the settings file's key, the
// label, the field and its range. `signal` marks stage 1's.
struct CrtFilterSlider {
    const char* key;
    const char* label;
    int CrtFilterSettings::* field;
    int min, max;
    bool signal;
};
extern const CrtFilterSlider CRT_FILTER_SLIDERS[];
extern const int CRT_FILTER_SLIDER_COUNT;
extern const char* const CRT_FILTER_MASK_SHAPES[4];

// The GPU side. Made once there is a GL context; render() draws into a texture of its own.
class CrtFilterRenderer {
public:
    CrtFilterRenderer();
    ~CrtFilterRenderer();
    // The shading language's #version line the context takes ("#version 130"). False, with
    // the reason in `error`, when the context cannot run the filter.
    bool init(const char* glslVersion);
    // The picture in `source` (sourceW x sourceH, each CPC line `rowStep` rows) through the
    // filter, into a texture of outW x outH. pixelScale is the screen's pixels per point
    // (2 on a Retina display); newFrame moves the rolling lines and bar on. 0 on failure.
    unsigned render(unsigned source, int sourceW, int sourceH, int rowStep, int outW, int outH,
                    float pixelScale, bool newFrame, const CrtFilterSettings& settings);
    std::string error;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cpcse
