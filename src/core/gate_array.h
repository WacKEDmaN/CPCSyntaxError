// CPCSyntaxError — CPC Gate Array.
// Memory commands, display mode latching and 300 Hz interrupts.
#pragma once
#include "common.h"
#include "gate_array_model.h"

namespace cpcse {

class GXMemory;
class PlusAsic;
class Z80;

class GateArray {
public:
    // Which of the five this machine is fitted with. ACCC §9 (p.44). Defaults to the
    // 40010, the one most 6128's carry; the two ASIC's are selected by the CRTC type,
    // because on those machines the CRTC and the GATE ARRAY are one chip.
    const GateArrayModel* model = gateArrayModel40010();
    GXMemory* memory;
    PlusAsic* asic;
    Z80* cpu = nullptr;
    bool plusHardware = false;
    int interruptCounter = 0;
    // CPCSE_TRACE_INTRAISE: the host prints the CRTC position, which this chip has no
    // business knowing, so it hands the event out instead of reaching for it.
    std::function<void(const char*, bool, int)> onTraceRaise;
    long traceRaiseBudget = -2;
    int interruptSyncCount = 0;
    // ACCC §27.6.1 (p.287): "an interrupt always starts 1 µsec AFTER THE END OF THE
    // HSYNC regardless of the CRTC", and §27.6.2's chart puts the interrupted code at
    // C0vs=R2+R3+1 -- the cell after the one where C3 reaches R3. R52 itself counts on
    // the HSYNC's end (§27.2's note: "incremented on R2+R3-1"); only the INT line the
    // count raises comes up a microsecond later. Counts characters: 2 because onCharacter
    // runs later in the same character the end arrived in.
    int interruptRaiseIn = 0;
    int modeLatchDelay = 0;
    bool modeLatchJustArmed = false;
    // ACCC §9.3.4 (p.54): the GATE ARRAY does not switch graphic mode on a byte
    // boundary. "On CRTC's 0, 1 and 2, the GATE ARRAY switches the mode update to
    // the 6th pixel mode 2 (i.e. the 3rd pixel mode 1, the 2nd pixel mode 0). On
    // the CRTC 4, the GATE ARRAY switches the mode change to the 4th pixel mode 2."
    // Pushed in by the host from the CRTC profile; index is 0-based Pixel-M2.
    int modeSwitchPixelInByte = 5;
    // ACCC §27 (p.284): "If the request to reset R52 counter to 0 is made via the RMR
    // function of the Gate Array and this request takes place on the last µsecond of
    // the HSYNC (C0=R2+R3-1), then THE RESET TO 0 HAS PRIORITY OVER INCREMENTATION."
    // Set by the host when the reset lands on that position; consumed by onHsync.
    bool suppressNextR52Increment = false;

    // ---- ACCC §9.1: the GATE ARRAY's own registers --------------------------------
    // "A 17th colour, STORED IN THE GATE ARRAY, is displayed when BORDER is activated."
    // The pen select, the 17 inks, the graphic mode and the ROM/interrupt byte are this
    // chip's registers and are written through this chip's port (&7Fxx). They lived on
    // PlusAsic, which is the CPC+ extension, not the part a 464 has.
    int paletteIndex = 0;                  // the selected pen, 0..15 or 16 = BORDER
    std::array<uint8_t, 17> gaPalette{};   // 16 inks + the border, as 5-bit hardware colours
    int mode = 0, newMode = 0;             // the graphic mode, and the one awaiting HSYNC
    // ACCC §9.3.4 MODE SPLITTING: a mode latch lands inside the byte this chip is
    // decoding. modeSwitchPixel is the 0-based Pixel-M2 within that byte at which the
    // new mode takes over (-1 = none pending); modeBefore is the mode the earlier
    // pixels of that byte were decoded in.
    int modeSwitchPixel = -1, modeBefore = 0;
    int romConfig = 0;                     // the RMR byte as last written
    void latchMode(int switchPixel = -1);

    // ACCC §9.1: "A 17th colour, stored in the GATE ARRAY, is displayed when BORDER is
    // activated." Pen 16 IS that colour, and choosing between it and the decoded pixel
    // is this chip's decision, taken from the CRTC's DISPTMG pin -- not the renderer's.
    static constexpr int BORDER_PEN = 16;
    int outputPen(bool displayEnabled, int decodedPen) const {
        return displayEnabled ? decodedPen : BORDER_PEN;
    }
    // Pen -> the 5-bit hardware colour this chip holds -> R, G and B. The tube's tint
    // (colour, green, monochrome) is applied after this, by the monitor.
    int penHardwareColour(int pen) const;
    int penRgb(int pen) const;
    static int rgbForHardwareColour(int hardwareColour);
    // ACCC §27 (p.284): the RMR's bit 4 zeroes R52, and "if this request takes place on
    // the last µsecond of the HSYNC (C0=R2+R3-1), then THE RESET TO 0 HAS PRIORITY OVER
    // INCREMENTATION". The host supplies that position test, which needs the CRTC.
    std::function<void()> onInterruptReset;
    void resetInterruptCounter() { if (onInterruptReset) onInterruptReset(); }

    GateArray(GXMemory* memory, PlusAsic* asic, bool plusHardware = false);
    void reset();
    void attachCpu(Z80* cpu) { this->cpu = cpu; }
    void setPlusHardware(bool enabled) { plusHardware = enabled; }
    // The &7Fxx port decode -- pen select, ink, RMR and RAM config. This is the GATE
    // ARRAY's port; only the CPC+ cartridge-page case inside it belongs to the ASIC.
    void write(int port, int value);
    int read() { return 0xff; }
    void acknowledgeInterrupt() { interruptCounter &= 0x1f; }
    // hsyncWidth is the HSYNC-CRTC length in µsec (characters) as the chip actually
    // runs it, hsyncDelay the per-chip lag before the GATE ARRAY sees that HSYNC
    // (1 µsec on the ASIC's of CRTC 3 and 4). ACCC §9.3.1.
    // betweenCharacters: the HSYNC was raised from an R2.JIT write, which lands BETWEEN
    // two characters rather than from inside tickCharacter. In the ordinary path the
    // arming character's own onCharacter() follows immediately and has to be swallowed;
    // there is no such call to swallow here, so swallowing one would eat a real
    // character and put the mode latch a microsecond late.
    void onHsyncStart(int hsyncWidth = 0, int hsyncDelay = 0, bool betweenCharacters = false, int fallPhase16 = 0);
    // ACCC §9.3.4.1 (p.54): an R3.JIT cuts the HSYNC short, and the pending graphic-mode
    // update goes with it. "If this technique is used on the 2ND µSEC OF THE HSYNC, THE
    // CHANGE OF GRAPHICS MODE DOES NOT TAKE PLACE. The change of MODE is considered only
    // on an R3.JIT during the 3RD µSEC. This implies a non-display time of 2.25 µsec for
    // a mode update." elapsedUsec is the 1-based microsecond the JIT stopped the HSYNC on.
    void onHsyncStoppedByJit(int elapsedUsec);
    void onHsyncCutShort(int elapsedUsec);
    void cancelHsyncStart();
    void onCharacter();
    void onVsyncStart(bool hsyncActive = false, bool gatedByCrtcVsync = false);
    // ACCC §16.1: "When the VSYNC CRTC begins, the GATE ARRAY ... initializes in
    // particular a V26 counter with value 0, which will be incremented at the end
    // of each HSYNC. When V26 reaches 2, the GATE ARRAY activates a composite
    // synchronization C-SYNC signal for the monitor, until V26 reaches 6. This
    // duration represents 4 lines of 64 useconds in a standard case (R0 = 63)."
    // So the monitor's vertical sync starts TWO lines after the CRTC's and lasts
    // FOUR, whatever VSYNC width the CRTC was programmed with. The V26 counter runs
    // whatever the size of the HSYNC encountered.
    // §16.2.1 names the counter: "When the GATE ARRAY receives the signal emitted by
    // the CRTC (when C4==R7), it sets its counter V26 to 0. The black color will be
    // displayed for 26 lines." So V26 runs 0..26: the composite sync occupies lines
    // 2..6 of that span, and the GATE ARRAY blanks its output to black across the
    // whole 26.
    int v26 = -1;                       // -1 = no CRTC VSYNC in progress
    // CRTC 3/4 (§16.2.3, p.164): the C-VSYNC also needs the CRTC's VSYNC pin, which the
    // host reports every character through onCrtcVsyncPin.
    bool cVsyncGatedByCrtc = false;
    bool crtcVsyncPin = false;
    void onCrtcVsyncPin(bool high);
    bool monitorSyncStarted = false;    // set on the V26 0->2 edge, consumed by the host
    bool consumeMonitorSyncStart() { bool v = monitorSyncStarted; monitorSyncStarted = false; return v; }
    bool blanking() const { return cblackVsync; }

    // ---- ACCC §16.2.3 C-SYNC ALGORITHM (p.166) -----------------------------------
    // The monitor has ONE synchronisation input. The CRTC has no connection to it:
    // its HSYNC (pin 14) and VSYNC (pin 15) go to the GATE ARRAY, which combines them
    // into the composite C-SYNC on pin 11 (40007/40008) or pin 5 (40010), and that is
    // what reaches pin 4 of the DIN6. §16.2.2: "C-SYNC=SIG_HSYNC XNOR SIG_VSYNC", the
    // SIG_GA_* are "low (0) when inactive", and "the C-SYNC signal produced by the
    // GATE ARRAY is active when it is low (0)" -- so C-SYNC is active exactly when the
    // two differ, which is why an HSYNC inside a VSYNC comes out inverted (serration)
    // instead of cancelling the vertical pulse.
    //
    // The horizontal half is the part the CRTC cannot give you: the GATE ARRAY does
    // not know C3l, so it counts characters itself in H06 from the HSYNC's rising
    // edge and emits its own 4 µsec pulse from H06==2 to H06==6. A second HSYNC in
    // one line -- ACCC §15.3, and SHAKER's D3 -- resets H06 mid-pulse, which moves and
    // stretches the edge the monitor's flywheel actually sees. Driving the monitor
    // from the CRTC's HSYNC instead made that invisible.
    // ---- the CK16 clock -----------------------------------------------------------
    // ACCC §16.2.2 (p.165): "The GATE ARRAY clocks the CRTC via a clock signal CLK whose
    // period is 5 x 0.0625 µsec high and 11 x 0.0625 µsec low" -- 16 Pixel-M2 to the
    // CRTC's one character. This chip is the faster of the two and everything it times
    // is counted in Pixel-M2, not in characters: the CRTC's character clock is derived
    // from this one, not the other way round.
    static constexpr int PIXELS_PER_CHARACTER = 16;
    void clockPixel();                  // one Pixel-M2 = 0.0625 µsec
    // This chip is the clock master: CK16 comes in at 16 MHz and it divides that down
    // for the CRTC (one character per 16) and the sound chip. Anything else clocked
    // from it -- the monitor's sweep, which sees C-SYNC change WITHIN a character --
    // hangs off this rather than off the CRTC's slower one.
    // The monitor, clocked on every Pixel-M2: a plain function pointer, because this is
    // called 16 million times per emulated second. onPixel is an optional extra observer
    // (the SHAKER runner's recorders, ga-check), called after it.
    void (*pixelSink)(void*) = nullptr;
    void* pixelSinkContext = nullptr;
    std::function<void()> onPixel;
    int h06Pixels = 0;                  // Pixel-M2 since HSYNC-CRTC rose
    long tipTraceBudget = 0;            // diagnostic: log this many C-HSYNC ends
    // §16.2.2: C-HSYNC "becomes active ... before the end of the 2 µs if R3l>=2" and
    // inactive "if the duration of C-HSYNC has reached 4 µsec". 2 µsec and 6 µsec from
    // the HSYNC's rise, in Pixel-M2.
    static constexpr int CHSYNC_RISE_PIXELS = 32;
    static constexpr int CHSYNC_FALL_PIXELS = 96;
    // §16.2.2's other end condition: "1 or 2 Pixel-M2 after the end of the HSYNC
    // signal". The CRTC raises that end on a character boundary, but its pin does not
    // fall there -- §14.4's per-chip residue says where inside the character it does.
    int hsyncPinFallIn = -1;            // Pixel-M2 remaining, -1 = not pending
    // THE ASIC'S C-HSYNC ENDS ON THE PIN, NOT ON ITS LATE BOOKKEEPING. CRTC 3 and 4 pass
    // the end of HSYNC to this chip one usec late (§27.6 measures it on the interrupt), and
    // that lag is right for R52, V26 and the interrupt. But the ASIC delays its WHOLE HSYNC
    // -- §15.1 p.146: "If R2 is 10, on a CPC with 'CRTC' (3 and 4) then HSYNC starts from C0
    // displayed=10 OVER A LENGTH R3 usec" -- and the start is not delayed here (doing so
    // moved every CRTC 3/4 picture a character, +1268 SHAKER error). Delaying only the end
    // stretched every C-HSYNC by a usec: 64 Pixel-M2 wide at R3l=5, where §14.4 has the
    // pulse shrink with R3l "whatever the type", and 48 at R3l=4.
    //
    // So the host arms the pin's fall on time (armCHsyncFall), and the deferred onHsync
    // that follows skips re-arming it. Set by the arm, consumed by that deferred call.
    bool cHsyncFallArmed = false;
    void armCHsyncFall();
    void endCHsync();                   // close the pulse and publish its width
    bool sigGaHsync = false;            // HIGH = active
    bool sigGaVsync = false;
    bool cblackHsync = false;
    // ACCC §14.3 (p.133): the GATE ARRAY times its own HSYNC black, from R3, and does not
    // follow the CRTC's pin. Characters of black left to run.
    int hsyncBlackRemaining = 0;
    bool cblackVsync = false;
    bool vsyncGa = false;
    bool cHsyncStarted = false;         // the SIG_GA_HSYNC rising edge, for the host
    // The width of the C-HSYNC pulse this chip last drove, in 1/16 µsec. It is NOT a
    // whole number of characters: the pulse opens on a character boundary (H06==2) but
    // closes either on H06==6, exactly 4 µsec later, or on the CRTC's HSYNC falling --
    // and that pin falls part-way through a character, by an amount belonging to the
    // CRTC (hsyncFallPhase16). ACCC §14.4's table on p.134 is the source.
    //
    // The monitor is handed this rather than measuring it, because it samples the pin
    // once per character and cannot see a 1/16 µsec residue. It is the true width of
    // the signal this chip drove, not a register read.
    int cHsyncWidth16 = 64;
    int hsyncFallPhase16 = 0;           // this CRTC's sub-character fall, from the host
    // The FALLING edge of C-HSYNC. ACCC §14.3/§14.4: the sweep starts where the pulse
    // ends, which is why a pulse that stops early moves the picture right. A renderer
    // homed on the pulse's start has to correct for a width it cannot know yet; one
    // homed here needs no correction at all.
    bool cHsyncEnded = false;
    bool consumeCHsyncEnd() { bool v = cHsyncEnded; cHsyncEnded = false; return v; }
    bool consumeCHsyncStart() { bool v = cHsyncStarted; cHsyncStarted = false; return v; }
    // C-SYNC as the monitor sees it: active low, so active when the two signals differ.
    bool csyncActive() const { return sigGaHsync != sigGaVsync; }
    void onHsync() { onHsync(cpu); }
    void onHsync(Z80* cpu);

    // ---- ACCC §9.1: the pixel pipeline -------------------------------------------
    // "The GATE ARRAY/ASIC reads the data pointed to by the CRTC from memory in order
    // to convert and display it as pixels." Implemented in gate_array_pixels.cpp. The
    // renderer used to do all of this; it is a consumer of RGB and should not know how
    // a VRAM byte becomes a pen. The 40007/40008-vs-40010 padding difference is taken
    // from `model` here rather than passed in by whoever is calling.
    int videoByte(int address) const;
    int penFromRotation(int mode, int byte, int rotation) const;
    void bytePens(int byte, int modeBefore, int modeAfter, int switchPixel, int pens[8]) const;
    std::array<int, 2> mode0Pens(int byte) const;
    std::array<int, 4> mode1Pens(int byte) const;
    int pixelPen(int mode, int byte, int pixel) const;
};

} // namespace cpcse
