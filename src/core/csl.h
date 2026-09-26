// CPCSyntaxError — CSL script player with SSM screenshots.
//
// Plays a CPC Script Language file (Longshot's CSL standard, v1.5) against a GX4000 and
// answers the SSM codes (ScreenShot Management, v1.1) the emulated program emits: the
// Z80 bytes ED LL ED HH, which the CPU reports through Z80::onUnwiredEd. SHAKER uses
// both, so its CSL scripts drive every test and each screen it wants kept names itself
// with an SSM code.
//
// The player owns nothing: it is handed a machine and a renderer, installs its SSM hook
// on the CPU for its own lifetime, and builds the machine the script configures
// (cpc_model, crtc_select, gate_array, memory_exp, rom_config) at each hard reset.
//
// Every CSL instruction of v1.5 is handled. What the machine cannot do is reported as an
// error, as the standard asks: the script, the line, the instruction, the reason, the
// script's CSL version and the supported one.
#pragma once
#include "common.h"
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cpcse {

class GX4000;
class CpcVideo;

struct CslSettings {
    std::string romDir = "roms";          // firmware and system cartridge when rom_dir is not given
    std::string diskDir;                  // default disk_dir ("" = the script's folder / cwd)
    std::string tapeDir;                  // default tape_dir
    std::string snapshotDir;              // default snapshot_dir, for loading and saving
    std::string screenshotDir = ".";      // default screenshot_dir
    // SSM image names are <emulatorName>_<crtcTag><crtc>_<HHLL>.bmp. The SSM standard
    // suggests <Emulator name>_<CRTC number>_<HHLL code>, i.e. an empty tag.
    std::string emulatorName = "CPCSE";
    std::string crtcTag;
    int defaultModel = 2;                 // cpc_model when the script names none (2 = 6128)
    int forceCrtc = -1;                   // >= 0 overrides every crtc_select (5 = CRTC 1-B)
    bool followCslLoad = true;            // csl_load runs the named script
    // crtc_select 3 with no cpc_model builds a 6128 Plus, the only machine with that
    // CRTC. true builds a classic 6128 with the CRTC 3 profile instead (for comparison).
    bool classicCrtc3 = false;
    double timeCapSeconds = 0;            // emulated seconds per script; 0 = from its waits
    // Corrections to published scripts that cannot work as written (see cslErrata in
    // csl.cpp). Each is logged when it applies.
    bool errata = true;
    std::string logPath;                  // the log of this CSL run ("" = none)
    bool echo = true;                     // log lines also go to stdout
};

class CslPlayer {
public:
    static constexpr const char* SUPPORTED_VERSION = "1.5";

    CslPlayer(GX4000& emu, CpcVideo& video, CslSettings settings);
    ~CslPlayer();

    // Plays the script (and, with followCslLoad, the scripts it chains). false = stopped
    // on an error; errorReport() says why in the standard's six points.
    bool run(const std::string& path);
    const std::string& errorReport() const { return report; }

    // Builds the machine the configuration describes, as a power-on. Public so a host
    // can start from the same machine without a script.
    bool hardReset(std::string& why);

    int crtcType() const { return crtc; }                  // the CRTC type now fitted
    long long clockOrigin() const { return origin; }       // machineCycles at the last reset
    int screenshots() const { return shotCount; }
    int snapshots() const { return snapCount; }

    // Host hooks. onMachineBuilt runs after every machine build, before the machine
    // runs (a host plugs its monitor in there). beforeScreenshot runs before a picture is
    // rendered for a screenshot, onScreenshot after it is written, both with its SSM code
    // (-1 for the screenshot instruction). onPicture runs at
    // every completed monitor picture that did not produce a screenshot.
    std::function<void()> onMachineBuilt;
    std::function<void(int code)> beforeScreenshot;
    std::function<void(int code, const std::string& path)> onScreenshot;
    std::function<void()> onPicture;
    // After every instruction: where it started and the machine T-states it took.
    std::function<void(int pc, long long tStates)> onStep;

private:
    struct Stroke { std::vector<std::pair<int, int>> cells; bool isReturn = false; bool noDelayAfter = false; };
    struct Script { std::string path, dir, versionText; int line = 0; std::string instruction; };

    GX4000& emu;
    CpcVideo& video;
    CslSettings settings;

    // machine configuration (applied at the next hard reset)
    int model = -1;                       // cpc_model, -1 = not given
    int crtcSelected = -1;                // crtc_select, -1 = not given
    int gateArray = 0;                    // gate_array, 0 = the machine's own
    int memoryExpansion = -1;             // memory_exp, -1 = none
    std::string romDirPrefix;             // rom_dir
    std::string lowerRomFile, cartridgeFile, multifaceFile;
    std::map<int, std::string> upperRomFiles;
    bool machineBuilt = false, configChanged = false;
    bool plusMenu = false;                // a Plus computer on its system cartridge: boots to the menu
    int crtc = 1;

    // directories (prefixes, concatenated before a name as the standard says)
    std::string diskDir, tapeDir, snapshotDir, screenshotDir;
    std::string screenshotName, snapshotName;
    int snapshotVersion = 3;

    // keyboard timing (usec)
    double keyPress = 19968, keyGap = 19968, keyAfterOutput = 19968;   // key_delay: press, gap, after the key_output

    // clocks, in machine T-states
    long long origin = 0;                 // machineCycles at the last reset
    long long scriptT = -1;               // the script's own time since origin
    long long capAt = 0;                  // machineCycles at which the running script is stopped

    // SSM
    int pendingLow = -1, pendingNextAddress = -1;
    std::vector<int> pendingShots;        // SSM codes waiting for the picture they end
    bool pendingSnapshot = false;         // SSM #FFFF, taken after its instruction
    bool ssm0000Seen = false;
    // CSL 1.5's wait_ssm <code>: the code being waited for, and whether it has arrived.
    // Every code received since the last wait_ssm returned: a code can arrive while the
    // PREVIOUS instruction is still running (a key_output's own post-key delay), and it
    // still ends the wait.
    std::vector<int> ssmSinceWait;
    int shotCount = 0, snapCount = 0, unnamedShots = 0;

    std::vector<Script> stack;
    std::string report;
    std::FILE* log = nullptr;

    bool runFile(const std::string& path);
    bool execute(const std::string& command, const std::vector<std::string>& args, const std::string& raw, std::string& why);
    bool ensureMachine(std::string& why);
    bool softReset(std::string& why);
    void plusMenuToBasic();
    void machineStarted();
    void onSsm(int suffix, int pcAfter);

    // machine time
    long picture() const;
    void stepOne();
    bool advanceTo(long long target, std::string& why);
    bool advanceUntil(const std::function<bool()>& done, std::string& why);
    bool wait(double usec, std::string& why);
    void reanchor();

    // exports
    std::string ssmName(int code) const;
    std::string resolveOut(const std::string& prefix, const std::string& name) const;
    bool takeScreenshot(int code, std::string& why);
    bool takeSnapshot(const std::string& path, std::string& why);
    void handlePicture();

    // keyboard
    bool parseKeys(const std::string& text, bool fromFile, std::vector<Stroke>& out, std::string& why) const;
    bool typeStrokes(const std::vector<Stroke>& strokes, std::string& why);

    // files
    std::string findFile(const std::string& prefix, const std::string& name, const std::string& fallbackDir) const;
    std::string findFirmware(const std::string& keyword) const;
    std::string findSystemCartridge() const;

    void say(const char* format, ...);
    double seconds() const;
};

// Writes a 32bpp BMP of a CpcVideo frame (pixels are 0xAABBGGRR).
bool writeVideoBmp(const std::string& path, const std::vector<uint32_t>& pixels, int width, int height);

} // namespace cpcse
