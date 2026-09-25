// CPCSyntaxError — CPC video renderer.
#include "video.h"
#include <cstdio>
#include <cstdlib>
#include "memory.h"
#include "asic.h"
#include "crtc.h"
#include "monitor_model.h"
#include "monitor_palette.h"
#include "monitor_renderer.h"
#include "gate_array.h"

namespace cpcse {

static const std::vector<std::shared_ptr<RasterLine>> EMPTY_FRAME;

XY spriteCoordinates(const Bytes& asicRam, int base) {
    int x = asicRam[base] | (asicRam[base + 1] & 3) << 8;
    int y = asicRam[base + 2] | (asicRam[base + 3] & 1) << 8;
    if ((x & 0x300) == 0x300) x -= 0x400;
    if (y & 0x100) y -= 0x200;
    return { x, y };
}
XY displayOrigin(const std::array<uint8_t, 18>& registers, int crtcType) {
    (void)crtcType;
    int x = (0x32 - registers[2]) * 16;
    int syncWidth = registers[3] & 0x0f;
    if (syncWidth < 6) x += syncWidth * 8;
    x += (registers[0] - 0x3f) * 16;
    // DISPTMG display-skew no longer shifts the origin: it is modelled at the
    // DISPEN pin (crtc.cpp) so the first `skew` chars of each line show border.
    int y = (registers[4] - registers[7] - 3) * 8;
    if (y & 0x100) y = (y | ~0x1ff);
    return { x, y };
}
XY classicMonitorLineOrigin(const std::array<uint8_t, 18>& registers, const std::array<uint8_t, 18>& frameRegisters, int crtcType) {
    XY origin = displayOrigin(registers, crtcType);
    if (crtcType != 0 && crtcType != 1) return origin;
    int r2Delta = (registers[2] & 0xff) - (frameRegisters[2] & 0xff);
    return { origin.x + r2Delta * 14, origin.y };
}
int classicMonitorReferenceR2(const std::vector<std::shared_ptr<RasterLine>>& rasterFrame, int fallback) {
    std::vector<int> values;
    for (const auto& state : rasterFrame) if (state) values.push_back(state->crtcRegisters[2] & 0xff);
    if (values.empty()) return fallback & 0xff;
    std::sort(values.begin(), values.end());
    return values[(unsigned)values.size() >> 1];
}
XY classicPhysicalLineOrigin(const RasterLine* state, const std::array<uint8_t, 18>& registers, const std::array<uint8_t, 18>& frameRegisters) {
    XY registerOrigin = classicMonitorLineOrigin(registers, frameRegisters, state ? state->crtcType : 3);
    if (state) {
        // ACCC §16.2.3 (p.165): the monitor's horizontal reference is the C-HSYNC edge,
        // and that edge is "1 or 2 Pixel-M2 BEFORE the end of the 2 us" -- off the
        // character grid, by an amount that "depend[s] on the type of CRTC". The GATE
        // ARRAY's pixels stay on the grid, so the picture sits that much further behind
        // the sync than a whole-character back porch says, on every line of every
        // screen. Measured against AmSpiriT before this: 138 of 166 scored CRTC 1
        // screens aligned best at exactly -1 pixel, and module C's mode-2 patterns --
        // one pixel wide, so a pixel of offset destroys the correlation -- scored 67
        // where a one-pixel shift scored 4.8.
        // ...and the sweep's own sub-character phase against the CRTC's grid. The video
        // bytes are filed by the sweep's CHARACTER counter, so this is the only way a pull
        // of less than a character reaches the screen -- and lineOffset's `hsyncCount & 15`
        // never carried it, being read at the flyback where hsyncCount is always 1056. A
        // normally locked line sits at phase 0 and does not move; SHAKER D3's doubled lines
        // sit at 12, where the reference moves them 13.
        // ...and ACCC 15.1's per-SET calibration (p.146). The back porch is the set's,
        // not the chip's: a CM14 and the CTM calibrated for the CRTC 4 machine hold the
        // picture a microsecond longer to absorb the microsecond the ASIC delays the
        // whole HSYNC by, which is what centres a Plus on its own monitor.
        return { state->monitorLineOffset + state->sweepPhase16
                     + 16 - MONITOR_CROP_LEFT - state->cHsyncRiseAdvance16
                     + state->monitorCalibration16,
                 registerOrigin.y };
    }
    return registerOrigin;
}
DisplayBounds activeDisplayBounds(const std::array<uint8_t, 18>& registers) {
    XY origin = displayOrigin(registers);
    bool enabled = ((unsigned)registers[8] >> 4 & 3) != 3;
    int maximumRaster = (registers[8] & 3) == 3 ? ((unsigned)(registers[9] & 0x1f) >> 1) : registers[9] & 0x1f;
    return {
        origin.x,
        origin.x + (enabled ? registers[1] * 16 : 0),
        origin.y * 2,
        (origin.y + registers[6] * (maximumRaster + 1)) * 2
    };
}
int rasterHeightFromRegisters(const std::array<uint8_t, 18>& registers) {
    int maximumRaster = (registers[8] & 3) == 3 ? ((unsigned)(registers[9] & 0x1f) >> 1) : registers[9] & 0x1f;
    return maximumRaster + 1;
}
int screenAddressFromRegisters(const std::array<uint8_t, 18>& registers) {
    return (((registers[12] & 0x3f) << 8) | registers[13]) & 0x3fff;
}
int rasterLineForMonitorY(int physicalY, int originY, int frameLength) {
    int line = physicalY - originY;
    if (frameLength <= 0) return line;
    return ((line % frameLength) + frameLength) % frameLength;
}
int physicalFrameOriginFromVsync(const std::vector<std::shared_ptr<RasterLine>>& rasterFrame, int fallback, int lockedFieldLines, bool anchorOnLockedField) {
    int length = (int)rasterFrame.size();
    if (!length) return fallback;
    // The Plus rupture picture (see CpcVideo::render) is closed at the CRTC wrap AFTER its
    // VSYNC, so the sync sits near the END of the buffer and the buffer's length is
    // whatever the program's sub-frames happened to add up to. Alcon 2020 scrolls by
    // varying them: 306, 312, 314 lines, with the display always ending 25 lines before
    // the sync. Reducing that late sync by the buffer length made the picture jump by the
    // length's change every few frames. A tube anchors on the sync: reduce by the field
    // the deflection runs at instead, which is the same thing on a 312-line picture.
    const int wrapLength = anchorOnLockedField && lockedFieldLines > 0 ? lockedFieldLines : length;
    // The capture buffer begins at the monitor's vertical frame edge, and that edge
    // is fired by the one VSYNC the flywheel locked onto (ACCC 16.1). A frame may
    // carry several VSYNCs -- a rupture's, or a second C4==R7 -- so the locked pulse
    // is not simply the first one in the buffer: it is the one AT the frame edge.
    // Pick the start nearest index 0 in either direction; a pulse that began a line
    // or two before the edge is a small negative offset, not a line-310 origin.
    int vsyncStart = 0;
    int nearest = length;
    for (int line = 0; line < length; line += 1) {
        if (!rasterFrame[line] || !rasterFrame[line]->vsync) continue;
        auto& previous = rasterFrame[(line + length - 1) % length];
        if (previous && previous->vsync) continue;
        int offset = line * 2 <= length ? line : line - wrapLength;
        if (std::abs(offset) < nearest) { nearest = std::abs(offset); vsyncStart = offset; }
    }
    if (nearest >= length) return fallback;
    int normalMonitorVsync = 280;
    int origin = normalMonitorVsync - vsyncStart;
    // ...and now the length this was wrapped by, which is the whole of the trouble.
    //
    // The wrap made the picture's position depend on HOW MANY LINES THIS FIELD RAN FOR.
    // Pinning the sync at row 280 and then reducing modulo `length` puts the content
    // (which sits a fixed number of lines after the frame edge) at row 280 - (length - c):
    // a field one line short drops the whole picture one scanline. A tube does the
    // opposite. The sweep was fired by the sync BEFORE this buffer began, the content
    // follows that sync by a distance the CRTC fixes (R4-R7 rows of R9+1 lines), and a
    // field that ends a line early shortens the blanking, not the picture.
    //
    // Measured on SHAKER module C / CRTC 1: the C2 screens are captured on frames the
    // CRTC runs for 311 lines where R4=38 R9=7 R5=0 gives 312, and every one of them
    // came out exactly one scanline low (dy=+2 image rows) against the reference while
    // its 312-line partner was exact.
    //
    // Reducing by the flywheel's own period instead is a NO-OP whenever the field is the
    // length the deflection is running at -- 282 and 282-312 are congruent modulo a
    // 312-line field -- so nothing moves on an ordinary frame, of any geometry. Only a
    // field that deviates from the locked period moves, and it moves by the deviation,
    // which is what a deflection with a flywheel in it does (ACCC §16.2.4: the monitor
    // "anchors" the image and the potentiometer only sets how far it can be pulled).
    if (lockedFieldLines > 0) origin -= lockedFieldLines;
    return origin;
}
RasterLine* physicalFrameState(const std::vector<std::shared_ptr<RasterLine>>& currentFrame, const std::vector<std::shared_ptr<RasterLine>>& previousFrame, int sourceLine, int sourceStart) {
    bool usePrevious = sourceStart > 0 && sourceLine >= sourceStart && !previousFrame.empty();
    auto at = [](const std::vector<std::shared_ptr<RasterLine>>& f, int i) -> RasterLine* { return (i >= 0 && i < (int)f.size() && f[i]) ? f[i].get() : nullptr; };
    RasterLine* r = usePrevious ? at(previousFrame, sourceLine) : at(currentFrame, sourceLine);
    if (r) return r;
    r = at(currentFrame, sourceLine); if (r) return r;
    return at(previousFrame, sourceLine);
}
int firstVisibleByte(int extendBorder) { return extendBorder ? 2 : 0; }
bool frameHorizontalScrollActive(const std::vector<std::shared_ptr<RasterLine>>& rasterFrame, int fallback) {
    if (fallback) return true;
    for (const auto& state : rasterFrame) {
        if (!state) continue;
        if (state->horizontalScroll) return true;
        for (const auto& segment : state->segments) if (segment.horizontalScroll) return true;
    }
    return false;
}
bool rasterExtendBorderActive(const RasterLine* state, int extendBorder) {
    if (extendBorder) return true;
    if (!state) return false;
    for (const auto& segment : state->segments) if (segment.character <= 1 && segment.extendBorder) return true;
    return false;
}
int spriteClipLeft(int displayLeft, const RasterLine* state, int extendBorder, bool frameScroll) {
    // legacy frame-scroll clip only applied to bare state objects; captured
    // RasterLine records always carry crtcRegisters, so it never applies here.
    (void)frameScroll;
    return displayLeft + (rasterExtendBorderActive(state, extendBorder) ? 16 : 0);
}

// The two GATE ARRAY behaviours the compendium indexes by GATE ARRAY MODEL, asked of
// the chip rather than re-derived here. They fall back to the commonest part (the
// 40010) when no GATE ARRAY is attached, which is only the case in isolated tests.
int CpcVideo::gaMode2Advance() const {
    return gateArray && gateArray->model ? gateArray->model->mode2PixelAdvance()
                                         : gateArrayModel40010()->mode2PixelAdvance();
}

static PenState penFromLine(const RasterLine* s) {
    PenState p; if (!s) return p;
    p.present = true; p.locked = s->locked; p.gaPalette = s->gaPalette; p.palette = s->palette; p.mode = s->mode; p.horizontalScroll = s->horizontalScroll; return p;
}
static PenState penFromSegment(const RasterSegment& s) {
    PenState p; p.present = true; p.locked = s.locked; p.gaPalette = s.gaPalette; p.palette = s.palette; p.mode = s.mode; p.modeBefore = s.modeBefore; p.modeSwitchPixel = s.modeSwitchPixel; p.splitCharacter = s.character; p.horizontalScroll = s.horizontalScroll; return p;
}

CpcVideo::CpcVideo(int width, int height, GXMemory* memory, PlusAsic* asic, CRTC6845* crtc, GateArray* gateArray)
    : width(width), height(height), memory(memory), asic(asic), crtc(crtc), gateArray(gateArray),
      monitorMode(MONITOR_MODE_COLOUR) {
    pixels.assign((size_t)width * height, 0);
}
// Beam position offsets: place the CRTC-line/HSYNC-relative sweep into the
// visible 768x544 window. Calibrated so the standard display centres as the
// legacy renderer/monitor crop do.
// The sweep is homed on the GATE ARRAY's C-HSYNC, which ACCC §16.2.3's H06 counter
// raises 2 characters after the CRTC's HSYNC -- so this back porch is 32 pixels
// shorter than when the beam was (wrongly) homed on the CRTC pin, exactly as
// MONITOR_CROP_LEFT is 2 characters shorter on the legacy path.
// The sweep now starts at the END of that 4 µsec pulse, so the back porch is 4 more
// characters shorter again.
// ...plus ACCC 15.1's per-set calibration, added where the sweep is placed (see
// monitorCalibration16): it belongs to the monitor, so it cannot be a constant here.
static const int BEAM_X_OFFSET = -128;  // pixels; beamX = beamCol*16 + this
// The GATE ARRAY's V26 counter delays C-SYNC to the monitor by two lines after the
// CRTC's VSYNC (ACCC §16.1). These origin constants were calibrated against the
// undelayed sync, so they carry that delay back out — otherwise it is counted twice
// and the whole picture sits two lines high.
static const int BEAM_Y_OFFSET = -66 + 4;   // pixels; beamY = beamRow*2 + this
void CpcVideo::beamReset() {
    beamFront.assign((size_t)width * height, 0);
    beamBack.assign((size_t)width * height, 0);
    beamCol = beamRow = 0;
}
// Vertical flywheel: a real monitor free-runs at ~50 Hz (~312 lines) and only
// locks to VSYNC near the expected frame end. Under an R0/R7 rupture the CRTC
// fires VSYNC erratically; accept it only inside a lock window, and free-run
// (present anyway) if none arrives — so the frame stays a stable full height.
static const int BEAM_LINE_MIN = 62;         // characters; ignore HSYNC before this
static const int BEAM_LINE_MAX = 66;         // ...and retrace by here regardless
static const int BEAM_VSYNC_LOCK = 260;      // accept VSYNC only past this row
static const int BEAM_FRAME_MAX = 340;       // free-run failsafe if no VSYNC
void CpcVideo::beamPresent() {
    if (beamBack.empty()) beamReset();
    beamFront = beamBack;
    pixels = beamBack;
    std::fill(beamBack.begin(), beamBack.end(), packedColor(applyMonitor(0)));
    beamRow = 0;
}
void CpcVideo::beamNewLine() {                // horizontal retrace: home the beam
    beamRow += 1;
    beamCol = 0;
    if (beamRow >= BEAM_FRAME_MAX) beamPresent();   // flywheel free-run
}
void CpcVideo::beamHsync() {                  // GATE ARRAY C-HSYNC: begin a monitor line
    // A monitor's horizontal oscillator is a FLYWHEEL: it free-runs at ~64 usec and
    // ignores sync pulses that arrive well before the line period is up. It only
    // resyncs inside a window around its natural period, which is exactly what
    // ClassicMonitorRenderer::clockCharacter already does for the legacy path
    // (MONITOR_HSYNC_MIN..MAX = 62..66 characters).
    //
    // The beam path had no such window and took EVERY CRTC HSYNC as a new line.
    // Under an R0 rupture the CRTC emits one every few usec — Shaker A3 runs R0=3,
    // so 16 CRTC lines belong to a single 64 usec monitor line — and each one both
    // advanced beamRow and homed beamCol, stacking a whole line's worth of picture
    // into a dozen rows at the left edge. Measured on A3/I1: the CRTC enables
    // display on 111 monitor lines per frame (the oracle shows 113) and the capture
    // keeps every one of them, yet only 84 reached the beam framebuffer.
    if (beamCol < BEAM_LINE_MIN) return;         // too soon: not a line boundary
    // CPCSE_TRACE_BEAMLINE=1 prints, for every monitor line the flywheel ACCEPTS, how
    // long that line actually ran and where the sweep is standing. "Measure twice, code
    // once": three changes to this loop were reverted for want of exactly these numbers.
    // col is the line length in characters (64 is nominal), shown is §15.7's gradual
    // shift as the renderer currently applies it.
    if (std::getenv("CPCSE_TRACE_BEAMLINE")) {
        static long budget = -1, skip = 0;
        if (budget == -1) {
            const char* cap = std::getenv("CPCSE_TRACE_BEAMLINE_MAX");
            budget = cap ? std::atol(cap) : 400;
            // ...and SKIP that many accepted lines first, because the screen worth
            // looking at is 119 seconds into a SHAKER module, about 1.86M lines in.
            const char* from = std::getenv("CPCSE_TRACE_BEAMLINE_SKIP");
            skip = from ? std::atol(from) : 0;
        }
        if (skip > 0) skip -= 1;
        else if (budget > 0) {
            std::fprintf(stderr, "BEAMLINE row=%3d col=%3d shift=%4d shown=%4d\n",
                         beamRow, beamCol, beamLineShift, beamLineShiftShown);
            budget -= 1;
        }
    }
    // ACCC §14.4: "When R3l drops, excluding R3.JIT, the image is shifted to the right
    // of half a unit (0.5 µsec if R3l drops by 1) whatever the value of R2." Taken off
    // the pin, not off R3l: §16.2.2 caps C-HSYNC at 4 µsec and emits none at all below
    // R3l=2, so every microsecond the GATE ARRAY's pulse falls short of 4 is 8 units of
    // that shift, and a programmed width too short to raise a pulse leaves the sweep
    // free-running with nothing to correct against -- where reading R3l invented a
    // 40-unit shift for a sync that never reached the monitor.
    // No width correction at all any more. §14.4's "the image is shifted to the right
    // of half a unit (0.5 µsec if R3l drops by 1)" is not a rule the renderer has to
    // apply: it is what FALLS OUT of homing the sweep where C-HSYNC ends, because a
    // pulse the CRTC cut short ends earlier. Reading R3l here was the renderer
    // re-deriving, from a register, a consequence of a pin it can simply follow.
    beamLineShift = 0;
    // ACCC §15.1's one-character ASIC delay used to be subtracted here, moving the
    // CRTC 3/4 picture a character left. It is gone: §9.3.1's delay is on where the
    // GATE ARRAY puts the BLACK, and that is already modelled at the black-zone pins
    // (hsyncBlackStartPixel, and the HSYNC pipe below). AmSpiriT's per-chip references
    // carry no such shift, and putting it in the C-HSYNC instead cost +1268 SHAKER
    // error on CRTC 3 -- the double-count this comment used to warn about.
    // ACCC §15.7: a monitor does not jump to a new horizontal sync position -- "since
    // it takes several lines to achieve this, this results visually in a GRADUAL SHIFT
    // of the lines". Converge a quarter of the way per line, which lands within the
    // "several lines" the chapter describes. (The rate itself is not in the document.)
    beamLineShiftShown += (beamLineShift - beamLineShiftShown) / 4;
    if (beamLineShiftShown != beamLineShift
        && (beamLineShift - beamLineShiftShown) * (beamLineShift - beamLineShiftShown) <= 4)
        beamLineShiftShown = beamLineShift;      // settle the last pixel or two
    beamNewLine();
}
// ACCC §16.6: the beam rises higher the later in the line the C-VSYNC arrives, over
// a range of one CPC pixel across the line. Our rows are half a CPC line each, so a
// full line of range is 2 rows; the residue below that is the 1/64th-pixel precision
// the chapter describes, which this raster cannot show.
void CpcVideo::beamVsyncAtCharacter(int c0, int r0) {
    int line = r0 + 1;
    if (line <= 0) { beamFrameSubline = 0; return; }
    if (c0 < 0) c0 = 0;
    if (c0 > r0) c0 = r0;
    // Later in the line = higher up the screen = a smaller y.
    beamFrameSubline = -((c0 * 2) / line);
}
void CpcVideo::beamVsync() {                  // CRTC VSYNC pin: vertical retrace
    if (beamRow >= BEAM_VSYNC_LOCK) beamPresent();  // locked: real frame end
    // else: spurious mid-frame VSYNC (rupture) — flywheel ignores it
}
void CpcVideo::plotBeamCharacter() {
    if (beamBack.empty()) beamReset();
    // Free-run failsafe: if no HSYNC arrives inside the flywheel window, retrace at
    // the top of the window anyway, so a missing sync cannot run a line on forever.
    if (beamCol >= BEAM_LINE_MAX) beamNewLine();
    int beamX = beamCol * 16 + BEAM_X_OFFSET + beamLineShiftShown   // ACCC §14.4/§15.7
                + monitorCalibration16();                           // ACCC §15.1, per set
    int beamY = beamRow * 2 + BEAM_Y_OFFSET + beamFrameSubline;   // ACCC §16.6
    beamCol += 1;

    // The GATE ARRAY's black zones do not begin or end on a character boundary, so
    // the character is composed into a 16-Pixel-M2 buffer and the black overlaid at
    // the exact Pixel-M2 the chapters give (§14.7.1, §16.2.1, §16.2.2). Both edges
    // are tracked here because they are edges of a per-character CRTC pin.
    const CrtcBehaviour* chip = crtc->behaviour;
    // ACCC §9.3.1 (p.52): "HSYNC is considered more quickly by the GATE ARRAY on
    // CRTC's 0, 1 and 2, than on the ASIC's of CRTC's 3 and 4. ASIC's delay HSYNC by
    // 1 µsec." §14.9's chart (p.145) shows it: with R2=10 the black opens on the
    // GATE ARRAY's character 9 on CRTC 0/1/2 and on character 10 on CRTC 4. The pipe
    // is a no-op (identity) for every chip whose delay is 0.
    beamHsyncPipe = (beamHsyncPipe << 1) | (crtc->hsync ? 1u : 0u);
    int hsyncDelay = chip->hsyncDisplayDelayCharacters() & 7;
    bool hsyncNow = (beamHsyncPipe >> hsyncDelay) & 1;
    bool hsyncBefore = (beamHsyncPipe >> (hsyncDelay + 1)) & 1;
    bool blankNow = crtc->gateArrayBlanking;
    // ACCC §14.7.1/§9.3.4.3, shared with the legacy compositor's capture so the two
    // renderers cannot disagree about where the black sits.
    int hsyncFrom = 16, hsyncTo = 16;      // Pixel-M2 half-open range to blacken
    gaHsyncBlackWindow(chip, hsyncNow, hsyncBefore,
                       crtc->r2WrittenThisCharacter, crtc->hsyncEndedJit, hsyncFrom, hsyncTo);
    int blankFrom = 16, blankTo = 16;      // ACCC §16.2.1's 26 black lines
    if (blankNow && beamBlankLast) { blankFrom = beamBlankCarry; beamBlankCarry = 0; }
    else if (blankNow && !beamBlankLast) {
        // ACCC §16.2.1: where inside the character the VSYNC black opens, and its
        // R7.JIT variant when R7 was written on this very character. CRTC 0's JIT
        // position lands a whole microsecond later, so the excess is carried into the
        // next character rather than lost.
        blankFrom = crtc->r7WrittenThisCharacter
            ? chip->vsyncBlackStartPixelJit(crtc->lastWriteBlockIo)
            : chip->vsyncBlackStartPixel();
        if (blankFrom > 16) { beamBlankCarry = blankFrom - 16; blankFrom = 16; }
    }
    else if (!blankNow && beamBlankLast) {
        blankFrom = 0; blankTo = chip->displayRestorePixelAfterHsync(); beamBlankCarry = 0;
    }
    beamBlankLast = blankNow;

    if (beamY < 0 || beamY + 1 >= height) return;
    auto put = [&](int x, uint32_t colour) {
        if (x < 0 || x >= width) return;
        beamBack[(size_t)beamY * width + x] = colour;
        beamBack[(size_t)(beamY + 1) * width + x] = colour;
    };
    // Per-BYTE display enable, not per-character: ACCC §17.6.2 has CRTC 0 and 2
    // bring the BORDER forward by exactly one byte (0.5 µsec) at the end of a line
    // when R1>R0, which is half a character.
    int bytes = crtc->displayOutputBytes();
    // The 17 pen colours as they stand for THIS character, and as they stood for the
    // previous one. ACCC §9.2.2 (p.49) needs both: "Updating the colour of an ink is
    // applied to the same position regardless of the graphics mode on CRTCs 0, 1, 2,
    // and 3. On CRTC 4, however, the color change 0.0625 µsec (1 pixel mode 2) earlier
    // in mode 2 than in the other three graphics modes." In mode 2 the pixel stream
    // runs one Pixel-M2 ahead (§9.1), so a character's leading pixel is painted in the
    // PREVIOUS character's time slot — and an ink written on the boundary between them
    // has, on every chip but the 40226, not happened yet at that instant.
    // Rebuilt only when an ink actually changed, so the common path costs nothing.
    if (!beamPensValid || asic->videoRevision != beamPenRevision) {
        for (int p = 0; p <= 16; p += 1) beamPensPrevious[p] = beamPensValid ? beamPens[p] : 0;
        for (int p = 0; p <= 16; p += 1) beamPens[p] = packedColor(penColor(p));
        beamPenRevision = asic->videoRevision;
        beamPensChanged = beamPensValid;      // nothing to carry on the very first one
        beamPensValid = true;
    } else beamPensChanged = false;
    uint32_t border = beamPens[16];
    uint32_t out[16];
    int outPen[16];
    for (int i = 0; i < 16; i += 1) outPen[i] = 16;
    int mode = ga().mode;
    // ACCC §9.3.4: a mode latch lands inside the FIRST byte of the character the GATE
    // ARRAY is decoding, at the Pixel-M2 that chip switches on. It applies to that one
    // byte, so it is consumed here whether or not the character is displayed —
    // otherwise a latch taken inside the HSYNC (where the CPC is normally in the
    // BORDER) would be held over and split the first displayed byte of the next line.
    int switchPixel = ga().modeSwitchPixel;
    int modeBefore = ga().modeBefore;
    ga().modeSwitchPixel = -1;
    // §9.3.4.1's figure (6th Pixel-M2 on CRTC 0/1/2, 4th on CRTC 4) is quoted for a
    // line displayed in MODE 2. §9.3.4.3.1-3 give the matching resume positions and
    // they move by one between a MODE 2 line and a MODE 0/1/3 line — "since the MODE 2
    // pixels are 'ahead' of the pixels of the other modes". The same wall-clock instant
    // is therefore one byte index earlier from any other source mode. (The pen values
    // are unaffected: a group cut by the switch does not advance the bit counter, so
    // both indices decode the new mode at the same rotation.)
    if (switchPixel > 0 && modeBefore != 2) switchPixel -= 1;
    // Which mode each of the 16 Pixel-M2 was decoded in, for §9.1's mode-2 advance.
    int pixelMode[16];
    for (int i = 0; i < 16; i += 1) pixelMode[i] = mode;
    bool displaying = bytes != 0;
    if (!displaying) {
        for (int i = 0; i < 16; i += 1) out[i] = border;
    } else {
        int base = crtcAddress(crtc->maRow) + (crtc->vlc & 7) * 0x800;
        int b0 = memory->readVideo(base), b1 = memory->readVideo(base + 1);
        int pens0[8], pens1[8];
        bytePens(b0, modeBefore, mode, switchPixel, pens0);
        bytePens(b1, mode, mode, -1, pens1);
        for (int p = 0; p < 8; p += 1) { outPen[p] = (bytes & 1) ? pens0[p] : 16; out[p] = beamPens[outPen[p]]; }
        for (int p = 0; p < 8; p += 1) { outPen[8 + p] = (bytes & 2) ? pens1[p] : 16; out[8 + p] = beamPens[outPen[8 + p]]; }
        if (switchPixel >= 0 && switchPixel < 8)
            for (int p = 0; p < switchPixel; p += 1) pixelMode[p] = modeBefore;
    }
    // ACCC §14.3: "When the GATE ARRAY receives a HSYNC signal from the CRTC, it
    // displays black color for 2 µsec... (during this period he stops displaying
    // colors). If the value programmed in R3 is greater than 6, the GATE ARRAY will
    // display black color again for the remaining period." So the HSYNC is black
    // throughout, and §16.2.1's V26 window likewise — "the black color will be
    // displayed for 26 lines".
    // ACCC §9.2.1: in mode 2 the GATE ARRAY is one Pixel-M2 ahead of itself, so the
    // whole pixel stream — border edges included — sits one Pixel-M2 to the left. The
    // pixel that lands on the previous character's last column is exactly the "BORDER
    // stops 1 pixel earlier" the chapter describes. The sync black zones below are the
    // GATE ARRAY's own and are NOT shifted with the pixel stream.
    // Per Pixel-M2, because a split character carries two modes: §9.3.4.3.1 has a
    // MODE 2 line resume "from the 5th Pixel-M2 of the 5th VRAM byte" where a MODE 0
    // or MODE 1 line resumes from the 4th — the same wall-clock instant, one byte
    // index later, which is exactly this advance seen from the byte's side.
    int shift = gaMode2Advance();
    // §9.2.2's charts (p.50/51) do not put an ink change on the character boundary: the
    // first pixel in the new colour is the SECOND BYTE of the character on the classic
    // GATE ARRAY and the 40226, and half a byte earlier on the 40489. Mode 2 is one
    // Pixel-M2 later still — except on the 40226, the one chip where §9.2.2 says the
    // colour change travels with the mode-2 stream instead of staying put.
    int inkPixel = chip->inkChangePixelInCharacter();
    int inkPixelMode2 = inkPixel + (chip->inkChangeFollowsMode2Advance() ? 0 : 1);
    for (int i = 0; i < 16; i += 1) {
        bool mode2 = pixelMode[i] == 2;
        int back = mode2 ? shift : 0;
        uint32_t colour = out[i];
        if (beamPensChanged && i < (mode2 ? inkPixelMode2 : inkPixel))
            colour = beamPensPrevious[outPen[i]];
        put(beamX + i - back, colour);
    }
    uint32_t black = packedColor(0);
    for (int i = hsyncFrom; i < hsyncTo; i += 1) put(beamX + i, black);
    for (int i = blankFrom; i < blankTo; i += 1) put(beamX + i, black);
}
// The tube. An explicit tint is an OVERRIDE for looking at a colour program in green;
// with none, the tube is whatever the set that is plugged in has, which is the only
// physical answer. Passing "" or "set" hands it back to the set.
void CpcVideo::setMonitorMode(const std::string& mode) {
    monitorTint = (mode.empty() || mode == "set") ? std::string() : normaliseMonitorMode(mode);
    monitorMode = monitorTint.empty()
        ? normaliseMonitorMode(monitorSet ? monitorSet->phosphor : MONITOR_MODE_COLOUR)
        : monitorTint;
}
void CpcVideo::setMonitorModel(const MonitorModel* model) {
    monitorSet = model;
    setMonitorMode(monitorTint);      // re-take the tube from the new set
}
// ACCC §15.1 (p.146): the set's own horizontal calibration, as Pixel-M2 of back porch.
// A CM14 (and the CTM Amstrad calibrated for the CRTC 4 machine) sits a microsecond
// further behind the sync than a CTM 640/644, which is what cancels the microsecond the
// ASIC delays the whole HSYNC by -- so a Plus on its own monitor is centred, and the same
// machine on a plain CTM has "the image shifted to the left".
int CpcVideo::monitorCalibration16() const {
    // Asked of the MONITOR, never of this object's own copy: the set is the monitor's
    // identity and a renderer that kept its own would be able to disagree with it.
    return monitor ? monitor->calibration16() : 0;
}
std::optional<bool> CpcVideo::lightgunBrightnessAt(double x, double y) {
    int px = (int)std::floor(x), py = (int)std::floor(y);
    if (px < 0 || py < 0 || px >= width || py >= height) return std::nullopt;
    uint32_t packed = pixels[(size_t)py * width + px];
    int r = packed & 0xff, g = (unsigned)packed >> 8 & 0xff, b = (unsigned)packed >> 16 & 0xff;
    return r * 0.299 + g * 0.587 + b * 0.114 >= 140;
}
bool CpcVideo::lightgunRgbBright(int rgb) {
    int r = (unsigned)rgb >> 16 & 255, g = (unsigned)rgb >> 8 & 255, b = rgb & 255;
    return r * 0.299 + g * 0.587 + b * 0.114 >= 140;
}
uint32_t CpcVideo::packedColor(int rgb) { return (uint32_t)(0xff000000u | (rgb & 0xff) << 16 | (rgb & 0xff00) | ((unsigned)rgb >> 16)); }
int CpcVideo::applyMonitor(int rgb) { return monitorTransformRgb(rgb, monitorMode); }
int CpcVideo::penColor(int pen) {
    // The GATE ARRAY says which RGB, the tube says what that looks like -- in that order,
    // once. This used to ask monitor_palette for an ink-indexed colour, which meant a
    // second copy of the GATE ARRAY's colour table and a second, different green model.
    if (asic->locked) return applyMonitor(GateArray::rgbForHardwareColour(ga().gaPalette[pen]));
    return applyMonitor(asic->color(pen));
}
int CpcVideo::penColor(int pen, const PenState& state) {
    if (!state.present) return penColor(pen);
    if (state.locked) return applyMonitor(GateArray::rgbForHardwareColour(state.gaPalette[pen]));
    return applyMonitor(asic->colorValue(state.palette[pen & 0x1f]));
}
void CpcVideo::setPixel(int x, int y, int color, int w) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    uint32_t packed = packedColor(color); int row = y * width + x;
    int end = std::min(row + w, (y + 1) * width);
    std::fill(pixels.begin() + row, pixels.begin() + end, packed);
}
void CpcVideo::drawScaledPixel(int x, int y, int color, int w, int horizontalScroll) {
    int screenX = (lineOrigin ? lineOrigin->x : origin.x) + x + horizontalScroll;
    // The monitor's field offset (monitorHalfLine) moves the whole line down one row: the
    // border and the HSYNC black already take it, so the character pixels must too. They
    // did not, and on an odd field the black HSYNC zone and the characters it covers came
    // out interleaved row by row (SHAKER BI/A-H on CRTC 2, after an odd-length frame had
    // flipped the monitor's field parity many tests earlier).
    int screenY = (linePhysicalY ? *linePhysicalY : (origin.y + y)) * 2 + lineHalfLine;
    int clippedWidth = !lineClipRight ? w : std::min(w, *lineClipRight - screenX);
    if (clippedWidth <= 0) return;
    setPixel(screenX, screenY, color, clippedWidth); setPixel(screenX, screenY + 1, color, clippedWidth);
}
void CpcVideo::drawScaledPixel(int x, int y, int color, int w) { drawScaledPixel(x, y, color, w, asic->horizontalScroll); }
void CpcVideo::paintBorderScanline(int py, const RasterLine* state, const std::array<uint8_t, 18>& frameRegisters) {
    // ACCC §16.2.1: while the GATE ARRAY's V26 is running the line is BLACK -- not the
    // border pen, and not the display. A VSYNC in the middle of the picture therefore
    // lays a 26-line black band across it, which is exactly what SHAKER's DT test
    // measures. Painting it here covers the border and the display together, because the
    // caller draws characters on top of this and the blank has to survive that.
    if (state && state->gateArrayBlank) {
        uint32_t black = packedColor(0);
        if (py >= 0 && py < height) std::fill(pixels.begin() + py * width, pixels.begin() + (py + 1) * width, black);
        if (py + 1 >= 0 && py + 1 < height) std::fill(pixels.begin() + (py + 1) * width, pixels.begin() + (py + 2) * width, black);
        return;
    }
    // ACCC §9.1: the BORDER is a pen like any other -- §9.1's 17th colour -- and which
    // pen leaves the GATE ARRAY is that chip's call, from the CRTC's DISPTMG pin.
    uint32_t lineBorder = packedColor(penColor(ga().outputPen(false, 0), penFromLine(state)));
    if (py >= 0 && py < height) std::fill(pixels.begin() + py * width, pixels.begin() + (py + 1) * width, lineBorder);
    if (py + 1 >= 0 && py + 1 < height) std::fill(pixels.begin() + (py + 1) * width, pixels.begin() + (py + 2) * width, lineBorder);
    if (!state || state->segments.empty()) return;
    const std::array<uint8_t, 18>& lineRegisters = state->crtcRegisters;
    // A mid-line BORDER change is the same beam, on the same line, at the character the
    // segment names -- so it has to start from the same origin the characters do. The
    // display path takes that from the monitor's own sweep (classicPhysicalLineOrigin,
    // i.e. monitorLineOffset); this was computing its own from the register file
    // instead, (0x32-R2)*16 + R3l*8 + (R0-0x3f)*16. The two agree only while the line's
    // registers match the frame's -- which on a ruptured line, the whole point of these
    // segments, they do not. So the border step landed at one x and the characters it
    // bordered at another.
    bool physical = crtc->usesPhysicalRasterFrame ? crtc->usesPhysicalRasterFrame() : false;
    int rasterCharacterZero = physical
        ? classicPhysicalLineOrigin(state, lineRegisters, frameRegisters).x
        : classicMonitorLineOrigin(lineRegisters, frameRegisters, state->crtcType).x;
    for (const auto& segment : state->segments) {
        int startX = std::max(0, std::min(width, rasterCharacterZero + segment.character * 16));
        uint32_t colour = packedColor(penColor(ga().outputPen(false, 0), penFromSegment(segment)));
        if (py >= 0 && py < height) std::fill(pixels.begin() + py * width + startX, pixels.begin() + (py + 1) * width, colour);
        if (py + 1 >= 0 && py + 1 < height) std::fill(pixels.begin() + (py + 1) * width + startX, pixels.begin() + (py + 2) * width, colour);
    }
}
// ACCC §14.7.1 / §9.3.4.3: THE GATE ARRAY'S HSYNC BLACK, LAID OVER THE FINISHED LINE.
//
// "During HSYNC, in principle, nothing is displayed anymore" (§15.1). That black is the
// GATE ARRAY's output stage, not a property of the border or of the characters, so it
// goes on after both -- the same reason §16.2.1's VSYNC blanking is painted in
// paintBorderScanline ahead of them and then protected by a `continue`.
//
// The beam renderer had this from the start, because it plots what the pins say at the
// moment it plots. The legacy compositor rebuilds a line from a RasterLine record and
// had nothing to rebuild it from, so every HSYNC that fell inside the picture -- which is
// what SHAKER's AR test is made of, and what any mid-line R2 move produces -- simply lost
// its black. That was 28 failing screens across all five chips.
//
// The window is per character and sub-character (the zone opens on the 6th Pixel-M2),
// which is why it is stored as a range rather than a flag; gaHsyncBlackWindow decides it
// once for both renderers.
void CpcVideo::paintHsyncBlack(int py, const RasterLine* state,
                               const std::array<uint8_t, 18>& frameRegisters) {
    if (!state || state->hsyncBlackFrom.empty() || state->gateArrayBlank) return;
    // CPCSE_NO_HSYNC_BLACK=1: leave it off, to tell a black the GATE ARRAY paints from a
    // black that is in the picture data -- a diagnostic, the picture is wrong without it.
    static const bool skip = std::getenv("CPCSE_NO_HSYNC_BLACK") != nullptr;
    if (skip) return;
    const std::array<uint8_t, 18>& lineRegisters = state->crtcRegisters;
    // The same character-zero the mid-line border segments use, so the black lands in
    // the coordinate space the captured character indices are in.
    bool physical = crtc->usesPhysicalRasterFrame ? crtc->usesPhysicalRasterFrame() : false;
    int characterZero = physical
        ? classicPhysicalLineOrigin(state, lineRegisters, frameRegisters).x
        : classicMonitorLineOrigin(lineRegisters, frameRegisters, state->crtcType).x;
    uint32_t black = packedColor(0);
    int painted = state->capturedCharacters > 0
        ? std::min((int)state->hsyncBlackFrom.size(), state->capturedCharacters) : 0;
    for (int character = 0; character < painted; character += 1) {
        int from = state->hsyncBlackFrom[character], to = state->hsyncBlackTo[character];
        if (from >= to) continue;
        int x0 = std::max(0, std::min(width, characterZero + character * 16 + from));
        int x1 = std::max(0, std::min(width, characterZero + character * 16 + to));
        if (x1 <= x0) continue;
        if (py >= 0 && py < height)
            std::fill(pixels.begin() + (size_t)py * width + x0, pixels.begin() + (size_t)py * width + x1, black);
        if (py + 1 >= 0 && py + 1 < height)
            std::fill(pixels.begin() + (size_t)(py + 1) * width + x0, pixels.begin() + (size_t)(py + 1) * width + x1, black);
    }
}
// ACCC §9.1: decoding a VRAM byte into pens is the GATE ARRAY's work, not the
// renderer's -- "the GATE ARRAY/ASIC reads the data pointed to by the CRTC from memory
// in order to convert and display it as pixels". These four forward to that chip so
// the existing call sites read the same; the 40007/40008-vs-40010 padding difference
// now comes from the model rather than from an argument threaded through the renderer.
// The fallback is the commonest part, and is only reached in isolated tests that build
// a CpcVideo with no machine behind it.
GateArray& CpcVideo::ga() const {
    static GateArray fallback(nullptr, nullptr, false);   // mutable: it is a real chip
    return gateArray ? *gateArray : fallback;
}
std::array<int, 2> CpcVideo::mode0(int byte) { return ga().mode0Pens(byte); }
std::array<int, 4> CpcVideo::mode1(int byte) { return ga().mode1Pens(byte); }
void CpcVideo::bytePens(int byte, int modeBefore, int modeAfter, int switchPixel, int pens[8]) {
    ga().bytePens(byte, modeBefore, modeAfter, switchPixel, pens);
}
int CpcVideo::pixelPen(int mode, int byte, int pixel) { return ga().pixelPen(mode, byte, pixel); }
int CpcVideo::scrolledVideoByte(const RasterLine* state, int lineBase, int sourceColumn) {
    if (sourceColumn >= 0) return state ? state->videoBytes[sourceColumn] : memory->readVideo(lineBase + sourceColumn);
    if (state && sourceColumn >= -2) return state->videoLookbehind[sourceColumn + 2];
    int page = lineBase & ~0x07ff;
    return memory->readVideo(page | (lineBase + sourceColumn & 0x07ff));
}
// 0 or 1 framebuffer rows: the sub-scanline position of the field the monitor is
// currently sweeping. Zero when there is no monitor (the Plus path renders from the
// CRTC's own frame, and the oracle harnesses build a CpcVideo on its own).
int CpcVideo::monitorHalfLine() const { return monitor ? monitor->verticalHalfLine : 0; }
int CpcVideo::crtcAddress(int ma) {
    int doubled = (ma & 0x3fff) << 1;
    return (doubled & 0x07fe) | ((doubled & 0x6000) << 1);
}
void CpcVideo::render() {
    if (beamMode) return;                        // beam renderer already filled `pixels`
    uint32_t border = packedColor(penColor(ga().outputPen(false, 0))); std::fill(pixels.begin(), pixels.end(), border);
    std::array<uint8_t, 18> frameRegisters = crtc->getFrameRegisters ? crtc->getFrameRegisters() : crtc->registers;
    origin = displayOrigin(frameRegisters, crtc->type);
    const std::vector<std::shared_ptr<RasterLine>>& rasterFrame = crtc->getRasterFrame ? crtc->getRasterFrame() : EMPTY_FRAME;
    const std::vector<std::shared_ptr<RasterLine>>& previousRasterFrame = crtc->getPreviousRasterFrame ? crtc->getPreviousRasterFrame() : EMPTY_FRAME;
    bool physicalRasterFrame = crtc->usesPhysicalRasterFrame ? crtc->usesPhysicalRasterFrame() : false;
    std::array<uint8_t, 18> monitorRegisters = frameRegisters;
    if (physicalRasterFrame) monitorRegisters[2] = (uint8_t)classicMonitorReferenceR2(rasterFrame, frameRegisters[2]);
    // Plus rupture: the accumulated monitor frame is taller than R4 implies — the
    // CRTC restarted mid-frame with VSYNC suppressed (Alcon 2020: R4=9 sub-frames).
    // The register origin formula (R4-R7) can't place such a frame, so position it
    // by the real VSYNC line like the physical monitor path, while keeping the Plus
    // per-line renderer. Normal Plus frames (size == R4 geometry) are untouched.
    // !physicalRasterFrame IS the Plus (usesPhysicalRasterFrame() is plusHardware ==
    // false), so no CRTC type test is needed on top of it -- and none should be: a
    // CRTC 4 machine is a classic CPC and takes the physical path like any other.
    bool plusRupture = !physicalRasterFrame && !rasterFrame.empty()
        && (int)rasterFrame.size() > (((frameRegisters[4] & 0x7f) + 1) * rasterHeightFromRegisters(frameRegisters)) + 16;
    bool physicalPos = physicalRasterFrame || plusRupture;
    int physicalOriginY = physicalPos
        ? physicalFrameOriginFromVsync(rasterFrame, (physicalRasterFrame && crtc->getPhysicalFrameOriginY) ? crtc->getPhysicalFrameOriginY() : origin.y,
                                       monitor ? monitor->lockedFieldLines() : 0,
                                       plusRupture && !physicalRasterFrame)
        : origin.y;
    int physicalSourceStart = physicalRasterFrame ? rasterLineForMonitorY(0, physicalOriginY, (int)rasterFrame.size()) : 0;
    bool frameScroll = frameHorizontalScrollActive(rasterFrame, rasterFrame.empty() ? asic->horizontalScroll : 0);
    // The interlaced field is swept half a scanline lower than the other one, which is
    // one row of this framebuffer (ACCC §19.3.1; see CpcVideo::monitorHalfLine). Both
    // passes have to take it or the border and the characters come apart by a row.
    int halfLine = physicalPos ? monitorHalfLine() : 0;
    lineHalfLine = halfLine;
    if (!rasterFrame.empty()) {
        for (int py = 0; py < height; py += 2) {
            int physicalY = (int)std::floor(py / 2);
            int sourceLine = rasterLineForMonitorY(physicalY, physicalOriginY, (int)rasterFrame.size());
            RasterLine* state = physicalRasterFrame
                ? physicalFrameState(rasterFrame, previousRasterFrame, sourceLine, physicalSourceStart)
                : (sourceLine >= 0 && sourceLine < (int)rasterFrame.size() ? rasterFrame[sourceLine].get() : nullptr);
            paintBorderScanline(py + halfLine, state, monitorRegisters);
        }
    }
    int charactersPerLine = frameRegisters[1];
    int rasterHeight = rasterHeightFromRegisters(frameRegisters);
    int displayedRows = frameRegisters[6];
    int maxDisplayBytes = physicalRasterFrame ? 0x84 : (int)std::ceil(width / 8.0) + 4;
    int fallbackLines = displayedRows * rasterHeight;
    int displayedLines = physicalPos
        ? std::min((int)rasterFrame.size(), (int)std::ceil(height / 2.0))
        : std::min((int)(rasterFrame.empty() ? fallbackLines : rasterFrame.size()), (int)std::ceil(height / 2.0) + std::max(0, -origin.y));
    for (int monitorY = 0; monitorY < displayedLines; monitorY += 1) {
        int y = physicalPos ? rasterLineForMonitorY(monitorY, physicalOriginY, (int)rasterFrame.size()) : monitorY;
        RasterLine* state = physicalRasterFrame
            ? physicalFrameState(rasterFrame, previousRasterFrame, y, physicalSourceStart)
            : (y >= 0 && y < (int)rasterFrame.size() ? rasterFrame[y].get() : nullptr);
        if (state && state->vDisplay == false) {
            bool anyEnabled = false; for (uint8_t v : state->displayEnabled) if (v) { anyEnabled = true; break; }
            if (!anyEnabled) continue;
        }
        const std::array<uint8_t, 18>& lineRegisters = state ? state->crtcRegisters : frameRegisters;
        // ACCC §19.2.1 BORDER ON: R8 = 001100xx "allows you to deactivate the display of
        // characters". The skew bits are read through the chip profile at capture time
        // (RasterLine::displaySkewBits), so the chips that have no such function --
        // CRTC 1 and 2, §19.2 -- report 0 without the renderer having to know which.
        int lineSkew = state ? state->displaySkewBits : crtc->behaviour->displaySkew(*crtc);
        if (lineSkew == 3) continue;
        lineOrigin = physicalRasterFrame
            ? classicPhysicalLineOrigin(state, lineRegisters, monitorRegisters)
            : classicMonitorLineOrigin(lineRegisters, monitorRegisters, state ? state->crtcType : 3);
        linePhysicalY = physicalPos ? std::optional<int>(monitorY) : std::nullopt;
        int lineCharacters = state ? state->charactersPerLine : charactersPerLine;
        int displayStartCharacter = state ? state->displayStartCharacter : 0;
        int displayStartByte = displayStartCharacter * 2;
        int lineRasterHeight = state ? state->rasterHeight : rasterHeight;
        int horizontalScroll = state ? state->horizontalScroll : asic->horizontalScroll;
        int verticalScroll = state ? state->verticalScroll : asic->verticalScroll;
        int extendBorder = state ? state->extendBorder : asic->softScrollControl;
        int rowY = (physicalPos ? monitorY * 2 : (origin.y + y) * 2) + halfLine;
        paintBorderScanline(rowY, state, monitorRegisters);
        // ACCC §16.2.1: the GATE ARRAY "does not display a character" while V26 is
        // running, so the blank just painted must not be drawn over.
        if (state && state->gateArrayBlank) continue;
        int sourceY = y + verticalScroll;
        int lineBase;
        if (state) lineBase = crtcAddress(state->lineAddress) + (state->videoRaster & 7) * 0x800;
        else {
            int characterRow = (int)std::floor((double)sourceY / lineRasterHeight), raster = sourceY % lineRasterHeight;
            // ACCC §10.1 (p.74): "The internal 'row' counter C9 is connected directly to
            // the bits 11, 12 and 13 of the VRAM pointer... Bits 3 and 4 of the counter
            // are not considered in the calculation of the video pointer on C9 values
            // that exceed 7." R9 is five bits, so a character of more than 8 lines
            // repeats the same eight 2 KB slices rather than running off the page --
            // the three other places this address is built already mask it.
            lineBase = crtcAddress(screenAddressFromRegisters(frameRegisters) + characterRow * lineCharacters) + (raster & 7) * 0x800;
        }
        int renderCharacters = lineCharacters;
        if (state) {
            int lastEnabled = -1;
            for (int character = (int)state->displayEnabled.size() - 1; character >= displayStartCharacter; character -= 1) {
                if (state->displayEnabled[character]) { lastEnabled = character; break; }
            }
            renderCharacters = lastEnabled < displayStartCharacter ? 0 : lastEnabled - displayStartCharacter + 1;
        }
        int lineBytes = std::min(renderCharacters * 2, maxDisplayBytes);
        // CPCSE_TRACE_ROW=<monitor row>: how that row was composed -- which record, where
        // its character zero landed, and the byte range drawn. For a line that shows the
        // wrong thing while its record looks right.
        const char* traceRowEnv = std::getenv("CPCSE_TRACE_ROW");
        if (traceRowEnv && (!traceRowsOnlyWhenArmed || traceRowsArmed)) {
            const char* row = traceRowEnv;
            const bool every = std::string(row) == "all";
            if (physicalPos && (every || monitorY == std::atoi(row)) && state) {
                const bool fromPrevious = physicalRasterFrame && physicalSourceStart > 0
                    && y >= physicalSourceStart && !previousRasterFrame.empty();
                std::fprintf(stderr, "ROWSRC %3d <- line %3d of %s frame (origin %d, start %d, size %zu) "
                             "ma=%04x vlc=%d C4=%d C9=%d\n",
                             monitorY, y, fromPrevious ? "PREVIOUS" : "current",
                             physicalOriginY, physicalSourceStart, rasterFrame.size(),
                             state->lineAddress & 0x3fff, state->videoRaster,
                             state->verticalCounter, state->rasterCounter);
                int firstEn = -1;
                for (int c = 0; c < (int)state->displayEnabled.size(); c++)
                    if (state->displayEnabled[c]) { firstEn = c; break; }
                std::fprintf(stderr, "ROW %d: record c0@=%d off=%d ph=%d originX=%d start=%d "
                             "renderChars=%d lineBytes=%d maxDisplayBytes=%d firstEn=%d -> x=%d "
                             "decision %c phase %d move %+d tips %d\n",
                             monitorY, state->horizontalCounterAtMonitorLine, state->monitorLineOffset,
                             state->sweepPhase16, lineOrigin ? lineOrigin->x : -9999,
                             displayStartCharacter, renderCharacters, lineBytes, maxDisplayBytes,
                             firstEn, (lineOrigin ? lineOrigin->x : 0) + firstEn * 16,
                             state->monitorDecisionBranch, state->monitorDecisionPhase,
                             state->monitorDecisionMove, state->monitorDecisionTips);
            }
        }
        // The two §9.2.2 hooks are the CRTC profile's, asked of the chip exactly as the
        // beam path asks for them, rather than re-derived here.
        const CrtcBehaviour* chipBehaviour = crtc->behaviour;
        int segmentIndex = -1; PenState pixelState = penFromLine(state);
        bool extendActive = rasterExtendBorderActive(state, extendBorder);
        lineClipRight = extendActive ? std::optional<int>(lineOrigin->x + renderCharacters * 16) : std::nullopt;
        if (frameScroll) {
            int firstPixel = extendActive ? 16 : 0;
            for (int x = firstPixel; x < lineBytes * 8; x += 1) {
                int character = displayStartCharacter + ((unsigned)x >> 4);
                while (state && segmentIndex + 1 < (int)state->segments.size() && state->segments[segmentIndex + 1].character <= character) {
                    segmentIndex += 1; pixelState = penFromSegment(state->segments[segmentIndex]);
                }
                if (state && !(state->displayEnabled[character] & 1 << ((unsigned)x >> 3 & 1))) continue;
                int mode = pixelState.present ? pixelState.mode : ga().mode;
                int pixelScroll = pixelState.present ? pixelState.horizontalScroll : horizontalScroll;
                int sourcePixel = x - pixelScroll;
                int sourceColumn = displayStartByte + (int)std::floor(sourcePixel / 8.0);
                int sourceSubPixel = (sourcePixel % 8 + 8) & 7;
                int byte = scrolledVideoByte(state, lineBase, sourceColumn);
                int pen = pixelPen(mode, byte, sourceSubPixel);
                drawScaledPixel(x, y, penColor(pen, pixelState), 1, 0);
            }
        } else {
            // ACCC §9.2.2's ink-update charts (p.50 for the 40010 and the 40226, p.51 for
            // the 40489), which the beam path already reads through the same two hooks.
            // They are drawn against a "Byte Offset (0.5 µsec)" ruler, so one CRTC
            // character is 16 Mode-2 pixels, and the new colour does NOT take over on the
            // character boundary: with OUT (C),r8 the first pixel in the new ink is
            // Mode-2 pixel 120 of a chart that opens at 80, i.e. half a microsecond into
            // the character -- its SECOND BYTE. (Half a byte earlier on the 40489, hence
            // the per-chip hook rather than a constant here.) Taking the segment's palette
            // from the character's first byte put every mid-line ink change half a
            // character early, which is what SHAKER module D's DI bar measures: its
            // handler at #8523 holds the BORDER pen at #4A for exactly one OUT (C),r8, and
            // the left edge of the bar that paints is this half microsecond.
            const int inkPixel = chipBehaviour->inkChangePixelInCharacter();
            const int inkPixelMode2 =
                inkPixel + (chipBehaviour->inkChangeFollowsMode2Advance() ? 0 : 1);
            PenState previousState = pixelState;
            for (int column = firstVisibleByte(extendActive); column < lineBytes; column += 1) {
                int sourceColumn = displayStartByte + column;
                int character = displayStartCharacter + ((unsigned)column >> 1);
                while (state && segmentIndex + 1 < (int)state->segments.size() && state->segments[segmentIndex + 1].character <= character) {
                    segmentIndex += 1;
                    previousState = pixelState;
                    pixelState = penFromSegment(state->segments[segmentIndex]);
                }
                if (state && !(state->displayEnabled[character] & 1 << (column & 1))) continue;
                int mode = pixelState.present ? pixelState.mode : ga().mode;
                int byte = state ? state->videoBytes[sourceColumn] : memory->readVideo(lineBase + sourceColumn);
                // §9.2.2, above: on the character a segment starts on, the pixels before
                // the chip's ink instant are still drawn in the OLD ink. inkSplit is that
                // instant measured from this byte's own first pixel, so it is <= 0 for a
                // byte wholly in the new ink and >= 8 for one wholly in the old.
                const int inkSplit = (previousState.present && pixelState.present
                                      && character == pixelState.splitCharacter)
                    ? (mode == 2 ? inkPixelMode2 : inkPixel) - (column & 1) * 8
                    : 0;
                auto inkAt = [&](int pixelInByte) -> const PenState& {
                    return pixelInByte < inkSplit ? previousState : pixelState;
                };
                // ACCC §9.3.4 (p.54): "Given that mode update occurs during the processing
                // of the byte read in ram, the algorithm for determining the PEN by the GA
                // switches during processing. Consequently, the calculated PENs are
                // distorted for the rest of the pixels to be displayed in the byte." That
                // byte is the first one of the character the segment starts on; every
                // other byte decodes in the segment's mode whole. Without this the first
                // character after a mid-line mode split took the new mode's pens all the
                // way across, which showed up as the wrong colour on the leading pixels of
                // the first letter.
                const bool modeSplitHere = pixelState.modeSwitchPixel >= 0
                    && character == pixelState.splitCharacter && (column & 1) == 0;
                // A mode split and an ink split land on the same byte -- both belong to
                // the character the segment starts on -- so they are decoded together:
                // the pens come from the mode, the ink from §9.2.2's instant.
                if (modeSplitHere || (inkSplit > 0 && inkSplit < 8)) {
                    int pens[8];
                    if (modeSplitHere) {
                        int switchPixel = pixelState.modeSwitchPixel;
                        // §9.3.4.3: the instant is one index earlier when the outgoing mode
                        // is not 2, "since the MODE 2 pixels are 'ahead' of the pixels of
                        // the other modes" -- the same shift the beam path applies.
                        if (switchPixel > 0 && pixelState.modeBefore != 2) switchPixel -= 1;
                        bytePens(byte, pixelState.modeBefore, mode, switchPixel, pens);
                    } else bytePens(byte, mode, mode, -1, pens);
                    for (int pixel = 0; pixel < 8; pixel += 1)
                        drawScaledPixel(column * 8 + pixel, y, penColor(pens[pixel], inkAt(pixel)), 1, 0);
                    continue;
                }
                // One ink across the whole byte: the old one while the character is still
                // before §9.2.2's instant, otherwise the segment's own.
                const PenState& ink = inkSplit >= 8 ? previousState : pixelState;
                if (mode == 0 || mode == 3) { auto values = mode0(byte); int mask = mode == 3 ? 3 : 15; drawScaledPixel(column * 8, y, penColor(values[0] & mask, ink), 4, 0); drawScaledPixel(column * 8 + 4, y, penColor(values[1] & mask, ink), 4, 0); }
                else if (mode == 1) { auto values = mode1(byte); for (int pixel = 0; pixel < 4; pixel += 1) drawScaledPixel(column * 8 + pixel * 2, y, penColor(values[pixel], ink), 2, 0); }
                else for (int pixel = 0; pixel < 8; pixel += 1) drawScaledPixel(column * 8 + pixel, y, penColor((unsigned)byte >> (7 - pixel) & 1, ink), 1, 0);
            }
        }
        paintHsyncBlack(rowY, state, monitorRegisters);
    }
    lineClipRight = std::nullopt; lineOrigin = std::nullopt; linePhysicalY = std::nullopt;
    lineHalfLine = 0;
    renderSprites(frameScroll);
}
void CpcVideo::drawSpriteChunk(const RasterLine* state, const SpriteState& spriteState, int displayY, int pixelY,
    const DisplayBounds& lineBounds, int spriteRasterLeft, int lineClipLeft, int chunkLeft, int chunkRight) {
    const Bytes* attributes = spriteState.spriteAttributes;
    const std::vector<int>* magnifications = spriteState.spriteMagnification;
    const std::vector<int>* palette = spriteState.palette;
    if (!attributes || !magnifications) return;
    for (int sprite = 15; sprite >= 0; sprite -= 1) {
        XY position = spriteCoordinates(*attributes, sprite * 8);
        int magnify = (*magnifications)[sprite], magX = (unsigned)magnify >> 2 & 3, magY = magnify & 3;
        if (!magX || !magY) continue;
        static const int sc[4] = { 1, 1, 2, 4 };
        int sx = sc[magX], sy = sc[magY];
        int spriteLine = state ? (state->verticalAdjust ? 0 : (((state->verticalCounter & 0x3f) << 3) | (state->rasterCounter & 7))) : displayY;
        int relativeY = (spriteLine - position.y) & 0x1ff;
        if (relativeY >= 16 * sy) continue;
        int sourceRow = (int)std::floor((double)relativeY / sy);
        const Bytes* patterns = spriteState.spritePatterns;
        const uint8_t* data;
        Bytes liveData;
        if (patterns) data = patterns->data() + sprite * 256;
        else { liveData.assign(asic->sprites[sprite].begin(), asic->sprites[sprite].end()); data = liveData.data(); }
        int x = spriteRasterLeft + position.x;
        for (int column = 0; column < 16; column += 1) {
            int pen = data[sourceRow * 16 + column]; if (!pen) continue;
            int rgb = applyMonitor(palette ? asic->spriteColorValue((*palette)[16 + pen]) : asic->spriteColor(16 + pen));
            for (int xx = 0; xx < sx; xx += 1) {
                int pixelX = x + column * sx + xx;
                if (pixelX >= chunkLeft && pixelX < chunkRight && pixelX >= lineClipLeft && pixelX < lineBounds.right) {
                    setPixel(pixelX, pixelY, rgb); setPixel(pixelX, pixelY + 1, rgb);
                }
            }
        }
    }
}
void CpcVideo::renderSprites(bool frameScroll) {
    std::array<uint8_t, 18> frameRegisters = crtc->getFrameRegisters ? crtc->getFrameRegisters() : crtc->registers;
    DisplayBounds clip = activeDisplayBounds(frameRegisters);
    const std::vector<std::shared_ptr<RasterLine>>& rasterFrame = crtc->getRasterFrame ? crtc->getRasterFrame() : EMPTY_FRAME;
    int rasterHeight = rasterHeightFromRegisters(frameRegisters);
    int displayedLines = std::min((int)(rasterFrame.empty() ? frameRegisters[6] * rasterHeight : rasterFrame.size()), (int)std::ceil(height / 2.0) + std::max(0, -origin.y));
    Bytes currentAttributes = Bytes(memory->asicRam.begin() + 0x2000, memory->asicRam.begin() + 0x2080);
    std::vector<int> liveMagnification = std::vector<int>(asic->spriteMagnification.begin(), asic->spriteMagnification.end());
    for (int displayY = 0; displayY < displayedLines; displayY += 1) {
        RasterLine* state = (displayY < (int)rasterFrame.size() && rasterFrame[displayY]) ? rasterFrame[displayY].get() : nullptr;
        if ((state ? state->locked : asic->locked) || (state && state->vDisplay == false)) continue;
        const std::array<uint8_t, 18>& lineRegisters = state ? state->crtcRegisters : frameRegisters;
        DisplayBounds lineBounds = activeDisplayBounds(lineRegisters);
        int skew = (unsigned)lineRegisters[8] >> 4 & 3;
        int spriteRasterLeft = lineBounds.left - (skew < 3 ? skew * 16 : 0);
        int lineClipLeft = spriteClipLeft(lineBounds.left, state, state ? state->extendBorder : asic->softScrollControl, frameScroll);
        int pixelY = (origin.y + displayY) * 2;
        if (pixelY + 1 < clip.top || pixelY >= clip.bottom) continue;
        int lineCharacters = state ? state->charactersPerLine : lineRegisters[1];
        SpriteState spriteState;
        spriteState.spriteAttributes = state ? &state->spriteAttributes : &currentAttributes;
        spriteState.spriteMagnification = state ? &state->spriteMagnification : &liveMagnification;
        spriteState.spritePatterns = state && !state->spritePatterns.empty() ? &state->spritePatterns : nullptr;
        spriteState.palette = state ? &state->palette : nullptr;
        int startCharacter = 0;
        // Keep segment-derived storage alive for the pointers in spriteState.
        std::vector<Bytes> segAttrStore; std::vector<std::vector<int>> segMagStore; std::vector<Bytes> segPatStore; std::vector<std::vector<int>> segPalStore;
        auto drawUntil = [&](int endCharacter) {
            int end = std::max(startCharacter, std::min(lineCharacters, endCharacter));
            if (end > startCharacter) {
                drawSpriteChunk(state, spriteState, displayY, pixelY, lineBounds, spriteRasterLeft, lineClipLeft,
                    spriteRasterLeft + startCharacter * 16, spriteRasterLeft + end * 16);
            }
            startCharacter = end;
        };
        if (state) {
            for (const auto& segment : state->segments) {
                if (segment.character > startCharacter) drawUntil(segment.character);
                // segments always carry all four fields in captureRasterCharacter.
                segAttrStore.push_back(segment.spriteAttributes); spriteState.spriteAttributes = &segAttrStore.back();
                segMagStore.push_back(segment.spriteMagnification); spriteState.spriteMagnification = &segMagStore.back();
                segPatStore.push_back(segment.spritePatterns); spriteState.spritePatterns = segPatStore.back().empty() ? spriteState.spritePatterns : &segPatStore.back();
                segPalStore.push_back(segment.palette); spriteState.palette = &segPalStore.back();
            }
        }
        drawUntil(lineCharacters);
    }
}

} // namespace cpcse
