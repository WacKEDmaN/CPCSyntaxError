// CPCSyntaxError — per-scanline raster capture records shared by the emulator (producer)
// and the video renderer (consumer). Mirrors the plain objects that
// GX4000.captureRasterState()/captureRasterCharacter() build.
#pragma once
#include "common.h"

namespace cpcse {

// A mid-line register/palette/sprite change captured at a character position.
struct RasterSegment {
    int character = 0;
    int mode = 0;
    // ACCC §9.3.4 (p.54): "Given that mode update occurs during the processing of the
    // byte read in ram, the algorithm for determining the PEN by the GA switches during
    // processing. Consequently, THE CALCULATED PENS ARE DISTORTED FOR THE REST OF THE
    // PIXELS TO BE DISPLAYED IN THE BYTE." So the character a mode change lands in is
    // decoded in BOTH modes -- the leading Pixel-M2 in the old one. Carrying only the new
    // mode, as this did, painted the whole byte in it and put the wrong pens on the first
    // pixels of the first character after a mid-line mode split.
    int modeBefore = 0;
    int modeSwitchPixel = -1;   // -1 = no split in this segment's first byte
    bool locked = false;
    std::vector<int> gaPalette;
    std::vector<int> palette;
    int horizontalScroll = 0;
    int verticalScroll = 0;
    int extendBorder = 0;
    Bytes spriteAttributes;
    std::vector<int> spriteMagnification;
    Bytes spritePatterns;
};

struct RasterLine {
    int mode = 0;
    bool locked = false;
    std::vector<int> gaPalette;
    std::vector<int> palette;
    int screenAddress = 0;
    int lineAddress = 0;
    int videoRaster = 0;
    int verticalCounter = 0;
    int rasterCounter = 0;
    int verticalAdjust = 0;
    int crtcType = 0;
    // ACCC §19.2: the SKEW-DISPTMG bits as THIS chip reads them (0 on CRTC 1 and 2,
    // which have no such function). Captured from the chip profile rather than
    // re-derived from crtcType in the renderer, so the behaviour stays with the chip.
    int displaySkewBits = 0;
    int interlaceVideo = 0;
    int interlaceField = 0;
    int crtcFrame = 0;
    int horizontalCounterAtMonitorLine = 0;
    int monitorLineOffset = 0;
    // Where the monitor's sweep stood, in Pixel-M2 within its character, at the CRTC
    // character boundary this line was captured on -- the sub-character phase between
    // the beam and the CRTC's grid. The video bytes are filed by the sweep's CHARACTER
    // counter, so without this a pull of less than a character cannot reach the screen at
    // all; the phase is only measurable where the two clocks meet, which is here.
    int sweepPhase16 = 0;
    // ACCC §16.2.3 (p.165): Pixel-M2 by which this chip's C-HSYNC rises BEFORE the
    // 2 usec boundary, which is the same amount by which the first visible pixel sits
    // later than a whole-character back porch would put it.
    int cHsyncRiseAdvance16 = 1;
    // ACCC 15.1 (p.146): the SET'S own back porch calibration, in Pixel-M2. A CM14, and
    // the CTM Amstrad calibrated for the CRTC 4 machine, sit a microsecond further behind
    // the sync than a CTM 640/644 to absorb the microsecond the ASIC delays HSYNC by.
    // It travels with the line for the same reason monitorLineOffset does: it is the
    // monitor's answer, not the register file's.
    int monitorCalibration16 = 0;
    // Diagnostics: the monitor decision taken during this line (CtmMonitor::decision*).
    char monitorDecisionBranch = ' ';
    int monitorDecisionPhase = 0, monitorDecisionMove = 0, monitorDecisionTips = 0;
    int monitorDecisionPull = 0, monitorDecisionWidth = 0, monitorDecisionSlow = 0;
    bool hsyncAtMonitorLine = false;
    bool vsync = false;
    // ACCC §16.2.1 (p.160): "When the GATE ARRAY receives the signal emitted by the CRTC
    // (when C4==R7), it sets its counter V26 to 0. THE BLACK COLOR WILL BE DISPLAYED FOR
    // 26 LINES." That black is the GATE ARRAY's, not the CRTC's: it does not care where
    // the display window is, and a VSYNC placed mid-screen puts a 26-line black band
    // across the picture. The beam renderer reads the live flag; the legacy compositor
    // rebuilds from these records and so needs it captured per line.
    bool gateArrayBlank = false;
    int charactersPerLine = 0;
    std::array<uint8_t, 18> crtcRegisters{};
    bool vDisplay = false;
    int displayStartCharacter = 0;
    int rasterHeight = 0;
    int horizontalScroll = 0;
    int verticalScroll = 0;
    int extendBorder = 0;
    int splitLine = 0;
    int splitAddress = 0;
    Bytes spriteAttributes;                 // 0x80 bytes
    std::vector<int> spriteMagnification;
    Bytes spritePatterns;
    Bytes videoBytes;                       // 0x200
    Bytes displayEnabled;                   // 0x100
    Bytes horizontalCounters;               // 0x100
    // ACCC §14.7.1: the GATE ARRAY's HSYNC black, per character, as a half-open range of
    // Pixel-M2 (16..16 = none). The same reason gateArrayBlank above is captured: the
    // beam renderer reads the live pins, the legacy compositor rebuilds from these
    // records and had nothing to rebuild this from.
    Bytes hsyncBlackFrom;                   // 0x100
    Bytes hsyncBlackTo;                     // 0x100
    // How many character slots this line actually produced. A ruptured line is a handful
    // of characters, not 64, and displayOrigin carries (R0-0x3f)*16 -- so on such a line
    // the origin swings hundreds of pixels and black taken from slots the line never
    // reached sweeps across the picture. SHAKER A2/A3 on CRTC 1 are exactly that.
    int capturedCharacters = 0;
    Bytes videoLookbehind;                  // 2
    int videoRevision = 0;
    int spriteRevision = 0;
    int spriteDataRevision = 0;
    std::vector<RasterSegment> segments;
};

} // namespace cpcse
