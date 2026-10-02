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
#include <string>
#include <vector>

#include "emuhost.h"
#include "core/machine_sounds.h"
#include "gui_shell.h"
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
#include "core/video.h"
#include "core/z80.h"
#include "core/memory.h"
#include "core/crtc.h"
#include "core/gate_array.h"
#include "core/asic.h"
#include "core/fdc.h"
#include "core/dsk.h"
#include "core/disassembler.h"
#include "core/z80_assembler.h"


using namespace cpcse;

namespace {

std::string iniFilePath() {
    char* b = SDL_GetBasePath();
    std::string p = b ? b : "";
    if (b) SDL_free(b);
    return p + "cpcse.ini";
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
        std::string shot, modelId, snaPath, saveSnaPath, dumpRamPath, diskPath, cartPath, tapePath, typeStr, keysStr; int wantCrtc = -1, wantGa = 0, frames = 200, f1at = -1, wantRam = -1, seq = 0; bool beam = false, diag = false, gfx9000 = false, opl4 = false, playcity = false; std::string v9990Shot, video9000Shot, m4Folder, mouseScript, wavPath, speechKind, mp3Card, dacType; bool sf2 = false;
        for (int i = 1; i < argc; i++) { std::string a = argv[i];
            if (a == "--shot" && i + 1 < argc) shot = argv[++i];
            else if (a == "--ram" && i + 1 < argc) wantRam = std::atoi(argv[++i]);
            else if (a == "--model" && i + 1 < argc) modelId = argv[++i];
            else if (a == "--sna" && i + 1 < argc) snaPath = argv[++i];
            else if (a == "--savesna" && i + 1 < argc) saveSnaPath = argv[++i];   // dump RAM at the end
            else if (a == "--dumpram" && i + 1 < argc) dumpRamPath = argv[++i];  // raw RAM, no header
            else if (a == "--disk" && i + 1 < argc) diskPath = argv[++i];
            else if (a == "--cart" && i + 1 < argc) cartPath = argv[++i];
            else if (a == "--tape" && i + 1 < argc) tapePath = argv[++i];   // .cdt/.tzx/.wav in the deck
            else if (a == "--type" && i + 1 < argc) typeStr = argv[++i];   // typed after boot (\\n = Enter)
            else if (a == "--keys" && i + 1 < argc) keysStr = argv[++i];   // tap after F1 loads: e.g. "Space" launches PD ball
            else if (a == "--f1at" && i + 1 < argc) f1at = std::atoi(argv[++i]);
            else if (a == "--crtc" && i + 1 < argc) wantCrtc = std::atoi(argv[++i]);
            else if (a == "--gate-array" && i + 1 < argc) wantGa = std::atoi(argv[++i]);   // 40007/40008/40010
            else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
            else if (a == "--seq" && i + 1 < argc) seq = std::atoi(argv[++i]);   // save this many CONSECUTIVE frames (motion) from --frames
            else if (a == "--diag") diag = true;
            else if (a == "--beam") beam = true;
            else if (a == "--gfx9000") gfx9000 = true;
            else if (a == "--opl4") opl4 = true;                              // an OPL4 card on the AMSDAP
            else if (a == "--playcity") playcity = true;                      // a PlayCity
            else if (a == "--dac" && i + 1 < argc) dacType = argv[++i];        // digiblaster / amdrum
            else if (a == "--speech" && i + 1 < argc) speechKind = argv[++i]; // ssa1 / dktronics / lambdaspeak3
            else if (a == "--mp3card" && i + 1 < argc) mp3Card = argv[++i];   // LambdaSpeak 3's MP3 card folder
            else if (a == "--wav" && i + 1 < argc) wavPath = argv[++i];        // the sound of the whole run
            else if (a == "--m4" && i + 1 < argc) m4Folder = argv[++i];   // an M4 board, this folder its SD card
            else if (a == "--sf2") sf2 = true;                                // a Symbiface II (its PS/2 mouse)
            else if (a == "--mouse" && i + 1 < argc) { mouseScript = argv[++i]; sf2 = true; }
            else if (a == "--v9990-shot" && i + 1 < argc) { v9990Shot = argv[++i]; gfx9000 = true; }
            else if (a == "--video9000-shot" && i + 1 < argc) { video9000Shot = argv[++i]; gfx9000 = true; } }
        if (!shot.empty()) {
            attachParentConsole();
            EmuHost host; host.scanModels();
            loadMachineSounds();
            host.gateArrayPart = wantGa;
            if (!m4Folder.empty()) { host.m4Enabled = true; host.m4Folder = m4Folder; }
            if (sf2) host.symbifaceModule = "sf2";
            int mi = 0; for (int i = 0; i < (int)host.models.size(); i++) if (host.models[i].id == modelId) mi = i;
            host.bootModel(mi, wantRam, wantCrtc);
            if (!snaPath.empty()) host.loadSnapshot(snaPath);
            if (wantCrtc >= 0 && host.emu && host.emu->crtc) host.emu->crtc->setType(wantCrtc);
            host.setBeamRenderer(beam);
            if (gfx9000) host.setV9990(true);
            if (opl4) host.setOpl4(true);
            if (playcity) host.setPlayCity(true);
            if (!dacType.empty()) host.setDacType(dacType);
            if (!mp3Card.empty()) host.mp3Card = mp3Card;
            if (!speechKind.empty()) host.setSpeech(speechKind);
            KeyboardMatrix* kb = host.emu ? host.emu->keyboard : nullptr;
            std::vector<int16_t> wav;
            auto run = [&](int n) {
                for (int f = 0; f < n; f++) {
                    host.runFrame();
                    if (!wavPath.empty()) wav.insert(wav.end(), host.audioOut.begin(), host.audioOut.end());
                }
            };
            auto tap = [&](const std::string& code) { if (kb) { kb->setKey(code, true); run(4); kb->setKey(code, false); run(4); } };
            if (!cartPath.empty()) { host.loadCartridgeFile(cartPath); if (wantCrtc >= 0 && host.emu && host.emu->crtc) host.emu->crtc->setType(wantCrtc); run(150); }
            if (!diskPath.empty()) { host.loadDiskFile(diskPath, 0); run(150); }  // let BASIC settle
            if (!tapePath.empty()) host.loadTapeFile(tapePath);   // plays when the firmware starts the motor
            // The firmware has to reach its keyboard scan before a tap registers at
            // all; without a disk or cart to wait on, nothing else provides that time.
            if (!typeStr.empty() && diskPath.empty() && cartPath.empty()) run(150);
            // Type a BASIC line on the emulated keyboard. Letters, digits and the
            // punctuation a CRTC/Gate Array poke needs — "OUT &BC00,4:OUT &BD00,36" —
            // so a register experiment can be driven from the command line. Shifted
            // keys follow the CPC's own layout, not a PC's.
            auto shiftTap = [&](const std::string& code) {
                if (!kb) return;
                kb->setKey("ShiftLeft", true); kb->setKey(code, true); run(4);
                kb->setKey(code, false); kb->setKey("ShiftLeft", false); run(4);
            };
            auto typeText = [&](const std::string& text) {
              for (char c : text) {
                if (c == '\n') { tap("Enter"); continue; }
                std::string shifted, code;
                switch (c) {
                    case '"': shifted = "Digit2"; break;    // CPC: SHIFT+2
                    case '&': shifted = "Digit6"; break;    // CPC: SHIFT+6
                    case '#': shifted = "Digit3"; break;
                    case '$': shifted = "Digit4"; break;
                    case '%': shifted = "Digit5"; break;
                    case '(': shifted = "Digit8"; break;
                    case ')': shifted = "Digit9"; break;
                    case '_': shifted = "Digit0"; break;
                    case '=': shifted = "Minus"; break;     // CPC: the "- =" key
                    case '+': shifted = "Quote"; break;     // CPC: the "; +" key
                    case '*': shifted = "Semicolon"; break; // CPC: the ": *" key
                    case '<': shifted = "Comma"; break;
                    case '>': shifted = "Period"; break;
                    case '?': shifted = "Slash"; break;
                    case ',': code = "Comma"; break;
                    case '.': code = "Period"; break;
                    case ':': code = "Semicolon"; break;    // CPC matrix {3,5}
                    case ';': code = "Quote"; break;        // CPC matrix {3,4}
                    case '@': code = "BracketLeft"; break;  // CPC: the "@ |" key
                    case '^': code = "Equal"; break;        // CPC: the "^ Â£" key
                    case '-': code = "Minus"; break;
                    case '/': code = "Slash"; break;
                    case '\\': code = "Backslash"; break;
                    case '[': code = "BracketLeft"; break;
                    case ']': code = "BracketRight"; break;
                    case ' ': code = "Space"; break;
                    default:
                        if (c >= 'A' && c <= 'Z') code = std::string("Key") + c;
                        else if (c >= 'a' && c <= 'z') code = std::string("Key") + (char)(c - 32);
                        else if (c >= '0' && c <= '9') code = std::string("Digit") + c;
                        break;
                }
                if (!shifted.empty()) shiftTap(shifted);
                else if (!code.empty()) tap(code);
              }
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
                    } else if (kb) { kb->setKey(code, true); run(6); kb->setKey(code, false); run(6); }
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
                    case 's': host.render(); host.saveScreenshotBmp(tok.substr(1)); std::printf("[mouse] shot %s\n", tok.c_str() + 1); break;
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
            host.render();
            if (host.emu && host.emu->crtc) { auto* c = host.emu->crtc; std::printf("[regs] type=%d R0=%d R1=%d R2=%d R3=%d R4=%d R5=%d R6=%d R7=%d R8=%d R9=%d R12=%d R13=%d\n",
                c->type, c->registers[0],c->registers[1],c->registers[2],c->registers[3],c->registers[4],c->registers[5],c->registers[6],c->registers[7],c->registers[8],c->registers[9],c->registers[12],c->registers[13]); }
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

    int winW = geti("winw", 1440), winH = geti("winh", 900);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window* window = SDL_CreateWindow(
        "CPCSyntaxError", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        winW, winH, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CPCSyntaxError", SDL_GetError(), nullptr); SDL_Quit(); return 1; }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
	enableDarkTitleBar(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The window layout (docking, positions, sizes) is ImGui's own ini, beside cpcse.ini.
    static std::string layoutIni;
    {
        char* b = SDL_GetBasePath();
        layoutIni = std::string(b ? b : "") + "cpcse_layout.ini";
        if (b) SDL_free(b);
    }
    io.IniFilename = layoutIni.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;   // dragging inside the screen never moves it
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 130");

    EmuHost host;
    loadMachineSounds();
    GuiShell shell(host);
    shell.applyStyle();
    shell.loadSettings(ini);

    // Apply persisted settings (before any boot, so applySettings picks them up).
    if (ini.count("romdir")) host.setRomDir(ini["romdir"]);
    for (int i = 1; i < argc; i++) {   // command line overrides the ini
        std::string a = argv[i];
        if ((a == "--roms" || a == "-r") && i + 1 < argc) host.setRomDir(argv[++i]);
    }
    host.setMonitorMode(gets("monitor", ""));
    host.setMonitorSet(gets("monitorset", ""));
    host.audioEnabled   = geti("audio", 1) != 0;
    host.masterVolume   = (float)getf("volume", 0.6);
    host.driveSounds    = geti("drive_sounds", 1) != 0;
    host.keySounds      = geti("key_sounds", 0) != 0;
    host.mechanicsVolume = (float)getf("noises_volume", 0.5);
    host.dacType        = gets("dac", "none");
    host.keyboardRegion = gets("region", "uk");
    host.joystickEnabled = geti("joystick", 1) != 0;
    host.speed          = (float)getf("speed", 1.0);
    host.integerScale   = geti("integerscale", 0) != 0;
    host.maintainAspect = geti("aspect", 1) != 0;
    host.crtEffect      = geti("crt", 0) != 0;
    host.crtScanline    = (float)getf("crtscanline", 0.5);
    host.crtBloom       = (float)getf("crtbloom", 0.2);
    host.m4Folder       = gets("m4folder", "");
    host.m4Enabled      = geti("m4", 0) != 0;             // applied by applySettings during boot
    host.symbifaceModule = gets("symbiface", "none");
    host.mouseSensitivity = (float)getf("mousesens", 1.0);
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
    host.tapeRelayDelay = geti("taperelay", 1) != 0;
    host.beamRenderer   = geti("beam", 0) != 0;

    host.gateArrayPart  = geti("gatearray", 0);            // applied by bootModel
    // Boot the default machine.
    // With no ini (a first start) or a machine whose ROMs have gone: a CPC 6128, else the
    // first machine there are ROMs for.
    std::string savedModel = gets("model", "");
    int savedRam = geti("ram", -1), savedCrtc = geti("crtc", -1);
    if (geti("autoboot", 1)) {
        int pick = -1;
        auto find = [&](const std::string& id) {
            for (int i = 0; i < (int)host.models.size(); i++) if (host.models[i].id == id && host.models[i].available) return i;
            return -1;
        };
        if (!savedModel.empty()) pick = find(savedModel);
        if (pick < 0) { savedRam = -1; savedCrtc = -1; pick = find("cpc6128"); }
        for (int i = 0; pick < 0 && i < (int)host.models.size(); i++) if (host.models[i].available) pick = i;
        if (pick >= 0) host.bootModel(pick, savedRam, savedCrtc);
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

    const double FRAME_SECONDS = 1.0 / 50.08;
    uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prev = SDL_GetPerformanceCounter();
    double accumulator = 0.0;

    // Emulation-speed tracker (emulated frames vs real time).
    double spdTimer = 0.0;
    int emuFrameCount = 0;
    float emuSpeedPct = 0.0f;

    // Physical game controllers -> CPC joysticks (pad 0 -> joy 0, pad 1 -> joy 1
    // for GX4000 dual). Hot-pluggable.
    std::vector<SDL_GameController*> pads;
    auto openPads = [&]() {
        for (auto* c : pads) if (c) SDL_GameControllerClose(c);
        pads.clear();
        for (int i = 0; i < SDL_NumJoysticks() && (int)pads.size() < 2; i++)
            if (SDL_IsGameController(i)) { if (SDL_GameController* c = SDL_GameControllerOpen(i)) pads.push_back(c); }
    };
    openPads();
    auto pollPads = [&]() {
        if (!host.emu || !host.emu->gamepad) return;
        std::vector<Gamepad> gs;
        for (auto* c : pads) {
            if (!c) continue;
            Gamepad g; g.buttons.assign(16, false); g.axes.assign(2, 0.0);
            auto b = [&](int idx, SDL_GameControllerButton sb) { g.buttons[idx] = SDL_GameControllerGetButton(c, sb) != 0; };
            b(0, SDL_CONTROLLER_BUTTON_A); b(1, SDL_CONTROLLER_BUTTON_B);
            b(12, SDL_CONTROLLER_BUTTON_DPAD_UP); b(13, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            b(14, SDL_CONTROLLER_BUTTON_DPAD_LEFT); b(15, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            g.axes[0] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0;
            g.axes[1] = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0;
            gs.push_back(g);
        }
        host.emu->gamepad->update(gs);
    };

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
        while (SDL_PollEvent(&ev)) {
            const bool mouseEvent = ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEBUTTONDOWN
                                 || ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEWHEEL;
            if (!(host.mouseCaptured && mouseEvent)) ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (shell.quitRequested) running = false;
            if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) openPads();
            if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window)) running = false;
            if (ev.type == SDL_DROPFILE && ev.drop.file) { host.loadByExtension(ev.drop.file); SDL_free(ev.drop.file); }
            // While grabbed, the host mouse is the Symbiface mouse.
            if (host.mouseCaptured && !host.symbifaceMouseActive()) releaseMouse();
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
                if (ev.key.keysym.scancode == SDL_SCANCODE_F11) toggleFullscreen();
                else if (ev.key.keysym.scancode == SDL_SCANCODE_F12) releaseMouse();
                else if (shell.keyboardToCpc()) host.setKey(ev.key.keysym.scancode, true);
            }
        }
        pollPads();   // feed physical controllers into the CPC joysticks

        uint64_t now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) / (double)perfFreq;
        prev = now;
        accumulator += dt;
        bool ran = false;
        int framesThisIter = 0;
        auto feedAudio = [&]() {
            if (audioDev && !host.audioOut.empty() && !host.turbo &&
                SDL_GetQueuedAudioSize(audioDev) < (Uint32)(host.sampleRate * 2 * sizeof(int16_t) / 5))
                SDL_QueueAudio(audioDev, host.audioOut.data(), (Uint32)(host.audioOut.size() * sizeof(int16_t)));
        };
        if (host.turbo) { for (int i = 0; i < 8; i++) { host.runFrame(); feedAudio(); } ran = true; framesThisIter = 8; accumulator = 0.0; }
        else {
            double period = FRAME_SECONDS / std::max(0.1f, host.speed);
            int steps = 0;
            while (accumulator >= period && steps < 6) { host.runFrame(); feedAudio(); ran = true; steps++; accumulator -= period; }
            framesThisIter = steps;
            if (accumulator > 0.25) accumulator = 0.0;
        }
        emuFrameCount += framesThisIter;
        spdTimer += dt;
        if (spdTimer >= 0.5) { emuSpeedPct = (float)(emuFrameCount / (50.08 * spdTimer) * 100.0); emuFrameCount = 0; spdTimer = 0.0; }
        if (ran) {
            host.render();
            glBindTexture(GL_TEXTURE_2D, screenTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TEX_W, TEX_H, GL_RGBA, GL_UNSIGNED_BYTE, host.video->pixels.data());
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
        // Windows pulled out of the main window live in OS windows of their own.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backupWindow = SDL_GL_GetCurrentWindow();
            SDL_GLContext backupContext = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backupWindow, backupContext);
        }
        SDL_GL_SwapWindow(window);
    }

    // Persist settings.
    {
        int cw = winW, ch = winH;
        SDL_GetWindowSize(window, &cw, &ch);
        std::string defModel = host.currentModel >= 0 ? host.models[host.currentModel].id : savedModel;
        std::ofstream f(iniFilePath());
        if (f) {
            f << "# CPCSyntaxError settings\n";
            f << "romdir=" << host.romDir << "\n";
            f << "model=" << defModel << "\n";
            f << "ram=" << host.ramKiB << "\n";
            f << "crtc=" << host.crtcType << "\n";
            f << "autoboot=1\n";
            f << "monitor=" << host.monitorMode << "\n";
            f << "monitorset=" << host.monitorSetId << "\n";
            f << "gatearray=" << host.gateArrayPart << "\n";
            f << "audio=" << (host.audioEnabled ? 1 : 0) << "\n";
            f << "volume=" << host.masterVolume << "\n";
            f << "drive_sounds=" << (host.driveSounds ? 1 : 0) << "\n";
            f << "key_sounds=" << (host.keySounds ? 1 : 0) << "\n";
            f << "noises_volume=" << host.mechanicsVolume << "\n";
            f << "dac=" << host.dacType << "\n";
            f << "region=" << host.keyboardRegion << "\n";
            f << "joystick=" << (host.joystickEnabled ? 1 : 0) << "\n";
            f << "speed=" << host.speed << "\n";
            f << "integerscale=" << (host.integerScale ? 1 : 0) << "\n";
            f << "crt=" << (host.crtEffect ? 1 : 0) << "\n";
            f << "crtscanline=" << host.crtScanline << "\n";
            f << "crtbloom=" << host.crtBloom << "\n";
            f << "aspect=" << (host.maintainAspect ? 1 : 0) << "\n";
            f << "m4=" << (host.m4Enabled ? 1 : 0) << "\n";
            f << "m4folder=" << host.m4Folder << "\n";
            f << "symbiface=" << host.symbifaceModule << "\n";
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
            f << "taperelay=" << (host.tapeRelayDelay ? 1 : 0) << "\n";
            f << "beam=" << (host.beamRenderer ? 1 : 0) << "\n";
            shell.saveSettings(f);
            f << "winw=" << cw << "\n";
            f << "winh=" << ch << "\n";
        }
    }

    if (audioDev) SDL_CloseAudioDevice(audioDev);
    glDeleteTextures(1, &screenTex);
    for (auto& kv : toolTextures) glDeleteTextures(1, &kv.second.id);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    for (auto* c : pads) if (c) SDL_GameControllerClose(c);
    SDL_Quit();
    return 0;
}
