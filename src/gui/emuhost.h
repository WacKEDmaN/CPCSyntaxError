// CPCSyntaxError GUI — emulator host.
//
// Wraps the headless GX4000 core with everything a desktop front end needs:
// firmware/ROM discovery, media loading (cartridge / disk / tape / snapshot),
// framing, audio draining, host-key translation and a small pile of settings
// the UI edits directly. It owns exactly one GX4000 and one CpcVideo; changing
// model/RAM/CRTC reconfigures the same machine via loadClassicFirmware().
#pragma once
#include <SDL.h>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include "core/common.h"

namespace cpcse {

class GX4000;
class CPCTapeDrive;
struct Disk;
class CpcVideo;
class MatrixPrinter;
struct V9990Picture;

struct ModelProfile {
    std::string id;
    std::string label;
    std::string osKey;      // case-insensitive filename keyword for the OS ROM
    std::string basicKey;   // …and the BASIC ROM
    bool amsdos = false;    // needs AMSDOS.ROM + a floppy controller
    int defaultRam = 64;
    int defaultCrtc = 1;
    bool plus = false;         // a CPC Plus: boots from a system cartridge (ASIC on)
    std::string cartKey;       // keyword to locate the Plus system cartridge (.cpr)
    bool available = false;    // ROMs / cartridge were found
    bool computer = true;      // true = has a keyboard (464/6128 + Plus); false = GX4000 console
};

class EmuHost {
public:
    GX4000* emu = nullptr;
    CpcVideo* video = nullptr;

    // configuration the UI edits
    std::string romDir = "roms";
    std::string mediaDir = "media";   // where cartridges (incl. the Plus system cart) live
    std::vector<ModelProfile> models;
    int currentModel = -1;
    int ramKiB = 64;
    int crtcType = 1;
    // WHICH MONITOR IS PLUGGED IN (src/core/monitor_model.h), by id -- "" means the set
    // the machine shipped with, which is the only pairing ACCC 15.1 calls centred.
    std::string monitorSetId;
    // WHICH GATE ARRAY IS FITTED (src/core/gate_array_model.h): 40007, 40008 or 40010 on a
    // classic machine -- ACCC 9 (p.46) names all three in 464s and 6128s. 0 is the 40010,
    // the part most 6128s carry. On CRTC 3/4 there is no choice: the ASIC (40489/40226)
    // IS the GATE ARRAY, so this is ignored there.
    int gateArrayPart = 0;
    // ...and an explicit tube override for looking at a colour program in green or grey.
    // "" takes the tube from the set, which is the physical answer.
    std::string monitorMode;
    bool paused = false;
    bool audioEnabled = true;
    // The machine's mechanical noises (core/machine_sounds.h): the disc drive's motor,
    // head steps and insert, and the keyboard's clicks. Not the CPC's sound chip.
    bool driveSounds = true;
    bool keySounds = false;
    float mechanicsVolume = 0.5f;
    std::array<bool, 512> keyHeld{};   // SDL scancodes down, so a key repeat does not click
    float masterVolume = 0.6f;
    int sampleRate = 44100;

    // input / expansion options
    bool joystickEnabled = true;
    std::string keyboardRegion = "uk";      // uk / fr / es
    // THE PRINTER PORT holds one device at a time: none / digiblaster / amdrum (sound DACs)
    // / printer (a text printer) / matrix (an Epson-style dot-matrix printer, page image).
    std::string dacType = "none";
    // THE JOYSTICK PORT'S LIGHTGUN: none / trojan (Trojan Light Phazer, on the CRTC's light
    // pen input) / gunstick (Loriciel Gunstick) / westphaser (Loriciel West Phaser). Aimed
    // with the mouse over the Screen window, left button to fire.
    std::string lightgunType = "none";
    // GFX9000: the Yamaha V9990 cartridge at &FF60. Its picture goes to its own monitor --
    // the GFX9000 window.
    bool v9990Enabled = false;
    // AN OPL4 SOUND CARD on the AMSDAP (YMF278B: OPL3 FM + wavetable, &FFC4-7/&FF7E-F),
    // MoonSound-style. Its General MIDI samples are Yamaha's YRW801 ROM (yrw801*.rom in
    // the ROM folder); without it the wavetable plays only what programs load into RAM.
    bool opl4Enabled = false;
    int opl4RamKiB = 2048;
    bool playCityEnabled = false;      // TotO's PlayCity (two YMZ294 + a Z80 CTC)
    std::string speechKind = "none";   // "none" / "ssa1" / "dktronics" / "lambdaspeak3" (core/speech.h)
    std::string mp3Card;               // LambdaSpeak 3's MP3 module: its micro-SD card folder
    bool opl4HasRom() const;
    bool m4HasRom() const { return !findRom("m4rom").empty() || !findRom("m4").empty(); }
    // Copies a ROM file into the ROM folder as destName, after checking its size; the
    // reason on failure.
    bool installRom(const std::string& source, const std::string& destName, size_t size, std::string& why);
    // Where its monitor is: "beside" the CPC's in the Screen window, in a "window" of its
    // own, or "switch" -- one monitor, showing the GFX9000's picture while it displays one
    // and the CPC's otherwise, as a Video9000 passes the computer's video through.
    std::string gfx9000Monitor = "switch";   // + "video9000": one monitor through a Video9000
    // One monitor, and the GFX9000 has it now: its display on, and no program has handed
    // the picture back through P#F (&FF6F bit 4, as Video9000 programs do on exit).
    bool gfx9000OnMainScreen() const;
    // THE VIDEO9000 (Sunrise; its manual, docs/reference/Video9000-manual.pdf, ch.5): the
    // GFX9000's genlock and superimposer, its control register at &FF6F --
    //   b6 S1, b5 S0  input: 0x = RGB (the computer's own picture), 10 = CVBS, 11 = S-VHS
    //   b4 GEN        genlock on (off: the GFX9000's picture alone)
    //   b3 TRAN       0: the GFX9000 wholly transparent (the input alone); 1: where its YS
    //                 bits say (R#8 YSE on, palette R bit 7 / BD16 bit 15 / BD8 dot 0)
    //   b1 YMIX       the two mixed, each at half brightness (appendix B, MIX)
    //   b0 YM         half-tone: the input at half brightness where it shows through
    // At power-on &10 (p.13): genlock on, the GFX9000 transparent -- the CPC's picture.
    // The monitor's picture, at the GFX9000's own geometry (false: nothing to compose).
    bool video9000Picture(std::vector<uint32_t>& out, int& width, int& height) const;
    int video9000Control() const;
    // The host mouse is grabbed for the CPC's mouse (click the picture; F12 releases).
    bool mouseCaptured = false;
    // The tape deck: whether the tape follows the cassette motor relay (as on a CPC), and
    // the relay's start-up delay.
    bool tapeFollowsMotor = true;
    bool tapeRelayDelay = true;

    // speed / display preferences (read by the shell)
    float speed = 1.0f;                     // 0.25 .. 4.0
    bool turbo = false;                      // run uncapped
    bool integerScale = false;
    bool maintainAspect = true;
    bool fullscreen = false;
    bool crtEffect = false;      // scanlines + bloom overlay
    float crtScanline = 0.5f;
    float crtBloom = 0.2f;

    // media state (for display)
    std::string diskName[2];
    std::string tapeName;
    std::string cartName;
    std::string snapshotName;               // the .SNA last loaded ("" once the machine reboots)

    // firmware ROM overrides (empty = auto-find by keyword). Let the user see
    // exactly which files are installed and swap them.
    std::string osRomPath, basicRomPath, amsdosRomPath;   // override full paths
    std::string osRomName, basicRomName, amsdosRomName;    // resolved filenames (display)

    // expansion ROMs fitted into upper-ROM slots (persist across reboots)
    struct FittedRom { int slot; std::string name; Bytes data; };
    std::vector<FittedRom> expansionRoms;

    // M4 board (SD storage backed by a host folder) — classic CPC only.
    bool m4Enabled = false;
    std::string m4Folder;
    bool m4Network = true;       // its WiFi is the host's network (off: no network at all)
    bool m4NetworkLan = false;   // programs that listen take connections from other computers
    void applyM4Network();
    // Symbiface II / III: mouse, RTC and the IDE/CF interface.
    std::string symbifaceModule = "none";   // none / sf2 / sf3
    // Both cards' IDE/CF interface: this folder is its disc ("symide" beside the program,
    // set by main). Made when the card is turned on and the folder is not there yet.
    std::string ideFolder;
    void setIdeFolder(const std::string& folder);
    void rescanIde();
    float mouseSensitivity = 1.0f;

    // last operation status line for the UI
    std::string status = "No machine booted.";

    // drained audio for the current frame (interleaved stereo S16)
    std::vector<int16_t> audioOut;

    EmuHost();
    ~EmuHost();

    void setRomDir(const std::string& dir);
    void scanModels();                       // mark which profiles have ROMs
    bool bootModel(int index, int ram = -1, int crtc = -1);
    bool booted() const { return currentModel >= 0 || !cartName.empty(); }

    bool loadCartridgeFile(const std::string& path);
    bool loadDiskFile(const std::string& path, int unit);
    void ejectDisk(int unit);
    // The DSK editor's disc into drive A (0) or B (1), shared: what the CPC writes, the
    // editor sees, and the other way round.
    bool insertDisk(std::shared_ptr<Disk> disk, int unit, const std::string& name);
    std::shared_ptr<Disk> driveDisk(int unit) const;
    // Where the disc in a drive came from (empty for one made in the editor).
    std::string diskPath[2];
    bool loadTapeFile(const std::string& path);
    // THE TAPE DECK (core/tape.h): its buttons. Whether it plays is asked of the deck --
    // a tape stops by itself too (a stop block, its end).
    CPCTapeDrive* tapeDeck() const;           // null with no tape in
    bool tapePlaying() const;                 // PLAY down and not paused
    bool tapePaused() const;
    void tapePlay();
    void tapePause();                         // toggles PAUSE
    void tapeStop();
    void tapeRewind();                        // to the start
    void tapeRewindBlock();                   // to this block's start, or the one before
    void tapeFastForward();                   // to the next block
    void tapeSeekBlock(int index);
    void tapeEject();
    void tapeResetCounter();

    // Out of the slot: the machine as it boots without it (a Plus on its system
    // cartridge). false when there is none.
    bool ejectCartridge();
    // The snapshot let go: the machine booted afresh. false when none is loaded.
    bool ejectSnapshot();
    bool saveSnapshot(const std::string& path);
    bool loadSnapshot(const std::string& path);

    void reset();
    void stepInstruction(bool redraw = true);   // false: leave the picture to the caller (many steps)
    void runFrame();                         // advance one frame + drain audio
    uint64_t framesRun = 0;                  // frames runFrame has run (not counting paused calls)

    // Debugger hooks. A breakpoint, a watchpoint or a reached step target stops the
    // machine by setting `paused` -- the same pause the UI's Pause shows -- and says why
    // in `breakReason`. stopAfter, when set, is asked after every instruction (with the
    // PC that instruction started at) and stops the machine once it answers true; it is
    // cleared when it fires. watchArmed arms the memory watch hook while the machine runs.
    std::function<bool(int previousPc)> stopAfter;
    bool watchArmed = false;
    std::string breakReason;
    // Where the instruction being executed began. The Z80's PC has moved past its
    // operands by the time it touches memory, so a watchpoint names the instruction by
    // this. Kept only while stopAfter or a watchpoint is armed (-1 otherwise).
    int instructionPc = -1;
    void render();                           // CpcVideo::render() into video->pixels
    double cpcRefreshHz = 50.0;              // emulated CPC display refresh (VSYNC rate)
    bool beamRenderer = false;               // beam-driven CRT renderer (pin-accurate)
    void setBeamRenderer(bool on);           // classic path only; Plus keeps legacy

    void setKey(SDL_Scancode sc, bool pressed);
    void setMonitorMode(const std::string& mode);
    // Plug a set in, by monitor_model.h id; "" restores the one the machine shipped with.
    void setMonitorSet(const std::string& id);
    void applyMonitorSet();
    // Fit a GATE ARRAY part (40007/40008/40010, 0 = the 40010). Live: the chip reads its
    // model on every pixel, so no reset is needed.
    void setGateArrayPart(int part);
    void applyGateArrayPart();
    int fittedGateArrayPart() const;   // what is actually fitted, ASIC included

    void setJoystickEnabled(bool on);
    void setKeyboardRegion(const std::string& region);
    void setDacType(const std::string& type);   // the printer-port device
    bool printerAttached() const { return dacType == "printer" || dacType == "matrix"; }

    // Printer output. The text printer's characters, and the dot-matrix printer's pages.
    std::string printerText;
    MatrixPrinter* matrixPrinter = nullptr;      // owned; created with the host
    int printerRevision = 0;                     // bumps on every byte printed
    void clearPrinter();
    bool savePrinterText(const std::string& path) const;
    bool savePrinterPageBmp(const std::string& path) const;
    bool savePrinterPageSvg(const std::string& path) const;

    // Lightgun.
    void setLightgun(const std::string& type);
    bool lightgunActive() const { return lightgunType != "none"; }
    // Where the gun points, in screen-framebuffer pixels (x < 0: off the screen), and
    // whether the trigger is held.
    void lightgunAim(double x, double y, bool trigger);

    // GFX9000 (V9990).
    void setV9990(bool on);
    void setOpl4(bool on);
    void setPlayCity(bool on);
    void setSpeech(const std::string& kind);
    void applySpeech();
    bool speechHasRom() const { return !findRom("sp0256 al2").empty(); }
    void applyPlayCity();
    void applyOpl4();
    // The V9990's own monitor: its last field, or null while it shows nothing (not
    // fitted, in stand-by, or never displayed).
    const V9990Picture* v9990Picture() const;

    // Tape deck options.
    void applyTapeOptions();

    // CPC Plus analogue port (ASIC ADC0-7, &6808-&680F): 0..63 per channel.
    int analogue(int channel) const;
    void setAnalogue(int channel, int value);

    bool fitExpansionRom(int slot, const std::string& path);
    void clearExpansionRom(int slot);
    std::string expansionRomName(int slot) const;   // "" if none fitted

    // firmware ROM manager: which = 0 OS / 1 BASIC / 2 AMSDOS. Overriding or
    // resetting re-boots the current model so the change takes effect.
    void setFirmwareRom(int which, const std::string& path);
    void clearFirmwareRom(int which);

    bool setM4(bool enabled, const std::string& folder);
    void rescanM4();

    void setSymbiface(const std::string& module);    // none / sf2 / sf3
    void setMouseSensitivity(float s);
    bool symbifaceMouseActive() const;
    void mouseMove(float dx, float dy);
    void mouseButton(int index, bool pressed);
    void mouseScroll(float delta);

    bool saveScreenshotBmp(const std::string& path);
    bool loadByExtension(const std::string& path);   // drag-and-drop dispatch

    static Bytes readFile(const std::string& path, bool& ok);

private:
    void drainAudio();
    void applySettings();                    // re-apply region/joystick/DAC after a boot
    void applyExpansionRoms();               // re-fit slot ROMs after a boot
    void applyM4();                          // re-enable M4 after a boot
    void applySymbiface();
    void applyLightgun();
    void applyPrinter();
    std::string findRom(const std::string& keyword) const;
    std::string findCart(const std::string& keyword) const;   // search mediaDir/romDir for a .cpr
    void applyAudioRate();
};

} // namespace cpcse
