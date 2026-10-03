// CPCSyntaxError GUI — the settings sections (Video, Audio, Input, Expansions, tape
// options) and the tool windows (Printer, GFX9000, CSL scripts).
//
// Each section is drawn both inside the Settings window and as a menu of its own on the
// menu bar: plain widgets work in either, only the widths differ.
#include "gui_shell.h"
#include "core/machine_sounds.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <urlmon.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "emuhost.h"
#include "gui_widgets.h"
#include "core/emulator.h"
#include "core/matrix_printer.h"
#include "core/monitor_model.h"
#include "core/v9990.h"
#include "core/opl4.h"
#include "core/speech.h"

#ifndef _WIN32
namespace {
// Start a program with these arguments (no shell: nothing in them is interpreted), with
// stdout and stderr on `outFd` if it is not -1. Returns the child's pid, or -1.
pid_t spawnArgs(const std::vector<std::string>& args, int outFd, bool search) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        if (outFd >= 0) { dup2(outFd, 1); dup2(outFd, 2); close(outFd); }
        if (search) execvp(argv[0], argv.data()); else execv(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}
// ...and forget it, reaping it when it ends (a file manager, a browser).
void spawnDetached(const std::vector<std::string>& args) {
    const pid_t pid = spawnArgs(args, -1, true);
    if (pid > 0) std::thread([pid] { int st = 0; waitpid(pid, &st, 0); }).detach();
}
// This program's own path, to run its windowless modes.
std::string selfExecutable() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; return buf; }
    return "cpcse";
}
}  // namespace
#endif

namespace cpcse {

static float itemWidth(bool asMenu) { return asMenu ? 220.0f : -1.0f; }

// A row of radio buttons (window) or checkable items (menu) choosing one string value.
struct Choice { const char* label; const char* value; const char* tip; };
template <size_t N>
static void choiceRow(bool asMenu, const Choice (&items)[N], const std::string& current,
                      const std::function<void(const char*)>& pick) {
    for (size_t i = 0; i < N; i++) {
        bool chosen;
        if (asMenu) {
            chosen = ImGui::MenuItem(items[i].label, nullptr, current == items[i].value);
        } else {
            if (i) ImGui::SameLine();
            chosen = ImGui::RadioButton(items[i].label, current == items[i].value);
        }
        if (items[i].tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", items[i].tip);
        if (chosen) pick(items[i].value);
    }
}

static bool plusMachine(const EmuHost& host) {
    if (host.currentModel >= 0 && host.currentModel < (int)host.models.size()) return host.models[host.currentModel].plus;
    return !host.cartName.empty();
}

// ============================================================== Video
void GuiShell::sectionVideo(bool asMenu) {
    // The set itself is a chip of the machine (Machine > Monitor); here is only how its
    // picture is looked at.
    const MonitorModel* set = host.monitorSetId.empty() ? (host.emu ? host.emu->shippedMonitorModel() : nullptr)
                                                        : monitorModelFor(host.monitorSetId);
    if (set) ImGui::TextDisabled("Monitor: %s (Machine > Monitor)", set->name);

    static const Choice tubes[] = {
        { "As set", "", "The tube the monitor set has" },
        { "Colour", "colour", nullptr }, { "Green", "green", nullptr },
        { "Grey", "grey", nullptr }, { "Mono", "mono", nullptr } };
    auto pickTube = [this](const char* v) { host.setMonitorMode(v); };
    if (asMenu) {
        if (ImGui::BeginMenu("Tube")) { choiceRow(true, tubes, host.monitorMode, pickTube); ImGui::EndMenu(); }
    } else {
        ImGui::TextDisabled("Tube");
        choiceRow(false, tubes, host.monitorMode, pickTube);
    }

    if (asMenu) ImGui::Separator(); else sectionHeading("Renderer");
    static const Choice renderers[] = {
        { "Standard", "std", "Reconstructs each line from the Gate Array's per-line capture." },
        { "Beam (pin-accurate CRT)", "beam",
          "Draws each Gate Array character at the live CRT beam\nposition (HSYNC/VSYNC-driven). Classic machines only;\nthe Plus keeps the standard renderer." } };
    choiceRow(asMenu, renderers, host.beamRenderer ? "beam" : "std",
              [this](const char* v) { host.setBeamRenderer(std::string(v) == "beam"); });

    if (asMenu) ImGui::Separator(); else sectionHeading("Scaling");
    ImGui::Checkbox("Integer scaling", &host.integerScale);
    ImGui::Checkbox("Maintain aspect ratio", &host.maintainAspect);
    if (asMenu) {
        if (ImGui::MenuItem(host.fullscreen ? "Windowed" : "Fullscreen", "F11") && toggleFullscreen) toggleFullscreen();
    } else if (ImGui::Button(host.fullscreen ? "Windowed (F11)" : "Fullscreen (F11)") && toggleFullscreen) {
        toggleFullscreen();
    }

    if (asMenu) ImGui::Separator(); else sectionHeading("CRT effect");
    ImGui::Checkbox("CRT effect", &host.crtEffect);
    ImGui::BeginDisabled(!host.crtEffect);
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderFloat("##scan", &host.crtScanline, 0.0f, 1.0f, "scanlines %.2f");
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderFloat("##bloom", &host.crtBloom, 0.0f, 1.0f, "bloom %.2f");
    ImGui::EndDisabled();

    if (asMenu) ImGui::Separator(); else sectionHeading("GFX9000 monitor");
    static const Choice gfxPlace[] = {
        { "Beside the CPC screen", "beside", "Two monitors: its own picture next to the CPC's, in the Screen window" },
        { "Own window", "window", "Two monitors: the GFX9000's in the GFX9000 window" },
        { "One monitor (switched)", "switch",
          "One monitor: the GFX9000's picture while it displays one, the CPC's otherwise.\n"
          "A program hands it back with OUT &FF6F,&10 and takes it with OUT &FF6F,0." },
        { "One monitor via a Video9000", "video9000",
          "A Sunrise Video9000 on the GFX9000: its genlock superimposes the GFX9000's picture\n"
          "on the CPC's, as programs set it through &FF6F (GEN, TRAN, YMIX, YM, input) and\n"
          "the V9990's YS bits. Powers on at &10: the CPC's picture." } };
    ImGui::BeginDisabled(!host.v9990Enabled);
    if (!asMenu) {
        for (size_t i = 0; i < 4; i++) {      // too wide for one line in a docked window
            if (ImGui::RadioButton(gfxPlace[i].label, host.gfx9000Monitor == gfxPlace[i].value)) {
                host.gfx9000Monitor = gfxPlace[i].value;
                if (host.gfx9000Monitor == "window") panelOpen("GFX9000") = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", gfxPlace[i].tip);
        }
    } else {
        choiceRow(true, gfxPlace, host.gfx9000Monitor, [this](const char* v) {
            host.gfx9000Monitor = v;
            if (host.gfx9000Monitor == "window") panelOpen("GFX9000") = true;
        });
    }
    ImGui::EndDisabled();
}

// ============================================================== Audio
void GuiShell::sectionAudio(bool asMenu) {
    ImGui::Checkbox("Sound enabled", &host.audioEnabled);
    float volPct = host.masterVolume * 100.0f;
    ImGui::SetNextItemWidth(itemWidth(asMenu));
    if (ImGui::SliderFloat("##vol", &volPct, 0.0f, 100.0f, "volume %.0f%%")) host.masterVolume = volPct / 100.0f;
    // The machine's own noises, not its sound chip: synthesised, mixed beside the AY.
    if (asMenu) ImGui::Separator(); else sectionHeading("Machine noises");
    ImGui::Checkbox("Disc drive sounds", &host.driveSounds);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The 3-inch drive's motor, head steps and a disc going in");
    ImGui::Checkbox("Keyboard sounds", &host.keySounds);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A click for each key pressed and released");
    if (machineSounds.loadedCount() == 0)
        ImGui::TextDisabled("(no recordings in the \"sounds\" folder)");
    float mechPct = host.mechanicsVolume * 100.0f;
    ImGui::SetNextItemWidth(itemWidth(asMenu));
    if (ImGui::SliderFloat("##mechvol", &mechPct, 0.0f, 100.0f, "noises %.0f%%")) host.mechanicsVolume = mechPct / 100.0f;
    // The DACs sit on the printer port, so they share its one socket with the printers.
    if (asMenu) ImGui::Separator(); else sectionHeading("Printer-port DAC");
    static const Choice dacs[] = {
        { "None", "none", nullptr },
        { "DigiBlaster", "digiblaster", "8-bit DAC on the printer port (7 data lines + STROBE)" },
        { "AmDrum", "amdrum", "Cheetah AmDrum: 8-bit DAC at &FFxx" } };
    const bool isDac = host.dacType == "none" || host.dacType == "digiblaster" || host.dacType == "amdrum";
    choiceRow(asMenu, dacs, isDac ? host.dacType : std::string("-"), [this](const char* v) { host.setDacType(v); });
    if (!isDac) ImGui::TextDisabled("(the printer port holds a printer: Expansions)");
}

// ============================================================== Input
void GuiShell::lightgunItems(bool asMenu) {
    static const Choice guns[] = {
        { "None", "none", nullptr },
        { "Trojan Light Phazer", "trojan", "On the CRTC's light pen input (R16/R17)" },
        { "Gunstick", "gunstick", "Loriciel Gunstick: reads the brightness at the aim point" },
        { "West Phaser", "westphaser", "Loriciel West Phaser" } };
    if (!asMenu) {
        // four choices do not fit on one line in a docked window
        for (size_t i = 0; i < 4; i++) {
            if (i & 1) ImGui::SameLine(160.0f);
            if (ImGui::RadioButton(guns[i].label, host.lightgunType == guns[i].value)) host.setLightgun(guns[i].value);
            if (guns[i].tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", guns[i].tip);
        }
    } else {
        choiceRow(true, guns, host.lightgunType, [this](const char* v) { host.setLightgun(v); });
    }
    if (host.lightgunActive()) ImGui::TextDisabled("Aim with the mouse over the Screen; left button fires.");
}

void GuiShell::sectionInput(bool asMenu) {
    static const Choice regions[] = { { "UK", "uk", nullptr }, { "French", "fr", nullptr }, { "Spanish", "es", nullptr } };
    auto pickRegion = [this](const char* v) { host.setKeyboardRegion(v); };
    if (asMenu) {
        if (ImGui::BeginMenu("Keyboard layout")) { choiceRow(true, regions, host.keyboardRegion, pickRegion); ImGui::EndMenu(); }
    } else {
        ImGui::TextDisabled("Keyboard");
        choiceRow(false, regions, host.keyboardRegion, pickRegion);
    }

    if (asMenu) ImGui::Separator(); else sectionHeading("Joystick & mouse");
    bool joy = host.joystickEnabled;
    if (ImGui::Checkbox("Joystick emulation", &joy)) host.setJoystickEnabled(joy);
    float sens = host.mouseSensitivity;
    ImGui::SetNextItemWidth(itemWidth(asMenu));
    if (ImGui::SliderFloat("##sens", &sens, 0.1f, 4.0f, "mouse sensitivity %.2f")) host.setMouseSensitivity(sens);

    if (asMenu) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Lightgun")) { lightgunItems(true); ImGui::EndMenu(); }
    } else {
        sectionHeading("Lightgun");
        lightgunItems(false);
    }

    // THE PLUS ANALOGUE PORT: the ASIC's ADC0-7 (&6808-&680F), 6 bits each. Two paddles
    // or an analogue stick; 32 is centre.
    const bool plus = plusMachine(host);
    auto analogue = [&]() {
        ImGui::BeginDisabled(!plus);
        for (int ch = 0; ch < 8; ch++) {
            int v = host.analogue(ch);
            char id[16]; std::snprintf(id, sizeof id, "##adc%d", ch);
            char fmt[24]; std::snprintf(fmt, sizeof fmt, "ADC%d  %%d", ch);
            ImGui::SetNextItemWidth(asMenu ? 220.0f : -1.0f);
            if (ImGui::SliderInt(id, &v, 0, 63, fmt)) host.setAnalogue(ch, v);
        }
        if (ImGui::Button("Centre all")) for (int ch = 0; ch < 8; ch++) host.setAnalogue(ch, 32);
        ImGui::EndDisabled();
        if (!plus) ImGui::TextDisabled("CPC Plus / GX4000 only");
    };
    if (asMenu) {
        if (ImGui::BeginMenu("Plus analogue port")) { analogue(); ImGui::EndMenu(); }
    } else if (ImGui::TreeNode("Plus analogue port")) {
        analogue();
        ImGui::TreePop();
    }
}

// ============================================================== Expansions
void GuiShell::printerPortItems(bool asMenu) {
    static const Choice ports[] = {
        { "Nothing", "none", nullptr },
        { "Text printer", "printer", "Characters to the Printer window's Text tab" },
        { "Dot-matrix printer", "matrix", "Epson-style ESC/P printer: pages in the Printer window" },
        { "DigiBlaster", "digiblaster", "8-bit sound DAC" },
        { "AmDrum", "amdrum", "Cheetah AmDrum sound DAC" } };
    if (asMenu) {
        choiceRow(true, ports, host.dacType, [this](const char* v) { host.setDacType(v); });
    } else {
        ImGui::SetNextItemWidth(-1);
        const char* cur = "Nothing";
        for (const auto& p : ports) if (host.dacType == p.value) cur = p.label;
        if (ImGui::BeginCombo("##printerport", cur)) {
            for (const auto& p : ports) {
                if (ImGui::Selectable(p.label, host.dacType == p.value)) host.setDacType(p.value);
                if (p.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.tip);
            }
            ImGui::EndCombo();
        }
    }
}

void GuiShell::sectionExpansions(bool asMenu) {
    if (!asMenu) ImGui::TextDisabled("M4 board (SD card, network)");
    bool m4 = host.m4Enabled;
    if (ImGui::Checkbox("M4 board", &m4)) {
        if (m4 && !host.m4HasRom()) romPromptFor = "m4";
        else host.setM4(m4, host.m4Folder.empty() ? host.romDir : host.m4Folder);
    }
    if (asMenu) {
        if (ImGui::MenuItem("M4 folder..."))
            browser.openDir("M4 files folder", host.m4Folder.empty() ? host.romDir : host.m4Folder,
                            [this](const std::string& dir) { host.m4Folder = dir; if (host.m4Enabled) host.setM4(true, dir); });
        if (ImGui::MenuItem("Rescan M4 folder", nullptr, false, host.m4Enabled)) host.rescanM4();
    } else {
        ImGui::SameLine();
        if (ImGui::SmallButton("Folder..."))
            browser.openDir("M4 files folder", host.m4Folder.empty() ? host.romDir : host.m4Folder,
                            [this](const std::string& dir) { host.m4Folder = dir; if (host.m4Enabled) host.setM4(true, dir); });
        if (host.m4Enabled) { ImGui::SameLine(); if (ImGui::SmallButton("Rescan")) host.rescanM4(); }
        if (!host.m4Folder.empty()) ImGui::TextDisabled("  %s", host.m4Folder.c_str());
    }

    static const Choice sf[] = { { "Off", "none", nullptr }, { "SF2", "sf2", "Symbiface II: PS/2 mouse, RTC, IDE" },
                                 { "SF3", "sf3", "Symbiface III: USB mouse, RTC" } };
    auto pickSf = [this](const char* v) { host.setSymbiface(v); };
    if (asMenu) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Symbiface")) { choiceRow(true, sf, host.symbifaceModule, pickSf); ImGui::EndMenu(); }
    } else {
        sectionHeading("Symbiface (mouse + RTC)");
        choiceRow(false, sf, host.symbifaceModule, pickSf);
    }

    if (asMenu) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Printer port")) { printerPortItems(true); ImGui::EndMenu(); }
        ImGui::MenuItem("Printer output", nullptr, &panelOpen("Printer"), host.printerAttached());
    } else {
        sectionHeading("Printer port");
        printerPortItems(false);
        if (host.printerAttached() && ImGui::SmallButton("Show printer output")) panelOpen("Printer") = true;
    }

    bool gfx = host.v9990Enabled;
    if (asMenu) {
        ImGui::Separator();
        if (ImGui::MenuItem("GFX9000 (V9990)", nullptr, &gfx)) host.setV9990(gfx);
        ImGui::MenuItem("GFX9000 monitor", nullptr, &panelOpen("GFX9000"), host.v9990Enabled);
    } else {
        sectionHeading("Graphics cartridge");
        if (ImGui::Checkbox("GFX9000 (Yamaha V9990 at &FF60)", &gfx)) host.setV9990(gfx);
        if (host.v9990Enabled) { ImGui::SameLine(); if (ImGui::SmallButton("Monitor")) panelOpen("GFX9000") = true; }
    }

    // THE OPL4: a MoonSound-style card on the AMSDAP
    bool opl = host.opl4Enabled;
    static const int ramSizes[] = { 128, 640, 1024, 2048 };
    auto ramLabel = [](int k) { return k >= 1024 ? std::to_string(k / 1024) + " MB" : std::to_string(k) + "K"; };
    if (asMenu) {
        ImGui::Separator();
        bool pc = host.playCityEnabled;
        if (ImGui::MenuItem("PlayCity (2 x YMZ294 + CTC)", nullptr, &pc)) host.setPlayCity(pc);
        if (ImGui::BeginMenu("Speech synthesiser")) {
            static const std::pair<const char*, const char*> kinds[] = {
                { "None", "none" }, { "Amstrad SSA-1 (&FBEE)", "ssa1" }, { "dk'tronics (&FBFE)", "dktronics" },
                { "LambdaSpeak 3 + MP3 module", "lambdaspeak3" } };
            for (const auto& k : kinds)
                if (ImGui::MenuItem(k.first, nullptr, host.speechKind == k.second)) {
                    host.setSpeech(k.second);
                    if (host.speechKind != "none" && !host.speechHasRom()) romPromptFor = "sp0256";
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("OPL4 sound card (YMF278B)", nullptr, &opl)) {
            host.setOpl4(opl);
            if (opl && !host.opl4HasRom()) romPromptFor = "opl4";
        }
        if (ImGui::BeginMenu("OPL4 sample RAM", host.opl4Enabled)) {
            for (int k : ramSizes)
                if (ImGui::MenuItem(ramLabel(k).c_str(), nullptr, host.opl4RamKiB == k)) { host.opl4RamKiB = k; host.applyOpl4(); }
            ImGui::EndMenu();
        }
    } else {
        sectionHeading("Sound card");
        bool pc = host.playCityEnabled;
        if (ImGui::Checkbox("PlayCity (2 x YMZ294 + Z80 CTC, &F880-&F988)", &pc)) host.setPlayCity(pc);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("TotO's PlayCity: six more AY channels in stereo (one YMZ294 a side),\n"
                              "and a Z80 CTC for raster NMIs and IM2 timer interrupts.");
        {
            static const std::pair<const char*, const char*> kinds[] = {
                { "No speech synthesiser", "none" }, { "Amstrad SSA-1 (&FBEE)", "ssa1" }, { "dk'tronics (&FBFE)", "dktronics" },
                { "LambdaSpeak 3 + MP3 module", "lambdaspeak3" } };
            const char* shown = kinds[0].first;
            for (const auto& k : kinds) if (host.speechKind == k.second) shown = k.first;
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("speech", shown)) {
                for (const auto& k : kinds)
                    if (ImGui::Selectable(k.first, host.speechKind == k.second)) {
                        host.setSpeech(k.second);
                        if (host.speechKind != "none" && !host.speechHasRom()) romPromptFor = "sp0256";
                    }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("An SP0256-AL2 speech chip: Amstrad's SSA-1 or the dk'tronics synthesiser.");
            if (host.speechKind == "lambdaspeak3") {
                if (ImGui::Button("MP3 card folder...")) {
                    browser.openDir("LambdaSpeak 3 MP3 card", host.mp3Card.empty() ? host.romDir : host.mp3Card,
                                    [this](const std::string& dir) { host.mp3Card = dir; host.applySpeech(); });
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The Catalex MP3 module's micro-SD card: folders 01, 02 ... of files\n"
                                      "named 001xxx.mp3, 002xxx.mp3 ... (LambdaSpeak 3's MP3.BAS plays them).");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", host.mp3Card.empty() ? "(no card)" : host.mp3Card.c_str());
                if (host.emu && host.emu->speech && host.emu->speech->mp3.playing())
                    ImGui::TextDisabled("Playing %s", host.emu->speech->mp3.nowPlaying().c_str());
            }
            if (host.speechKind != "none" && !host.speechHasRom()) {
                ImGui::TextDisabled("No sp0256-al2.bin: silent");
                ImGui::SameLine(); if (ImGui::SmallButton("Get it...##sp")) romPromptFor = "sp0256";
            }
        }
        if (ImGui::Checkbox("OPL4 (YMF278B, AMSDAP &FFC4/&FF7E)", &opl)) {
            host.setOpl4(opl);
            if (opl && !host.opl4HasRom()) romPromptFor = "opl4";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A MoonSound-style OPL4: 18 FM channels (OPL3) and 24 wavetable channels.\n"
                              "SymbOS / SymAmp use it for FM, MOD and wavetable music.");
        ImGui::BeginDisabled(!host.opl4Enabled);
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("sample RAM", ramLabel(host.opl4RamKiB).c_str())) {
            for (int k : ramSizes)
                if (ImGui::Selectable(ramLabel(k).c_str(), host.opl4RamKiB == k)) { host.opl4RamKiB = k; host.applyOpl4(); }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (host.opl4Enabled) {
            ImGui::TextDisabled(host.opl4HasRom() ? "YRW801 sample ROM fitted" : "No yrw801*.rom: GM instruments silent");
            if (!host.opl4HasRom()) { ImGui::SameLine(); if (ImGui::SmallButton("Get it...")) romPromptFor = "opl4"; }
        }
    }
}

// ============================================================== tape deck
void GuiShell::sectionTape(bool asMenu) {
    bool motor = host.tapeFollowsMotor, relay = host.tapeRelayDelay;
    bool changed;
    if (asMenu) {
        changed = ImGui::MenuItem("Follow the motor relay", nullptr, &motor);
        changed |= ImGui::MenuItem("Relay start-up delay", nullptr, &relay, motor);
    } else {
        changed = ImGui::Checkbox("Follow the motor relay", &motor);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The tape only plays while the PPI switches the cassette motor on,\nas on a real CPC.");
        ImGui::SameLine();
        ImGui::BeginDisabled(!motor);
        changed |= ImGui::Checkbox("Relay delay", &relay);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The relay and the motor take a moment to come up to speed.");
    }
    if (changed) { host.tapeFollowsMotor = motor; host.tapeRelayDelay = relay; host.applyTapeOptions(); }
}

// ============================================================== lightgun aim
void GuiShell::lightgunFromScreen(float ox, float oy, float drawW, float drawH, int texW, int texH) {
    if (!screenHovered || drawW <= 0 || drawH <= 0) { host.lightgunAim(-1, -1, false); return; }
    const ImVec2 m = ImGui::GetIO().MousePos;
    const double x = (m.x - ox) / drawW * texW, y = (m.y - oy) / drawH * texH;
    if (x < 0 || y < 0 || x >= texW || y >= texH) { host.lightgunAim(-1, -1, false); return; }
    host.lightgunAim(x, y, ImGui::IsMouseDown(ImGuiMouseButton_Left));
}

// ============================================================== Printer window
void GuiShell::windowPrinter() {
    bool& open = panelOpen("Printer");
    if (!open) return;
    if (ImGui::Begin("Printer", &open)) {
        ImGui::TextDisabled("Printer port:");
        ImGui::SameLine();
        printerPortItems(false);
        // The tab of the printer that is fitted comes to the front when it is fitted.
        const bool refit = printerTabDevice != host.dacType;
        printerTabDevice = host.dacType;
        if (ImGui::BeginTabBar("##printertabs")) {
            if (ImGui::BeginTabItem("Text", nullptr, refit && host.dacType == "printer" ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ImGui::Button("Save as text...")) saver.open("Save printer text", host.romDir, "printer.txt",
                                                                [this](const std::string& p) { host.savePrinterText(p); });
                ImGui::SameLine();
                if (ImGui::Button("Clear")) host.clearPrinter();
                ImGui::SameLine();
                ImGui::TextDisabled("%zu characters", host.printerText.size());
                ImGui::InputTextMultiline("##printertext", host.printerText.data(), host.printerText.size() + 1,
                                          ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Dot-matrix", nullptr, refit && host.dacType == "matrix" ? ImGuiTabItemFlags_SetSelected : 0)) {
                MatrixPrinter* mp = host.matrixPrinter;
                const int pages = mp ? mp->pageCount() : 0;
                if (ImGui::Button("Save page as BMP...")) saver.open("Save printer page", host.romDir, "page.bmp",
                                                                     [this](const std::string& p) { host.savePrinterPageBmp(p); });
                ImGui::SameLine();
                if (ImGui::Button("Save page as SVG...")) saver.open("Save printer page", host.romDir, "page.svg",
                                                                     [this](const std::string& p) { host.savePrinterPageSvg(p); });
                ImGui::SameLine();
                if (ImGui::Button("Clear##mp")) host.clearPrinter();
                ImGui::SameLine();
                ImGui::TextDisabled("page %d of %d", mp ? mp->pageNumber() : 0, pages);
                if (mp && pages > 0 && uploadTexture) {
                    if (printerTextureRevision != host.printerRevision || !printerTexture) {
                        std::vector<uint8_t>& page = mp->pageData(mp->pageNumber() - 1);
                        printerTexture = uploadTexture(2, reinterpret_cast<const uint32_t*>(page.data()), mp->width, mp->height);
                        printerTextureRevision = host.printerRevision;
                    }
                    // the page, fitted to the window's width
                    const float w = ImGui::GetContentRegionAvail().x;
                    const float h = w * mp->height / (float)mp->width;
                    ImGui::BeginChild("##page", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::Image((ImTextureID)(intptr_t)printerTexture, ImVec2(w, h));
                    ImGui::EndChild();
                } else {
                    ImGui::TextDisabled(host.dacType == "matrix" ? "Nothing printed yet." : "Fit the dot-matrix printer to the printer port.");
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// ============================================================== a ROM that is missing
// The download runs on its own thread (URLDownloadToFile) into a temporary file, which is
// then checked and copied into the ROM folder like a chosen one.
struct GuiShell::RomFetch {
    std::atomic<bool> done { false };
    std::atomic<bool> ok { false };
    std::string path, error;
    std::thread worker;
    ~RomFetch() { if (worker.joinable()) worker.join(); }
};

// The ROMs that are not shipped, and are asked for when a device that needs one is fitted.
namespace {
struct RomNeed { const char* key; const char* title; const char* dest; size_t size; const char* blurb; };
const RomNeed kRomNeeds[] = {
    { "opl4", "OPL4 sample ROM", "yrw801.rom", 2097152,
      "The OPL4's General MIDI instruments are in Yamaha's YRW801 sample ROM (2 MB), which is not "
      "included with CPCSyntaxError. Without it the card still plays FM, and samples programs load into its RAM." },
    { "m4", "M4 board ROM", "M4ROM.ROM", 16384,
      "The M4 board needs its own ROM (M4ROM.ROM, 16K, by Duke -- spinpoint.org)." },
    { "sp0256", "Speech chip ROM", "sp0256-al2.bin", 2048,
      "The SSA-1 and dk'tronics speech synthesisers use General Instrument's SP0256-AL2, whose "
      "allophones are in the chip's own 2 KB ROM. It is not included with CPCSyntaxError." },
};
const RomNeed* romNeed(const std::string& key) {
    for (const RomNeed& n : kRomNeeds) if (key == n.key) return &n;
    return nullptr;
}
}

void GuiShell::romInstalled(const std::string& key) {
    if (key == "opl4") { host.emu->opl4->rom.clear(); host.applyOpl4(); }
    else if (key == "m4") host.setM4(true, host.m4Folder.empty() ? host.romDir : host.m4Folder);
    else if (key == "sp0256") { host.emu->speech->hasRom = false; host.applySpeech(); }
}

void GuiShell::romPrompt() {
    if (romPromptFor.empty()) return;
    const RomNeed* need = romNeed(romPromptFor);
    if (!need) { romPromptFor.clear(); return; }
    const char* title = need->title;
    const std::string dest = need->dest;
    const size_t size = need->size;
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    static std::string status;
    auto installed = [&](const std::string& from) {
        std::string why;
        if (!host.installRom(from, dest, size, why)) { status = "Not fitted: " + why; return; }
        status.clear();
        romInstalled(romPromptFor);
        romPromptFor.clear();
        ImGui::CloseCurrentPopup();
    };
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", need->blurb);
        ImGui::Spacing();
        ImGui::TextWrapped("Download it from a URL, or choose a copy you already have. It is saved to the ROM folder as %s.",
                           dest.c_str());
        ImGui::SetNextItemWidth(-90);
        ImGui::InputTextWithHint("##romurl", "https://...", romUrl, sizeof romUrl);
        ImGui::SameLine();
        const bool busy = romFetch && !romFetch->done;
        ImGui::BeginDisabled(busy || !romUrl[0]);
        if (ImGui::Button("Download", ImVec2(-1, 0))) {
            status = "Downloading...";
            auto fetch = std::make_shared<RomFetch>();
            std::error_code ec;
            fetch->path = (std::filesystem::temp_directory_path(ec) / ("cpcse_" + dest + ".part")).string();
            RomFetch* f = fetch.get();
            const std::string url = romUrl;
            fetch->worker = std::thread([f, url] {
#ifdef _WIN32
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                const HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), f->path.c_str(), 0, nullptr);
                CoUninitialize();
                f->ok = SUCCEEDED(hr);
                if (!f->ok) { char b[64]; std::snprintf(b, sizeof b, "download failed (0x%08lx)", (unsigned long)hr); f->error = b; }
#else
                // curl, else wget: whichever the system has. Exit 127 = not installed.
                auto fetch = [&](const std::vector<std::string>& args) {
                    const pid_t pid = spawnArgs(args, -1, true);
                    if (pid <= 0) return 127;
                    int st = 0; waitpid(pid, &st, 0);
                    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
                };
                int rc = fetch({ "curl", "-fsSL", "-o", f->path, url });
                if (rc == 127) rc = fetch({ "wget", "-q", "-O", f->path, url });
                f->ok = rc == 0;
                if (!f->ok) f->error = rc == 127 ? "needs curl or wget" : "download failed (exit " + std::to_string(rc) + ")";
#endif
                f->done = true;
            });
            romFetch = fetch;
        }
        ImGui::EndDisabled();
        if (romFetch && romFetch->done) {
            auto f = romFetch;
            romFetch.reset();
            if (f->ok) installed(f->path); else status = "Not fitted: " + f->error;
            std::error_code ec;
            std::filesystem::remove(f->path, ec);   // the download's own temporary file
        }
        if (ImGui::Button("Choose file...")) {
            browser.open(title, host.romDir, { ".rom", ".bin" }, [this](const std::string& p) {
                const RomNeed* n = romNeed(romPromptFor);
                if (!n) return;
                std::string why;
                if (!host.installRom(p, n->dest, n->size, why)) { romChooseStatus = "Not fitted: " + why; return; }
                romInstalled(romPromptFor);
                romPromptFor.clear();
            });
        }
        if (!romChooseStatus.empty()) { status = romChooseStatus; romChooseStatus.clear(); }
        ImGui::SameLine();
        if (ImGui::Button("Not now")) { romPromptFor.clear(); status.clear(); ImGui::CloseCurrentPopup(); }
        if (!status.empty()) ImGui::TextColored(status.rfind("Not", 0) == 0 ? ImVec4(1, 0.5f, 0.4f, 1) : kAccent, "%s", status.c_str());
        if (romPromptFor.empty()) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    (void)installed;
}

// ============================================================== GFX9000 monitor
const V9990Picture* GuiShell::gfxPictureTexture() {
    const V9990Picture* pic = host.v9990Picture();
    if (!pic || !uploadTexture) return nullptr;
    if (pic->fields != gfxTextureFields || !gfxTexture) {
        gfxTexture = uploadTexture(1, pic->pixels.data(), pic->width, pic->height);
        gfxTextureFields = pic->fields;
    }
    return pic;
}

void GuiShell::drawGfxMonitor(ImDrawList* dl, float x, float y, float w, float h) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0, 0, 0, 255));
    if (const V9990Picture* pic = gfxPictureTexture()) {
        dl->AddImage((ImTextureID)(intptr_t)gfxTexture, ImVec2(x, y), ImVec2(x + w, y + h));
        char label[48];
        std::snprintf(label, sizeof label, "GFX9000  %s", v9990ModeName(pic->mode));
        dl->AddText(ImVec2(x + 6, y + h - ImGui::GetTextLineHeight() - 4), IM_COL32(255, 255, 255, 90), label);
    } else {
        const char* msg = "GFX9000: no signal";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
    }
}

// Everything of the GFX9000's in one window: fitted or not, where its picture goes, the
// picture, and the V9990's state, registers, palette and VRAM.
void GuiShell::windowGfx9000() {
    if (!panelOpen("GFX9000")) return;
    if (beginTool("GFX9000")) {
        bool gfx = host.v9990Enabled;
        if (ImGui::Checkbox("Fitted", &gfx)) host.setV9990(gfx);
        ImGui::SameLine(0, 20);
        static const char* places[] = { "beside", "window", "switch", "video9000" };
        static const char* placeNames[] = { "beside the CPC's screen", "in this window", "one monitor, switched", "one monitor, via a Video9000" };
        int place = 0;
        for (int i = 0; i < 4; i++) if (host.gfx9000Monitor == places[i]) place = i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
        if (ImGui::Combo("picture", &place, placeNames, 4)) host.gfx9000Monitor = places[place];
        if (!host.v9990Enabled) ImGui::TextDisabled("Not fitted.");
        else if (ImGui::BeginTabBar("##gfxtabs")) {
          if (ImGui::BeginTabItem("Picture")) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float h = std::max(1.0f, std::min(avail.y, avail.x * 0.75f)), w = h * 4.0f / 3.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            drawGfxMonitor(ImGui::GetWindowDrawList(), at.x + (avail.x - w) * 0.5f, at.y, w, h);
            ImGui::InvisibleButton("##gfxarea", ImVec2(std::max(1.0f, avail.x), h));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && host.symbifaceMouseActive() && !host.mouseCaptured)
                mouseCaptureRequested = true;      // its picture grabs the mouse too
            if (const V9990Picture* pic = host.v9990Picture())
                ImGui::TextDisabled("%s, %d x %d, field %lld", v9990ModeName(pic->mode), pic->width, pic->height, pic->fields);
            ImGui::EndTabItem();
          }
          gfxInternalsTabs();
          ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// ============================================================== CSL scripts window
// A script runs in its own process -- this exe with --csl -- so it gets a machine of its
// own and the one on screen is left alone. Its console output is shown here.
struct GuiShell::CslRun {
    std::mutex lock;
    std::string log;
    std::atomic<bool> running { true };
    std::atomic<int> exitCode { -1 };
    std::thread reader;
#ifdef _WIN32
    HANDLE process = nullptr;
#else
    pid_t pid = -1;
#endif
    ~CslRun() {
#ifdef _WIN32
        if (running && process) TerminateProcess(process, 1);
#else
        if (running && pid > 0) kill(pid, SIGTERM);
#endif
        if (reader.joinable()) reader.join();
#ifdef _WIN32
        if (process) CloseHandle(process);
#endif
    }
};

#ifdef _WIN32
static std::string quoteArg(const std::string& s) { return "\"" + s + "\""; }
#endif

void GuiShell::windowCslScripts() {
    bool& open = panelOpen("CSL scripts");
    if (!open) return;
    if (ImGui::Begin("CSL scripts", &open)) {
        const bool busy = cslRun && cslRun->running;
        ImGui::BeginDisabled(busy);
        auto pathRow = [&](const char* label, std::string& value, const char* button, auto pick) {
            ImGui::TextUnformatted(label);
            ImGui::SameLine(110);
            ImGui::SetNextItemWidth(-90);
            char buf[512]; std::snprintf(buf, sizeof buf, "%s", value.c_str());
            char id[32]; std::snprintf(id, sizeof id, "##%s", label);
            if (ImGui::InputText(id, buf, sizeof buf)) value = buf;
            ImGui::SameLine();
            ImGui::PushID(label);
            if (ImGui::Button(button, ImVec2(-1, 0))) pick();
            ImGui::PopID();
        };
        pathRow("Script", cslScript, "Browse...", [&] {
            const std::string start = cslScript.empty() ? host.romDir : std::filesystem::path(cslScript).parent_path().string();
            browser.open("CSL script", start, { ".csl" }, [this](const std::string& p) { cslScript = p; });
        });
        pathRow("Screenshots", cslOut, "Folder...", [&] {
            browser.openDir("Screenshot folder", cslOut, [this](const std::string& d) { cslOut = d; });
        });
        pathRow("Disc folder", cslDiskDir, "Folder...", [&] {
            browser.openDir("Disc image folder", cslDiskDir.empty() ? host.romDir : cslDiskDir, [this](const std::string& d) { cslDiskDir = d; });
        });
        ImGui::TextUnformatted("CRTC");
        ImGui::SameLine(110);
        ImGui::SetNextItemWidth(220);
        const char* crtcs[] = { "As the script says", "0", "1", "2", "3", "4" };
        int sel = cslCrtc + 1;
        if (ImGui::Combo("##cslcrtc", &sel, crtcs, 6)) cslCrtc = sel - 1;
        ImGui::Checkbox("Follow csl_load chains", &cslChain);
        ImGui::SameLine();
        ImGui::Checkbox("Apply published-script errata", &cslErrata);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: play published scripts exactly as written.");
        ImGui::EndDisabled();

        ImGui::BeginDisabled(busy || cslScript.empty());
        if (ImGui::Button("Run")) {
#ifdef _WIN32
            char exe[MAX_PATH] = { 0 };
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string cmd = quoteArg(exe) + " --csl " + quoteArg(cslScript) + " --out " + quoteArg(cslOut)
                              + " --roms " + quoteArg(host.romDir);
            if (cslCrtc >= 0) cmd += " --crtc " + std::to_string(cslCrtc);
            if (!cslDiskDir.empty()) cmd += " --disk-dir " + quoteArg(cslDiskDir);
            if (!cslChain) cmd += " --no-chain";
            if (!cslErrata) cmd += " --no-errata";
            std::error_code ec;
            std::filesystem::create_directories(cslOut, ec);

            auto run = std::make_shared<CslRun>();
            SECURITY_ATTRIBUTES sa { sizeof sa, nullptr, TRUE };
            HANDLE readEnd = nullptr, writeEnd = nullptr;
            CreatePipe(&readEnd, &writeEnd, &sa, 0);
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOA si {};
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = writeEnd;
            si.hStdError = writeEnd;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION pi {};
            std::vector<char> line(cmd.begin(), cmd.end());
            line.push_back(0);
            if (CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                run->process = pi.hProcess;
                run->log = "> " + cmd + "\n";
                CslRun* r = run.get();
                run->reader = std::thread([r, readEnd] {
                    char buf[1024];
                    DWORD got = 0;
                    while (ReadFile(readEnd, buf, sizeof buf, &got, nullptr) && got) {
                        std::lock_guard<std::mutex> g(r->lock);
                        for (DWORD i = 0; i < got; i++) if (buf[i] != '\r') r->log.push_back(buf[i]);
                    }
                    CloseHandle(readEnd);
                    WaitForSingleObject(r->process, INFINITE);
                    DWORD code = 1;
                    GetExitCodeProcess(r->process, &code);
                    r->exitCode = (int)code;
                    r->running = false;
                });
            } else {
                CloseHandle(readEnd);
                run->log = "Could not start: " + cmd + "\n";
                run->running = false;
            }
            CloseHandle(writeEnd);
            cslRun = run;
#else
            std::vector<std::string> args = { selfExecutable(), "--csl", cslScript, "--out", cslOut, "--roms", host.romDir };
            if (cslCrtc >= 0) { args.push_back("--crtc"); args.push_back(std::to_string(cslCrtc)); }
            if (!cslDiskDir.empty()) { args.push_back("--disk-dir"); args.push_back(cslDiskDir); }
            if (!cslChain) args.push_back("--no-chain");
            if (!cslErrata) args.push_back("--no-errata");
            std::error_code ec;
            std::filesystem::create_directories(cslOut, ec);
            auto run = std::make_shared<CslRun>();
            std::string shown = ">";
            for (const std::string& a : args) shown += " " + a;
            int fds[2] = { -1, -1 };
            if (pipe(fds) == 0) {
                fcntl(fds[0], F_SETFD, FD_CLOEXEC);
                run->pid = spawnArgs(args, fds[1], false);
                close(fds[1]);
            }
            if (run->pid > 0) {
                run->log = shown + "\n";
                CslRun* r = run.get();
                const int readEnd = fds[0];
                run->reader = std::thread([r, readEnd] {
                    char buf[1024];
                    ssize_t got;
                    while ((got = read(readEnd, buf, sizeof buf)) > 0) {
                        std::lock_guard<std::mutex> g(r->lock);
                        for (ssize_t i = 0; i < got; i++) if (buf[i] != '\r') r->log.push_back(buf[i]);
                    }
                    close(readEnd);
                    int st = 0;
                    waitpid(r->pid, &st, 0);
                    r->exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
                    r->running = false;
                });
            } else {
                if (fds[0] >= 0) close(fds[0]);
                run->log = "Could not start:" + shown.substr(1) + "\n";
                run->running = false;
            }
            cslRun = run;
#endif
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!busy);
        if (ImGui::Button("Stop")) {
#ifdef _WIN32
            if (cslRun && cslRun->process) TerminateProcess(cslRun->process, 1);
#else
            if (cslRun && cslRun->pid > 0) kill(cslRun->pid, SIGTERM);
#endif
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Open screenshot folder")) {
#ifdef _WIN32
            std::error_code ec;
            const std::string abs = std::filesystem::absolute(cslOut, ec).string();
            ShellExecuteA(nullptr, "open", abs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
            std::error_code ec;
            std::filesystem::create_directories(cslOut, ec);
            spawnDetached({ "xdg-open", std::filesystem::absolute(cslOut, ec).string() });
#endif
        }
        if (cslRun) {
            ImGui::SameLine();
            if (cslRun->running) ImGui::TextColored(kAccent, "running...");
            else ImGui::TextDisabled(cslRun->exitCode == 0 ? "finished" : "stopped (exit %d)", (int)cslRun->exitCode);
            ImGui::BeginChild("##csllog", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> g(cslRun->lock);
                ImGui::TextUnformatted(cslRun->log.c_str(), cslRun->log.c_str() + cslRun->log.size());
            }
            if (cslRun->running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        } else {
            ImGui::TextDisabled("Plays a CSL script (SHAKER and other test suites) on a machine of its own;\n"
                                "the screenshots it asks for go to the screenshot folder.");
        }
    }
    ImGui::End();
}

} // namespace cpcse
