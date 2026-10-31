// CPCSyntaxError — CPC and GX4000 machine core.
// Connects CPU, memory, video, audio, storage and expansion devices.
#pragma once
#include "common.h"
#include "mouse_interface.h"
#include "raster.h"
#include "cpr_loader.h"
#include "debug_symbols.h"
#include "logic_analyser.h"
#include "profiler.h"
#include <any>

namespace cpcse {

class GXMemory; class PlusAsic; class GateArray; class M4Board; class SymbifaceIde; class Hd20; class Multiface2; class V9990; class Opl4Card;
class PlayCity; class AmstradSerial;
class SpeechSynth;
class SymbifaceMouse; class Symbiface2Rtc; class Symbiface3; class CtmMonitor;
struct MonitorModel;
class CRTC6845; class KeyboardMatrix; class AY38912; class CpcDac; class CPCTapeDrive;
class PPI8255; class GamepadController; class UPD765A; class Z80;

class GX4000 {
public:
    // --- subsystems (owned) ---
    GXMemory* memory = nullptr;
    PlusAsic* asic = nullptr;
    GateArray* gateArray = nullptr;
    M4Board* m4 = nullptr;
    V9990* v9990 = nullptr;
    Opl4Card* opl4 = nullptr;          // an OPL4 card on the AMSDAP (&FFC4-7, &FF7E-F)
    PlayCity* playcity = nullptr;      // TotO's PlayCity: two YMZ294 + a Z80 CTC (&F880-&F988)
    AmstradSerial* serial = nullptr;   // Amstrad's RS232C: a Z80 DART (&FADC-F) + 8253 (&FBDC-F)
    SpeechSynth* speech = nullptr;     // an SP0256-AL2 speech synthesiser (SSA-1 / dk'tronics)
    SymbifaceMouse* symbifaceMouse = nullptr;
    Symbiface2Rtc* sf2Rtc = nullptr;
    Symbiface3* sf3 = nullptr;
    SymbifaceIde* ide = nullptr;
    Hd20* hd20 = nullptr;                  // the Dobbertin HD20 (off until an image is opened)
    // The Kempston mouse or the MultiPlay (mouse_interface.h). A value, not a chip pointer:
    // the timeline's copy of the machine takes its counters with the rest.
    MouseInterface mouseIf;
    Multiface2* multiface = nullptr;   // Romantic Robot's Multiface II     // the SYMBiFACE II / III IDE, a host folder its disc
    CtmMonitor* monitorRenderer = nullptr;
    // WHICH MONITOR IS PLUGGED IN (monitor_model.h). The machine needs to know because
    // ACCC 15.1 (p.146) makes the horizontal calibration a property of the PAIRING: the
    // CM14 and the CRTC 4 machine's CTM absorb the microsecond the ASIC delays HSYNC by,
    // a plain CTM 640/644 does not. nullptr means the set the machine shipped with.
    const MonitorModel* monitorSet = nullptr;
    int monitorCalibration16() const;
    void setMonitorModel(const MonitorModel* model);
    const MonitorModel* shippedMonitorModel() const;
    CRTC6845* crtc = nullptr;
    KeyboardMatrix* keyboard = nullptr;
    AY38912* ay = nullptr;
    CpcDac* dac = nullptr;
    CPCTapeDrive* tape = nullptr;
    PPI8255* ppi = nullptr;
    GamepadController* gamepad = nullptr;
    UPD765A* fdc = nullptr;
    Z80* cpu = nullptr;

    // --- machine state ---
    bool plusHardware = true;
    bool hasFdc = false;
    bool plusComputer = false;
    bool ramExpansion = false;
    int ramKiB = 64;
    std::string model = "CPC";

    std::vector<std::shared_ptr<RasterLine>> rasterCapture, rasterFrame, previousRasterFrame;
    std::vector<std::shared_ptr<RasterLine>> rasterPool;   // records of old frames, to fill again (completeRasterFrame)
    int classicMonitorCharacter = 0;
    // ACCC §27.6.5 (p.288): the ASIC's of CRTC 3 and 4 hand the GATE ARRAY their HSYNC
    // a microsecond late, so R52 increments -- and the interrupt lands -- one usec
    // later than on CRTC 0/1/2 ("code interrupted 16 usec after C0vs=R2" against 15).
    // Counts characters until the deferred GateArray::onHsync fires; 0 = idle.
    int gateArrayHsyncPending = 0;
    // Diagnostic counters (surfaced by the --shot --diag runner path). vsyncs/frame
    // should be 1.0; a shortfall means the CRTC is missing per-frame VSYNCs (rupture
    // timing). blankFrames = monitor frames with no display enabled.
    long dbgVsyncCount = 0, dbgFrameCount = 0, dbgBlankFrames = 0;
    int classicMonitorLine = 0;
    int classicMonitorFrame = 0;
    int classicMonitorLineCharacters = 64;
    int classicMonitorHsyncLimit = 64 * 16;
    int classicMonitorHsyncCount = 0;
    int classicMonitorHsyncMatch = 0;
    int classicMonitorLineOffset = 0;
    int appliedLineSnap16 = 0;      // the part of CtmMonitor::lineSnap16 already applied
    int lineSlotShift = 0;
    long seenDecisionSeq = 0;       // the last CtmMonitor decision filed on a line          // characters a re-lock moved this sweep line's filing by
    int classicDisplayOriginY = 40;
    Bytes spritePatternSnapshot;
    int spritePatternRevision = -1;
    std::array<uint8_t, 18> videoFrameRegisters{};
    std::array<uint8_t, 18> videoCaptureRegisters{};
    int classicFrameOvershoot = 0;
    bool cpuIoAccessed = false;
    bool timingInstructionActive = false;
    int hardwareCyclesAdvanced = 0;
    // ACCC §27.7.2 (p.290): an INT the GATE ARRAY only raises at the very end of an
    // instruction does not reach the Z80A in time to be taken at that boundary, so the
    // processor runs one ADDITIONAL INSTRUCTION first. It waits here meanwhile.
    int interruptHeldOver = -1;
    long interruptsHeldOver = 0;       // diagnostic counter for cpcse-int-check
    // ACCC §27.3.1: R52's bit 5 is cleared at the "real" end of the instruction the
    // interrupt follows. The idle T-states that instruction left at the end of its last
    // microsecond (padded CPC length minus the Z80A's own), and whether this step's
    // acknowledge has already done the clear at that moment. See stepInstruction.
    int lastInstructionSlackT = 0;
    bool r52AcknowledgedThisStep = false;
    bool interruptPhaseTrace = false;  // diagnostic: log the microsecond each INT arrives in
    std::shared_ptr<Cartridge> cartridge;
    std::shared_ptr<DebugMetadata> snapshotDebugMetadata;

    // debugger integration
    bool debuggerPaused = false;
    std::unordered_set<int>* breakpoints = nullptr;
    std::function<bool(int)> breakpointPredicate;
    int breakpointSkipOnce = -1;
    int stepTarget = -1;
    std::any watchpointPending;
    std::function<void(const std::any&)> onWatchpoint;
    std::function<void()> onBreakpoint;
    // snapshot/debugger metadata sources (populated by the debugger, if present)
    std::unordered_map<int, std::string> breakpointSources;
    std::vector<RemuWatchpoint> watchpoints;

    // lightgun samplers wired by the renderer (optional)
    std::function<std::optional<int>(int, int)> lightgunRasterSampler;
    std::function<std::optional<int>(int, int)> lightgunPixelSampler;

    // Beam-driven renderer hooks (classic monitor path). onBeamCharacter fires
    // once per CRTC character with the live monitor beam position (character
    // column, monitor line); a beam renderer plots the Gate Array's output for
    // the current CRTC state at that spot. onBeamFrame fires at each vertical
    // frame edge. Both empty unless a beam renderer is attached.
    std::function<void()> onBeamCharacter;   // plot the current CRTC character at the beam
    std::function<void()> onBeamHsync;       // horizontal retrace (CRTC HSYNC pin)
    std::function<void()> onBeamVsync;       // vertical retrace / present (CRTC VSYNC pin)
    // ACCC §16.6: the C0 the monitor's C-VSYNC arrived on, and that line's R0. How
    // high the beam retraces depends on it, so the renderer reads them in onBeamVsync.
    int monitorSyncCharacter = 0, monitorSyncLineLength = 63;
    std::function<void()> onBeamFrame;       // legacy monitor-frame edge (unused by the pin beam)

    struct Beam { double x = 0, y = 0; };

    GX4000();
    ~GX4000();

    void applyGunstickProbeBrightness(bool bright);
    void refreshGunstickRenderedSample();
    void updateGunstickSensor(Beam beam, double aimX, double aimY);
    void completeRasterFrame();
    void advanceClassicMonitorCharacter();
    void advanceClassicMonitorRun(int n);
    void syncClassicMonitorTiming();
    int rasterByteAddress(int ma, int raster);
    int rasterByteAddress(int ma);
    const Bytes& snapshotSpritePatterns();
    Beam lightgunDisplayOrigin();
    Beam lightgunBeamCanvasPosition();
    bool isLightgunPixelBright(double x, double y, bool liveBeam = false);
    void captureRasterState();
    void captureRasterCharacter();
    unsigned classicHsyncPipe = 0;   // ACCC §9.3.1's per-chip HSYNC display delay

    // internalRamKiB: the machine's own RAM (64 on a 464 Plus or GX4000, 128 on a 6128 Plus or
    // a GX4000 with the 128K mod); classicExpansion: RAM beyond it is a board on the
    // expansion port (GXMemory::classicExpansion), else an internal upgrade.
    struct LoadCartridgeOptions { bool plusComputer = false; int ramKiB = -1; bool ram128 = false; int internalRamKiB = -1; bool classicExpansion = true; };
    void loadCartridge(const Cartridge& cartridge, LoadCartridgeOptions options);
    void loadCartridge(const Cartridge& cartridge);
    struct LoadClassicOptions { Bytes lowerRom; Bytes basicRom; Bytes amsdosRom; int ramKiB = -1; bool ram128 = false; int crtcType = 0; std::string model = "CPC"; };
    void loadClassicFirmware(const LoadClassicOptions& options);
    void setDacType(const std::string& type = "none");
    void writePort(int port, int value);
    int readPort(int port);
    int readPortUntraced(int port);
    int cpcIoEffectOffset(int offset, const std::string& kind = "generic", int port = 0);
    void advanceHardwareToInstructionOffset(int offset = 0);
    int cpcInstructionCycles(int z80Cycles, bool ioAccessed);
    int cpcInstructionCycles(int z80Cycles);
    int stepInstruction();
    // Instant tape loading (CPCTapeDrive::instantLoad): the Z80 at the OS ROM's CAS READ
    // (known by its first bytes); casReadTrap does the routine's work, or returns false.
    bool casReadTrap();
    // The logic analyser (logic_analyser.h): a sample at the end of every character while
    // it is enabled; a trigger it fires stops the machine as a breakpoint does.
    LogicAnalyser analyser;
    void recordAnalyser();
    // The code profiler (profiler.h): told every step while set; null, nothing is done.
    CodeProfiler* profiler = nullptr;
    // The raster timing overlay (RasterLine::ioMarks): each OUT noted on the line being swept
    // while ioOverlay is set; nothing otherwise.
    bool ioOverlay = false;
    void noteIo(int port, int value, int kind = -1);
    // THE TIMELINE'S CLOCK (timeline.h): instructions stepped since power-on. An input
    // logged at `instructions == k` goes in before instruction k on a replay: onInputDue is
    // called at the start of the step whose number reaches inputDueAt.
    uint64_t instructions = 0;
    uint64_t inputDueAt = UINT64_MAX;
    std::function<void()> onInputDue;
    bool motorStartWaitTrap();
    // Machine time in T-states (4 per usec): every instruction as long as the hardware
    // ran it, i.e. after the CPC's rounding to whole usec. cpu->tStates is the Z80's own
    // count and runs ~14% short of this -- it is not a clock for anything outside the CPU.
    long long machineCycles = 0;
    long traceMonitorEdgeBudget = 0;   // diagnostic: log this many monitor frame edges
    long traceGateArrayBudget = 0;     // diagnostic: log this many GATE ARRAY writes
    bool debuggerBreakpointHit();
    int runFrame();
    void reset();
    void acknowledgeInterrupt();
};

} // namespace cpcse
