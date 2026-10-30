// CPCSyntaxError — desktop front end.
//
// SDL2 + OpenGL + Dear ImGui (docking branch) around the GX4000 core. The UI itself
// -- main menu bar, dockable windows, status bar -- is GuiShell (gui_shell.cpp); this
// file owns the headless CLI, the SDL window, audio, pads and the frame pacing.
// Emulation is paced to 50 Hz off the wall clock; the display refreshes at the
// monitor's rate.
#include <SDL.h>          // SDL renames main()->SDL_main; SDL2main provides the
                          // real WinMain, so this links as a GUI-subsystem exe.
#include <SDL_opengl.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <SDL_syswm.h> // Must come AFTER <SDL.h> and <windows.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "cpc_typing.h"
#include "devserver.h"
#include "emuhost.h"
#include "c_build.h"
#include "version.h"
#include "core/machine_sounds.h"
#include "gui_shell.h"
#include "gui_widgets.h"
#include "../core/monitor_model.h"
#include "../core/monitor_renderer.h"
#include "core/emulator.h"
#include "core/csl.h"
#include "core/v9990.h"
#include "core/symbiface_mouse.h"
#include "core/gamepad.h"
#include "core/crtc.h"
#include "core/keyboard.h"
#include "keymap.h"
#include "core/emulator.h"
#include "core/tape.h"
#include "core/video.h"
#include "core/z80.h"
#include "core/memory.h"
#include "core/crtc.h"
#include "core/gate_array.h"
#include "core/asic.h"
#include "core/fdc.h"
#include "core/dsk.h"
#include "core/disassembler.h"


using namespace cpcse;

namespace {

// Where the program keeps what it writes (cpcse.ini, the window layout, the IDE's disc):
// beside the program, or the folder CPCSE_HOME names (the Flatpak sets it, because the
// program's own folder is read-only there). Ends with a separator, or is "".
std::string homeDir() {
    if (const char* h = std::getenv("CPCSE_HOME"); h && *h) {
        std::string p = h;
        return p.back() == '/' || p.back() == '\\' ? p : p + "/";
    }
    char* b = SDL_GetBasePath();
    std::string p = b ? b : "";
    if (b) SDL_free(b);
    return p;
}

std::string iniFilePath() { return homeDir() + "cpcse.ini"; }

// The window where it was last time (x, y: its picture's top-left; `placed` false: centred),
// kept whole -- title bar and frame too -- inside the usable part of a display, the part no
// taskbar covers, wherever the taskbar is. A display that has gone, or one too small for
// the old size now, gets the window centred, or shrunk to fit.
void placeWindow(SDL_Window* window, bool placed, int x, int y, int w, int h) {
    int display = -1;
    for (int d = 0; placed && d < SDL_GetNumVideoDisplays(); d++) {
        SDL_Rect b;
        const SDL_Point centre{ x + w / 2, y + h / 2 };
        if (SDL_GetDisplayBounds(d, &b) == 0 && SDL_PointInRect(&centre, &b)) { display = d; break; }
    }
    if (display < 0) { placed = false; display = std::max(0, SDL_GetWindowDisplayIndex(window)); }
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(display, &usable) != 0) return;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right) != 0) { top = 32; left = right = bottom = 8; }   // not known: a usual frame
    const int roomW = usable.w - left - right, roomH = usable.h - top - bottom;
    w = std::max(320, std::min(w, roomW));
    h = std::max(240, std::min(h, roomH));
    const int minX = usable.x + left, minY = usable.y + top;
    if (!placed) { x = minX + (roomW - w) / 2; y = minY + (roomH - h) / 2; }
    x = std::max(minX, std::min(x, minX + roomW - w));
    y = std::max(minY, std::min(y, minY + roomH - h));
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, x, y);
}

// The drive's and the keyboard's recordings (core/machine_sounds.h), beside the program.
// Beside the program first, then the working directory (a build directory, or a Linux
// install whose binary sits elsewhere), then ../share/cpcsyntaxerror beside the binary.
void loadMachineSounds() {
    char* b = SDL_GetBasePath();
    std::string p = b ? b : "";
    if (b) SDL_free(b);
    for (const std::string& dir : { p + "sounds", std::string("sounds"), p + "../share/cpcsyntaxerror/sounds" })
        if (machineSounds.load(dir) > 0) return;
}

// "R,G,B" (as --drive/--bias and the .ini keep the guns) into three ints; a missing or
// short value leaves the rest as they are.
void parseTriple(const std::string& text, std::array<int, 3>& out) {
    size_t at = 0;
    for (size_t k = 0; k < 3 && at <= text.size(); k++) {
        const size_t comma = text.find(',', at);
        const std::string part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!part.empty()) out[k] = std::atoi(part.c_str());
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
}

std::map<std::string, std::string> parseIni(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line;
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
}
void enableDarkTitleBar(SDL_Window* window) {
#ifdef _WIN32
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        if (wmInfo.subsystem == SDL_SYSWM_WINDOWS) {
            HWND hwnd = wmInfo.info.win.window;
            BOOL useDarkMode = TRUE;

            // Dynamically load dwmapi.dll to avoid linker errors on GCC/MinGW
            HMODULE hDwm = LoadLibraryA("dwmapi.dll");
            if (hDwm) {
                typedef HRESULT (WINAPI *pfnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
                auto setAttr = reinterpret_cast<pfnDwmSetWindowAttribute>(reinterpret_cast<void (*)()>(GetProcAddress(hDwm, "DwmSetWindowAttribute")));
                if (setAttr) {
                    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 (Win11 / Win10 20H1+), fallback to 19 (older Win10)
                    if (FAILED(setAttr(hwnd, 20, &useDarkMode, sizeof(useDarkMode)))) {
                        setAttr(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
                    }
                }
                FreeLibrary(hDwm);
            }
        }
    }
#else
    (void)window;
#endif
}
} // namespace

// cpcse.exe is a GUI-subsystem program, so it has no console of its own: run from a
// Command Prompt, the headless modes' output would go nowhere. They attach to the
// console they were started from -- unless their output is already going somewhere
// (a pipe, a file), which is left alone.
static void attachParentConsole() {
#ifdef _WIN32
    const DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::printf("\n");               // the prompt has already been printed
    }
#endif
}

// ------------------------------------------------------------------ entry point
int main(int argc, char** argv) {
    // CSL mode: play a CPC Script Language file with no window, writing the screenshots
    // its SSM codes ask for. Nothing of it -- not even the CPU's SSM hook -- exists unless
    // --csl is on the command line.
    //   --csl <file> [--out <dir>] [--roms <dir>] [--model <id>] [--crtc <n>]
    //   [--disk-dir <dir>] [--tape-dir <dir>] [--emu-name <name>] [--csl-log <file>] [--no-chain]
    //   [--no-errata]   (play published scripts exactly as written; see cslErrata in csl.cpp)
    {
        std::string cslPath, outDir = "screenshots", romDir, modelId, emuName = "CPCSE", logPath, diskDir, tapeDir;
        int forceCrtc = -1;
        bool chain = true, errata = true;
        for (int i = 1; i < argc; i++) { std::string a = argv[i];
            if (a == "--csl" && i + 1 < argc) cslPath = argv[++i];
            else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
            else if ((a == "--roms" || a == "-r") && i + 1 < argc) romDir = argv[++i];
            else if (a == "--model" && i + 1 < argc) modelId = argv[++i];
            else if (a == "--crtc" && i + 1 < argc) forceCrtc = std::atoi(argv[++i]);
            else if (a == "--emu-name" && i + 1 < argc) emuName = argv[++i];
            else if (a == "--csl-log" && i + 1 < argc) logPath = argv[++i];
            else if (a == "--disk-dir" && i + 1 < argc) diskDir = argv[++i];
            else if (a == "--tape-dir" && i + 1 < argc) tapeDir = argv[++i];
            else if (a == "--no-chain") chain = false;
            else if (a == "--no-errata") errata = false; }
        if (!cslPath.empty()) {
            attachParentConsole();
            if (romDir.empty()) {
                // The roms folder in the working directory, else the one beside the exe.
                std::error_code ec;
                romDir = "roms";
                if (!std::filesystem::is_directory(romDir, ec)) {
                    char* base = SDL_GetBasePath();
                    if (base) { romDir = std::string(base) + "roms"; SDL_free(base); }
                }
            }
            EmuHost host;          // the front end's own machine and renderer, windowless
            loadMachineSounds();
            CslSettings cs;
            cs.romDir = romDir;
            cs.screenshotDir = outDir;
            cs.snapshotDir = outDir;
            cs.diskDir = diskDir;
            cs.tapeDir = tapeDir;
            cs.emulatorName = emuName;
            cs.followCslLoad = chain;
            cs.errata = errata;
            cs.forceCrtc = forceCrtc;
            cs.logPath = logPath.empty() ? outDir + "/csl.log" : logPath;
            // The machine a script gets when it names none (CSL cpc_model numbers).
            if (modelId == "cpc464") cs.defaultModel = 0;
            else if (modelId == "cpc664") cs.defaultModel = 1;
            else if (modelId == "cpc6128plus") cs.defaultModel = 4;
            else if (modelId == "cpc464plus") cs.defaultModel = 5;
            else if (modelId == "gx4000") cs.defaultModel = 6;
            CslPlayer player(*host.emu, *host.video, cs);
            return player.run(cslPath) ? 0 : 1;      // an error is reported, and logged, by the player
        }
    }
    // Headless screenshot: exercise the exact EmuHost boot/render path the GUI
    // uses, with no window. --shot out.bmp [--model id] [--sna f] [--crtc N] [--gate-array 40007|40008|40010]
    // [--frames N] [--beam] [--gfx9000] [--v9990-shot out.bmp] (the GFX9000's own monitor).
    {
        std::string shot, modelId, snaPath, saveSnaPath, dumpRamPath, diskPath, cartPath, tapePath, typeStr, keysStr; int croGroup = -1, wantCrtc = -1, wantGa = 0, frames = 200, f1at = -1, wantRam = -1, seq = 0; bool beam = false, diag = false, gfx9000 = false, opl4 = false, playcity = false; std::string v9990Shot, video9000Shot, m4Folder, ideFolder, mouseScript, mouseInterface, cbuildPath, cbuildCpct, cbuildSdcc, wavPath, speechKind, mp3Card, dacType; int cbuildLoc = -1; bool cbuildForce = false; std::string cbuildZ88dk, sf3Usb, sf3Sd; bool cbuildSccz80 = false; bool cbuildUseZ88dk = false; bool sf2 = false, sf3 = false, xmass = false, multiface = false, diskWriteBack = false; int monitorHHold = std::getenv("CPCSE_MONITOR_HHOLD") ? std::atoi(std::getenv("CPCSE_MONITOR_HHOLD")) : 0; int monitorVHold = std::getenv("CPCSE_MONITOR_VHOLD") ? std::atoi(std::getenv("CPCSE_MONITOR_VHOLD")) : 0; std::vector<std::string> xroms; std::string serialLine, recordTape, tapeFast, recordPath, recordFormat = "mjpeg", recordSource = "shown", gfx9000Monitor; bool rasterOverlayOn = false; bool z80Cmos = false, gx4000Ram128 = false, plusExpansionClassic = true; std::string monitorSet; TubeKnobs monitorKnobs; std::array<int, 3> monitorIntensity = TubeKnobs().intensity;
        for (int i = 1; i < argc; i++) { std::string a = argv[i];
            if (a == "--shot" && i + 1 < argc) shot = argv[++i];
            else if (a == "--ram" && i + 1 < argc) wantRam = std::atoi(argv[++i]);
            else if (a == "--model" && i + 1 < argc) modelId = argv[++i];
            else if (a == "--sna" && i + 1 < argc) snaPath = argv[++i];
            else if (a == "--savesna" && i + 1 < argc) saveSnaPath = argv[++i];   // dump RAM at the end
            else if (a == "--dumpram" && i + 1 < argc) dumpRamPath = argv[++i];  // raw RAM, no header
            else if (a == "--disk" && i + 1 < argc) diskPath = argv[++i];
            else if (a == "--cart" && i + 1 < argc) cartPath = argv[++i];
            else if (a == "--cro-group" && i + 1 < argc) croGroup = std::atoi(argv[++i]);   // the CRO group (GNUM) --cart takes
            else if (a == "--cbuild" && i + 1 < argc) cbuildPath = argv[++i];        // build this C file's project with SDCC, then run it
            else if (a == "--cpctelera" && i + 1 < argc) cbuildCpct = argv[++i];     // ...with CPCtelera's library from this folder
            else if (a == "--sdcc" && i + 1 < argc) cbuildSdcc = argv[++i];          // ...SDCC from this folder (default: found)
            else if (a == "--codeloc" && i + 1 < argc) cbuildLoc = (int)std::strtol(argv[++i], nullptr, 0);   // without cfg/build_config.mk
            else if (a == "--force-assets") cbuildForce = true;                    // run every CPCtelera asset step again
            else if (a == "--z88dk" && i + 1 < argc) { cbuildZ88dk = argv[++i]; cbuildUseZ88dk = true; }   // build with z88dk (folder, or "" = find it)
            else if (a == "--sccz80") { cbuildSccz80 = true; cbuildUseZ88dk = true; }   // ...with its sccz80, not zsdcc
            else if (a == "--tape" && i + 1 < argc) tapePath = argv[++i];   // .cdt/.tzx/.wav in the deck
            else if (a == "--record-tape" && i + 1 < argc) recordTape = argv[++i];   // a blank tape, REC down; saved here at the end
            else if (a == "--tape-fast" && i + 1 < argc) tapeFast = argv[++i];   // fast loading: a baud rate, instant, or off
            else if (a == "--raster-overlay") rasterOverlayOn = true;   // the raster timing overlay drawn on the picture
            else if (a == "--type" && i + 1 < argc) typeStr = argv[++i];   // typed after boot (\\n = Enter)
            else if (a == "--keys" && i + 1 < argc) keysStr = argv[++i];   // tap after F1 loads: e.g. "Space" launches PD ball
            else if (a == "--f1at" && i + 1 < argc) f1at = std::atoi(argv[++i]);
            else if (a == "--crtc" && i + 1 < argc) wantCrtc = std::atoi(argv[++i]);
            else if (a == "--gate-array" && i + 1 < argc) wantGa = std::atoi(argv[++i]);   // 40007/40008/40010
            else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
            else if (a == "--seq" && i + 1 < argc) seq = std::atoi(argv[++i]);   // save this many CONSECUTIVE frames (motion) from --frames
            else if (a == "--diag") diag = true;
            else if (a == "--beam") beam = true;
            else if (a == "--hhold" && i + 1 < argc) monitorHHold = std::atoi(argv[++i]);   // H. Hold, Pixel-M2 per line
            else if (a == "--vhold" && i + 1 < argc) monitorVHold = std::atoi(argv[++i]);   // V. Hold, lines
            else if (a == "--monitor-set" && i + 1 < argc) monitorSet = argv[++i];   // a monitor_model.h id: ctm644, gt65, mm12...
            else if (a == "--brightness" && i + 1 < argc) monitorKnobs.brightness = std::atoi(argv[++i]);   // the set's front knob, 0..100
            else if (a == "--contrast" && i + 1 < argc) monitorKnobs.contrast = std::atoi(argv[++i]);       // monochrome sets only, 0..100
            else if (a == "--intensity" && i + 1 < argc) parseTriple(argv[++i], monitorIntensity);          // R,G,B output intensity, 0..200 %
            else if (a == "--gx4000-128k") gx4000Ram128 = true;                     // the GX4000 128K RAM mod
            else if (a == "--plus-expansion" && i + 1 < argc) plusExpansionClassic = std::string(argv[++i]) != "internal";   // classic (default) or internal
            else if (a == "--z80" && i + 1 < argc) z80Cmos = std::string(argv[++i]) == "cmos";   // nmos (default) or cmos
            else if (a == "--gfx9000") gfx9000 = true;
            else if (a == "--gfx9000-monitor" && i + 1 < argc) gfx9000Monitor = argv[++i];   // beside, window, switch (default) or video9000
            else if (a == "--opl4") opl4 = true;                              // an OPL4 card on the AMSDAP
            else if (a == "--playcity") playcity = true;                      // a PlayCity
            else if (a == "--dac" && i + 1 < argc) dacType = argv[++i];        // digiblaster / amdrum
            else if (a == "--speech" && i + 1 < argc) speechKind = argv[++i]; // ssa1 / dktronics / lambdaspeak3
            else if (a == "--mp3card" && i + 1 < argc) mp3Card = argv[++i];   // LambdaSpeak 3's MP3 card folder
            else if (a == "--wav" && i + 1 < argc) wavPath = argv[++i];        // the sound of the whole run
            else if (a == "--record" && i + 1 < argc) recordPath = argv[++i];  // the whole run to .avi (or .wav)
            else if (a == "--record-source" && i + 1 < argc) recordSource = argv[++i];   // shown (default), cpc, gfx or both
            else if (a == "--record-format" && i + 1 < argc) recordFormat = argv[++i];   // mjpeg (default), zmbv or h264 (ffmpeg)
            else if (a == "--m4" && i + 1 < argc) m4Folder = argv[++i];   // an M4 board, this folder its SD card
            else if (a == "--sf2") sf2 = true;                                // a Symbiface II (its PS/2 mouse)
            else if (a == "--ide" && i + 1 < argc) { ideFolder = argv[++i]; sf2 = true; }   // its IDE, this folder the disc
            else if (a == "--xmass") xmass = true;                            // an X-MASS IDE interface (with --ide)
            else if (a == "--sf3-usb" && i + 1 < argc) { sf3Usb = argv[++i]; sf3 = true; }   // its USB stick, this folder
            else if (a == "--sf3-sd" && i + 1 < argc) { sf3Sd = argv[++i]; sf3 = true; }     // its SD card, this folder
            else if (a == "--sf3") sf3 = true;                                // a Symbiface III instead (with --ide too)
            else if (a == "--disk-writeback") diskWriteBack = true;              // save the disc's writes into its .dsk (headless: off)
            else if (a == "--multiface") multiface = true;                    // a Multiface II (its ROM from roms/); n in --mouse is STOP
            else if (a == "--serial" && i + 1 < argc) serialLine = argv[++i];  // the RS232C card: tcp:host:port, listen:port, com:COM3, loopback
            else if (a == "--xrom" && i + 1 < argc) xroms.push_back(argv[++i]);   // <slot>=<file>: an expansion ROM
            else if (a == "--mouse" && i + 1 < argc) { mouseScript = argv[++i]; sf2 = true; }
            else if (a == "--mouse-interface" && i + 1 < argc) mouseInterface = argv[++i];   // amx, kempston, multiplay-a, multiplay-b
            else if (a == "--v9990-shot" && i + 1 < argc) { v9990Shot = argv[++i]; gfx9000 = true; }
            else if (a == "--video9000-shot" && i + 1 < argc) { video9000Shot = argv[++i]; gfx9000 = true; } }
        if (!shot.empty()) {
            attachParentConsole();
            EmuHost host; host.scanModels();
            loadMachineSounds();
            host.gateArrayPart = wantGa;
            host.croGroup = croGroup;
            host.monitorHHold = monitorHHold; host.monitorVHold = monitorVHold;
            host.z80Cmos = z80Cmos;
            host.gx4000Ram128 = gx4000Ram128;
            host.plusExpansionClassic = plusExpansionClassic;
            host.monitorKnobs = monitorKnobs;
            host.monitorSetId = monitorSet;
            if (!m4Folder.empty()) { host.m4Enabled = true; host.m4Folder = m4Folder; }
            if (!mouseInterface.empty()) host.mouseInterface = mouseInterface;
            if (sf2) host.symbifaceModule = "sf2";
            if (sf3) host.symbifaceModule = "sf3";
            if (xmass) host.symbifaceModule = "xmass";
            if (multiface) host.multifaceEnabled = true;
            host.diskWriteBack = diskWriteBack;
            if (tapeFast == "off") host.tapeFastLoad = false;
            else if (tapeFast == "instant") host.tapeInstantLoad = true;
            else if (!tapeFast.empty())
                host.tapeFastBaud = std::clamp(std::atoi(tapeFast.c_str()), (int)CPCTapeDrive::FAST_BAUD_MIN, (int)CPCTapeDrive::FAST_BAUD_MAX);
            if (!serialLine.empty()) {
                const size_t colon = serialLine.find(':');
                host.serialKind = serialLine.substr(0, colon);
                host.serialTarget = colon == std::string::npos ? "" : serialLine.substr(colon + 1);
                host.serialEnabled = true;
                if (!host.connectSerial()) { std::printf("[shot] --serial %s: %s\n", serialLine.c_str(), host.status.c_str()); return 2; }
            }
            for (const std::string& x : xroms) {
                const size_t eq = x.find('=');
                const int slot = eq == std::string::npos ? -1 : std::atoi(x.substr(0, eq).c_str());
                if (slot < 0 || !host.fitExpansionRom(slot, x.substr(eq + 1))) { std::printf("[shot] --xrom %s: %s\n", x.c_str(), host.status.c_str()); return 2; }
            }
            if (!ideFolder.empty()) host.ideFolder = ideFolder;
            if (!sf3Usb.empty()) host.sf3UsbFolder = sf3Usb;
            if (!sf3Sd.empty()) host.sf3SdFolder = sf3Sd;
            int mi = 0; for (int i = 0; i < (int)host.models.size(); i++) if (host.models[i].id == modelId) mi = i;
            if (!host.bootModel(mi, wantRam, wantCrtc)) { std::printf("[shot] %s\n", host.status.c_str()); return 2; }
            if (monitorIntensity != TubeKnobs().intensity) host.setMonitorIntensity(monitorIntensity);   // for the set now plugged in
            if (!snaPath.empty()) host.loadSnapshot(snaPath);
            if (wantCrtc >= 0 && host.emu && host.emu->crtc) host.emu->crtc->setType(wantCrtc);
            host.setBeamRenderer(beam);
            if (rasterOverlayOn) host.setRasterOverlay(true);
            if (gfx9000) host.setV9990(true);
            if (opl4) host.setOpl4(true);
            if (playcity) host.setPlayCity(true);
            if (!dacType.empty()) host.setDacType(dacType);
            if (!mp3Card.empty()) host.mp3Card = mp3Card;
            if (!speechKind.empty()) host.setSpeech(speechKind);
            KeyboardMatrix* kb = host.emu ? host.emu->keyboard : nullptr;
            std::vector<int16_t> wav;
            // CPCSE_RENDER_EVERY=1: draw every frame as the window does. The shot mode only
            // draws the last one, so a fault in the renderer never shows here without it.
            static const bool renderEvery = std::getenv("CPCSE_RENDER_EVERY") != nullptr;
            auto run = [&](int n) {
                for (int f = 0; f < n; f++) {
                    host.runFrame();
                    if (renderEvery) host.render();
                    if (!wavPath.empty()) wav.insert(wav.end(), host.audioOut.begin(), host.audioOut.end());
                }
            };
            if (!gfx9000Monitor.empty()) host.gfx9000Monitor = gfx9000Monitor;
            if (!recordPath.empty()) {
                host.recordFormat = recordFormat;
                host.recordSource = recordSource;
                std::string why;
                if (!host.startRecording(recordPath, why)) { std::printf("[record] %s\n", why.c_str()); return 2; }
            }
            if (!cartPath.empty()) { if (!host.loadCartridgeFile(cartPath) || diag) std::printf("[cart] %s\n", host.status.c_str()); if (wantCrtc >= 0 && host.emu && host.emu->crtc) host.emu->crtc->setType(wantCrtc); run(150); }
            if (!diskPath.empty()) { host.loadDiskFile(diskPath, 0); run(150); }  // let BASIC settle
            if (!cbuildPath.empty()) {
                // A C program built (c_build.h) and started as from BASIC (EmuHost::startProgram);
                // --frames, --keys and --shot then apply to it.
                CToolchain t;
                t.sdccFolder = cbuildSdcc;
                t.cpcteleraFolder = cbuildCpct;
                if (cbuildLoc >= 0) t.codeLoc = cbuildLoc;
                t.forceAssets = cbuildForce;
                if (cbuildUseZ88dk) { t.compiler = "z88dk"; t.z88dkFolder = cbuildZ88dk; t.z88dkSccz80 = cbuildSccz80; }
                t.makeSnapshot = host.programSnapshotMaker();
                std::error_code ec;
                const CBuildResult res = buildC(t, cbuildPath, (std::filesystem::temp_directory_path(ec) / "cpcse-cbuild").u8string(), nullptr);
                for (const CBuildMessage& m : res.messages)
                    std::printf("[cbuild] %s%s%s\n", m.file.empty() ? "" : (m.file + ":" + std::to_string(m.line) + ": ").c_str(), m.text.c_str(), m.error ? "" : " (warning)");
                if (diag) std::printf("%s", res.log.c_str());
                if (!res.ok) { std::printf("[cbuild] FAILED\n"); return 3; }
                for (const std::string* p : { &res.dskPath, &res.cdtPath, &res.snaPath }) if (!p->empty()) std::printf("[cbuild] wrote %s\n", p->c_str());
                // its disc in drive A when the program reads it (EmuHost::loadDiskFile), as the window does
                if (res.discNeeded && !res.dskPath.empty() && host.emu && host.emu->hasFdc) host.loadDiskFile(res.dskPath, 0);
                std::string how;
                host.startProgram(res.bytes, res.loadAddress, res.runAddress, how);
                std::printf("[cbuild] %s: %d bytes at &%04X, run &%04X (%s)\n", res.project.c_str(), (int)res.bytes.size(),
                            res.loadAddress & 0xffff, res.runAddress & 0xffff, how.c_str());
            }
            if (!tapePath.empty() && host.loadTapeFile(tapePath)) host.tapePlay();   // PLAY down: it moves when the firmware starts the motor
            if (!recordTape.empty()) { host.tapeNewBlank(); host.tapeRecord(); }   // REC and PLAY down on a blank tape
            // The firmware has to reach its keyboard scan before a tap registers at
            // all; without a disk or cart to wait on, nothing else provides that time.
            if (!typeStr.empty() && diskPath.empty() && cartPath.empty()) run(150);
            // Type a BASIC line on the emulated keyboard (cpc_typing.h) -- "OUT &BC00,4:OUT
            // &BD00,36" -- so a register experiment can be driven from the command line.
            auto typeText = [&](const std::string& given) {
                // "\n" in the text is Enter, as typed on a command line (a real newline is too)
                std::string text;
                for (size_t i = 0; i < given.size(); i++) {
                    if (given[i] == '\\' && i + 1 < given.size() && given[i + 1] == 'n') { text += '\n'; i++; }
                    else text += given[i];
                }
                for (char c : text)
                    if (kb && cpcTypeKey(*kb, c, true)) { run(4); cpcTypeKey(*kb, c, false); run(4); }
            };
            typeText(typeStr);                              // type the boot command
            int last = 0;
            if (f1at >= 0 && kb) {
                for (int t = f1at; t < frames; t += (f1at > 0 ? f1at : frames)) {   // press F1 every f1at frames
                    run(std::max(0, t - last)); last = t;
                    kb->setKey("F1", true); run(4); kb->setKey("F1", false); last += 4;
                }
            }
            run(std::max(0, frames - last));
            // Optional input taps after load, e.g. launch the ball so the playfield
            // SCROLLS, or drive a cart through its menus INTO gameplay. A token of
            // the form r<N> (e.g. "r120") runs N frames without a keypress, so you
            // can space fire presses across menu transitions:
            //   --keys "ControlLeft,r150,ControlLeft,r150,ControlLeft,r400"
            // (ControlLeft = joystick fire). Otherwise each token is a key, held
            // for 6 frames then released for 6.
            for (size_t k = 0; k < keysStr.size(); ) {
                size_t e = keysStr.find(',', k); std::string code = keysStr.substr(k, e == std::string::npos ? e : e - k);
                if (!code.empty()) {
                    if (code[0] == 'r' && code.size() > 1 && code.find_first_not_of("0123456789", 1) == std::string::npos) {
                        run(std::atoi(code.c_str() + 1));               // r<N>: run N frames (no key)
                    } else if (kb) {
                        // "ShiftLeft+Digit2": the keys of a token held together (a " after F1)
                        std::vector<std::string> held;
                        for (size_t s = 0; s <= code.size(); ) {
                            size_t plus = code.find('+', s);
                            held.push_back(code.substr(s, plus == std::string::npos ? std::string::npos : plus - s));
                            if (plus == std::string::npos) break;
                            s = plus + 1;
                        }
                        for (const std::string& h : held) kb->setKey(h, true);
                        run(6);
                        for (const std::string& h : held) kb->setKey(h, false);
                        run(6);
                    }
                }
                if (e == std::string::npos) break;
                k = e + 1;
            }
            // --mouse "w120;m-400,-400;m100,50;c;d;kEnter;s<file.bmp>": after everything else, a
            // script for the Symbiface mouse -- wait frames, move (relative, in mouse counts),
            // click, double-click, tap a key, save a screenshot -- so a desktop like SymbOS
            // can be driven with no window.
            for (size_t k = 0; k < mouseScript.size() && host.emu && host.emu->symbifaceMouse; ) {
                size_t e = mouseScript.find(';', k);
                const std::string tok = mouseScript.substr(k, e == std::string::npos ? std::string::npos : e - k);
                SymbifaceMouse* mouse = host.emu->symbifaceMouse;
                auto click = [&]() { mouse->button(0, true); run(3); mouse->button(0, false); run(3); };
                if (!tok.empty()) switch (tok[0]) {
                    case 'w': run(std::atoi(tok.c_str() + 1)); break;
                    case 'm': {
                        int dx = 0, dy = 0;
                        std::sscanf(tok.c_str() + 1, "%d,%d", &dx, &dy);
                        // in steps a mouse would report, a frame apart
                        const int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)) / 20);
                        for (int s = 0; s < steps; s++) { mouse->move((double)dx / steps, (double)dy / steps); run(1); }
                        run(2);
                        break;
                    }
                    case 'j': {                    // j<bits>,<frames>: hold the joystick (b0 up b1 down b2 left b3 right b4 fire)
                        int bits = 0, frames = 1;
                        std::sscanf(tok.c_str() + 1, "%d,%d", &bits, &frames);
                        if (kb) {
                            for (int b = 0; b < 6; b++) if (bits >> b & 1) kb->setJoystick(b, true);
                            run(frames);
                            for (int b = 0; b < 6; b++) if (bits >> b & 1) kb->setJoystick(b, false);
                            run(2);
                        }
                        break;
                    }
                    case 'c': click(); break;
                    case 'd': mouse->button(0, true); run(1); mouse->button(0, false); run(2); mouse->button(0, true); run(1); mouse->button(0, false); run(3); break;
                    case 't': typeText(tok.substr(1)); break;   // t<text>: typed (then kEnter)
                    case 'k': if (kb) { kb->setKey(tok.substr(1), true); run(4); kb->setKey(tok.substr(1), false); run(4); } break;
                    case 'n': host.multifaceStop(); run(2); break;   // n: the Multiface II's STOP
                    case 's': host.render(); host.saveScreenshotBmp(tok.substr(1)); std::printf("[mouse] shot %s\n", tok.c_str() + 1); break;
                    case 'v': {                    // v<file.bmp>: the GFX9000's picture now
                        const V9990Picture* pic = host.v9990Picture();
                        const bool ok = pic && writeVideoBmp(tok.substr(1), pic->pixels, pic->width, pic->height);
                        std::printf("[mouse] v9990 shot %s (%s)\n", tok.c_str() + 1, ok ? "ok" : "not displaying");
                        break;
                    }
                    default: break;
                }
                if (e == std::string::npos) break;
                k = e + 1;
            }
            // CPCSE_TRACE_TIP=<n>: run ONE more frame with the monitor's per-flyback PULL
            // trace armed for n lines, so a line the monitor misplaces in a demo can be
            // read off the numbers the flywheel acted on -- the SHAKER runner was the only
            // thing that could arm it, and demos do not run under the runner.
            if (const char* tip = std::getenv("CPCSE_TRACE_TIP")) {
                if (host.emu && host.emu->monitorRenderer)
                    host.emu->monitorRenderer->pullTraceBudget = std::atol(tip) > 1 ? std::atol(tip) : 400;
                run(1);
            }
            if (diag && host.emu && host.emu->crtc) {
                host.render();
                const auto& rf = host.emu->crtc->getRasterFrame();
                int disp = 0, dispHud = 0; std::map<int,int> addrCount;
                for (size_t li = 0; li < rf.size(); li++) {
                    const auto& L = *rf[li];
                    int on = 0; for (uint8_t b : L.displayEnabled) if (b) on++;
                    bool lineOn = L.vDisplay && on > 0;
                    if (lineOn) { disp++; addrCount[L.lineAddress & 0x3fff]++; if ((int)li > 200) dispHud++; }
                    if (diag && (std::getenv("CPCSE_DIAG_ALL") || li % 16 == 0 || (li>=200 && li<=230))) {
                        // en=[first..last] is the span the renderer will draw; hc is how
                        // many character slots this line was captured into at all. A line
                        // whose hc is far below R0+1 was not captured as one line -- the
                        // capture index moved underneath it.
                        int firstEn = -1, lastEn = -1, hcSeen = 0;
                        for (int ci = 0; ci < (int)L.displayEnabled.size(); ci++) {
                            if (L.displayEnabled[ci]) { if (firstEn < 0) firstEn = ci; lastEn = ci; }
                            if (L.horizontalCounters[ci]) hcSeen++;
                        }
                        // off/ph/c0@: where the MONITOR put this line -- its sweep's whole-
                        // character offset, its sub-character phase, and the CRTC's C0 at
                        // the moment the line was filed. A line drawn at the wrong x with
                        // the right CRTC counters is the monitor's, not the CRTC's.
                        std::printf("  line %3zu: vDisp=%d on=%3d en=[%d..%d] hc=%d start=%d R0=%d R1=%d addr=%04x scr=%04x vlc=%d R4=%d R7=%d R12=%d R13=%d split=%d/%04x off=%d ph=%d c0@=%d\n",
                            li, L.vDisplay?1:0, on, firstEn, lastEn, hcSeen, L.displayStartCharacter, L.crtcRegisters[0], L.crtcRegisters[1],
                            L.lineAddress&0x3fff, L.screenAddress&0x3fff, L.videoRaster,
                            L.crtcRegisters[4], L.crtcRegisters[7], L.crtcRegisters[12], L.crtcRegisters[13], L.splitLine, L.splitAddress&0x3fff,
                            L.monitorLineOffset, L.sweepPhase16, L.horizontalCounterAtMonitorLine);
                        // CPCSE_DIAG_C0=<line>: that line's record character by character --
                        // which CRTC C0 each slot holds and whether DISPEN was on -- because a
                        // line filed across a monitor-line boundary is only visible here.
                        if (const char* want = std::getenv("CPCSE_DIAG_C0")) {
                            if ((int)li == std::atoi(want)) {
                                std::printf("    C0 :");
                                for (size_t ci = 0; ci < L.horizontalCounters.size() && ci < 64; ci++)
                                    std::printf(" %2d", (int)L.horizontalCounters[ci]);
                                std::printf("\n    en :");
                                for (size_t ci = 0; ci < L.displayEnabled.size() && ci < 64; ci++)
                                    std::printf(" %2d", (int)L.displayEnabled[ci]);   // 3 = both bytes, 1 = first only (17.6.2)
                                std::printf("\n");
                                // ...and every ink/mode change filed on it, with the first four
                                // inks it switched to: a palette change landing one character
                                // into a line draws that first character in the OLD palette.
                                // The two video bytes filed per slot, beside what RAM holds for that
                                // slot's C0 on this line's row and on the rasters either side -- a
                                // byte read on the wrong raster shows up as matching a neighbour.
                                std::printf("    bytes:");
                                for (size_t ci = 0; ci < L.horizontalCounters.size() && ci < 64; ci++)
                                    std::printf(" %02X%02X", L.videoBytes[ci * 2], L.videoBytes[ci * 2 + 1]);
                                std::printf("\n");
                                for (int dv = -1; dv <= 1; dv++) {
                                    std::printf("    RAM vlc%+d:", dv);
                                    for (size_t ci = 0; ci < L.horizontalCounters.size() && ci < 64; ci++) {
                                        int ma = (L.lineAddress + L.horizontalCounters[ci]) & 0x3fff;
                                        int a = host.emu->rasterByteAddress(ma, (L.videoRaster + dv) & 7);
                                        std::printf(" %02X%02X", host.emu->memory->readVideo(a), host.emu->memory->readVideo(a + 1));
                                    }
                                    std::printf("\n");
                                }
                                std::printf("    line inks:");
                                for (int k = 0; k < 4 && k < (int)L.gaPalette.size(); k++) std::printf(" %02X", L.gaPalette[k]);
                                std::printf("\n");
                                for (const auto& sg : L.segments) {
                                    std::printf("    seg @slot %d mode %d inks:", sg.character, sg.mode);
                                    for (int k = 0; k < 4 && k < (int)sg.gaPalette.size(); k++) std::printf(" %02X", sg.gaPalette[k]);
                                    std::printf("\n");
                                }
                            }
                        }
                    }
                }
                std::printf("[diag] rasterLines=%zu displayedLines=%d (hud>200:%d) distinctAddrs=%zu\n",
                    rf.size(), disp, dispHud, addrCount.size());
                std::printf("[diag] vsyncs=%ld frames=%ld blankFrames=%ld (vsync/frame=%.3f)\n",
                    host.emu->dbgVsyncCount, host.emu->dbgFrameCount, host.emu->dbgBlankFrames,
                    host.emu->dbgFrameCount ? (double)host.emu->dbgVsyncCount / host.emu->dbgFrameCount : 0.0);
                // `vsyncs` above is the GATE ARRAY's C-VSYNC -- what the monitor actually
                // receives. The CRTC can raise VSYNC and the GA still send nothing, because
                // C-VSYNC only starts once V26 has counted 2 HSYNC ends (ACCC §16.2.3) and
                // V26 restarts at every CRTC VSYNC. A gap between these two numbers is that
                // happening, and it is invisible in the ratio above.
                std::printf("[diag] crtcVsyncStarts=%ld gaCVsyncStarts=%ld (lost in V26: %ld)\n",
                    host.emu->crtc->dbgCrtcVsyncStarts, host.emu->dbgVsyncCount,
                    host.emu->crtc->dbgCrtcVsyncStarts - host.emu->dbgVsyncCount);
            }
            if (seq > 1) {   // capture CONSECUTIVE frames to reveal motion/roll
                std::string basePath = shot; size_t dot = basePath.rfind('.'); std::string ext = dot==std::string::npos?"":basePath.substr(dot); std::string stem = dot==std::string::npos?basePath:basePath.substr(0,dot);
                for (int s = 0; s < seq; s++) { host.render(); char nm[512]; std::snprintf(nm, sizeof(nm), "%s_%02d%s", stem.c_str(), s, ext.c_str()); host.saveScreenshotBmp(nm); host.runFrame(); }
                std::printf("[seq] %d consecutive frames from %d -> %s_NN%s\n", seq, frames, stem.c_str(), ext.c_str());
                return 0;
            }
            if (!saveSnaPath.empty()) std::printf("[sna] %s (%s)\n", saveSnaPath.c_str(), host.saveSnapshot(saveSnaPath) ? "ok" : "FAILED");
            if (!recordTape.empty()) { host.tapeStop(); std::printf("[tape] %s (%s)\n", recordTape.c_str(), host.tapeSaveAs(recordTape) ? "ok" : host.status.c_str()); }
            // Raw RAM, no header of any kind. A .SNA carries the CRTC type in its header,
            // which makes it the wrong instrument for comparing one chip's run against
            // another's; this is just the bytes.
            if (!dumpRamPath.empty() && host.emu && host.emu->memory) {
                std::FILE* f = std::fopen(dumpRamPath.c_str(), "wb");
                bool ok2 = f != nullptr;
                if (f) {
                    const Bytes& ram = host.emu->memory->ram;
                    ok2 = std::fwrite(ram.data(), 1, ram.size(), f) == ram.size();
                    std::fclose(f);
                    std::printf("[ram] %s (%zu bytes, %s)\n", dumpRamPath.c_str(), ram.size(), ok2 ? "ok" : "FAILED");
                } else std::printf("[ram] %s (FAILED to open)\n", dumpRamPath.c_str());
            }
            if (!v9990Shot.empty()) {
                const V9990Picture* pic = host.v9990Picture();
                const bool ok = pic && writeVideoBmp(v9990Shot, pic->pixels, pic->width, pic->height);
                if (pic) std::printf("[v9990] %s %dx%d fields=%lld P#F=%02x%s -> %s (%s)\n", v9990ModeName(pic->mode), pic->width, pic->height,
                                     pic->fields, host.emu->v9990->outputControl, host.emu->v9990->outputControlWritten ? "" : " (never written)",
                                     v9990Shot.c_str(), ok ? "ok" : "FAILED");
                else std::printf("[v9990] not displaying (stand-by, or never set up) -> nothing written\n");
            }
            if (!video9000Shot.empty()) {
                host.render();
                std::vector<uint32_t> px; int w = 0, h = 0;
                const bool ok = host.video9000Picture(px, w, h) && writeVideoBmp(video9000Shot, px, w, h);
                std::printf("[video9000] &FF6F=%02x %dx%d -> %s (%s)\n", host.video9000Control(), w, h, video9000Shot.c_str(), ok ? "ok" : "FAILED");
            }
            if (!wavPath.empty()) {
                // 16-bit stereo PCM at the host's rate; and how loud it was, for a script
                std::FILE* f = std::fopen(wavPath.c_str(), "wb");
                if (f) {
                    const uint32_t bytes = (uint32_t)(wav.size() * 2), rate = (uint32_t)host.sampleRate;
                    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
                    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
                    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
                    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
                    std::fwrite("data", 1, 4, f); u32(bytes);
                    std::fwrite(wav.data(), 2, wav.size(), f);
                    std::fclose(f);
                }
                double sum = 0; int peak = 0;
                for (int16_t s : wav) { sum += (double)s * s; peak = std::max(peak, std::abs((int)s)); }
                std::printf("[wav] %s: %.1f s, rms %.0f, peak %d\n", wavPath.c_str(), wav.size() / 2.0 / host.sampleRate,
                            wav.empty() ? 0.0 : std::sqrt(sum / wav.size()), peak);
            }
            if (host.recording()) { host.stopRecording(); std::printf("[record] %s: %s\n", recordPath.c_str(), host.status.c_str()); }
            host.render();
            if (host.emu && host.emu->crtc) { auto* c = host.emu->crtc; std::printf("[regs] type=%d R0=%d R1=%d R2=%d R3=%d R4=%d R5=%d R6=%d R7=%d R8=%d R9=%d R12=%d R13=%d\n",
                c->type, c->registers[0],c->registers[1],c->registers[2],c->registers[3],c->registers[4],c->registers[5],c->registers[6],c->registers[7],c->registers[8],c->registers[9],c->registers[12],c->registers[13]); }
            // fast loading's decision for each block that played
            if (const CPCTapeDrive* deck = host.tapeDeck(); deck && !tapePath.empty()) {
                std::string decided;
                for (size_t b = 0; b < deck->blockFast.size(); b++) {
                    const int8_t v = deck->blockFast[b];
                    if (v == CPCTapeDrive::FAST_UNKNOWN) continue;
                    decided += " [" + std::to_string(b) + "]" + (v == CPCTapeDrive::FAST_ON ? "fast" : v == CPCTapeDrive::FAST_NORMAL ? "normal"
                                                                 : v == CPCTapeDrive::FAST_INSTANT ? "instant" : "as-fast");
                }
                std::printf("[tape] fast loading %s%s, head at block %d of %zu;%s\n", host.tapeFastLoad ? (std::to_string(host.tapeFastBaud) + " baud").c_str() : "off",
                            host.tapeInstantLoad ? ", instant" : "", deck->blockAtPosition(), deck->blocks.size(), decided.empty() ? " no block decided" : decided.c_str());
            }
            bool ok = host.saveScreenshotBmp(shot);
            std::printf("[shot] model=%s crtc=%d frames=%d beam=%d -> %s (%s)\n",
                host.currentModel >= 0 ? host.models[host.currentModel].label.c_str() : "?",
                wantCrtc, frames, beam ? 1 : 0, shot.c_str(), ok ? "ok" : "FAILED");
            return ok ? 0 : 1;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CPCSyntaxError", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);   // physical joysticks/joypads (optional)

    // Load persisted settings.
    std::map<std::string, std::string> ini = parseIni(iniFilePath());
    auto geti = [&](const char* k, int d) { auto it = ini.find(k); return it != ini.end() ? std::atoi(it->second.c_str()) : d; };
    auto getf = [&](const char* k, double d) { auto it = ini.find(k); return it != ini.end() ? std::atof(it->second.c_str()) : d; };
    auto gets = [&](const char* k, const char* d) { auto it = ini.find(k); return it != ini.end() ? it->second : std::string(d); };

    // A hand-edited or damaged cpcse.ini must not stop the program starting: sizes held to
    // what a window can be, levels to their range ("nan" is a number to atof).
    int winW = std::clamp(geti("winw", 1440), 320, 16384), winH = std::clamp(geti("winh", 900), 240, 16384);
    auto level = [&](const char* k, double d, double lo, double hi) {
        const double v = getf(k, d);
        return (float)(std::isfinite(v) ? std::clamp(v, lo, hi) : d);
    };

#if defined(__APPLE__)
    // macOS gives a modern OpenGL only as a 3.2+ Core Profile, forward-compatible context;
    // asked for anything else it hands back the legacy 2.1 one, which ImGui's GL3 renderer
    // cannot start on ("Failed to initialize OpenGL loader!"). Every GL call here is core.
    const char* glslVersion = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    const char* glslVersion = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    // Made hidden, put where it was, then shown: it never appears in the wrong place first.
    SDL_Window* window = SDL_CreateWindow(
        "CPCSyntaxError " CPCSE_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        winW, winH, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!window) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CPCSyntaxError", SDL_GetError(), nullptr); SDL_Quit(); return 1; }
    placeWindow(window, ini.count("winx") && ini.count("winy"), geti("winx", 0), geti("winy", 0), winW, winH);
    // Its place and size when not maximized, minimized or full screen: what is saved, and
    // what a maximized window goes back to.
    int normalX = 0, normalY = 0, normalW = winW, normalH = winH;
    SDL_GetWindowPosition(window, &normalX, &normalY);
    SDL_GetWindowSize(window, &normalW, &normalH);
    if (geti("winmax", 0)) SDL_MaximizeWindow(window);
    if (!std::getenv("CPCSE_HIDDEN_WINDOW")) SDL_ShowWindow(window);   // for tests: no window shown
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        const std::string why = std::string("This computer's graphics driver could not give an OpenGL 3 context: ") + SDL_GetError();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CPCSyntaxError", why.c_str(), window);
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
	enableDarkTitleBar(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The window layout (docking, positions, sizes) is ImGui's own ini, beside cpcse.ini.
    static std::string layoutIni;
    layoutIni = homeDir() + "cpcse_layout.ini";
    io.IniFilename = layoutIni.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;   // dragging inside the screen never moves it
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        // A context too old for the renderer: say so, rather than assert in its first frame.
        const char* version = (const char*)glGetString(GL_VERSION);
        const std::string why = std::string("The interface needs OpenGL 3; this context is ") + (version ? version : "unknown") + ".";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CPCSyntaxError", why.c_str(), window);
        SDL_GL_DeleteContext(gl); SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }

    EmuHost host;
    loadMachineSounds();
    GuiShell shell(host);
    shell.loadSettings(ini);
    shell.applyStyle();                         // after the settings: the saved interface size

    // Apply persisted settings (before any boot, so applySettings picks them up).
    if (ini.count("romdir")) host.setRomDir(ini["romdir"]);
    for (int i = 1; i < argc; i++) {   // command line overrides the ini
        std::string a = argv[i];
        if ((a == "--roms" || a == "-r") && i + 1 < argc) host.setRomDir(argv[++i]);
    }
    host.setMonitorMode(gets("monitor", ""));
    host.setMonitorSet(gets("monitorset", ""));
    host.setMonitorHolds(geti("monitorhhold", 0), geti("monitorvhold", 0));
    host.setZ80Cmos(geti("z80cmos", 0) != 0);
    host.gx4000Ram128 = geti("gx4000ram128", 0) != 0;     // before the machine boots
    host.plusExpansionClassic = geti("plusexpansionclassic", 1) != 0;
    {
        TubeKnobs knobs;
        knobs.brightness = geti("monitorbrightness", MONITOR_BRIGHTNESS_DEFAULT);
        knobs.contrast = geti("monitorcontrast", MONITOR_CONTRAST_DEFAULT);
        // each set's output intensity: monitorintensity.<set id>=R,G,B
        for (const auto& kv : ini) {
            const std::string prefix = "monitorintensity.";
            if (kv.first.rfind(prefix, 0) != 0 || kv.first.size() <= prefix.size()) continue;
            std::array<int, 3> rgb = TubeKnobs().intensity;
            parseTriple(kv.second, rgb);
            host.monitorIntensity[kv.first.substr(prefix.size())] = rgb;
        }
        host.setMonitorKnobs(knobs);
    }
    host.audioEnabled   = geti("audio", 1) != 0;
    host.masterVolume   = level("volume", 0.6, 0.0, 1.0);
    for (const auto& s : EmuHost::soundLevels()) host.*(s.level) = (float)level(("vol_" + std::string(s.key)).c_str(), 1.0, 0.0, 2.0);
    host.driveSounds    = geti("drive_sounds", 1) != 0;
    host.keySounds      = geti("key_sounds", 0) != 0;
    host.mechanicsVolume = level("noises_volume", 0.5, 0.0, 1.0);
    host.dacType        = gets("dac", "none");
    host.keyboardRegion = gets("region", "uk");
    host.keyboardMapping = gets("keymapping", "cpc") == "pc" ? "pc" : "cpc";
    host.joystickEnabled = geti("joystick", 1) != 0;
    host.speed          = level("speed", 1.0, 0.25, 4.0);
    host.integerScale   = geti("integerscale", 0) != 0;
    host.maintainAspect = geti("aspect", 1) != 0;
    host.crtEffect      = geti("crt", 0) != 0;
    host.crtScanline    = level("crtscanline", 0.5, 0.0, 1.0);
    host.crtBloom       = level("crtbloom", 0.2, 0.0, 1.0);
    host.crtBlur        = level("crtblur", 0.0, 0.0, 1.0);
    host.crtPattern     = gets("crtpattern", "auto");
    if (host.crtPattern != "slot" && host.crtPattern != "soft" && host.crtPattern != "scanlines" && host.crtPattern != "none") host.crtPattern = "auto";
    host.crtMask        = level("crtmask", 0.5, 0.0, 1.0);
    host.crtMaskSize    = std::clamp(geti("crtmasksize", 1), 1, 4);
    host.crtMaskTurned  = geti("crtmaskturned", 0) != 0;
    host.crtFilter.load(ini);
    host.m4Folder       = gets("m4folder", "");
    host.m4Enabled      = geti("m4", 0) != 0;             // applied by applySettings during boot
    host.m4Network      = geti("m4net", 1) != 0;
    host.m4NetworkLan   = geti("m4netlan", 0) != 0;
    host.multifaceEnabled = geti("multiface", 0) != 0;   // applied by applySettings during boot
    host.serialKind     = gets("serialkind", "listen");
    host.serialTarget   = gets("serialtarget", "2323");
    host.serialLan      = geti("seriallan", 0) != 0;
    if (geti("serial", 0) != 0) { host.serialEnabled = true; host.connectSerial(); }   // fitted by applySettings during boot
    host.symbifaceModule = gets("symbiface", "none");
    host.mouseInterface = gets("mouseinterface", "none");   // applied by applySettings (kept out while an SF2/SF3 is fitted)
    host.ideFolder = gets("idefolder", (homeDir() + "symide").c_str());   // the IDE's disc
    if (ini.count("sf3usb")) host.sf3UsbFolder = ini.at("sf3usb");
    if (ini.count("sf3sd")) host.sf3SdFolder = ini.at("sf3sd");
    host.mouseSensitivity = level("mousesens", 1.0, 0.25, 4.0);
    host.lightgunType   = gets("lightgun", "none");       // these four are applied by applySettings
    host.v9990Enabled   = geti("v9990", 0) != 0;
    host.opl4Enabled    = geti("opl4", 0) != 0;
    host.playCityEnabled = geti("playcity", 0) != 0;
    if (ini.count("speech")) host.speechKind = ini.at("speech");
    if (ini.count("mp3card")) host.mp3Card = ini.at("mp3card");
    host.opl4RamKiB     = geti("opl4ram", 2048);
    // One monitor, switched, unless the ini says otherwise (an older ini's v9990beside kept).
    host.gfx9000Monitor = gets("v9990monitor", ini.count("v9990beside") ? (geti("v9990beside", 1) ? "beside" : "window") : "switch");
    host.tapeFollowsMotor = geti("tapemotor", 1) != 0;
    host.diskWriteBack  = geti("diskwriteback", 1) != 0;   // a program's disc writes saved into its .dsk
    host.tapeRelayDelay = geti("taperelay", 1) != 0;
    host.tapeFastLoad   = geti("tapefastload", 1) != 0;
    host.timelineEnabled = geti("timeline", 0) != 0;
    host.timelineBudgetMB = std::clamp(geti("timelinemb", 256), 32, 1024);
    host.tapeInstantLoad = geti("tapeinstant", 0) != 0;
    host.tapeFastBaud   = std::clamp(geti("tapefastbaud", host.tapeFastBaud), (int)CPCTapeDrive::FAST_BAUD_MIN, (int)CPCTapeDrive::FAST_BAUD_MAX);
    host.tapeTurbo      = std::clamp(geti("tapeturbo", 1), 1, (int)EmuHost::TAPE_TURBO_UNLIMITED);
    host.beamRenderer   = geti("beam", 0) != 0;
    {
        const std::string rf = gets("recordformat", "mjpeg");
        host.recordFormat = rf == "h264" ? rf : "mjpeg";   // ZMBV left the menu: an old setting is MJPEG
    }
    host.ffmpegPath     = gets("ffmpeg", "");
    {
        const std::string rs = gets("recordsource", "shown");
        host.recordSource = rs == "cpc" || rs == "gfx" || rs == "both" || rs == "screen" ? rs : "shown";
    }
    host.stateDir       = homeDir() + "states";     // the quick save slots
    host.ls3EepromFile  = homeDir() + "ls3eeprom.bin";   // LambdaSpeak 3's EEPROM
    host.recordDir      = homeDir() + "recordings";
    for (size_t i = EmuHost::RECENT_MAX; i-- > 0;) {
        const std::string r = gets(("recent." + std::to_string(i)).c_str(), "");
        if (!r.empty()) host.noteRecent(r);     // oldest first: the newest ends on top
    }

    host.gateArrayPart  = geti("gatearray", 0);            // applied by bootModel
    // Boot the default machine.
    // With no ini (a first start) or a machine whose ROMs have gone: a CPC 6128, else the
    // first machine there are ROMs for.
    std::string savedModel = gets("model", "");
    int savedRam = geti("ram", -1), savedCrtc = geti("crtc", -1);
    // -O system.model=N, as TRSE passes it: this session's machine, not the
    // settings' (commandLineModel keeps them from being saved over).
    int commandLineModel = -1;
    if (geti("autoboot", 1)) {
        int pick = -1;
        auto find = [&](const std::string& id) {
            for (int i = 0; i < (int)host.models.size(); i++) if (host.models[i].id == id && host.models[i].available) return i;
            return -1;
        };
        const std::map<std::string, std::string> overrides = commandLineOverrides(argc, argv);
        if (auto o = overrides.find("system.model"); o != overrides.end()) {
            static const char* const modelByNumber[] = { "cpc464", "cpc664", "cpc6128", "cpc6128plus" };
            const int n = std::atoi(o->second.c_str());
            if (n >= 0 && n < 4 && (pick = find(modelByNumber[n])) >= 0) {
                commandLineModel = pick;
                if (modelByNumber[n] != savedModel) { savedRam = -1; savedCrtc = -1; }   // that machine's own
            }
        }
        if (pick < 0 && !savedModel.empty()) pick = find(savedModel);
        if (pick < 0) { savedRam = -1; savedCrtc = -1; pick = find("cpc6128"); }
        for (int i = 0; pick < 0 && i < (int)host.models.size(); i++) if (host.models[i].available) pick = i;
        // a RAM size or CRTC from the settings that cannot be fitted: the model's own instead
        if (pick >= 0 && !host.bootModel(pick, savedRam, savedCrtc)) host.bootModel(pick);
    }

    // External development (devserver.h): the servers the settings ask for, then the
    // command line's --gdb/--api/--load/--watch for this session.
    DevServer& dev = shell.devServer();
    dev.startFromSettings();
    {
        std::string err;
        if (!applyDevArguments(dev, argc, argv, err))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "CPCSyntaxError", err.c_str(), nullptr);
        if (!dev.lastEvent.empty()) host.status = dev.lastEvent;
    }

    GLuint screenTex = 0;
    glGenTextures(1, &screenTex);
    glBindTexture(GL_TEXTURE_2D, screenTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const int TEX_W = host.video->width, TEX_H = host.video->height;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, host.video->pixels.data());

    SDL_AudioSpec want{}, have{};
    want.freq = host.sampleRate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = nullptr;
    SDL_AudioDeviceID audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audioDev) SDL_PauseAudioDevice(audioDev, 0);

    auto toggleFullscreen = [&]() {
        host.fullscreen = !host.fullscreen;
        SDL_SetWindowFullscreen(window, host.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    };
    shell.toggleFullscreen = toggleFullscreen;
    // the bloom's copies add their light (the backend's next ResetRenderState undoes it)
    shell.additiveBlend = [](const ImDrawList*, const ImDrawCmd*) { glBlendFunc(GL_SRC_ALPHA, GL_ONE); };

    // The tool windows' pictures (GFX9000 monitor, printer page): one texture per slot,
    // re-created when the size changes.
    struct ToolTexture { GLuint id = 0; int w = 0, h = 0; };
    std::map<int, ToolTexture> toolTextures;
    shell.uploadTexture = [&](int slot, const uint32_t* rgba, int w, int h) -> unsigned {
        ToolTexture& t = toolTextures[slot];
        if (!t.id) {
            glGenTextures(1, &t.id);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, t.id);
        if (t.w != w || t.h != h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            t.w = w; t.h = h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        return t.id;
    };

    // The advanced CRT filter, on the GPU. A context that cannot run it says so once, and the
    // plain picture is drawn.
    CrtFilterRenderer crtFilter;
    bool crtFilterFailed = false;
    shell.crtFilterRender = [&](int outW, int outH, float pixelScale, bool newFrame) -> unsigned {
        if (crtFilterFailed) return 0;
        if (!crtFilter.init(glslVersion)) {
            crtFilterFailed = true;
            host.status = "The advanced CRT filter cannot run here: " + crtFilter.error;
            return 0;
        }
        return crtFilter.render(screenTex, TEX_W, TEX_H, 2, outW, outH, pixelScale, newFrame, host.crtFilter);
    };

    const double FRAME_SECONDS = 1.0 / 50.08;
    uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prev = SDL_GetPerformanceCounter();
    double accumulator = 0.0;

    // Emulation-speed tracker (emulated frames vs real time).
    double spdTimer = 0.0;
    int emuFrameCount = 0;
    float emuSpeedPct = 0.0f;

    // Physical joysticks and joypads -> the CPC's joysticks (joypads.h): each CPC joystick
    // takes the device chosen for it, through that device's own map. Hot-pluggable.
    host.pads.load(ini);
    host.pads.open();

    // The mouse grab: a click on a picture takes the host mouse for the CPC's (every motion
    // reaches it, wherever the host pointer would be); F12, the middle button, or the
    // window losing the focus gives it back.
    auto releaseMouse = [&]() {
        if (!host.mouseCaptured) return;
        SDL_SetRelativeMouseMode(SDL_FALSE);
        host.mouseCaptured = false;
        for (int b = 0; b < 3; b++) host.mouseButton(b, false);
    };
    bool running = true;
    while (running) {
        SDL_Event ev;
        SDL_Scancode waitingForText = SDL_SCANCODE_UNKNOWN;   // the PC layout: a key waiting for the character it types
        while (SDL_PollEvent(&ev)) {
            const bool mouseEvent = ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN
                                 || ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL;
            if (!(host.mouseCaptured && mouseEvent)) ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (shell.quitRequested) running = false;
            if (ev.type == SDL_JOYDEVICEADDED || ev.type == SDL_JOYDEVICEREMOVED) host.pads.open();
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window)) running = false;
            if (ev.type == SDL_DROPFILE && ev.drop.file) { host.loadByExtension(ev.drop.file); SDL_free(ev.drop.file); }
            // While grabbed, the host mouse is the CPC's mouse (a Symbiface or a mouse interface).
            if (host.mouseCaptured && !host.emulatedMouseActive()) releaseMouse();
            if (host.mouseCaptured && mouseEvent) {
                if (ev.type == SDL_MOUSEMOTION) host.mouseMove((float)ev.motion.xrel, (float)ev.motion.yrel);
                else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_MIDDLE) releaseMouse();
                else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                    if (ev.button.button != SDL_BUTTON_MIDDLE)
                        host.mouseButton(ev.button.button == SDL_BUTTON_RIGHT ? 1 : 0, ev.type == SDL_MOUSEBUTTONDOWN);
                } else if (ev.type == SDL_MOUSEWHEEL) host.mouseScroll((float)ev.wheel.y);
            }
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) releaseMouse();
            // Keys reach the CPC unless a text field or a debugger/assembler window has them.
            // Releases always go through, so a key held while the focus moves never sticks.
            if (ev.type == SDL_KEYUP && !ev.key.repeat) host.setKey(ev.key.keysym.scancode, false);
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat && !io.WantTextInput) {
                if (ev.key.keysym.scancode == SDL_SCANCODE_F11) shell.f11();
                else if (ev.key.keysym.scancode == SDL_SCANCODE_F12) releaseMouse();
                else if (ev.key.keysym.scancode == SDL_SCANCODE_F10 && host.multifaceEnabled) host.multifaceStop();   // its STOP button
                else if (shell.keyboardToCpc()) {
                    // The PC layout: a key that types a character waits for that character (the
                    // text event that follows it) and presses the CPC key that types it; any
                    // other key, or one that typed nothing, goes by its position.
                    const SDL_Keycode sym = ev.key.keysym.sym;
                    const bool typing = host.keyboardMapping == "pc" && sym != SDLK_SPACE && ((sym > 32 && sym < 127) || (sym >= 160 && sym < 0x40000000))
                                        && !(ev.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) ;
                    if (typing) waitingForText = ev.key.keysym.scancode;
                    else host.setKey(ev.key.keysym.scancode, true);
                }
            }
            if (ev.type == SDL_TEXTINPUT && waitingForText != SDL_SCANCODE_UNKNOWN && !io.WantTextInput) {
                host.setKeyChar(waitingForText, ev.text.text);
                waitingForText = SDL_SCANCODE_UNKNOWN;
            }
        }
        // a key that typed nothing after all (a dead key, a shortcut): by its position
        if (waitingForText != SDL_SCANCODE_UNKNOWN) { host.setKey(waitingForText, true); waitingForText = SDL_SCANCODE_UNKNOWN; }
        host.pollPads();   // the physical joysticks into the CPC's
        dev.poll();   // an external debugger's or script's requests, a watched file, typing

        uint64_t now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / (double)perfFreq;
        prev = now;
        accumulator += dt;
        bool ran = false;
        int framesThisIter = 0;
        // tape turbo: faster while the tape moves (unlimited at the slider's top)
        const bool tapeFast = host.tapeTurboActive();
        const bool unlimited = host.turbo || (tapeFast && host.tapeTurbo >= EmuHost::TAPE_TURBO_UNLIMITED);
        const float speed = host.speed * (tapeFast ? (float)host.tapeTurbo : 1.0f);
        auto feedAudio = [&]() {
            if (audioDev && !host.audioOut.empty() && !unlimited && speed <= 1.5f &&
                SDL_GetQueuedAudioSize(audioDev) < (Uint32)(host.sampleRate * 2 * sizeof(int16_t) / 5))
                SDL_QueueAudio(audioDev, host.audioOut.data(), (Uint32)(host.audioOut.size() * sizeof(int16_t)));
        };
        // Unlimited: frames for about 30 ms of this computer's time, then one picture, and the
        // swap does not wait for the display's refresh (a fixed 8 frames then a wait for the
        // next refresh left the machine idle for part of every refresh).
        static bool swapWaits = true;
        if (swapWaits == unlimited) { swapWaits = !unlimited; SDL_GL_SetSwapInterval(swapWaits ? 1 : 0); }
        // (plus what the last picture cost, so the CPC has at least half the time when the
        // picture is dear: the CRT effect's blur and slot mask are milliseconds each)
        static uint64_t uiCost = 0;
        if (unlimited) {
            const uint64_t budget = perfFreq * 30 / 1000 + std::min<uint64_t>(uiCost, perfFreq / 20);
            do { host.runFrame(); feedAudio(); framesThisIter++; }
            while (SDL_GetPerformanceCounter() - now < budget && framesThisIter < 200 && !host.paused);
            ran = true; accumulator = 0.0;
        }
        else {
            double period = FRAME_SECONDS / std::max(0.1f, speed);
            int steps = 0;
            const int most = std::max(6, (int)std::ceil(speed * 2));
            while (accumulator >= period && steps < most) { host.runFrame(); feedAudio(); ran = true; steps++; accumulator -= period; }
            framesThisIter = steps;
            if (accumulator > 0.25) accumulator = 0.0;
        }
        emuFrameCount += framesThisIter;
        spdTimer += dt;
        if (spdTimer >= 0.5) { emuSpeedPct = (float)(emuFrameCount / (50.08 * spdTimer) * 100.0); emuFrameCount = 0; spdTimer = 0.0; }
        const uint64_t uiStart = SDL_GetPerformanceCounter();
        if (ran) host.render();
        // a new picture: from the frames just run, a debugger step, or the timeline going back
        if (host.pictureChanged) {
            host.pictureChanged = false;
            glBindTexture(GL_TEXTURE_2D, screenTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_W, TEX_H, GL_RGBA, GL_UNSIGNED_BYTE, host.video->pixels.data());
        }

        // Looked at here, once the events are all in: Windows reports a maximize's move
        // before the window counts as maximized.
        if (!(SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
            SDL_GetWindowPosition(window, &normalX, &normalY);
            SDL_GetWindowSize(window, &normalW, &normalH);
        }

        // ---- UI ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ShellFrameInfo info;
        info.screenTexture = screenTex;
        info.textureWidth = TEX_W;
        info.textureHeight = TEX_H;
        info.emuSpeedPct = emuSpeedPct;
        info.uiFramerate = io.Framerate;
        info.soundOpen = audioDev != 0;
        info.soundQueuedMs = audioDev ? (float)SDL_GetQueuedAudioSize(audioDev) /
                             (float)(host.sampleRate * 2 * (int)sizeof(int16_t)) * 1000.0f : 0.0f;
        shell.draw(info);
        if (shell.mouseCaptureRequested) {
            shell.mouseCaptureRequested = false;
            if (!host.mouseCaptured && SDL_SetRelativeMouseMode(SDL_TRUE) == 0) host.mouseCaptured = true;
        }
        if (shell.quitRequested) running = false;

        ImGui::Render();
        int w, h; SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.055f, 0.067f, 0.090f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // The recorder's "screen" source: the Screen window's picture as drawn (its CRT
        // effects with it), read back from the frame just drawn and scaled to the
        // recording's size; the next emulated frame records it.
        {
            const ImVec2 fbScale = io.DisplayFramebufferScale;
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            const int px = (int)std::lround((shell.screenRect[0] - vp->Pos.x) * fbScale.x), py = (int)std::lround((shell.screenRect[1] - vp->Pos.y) * fbScale.y);
            const int pw = (int)std::lround(shell.screenRect[2] * fbScale.x), ph = (int)std::lround(shell.screenRect[3] * fbScale.y);
            host.screenPictureW = shell.screenRectMain ? pw : 0;
            host.screenPictureH = shell.screenRectMain ? ph : 0;
            if (host.recording() && host.recordSource == "screen" && shell.screenRectMain && pw > 0 && ph > 0) {
                const int x0 = std::max(0, px), y0 = std::max(0, py), x1 = std::min(w, px + pw), y1 = std::min(h, py + ph);
                if (x1 > x0 && y1 > y0) {
                    static std::vector<uint32_t> grab;
                    grab.resize((size_t)(x1 - x0) * (y1 - y0));
                    glPixelStorei(GL_PACK_ALIGNMENT, 4);
                    glReadPixels(x0, h - y1, x1 - x0, y1 - y0, GL_RGBA, GL_UNSIGNED_BYTE, grab.data());
                    const int gw = x1 - x0, gh = y1 - y0, rw = host.recordWidth, rh = host.recordHeight;
                    if (host.screenCapture.size() != (size_t)rw * rh) host.screenCapture.assign((size_t)rw * rh, 0xff000000u);
                    // GL's rows run upwards; the picture's rectangle may be cut by the window's edge
                    for (int y = 0; y < rh; y++) {
                        const int sy = py + (int)((long long)y * ph / rh) - y0;
                        uint32_t* out = &host.screenCapture[(size_t)y * rw];
                        if (sy < 0 || sy >= gh) { std::fill(out, out + rw, 0xff000000u); continue; }
                        const uint32_t* row = &grab[(size_t)(gh - 1 - sy) * gw];
                        for (int x = 0; x < rw; x++) {
                            const int sx = px + (int)((long long)x * pw / rw) - x0;
                            out[x] = sx >= 0 && sx < gw ? row[sx] | 0xff000000u : 0xff000000u;
                        }
                    }
                }
            }
        }
        // Windows pulled out of the main window live in OS windows of their own.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backupWindow = SDL_GL_GetCurrentWindow();
            SDL_GLContext backupContext = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backupWindow, backupContext);
        }
        SDL_GL_SwapWindow(window);
        uiCost = SDL_GetPerformanceCounter() - uiStart;
    }

    // Persist settings.
    {
        const bool maximized = (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) != 0;
        std::string defModel = host.currentModel >= 0 ? host.models[host.currentModel].id : savedModel;
        // Gathered first and written whole (writeFileSafely): a full disc must not leave
        // half a settings file behind.
        std::ostringstream f;
        {
            f << "# CPCSyntaxError settings\n";
            f << "romdir=" << host.romDir << "\n";
            if (commandLineModel >= 0 && host.currentModel == commandLineModel) {
                // still the command line's machine: the settings keep the one they had
                f << "model=" << gets("model", "") << "\n";
                f << "ram=" << gets("ram", std::to_string(host.ramKiB).c_str()) << "\n";
                f << "crtc=" << gets("crtc", std::to_string(host.crtcType).c_str()) << "\n";
            } else {
                f << "model=" << defModel << "\n";
                f << "ram=" << host.ramKiB << "\n";
                f << "crtc=" << host.crtcType << "\n";
            }
            f << "autoboot=1\n";
            f << "monitor=" << host.monitorMode << "\n";
            f << "monitorset=" << host.monitorSetId << "\n";
            f << "monitorhhold=" << host.monitorHHold << "\n";
            f << "monitorvhold=" << host.monitorVHold << "\n";
            f << "monitorbrightness=" << host.monitorKnobs.brightness << "\n";
            f << "monitorcontrast=" << host.monitorKnobs.contrast << "\n";
            for (const auto& kv : host.monitorIntensity)
                f << "monitorintensity." << kv.first << "=" << kv.second[0] << "," << kv.second[1] << "," << kv.second[2] << "\n";
            f << "gatearray=" << host.gateArrayPart << "\n";
            f << "z80cmos=" << (host.z80Cmos ? 1 : 0) << "\n";
            f << "gx4000ram128=" << (host.gx4000Ram128 ? 1 : 0) << "\n";
            f << "plusexpansionclassic=" << (host.plusExpansionClassic ? 1 : 0) << "\n";
            f << "audio=" << (host.audioEnabled ? 1 : 0) << "\n";
            f << "volume=" << host.masterVolume << "\n";
            for (const auto& s : EmuHost::soundLevels()) f << "vol_" << s.key << "=" << host.*(s.level) << "\n";
            f << "drive_sounds=" << (host.driveSounds ? 1 : 0) << "\n";
            f << "key_sounds=" << (host.keySounds ? 1 : 0) << "\n";
            f << "noises_volume=" << host.mechanicsVolume << "\n";
            f << "dac=" << host.dacType << "\n";
            f << "region=" << host.keyboardRegion << "\n";
            f << "keymapping=" << host.keyboardMapping << "\n";
            host.pads.save(f);
            f << "joystick=" << (host.joystickEnabled ? 1 : 0) << "\n";
            f << "speed=" << host.speed << "\n";
            f << "integerscale=" << (host.integerScale ? 1 : 0) << "\n";
            f << "crt=" << (host.crtEffect ? 1 : 0) << "\n";
            f << "crtscanline=" << host.crtScanline << "\n";
            f << "crtbloom=" << host.crtBloom << "\n";
            f << "crtblur=" << host.crtBlur << "\n";
            f << "crtpattern=" << host.crtPattern << "\n";
            f << "crtmask=" << host.crtMask << "\n";
            f << "crtmasksize=" << host.crtMaskSize << "\n";
            f << "crtmaskturned=" << (host.crtMaskTurned ? 1 : 0) << "\n";
            host.crtFilter.save(f);
            f << "aspect=" << (host.maintainAspect ? 1 : 0) << "\n";
            f << "m4=" << (host.m4Enabled ? 1 : 0) << "\n";
            f << "m4folder=" << host.m4Folder << "\n";
            f << "m4net=" << (host.m4Network ? 1 : 0) << "\n";
            f << "m4netlan=" << (host.m4NetworkLan ? 1 : 0) << "\n";
            f << "multiface=" << (host.multifaceEnabled ? 1 : 0) << "\n";
            f << "serial=" << (host.serialEnabled ? 1 : 0) << "\n";
            f << "serialkind=" << host.serialKind << "\n";
            f << "serialtarget=" << host.serialTarget << "\n";
            f << "seriallan=" << (host.serialLan ? 1 : 0) << "\n";
            f << "symbiface=" << host.symbifaceModule << "\n";
            f << "mouseinterface=" << host.mouseInterface << "\n";
            f << "idefolder=" << host.ideFolder << "\n";
            f << "sf3usb=" << host.sf3UsbFolder << "\n";
            f << "sf3sd=" << host.sf3SdFolder << "\n";
            f << "mousesens=" << host.mouseSensitivity << "\n";
            f << "lightgun=" << host.lightgunType << "\n";
            f << "v9990=" << (host.v9990Enabled ? 1 : 0) << "\n";
            f << "opl4=" << (host.opl4Enabled ? 1 : 0) << "\n";
            f << "playcity=" << (host.playCityEnabled ? 1 : 0) << "\n";
            f << "speech=" << host.speechKind << "\n";
            f << "mp3card=" << host.mp3Card << "\n";
            f << "opl4ram=" << host.opl4RamKiB << "\n";
            f << "v9990monitor=" << host.gfx9000Monitor << "\n";
            f << "tapemotor=" << (host.tapeFollowsMotor ? 1 : 0) << "\n";
            f << "diskwriteback=" << (host.diskWriteBack ? 1 : 0) << "\n";
            f << "taperelay=" << (host.tapeRelayDelay ? 1 : 0) << "\n";
            f << "tapefastload=" << (host.tapeFastLoad ? 1 : 0) << "\n";
            f << "timeline=" << (host.timelineEnabled ? 1 : 0) << "\n";
            f << "timelinemb=" << host.timelineBudgetMB << "\n";
            f << "tapeinstant=" << (host.tapeInstantLoad ? 1 : 0) << "\n";
            f << "tapefastbaud=" << host.tapeFastBaud << "\n";
            f << "tapeturbo=" << host.tapeTurbo << "\n";
            f << "beam=" << (host.beamRenderer ? 1 : 0) << "\n";
            f << "recordformat=" << host.recordFormat << "\n";
            f << "ffmpeg=" << host.ffmpegPath << "\n";
            f << "recordsource=" << host.recordSource << "\n";
            for (size_t i = 0; i < host.recentFiles.size(); i++) f << "recent." << i << "=" << host.recentFiles[i] << "\n";
            shell.saveSettings(f);
            f << "winx=" << normalX << "\n";
            f << "winy=" << normalY << "\n";
            f << "winw=" << normalW << "\n";
            f << "winh=" << normalH << "\n";
            f << "winmax=" << (maximized ? 1 : 0) << "\n";
        }
        const std::string text = f.str();
        writeFileSafely(iniFilePath(), text.data(), text.size());
    }

    host.stopRecording();                       // a recording running at exit: finished
    if (audioDev) SDL_CloseAudioDevice(audioDev);
    glDeleteTextures(1, &screenTex);
    for (auto& kv : toolTextures) glDeleteTextures(1, &kv.second.id);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    host.pads.close();
    SDL_Quit();
    return 0;
}
