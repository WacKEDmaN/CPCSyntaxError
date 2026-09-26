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
class CpcVideo;
struct M4Drive;

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
    float masterVolume = 0.6f;
    int sampleRate = 44100;

    // input / expansion options
    bool joystickEnabled = true;
    std::string keyboardRegion = "uk";      // uk / fr / es
    std::string dacType = "none";           // none / digiblaster / amdrum

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
    bool tapePlaying = false;

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
    // Symbiface mouse + RTC. NOTE: the Symbiface hard-disk/CF interface is not
    // emulated — only the SF2/SF3 mouse and RTC. There is no HDD image to load.
    std::string symbifaceModule = "none";   // none / sf2 / sf3
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
    bool loadTapeFile(const std::string& path);
    void tapePlayToggle();
    void tapeRewind();

    bool saveSnapshot(const std::string& path);
    bool loadSnapshot(const std::string& path);

    void reset();
    void stepInstruction();
    void runFrame();                         // advance one frame + drain audio

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
    void setDacType(const std::string& type);

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
    int  readMem(int addr) const;            // for the memory viewer
    bool loadByExtension(const std::string& path);   // drag-and-drop dispatch

    static Bytes readFile(const std::string& path, bool& ok);

private:
    M4Drive* m4Drive = nullptr;              // owned; a host-folder-backed M4 drive
    void drainAudio();
    void applySettings();                    // re-apply region/joystick/DAC after a boot
    void applyExpansionRoms();               // re-fit slot ROMs after a boot
    void applyM4();                          // re-enable M4 after a boot
    void applySymbiface();
    std::string findRom(const std::string& keyword) const;
    std::string findCart(const std::string& keyword) const;   // search mediaDir/romDir for a .cpr
    void applyAudioRate();
};

} // namespace cpcse
