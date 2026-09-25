// CPCSyntaxError — CPC video renderer.
// Composes Gate Array pixels, borders, raster state and Plus sprites.
// Renders into a headless uint32 framebuffer (pixels).
#pragma once
#include "common.h"
#include "raster.h"

namespace cpcse {

class GXMemory; class PlusAsic; class CRTC6845; class GateArray; class CtmMonitor;
struct MonitorModel;

struct XY { int x = 0; int y = 0; };
struct DisplayBounds { int left, right, top, bottom; };

XY spriteCoordinates(const Bytes& asicRam, int base);
XY displayOrigin(const std::array<uint8_t, 18>& registers, int crtcType = 0);
XY classicMonitorLineOrigin(const std::array<uint8_t, 18>& registers, const std::array<uint8_t, 18>& frameRegisters, int crtcType = 3);
int classicMonitorReferenceR2(const std::vector<std::shared_ptr<RasterLine>>& rasterFrame, int fallback = 50);
XY classicPhysicalLineOrigin(const RasterLine* state, const std::array<uint8_t, 18>& registers, const std::array<uint8_t, 18>& frameRegisters);
DisplayBounds activeDisplayBounds(const std::array<uint8_t, 18>& registers);
int rasterHeightFromRegisters(const std::array<uint8_t, 18>& registers);
int screenAddressFromRegisters(const std::array<uint8_t, 18>& registers);
int rasterLineForMonitorY(int physicalY, int originY, int frameLength);
// lockedFieldLines: the period the monitor's vertical flywheel is running at, in lines.
// 0 leaves the origin wrapped by the frame's own length, which is what it did before the
// flywheel could be asked -- see the body for why that is wrong.
int physicalFrameOriginFromVsync(const std::vector<std::shared_ptr<RasterLine>>& rasterFrame, int fallback = 40, int lockedFieldLines = 0, bool anchorOnLockedField = false);
RasterLine* physicalFrameState(const std::vector<std::shared_ptr<RasterLine>>& currentFrame, const std::vector<std::shared_ptr<RasterLine>>& previousFrame, int sourceLine, int sourceStart);
int firstVisibleByte(int extendBorder);
bool frameHorizontalScrollActive(const std::vector<std::shared_ptr<RasterLine>>& rasterFrame, int fallback = 0);
bool rasterExtendBorderActive(const RasterLine* state, int extendBorder = 0);
int spriteClipLeft(int displayLeft, const RasterLine* state, int extendBorder = 0, bool frameScroll = false);

// A pen-source: a captured raster state / segment, or "live" (asic) when absent.
struct PenState {
    bool present = false;
    bool locked = false;
    std::vector<int> gaPalette;
    std::vector<int> palette;
    int mode = 0;
    int modeBefore = 0;
    int modeSwitchPixel = -1;   // ACCC §9.3.4's split byte; -1 = none
    int splitCharacter = -1;    // the character that split byte belongs to
    int horizontalScroll = 0;
};

class CpcVideo {
public:
    int width, height;
    std::vector<uint32_t> pixels;   // framebuffer (0xAABBGGRR-ish packed, see packedColor)
    GXMemory* memory;
    PlusAsic* asic;
    CRTC6845* crtc;
    // ACCC §9.1: "The GATE ARRAY/ASIC reads the data pointed to by the CRTC from
    // memory in order to convert and display it as pixels." The pen decoding below is
    // that chip's work, so which model is fitted has to be reachable from here.
    GateArray* gateArray = nullptr;
    // Where the picture sits vertically is the MONITOR's answer, not the register
    // file's: on the interlaced field its vertical sync arrives half a line late
    // (ACCC §19.3.1) and the whole field is drawn half a line lower. A framebuffer row
    // IS half a scanline here -- every line is painted as two rows -- so that half is
    // exactly one row, and it can only come from the set that recovered it.
    CtmMonitor* monitor = nullptr;
    int monitorHalfLine() const;
    // WHICH SET IS PLUGGED IN. It decides the tube's colour (a GT 65 is green because it
    // is a green set) and, by ACCC §15.1, the horizontal calibration: the CM14 and the
    // CRTC 4 machine's CTM have a back porch a microsecond longer than a CTM 640/644, to
    // absorb the microsecond the ASIC delays HSYNC by. See monitor_model.h.
    const MonitorModel* monitorSet = nullptr;
    // An explicit tube OVERRIDE, for looking at a colour program in green or in grey.
    // Empty means "whatever the set has", which is the only physical answer.
    std::string monitorTint;
    // §15.1's calibration, in Pixel-M2, for whoever places the picture behind the sync.
    int monitorCalibration16() const;
    // CPCSE_TRACE_ROW: in a harness that renders hundreds of times (the SHAKER runner),
    // print only the render the harness has armed for the screen it is dumping.
    bool traceRowsOnlyWhenArmed = false;
    bool traceRowsArmed = false;
    // The tube actually in use: the override if there is one, else the set's phosphor.
    std::string monitorMode;

    // transient render state
    XY origin;
    std::optional<XY> lineOrigin;
    std::optional<int> linePhysicalY;
    int lineHalfLine = 0;     // the monitor's half-line field offset, for the character pixels
    std::optional<int> lineClipRight;

    CpcVideo(int width, int height, GXMemory* memory, PlusAsic* asic, CRTC6845* crtc, GateArray* gateArray = nullptr);
    void setMonitorMode(const std::string& mode);
    void setMonitorModel(const MonitorModel* model);
    std::optional<bool> lightgunBrightnessAt(double x, double y);
    std::optional<bool> lightgunBrightnessAtRaster(double x, double y);
    bool lightgunRgbBright(int rgb);
    uint32_t packedColor(int rgb);
    int applyMonitor(int rgb);
    int penColor(int pen);
    int penColor(int pen, const PenState& state);
    void setPixel(int x, int y, int color, int width = 1);
    void drawScaledPixel(int x, int y, int color, int width);
    void drawScaledPixel(int x, int y, int color, int width, int horizontalScroll);
    void paintBorderScanline(int py, const RasterLine* state, const std::array<uint8_t, 18>& frameRegisters);
    void paintHsyncBlack(int py, const RasterLine* state, const std::array<uint8_t, 18>& frameRegisters);
    std::array<int, 2> mode0(int byte);
    std::array<int, 4> mode1(int byte);
    int pixelPen(int mode, int byte, int pixel);
    // ACCC §9.3.4 MODE SPLITTING. Decodes one video byte into its 8 Pixel-M2 pens,
    // honouring a graphic-mode change that lands mid-byte. switchPixel < 0 (or >= 8)
    // means no change, and the result is identical to pixelPen() for every pixel.
    void bytePens(int byte, int modeBefore, int modeAfter, int switchPixel, int pens[8]);
    int scrolledVideoByte(const RasterLine* state, int lineBase, int sourceColumn);
    int crtcAddress(int ma);
    GateArray& ga() const;        // the attached GATE ARRAY, or a default 40010
    int gaMode2Advance() const;   // ACCC §9.2.1, per GATE ARRAY model
    void render();

    // --- Beam-driven renderer (faithful CRT model, classic monitor path) ---
    // Instead of capturing per-line CRTC snapshots and reconstructing, plot the
    // Gate Array's 16 pixels for the current CRTC character at the live beam
    // position, exactly as the hardware paints the tube. beamFront is the
    // displayed frame; beamBack accumulates the frame in progress.
    bool beamMode = false;
    std::vector<uint32_t> beamFront, beamBack;
    int beamCol = 0, beamRow = 0;                 // beam position, driven by C-HSYNC/C-VSYNC
    // ACCC §9.3.4.3's "purple pixel" — the Pixel-M2 that comes back in the previous
    // graphic mode on CRTC 0, 2 and 4 but not on CRTC 1 — is not a mechanism of its
    // own here. It is the gap between where the HSYNC black stops
    // (hsyncBlackEndPixel: 4, 5, 4, 2) and where the GATE ARRAY switches mode
    // (modeSwitchPixelInByte: 5, 5, 5, 3), which is 1 Pixel-M2 on CRTC 0, 2 and 4 and
    // none at all on CRTC 1 — exactly what the chapter reports.
    // ACCC §14.3/§14.4: the monitor line starts at the END of the GATE ARRAY's
    // C-HSYNC. That is a flat 4 µsec while R3l >= 6, but below 6 it is the CRTC's own
    // end-of-HSYNC that stops it, so the picture moves right by 0.5 µsec (8 Pixel-M2)
    // for every unit R3l drops. Captured per line at the HSYNC.
    int beamLineShift = 0;
    // Previous character's HSYNC / V26-blanking state, so their sub-character start
    // and restore positions (ACCC §14.7.1, §16.2.1, §16.2.2) can be found on the edge.
    bool beamBlankLast = false;
    // ACCC §16.2.1: an R7.JIT can put the VSYNC black's start past this character's
    // 16 Pixel-M2; the remainder blacks the head of the next one.
    int beamBlankCarry = 0;
    // ACCC §9.3.1: the ASIC's of CRTC 3 and 4 see the CRTC's HSYNC 1 µsec late, so
    // the HSYNC pin is kept as a per-character shift register and read at the chip's
    // own delay. Bit 0 is this character, bit 1 the previous one, and so on.
    unsigned beamHsyncPipe = 0;
    // ACCC §9.2.2: the pen colours this character and the one before it were drawn
    // with, so a mode-2 pixel pushed into the previous character's slot can be
    // painted in the ink that was in force there. Rebuilt only on an ink write.
    uint32_t beamPens[17] = {0}, beamPensPrevious[17] = {0};
    int beamPenRevision = -1;
    bool beamPensValid = false, beamPensChanged = false;
    // ACCC §16.6 LIMITLESS VSYNC (p.173-175): "it is the starting position of the
    // VSYNC during the line that allows to raise the electron gun more or less high
    // (the later the VSYNC starts in the line, the higher the beam goes up)... if the
    // beam starts to rise from the middle of a line, it will rise higher by 1/2 pixel
    // than if the beam had gone up from position C0=0, and lower by 1/2 pixel than if
    // the beam had risen from position C0=63." One CPC line of range, so the CPC can
    // "manage fluid vertical scrolling at 1/64th of a pixel". In framebuffer rows
    // (2 per CPC line), applied to the whole frame from the C-VSYNC's C0.
    int beamFrameSubline = 0;
    // ACCC §15.7: "the monitor tries to set its image to the new position of C0=R2.
    // SINCE IT TAKES SEVERAL LINES TO ACHIEVE THIS, this results visually in a gradual
    // shift of the lines." So a changed sync position is not snapped to; the monitor
    // converges on it. The compendium gives no convergence RATE, only "several lines",
    // so the fraction used below is a modelling choice rather than a figure from the
    // document -- the one free parameter in this renderer.
    // §16.7 states the vertical counterpart, and states it as a property of the
    // MONITOR rather than of any chip: the image settles on a new C4=R7 "faster or
    // slower depending on the v-hold setting". There is no figure to implement and no
    // chip behaviour behind it, so the vertical origin is not converged -- the beam
    // path's VSYNC lock window and free-run failsafe are the flywheel §16.7 describes.
    int beamLineShiftShown = 0;
    void beamVsyncAtCharacter(int c0, int r0);    // C-VSYNC arrived at this C0
    void beamReset();
    void beamNewLine();                           // horizontal retrace: new line
    void beamHsync();                             // CRTC HSYNC pin (flywheel-gated)
    void beamVsync();                             // vertical retrace: present + home the beam
    void beamPresent();                           // present beamBack, clear, home beamRow
    void plotBeamCharacter();                     // plot current CRTC character, advance the beam

    struct SpriteState {
        const Bytes* spriteAttributes = nullptr;
        const std::vector<int>* spriteMagnification = nullptr;
        const Bytes* spritePatterns = nullptr;
        const std::vector<int>* palette = nullptr;
    };
    void drawSpriteChunk(const RasterLine* state, const SpriteState& spriteState, int displayY, int pixelY,
        const DisplayBounds& lineBounds, int spriteRasterLeft, int lineClipLeft, int chunkLeft, int chunkRight);
    void renderSprites(bool frameScroll = false);
};

} // namespace cpcse
