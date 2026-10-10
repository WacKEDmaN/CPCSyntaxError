// CPCSyntaxError — CPC and GX4000 machine core.
#include "emulator.h"
#include "memory.h"
#include "asic.h"
#include "z80.h"
#include "gate_array.h"
#include "crtc.h"
#include "keyboard.h"
#include "ay.h"
#include "dac.h"
#include "tape.h"
#include "ppi.h"
#include "gamepad.h"
#include "fdc.h"
#include "m4.h"
#include "v9990.h"
#include "opl4.h"
#include "playcity.h"
#include "rs232.h"
#include "speech.h"
#include "symbiface_mouse.h"
#include "sf2_rtc.h"
#include "sf3.h"
#include "symbiface_ide.h"
#include "multiface.h"
#include "monitor_model.h"
#include "monitor_renderer.h"
#include "lightgun.h"

namespace cpcse {

static std::vector<int> arrToVec(const std::array<uint8_t, 17>& a) { return std::vector<int>(a.begin(), a.end()); }
static std::vector<int> arrToVec16(const std::array<uint16_t, 32>& a) { std::vector<int> v; v.reserve(32); for (uint16_t x : a) v.push_back(x); return v; }
static std::vector<int> arrToVec(const std::array<uint8_t, 16>& a) { return std::vector<int>(a.begin(), a.end()); }
static Bytes sliceAsicRam(const Bytes& ram, int a, int b) { return Bytes(ram.begin() + a, ram.begin() + b); }

GX4000::GX4000() {
    memory = new GXMemory(64); plusHardware = true; hasFdc = false;
    memory->setTimingProvider([this](int offset, bool, int) { advanceHardwareToInstructionOffset(offset); });
    asic = new PlusAsic(memory);
    gateArray = new GateArray(memory, asic, true);
    m4 = new M4Board(this);
    v9990 = new V9990();
    opl4 = new Opl4Card();
    playcity = new PlayCity();
    serial = new AmstradSerial();
    speech = new SpeechSynth();
    symbifaceMouse = new SymbifaceMouse();
    sf2Rtc = new Symbiface2Rtc();
    sf3 = new Symbiface3();
    ide = new SymbifaceIde();
    multiface = new Multiface2();
    memory->setUpperRomReadHandler([this](int address, int rom) { return m4 ? m4->readMemory(address, rom) : -1; });
    classicMonitorCharacter = 0; classicMonitorLine = 0; classicMonitorFrame = 0;
    classicMonitorLineCharacters = 64; classicMonitorHsyncLimit = 64 * 16; classicMonitorHsyncCount = 0;
    classicMonitorHsyncMatch = 0;
    gateArrayHsyncPending = 0;
    monitorRenderer = new CtmMonitor();
    // ACCC §16.2.2: the GATE ARRAY divides CK16 down for the CRTC. The monitor is on
    // the fast side of that divider, so it is clocked from here -- on a Plus as on a
    // CPC: the ASIC is that machine's GATE ARRAY, and its C-SYNC goes to a monitor too.
    gateArray->pixelSink = [](void* machine) { static_cast<GX4000*>(machine)->advanceClassicMonitorCharacter(); };
    gateArray->pixelSinkContext = this;
    gateArray->onTraceRaise = [this](const char* why, bool raised, int r52) {
        std::fprintf(stderr, "f%-6d %-8s %-8s C0=%2d C4=%3d C9=%d  R52=%2d\n",
                     crtc ? crtc->frame : -1, raised ? "RAISE" : "(no int)", why,
                     crtc ? crtc->horizontal : -1, crtc ? crtc->vertical : -1,
                     crtc ? crtc->raster : -1, r52);
    };
    classicDisplayOriginY = 40 + 2;   // V26 delay (ACCC §16.1)
    spritePatternSnapshot.clear(); spritePatternRevision = -1;

    Crtc6845Options o;
    o.onHsync = [this]() {
        // The mode latch used to be taken here, at the end of the HSYNC-CRTC. ACCC
        // §9.3.3's chronogram puts it at the end of HSYNC-GA instead (6 µsec, +1 for
        // the ASIC's lag), which is what GateArray::onCharacter now does on both
        // machines — so there is no second, later latch.
        // ACCC §27.6 (p.288): the interrupt's deadline is one µsec later on CRTC 3 and
        // 4 than on 0, 1 and 2, because the ASIC passes the end of HSYNC on late. Only
        // the GATE ARRAY's own reaction (R52, and so the interrupt) is deferred here;
        // the monitor's line timing carries that delay separately in the renderer.
        // ACCC §9.3.4.1: an R3.JIT ends the HSYNC early and decides the pending graphic
        // mode update with it -- cancelled below the 3rd µsec, taken from the 3rd.
        // crtc->hsyncCounter is the 1-based microsecond it stopped on.
        if (crtc->hsyncEndedJit) gateArray->onHsyncStoppedByJit(crtc->hsyncCounter);
        if (crtc->hsyncEndedByBlockWrite) gateArray->onHsyncCutShort(crtc->hsyncCounter);
        int hsyncDelay = crtc->behaviour->hsyncDisplayDelayCharacters();
        // onCharacter() runs later in this SAME character, so the countdown has to be
        // one longer than the lag for the Gate Array to see the HSYNC end a whole
        // microsecond late. ACCC §27.6 measures that microsecond directly: the interrupt
        // lands 16/10/3/18 usec after C0vs=R2 on CRTC 3 and 4 where CRTC 0, 1 and 2 give
        // 15/9/2/17. With the countdown set to the lag itself it fired here and the
        // ASIC's extra usec disappeared.
        // ...but the C-HSYNC the monitor sees ends on the pin, now: the ASIC delays the
        // WHOLE HSYNC (§15.1 p.146, "over a length R3 usec"), and only the end was being
        // delayed here, which stretched every pulse by a usec (GateArray::cHsyncFallArmed).
        if (hsyncDelay > 0) { gateArray->armCHsyncFall(hsyncDelay); gateArrayHsyncPending = hsyncDelay + 1; }
        else gateArray->onHsync();
        crtc->gateArrayBlanking = gateArray->blanking();   // ACCC §16.2.1
        // ACCC §16.1: C-SYNC reaches the monitor when V26 reaches 2, two HSYNCs
        // after the CRTC raised VSYNC.
        if (gateArray->consumeMonitorSyncStart()) {
            dbgVsyncCount++;
            // The legacy monitor is NOT told about this: it separates the vertical
            // sync out of the C-SYNC stream itself, with an integrator, exactly as
            // the CTM's LA7800 does. Only the beam renderer still takes the event.
            // ACCC §16.6: how high the beam retraces depends on where in the line the
            // C-VSYNC arrived, so the position travels with the event.
            monitorSyncCharacter = crtc->horizontal;
            monitorSyncLineLength = crtc->registers[0];
            if (onBeamVsync) onBeamVsync();
        }
    };
    o.onHsyncStart = [this]() {
        int rasterLine = ((crtc->vertical & 0x3f) << 3) | (crtc->raster & 7);
        int hsyncWidth = (crtc->registers[3] & 0x0f) ? (crtc->registers[3] & 0x0f) : 16;
        // ACCC §9.3.1: the graphic-mode latch belongs to the GATE ARRAY/ASIC and is
        // driven by HSYNC-GA on every machine ("for all CPCs without exception,
        // CRTC's 0 to 4"), so it is armed here for the Plus as well as the classic.
        // ACCC §15.3.4: on a C3l overflow CRTC 1 puts an INVISIBLE END out first and only
        // then the restart. Without the end, resetting H06 merely stretches the pulse the
        // GATE ARRAY is already sending and no second monitor sync ever leaves the chip.
        if (crtc->hsyncRestartFromOverflow) gateArray->endCHsync();
        gateArray->onHsyncStart(crtc->effectiveHsyncWidth(),
            crtc->behaviour->hsyncDisplayDelayCharacters(),
            crtc->hsyncArmedBetweenCharacters,
            crtc->behaviour->hsyncFallPhase16());   // ACCC §14.4 p.134, per chip
        // The monitor is NOT wired to the CRTC. Its horizontal reference is the GATE
        // ARRAY's C-HSYNC, which that chip times itself from H06 (ACCC §16.2.3), so
        // the retrace is raised from onCharacter below when SIG_GA_HSYNC rises --
        // not here, where only the CRTC's pin has moved.
        if (plusHardware != false) asic->onHsyncStart(ay, rasterLine, hsyncWidth);
    };
    o.onHsyncCancelled = [this]() {
        gateArray->cancelHsyncStart();
        crtc->gateArrayBlanking = gateArray->blanking();
    };
    o.onCharacterStart = [this]() {
        captureRasterCharacter();
        if (onBeamCharacter && plusHardware == false) onBeamCharacter();
    };
    o.onCharacter = [this]() {
        if (gateArrayHsyncPending > 0 && --gateArrayHsyncPending == 0) {
            gateArray->onHsync();                         // ACCC §27.6.5 (ASIC lag)
            crtc->gateArrayBlanking = gateArray->blanking();
        }
        gateArray->onCrtcVsyncPin(crtc->vsync);          // ACCC §16.2.3 / p.164 (CRTC 3/4)
        gateArray->onCharacter();
        // The 16 Pixel-M2 just clocked the monitor; what the machine mirrors of it is read
        // at character boundaries only, so it is copied once per character, not per pixel.
        syncClassicMonitorTiming();
        // ACCC §16.2.3: SIG_GA_HSYNC rises H06 characters after the CRTC's HSYNC did.
        // The legacy monitor reads the C-SYNC pin itself in advanceClassicMonitorCharacter
        // below; the beam renderer still wants the edge as an event.
        gateArray->consumeCHsyncStart();              // the legacy monitor reads the level
        // ACCC §14.3/§14.4: the sweep starts where C-HSYNC ENDS, which is why a pulse
        // that stops early moves the picture right. Homing the beam on the pulse's
        // START meant correcting for a width the monitor cannot know yet -- one line
        // stale, and measurably worse (module A CRTC 0 2536.2 -> 2558.4).
        if (plusHardware == false && gateArray->consumeCHsyncEnd()) {
            if (onBeamHsync) onBeamHsync();
        }
        if (plusHardware != false) asic->onCharacter();
        ay->advanceTStates(4); dac->advanceTStates(4); ppi->advanceTStates(4); fdc->advanceCycles(1); tape->advanceCycles(1);
        if (playcity->enabled) { playcity->advanceMicrosecond(); playcity->cursorPin(crtc->cursorOutput()); }
        speech->advanceMicrosecond();
        if (serial->enabled) serial->advanceMicrosecond();
    };
    o.onVsyncStart = [this]() {
        gateArray->onVsyncStart(crtc->hsync,
            crtc->behaviour->cVsyncNeedsCrtcVsync());       // ACCC §16.2.3: the pin gates it on CRTC 3/4
        // ACCC §16.1: the monitor is not driven from the CRTC's VSYNC directly — the
        // GATE ARRAY's V26 counter delays the composite C-SYNC by two HSYNCs. The
        // monitor is therefore synced from onHsync above, not from here.
    };
    // Neither the CRTC's C4 wrap nor its C0 wrap is a picture or a line: the monitor
    // decides both from the C-SYNC it is sent (advanceClassicMonitorCharacter), on the
    // Plus as on the CPC. The Plus used to file one record per CRTC line and close its
    // picture at the CRTC frame after a VSYNC, which an R0 rupture (sixteen CRTC lines
    // in one monitor line, SHAKER A3) or a suppressed VSYNC (Alcon 2020) tore apart.
    o.onScanline = [this](int) {
        if (plusHardware) {
            int scanline = ((crtc->vertical & 0x3f) << 3) | (crtc->raster & 7);
            if (asic->rasterInterruptEnabled() && scanline == asic->rasterInterruptLine) {
                int r2 = crtc->registers[2];
                int width = (crtc->registers[3] & 0x0f) ? (crtc->registers[3] & 0x0f) : 16;
                int r0 = crtc->registers[0];
                if (r2 + width >= r0 && !asic->rasterInterruptPending) {
                    asic->raiseRasterInterrupt(cpu);
                    asic->rasterInterruptDelay = 0;
                    asic->suppressPriDelayLine = scanline;
                }
            }
        }
    };
    o.onSelect = [this](int value) { if (plusHardware) asic->advanceUnlock(value); };
    o.onLightgunBeam = [this](int, int) {
        std::string mode = keyboard ? keyboard->lightgunMode : "";
        if (!crtc->lightgunEnabled) return;
        if (mode != "gunstick" && mode != "westphaser" && mode != "trojan") return;
        Beam beam = lightgunBeamCanvasPosition();
        double aimX = crtc->lightgunX, aimY = crtc->lightgunY;
        bool aimValid = std::isfinite(aimX) && std::isfinite(aimY)
            && aimX >= 0 && aimX < crtc->lightgunViewWidth
            && aimY >= 0 && aimY < crtc->lightgunViewHeight;
        if (mode == "trojan") {
            // A light pen fires when the beam passes under it, and the CRTC then
            // latches its own MA (UM6845R data sheet, LPEN). The address used to be
            // reconstructed from R1, R2, R7 and R12/R13 by a formula with fitted
            // constants, which is the geometry the chip already holds.
            if (!aimValid || !crtc->lightgunTriggered || crtc->lightgunLatched) return;
            if (!gunstickBeamAtAim(beam.x, beam.y, aimX, aimY, 16)) return;
            crtc->strobeLightPen();
            crtc->lightgunLatched = true;
            return;
        }
        if (!aimValid) { keyboard->setLightgunBeamHit(false); return; }
        if (mode == "westphaser") {
            keyboard->setLightgunBeamHit(westphaserBeamHit(beam.x, beam.y, aimX, aimY));
        } else {
            updateGunstickSensor(beam, aimX, aimY);
        }
    };
    o.getHorizontalScroll = [this]() { return asic->verticalScroll; };
    o.getSplit = [this]() -> std::optional<CrtcSplit> { return asic->rasterSplitLine ? std::optional<CrtcSplit>(CrtcSplit{ asic->rasterSplitLine, asic->splitAddress }) : std::nullopt; };
    crtc = new CRTC6845(o);

    asic->onSoftScrollChange = [this]() { crtc->vlc = crtc->videoRaster(asic->verticalScroll); };
    keyboard = new KeyboardMatrix();
    keyboard->onLightgunRead = [this](const std::string& mode) {
        if (mode == "trojan") crtc->refreshTrojanLightgun();
        else if (mode == "gunstick") refreshGunstickRenderedSample();
    };
    ay = new AY38912(keyboard);
    dac = new CpcDac();
    tape = new CPCTapeDrive(ay);
    ppi = new PPI8255(ay, [this]() { return crtc->vsyncPinActive(); }, tape);
    dac->onPrinterStrobe = [this](int) { ppi->onPrinterStrobe(dac->mode == "matrix" ? 0 : 4000); };
    gamepad = new GamepadController(keyboard);
    fdc = new UPD765A();
    cpuIoAccessed = false;

    Z80Ports ports;
    ports.write = [this](int port, int value, int timingOffset, const std::string& kind) {
        advanceHardwareToInstructionOffset(cpcIoEffectOffset(timingOffset, kind, port));
        cpuIoAccessed = true;
        // ACCC §13.3 note 3: OUTI/OUTD reach the CRTC differently from OUT(C),r8.
        crtc->lastWriteBlockIo = kind == "block-out";
        writePort(port, value);
    };
    // /WAIT on the expansion bus: the GFX9000's V9990 holds an access while it is busy
    // (its pin description, p.5), and lets it go when it is done. The Gate Array lets the
    // Z80 go only on its microsecond, so the hold is whole microseconds.
    ports.ioWait = [this](int port, int timingOffset, bool write) -> int {
        if (!v9990->handlesPort(port)) return 0;
        const double us = v9990->ioWait(port, write, machineCycles + timingOffset);
        return us > 0 ? (int)std::ceil(us - 1e-9) * 4 : 0;
    };
    ports.read = [this](int port, int timingOffset, const std::string& kind) {
        advanceHardwareToInstructionOffset(cpcIoEffectOffset(timingOffset, kind, port));
        cpuIoAccessed = true;
        int value = readPort(port);
        // ACCC §4.4 (p.25): "the CRTCs are not connected to the Z80A's RD and WR
        // pins, so there is no detection of the I/O direction. Consequently, if a
        // read instruction is used on a write register of the CRTC, then a data is
        // sent to the CRTC ... with the IN A,(n) instruction, it is possible to send
        // the contents of A to the An port, just as it can be done with the OUT
        // (n),A instruction." The compendium's own example is
        //     LD A,%00011001 : IN A,(#FF)   ; sends #19 to the selected CRTC register
        // Port bits 9-8 pick the CRTC function; 0 and 1 are its write ports. For
        // IN A,(n) the byte on the bus is A, which is the port's high byte.
        if ((port & 0x4000) == 0 && (((unsigned)port >> 8) & 3) <= 1 && kind == "in-n") {
            crtc->lastWriteBlockIo = false;
            crtc->write(port, ((unsigned)port >> 8) & 0xff);
        }
        return value;
    };
    ports.im0Address = [](int) { return 0x0038; };
    ports.acknowledge = [this]() { acknowledgeInterrupt(); };
    // ACCC §8 DISPLAY, Z80A & GATE ARRAY (p.44-45): a Z80A write into video RAM is
    // seen by the GATE ARRAY from the microsecond the write actually lands in, not
    // from the start of the instruction. §8.1's LD (HL),reg8 spanning C0vs 00-01 shows
    // the old byte at video pointers 0 and 1 and the new one at 2 and 3 — the two
    // bytes read in the same microsecond as the write. "The RAM reading by the GATE
    // ARRAY/ASIC for data modified by the Z80A is the same for all CPC's."
    // So the display hardware is advanced to the write's own microsecond before the
    // byte changes; rounding DOWN, because the character of that microsecond has not
    // been read out yet when the write occurs.
    memory->setTimingProvider([this](int offset, bool write, int) {
        if (write) advanceHardwareToInstructionOffset(offset & ~3);
    });
    cpu = new Z80(memory, ports);
    multiface->attach(memory, cpu);
    cpu->onLowM1 = [this](int address) { if (multiface->enabled) multiface->opcodeFetch(address); };
    // The GATE ARRAY stretches every M-cycle out to a whole microsecond, so a bus
    // access starts on a microsecond boundary and never inside one.
    cpu->alignBusToMicroseconds = true;
    gateArray->attachCpu(cpu);
    gateArray->onInterruptReset = [this]() {
        cpu->pendingInterrupt = -1;
        gateArray->interruptCounter = 0;
        // ACCC §27 (p.284): "If the request to reset R52 counter to 0 is made via the
        // RMR function of the Gate Array and this request takes place on the last
        // µsecond of the HSYNC (C0=R2+R3-1), then the reset to 0 has priority over
        // incrementation. If the request comes 1 µsecond before the end of the HSYNC
        // (C0=R2+R3-2), then R52 is zeroed, then incremented on R2+R3-1." The second
        // case needs nothing: the increment at the HSYNC end simply follows.
        int hsyncEnd = crtc->registers[2] + crtc->effectiveHsyncWidth();
        if (crtc->hsync && crtc->horizontal == ((hsyncEnd - 1) & 0xff))
            gateArray->suppressNextR52Increment = true;
        asic->clearGateArrayInterrupt();
    };
    asic->onAsicInterrupt = [this](int vector) { cpu->requestInterrupt(vector); };
    asic->hasPendingInterrupt = [this]() { return cpu->pendingInterrupt != -1; };
    asic->clearPendingInterrupt = [this]() { cpu->pendingInterrupt = -1; };
    crtc->getRasterFrame = [this]() -> const std::vector<std::shared_ptr<RasterLine>>& { return rasterFrame; };
    crtc->getPreviousRasterFrame = [this]() -> const std::vector<std::shared_ptr<RasterLine>>& { return previousRasterFrame; };
    crtc->getFrameRegisters = [this]() { return videoFrameRegisters; };
    crtc->usesPhysicalRasterFrame = []() { return true; };   // the monitor's frame, on every machine
    crtc->getPhysicalFrameOriginY = [this]() { return classicDisplayOriginY; };
    videoFrameRegisters = crtc->registers; videoCaptureRegisters = crtc->registers;
    classicFrameOvershoot = 0;
}

GX4000::~GX4000() {
    delete cpu; delete fdc; delete gamepad; delete ppi; delete tape; delete dac; delete ay; delete keyboard;
    delete crtc; delete monitorRenderer; delete sf3; delete ide; delete multiface; delete sf2Rtc; delete symbifaceMouse; delete v9990; delete opl4; delete playcity; delete serial; delete speech;
    delete m4; delete gateArray; delete asic; delete memory;
}

void GX4000::applyGunstickProbeBrightness(bool bright) {
    if (keyboard) keyboard->setLightgunBeamHit(bright);
}
void GX4000::refreshGunstickRenderedSample() {
    if (!keyboard || keyboard->lightgunMode != "gunstick" || !crtc || !crtc->lightgunEnabled
        || (!lightgunRasterSampler && !lightgunPixelSampler)) return;
    double aimX = crtc->lightgunX, aimY = crtc->lightgunY;
    if (!std::isfinite(aimX) || !std::isfinite(aimY)
        || aimX < 0 || aimX >= crtc->lightgunViewWidth
        || aimY < 0 || aimY >= crtc->lightgunViewHeight) {
        keyboard->setLightgunBeamHit(false);
        return;
    }
    int sampleX = (int)std::floor(aimX);
    if (lightgunRasterSampler) {
        std::optional<int> first = lightgunRasterSampler(sampleX, (int)aimY);
        std::optional<int> second = lightgunRasterSampler(sampleX ^ 1, (int)aimY);
        if (first.has_value() || second.has_value()) {
            applyGunstickProbeBrightness((first.value_or(0) != 0) || (second.value_or(0) != 0));
            return;
        }
    }
    applyGunstickProbeBrightness(isLightgunPixelBright(sampleX, aimY)
        || isLightgunPixelBright(sampleX ^ 1, aimY));
}
void GX4000::updateGunstickSensor(Beam beam, double aimX, double aimY) {
    if (!gunstickBeamAtAim(beam.x, beam.y, aimX, aimY)) return;
    int sampleX = (int)std::floor(aimX);
    bool bright = isLightgunPixelBright(sampleX, aimY) || isLightgunPixelBright(sampleX ^ 1, aimY);
    applyGunstickProbeBrightness(bright);
}
void GX4000::completeRasterFrame() {
    dbgFrameCount++;
    { int disp = 0; for (auto& l : rasterCapture) if (l && l->vDisplay) { for (uint8_t b : l->displayEnabled) if (b) { disp++; break; } } if (disp == 0) dbgBlankFrames++; }
    previousRasterFrame = rasterFrame;
    rasterFrame = rasterCapture; rasterCapture.clear();
    videoFrameRegisters = videoCaptureRegisters;
    videoCaptureRegisters = crtc->registers;
}
// ---- a re-lock moves the LINE BOUNDARY, so characters move across it ------------------
//
// A monitor line's record holds what the GATE ARRAY put out during ONE SWEEP, filed by
// the sweep's character counter. When CtmMonitor re-locks onto a sync that arrived far
// from where the free-running sweep expected it, the real sweep would have flown back AT
// that sync -- so the boundary between this record and the one above it was in the wrong
// place, and the characters between the two positions were filed on the wrong side.
//
// Moving the record sideways (monitorLineOffset) places everything it holds, but it cannot
// bring back a character that went into the NEIGHBOURING record. Pinball Dreams showed it:
// on the frames where its rupture line is 20 characters long the free-running flyback falls
// one character after the table line starts, so that line's C0=0 was filed as the last slot
// of the line above -- drawn at x=815, off the screen -- and the table's first line lost its
// first character. These move the characters across and keep the ink they were shown in.
static RasterSegment relockStateAt(const RasterLine& line, int slot) {
    const RasterSegment* last = nullptr;
    for (const auto& seg : line.segments) if (seg.character <= slot) last = &seg;
    RasterSegment out;
    if (last) out = *last;
    else {
        out.mode = line.mode; out.locked = line.locked;
        out.gaPalette = line.gaPalette; out.palette = line.palette;
        out.horizontalScroll = line.horizontalScroll; out.verticalScroll = line.verticalScroll;
        out.extendBorder = line.extendBorder;
        out.spriteAttributes = line.spriteAttributes; out.spriteMagnification = line.spriteMagnification;
    }
    out.modeBefore = out.mode; out.modeSwitchPixel = -1;   // a boundary, not a mid-byte split
    return out;
}
static bool relockSameState(const RasterSegment& a, const RasterSegment& b) {
    return a.mode == b.mode && a.locked == b.locked && a.gaPalette == b.gaPalette && a.palette == b.palette;
}
static void relockMoveSlot(const RasterLine& from, int fs, RasterLine& to, int ts) {
    to.videoBytes[ts * 2] = from.videoBytes[fs * 2]; to.videoBytes[ts * 2 + 1] = from.videoBytes[fs * 2 + 1];
    to.displayEnabled[ts] = from.displayEnabled[fs]; to.horizontalCounters[ts] = from.horizontalCounters[fs];
    to.hsyncBlackFrom[ts] = from.hsyncBlackFrom[fs]; to.hsyncBlackTo[ts] = from.hsyncBlackTo[fs];
}
static void relockClearSlot(RasterLine& line, int slot) {
    line.videoBytes[slot * 2] = line.videoBytes[slot * 2 + 1] = 0;
    line.displayEnabled[slot] = 0; line.horizontalCounters[slot] = 0;
    line.hsyncBlackFrom[slot] = 16; line.hsyncBlackTo[slot] = 16;
}
// chars > 0: the sweep should have started `chars` characters EARLIER (the sync came
// early), so the last `chars` characters of `prev` belong at the front of `cur`.
// chars < 0: it should have started later, so the first -chars of `cur` belong to `prev`.
static void relockRefile(RasterLine& prev, RasterLine& cur, int chars) {
    const int SLOTS = (int)cur.displayEnabled.size();
    if (chars > 0) {
        const int pc = prev.capturedCharacters;
        const int n = std::min(chars, pc);
        const RasterSegment curBase = relockStateAt(cur, -1);
        const RasterSegment carried = relockStateAt(prev, pc - n);
        for (int slot = SLOTS - 1; slot >= chars; slot -= 1) relockMoveSlot(cur, slot - chars, cur, slot);
        for (int slot = 0; slot < chars; slot += 1) relockClearSlot(cur, slot);
        for (int k = 0; k < n; k += 1) {
            relockMoveSlot(prev, pc - n + k, cur, chars - n + k);
            relockClearSlot(prev, pc - n + k);
        }
        std::vector<RasterSegment> front, kept;
        for (const auto& seg : prev.segments) {
            if (seg.character > pc - n) { RasterSegment m = seg; m.character = seg.character - (pc - n) + (chars - n); front.push_back(m); }
            else kept.push_back(seg);
        }
        prev.segments = kept;
        std::vector<RasterSegment> merged;
        if (!relockSameState(carried, curBase)) { RasterSegment c = carried; c.character = chars - n; merged.push_back(c); }
        for (auto& seg : front) merged.push_back(seg);
        const RasterSegment endOfMoved = merged.empty() ? curBase : merged.back();
        if (!relockSameState(endOfMoved, curBase)) { RasterSegment b = curBase; b.character = chars; merged.push_back(b); }
        for (auto seg : cur.segments) { seg.character += chars; merged.push_back(seg); }
        cur.segments = merged;
        prev.capturedCharacters = pc - n;
        cur.capturedCharacters = std::min(SLOTS, cur.capturedCharacters + chars);
    } else if (chars < 0) {
        const int pc = prev.capturedCharacters;
        const int m = std::min({ -chars, cur.capturedCharacters, SLOTS - pc });
        if (m <= 0) return;
        const RasterSegment prevEnd = relockStateAt(prev, pc);
        const RasterSegment curBase = relockStateAt(cur, -1);
        const RasterSegment curAfter = relockStateAt(cur, m);
        for (int k = 0; k < m; k += 1) relockMoveSlot(cur, k, prev, pc + k);
        for (int slot = 0; slot + m < SLOTS; slot += 1) relockMoveSlot(cur, slot + m, cur, slot);
        for (int slot = SLOTS - m; slot < SLOTS; slot += 1) relockClearSlot(cur, slot);
        if (!relockSameState(prevEnd, curBase)) { RasterSegment b = curBase; b.character = pc; prev.segments.push_back(b); }
        std::vector<RasterSegment> rest;
        for (const auto& seg : cur.segments) {
            if (seg.character < m) { RasterSegment moved = seg; moved.character = pc + seg.character; prev.segments.push_back(moved); }
            else { RasterSegment shifted = seg; shifted.character -= m; rest.push_back(shifted); }
        }
        if (!relockSameState(curAfter, curBase)) {
            RasterSegment first = curAfter; first.character = 0;
            if (rest.empty() || rest.front().character != 0) rest.insert(rest.begin(), first);
        }
        cur.segments = rest;
        prev.capturedCharacters = pc + m;
        cur.capturedCharacters -= m;
    }
    if (!cur.horizontalCounters.empty()) cur.horizontalCounterAtMonitorLine = cur.horizontalCounters[0];
}

void GX4000::advanceClassicMonitorCharacter() {
    // The monitor's one input: the GATE ARRAY's composite C-SYNC on pin 4 of the DIN6.
    // It used to be handed R0, R3, R4 and R12 instead, none of which a CRT can see.
    // Raised once per Pixel-M2 from the GATE ARRAY's own clock, because the C-SYNC
    // edges it has to see do not land on character boundaries.
    bool lineComplete = monitorRenderer->clockPixel(gateArray->csyncActive());
    // The monitor re-locked part-way into the line it is sweeping (see lineSnap16): the
    // line already filed under this monitor line was placed from the old phase, so it
    // moves by the snap. Pinball Dreams' first table line, after the scoreboard's short
    // R0=45 line puts ONE sync three characters early, was drawn three characters left.
    if (monitorRenderer->decisionSeq != seenDecisionSeq) {
        seenDecisionSeq = monitorRenderer->decisionSeq;
        const int at = crtc->scanlineInFrame;
        if (at >= 0 && at < (int)rasterCapture.size() && rasterCapture[at]) {
            RasterLine& l = *rasterCapture[at];
            l.monitorDecisionBranch = monitorRenderer->decisionBranch;
            l.monitorDecisionPhase = monitorRenderer->decisionPhase;
            l.monitorDecisionMove = monitorRenderer->decisionMove;
            l.monitorDecisionTips = monitorRenderer->decisionTips;
            l.monitorDecisionPull = monitorRenderer->decisionPull;
            l.monitorDecisionWidth = monitorRenderer->decisionWidth;
            l.monitorDecisionSlow = monitorRenderer->decisionSlow;
        }
    }
    if (lineComplete) { appliedLineSnap16 = 0; lineSlotShift = 0; }
    else if (monitorRenderer->lineSnap16 != appliedLineSnap16) {
        const int delta = monitorRenderer->lineSnap16 - appliedLineSnap16;
        const int at = crtc->scanlineInFrame;
        const bool have = at >= 0 && at < (int)rasterCapture.size() && rasterCapture[at];
        const bool havePrev = have && at >= 1 && rasterCapture[at - 1];
        if (havePrev) {
            // Whole characters cross the boundary; only the sub-character remainder is an
            // offset. The rest of this sweep line files `chars` further along, so what is
            // still to come lands after what was moved in front of it.
            const int chars = delta / 16;
            relockRefile(*rasterCapture[at - 1], *rasterCapture[at], chars);
            rasterCapture[at]->monitorLineOffset += delta - chars * 16;
            lineSlotShift += chars;
        } else if (have) rasterCapture[at]->monitorLineOffset += delta;
        if (std::getenv("CPCSE_TRACE_SNAP"))
            std::fprintf(stderr, "SNAP line %d delta %+d (record %s, capture size %zu, C0 %d)\n",
                         at, delta, have ? "amended" : "NOT YET CREATED",
                         rasterCapture.size(), crtc->horizontal);
        appliedLineSnap16 = monitorRenderer->lineSnap16;
    }
    if (!lineComplete) return;
    classicMonitorLine += 1;
    bool monitorEdge = monitorRenderer->consumeVerticalFrameEdge();
    // CPCSE_TRACE_ROLL=1 logs every frame the monitor has to CUT ITSELF, because no
    // vertical sync arrived within 380 lines. That is precisely a rolling picture, and
    // it is the event "vsync/frame = 0.815" is counting -- so this says which CRTC state
    // produced it, which counting VSYNCs after the fact never can.
    if (!monitorEdge && classicMonitorLine >= 380) {
        static long budget = -2;
        static long from = 0;
        if (budget == -2) {
            const char* want = std::getenv("CPCSE_TRACE_ROLL");
            budget = want ? (std::getenv("CPCSE_TRACE_ROLL_MAX")
                             ? std::atol(std::getenv("CPCSE_TRACE_ROLL_MAX")) : 200) : -1;
            const char* f = std::getenv("CPCSE_TRACE_ROLL_FROM");
            from = f ? std::atol(f) : 0;
        }
        if (budget > 0 && crtc->frame >= from) {
            std::fprintf(stderr, "f%-6d MONITOR TIMED OUT at 380 lines  C0=%2d C4=%3d C9=%d "
                                 "R4=%3d R7=%3d R9=%2d R5=%2d\n",
                         crtc->frame, crtc->horizontal, crtc->vertical, crtc->raster,
                         crtc->registers[4] & 0x7f, crtc->registers[7] & 0x7f,
                         crtc->registers[9] & 0x1f, crtc->registers[5] & 0x1f);
            budget -= 1;
        }
    }
    // CPCSE_TRACE_ROLL=1 also reports the frame that actually rolls: a displayed frame in
    // which the CRTC never raised VSYNC at all. The monitor then free-runs to its own
    // MONITOR_VSYNC_MAX and cuts a 353-line frame, which is the roll on screen. Counting
    // VSYNCs after the fact says how often; this says with what registers.
    if (monitorEdge || classicMonitorLine >= 380) {
        static long rollBudget = -2, rollFrom = 0, lastStarts = -1;
        if (rollBudget == -2) {
            const char* want = std::getenv("CPCSE_TRACE_ROLL");
            rollBudget = want ? (std::getenv("CPCSE_TRACE_ROLL_MAX")
                                 ? std::atol(std::getenv("CPCSE_TRACE_ROLL_MAX")) : 200) : -1;
            const char* f = std::getenv("CPCSE_TRACE_ROLL_FROM");
            rollFrom = f ? std::atol(f) : 0;
        }
        if (rollBudget > 0 && lastStarts == crtc->dbgCrtcVsyncStarts
            && classicMonitorFrame >= rollFrom) {
            std::fprintf(stderr, "displayed frame %d ROLLED (no CRTC VSYNC)  crtcFrame=%d "
                                 "lines=%d C0=%2d C4=%3d C9=%d R4=%3d R7=%3d R9=%2d R5=%2d\n",
                         classicMonitorFrame, crtc->frame, classicMonitorLine,
                         crtc->horizontal, crtc->vertical, crtc->raster,
                         crtc->registers[4] & 0x7f, crtc->registers[7] & 0x7f,
                         crtc->registers[9] & 0x1f, crtc->registers[5] & 0x1f);
            rollBudget -= 1;
        }
        lastStarts = crtc->dbgCrtcVsyncStarts;
        // Where the capture's line 0 is set, against the CRTC -- for a picture that sits
        // at the wrong height although the C-SYNC it was built from is ordinary.
        if (traceMonitorEdgeBudget > 0) {
            std::fprintf(stderr, "MONITOR EDGE frame %d after %d lines  crtc f%d C0=%2d C4=%3d C9=%2d "
                                 "vsync=%d edge=%d  sinceVsync=%d limit=%d\n",
                         classicMonitorFrame, classicMonitorLine, crtc->frame, crtc->horizontal,
                         crtc->vertical, crtc->raster, crtc->vsync ? 1 : 0, monitorEdge ? 1 : 0,
                         monitorRenderer->vsyncCount / 2, monitorRenderer->vsyncLimit / 2);
            traceMonitorEdgeBudget -= 1;
        }
        classicMonitorLine = 0;
        if ((int)rasterCapture.size() > 380) rasterCapture.resize(380);
        completeRasterFrame();
        if (onBeamFrame) onBeamFrame();
        crtc->scanlineInFrame = 0;
        classicMonitorFrame += 1;
    } else {
        crtc->scanlineInFrame = classicMonitorLine;
    }
}
void GX4000::syncClassicMonitorTiming() {
    classicMonitorCharacter = monitorRenderer->character;
    classicMonitorHsyncLimit = monitorRenderer->hsyncLimit;
    classicMonitorHsyncCount = monitorRenderer->hsyncCount;
    classicMonitorHsyncMatch = monitorRenderer->hsyncMatch;
    classicMonitorLineOffset = monitorRenderer->lineOffset;
    classicDisplayOriginY = 40 + 2 + monitorRenderer->verticalOffset;   // +2: V26 delay (§16.1)
    classicMonitorLineCharacters = (unsigned)classicMonitorHsyncLimit >> 4;
}
int GX4000::rasterByteAddress(int ma, int raster) {
    int doubled = (ma & 0x3fff) << 1;
    return ((doubled & 0x07fe) | ((doubled & 0x6000) << 1)) + (raster & 7) * 0x800;
}
int GX4000::rasterByteAddress(int ma) { return rasterByteAddress(ma, crtc->vlc); }
const Bytes& GX4000::snapshotSpritePatterns() {
    if (spritePatternRevision != asic->spriteDataRevision) {
        spritePatternSnapshot = Bytes(memory->asicRam.begin(), memory->asicRam.begin() + 0x1000);
        spritePatternRevision = asic->spriteDataRevision;
    }
    return spritePatternSnapshot;
}
GX4000::Beam GX4000::lightgunDisplayOrigin() {
    const std::array<uint8_t, 18>& r = crtc->registers;
    int x = (0x32 - r[2]) * 16;
    int syncWidth = r[3] & 0x0f;
    if (syncWidth < 6) x += syncWidth * 8;
    x += (r[0] - 0x3f) * 16;
    int skew = (unsigned)r[8] >> 4 & 3;
    if (skew < 3) x += skew * 16;
    int y = (r[4] - r[7] - 3) * 8;
    if (y & 0x100) y |= ~0x1ff;
    return { (double)x, (double)y };
}
// ACCC 15.1 (p.146): THE HORIZONTAL CALIBRATION BELONGS TO THE PAIRING. The ASIC's
// C-HSYNC leaves the GATE ARRAY a microsecond late ("delaying the display of the HSYNC by
// 1 usec"), and the set Amstrad shipped with those machines -- the CM14, and the CTM it
// calibrated for the CRTC 4 -- has a back porch a microsecond longer to absorb it, "so
// that the frame is centered on screen". Together they cancel, which is why every
// reference photograph of a Plus is centred. Apart, the chapter's shift appears: a CRTC 3
// or 4 on a plain CTM sits a character left, a CRTC 0 / 1 / 2 on a CM14 a character right.
//
// With no set chosen the machine has the one it came with, so the default is centred on
// every chip and this is invisible until someone mixes them.
int GX4000::monitorCalibration16() const {
    const MonitorModel* set = monitorSet;
    if (!set && monitorRenderer) set = monitorRenderer->model;
    if (!set) set = monitorModelShippedWith(plusHardware != false, crtc ? crtc->type : 0);
    return set->calibrationMicroseconds * 16;
}
// Plug a set in. Nothing else in the machine may hold this: the monitor is a chip and the
// set is what that chip IS, so it goes there and everybody reads it from there.
void GX4000::setMonitorModel(const MonitorModel* model) {
    monitorSet = model;              // nullptr: none chosen, the machine's own at every reset
    if (monitorRenderer) monitorRenderer->model = model ? model : shippedMonitorModel();
}
// ...and the one that came in the box, which is the only pairing 15.1 says is centred.
const MonitorModel* GX4000::shippedMonitorModel() const {
    return monitorModelShippedWith(plusHardware != false, crtc ? crtc->type : 0);
}
GX4000::Beam GX4000::lightgunBeamCanvasPosition() {
    return { (double)(classicMonitorCharacter * 16 + classicMonitorLineOffset - MONITOR_CROP_LEFT),
             (double)(classicMonitorLine * 2) };
}
bool GX4000::isLightgunPixelBright(double x, double y, bool liveBeam) {
    if (!liveBeam && lightgunPixelSampler) {
        std::optional<int> sampled = lightgunPixelSampler((int)x, (int)y);
        if (sampled.has_value()) return sampled.value() != 0;
    }
    x = std::floor(x); y = std::floor(y);
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    Beam origin = lightgunDisplayOrigin();
    x = std::floor(x - origin.x);
    y = std::floor(y / 2 - origin.y);
    int rasterHeight = crtc->rasterHeight();
    int activeWidth = crtc->registers[1] * 16;
    int activeHeight = crtc->registers[6] * rasterHeight;
    if (x < 0 || x >= activeWidth || y < 0 || y >= activeHeight) {
        int border = asic->color(16), br = (unsigned)border >> 16 & 255, bg = (unsigned)border >> 8 & 255, bb = border & 255;
        return br + bg + bb >= 384;
    }
    int characterRow = (int)std::floor(y / rasterHeight), raster = (int)y % rasterHeight;
    int character = (int)std::floor(x / 16), pixel = (int)x % 16;
    int ma = crtc->screenAddress() + characterRow * crtc->registers[1] + character;
    int address = rasterByteAddress(ma, raster);
    int left = memory->readVideo(address), right = memory->readVideo(address + 1);
    // ACCC §9.1: the GATE ARRAY's own decode of the byte, not a second copy of it.
    int pen = gateArray->pixelPen(gateArray->mode, pixel < 8 ? left : right, pixel % 8);
    int rgb = asic->color(pen), r = (unsigned)rgb >> 16 & 255, g = (unsigned)rgb >> 8 & 255, b = rgb & 255;
    return r + g + b >= 384;
}
void GX4000::captureRasterState() {
    int rasterHeight = crtc->rasterHeight();
    int displayLine = crtc->scanlineInFrame;
    int lineBase = rasterByteAddress(crtc->rowAddress, crtc->vlc);
    int linePage = lineBase & ~0x07ff;
    int previous2 = linePage | ((lineBase - 2) & 0x07ff);
    int previous1 = linePage | ((lineBase - 1) & 0x07ff);
    const Bytes& spritePatterns = snapshotSpritePatterns();
    bool existing = displayLine >= 0 && displayLine < (int)rasterCapture.size() && rasterCapture[displayLine];
    if (displayLine >= 0 && displayLine < 512 && !existing) {
        auto line = std::make_shared<RasterLine>();
        line->mode = gateArray->mode; line->locked = asic->locked;
        line->gaPalette = arrToVec(gateArray->gaPalette); line->palette = arrToVec16(asic->palette);
        line->screenAddress = crtc->screenAddress();
        line->lineAddress = crtc->rowAddress; line->videoRaster = crtc->vlc;
        line->verticalCounter = crtc->vertical; line->rasterCounter = crtc->raster; line->verticalAdjust = crtc->verticalAdjust;
        line->crtcType = crtc->type; line->interlaceVideo = crtc->oldInterlaceVideo();
        line->displaySkewBits = crtc->behaviour->displaySkew(*crtc);
        line->interlaceField = crtc->interlaceField;
        line->crtcFrame = crtc->frame; line->horizontalCounterAtMonitorLine = crtc->horizontal;
        line->monitorLineOffset = classicMonitorLineOffset + monitorRenderer->lineSnap16;
        line->cHsyncRiseAdvance16 = crtc->behaviour->cHsyncRiseAdvance16();
        line->sweepPhase16 = monitorRenderer->pixel & 15;
        line->monitorCalibration16 = monitorCalibration16();
        line->hsyncAtMonitorLine = crtc->hsync;
        line->vsync = crtc->vsyncPinActive();
        line->gateArrayBlank = crtc->gateArrayBlanking;   // ACCC §16.2.1's 26 black lines
        line->charactersPerLine = crtc->registers[1]; line->crtcRegisters = crtc->registers; line->vDisplay = crtc->vDisplay;
        line->displayStartCharacter = 0;   // DISPTMG skew now shows as DISPEN border gaps, not a data offset
        line->rasterHeight = rasterHeight; line->horizontalScroll = asic->horizontalScroll; line->verticalScroll = asic->verticalScroll; line->extendBorder = asic->softScrollControl;
        line->splitLine = asic->rasterSplitLine; line->splitAddress = asic->splitAddress;
        line->spriteAttributes = sliceAsicRam(memory->asicRam, 0x2000, 0x2080);
        line->spriteMagnification = arrToVec(asic->spriteMagnification);
        line->spritePatterns = spritePatterns;
        line->videoBytes = Bytes(0x200, 0); line->displayEnabled = Bytes(0x100, 0);
        line->horizontalCounters = Bytes(0x100, 0);
        line->hsyncBlackFrom = Bytes(0x100, 16); line->hsyncBlackTo = Bytes(0x100, 16);
        line->videoLookbehind = Bytes{ (uint8_t)memory->readVideo(previous2), (uint8_t)memory->readVideo(previous1) };
        line->videoRevision = asic->videoRevision; line->spriteRevision = asic->spriteRevision;
        line->spriteDataRevision = asic->spriteDataRevision;
        if (displayLine >= (int)rasterCapture.size()) rasterCapture.resize(displayLine + 1);
        rasterCapture[displayLine] = line;
    }
}
void GX4000::captureRasterCharacter() {
    // Before any early return: the HSYNC pipe must see every character or it desynchs
    // from the beam renderer's, which shifts unconditionally.
    classicHsyncPipe = (classicHsyncPipe << 1) | (crtc->hsync ? 1u : 0u);
    int rasterHeight = crtc->rasterHeight(); (void)rasterHeight;
    int displayLine = crtc->scanlineInFrame;
    if (displayLine < 0 || displayLine >= 512) return;
    std::shared_ptr<RasterLine> line = displayLine < (int)rasterCapture.size() ? rasterCapture[displayLine] : nullptr;
    if (!line) { captureRasterState(); line = displayLine < (int)rasterCapture.size() ? rasterCapture[displayLine] : nullptr; }
    if (!line) return;
    int character = classicMonitorCharacter & 0xff;
    // ...moved along by any re-lock taken earlier in this sweep line (relockRefile).
    if (lineSlotShift != 0) {
        character = classicMonitorCharacter + lineSlotShift;
        if (character < 0 || character > 0xff) return;
    }
    int address = rasterByteAddress(crtc->maRow, crtc->vlc);
    line->videoBytes[character * 2] = (uint8_t)memory->readVideo(address);
    line->videoBytes[character * 2 + 1] = (uint8_t)memory->readVideo(address + 1);
    line->horizontalCounters[character] = (uint8_t)(crtc->horizontal & 0xff);
    line->displayEnabled[character] = (uint8_t)crtc->displayOutputBytes();
    // The same question the beam renderer asks in plotBeamCharacter, asked at the same
    // moment -- onCharacterStart calls this and then that, so the pin has the same value
    // for both. The pipe models §9.3.1's per-chip HSYNC display delay.
    {
        int delay = crtc->behaviour->hsyncDisplayDelayCharacters() & 7;
        bool now = (classicHsyncPipe >> delay) & 1, before = (classicHsyncPipe >> (delay + 1)) & 1;
        int from = 16, to = 16;
        gaHsyncBlackWindow(crtc->behaviour, now, before,
                           crtc->r2WrittenThisCharacter, crtc->hsyncEndedJit,
                           (gateArray->model ? gateArray->model : gateArrayModel40010())
                               ->hsyncBlackEndLag(), crtc->hsyncCutFirstMicrosecond, from, to);
        // §15.1's lead: the black belongs on the character before the one the CRTC
        // raised HSYNC on. Stored where it is drawn, so the renderer stays a renderer.
        int at = (character - crtc->behaviour->hsyncBlackCharacterLead()) & 0xff;
        line->hsyncBlackFrom[at] = (uint8_t)from;
        line->hsyncBlackTo[at] = (uint8_t)to;
        if (character + 1 > line->capturedCharacters) line->capturedCharacters = character + 1;
    }
    if (line->videoRevision != asic->videoRevision
        || line->spriteRevision != asic->spriteRevision
        || line->spriteDataRevision != asic->spriteDataRevision) {
        line->videoRevision = asic->videoRevision;
        line->spriteRevision = asic->spriteRevision;
        line->spriteDataRevision = asic->spriteDataRevision;
        RasterSegment seg;
        seg.character = character; seg.mode = gateArray->mode; seg.locked = asic->locked;
        // ACCC §9.3.4's split byte, consumed here exactly as the beam renderer consumes
        // it, so the legacy path can decode the character a mid-line mode change lands in
        // with both modes instead of only the new one.
        seg.modeBefore = gateArray->modeBefore; seg.modeSwitchPixel = gateArray->modeSwitchPixel;
        // Only one renderer consumes the split. The beam path clears it in
        // plotBeamCharacter, so taking it here as well would leave that path with
        // nothing to split on.
        if (!onBeamCharacter) gateArray->modeSwitchPixel = -1;
        seg.gaPalette = arrToVec(gateArray->gaPalette); seg.palette = arrToVec16(asic->palette);
        seg.horizontalScroll = asic->horizontalScroll; seg.verticalScroll = asic->verticalScroll; seg.extendBorder = asic->softScrollControl;
        seg.spriteAttributes = sliceAsicRam(memory->asicRam, 0x2000, 0x2080);
        seg.spriteMagnification = arrToVec(asic->spriteMagnification);
        seg.spritePatterns = snapshotSpritePatterns();
        line->segments.push_back(seg);
    }
}
void GX4000::loadCartridge(const Cartridge& cart, LoadCartridgeOptions options) {
    memory->clearSnapshotRoms(); snapshotDebugMetadata.reset();
    int selectedRam = options.ramKiB >= 0 ? options.ramKiB : (options.ram128 ? 128 : 64);
    plusComputer = options.plusComputer;
    ramExpansion = selectedRam > 64;
    ramKiB = selectedRam;
    plusHardware = true; gateArray->setPlusHardware(true); crtc->setType(3); ppi->setPlusMode(true);
    // ACCC §9 (p.44): "ASIC 40489 (CRTC 3) is used on CPC + and GX 4000" -- the GATE ARRAY
    // is that ASIC. This boot path never said so and the chip stayed the 40010 it defaults
    // to, which is invisible until a model difference is drawn: §9.2.1's mode-2 advance,
    // which "ASIC 40489 of the CPC+ is not affected by", moved every Plus picture a pixel.
    gateArray->model = gateArrayModel40489();
    crtc->hostOwnsScanlineIndex = true;    // the monitor owns the capture index, as on a CPC
    // ACCC §9.3.4: the Pixel-M2 at which the GA switches graphic mode is a property
    // of the CRTC it is paired with, so it travels with the chip profile.
    gateArray->modeSwitchPixelInByte = crtc->behaviour->modeSwitchPixelInByte();
    memory->setRamSize(selectedRam);
    memory->loadCartridge(cart.banks); cartridge = std::make_shared<Cartridge>(cart); reset();
}
void GX4000::loadCartridge(const Cartridge& cart) { loadCartridge(cart, LoadCartridgeOptions{}); }
void GX4000::loadClassicFirmware(const LoadClassicOptions& options) {
    memory->clearSnapshotRoms(); snapshotDebugMetadata.reset();
    int selectedRam = options.ramKiB >= 0 ? options.ramKiB : (options.ram128 ? 128 : 64);
    plusComputer = false; plusHardware = false; gateArray->setPlusHardware(false); ramExpansion = selectedRam > 64; ramKiB = selectedRam; model = options.model;
    crtc->setType(options.crtcType); ppi->setPlusMode(false);
    // ACCC §9 (p.44): on a classic CPC the GATE ARRAY is its own chip and which of the
    // three is fitted is a property of the machine -- the 40010 is what most 6128's
    // carry, so it is the default. The 40226 and the 40489 are not separate chips at
    // all: that silicon IS the CRTC 4 and the CRTC 3, so there the CRTC type picks it.
    gateArray->model = options.crtcType == 3 ? gateArrayModel40489()
                     : options.crtcType == 4 ? gateArrayModel40226()
                     : gateArrayModel40010();
    gateArray->modeSwitchPixelInByte = crtc->behaviour->modeSwitchPixelInByte();   // ACCC §9.3.4
    crtc->hostOwnsScanlineIndex = true;   // the monitor owns the capture index
    memory->setRamSize(selectedRam);
    memory->loadCartridge({}); memory->setLowerRom(options.lowerRom);
    for (auto& r : memory->upperRoms) r.clear();
    memory->setUpperRom(0, options.basicRom);
    if (!options.amsdosRom.empty()) memory->setUpperRom(7, options.amsdosRom);
    cartridge = nullptr; reset();
    memory->useCartridgeUpper = false; memory->setUpperRomSelect(0);
}
void GX4000::setDacType(const std::string& type) { dac->setMode(type); }
// CPCSE_TRACE_IO=<n>: the first n I/O accesses away from the CPC's own chips (Gate Array,
// CRTC, PPI, FDC: what a program does with the expansions), from CPCSE_TRACE_IO_FROM
// T-states on. To stderr: "IO OUT ffc4 <- 05 @t" / "IO IN  fbee -> 80 @t".
static bool traceIoPort(int port) {
    static const long limit = std::getenv("CPCSE_TRACE_IO") ? std::atol(std::getenv("CPCSE_TRACE_IO")) : 0;
    static long count = 0;
    if (count >= limit) return false;
    const int high = (unsigned)port >> 8 & 0xff;
    if (high == 0x7f || (high >= 0xbc && high <= 0xbf) || (high >= 0xf4 && high <= 0xf7) || (port & 0xffff) == 0xfb7e || (port & 0xffff) == 0xfb7f) return false;
    count++;
    return true;
}
static long long traceIoFrom() {
    static const long long from = std::getenv("CPCSE_TRACE_IO_FROM") ? std::atoll(std::getenv("CPCSE_TRACE_IO_FROM")) : 0;
    return from;
}

void GX4000::writePort(int port, int value) {
    if (machineCycles >= traceIoFrom() && traceIoPort(port))
        std::fprintf(stderr, "IO OUT %04x <- %02x @%lld pc %04x\n", port & 0xffff, value & 0xff, machineCycles + hardwareCyclesAdvanced, cpu->instructionStartPc & 0xffff);
    int high = (unsigned)port >> 8 & 0xff;
    if (multiface->enabled && multiface->ioWrite(port, value)) return;   // it also notes GA/CRTC/PPI writes
    if (v9990->writePort(port, value, machineCycles + hardwareCyclesAdvanced)) return;
    if (opl4->handlesPort(port)) { opl4->writePort(port, value, machineCycles + hardwareCyclesAdvanced); return; }
    // &F8FF is the expansion bus's peripheral reset: every board on it hears it.
    if ((port & 0xffff) == 0xf8ff && playcity->enabled) playcity->reset();
    if (playcity->handlesPort(port)) { playcity->writePort(port, value); return; }
    if (serial->handlesPort(port)) { serial->writePort(port, value); return; }
    if (speech->handlesPort(port)) { speech->writePort(port, value); return; }
    if (symbifaceMouse->handlesWritePort(port)) { symbifaceMouse->writePort(port, value); return; }
    if (sf2Rtc->handlesWritePort(port)) { sf2Rtc->writePort(port, value); return; }
    if (sf3->handlesWritePort(port)) { sf3->writePort(port, value); return; }
    if (ide->handlesPort(port)) { ide->writePort(port, value); return; }
    // M4: DATAPORT &FExx takes the command, ACKPORT &FCxx starts it (M4ROM.s)
    if (m4->enabled && high == 0xfe) { m4->dataPortWrite(value); return; }
    if (m4->enabled && high == 0xfc) { m4->ack(cpu); return; }
    if ((port & 0xc000) == 0x4000) {
        // CPCSE_TRACE_GA=<budget>: every GATE ARRAY write with the CRTC position it landed
        // on -- pen selects, ink writes and RMR. An ink that shows in the wrong zone is
        // either the wrong value or the right value at the wrong character, and only the
        // sequence with C0 on it tells them apart.
        static long gaBudget = -2;
        if (gaBudget == -2) {
            const char* want = std::getenv("CPCSE_TRACE_GA");
            gaBudget = want ? std::atol(want) : -1;
        }
        if (traceGateArrayBudget > 0) { gaBudget = traceGateArrayBudget; traceGateArrayBudget = 0; }
        if (gaBudget > 0) {
            const char* what = (value & 0x80) ? ((value & 0x40) ? "RAM" : "RMR")
                                              : ((value & 0x40) ? "INK" : "PEN");
            std::fprintf(stderr, "GA %s %02X  C0=%2d C4=%3d C9=%d  pc=%04X  pens", what, value & 0xff,
                         crtc->horizontal, crtc->vertical, crtc->raster, cpu ? cpu->pc & 0xffff : 0);
            for (int p = 0; p < 17; p += 1) std::fprintf(stderr, " %02X", gateArray->gaPalette[p] & 0x1f);
            std::fprintf(stderr, "  mode=%d\n", gateArray->mode);
            gaBudget -= 1;
        }
        gateArray->write(port, value);
    }
    if ((port & 0x2000) == 0) {
        if (plusHardware != false) memory->selectUpperCartridgeFromPort(value);
        else memory->setUpperRomSelect(value);
    }
    if ((port & 0x4000) == 0 && (port & 0x0300) <= 0x0100) {
        // The PC of the instruction doing this I/O, for CPCSE_TRACE_REG. A register
        // trace says WHEN a write landed; only the program counter says WHAT CODE sent
        // it, which is the difference between guessing at a game's intent and reading it.
        crtc->tracePc = cpu ? cpu->pc : -1;
        crtc->write(port, value);
    }
    if ((port & 0x0800) == 0) ppi->write(port, value);
    // ACCC §5.1 (p.32): the CPC decodes I/O addresses only PARTIALLY — "a device is
    // affected by an Input/Output operation as soon as a few precise bits of the
    // address bus are set to 0 and/or 1", and its table gives the FDC exactly three:
    // FDC Motor is b10=0, b8=0, b7=0; FDC Status/Data is b10=0, b8=1, b7=0 with b0
    // choosing between them. Matching the full &FA7E/&FB7F addresses instead made us
    // miss the shortened forms the chapter is about ("it is therefore possible to send
    // the same value to different devices simultaneously").
    if (hasFdc != false && (port & 0x0580) == 0x0000) fdc->setMotor(value);
    if (hasFdc != false && (port & 0x0580) == 0x0100) fdc->write(port, value);
    // ACCC §20.5 (p.245): "bit 3 of R12 on CRTC 3 also corresponds to the 8th bit of the
    // data sent to the printer port" -- the Arnold V specification's "Eight-bit printer
    // support" (§2.12), where the ASIC drives the printer's D7 from "bit 3 in register 12
    // of the 6845". Everywhere else the port has seven data bits and a strobe.
    dac->printerHighBit = crtc->type == 3 ? (crtc->registers[12] >> 3 & 1) : 0;
    dac->writePort(port, value);
}
int GX4000::readPort(int port) {
    if (machineCycles >= traceIoFrom() && traceIoPort(port)) {
        const int v = readPortUntraced(port);
        std::fprintf(stderr, "IO IN  %04x -> %02x @%lld pc %04x\n", port & 0xffff, v & 0xff, machineCycles + hardwareCyclesAdvanced, cpu->instructionStartPc & 0xffff);
        return v;
    }
    return readPortUntraced(port);
}

int GX4000::readPortUntraced(int port) {
    int value = 0xff;
    int high = (unsigned)port >> 8 & 0xff;
    if (v9990->handlesPort(port)) return v9990->readPort(port, machineCycles + hardwareCyclesAdvanced);
    if (opl4->handlesPort(port)) return opl4->readPort(port, machineCycles + hardwareCyclesAdvanced);
    if (playcity->handlesPort(port)) return playcity->readPort(port);
    if (serial->handlesPort(port)) return serial->readPort(port);
    if (speech->handlesPort(port)) return speech->readPort(port);
    if (symbifaceMouse->handlesPort(port)) return symbifaceMouse->readPort(port);
    if (sf2Rtc->handlesPort(port)) return sf2Rtc->readPort(port);
    if (sf3->handlesPort(port)) return sf3->readPort(port);
    if (ide->handlesPort(port)) return ide->readPort(port);
    if (m4->enabled && high == 0xfe) return m4->dataPortRead();
    bool crtcSelected = (port & 0x4000) == 0;
    int crtcPort = (unsigned)port >> 8 & 3;
    int plusMachineId = plusComputer && ((ramKiB > 0 ? ramKiB : (int)(memory->ram.size() / 1024)) > 64) ? 0x79 : 0x78;
    if (crtcSelected && (crtcPort == 3 || crtcPort == 2)) {
        value &= crtc->read(port);
    } else if (plusHardware != false && (crtcSelected || (port & 0xc000) == 0x4000)) {
        value = plusMachineId;
        writePort(port, plusMachineId);
    }
    if (hasFdc != false && (port & 0x0580) == 0x0100) value &= fdc->read(port);   // ACCC §5.1
    if ((port & 0x0800) == 0) value &= ppi->read(port);
    value &= dac->readPort(port);
    return value;
}
int GX4000::cpcIoEffectOffset(int offset, const std::string& kind, int port) {
    // ACCC §4.4.3 ACCESS DELAYS (p.25). The I/O effect does not land at some
    // correction from the Z80's own bus cycle -- it lands on a specific MICROSECOND
    // of the instruction, and the table is per instruction and per CRTC:
    //
    //   INSTRUCTION   DURATION   CRTC 0,1,2   CRTC 3,4
    //   OUT (C),r8      4 usec     3rd usec     4th usec
    //   OUT (C),0       4 usec     3rd usec     4th usec
    //   OUT (n),A       3 usec     3rd usec     3rd usec
    //   OUTI / OUTD     5 usec     5th usec     5th usec
    //   IN r8,(C)       4 usec     4th usec     4th usec
    //   INI / IND       5 usec     4th usec     4th usec
    //   IN A,(n)        3 usec     3rd usec     3rd usec
    //
    // Two things follow that the previous per-kind corrections got wrong. CRTC 0, 1
    // and 2 are IDENTICAL -- there is no one-character write latch on the UM6845R.
    // And the ASIC's shift is specific to OUT (C),r8 / OUT (C),0: "the CRTC misses
    // the I/O on the 3rd usec and retrieves it on the 4th ... This shift does NOT
    // occur if OUTI/OUTD are used", so it is not a blanket latch either.
    //
    // Microsecond N is T-state offset (N-1)*4 from the start of the instruction.
    (void)offset;
    // §4.4.3's table is the CRTC's. The ASIC's shift is described as "the CRTC misses
    // the I/O on the 3rd usec and retrieves it on the 4th", and it is the CRTC that
    // misses it -- §9.3.1 (p.52) states the other device's number separately and
    // without an exception: "The update of the internal register containing the mode is
    // effective on the 3rd usecond of the Z80A instruction of I/O OUT (C),R8 TO THE
    // GATE ARRAY." §9.3.3's chart is the proof. With R2=46 and the same instruction
    // walked one microsecond at a time, one MORE starting position still changes the
    // mode on CRTC 3 and 4 than on CRTC 0, 1 and 2 -- which is the ASIC's 1 usec HSYNC
    // delay, alone. Had the GATE ARRAY write moved late as well, the two would have
    // cancelled and the charts would be identical.
    //
    // (A partially decoded port can in principle reach the CRTC and the PPI at once;
    // this then moves both. The compendium does not time that case, and the CRTC is
    // the device its table is about.)
    if (kind == "out-c") {
        bool toCrtc = (port & 0x4000) == 0 && (port & 0x0300) <= 0x0100;
        return crtc && toCrtc ? crtc->behaviour->outCWriteOffset() : 8;
    }
    if (kind == "out-n")    return 8;               // 3rd usec
    if (kind == "block-out") return 16;             // OUTI/OUTD, 5th usec
    if (kind == "in-c")     return 12;              // 4th usec
    if (kind == "block-in") return 12;              // INI/IND, 4th usec
    if (kind == "in-n")     return 8;               // 3rd usec
    return std::max(0, offset);
}
void GX4000::advanceHardwareToInstructionOffset(int offset) {
    if (!timingInstructionActive) return;
    int target = std::max(hardwareCyclesAdvanced, (std::max(0, offset) + 3) & ~3);
    int delta = target - hardwareCyclesAdvanced;
    if (delta > 0) crtc->tick(delta);
    hardwareCyclesAdvanced = target;
}
int GX4000::cpcInstructionCycles(int z80Cycles, bool ioAccessed) {
    int cycles = (z80Cycles + 3) & ~3;
    if (ioAccessed && (z80Cycles & 3) == 0) cycles += 4;
    // ACCC §27.4 (p.286): "The Z80A RST #38 instruction lasts 4 µsec when called by
    // code. When an interrupt occurs, the call in #38 lasts 5 µsec." The difference is
    // the interrupt-acknowledge cycle. (The chapter states the figure for IM 1; the
    // acknowledge is common to both modes, so IM 2 carries it too.)
    if (cpu->lastWasInterrupt) return cycles + 4;
    int opcode = cpu->lastOpcode;
    int extended = cpu->lastEdOpcode;
    if (opcode != -1) {
        if (opcode == 0x22 || opcode == 0x2a || opcode == 0xe3) cycles += 4;
        else if (opcode == 0x36 && cpu->lastIndex) cycles += 4;
        // ACCC §26 (p.282-283): the DD CB / FD CB group is a microsecond longer on
        // the CPC than the plain wait-state rounding gives. The table has
        // BIT x,(IX/IY+d) at 6 usec (23 T rounds to 5) and every
        // RLC/RRC/RL/RR/SLA/SRA/SLL/SRL/RES/SET (IX/IY+d) at 7 (rounds to 6) —
        // §29 confirms it from the other side: "an instruction like set n,(ix+n')
        // officially lasts 23 cycles-T without being stretched. On CPC the alignment
        // caused by the Gate Array lengthens this instruction to 27 or 28 cycles-T."
        else if (opcode == 0xcb && cpu->lastIndex) cycles += 4;
        else if ((opcode & 0xcf) == 0xc5) cycles += 4;
        else if ((opcode & 0xc7) == 0xc7) cycles += 4;
        else if ((opcode & 0xc7) == 0xc0 && cpu->lastBranchTaken == 1) cycles += 4;
        else if (opcode == 0x10 && cpu->lastBranchTaken == 0) cycles += 4;
        if (opcode == 0xed && extended != -1) {
            if ((extended & 0xcf) == 0x43 || (extended & 0xcf) == 0x4b) cycles += 4;
            else if (extended == 0xa0 || extended == 0xa8) cycles += 4;
            else if ((extended == 0xb0 || extended == 0xb8) && cpu->lastBranchTaken != 1) cycles += 4;
        }
    }
    return cycles;
}
int GX4000::cpcInstructionCycles(int z80Cycles) { return cpcInstructionCycles(z80Cycles, cpuIoAccessed); }
int GX4000::stepInstruction() {
    cpuIoAccessed = false;
    timingInstructionActive = true;
    hardwareCyclesAdvanced = 0;
    // The M4 holds the Z80 while it fetches from the internet (|HTTPGET, |HTTPMEM), and
    // LambdaSpeak 3 while it speaks in blocking mode (its READY line): the
    // rest of the machine runs on, a microsecond at a time, until the answer is in.
    if (m4->holdsCpu() || speech->holdsCpu()) {
        advanceHardwareToInstructionOffset(4);
        machineCycles += 4;
        timingInstructionActive = false;
        lastInstructionSlackT = 0;
        m4->netTick();
        if (v9990->enabled) v9990->advanceTo(machineCycles);
        if (opl4->enabled) opl4->tick(machineCycles);
        return 4;
    }
    // ACCC §27.7.2 (p.290): "If the CRTC activates the end of HSYNC during the LAST CYCLE
    // T of an instruction (0.25 usec), the delay is very short to allow the Gate Array to
    // activate the INT signal early so that the Z80A consider it. If the end of HSYNC
    // arrives too late, then the Z80A is not warned in time, and the interruption does not
    // take place. According to the CRTC, the Z80A can therefore generate ADDITIONAL
    // INSTRUCTION before generating its interruption."
    //
    // The GATE ARRAY can only raise on a character boundary (§16.2.3), and an instruction
    // is a whole number of characters, so there is exactly ONE boundary inside each 4-cycle
    // group: a raise seen only while the LAST group runs is a raise on the instruction's
    // own end boundary, which is the case the chapter calls too late. A raise on any
    // earlier boundary gives the Z80A at least a microsecond of warning and is taken
    // normally. So the quarter-microsecond deadline is decidable at this granularity.
    const bool interruptBeforeInstruction = cpu->pendingInterrupt != -1;
    const long long instructionStart = machineCycles;
    // ACCC §27.3.1: when an armed interrupt is taken, "bit 5 of counter R52 is reset to 0
    // at the 'real' end of this instruction" -- the instruction the interrupt follows.
    // The GATE ARRAY sees that end as the start of the Z80A's interrupt acknowledge cycle,
    // which begins on the Z80A's own last T-state. Z80::interrupt() calls the acknowledge
    // back only AFTER pushing PC, by which time the hardware has run most of the response
    // and R52 has had time to step past 31 -- which cleared bit 5 on SHAKER BR/B lines the
    // real CPC prints as not cleared ("FROM LAST INTER:#14F6 >> NEXT INTER:#0D04
    // (Exp:#0504)").
    //
    // So the clear is done here, first, in the microsecond the acknowledge is seen in.
    // Two SHAKER screens on a real CPC place it, each printing its own verdicts:
    //
    //   BR/B  EI + NOP / CP (HL) / EX (SP),HL -- idle tails of 0, 1 and 3 T-states --
    //         clears from #14F8, #14F7 and #14F4.
    //   BR/A  each instruction timed to end on the same microsecond, just after R52's
    //         #1F -> #20 step: #C4 (acknowledged BEFORE the step) for DEC HL and DEC IY
    //         (2 idle T-states), LD A,I and LD A,R (3); #CC (after it) for everything
    //         with 0 or 1 -- SBC HL,HL, RES 0,(HL), LD (IY+d),B, NEG...
    //
    // i.e. an instruction that ends with 2 or more idle T-states in its last microsecond
    // is acknowledged IN that microsecond, and one that ends with 0 or 1 in the next.
    // Nothing else sorts the two lists: 'ends in internal cycles' puts SBC HL,HL (7 of
    // them, #CC) with DEC HL (#C4).
    //
    // ...and the same moment is where the response starts. When the acknowledge falls in
    // the instruction's own last microsecond, that microsecond IS the response's first,
    // and the pair is one microsecond shorter than the two durations added. BR/B's
    // EI/EX (SP),HL column measures it: every line one microsecond longer
    // to the next interrupt on the real CPC (#0504, #0503, #0D02) than an unshared
    // 5-microsecond response gave (#0503, #0502, #0D01). After a NOP or a HALT there is
    // no idle tail, which is why §27.4's 5 microseconds hold as measured.
    r52AcknowledgedThisStep = false;
    bool responseSharesMicrosecond = false;
    if (plusHardware == false && !cpu->pendingNmi && cpu->pendingInterrupt != -1
        && cpu->iff1 && cpu->eiDelay == 0) {
        static const bool traceSlack = std::getenv("CPCSE_TRACE_ACK") != nullptr;
        if (traceSlack)
            std::fprintf(stderr, "ACK after slack %dT  C0=%d R52=%d\n", lastInstructionSlackT,
                         crtc->horizontal, gateArray->interruptCounter);
        if (lastInstructionSlackT <= 1) advanceHardwareToInstructionOffset(4);
        else responseSharesMicrosecond = true;
        gateArray->acknowledgeInterrupt();
        r52AcknowledgedThisStep = true;
    }
    int z80Cycles = cpu->step();
    int cycles = cpcInstructionCycles(z80Cycles);
    if (responseSharesMicrosecond && cpu->lastWasInterrupt) cycles -= 4;
    // Walk the instruction one character at a time so the microsecond the INT arrives in
    // is known, rather than only that it arrived somewhere inside.
    int raiseMicrosecond = -1;
    for (int group = 4; group <= cycles; group += 4) {
        advanceHardwareToInstructionOffset(group);
        if (raiseMicrosecond < 0 && !interruptBeforeInstruction && cpu->pendingInterrupt != -1)
            raiseMicrosecond = group / 4;
    }
    advanceHardwareToInstructionOffset(cycles);
    cycles = std::max(cycles, hardwareCyclesAdvanced);
    if (raiseMicrosecond > 0 && interruptPhaseTrace)
        std::fprintf(stderr, "INT us=%lld instr=%dus arrived in us %d of %d\n",
                     instructionStart / 4, cycles / 4, raiseMicrosecond, cycles / 4);
    // ...and the exemption the same section ends on: "The timing logic of the Gate Array
    // to align the cycles of the Z80A instructions allows for the avoidance of the
    // consequences of these discordant HSYNC ends FOR CERTAIN INSTRUCTIONS WHOSE FIRST
    // CYCLES OF THE GATE ARRAY ARE NOT ON THE LAST CYCLE T of the instruction of Z80A.
    // This is the case with NOP instruction (and by extension of its official supplier,
    // HALT instruction)."
    //
    // The chapter names NOP and HALT as "the case", not as the list, and the criterion it
    // states is about the GATE ARRAY's cycles, not about which opcode it is. The GATE
    // ARRAY only drives WAIT to align a BUS cycle -- an opcode fetch, a memory read or
    // write, an I/O access (§27.7.2's own reminder, and §4.4.2). So an instruction that
    // ENDS on a bus cycle has the Gate Array's cycles sitting on its last cycle T and the
    // late edge is still seen; one that ends in internal cycles runs its last cycle T
    // free of the motif, and the character boundary falls exactly there.
    //
    // The Z80A knows which it did: lastBusAccessEnd is where its last real bus cycle
    // finished, and the instruction is longer than that only when internal cycles follow.
    // SHAKER's DI tests exactly this distinction on a real 6128 and agrees -- DEC DE ends
    // with the two internal T-states of its 6-T M1 and takes the hit (#59), while
    // CP (IX+n) ends on a memory read (#C2) and SET n,(IX+n') on a memory write (#40),
    // and both are immune, as is NOP, whose only M-cycle is its fetch.
    const bool immuneToLateEdge = cpu->lastBusAccessEnd >= z80Cycles;
    if (raiseMicrosecond > 0 && raiseMicrosecond == cycles / 4 && !immuneToLateEdge
        && crtc->behaviour && crtc->behaviour->hsyncEndMissesInterruptSample()) {
        interruptHeldOver = cpu->pendingInterrupt;
        cpu->pendingInterrupt = -1;
        interruptsHeldOver += 1;
    } else if (interruptHeldOver != -1) {
        // The additional instruction has now run; the Z80A takes the interrupt at ITS end.
        if (cpu->pendingInterrupt == -1) cpu->pendingInterrupt = interruptHeldOver;
        interruptHeldOver = -1;
    }
    // What this instruction left idle at the end of its last microsecond, for the next
    // step's acknowledge (above). The Z80A's own last T-state is its T-states plus the
    // alignment waits; the CPC length is that rounded up to a microsecond, and only that
    // rounding is idle. Where the length is a microsecond MORE than the rounding (an I/O
    // cycle the GATE ARRAY stretches), the last T-state is at the end and nothing is idle.
    {
        const int realEnd = z80Cycles + cpu->alignmentWaits;
        const int rounded = (realEnd + 3) & ~3;
        lastInstructionSlackT = (cpu->lastWasInterrupt || cycles != rounded) ? 0 : rounded - realEnd;
    }
    machineCycles += cycles;
    timingInstructionActive = false;
    if (m4->networkActive()) m4->netTick();
    // The GFX9000 runs its own raster; its /INT is a level on the CPC's /INT, held while a
    // flag it enables is set, so the Z80A is asked again after each acknowledge until the
    // program clears the flag (V9990 manual p.76, p.82).
    if (v9990->enabled) {
        v9990->advanceTo(machineCycles);
        if (v9990->intAsserted() && cpu->pendingInterrupt == -1 && interruptHeldOver == -1) cpu->requestInterrupt(0xff);
    }
    // The OPL4's IRQ (its timers) is a level on /INT too.
    if (opl4->enabled) {
        opl4->tick(machineCycles);
        if (opl4->intAsserted() && cpu->pendingInterrupt == -1 && interruptHeldOver == -1) cpu->requestInterrupt(0xff);
    }
    // The PlayCity's CTC: channel 1's ZC/TO is wired to /NMI, and channels 0-3 put
    // their own IM2 vector on the bus (vector base | channel << 1).
    if (playcity->enabled) {
        if (playcity->takeNmi()) {
            cpu->requestNmi();
            if (machineCycles >= traceIoFrom() && traceIoPort(0x0066)) std::fprintf(stderr, "IO NMI (PlayCity CTC 1) @%lld\n", machineCycles);
        }
        if (cpu->pendingInterrupt == -1 && interruptHeldOver == -1) {
            const int vector = playcity->takeInterruptVector();
            if (vector >= 0) cpu->requestInterrupt(vector);
        }
    }
    if (watchpointPending.has_value()) {
        std::any hit = watchpointPending;
        watchpointPending.reset();
        debuggerPaused = true;
        stepTarget = -1;
        if (onWatchpoint) onWatchpoint(hit);
    }
    return cycles;
}
bool GX4000::debuggerBreakpointHit() {
    int pc = cpu->pc & 0xffff;
    bool stepTargetHit = stepTarget == pc;
    bool hasBreakpoint = breakpoints && breakpoints->count(pc);
    if (breakpointSkipOnce == pc) {
        breakpointSkipOnce = -1;
        return stepTargetHit;
    }
    if (!hasBreakpoint) return stepTargetHit;
    bool breakpoint = breakpointPredicate ? breakpointPredicate(pc) : true;
    return breakpoint || stepTargetHit;
}
int GX4000::runFrame() {
    if (debuggerPaused) return 0;
    // One call is one MONITOR picture, on every machine: a rupture display restarts the
    // CRTC several times per picture (Alcon 2020's R4=9 sub-frames), and one CRTC frame
    // per call ran such a game at a third of its speed with a picture every third call.
    int targetFrame = classicMonitorFrame + 1;
    videoCaptureRegisters = crtc->registers;
    int elapsed = 0;
    while (classicMonitorFrame < targetFrame && elapsed < 200000) {
        if (debuggerBreakpointHit()) {
            debuggerPaused = true;
            stepTarget = -1;
            if (onBreakpoint) onBreakpoint();
            break;
        }
        elapsed += stepInstruction();
        if (debuggerPaused) break;
    }
    if (ide->enabled) ide->poll(machineCycles);   // the disc's write-back once idle
    return elapsed;
}
void GX4000::reset() {
    breakpointSkipOnce = -1; watchpointPending.reset();
    timingInstructionActive = false; hardwareCyclesAdvanced = 0; classicFrameOvershoot = 0;
    interruptHeldOver = -1; interruptsHeldOver = 0;
    classicMonitorCharacter = 0; classicMonitorLine = 0; classicMonitorFrame = 0;
    monitorRenderer->reset(); syncClassicMonitorTiming(); classicDisplayOriginY = 40;
    // A set stays plugged in across a reset; with none chosen the machine has the one it
    // came with, which is ACCC 15.1's centred pairing (monitor_model.h).
    // Taken at EVERY reset, not only the first: a reset follows each machine build, and a
    // CRTC 4 machine built after the default CRTC 1 one used to keep that one's plain CTM
    // (every SHAKER CRTC 4 run until 2026-09-26 was a character out because of it).
    monitorRenderer->model = monitorSet ? monitorSet : shippedMonitorModel();
    memory->reset(); asic->reset(); gateArray->reset(); rasterCapture.clear(); rasterFrame.clear(); previousRasterFrame.clear();
    spritePatternSnapshot.clear(); spritePatternRevision = -1; crtc->reset();
    videoFrameRegisters = crtc->registers; videoCaptureRegisters = crtc->registers;
    keyboard->reset(); ay->reset(); ppi->reset(); fdc->reset(); dac->reset(); tape->reset(); m4->reset(); v9990->reset(); opl4->reset(); playcity->reset(); serial->reset(); speech->reset(); symbifaceMouse->reset(); sf2Rtc->reset(); sf3->reset(); ide->reset(); multiface->reset(); cpu->reset();
    cpu->sp = 0xbfff;
}
void GX4000::acknowledgeInterrupt() {
    // CPCSE_TRACE_INT=1 logs where every interrupt the Z80 ACCEPTS lands, in CRTC terms.
    // A game like Pinball Dreams syncs its rupture with EI/HALT/DI and writes a CRTC
    // register the instant the interrupt releases it (ACCC §27.3.1), so the interrupt's
    // line IS the write's line -- and a register trace alone cannot show that the CRTC
    // was innocent and the interrupt arrived somewhere else.
    {
        static long budget = -2, from = 0;
        if (budget == -2) {
            const char* want = std::getenv("CPCSE_TRACE_INT");
            budget = want ? (std::getenv("CPCSE_TRACE_INT_MAX")
                             ? std::atol(std::getenv("CPCSE_TRACE_INT_MAX")) : 200) : -1;
            const char* f = std::getenv("CPCSE_TRACE_INT_FROM");
            from = f ? std::atol(f) : 0;
        }
        if (budget > 0 && crtc && crtc->frame >= from) {
            std::fprintf(stderr, "f%-6d INT  C0=%2d C4=%3d C9=%d  R52=%2d  pc=%04X\n",
                         crtc->frame, crtc->horizontal, crtc->vertical, crtc->raster,
                         gateArray ? gateArray->interruptCounter : -1, cpu ? cpu->pc & 0xffff : -1);
            budget -= 1;
        }
    }
    if (plusHardware == false) {
        asic->clearGateArrayInterrupt();
        // stepInstruction has normally done this already, at the INTA's microsecond.
        if (!r52AcknowledgedThisStep) gateArray->acknowledgeInterrupt();
        return;
    }
    bool rasterOrGateArray = asic->rasterInterruptPending;
    asic->acknowledgeInterrupt();
    if (rasterOrGateArray) gateArray->acknowledgeInterrupt();
}

} // namespace cpcse
