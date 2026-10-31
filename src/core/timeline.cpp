// CPCSyntaxError — machine snapshots for the timeline. See timeline.h.
#include "timeline.h"

#include <new>
#include <optional>

#include "emulator.h"
#include "memory.h"
#include "asic.h"
#include "gate_array.h"
#include "v9990.h"
#include "playcity.h"
#include "symbiface_mouse.h"
#include "sf2_rtc.h"
#include "sf3.h"
#include "multiface.h"
#include "monitor_renderer.h"
#include "crtc.h"
#include "keyboard.h"
#include "ay.h"
#include "dac.h"
#include "tape.h"
#include "ppi.h"
#include "gamepad.h"
#include "fdc.h"
#include "z80.h"

namespace cpcse {

struct MachineSnapshot::Parts {
    // the machine's own fields; its chip pointers in it are the live chips' (never deleted)
    alignas(GX4000) unsigned char core[sizeof(GX4000)];
    bool hasCore = false;
    std::optional<GXMemory> memory;
    std::optional<PlusAsic> asic;
    std::optional<GateArray> gateArray;
    std::optional<V9990> v9990;
    std::optional<PlayCity> playcity;
    std::optional<SymbifaceMouse> symbifaceMouse;
    std::optional<Symbiface2Rtc> sf2Rtc;
    std::optional<Symbiface3> sf3;
    std::optional<Multiface2> multiface;
    std::optional<CtmMonitor> monitor;
    std::optional<CRTC6845> crtc;
    std::optional<KeyboardMatrix> keyboard;
    std::optional<AY38912> ay;
    std::optional<CpcDac> dac;
    std::optional<CPCTapeDrive> tape;
    std::optional<PPI8255> ppi;
    std::optional<GamepadController> gamepad;
    std::optional<UPD765A> fdc;
    std::optional<Z80> cpu;
    GX4000& coreCopy() { return *reinterpret_cast<GX4000*>(core); }
    const GX4000& coreCopy() const { return *reinterpret_cast<const GX4000*>(core); }
};

namespace {

// The memory's ROM images and cartridge, set aside and put back: they are the media, not
// the state, and copying them into every snapshot would be most of its size.
struct RomBulk {
    Bytes lowerRom, snapshotLowerRom;
    std::vector<Bytes> upperRoms, snapshotUpperRoms, cartridge;
    int populatedBanks = 0;
    void take(GXMemory& m) {
        lowerRom.swap(m.lowerRom); snapshotLowerRom.swap(m.snapshotLowerRom);
        upperRoms.swap(m.upperRoms); snapshotUpperRoms.swap(m.snapshotUpperRoms); cartridge.swap(m.cartridge);
        populatedBanks = m.populatedBanks;
    }
    void give(GXMemory& m) {
        lowerRom.swap(m.lowerRom); snapshotLowerRom.swap(m.snapshotLowerRom);
        upperRoms.swap(m.upperRoms); snapshotUpperRoms.swap(m.snapshotUpperRoms); cartridge.swap(m.cartridge);
        m.populatedBanks = populatedBanks;
    }
};
// The tape's image: the same.
struct TapeBulk {
    Bytes data;
    std::vector<TapeBlock> blocks;
    std::vector<long long> blockStartCycles;
    void take(CPCTapeDrive& t) { data.swap(t.data); blocks.swap(t.blocks); blockStartCycles.swap(t.blockStartCycles); }
    void give(CPCTapeDrive& t) { data.swap(t.data); blocks.swap(t.blocks); blockStartCycles.swap(t.blockStartCycles); }
};
// What of the machine object belongs to the host and the debugger, not to the machine:
// the live values stay across a restore.
struct HostSide {
    LogicAnalyser analyser;
    std::vector<std::shared_ptr<RasterLine>> rasterFrame, previousRasterFrame, rasterPool;
    bool debuggerPaused = false;
    std::unordered_set<int>* breakpoints = nullptr;
    std::function<bool(int)> breakpointPredicate;
    int breakpointSkipOnce = -1, stepTarget = -1;
    std::any watchpointPending;
    std::function<void(const std::any&)> onWatchpoint;
    std::function<void()> onBreakpoint, onBeamCharacter, onBeamHsync, onBeamVsync, onBeamFrame, onInputDue;
    std::unordered_map<int, std::string> breakpointSources;
    std::vector<RemuWatchpoint> watchpoints;
    std::function<std::optional<int>(int, int)> lightgunRasterSampler, lightgunPixelSampler;
    uint64_t inputDueAt = UINT64_MAX;
    bool ioOverlay = false;
    CodeProfiler* profiler = nullptr;
    void take(GX4000& m) {
        ioOverlay = m.ioOverlay;
        profiler = m.profiler;
        std::swap(analyser, m.analyser);
        rasterFrame.swap(m.rasterFrame); previousRasterFrame.swap(m.previousRasterFrame); rasterPool.swap(m.rasterPool);
        debuggerPaused = m.debuggerPaused; breakpoints = m.breakpoints; breakpointPredicate = m.breakpointPredicate;
        breakpointSkipOnce = m.breakpointSkipOnce; stepTarget = m.stepTarget; watchpointPending = m.watchpointPending;
        onWatchpoint = m.onWatchpoint; onBreakpoint = m.onBreakpoint; onBeamCharacter = m.onBeamCharacter;
        onBeamHsync = m.onBeamHsync; onBeamVsync = m.onBeamVsync; onBeamFrame = m.onBeamFrame; onInputDue = m.onInputDue;
        breakpointSources = m.breakpointSources; watchpoints = m.watchpoints;
        lightgunRasterSampler = m.lightgunRasterSampler; lightgunPixelSampler = m.lightgunPixelSampler;
        inputDueAt = m.inputDueAt;
    }
    void give(GX4000& m) {
        m.ioOverlay = ioOverlay;
        m.profiler = profiler;
        std::swap(analyser, m.analyser);
        rasterFrame.swap(m.rasterFrame); previousRasterFrame.swap(m.previousRasterFrame); rasterPool.swap(m.rasterPool);
        m.debuggerPaused = debuggerPaused; m.breakpoints = breakpoints; m.breakpointPredicate = breakpointPredicate;
        m.breakpointSkipOnce = breakpointSkipOnce; m.stepTarget = stepTarget; m.watchpointPending = watchpointPending;
        m.onWatchpoint = onWatchpoint; m.onBreakpoint = onBreakpoint; m.onBeamCharacter = onBeamCharacter;
        m.onBeamHsync = onBeamHsync; m.onBeamVsync = onBeamVsync; m.onBeamFrame = onBeamFrame; m.onInputDue = onInputDue;
        m.breakpointSources = breakpointSources; m.watchpoints = watchpoints;
        m.lightgunRasterSampler = lightgunRasterSampler; m.lightgunPixelSampler = lightgunPixelSampler;
        m.inputDueAt = inputDueAt;
    }
};
// the scan lines the frame being made has so far: copies of their own (they are written
// on as the frame goes on)
void ownLines(std::vector<std::shared_ptr<RasterLine>>& lines) {
    for (auto& l : lines) if (l) l = std::make_shared<RasterLine>(*l);
}

}   // namespace

std::unique_ptr<MachineSnapshot> MachineSnapshot::capture(GX4000& m) {
    std::unique_ptr<MachineSnapshot> s(new MachineSnapshot());
    s->instructions = m.instructions;
    s->parts = std::make_unique<Parts>();
    Parts& p = *s->parts;
    {
        HostSide host;
        host.take(m);
        new (p.core) GX4000(m);
        p.hasCore = true;
        host.give(m);
        ownLines(p.coreCopy().rasterCapture);
    }
    { RomBulk roms; roms.take(*m.memory); p.memory.emplace(*m.memory); roms.give(*m.memory); }
    { TapeBulk bulk; bulk.take(*m.tape); p.tape.emplace(*m.tape); bulk.give(*m.tape); }
    p.asic.emplace(*m.asic);
    p.gateArray.emplace(*m.gateArray);
    if (m.v9990 && m.v9990->enabled) {
        // its pictures (the one being drawn, the one shown) are output, like the CPC's: not kept
        V9990Picture work, shown;
        std::swap(work, m.v9990->work); std::swap(shown, m.v9990->shown);
        p.v9990.emplace(*m.v9990);
        std::swap(work, m.v9990->work); std::swap(shown, m.v9990->shown);
    }
    if (m.playcity && m.playcity->enabled) p.playcity.emplace(*m.playcity);
    if (m.symbifaceMouse) p.symbifaceMouse.emplace(*m.symbifaceMouse);
    if (m.sf2Rtc) p.sf2Rtc.emplace(*m.sf2Rtc);
    if (m.sf3) p.sf3.emplace(*m.sf3);
    if (m.multiface) p.multiface.emplace(*m.multiface);
    p.monitor.emplace(*m.monitorRenderer);
    p.crtc.emplace(*m.crtc);
    p.keyboard.emplace(*m.keyboard);
    p.ay.emplace(*m.ay);
    p.dac.emplace(*m.dac);
    p.ppi.emplace(*m.ppi);
    p.gamepad.emplace(*m.gamepad);
    p.fdc.emplace(*m.fdc);
    p.cpu.emplace(*m.cpu);
    // what it holds: every buffer of any size in the copies (the budget counts this)
    size_t b = sizeof(Parts);
    b += p.memory->ram.capacity() + p.memory->asicRam.capacity() + p.memory->accessMap.capacity();
    b += p.tape->pulseQueue.capacity() * sizeof(PulseEvent) + p.tape->recordPulses.capacity() * sizeof(uint32_t)
       + p.tape->recorded.capacity() + p.tape->loopStack.capacity() * sizeof(LoopFrame)
       + (p.tape->blockFast.capacity() + p.tape->blockZero.capacity() * sizeof(double));
    b += p.ay->sampleQueue.capacity() * sizeof(std::array<double, 2>) + p.dac->sampleQueue.capacity() * sizeof(double);
    b += p.fdc->transfer.capacity() + p.fdc->params.capacity() * sizeof(int);
    if (p.v9990) b += p.v9990->vram.capacity() + p.v9990->lineSprites.capacity() * 64;
    if (p.multiface) b += p.multiface->rom.capacity() + 16 * 1024;
    for (const auto& l : p.coreCopy().rasterCapture) if (l) b += sizeof(RasterLine) + 6 * 1024;
    b += p.coreCopy().spritePatternSnapshot.capacity();
    s->bytes = b;
    return s;
}

void MachineSnapshot::restore(GX4000& m) const {
    const Parts& p = *parts;
    {
        HostSide host;
        host.take(m);
        m = p.coreCopy();
        host.give(m);
        ownLines(m.rasterCapture);
    }
    {
        RomBulk roms;
        roms.take(*m.memory);
        // the debugger's hooks on the memory stay as they are now
        auto watchHook = m.memory->watchHook;
        const bool watchArmed = m.memory->watchArmed, trackAccess = m.memory->trackAccess;
        *m.memory = *p.memory;
        m.memory->watchHook = watchHook; m.memory->watchArmed = watchArmed; m.memory->trackAccess = trackAccess;
        roms.give(*m.memory);
    }
    { TapeBulk bulk; bulk.take(*m.tape); *m.tape = *p.tape; bulk.give(*m.tape); }
    {
        auto onPaletteWrite = m.asic->onPaletteWrite;
        void* context = m.asic->onPaletteWriteContext;
        *m.asic = *p.asic;
        m.asic->onPaletteWrite = onPaletteWrite; m.asic->onPaletteWriteContext = context;
    }
    {
        auto onPixel = m.gateArray->onPixel;
        *m.gateArray = *p.gateArray;
        m.gateArray->onPixel = onPixel;
    }
    if (p.v9990 && m.v9990) {
        V9990Picture work, shown;          // the live pictures stay
        std::swap(work, m.v9990->work); std::swap(shown, m.v9990->shown);
        *m.v9990 = *p.v9990;
        std::swap(work, m.v9990->work); std::swap(shown, m.v9990->shown);
    }
    if (p.playcity && m.playcity) *m.playcity = *p.playcity;
    if (p.symbifaceMouse && m.symbifaceMouse) *m.symbifaceMouse = *p.symbifaceMouse;
    if (p.sf2Rtc && m.sf2Rtc) *m.sf2Rtc = *p.sf2Rtc;
    if (p.sf3 && m.sf3) *m.sf3 = *p.sf3;
    if (p.multiface && m.multiface) *m.multiface = *p.multiface;
    *m.monitorRenderer = *p.monitor;
    *m.crtc = *p.crtc;
    {
        auto onInput = m.keyboard->onInput;
        auto onRowRead = m.keyboard->onRowRead;
        *m.keyboard = *p.keyboard;
        m.keyboard->onInput = onInput; m.keyboard->onRowRead = onRowRead;
    }
    *m.ay = *p.ay;
    *m.dac = *p.dac;
    *m.ppi = *p.ppi;
    *m.gamepad = *p.gamepad;
    *m.fdc = *p.fdc;
    {
        auto accessObserver = m.cpu->accessObserver;
        auto onUnwiredEd = m.cpu->onUnwiredEd;
        *m.cpu = *p.cpu;
        m.cpu->accessObserver = accessObserver; m.cpu->onUnwiredEd = onUnwiredEd;
    }
}

MachineSnapshot::~MachineSnapshot() {
    if (!parts || !parts->hasCore) return;
    // the copy's chip pointers are the live machine's: never deleted from here
    GX4000& c = parts->coreCopy();
    c.memory = nullptr; c.asic = nullptr; c.gateArray = nullptr; c.m4 = nullptr; c.v9990 = nullptr; c.opl4 = nullptr;
    c.playcity = nullptr; c.serial = nullptr; c.speech = nullptr; c.symbifaceMouse = nullptr; c.sf2Rtc = nullptr;
    c.sf3 = nullptr; c.ide = nullptr; c.hd20 = nullptr; c.multiface = nullptr; c.monitorRenderer = nullptr; c.crtc = nullptr;
    c.keyboard = nullptr; c.ay = nullptr; c.dac = nullptr; c.tape = nullptr; c.ppi = nullptr; c.gamepad = nullptr;
    c.fdc = nullptr; c.cpu = nullptr;
    c.~GX4000();
}

} // namespace cpcse
