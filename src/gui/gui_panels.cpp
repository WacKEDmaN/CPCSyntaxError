// CPCSyntaxError GUI — the settings sections (Video, Audio, Input, Expansions, tape
// options) and the tool windows (Printer, GFX9000, CSL scripts).
//
// Each section is drawn both inside the Settings window and as a menu of its own on the
// menu bar: plain widgets work in either, only the widths differ.
#include "gui_shell.h"
#include "core/machine_sounds.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiViewportP::PlatformMonitor (the GFX9000 screen's display)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
#if defined(__APPLE__)
#include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif
#endif

#include "devserver.h"
#include "emuhost.h"
#include "gui_widgets.h"
#include "core/emulator.h"
#include "core/matrix_printer.h"
#include "core/monitor_model.h"
#include "core/sf3.h"
#include "core/hd20.h"
#include "core/v9990.h"
#include "core/opl4.h"
#include "core/speech.h"
#include "core/tape.h"

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
#if defined(__APPLE__)
    uint32_t size = sizeof buf;                  // macOS has no /proc
    if (_NSGetExecutablePath(buf, &size) == 0) return buf;
#else
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; return buf; }
#endif
    return "cpcse";
}
// The desktop's own "open this folder / file" program.
const char* desktopOpener() {
#if defined(__APPLE__)
    return "open";
#else
    return "xdg-open";
#endif
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
    FlowRow row;                               // the radio buttons wrap in a narrow window
    for (size_t i = 0; i < N; i++) {
        bool chosen;
        if (asMenu) {
            chosen = ImGui::MenuItem(items[i].label, nullptr, current == items[i].value);
        } else {
            chosen = row.radio(items[i].label, current == items[i].value);
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
    // The monitor set (a chip of the machine: CtmMonitor) and its two hold controls, then
    // how its picture is looked at.
    const MonitorModel* shipped = host.monitorSetId.empty() && host.emu ? host.emu->shippedMonitorModel() : nullptr;
    if (asMenu) {
        if (ImGui::BeginMenu("Monitor")) {
            monitorSetItems(true);
            if (shipped) ImGui::TextDisabled("As shipped: %s", shipped->name);
            ImGui::Separator();
            monitorFrontItems();
            ImGui::Separator();
            monitorSyncItems();
            ImGui::EndMenu();
        }
    } else {
        sectionHeading("Monitor");
        const MonitorModel* chosen = host.monitorSetId.empty() ? nullptr : monitorModelFor(host.monitorSetId);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##videomonitor", chosen ? chosen->name : "As shipped with the machine")) { monitorSetItems(false); ImGui::EndCombo(); }
        if (shipped) ImGui::TextDisabled("As shipped: %s", shipped->name);
        monitorFrontItems();
        monitorSyncItems();
        sectionHeading("Tube");
    }

    static const Choice tubes[] = {
        { "As set", "", "The tube the monitor set has" },
        { "Colour", "colour", nullptr }, { "Green", "green", nullptr },
        { "Grey", "grey", nullptr }, { "Mono", "mono", nullptr } };
    auto pickTube = [this](const char* v) { host.setMonitorMode(v); };
    if (asMenu) {
        if (ImGui::BeginMenu("Tube")) { choiceRow(true, tubes, host.monitorMode, pickTube); ImGui::EndMenu(); }
    } else {
        choiceRow(false, tubes, host.monitorMode, pickTube);
    }

    if (asMenu) ImGui::Separator(); else sectionHeading("Renderer");
    static const Choice renderers[] = {
        { "Standard", "std", "Reconstructs each line from the Gate Array's per-line capture." },
        { "Beam (pin-accurate CRT)", "beam",
          "Draws each Gate Array character at the live CRT beam\nposition (HSYNC/VSYNC-driven). Classic machines only;\nthe Plus keeps the standard renderer." } };
    choiceRow(asMenu, renderers, host.beamRenderer ? "beam" : "std",
              [this](const char* v) { host.setBeamRenderer(std::string(v) == "beam"); });

    // Scaling, the CRT effect and the advanced CRT filter: each screen has its own. With a
    // GFX9000 fitted, a choice says which screen's are shown here.
    if (host.v9990Enabled) {
        if (asMenu) ImGui::Separator(); else sectionHeading("Screen");
        static const Choice screens[] = {
            { "CPC screen", "cpc", "The settings below are the CPC monitor's" },
            { "GFX9000 screen", "gfx9000", "The settings below are the GFX9000 monitor's" } };
        choiceRow(asMenu, screens, lookScreen == 1 ? "gfx9000" : "cpc", [this](const char* v) { lookScreen = std::string(v) == "gfx9000" ? 1 : 0; });
        if (lookScreen == 1) {
            const bool copy = asMenu ? ImGui::MenuItem("Copy the CPC screen's settings") : ImGui::SmallButton("Copy the CPC screen's settings");
            if (copy) host.gfxLook = host.cpcLook;
        }
    }
    ScreenLook& look = editedLook();
    if (asMenu) ImGui::Separator(); else sectionHeading("Scaling");
    ImGui::Checkbox("Integer scaling", &look.integerScale);
    if (ImGui::IsItemHovered() && &look == &host.gfxLook)
        ImGui::SetTooltip("Each of the GFX9000's lines the same whole number of screen lines;\nits width follows the monitor's shape.");
    ImGui::Checkbox("Maintain aspect ratio", &look.maintainAspect);
    if (asMenu) {
        if (ImGui::MenuItem(host.fullscreen ? "Windowed" : "Fullscreen", "F11") && toggleFullscreen) toggleFullscreen();
    } else if (ImGui::Button(host.fullscreen ? "Windowed (F11)" : "Fullscreen (F11)") && toggleFullscreen) {
        toggleFullscreen();
    }
    ImGui::Checkbox("Full screen: the picture only", &fullscreenPictureOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Full screen shows the picture alone: no menus, windows or status bar.\nF11 or a double-click goes back.");

    if (asMenu) ImGui::Separator(); else sectionHeading("CRT effect");
    ImGui::Checkbox("CRT effect", &look.crtEffect);
    if (look.crtFilterActive()) ImGui::TextDisabled("(the advanced CRT filter is drawn instead)");
    ImGui::BeginDisabled(!look.crtEffect || look.crtFilterActive());
    static const Choice patterns[] = {
        { "As the set", "auto", "A colour set (CTM 640/644, CM14) has a slot mask;\na green or grey set (GT 64/65, MM12) shows scanlines." },
        { "Slot mask", "slot", "The phosphor stripes and slots of a colour tube" },
        { "Soft slot mask", "soft", "Fine dark lines over the picture, each pixel's colour kept whole\n(the look ACE-DL gives a CTM)" },
        { "Scanlines", "scanlines", "Dark lines between the CPC's lines" },
        { "None", "none", nullptr } };
    choiceRow(asMenu, patterns, look.crtPattern, [&look](const char* v) { look.crtPattern = v; });
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderFloat("##mask", &look.crtMask, 0.0f, 1.0f, "slot mask %.2f");
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderInt("##masksize", &look.crtMaskSize, 1, 4, "stripe %d px");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The width of one phosphor stripe on your screen: a triad is three.");
    if (asMenu) ImGui::MenuItem("Turn the mask 90 degrees", nullptr, &look.crtMaskTurned);
    else checkboxWrapped("Turn the mask 90 degrees", &look.crtMaskTurned);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The stripes across the screen and the slots in rows,\nnot the stripes up and down.");
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderFloat("##scan", &look.crtScanline, 0.0f, 1.0f, "scanlines %.2f");
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderFloat("##bloom", &look.crtBloom, 0.0f, 1.0f, "bloom %.2f");
    ImGui::SetNextItemWidth(itemWidth(asMenu)); ImGui::SliderFloat("##blur", &look.crtBlur, 0.0f, 1.0f, "blur %.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The beam's spot is round and soft: the pixels' edges soften, as on a tube.\n0 = sharp.");
    ImGui::EndDisabled();

    if (asMenu) ImGui::Separator(); else sectionHeading("Advanced CRT Filter");
    ImGui::Checkbox("Advanced CRT Filter", &look.crtFilter.enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The picture as a PAL signal on a curved tube: its colour spread and scanlines,\n"
                          "then the glass, the phosphor mask, bleed and glow. Drawn by the graphics card.");
    if (asMenu) {
        if (ImGui::BeginMenu("Advanced CRT Filter settings")) { crtFilterItems(true); ImGui::EndMenu(); }
    } else if (ImGui::TreeNode("Settings##crtfilter")) {
        crtFilterItems(false);
        ImGui::TreePop();
    }

    if (asMenu) ImGui::Separator(); else sectionHeading("GFX9000 monitor");
    static const Choice gfxPlace[] = {
        { "Beside the CPC screen", "beside", "Two monitors: its own picture next to the CPC's, in the Screen window" },
        { "Own window", "window", "Two monitors: the GFX9000's in the GFX9000 screen window, which can go onto\nanother display and be full screen there (F11 while it has the focus)" },
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
                if (host.gfx9000Monitor == "window") panelOpen("GFX9000 screen") = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", gfxPlace[i].tip);
        }
    } else {
        choiceRow(true, gfxPlace, host.gfx9000Monitor, [this](const char* v) {
            host.gfx9000Monitor = v;
            if (host.gfx9000Monitor == "window") panelOpen("GFX9000 screen") = true;
        });
    }
    ImGui::EndDisabled();
}

void GuiShell::crtFilterItems(bool asMenu) {
    CrtFilterSettings& f = editedLook().crtFilter;
    ImGui::BeginDisabled(!f.enabled);
    auto slider = [&](const CrtFilterSlider& s) {
        std::string format;
        for (const char* c = s.label; *c; c++) format += *c == '%' ? "%%" : std::string(1, *c);
        format += " %d";
        ImGui::SetNextItemWidth(itemWidth(asMenu));
        ImGui::SliderInt((std::string("##crtfilter.") + s.key).c_str(), &(f.*s.field), s.min, s.max, format.c_str(),
                         ImGuiSliderFlags_AlwaysClamp);
    };
    auto stage = [&](bool signal) {
        for (int i = 0; i < CRT_FILTER_SLIDER_COUNT; i++) {
            const CrtFilterSlider& s = CRT_FILTER_SLIDERS[i];
            if (s.signal != signal) continue;
            if (std::string(s.key) == "tint") {
                float rgb[3] = { ((f.tintColour >> 16) & 0xff) / 255.0f, ((f.tintColour >> 8) & 0xff) / 255.0f, (f.tintColour & 0xff) / 255.0f };
                if (ImGui::ColorEdit3("Tint colour##crtfilter", rgb, ImGuiColorEditFlags_NoInputs))
                    f.tintColour = (uint32_t)std::lround(rgb[0] * 255) << 16 | (uint32_t)std::lround(rgb[1] * 255) << 8 | (uint32_t)std::lround(rgb[2] * 255);
            }
            slider(s);
            if (std::string(s.key) == "darkness") {
                ImGui::SetNextItemWidth(itemWidth(asMenu));
                int dir = f.columns ? 1 : 0;
                if (ImGui::Combo("##crtfilter.direction", &dir, "Lines in rows\0Lines in columns\0")) f.columns = dir == 1;
            }
            if (std::string(s.key) == "mask") {
                ImGui::SetNextItemWidth(itemWidth(asMenu));
                ImGui::Combo("##crtfilter.maskshape", &f.maskShape, CRT_FILTER_MASK_SHAPES, 4);
            }
        }
    };
    ImGui::Checkbox("Signal: PAL colour and scanlines", &f.signal);
    ImGui::BeginDisabled(!f.signal);
    stage(true);
    ImGui::EndDisabled();
    if (asMenu) ImGui::Separator();
    ImGui::Checkbox("Screen: glass, mask and glow", &f.screen);
    ImGui::BeginDisabled(!f.screen);
    stage(false);
    ImGui::EndDisabled();
    if (asMenu) ImGui::Separator();
    if (ImGui::Button("Defaults##crtfilter")) { const bool on = f.enabled; f = CrtFilterSettings(); f.enabled = on; }
    ImGui::EndDisabled();
}

// ============================================================== Audio
void GuiShell::sectionAudio(bool asMenu) {
    ImGui::Checkbox("Sound enabled", &host.audioEnabled);
    float volPct = host.masterVolume * 100.0f;
    ImGui::SetNextItemWidth(itemWidth(asMenu));
    if (ImGui::SliderFloat("##vol", &volPct, 0.0f, 100.0f, "volume %.0f%%")) host.masterVolume = volPct / 100.0f;
    // Each source under the master volume, up to twice its own level; a right click sets 100%.
    if (asMenu) ImGui::Separator(); else sectionHeading("Sound sources");
    for (const auto& s : EmuHost::soundLevels()) {
        float& v = host.*(s.level);
        float pct = v * 100.0f;
        const bool fitted = host.soundFitted(s.key);
        const std::string format = std::string(s.name) + (fitted ? " %.0f%%" : " %.0f%% (off)");
        ImGui::SetNextItemWidth(itemWidth(asMenu));
        if (ImGui::SliderFloat((std::string("##vol_") + s.key).c_str(), &pct, 0.0f, 200.0f, format.c_str())) v = pct / 100.0f;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) v = 1.0f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: 0 to 200%% of its own level (right click: 100%%)%s", s.name, fitted ? "" : "\nNot fitted now: the level is kept for when it is.");
    }
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
    choiceRow(asMenu, guns, host.lightgunType, [this](const char* v) { host.setLightgun(v); });
    if (host.lightgunActive()) ImGui::TextDisabled("Aim with the mouse over the Screen; left button fires.");
}

// A CPC joystick's device: automatic, none, or one of those connected.
void GuiShell::joystickDeviceItems(int p, bool asMenu) {
    JoypadManager& pads = host.pads;
    auto item = [&](const std::string& label, const std::string& value) {
        const bool on = pads.port[p] == value;
        if (asMenu ? ImGui::MenuItem(label.c_str(), nullptr, on) : ImGui::Selectable(label.c_str(), on)) pads.port[p] = value;
    };
    const int now = pads.deviceFor(p);
    item(std::string("Automatic") + (pads.port[p] == "auto" && now >= 0 ? " (" + pads.devices[(size_t)now].name + ")" : ""), "auto");
    item("None", "none");
    for (size_t i = 0; i < pads.devices.size(); i++) {
        ImGui::PushID((int)i);
        item(pads.devices[i].name, pads.devices[i].guid);
        ImGui::PopID();
    }
    if (pads.devices.empty()) ImGui::TextDisabled("(no joystick or joypad connected)");
}

// Both CPC joysticks: the device of each, and its map, line by line, with press-to-assign.
void GuiShell::joystickSettings() {
    JoypadManager& pads = host.pads;
    for (int p = 0; p < 2; p++) {
        ImGui::PushID(p);
        const int d = pads.deviceFor(p);
        char title[32]; std::snprintf(title, sizeof title, "CPC joystick %d", p + 1);
        ImGui::TextUnformatted(title);
        if (p == 1) { ImGui::SameLine(); ImGui::TextDisabled("(the GX4000's second pad; on a CPC, a joystick on the first one's port)"); }
        fitNextItemWidth(320.0f, "##dev");
        const std::string shown = pads.port[p] == "auto" ? (d >= 0 ? "Automatic: " + pads.devices[(size_t)d].name : "Automatic (none connected)")
                                : pads.port[p] == "none" ? "None" : d >= 0 ? pads.devices[(size_t)d].name : "(not connected)";
        if (ImGui::BeginCombo("##dev", shown.c_str())) { joystickDeviceItems(p, false); ImGui::EndCombo(); }
        // a Plus: the stick as a digital joystick, or on the analogue port
        if (plusMachine(host)) {
            bool digital = !pads.analogue[p];
            if (ImGui::RadioButton("Digital", digital)) pads.analogue[p] = false;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The directions on the joystick port, as on every CPC");
            ImGui::SameLine();
            if (ImGui::RadioButton("Analogue (Plus)", !digital)) pads.analogue[p] = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The stick on the Plus analogue port (ADC %d and %d: X and Y, 0 to 63);\n"
                                  "the fire buttons stay on the joystick port", p * 2, p * 2 + 1);
            if (pads.analogue[p]) { ImGui::SameLine(); ImGui::TextDisabled("X %d  Y %d", host.analogue(p * 2), host.analogue(p * 2 + 1)); }
        }
        if (d >= 0) {
            PadDevice& dev = pads.devices[(size_t)d];
            PadMap& map = pads.mapFor(dev);
            const int held = pads.lines(p);
            if (ImGui::BeginTable("##map", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
                for (int l = 0; l < PAD_LINES; l++) {
                    ImGui::PushID(l);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    led((held >> l & 1) != 0, padLineName(l));
                    ImGui::TableNextColumn();
                    std::string list;
                    for (const PadBinding& b : map.line[l]) list += (list.empty() ? "" : ", ") + b.text();
                    const bool capturing = pads.capturePort == p && pads.captureLine == l;
                    if (capturing) ImGui::TextColored(kAccent, "Press a button or move a stick...");
                    else { ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("%s", list.empty() ? "(nothing)" : list.c_str()); ImGui::PopTextWrapPos(); }
                    ImGui::TableNextColumn();
                    if (capturing) { if (ImGui::SmallButton("Stop")) pads.capturePort = -1; }
                    else {
                        if (ImGui::SmallButton("Set")) pads.captureStart(p, l, false);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This line: the next button, stick or hat you move, in place of these");
                        if (after().smallButton("Add")) pads.captureStart(p, l, true);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This line: the next button, stick or hat you move, as well");
                        if (after().smallButton("Clear")) map.line[l].clear();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (ImGui::SmallButton("Defaults")) map = pads.defaultMap(dev);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(dev.controller ? "Its d-pad and left stick, A for fire 1 and B for fire 2" : "Hat 0, axes 0 and 1, buttons 0 and 1");
            after().textDisabled(dev.controller ? "a known joypad" : "a joystick (its own layout)");
        }
        ImGui::PopID();
        if (p == 0) ImGui::Spacing();
    }
}

void GuiShell::sectionInput(bool asMenu) {
    static const Choice regions[] = { { "UK", "uk", nullptr }, { "French", "fr", nullptr }, { "Spanish", "es", nullptr } };
    auto pickRegion = [this](const char* v) { host.setKeyboardRegion(v); };
    static const Choice mappings[] = {
        { "CPC layout", "cpc", "Each key is the CPC key in its place on the keyboard (as on a real CPC)" },
        { "PC layout", "pc", "A key types what it says on your keyboard: the CPC key that types that\n"
                             "character is pressed, with or without SHIFT. Keys that type nothing\n"
                             "(arrows, Enter, Ctrl, the function keys) stay in their place." } };
    auto pickMapping = [this](const char* v) { host.keyboardMapping = v; };
    if (asMenu) {
        if (ImGui::BeginMenu("CPC keyboard")) { choiceRow(true, regions, host.keyboardRegion, pickRegion); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Keys")) { choiceRow(true, mappings, host.keyboardMapping, pickMapping); ImGui::EndMenu(); }
    } else {
        ImGui::TextDisabled("CPC keyboard");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The CPC's own keyboard: it should match its ROM (a French CPC has French keys)");
        choiceRow(false, regions, host.keyboardRegion, pickRegion);
        ImGui::TextDisabled("Keys");
        choiceRow(false, mappings, host.keyboardMapping, pickMapping);
    }

    if (asMenu) {
        ImGui::Separator();
        for (int p = 0; p < 2; p++) {
            char label[24]; std::snprintf(label, sizeof label, "Joystick %d", p + 1);
            if (ImGui::BeginMenu(label)) { joystickDeviceItems(p, true); ImGui::EndMenu(); }
        }
        ImGui::MenuItem("On-screen CPC keyboard", nullptr, &panelOpen("CPC keyboard"));
        // the buttons are redefined in the settings window (a table, press to assign)
        if (ImGui::MenuItem("Joysticks and buttons...")) { panelOpen("Input settings") = true; focusWindowNextFrame = "Input settings"; }
    } else {
        sectionHeading("Joysticks");
        joystickSettings();
    }

    if (asMenu) ImGui::Separator(); else sectionHeading("Keyboard joystick & mouse");
    bool joy = host.joystickEnabled;
    if (ImGui::Checkbox("Arrow keys as joystick", &joy)) host.setJoystickEnabled(joy);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The arrow keys, Ctrl and Alt are CPC joystick 1 (and not keys)");
    float sens = host.mouseSensitivity;
    ImGui::SetNextItemWidth(itemWidth(asMenu));
    if (ImGui::SliderFloat("##sens", &sens, 0.1f, 4.0f, "mouse sensitivity %.2f")) host.setMouseSensitivity(sens);

    // THE MOUSE INTERFACE: one at a time, and none while a Symbiface II/III brings its own.
    static const Choice mice[] = {
        { "None##mouse", "none", nullptr },
        { "AMX mouse", "amx", "On the joystick port (AMX Art and others): a pulse on a joystick\n"
                              "direction for each step, its buttons on the fire lines" },
        { "Kempston mouse", "kempston", "Counters at &FBEE (X) and &FBEF (Y), buttons at &FAEF\n(OCP Art Studio, Carrier Command)" },
        { "MultiPlay, port A", "multiplay-a", "TotO's MultiPlay, the mouse in its port A (&F990, &F992/&F993)" },
        { "MultiPlay, port B", "multiplay-b", "TotO's MultiPlay, the mouse in its port B (&F991, &F994/&F995)" } };
    const bool sfMouse = host.symbifaceModule == "sf2" || host.symbifaceModule == "sf3";
    auto mouseItems = [&](bool menu) {
        ImGui::BeginDisabled(sfMouse);
        choiceRow(menu, mice, host.mouseInterface, [this](const char* v) { host.setMouseInterface(v); });
        ImGui::EndDisabled();
        if (sfMouse) ImGui::TextDisabled("Off: the Symbiface %s has its own mouse", host.symbifaceModule == "sf2" ? "II" : "III");
        else if (host.mouseInterfaceActive()) ImGui::TextDisabled("Click the picture to use the mouse; F12 or the middle button lets it go.");
    };
    if (asMenu) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Mouse interface")) { mouseItems(true); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Lightgun")) { lightgunItems(true); ImGui::EndMenu(); }
    } else {
        sectionHeading("Mouse interface");
        mouseItems(false);
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
        if (after().smallButton("Folder..."))
            browser.openDir("M4 files folder", host.m4Folder.empty() ? host.romDir : host.m4Folder,
                            [this](const std::string& dir) { host.m4Folder = dir; if (host.m4Enabled) host.setM4(true, dir); });
        if (host.m4Enabled) { if (after().smallButton("Rescan")) host.rescanM4(); }
        if (!host.m4Folder.empty()) ImGui::TextDisabled("  %s", host.m4Folder.c_str());
    }
    if (asMenu) { if (ImGui::MenuItem("M4 WiFi: the PC's network", nullptr, &host.m4Network)) host.applyM4Network(); }
    else if (ImGui::Checkbox("WiFi: the PC's network", &host.m4Network)) host.applyM4Network();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The M4's sockets, |HTTPGET and |HTTPMEM reach the internet through this computer.\n"
                          "Off: the board has no network.");
    if (host.m4Network) {
        if (asMenu) { if (ImGui::MenuItem("M4 servers: accept other computers", nullptr, &host.m4NetworkLan)) host.applyM4Network(); }
        else if (checkboxWrapped("Servers: accept other computers", &host.m4NetworkLan)) host.applyM4Network();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A CPC program that listens (C_NETBIND) is reachable from your network.\n"
                              "Off: only from this computer (127.0.0.1).");
    }

    bool mf = host.multifaceEnabled;
    if (asMenu) {
        ImGui::Separator();
        if (ImGui::MenuItem("Multiface II", nullptr, &mf)) host.setMultiface(mf);
        if (ImGui::MenuItem("Multiface II: STOP", "F10", false, host.multifaceEnabled)) host.multifaceStop();
    } else {
        sectionHeading("Multiface II");
        if (ImGui::Checkbox("Multiface II", &mf)) host.setMultiface(mf);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Romantic Robot's freezer: F10 is its STOP button.\nIts ROM is not included: a file with \"multiface\" in its name in the roms folder.");
        if (host.multifaceEnabled) { if (after().smallButton("STOP (F10)")) host.multifaceStop(); }
    }

    // Amstrad's RS232C interface, its line plugged into this computer
    {
        bool on = host.serialEnabled;
        if (asMenu) {
            ImGui::Separator();
            if (ImGui::MenuItem("RS232C serial interface", nullptr, &on)) host.setSerial(on);
        } else {
            sectionHeading("RS232C serial interface");
            if (ImGui::Checkbox("Amstrad RS232C", &on)) host.setSerial(on);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A Z80 DART (&FADC-&FADF) and an 8253 (&FBDC-&FBDF), as Amstrad's and Pace's cards.\n"
                                  "Its ROM is not included: put it in a ROM slot (Expansion ROMs) if the software needs it.");
            static const Choice kinds[] = { { "Listen on a port", "listen", "Another program connects to this computer's port (telnet localhost 2323)" },
                                            { "Connect to host:port", "tcp", "A TCP connection out: a BBS, a telnet server" },
                                            { "Serial port", "com", "One of this computer's serial ports (COM3, /dev/ttyUSB0)" },
                                            { "Loopback plug", "loopback", "TXD to RXD, RTS to CTS, DTR to DCD" } };
            const char* shown = kinds[0].label;
            for (const Choice& k : kinds) if (host.serialKind == k.value) shown = k.label;
            fitNextItemWidth(180, "Line");
            if (ImGui::BeginCombo("Line", shown)) {
                for (const Choice& k : kinds) {
                    if (ImGui::Selectable(k.label, host.serialKind == k.value)) {
                        host.serialKind = k.value;
                        if (host.serialKind == "listen") host.serialTarget = "2323";
                        else if (host.serialKind == "tcp") host.serialTarget = "localhost:23";
                        else if (host.serialKind == "com") host.serialTarget = "COM1";
                        if (host.serialEnabled) host.connectSerial();
                    }
                    if (k.tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.tip);
                }
                ImGui::EndCombo();
            }
            if (host.serialKind != "loopback") {
                static char target[128] = "";
                static std::string shownFor;
                if (shownFor != host.serialTarget) { std::snprintf(target, sizeof target, "%s", host.serialTarget.c_str()); shownFor = host.serialTarget; }
                const char* targetLabel = host.serialKind == "listen" ? "Port" : host.serialKind == "tcp" ? "Host:port" : "Device";
                fitNextItemWidth(180, targetLabel);
                if (ImGui::InputText(targetLabel, target, sizeof target,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    host.serialTarget = shownFor = target;
                    if (host.serialEnabled) host.connectSerial();
                }
            }
            if (host.serialKind == "listen" && ImGui::Checkbox("Accept other computers##serial", &host.serialLan) && host.serialEnabled) host.connectSerial();
            if (host.serialEnabled) {
                if (after().smallButton("Reconnect")) host.connectSerial();
                ImGui::TextDisabled("  %s", host.serialStatus().c_str());
            }
        }
    }

    static const Choice sf[] = { { "Off", "none", nullptr }, { "SF2", "sf2", "Symbiface II: PS/2 mouse, RTC, IDE/CF" },
                                 { "SF3", "sf3", "Symbiface III: USB mouse, RTC, IDE/CF" },
                                 { "X-MASS", "xmass", "X-MASS: the IDE/CF interface alone (&FD08-&FD0F)" } };
    auto pickSf = [this](const char* v) { host.setSymbiface(v); };
    auto pickIde = [this]() {
        browser.openDir("Symbiface IDE folder", host.ideFolder.empty() ? host.romDir : host.ideFolder,
                        [this](const std::string& dir) { host.setIdeFolder(dir); });
    };
    const bool sfOn = host.symbifaceModule != "none";
    auto pickSf3 = [this](bool usb) {
        std::string& f = usb ? host.sf3UsbFolder : host.sf3SdFolder;
        browser.openDir(usb ? "SF3 USB stick folder" : "SF3 SD card folder", f.empty() ? host.romDir : f,
                        [this, usb](const std::string& dir) { host.setSf3Folder(usb, dir); });
    };
    if (asMenu) {
        ImGui::Separator();
        if (ImGui::BeginMenu("Symbiface")) {
            choiceRow(true, sf, host.symbifaceModule, pickSf);
            ImGui::Separator();
            if (ImGui::MenuItem("IDE folder...")) pickIde();
            if (ImGui::MenuItem("Rescan IDE folder", nullptr, false, sfOn)) host.rescanIde();
            ImGui::Separator();
            if (ImGui::MenuItem("SF3 USB stick folder...")) pickSf3(true);
            if (ImGui::MenuItem("SF3 SD card folder...")) pickSf3(false);
            ImGui::MenuItem("SF3 OLED display", nullptr, &panelOpen("SF3 OLED"), host.symbifaceModule == "sf3");
            ImGui::EndMenu();
        }
    } else {
        sectionHeading("Symbiface / X-MASS (mouse, RTC, IDE/CF)");
        choiceRow(false, sf, host.symbifaceModule, pickSf);
        ImGui::TextUnformatted("IDE disc");
        if (after().smallButton("Folder...##ide")) pickIde();
        if (sfOn) { if (after().smallButton("Rescan##ide")) host.rescanIde(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The IDE/CF drive is this folder, as one FAT16 partition:\nwhat SymbOS writes there goes back into the folder.");
        if (!host.ideFolder.empty()) ImGui::TextDisabled("  %s", host.ideFolder.c_str());
        if (host.symbifaceModule == "sf3") {
            ImGui::TextUnformatted("SF3 USB stick");
            if (after().smallButton("Folder...##sf3usb")) pickSf3(true);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drive 1: of the SE-ONE player (SEPLAYER, AT+MSDPLAY) and SymbOS's SF3 drive:\nthis folder, as one FAT16 disc; what is written there goes back into the folder.");
            if (!host.sf3UsbFolder.empty()) ImGui::TextDisabled("  %s", host.sf3UsbFolder.c_str());
            // Two discs made of one folder each write their own changes back: one can lose the other's.
            auto same = [](const std::string& a, const std::string& b) {
                std::error_code ec;
                return !a.empty() && !b.empty() && std::filesystem::equivalent(std::filesystem::u8path(a), std::filesystem::u8path(b), ec);
            };
            if (same(host.sf3UsbFolder, host.m4Enabled ? host.m4Folder : std::string()) || same(host.sf3UsbFolder, host.ideFolder))
                ImGui::TextDisabled("  The M4 or the IDE uses this folder too: write to it from one of them only.");
            ImGui::TextUnformatted("SF3 SD card");
            if (after().smallButton("Folder...##sf3sd")) pickSf3(false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drive 0: (SAMPLE, SETALK and the other system files).");
            if (!host.sf3SdFolder.empty()) ImGui::TextDisabled("  %s", host.sf3SdFolder.c_str());
            if (after().smallButton("OLED display##sf3")) panelOpen("SF3 OLED") = true;
            // the SD card's ROM banks (CPC/ROMTABLE/BANKnnn), as the SF3 loads them at power-on
            const auto banks = host.sf3Banks();
            const int inUse = host.sf3BankInUse();
            std::string inUseName = "none on the card";
            for (const auto& b : banks) if (b.number == inUse) inUseName = std::to_string(b.number) + "  " + b.name;
            std::string now = "None";
            if (host.sf3RomBank < 0) now = "Automatic: " + inUseName;
            for (const auto& b : banks) if (b.number == host.sf3RomBank) now = std::to_string(b.number) + "  " + b.name;
            if (host.sf3RomBank > 0 && now == "None") now = std::to_string(host.sf3RomBank) + " (not on the card)";
            ImGui::TextUnformatted("SF3 ROM bank");
            ImGui::SetNextItemWidth(itemWidth(false));
            if (ImGui::BeginCombo("##sf3rombank", now.c_str())) {
                if (ImGui::Selectable("Automatic (the card's bank for this machine)", host.sf3RomBank < 0)) host.setSf3RomBank(-1);
                if (ImGui::Selectable("None", host.sf3RomBank == 0)) host.setSf3RomBank(0);
                for (const auto& b : banks) {
                    const std::string label = std::to_string(b.number) + "  " + b.name;
                    if (ImGui::Selectable(label.c_str(), b.number == host.sf3RomBank)) host.setSf3RomBank(b.number);
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The ROMs of SD card CPC\\ROMTABLE\\BANKnnn: ROMnnn.BIN goes in ROM slot nnn (ROM127 is the lower ROM).\n"
                                  "They start at the next reset. ROM 7 needs ROM7_ENA=ON in SF3_CPC.INI on the card.");
            if (banks.empty()) ImGui::TextDisabled("  (the SD card folder has no CPC\\ROMTABLE banks)");
        }
    }
    // The Dobbertin HD20: an image file as its disc (X-DDOS 2.10 in a ROM slot drives it).
    auto pickHd20 = [this]() {
        const std::string start = host.hd20Image.empty() ? host.mediaDir : std::filesystem::u8path(host.hd20Image).parent_path().u8string();
        browser.open("Dobbertin HD20 image", start, { ".img" }, [this](const std::string& f) { host.setHd20Image(f); });
    };
    const Hd20* hd = host.emu ? host.emu->hd20 : nullptr;
    if (asMenu) {
        if (ImGui::BeginMenu("Dobbertin HD20")) {
            if (ImGui::MenuItem("Image...")) pickHd20();
            if (ImGui::MenuItem("Remove the image", nullptr, false, !host.hd20Image.empty())) host.setHd20Image("");
            ImGui::EndMenu();
        }
    } else {
        sectionHeading("Dobbertin HD20 (hard disc)");
        ImGui::TextUnformatted("Image");
        if (after().smallButton("File...##hd20")) pickHd20();
        if (!host.hd20Image.empty()) { if (after().smallButton("Remove##hd20")) host.setHd20Image(""); }
        if (ImGui::IsItemHovered() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A 21 MB HD20 image (614 cylinders, 4 heads, 17 sectors). Put X-DDOS 2.10 in a ROM slot to use it.\n"
                              "With no image here, an SF3's USB stick HD20\\HD20_01.IMG is used.");
        if (hd && hd->enabled()) ImGui::TextDisabled("  %s%s", hd->image().c_str(), hd->readOnly ? " (read only)" : "");
        else ImGui::TextDisabled("  (no image)");
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
        if (checkboxWrapped("GFX9000 (Yamaha V9990 at &FF60)", &gfx)) host.setV9990(gfx);
        if (host.v9990Enabled) { if (after().smallButton("Monitor")) panelOpen("GFX9000") = true; }
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
        if (checkboxWrapped("PlayCity (2 x YMZ294 + Z80 CTC, &F880-&F988)", &pc)) host.setPlayCity(pc);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("TotO's PlayCity: six more AY channels in stereo (one YMZ294 a side),\n"
                              "and a Z80 CTC for raster NMIs and IM2 timer interrupts.");
        {
            static const std::pair<const char*, const char*> kinds[] = {
                { "No speech synthesiser", "none" }, { "Amstrad SSA-1 (&FBEE)", "ssa1" }, { "dk'tronics (&FBFE)", "dktronics" },
                { "LambdaSpeak 3 + MP3 module", "lambdaspeak3" } };
            const char* shown = kinds[0].first;
            for (const auto& k : kinds) if (host.speechKind == k.second) shown = k.first;
            fitNextItemWidth(220, "speech");
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
                after().textDisabled(host.mp3Card.empty() ? "(no card)" : host.mp3Card.c_str());
                if (host.emu && host.emu->speech && host.emu->speech->mp3.playing())
                    ImGui::TextDisabled("Playing %s", host.emu->speech->mp3.nowPlaying().c_str());
            }
            if (host.speechKind != "none" && !host.speechHasRom()) {
                ImGui::TextDisabled("No sp0256-al2.bin: silent");
                if (after().smallButton("Get it...##sp")) romPromptFor = "sp0256";
            }
        }
        if (checkboxWrapped("OPL4 (YMF278B, AMSDAP &FFC4/&FF7E)", &opl)) {
            host.setOpl4(opl);
            if (opl && !host.opl4HasRom()) romPromptFor = "opl4";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A MoonSound-style OPL4: 18 FM channels (OPL3) and 24 wavetable channels.\n"
                              "SymbOS / SymAmp use it for FM, MOD and wavetable music.");
        ImGui::BeginDisabled(!host.opl4Enabled);
        fitNextItemWidth(120, "sample RAM");
        if (ImGui::BeginCombo("sample RAM", ramLabel(host.opl4RamKiB).c_str())) {
            for (int k : ramSizes)
                if (ImGui::Selectable(ramLabel(k).c_str(), host.opl4RamKiB == k)) { host.opl4RamKiB = k; host.applyOpl4(); }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (host.opl4Enabled) {
            ImGui::TextDisabled(host.opl4HasRom() ? "YRW801 sample ROM fitted" : "No yrw801*.rom: GM instruments silent");
            if (!host.opl4HasRom()) { if (after().smallButton("Get it...")) romPromptFor = "opl4"; }
        }
    }
}

// ============================================================== tape deck
void GuiShell::sectionTape(bool asMenu) {
    bool motor = host.tapeFollowsMotor, relay = host.tapeRelayDelay, fast = host.tapeFastLoad, instant = host.tapeInstantLoad;
    int baud = host.tapeFastBaud;
    bool changed;
    if (asMenu) {
        changed = ImGui::MenuItem("Follow the motor relay", nullptr, &motor);
        changed |= ImGui::MenuItem("Relay start-up delay", nullptr, &relay, motor);
        changed |= ImGui::MenuItem("Fast loading (firmware blocks)", nullptr, &fast);
        changed |= ImGui::MenuItem("Instant loading (firmware blocks)", nullptr, &instant);
    } else {
        changed = ImGui::Checkbox("Follow the motor relay", &motor);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The tape only plays while the PPI switches the cassette motor on,\nas on a real CPC.");
        ImGui::BeginDisabled(!motor);
        changed |= after().checkbox("Relay delay", &relay);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The relay and the motor take a moment to come up to speed.");
        // fast loading: the tape's signal itself faster, block by block
        changed |= ImGui::Checkbox("Fast loading", &fast);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The blocks that the CPC's own firmware reads play at the speed below.\n"
                              "The firmware times the bits against the leader, so it reads them.\n"
                              "A block that a custom or protected loader reads plays at its own speed.\n"
                              "The block list shows the decision for each block.");
        ImGui::BeginDisabled(!fast);
        // the speed as a baud rate, and the firmware's "0" pulse that it gives (in T-states)
        char shown[64];
        std::snprintf(shown, sizeof shown, "%%d baud (\"0\" pulse %d T)", (int)std::lround(3500000.0 / (3.0 * std::max(1, baud))));
        FlowRow row = after();
        row.place(200.0f);
        ImGui::SetNextItemWidth(-1);
        changed |= ImGui::SliderInt("##tapefastbaud", &baud, CPCTapeDrive::FAST_BAUD_MIN, CPCTapeDrive::FAST_BAUD_MAX, shown, ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The speed of the fast blocks. Each tape has its own speed: a block at\n"
                              "1000 baud plays 3x faster at 3000, a block at 2000 baud 1.5x.\n"
                              "A block that is already as fast plays as it is.\n"
                              "Lower it if a tape does not load.");
        ImGui::EndDisabled();
        changed |= checkboxWrapped("Instant: the firmware's blocks straight into memory", &instant);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When the firmware reads a block (CAS READ), its bytes go into memory at once:\n"
                              "no wait, no border stripes, no tape sound. A block that is not in the\n"
                              "firmware's format (a custom or protected loader) plays as it is.");
    }
    if (changed) {
        host.tapeFollowsMotor = motor; host.tapeRelayDelay = relay; host.tapeFastLoad = fast; host.tapeFastBaud = baud; host.tapeInstantLoad = instant;
        host.applyTapeOptions();
    }
}

// ============================================================== the tape deck
// The deck's face: its counter (000-999, a second a count, with its reset), where the tape
// is of how long, which block is under the head, the motor lamp, and the buttons -- start,
// back a block, PLAY, PAUSE, STOP, on a block, EJECT -- plus any block to go to.
void GuiShell::tapeDeckControls(bool asMenu) {
    CPCTapeDrive* deck = host.tapeDeck();
    const bool in = deck != nullptr;
    const int blocks = in ? (int)deck->blocks.size() : 0;
    const int block = in ? deck->blockAtPosition() : -1;
    auto clock = [](double s) {
        const int t = (int)s;
        char b[16];
        std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
        return std::string(b);
    };
    if (asMenu) {
        if (in) ImGui::TextDisabled("%03d   %s / %s   block [%d] of %d", deck->counter(), clock(deck->positionSeconds()).c_str(),
                                    clock(deck->lengthSeconds()).c_str(), std::max(0, block), blocks);
        if (ImGui::MenuItem("Play", nullptr, host.tapePlaying(), in)) host.tapePlay();
        if (ImGui::MenuItem("Pause", nullptr, host.tapePaused(), in && deck->playing)) host.tapePause();
        if (ImGui::MenuItem("Stop", nullptr, false, in && deck->playing)) host.tapeStop();
        if (ImGui::MenuItem("Fast forward (next block)", nullptr, false, in)) host.tapeFastForward();
        if (ImGui::MenuItem("Rewind (a block)", nullptr, false, in)) host.tapeRewindBlock();
        if (ImGui::MenuItem("Rewind to the start", nullptr, false, in)) host.tapeRewind();
        if (ImGui::MenuItem("Reset the counter", nullptr, false, in)) host.tapeResetCounter();
        if (ImGui::MenuItem("Record", nullptr, host.tapeRecording())) host.tapeRecord();
        if (ImGui::MenuItem("Turbo loading", nullptr, host.tapeTurbo > 1))
            host.tapeTurbo = host.tapeTurbo > 1 ? 1 : EmuHost::TAPE_TURBO_UNLIMITED;
        if (ImGui::MenuItem("New blank tape")) host.tapeNewBlank();
        if (ImGui::MenuItem("Save tape as...", nullptr, false, in))
            saver.open("Save tape", host.mediaDir, host.tapeName.empty() ? "tape.cdt" : host.tapeName, [this](const std::string& p) { host.tapeSaveAs(p); });
        if (ImGui::MenuItem("Eject", nullptr, false, in)) host.tapeEject();
        return;
    }
    // the counter, the time, the block, the motor
    ImGui::BeginDisabled(!in);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(in ? kAccent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%03d", in ? deck->counter() : 0);
    if (after().smallButton("0")) host.tapeResetCounter();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Set the counter to 000");
    const bool motor = in && deck->motorOn;
    {
        FlowRow lamp = after();
        lamp.spacing = 14.0f;
        lamp.textColored(motor ? ImVec4(0.45f, 0.9f, 0.45f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), motor ? "MOTOR" : "motor");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The cassette motor, as the CPC's relay switches it");
    // where the tape is, of how long; the block numbered as the list below numbers them
    if (in) ImGui::TextDisabled("%s / %s   block [%d] of %d", clock(deck->positionSeconds()).c_str(), clock(deck->lengthSeconds()).c_str(),
                                std::max(0, block), blocks);
    else ImGui::TextDisabled("0:00 / 0:00");
    // the buttons, on as many rows as the window's width needs
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    // all eight across when they fit; never narrower than the longest label
    const float widest = ImGui::CalcTextSize("Pause").x + style.FramePadding.x * 2;
    const float w = std::max(widest, (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 7) / 8);
    bool first = true;
    auto button = [&](const char* label, const char* tip, bool lit, bool enabled) {
        // decided before the cursor moves (SameLine then NewLine would count the SameLine
        // position as content, past the edge)
        if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x + w <= right + 1) ImGui::SameLine();
        first = false;
        if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::BeginDisabled(!enabled);
        const bool pressed = ImGui::Button(label, ImVec2(w, 0));
        ImGui::EndDisabled();
        if (lit) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
        return pressed;
    };
    const bool playing = in && deck->playing;
    const bool recording = in && deck->recording;
    if (button("|<", "Rewind to the start", false, in && !recording)) host.tapeRewind();
    if (button("<<", "Rewind: to this block's start, or the one before", false, in && !recording)) host.tapeRewindBlock();
    if (button("Play", "PLAY", playing && !deck->paused && !recording, in && !deck->tapeEnded && !recording)) host.tapePlay();
    if (button("Rec", "RECORD (with PLAY): what the CPC SAVEs goes onto the tape from here on.\n"
                      "No tape in: a blank one. Stop ends it; with \"Save writes to the image\" on, a .cdt file takes it.",
               recording, !recording)) host.tapeRecord();
    if (button("Pause", "PAUSE: the tape held, PLAY still down", in && deck->paused, playing)) host.tapePause();
    if (button("Stop", "STOP: PLAY (and REC) up", false, playing)) host.tapeStop();
    if (button(">>", "Fast forward: to the next block", false, in && !deck->tapeEnded && !recording)) host.tapeFastForward();
    if (button("Eject", "Take the tape out", false, in)) host.tapeEject();
    ImGui::EndDisabled();
    if (ImGui::SmallButton("New blank tape")) host.tapeNewBlank();
    ImGui::BeginDisabled(!in);
    if (after().smallButton("Save tape as..."))
        saver.open("Save tape", host.mediaDir, host.tapeName.empty() ? "tape.cdt" : host.tapeName, [this](const std::string& p) { host.tapeSaveAs(p); });
    if (in && deck->modified) after().textDisabled("recorded, not saved");
    // tape turbo: the machine faster while the tape moves
    {
        const bool fast = host.tapeTurboActive();
        if (ImGui::Button(fast ? "Turbo: on##tt" : "Turbo##tt")) host.tapeTurbo = host.tapeTurbo > 1 ? 1 : EmuHost::TAPE_TURBO_UNLIMITED;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Load faster: while the tape moves the whole CPC runs faster,\n"
                              "so every loader (protected ones too) works as it always does.\n"
                              "Click: unlimited / off. The slider sets how much faster.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        const char* shown = host.tapeTurbo <= 1 ? "off (real speed)" : host.tapeTurbo >= EmuHost::TAPE_TURBO_UNLIMITED ? "unlimited" : "%dx";
        ImGui::SliderInt("##tapeturbo", &host.tapeTurbo, 1, EmuHost::TAPE_TURBO_UNLIMITED, shown, ImGuiSliderFlags_AlwaysClamp);
    }
    // any block
    if (in && blocks > 0) {
        char preview[160];
        std::snprintf(preview, sizeof preview, "%s", deck->getBlockDescription(std::max(0, block)).c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##tapeblock", preview, ImGuiComboFlags_HeightLarge)) {
            // only the rows in view: a tape can have a million blocks
            ImGuiListClipper clip;
            clip.Begin(blocks);
            if (block >= 0 && ImGui::IsWindowAppearing()) clip.IncludeItemByIndex(block);
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                    const long long at = i < (int)deck->blockStartCycles.size() ? deck->blockStartCycles[(size_t)i] : 0;
                    // fast loading's decision for the block, once it has played
                    const int8_t fast = i < (int)deck->blockFast.size() ? deck->blockFast[(size_t)i] : (int8_t)CPCTapeDrive::FAST_UNKNOWN;
                    const char* how = fast == CPCTapeDrive::FAST_ON ? "  [fast]" : fast == CPCTapeDrive::FAST_NORMAL ? "  [normal: own loader]"
                                    : fast == CPCTapeDrive::FAST_AS_FAST ? "  [already fast]" : fast == CPCTapeDrive::FAST_INSTANT ? "  [instant]" : "";
                    char label[240];
                    std::snprintf(label, sizeof label, "%s  %s%s", clock(at / deck->cyclesPerSecond()).c_str(), deck->getBlockDescription(i).c_str(), how);
                    ImGui::PushID(i);
                    if (ImGui::Selectable(label, i == block)) host.tapeSeekBlock(i);
                    if (i == block && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
                    ImGui::PopID();
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to any block");
    }
    ImGui::EndDisabled();
}

// ============================================================== lightgun aim
void GuiShell::lightgunFromScreen(float ox, float oy, float drawW, float drawH, int texW, int texH) {
    if (!screenHovered || drawW <= 0 || drawH <= 0) { host.lightgunAim(-1, -1, false); return; }
    const ImVec2 m = ImGui::GetIO().MousePos;
    const double x = (m.x - ox) / drawW * texW, y = (m.y - oy) / drawH * texH;
    if (x < 0 || y < 0 || x >= texW || y >= texH) { host.lightgunAim(-1, -1, false); return; }
    host.lightgunAim(x, y, ImGui::IsMouseDown(ImGuiMouseButton_Left));
}

// ============================================================== SF3 OLED window
// The SYMBiFACE III's 128 x 32 OLED as the CPC wrote it (functions 200-210, sf3.h): the two
// text rows (202 upper, 203 lower) and the texts placed at x, y in a 10, 18 or 26 px font.
void GuiShell::windowSf3Oled() {
    bool& open = panelOpen("SF3 OLED");
    if (!open) return;
    if (ImGui::Begin("SF3 OLED", &open)) {
        WrapText wrapText;                     // text wraps at the window's edge
        Symbiface3* sf3 = host.emu && host.emu->sf3 && host.emu->sf3->enabled ? host.emu->sf3 : nullptr;
        if (!sf3) ImGui::TextDisabled("No SYMBiFACE III is fitted (Expansions: Symbiface / SF3).");
        else {
            const float scale = std::max(1.0f, std::floor(ImGui::GetContentRegionAvail().x / 128.0f));
            const ImVec2 o = ImGui::GetCursorScreenPos();
            ImDrawList* d = ImGui::GetWindowDrawList();
            d->AddRectFilled(o, ImVec2(o.x + 128 * scale, o.y + 32 * scale), IM_COL32(0, 0, 0, 255));
            const ImU32 ink = IM_COL32(120, 200, 255, 255);
            ImFont* font = ImGui::GetFont();
            for (int row = 0; row < 2; row++)
                d->AddText(font, 14.0f * scale, ImVec2(o.x + 2 * scale, o.y + (row * 16 + 1) * scale), ink, sf3->oledRows[(size_t)row].c_str());
            for (const auto& t : sf3->oledTexts) {
                const float px = t.font >= 26 ? 26.0f : t.font >= 18 ? 18.0f : 10.0f;
                d->AddText(font, px * scale, ImVec2(o.x + t.x * scale, o.y + t.y * scale), ink, t.text.c_str());
            }
            ImGui::Dummy(ImVec2(128 * scale, 32 * scale));
            const Sf3Audio& a = *sf3->audio;
            static const char* modes[] = { "MP3A (SymAmp)", "MP3B (USB player)", "REG (samples)", "SYNTH (MIDI)", "FM" };
            ImGui::TextDisabled("Sound card: %s%s", modes[(int)a.mode], a.route == 6 ? ", EMR MIDI on" : "");
            if (a.playing()) ImGui::TextDisabled("Playing: %s", a.nowPlaying().c_str());
            if (!a.lastFault().empty()) ImGui::TextDisabled("Last error: %s", a.lastFault().c_str());
        }
    }
    ImGui::End();
}

// ============================================================== Printer window
void GuiShell::windowPrinter() {
    bool& open = panelOpen("Printer");
    if (!open) return;
    if (ImGui::Begin("Printer", &open)) {
        WrapText wrapText;                     // text wraps at the window's edge
        {
            FlowRow port;
            port.textDisabled("Printer port:");
            port.place(180.0f);                // the combo, at least 180 wide, else on the next line
            printerPortItems(false);
        }
        // The tab of the printer that is fitted comes to the front when it is fitted.
        const bool refit = printerTabDevice != host.dacType;
        printerTabDevice = host.dacType;
        if (ImGui::BeginTabBar("##printertabs")) {
            if (ImGui::BeginTabItem("Text", nullptr, refit && host.dacType == "printer" ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (ImGui::Button("Save as text...")) saver.open("Save printer text", host.romDir, "printer.txt",
                                                                [this](const std::string& p) { host.savePrinterText(p); });
                if (after().button("Clear")) host.clearPrinter();
                { char n[48]; std::snprintf(n, sizeof n, "%zu characters", host.printerText.size()); after().textDisabled(n); }
                ImGui::InputTextMultiline("##printertext", host.printerText.data(), host.printerText.size() + 1,
                                          ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Dot-matrix", nullptr, refit && host.dacType == "matrix" ? ImGuiTabItemFlags_SetSelected : 0)) {
                MatrixPrinter* mp = host.matrixPrinter;
                const int pages = mp ? mp->pageCount() : 0;
                if (ImGui::Button("Save page as BMP...")) saver.open("Save printer page", host.romDir, "page.bmp",
                                                                     [this](const std::string& p) { host.savePrinterPageBmp(p); });
                if (after().button("Save page as SVG...")) saver.open("Save printer page", host.romDir, "page.svg",
                                                                     [this](const std::string& p) { host.savePrinterPageSvg(p); });
                if (after().button("Clear##mp")) host.clearPrinter();
                { char n[48]; std::snprintf(n, sizeof n, "page %d of %d", mp ? mp->pageNumber() : 0, pages); after().textDisabled(n); }
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
        // Only the web: URLDownloadToFile would also take file:// (any file on this machine),
        // and to curl a "URL" starting with '-' is an option.
        const std::string urlText = romUrl;
        const bool webUrl = urlText.rfind("https://", 0) == 0 || urlText.rfind("http://", 0) == 0;
        const bool clicked = ImGui::Button("Download", ImVec2(-1, 0));
        if (clicked && !webUrl) status = "Not fitted: the URL must start with https:// or http://";
        if (clicked && webUrl) {
            status = "Downloading...";
            auto fetch = std::make_shared<RomFetch>();
            std::error_code ec;
            // A name nobody can plant beforehand in the shared temporary folder.
            char unique[64];
            std::snprintf(unique, sizeof unique, "cpcse_%llx_%llx_",
                          (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count(),
                          (unsigned long long)(uintptr_t)fetch.get());
            fetch->path = (std::filesystem::temp_directory_path(ec) / (unique + dest + ".part")).string();
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
        if (after().button("Not now")) { romPromptFor.clear(); status.clear(); ImGui::CloseCurrentPopup(); }
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

// The picture's shape on the monitor. A 15 kHz mode's lines fill a 4:3 tube as a TV picture
// does (PAL 288 lines across 52 us of each line, NTSC 240 across 52.66 us), so the shape
// is the picture's visible time against the lines it shows, from the V9990's own timing (a
// B1 PAL picture, 290 lines in 52.9 us, is 1.35:1; B0/B2/B4, 53.6 us, are a little wider).
// B5 and B6 (24 and 31 kHz) go to a monitor that fills its 4:3 screen with them.
float GuiShell::gfxPictureAspect(const V9990Picture& pic) {
    if (pic.lines <= 0 || pic.spanUs <= 0) return 4.0f / 3.0f;
    if (pic.spanUs < 40.0) return 4.0f / 3.0f;
    const bool pal = pic.lines > 260;
    const double tvLines = pal ? 288.0 : 240.0, tvUs = pal ? 52.0 : 52.66;
    return (float)(4.0 / 3.0 * (pic.spanUs / tvUs) * (tvLines / pic.lines));
}

void GuiShell::gfxPictureRect(float x, float y, float w, float h, float out[4]) {
    const ScreenLook& look = host.gfxLook;
    float dw = w, dh = h;
    const V9990Picture* pic = gfxPictureTexture();
    if (look.maintainAspect) {
        const float aspect = pic ? gfxPictureAspect(*pic) : 4.0f / 3.0f;
        dh = std::min(h, w / aspect);
        if (look.integerScale && pic && pic->height > 0) {
            const float n = std::floor(dh / (float)pic->height);
            if (n >= 1.0f) dh = n * (float)pic->height;
        }
        dw = dh * aspect;
    }
    out[0] = x + (w - dw) * 0.5f; out[1] = y + (h - dh) * 0.5f; out[2] = dw; out[3] = dh;
}

void GuiShell::drawGfxMonitor(ImDrawList* dl, float x, float y, float w, float h) {
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(0, 0, 0, 255));
    if (const V9990Picture* pic = gfxPictureTexture()) {
        float r[4];
        gfxPictureRect(x, y, w, h, r);
        LookPicture p;
        p.screen = 1;
        p.texture = gfxTexture;
        p.pixels = pic->pixels.data();
        p.width = pic->width; p.height = pic->height;
        p.rowStep = 1;
        p.lines = pic->lines;
        p.frame = (uint64_t)pic->fields;
        p.colourTube = true;                 // the GFX9000 drives an RGB colour monitor
        drawThroughLook(dl, host.gfxLook, gfxLookState, p, r[0], r[1], r[2], r[3]);
        char label[48];
        std::snprintf(label, sizeof label, "GFX9000  %s", v9990ModeName(pic->mode));
        dl->AddText(ImVec2(x + 6, y + h - ImGui::GetTextLineHeight() - 4), IM_COL32(255, 255, 255, 90), label);
    } else {
        const char* msg = "GFX9000: no signal";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
    }
}

// The GFX9000's picture alone. Full screen: the window over the whole display it was on, with
// no frame, its own platform window (never merged into the main one), on top.
void GuiShell::windowGfxScreen() {
    bool& open = panelOpen("GFX9000 screen");
    gfxScreenFocused = false;
    if (!open) { if (gfxScreenFull) toggleGfxScreenFull(); return; }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (gfxScreenFull) {
        const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        if (gfxScreenMonitor >= 0 && gfxScreenMonitor < pio.Monitors.Size) {
            const ImGuiPlatformMonitor& m = pio.Monitors[gfxScreenMonitor];
            ImGui::SetNextWindowPos(m.MainPos);
            ImGui::SetNextWindowSize(m.MainSize);
        }
        ImGuiWindowClass wc;
        wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_TopMost;
        ImGui::SetNextWindowClass(&wc);
        ImGui::SetNextWindowDockID(0);
        flags |= ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoSavedSettings;
    } else {
        // A window of its own (a system window, never merged into the main one), so it can
        // be dragged anywhere, onto another display; docking it is still allowed. 640 x 480
        // the first time, at the main window's top right; never smaller than 320 x 240.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + std::max(0.0f, vp->Size.x - 680), vp->Pos.y + 60), ImGuiCond_FirstUseEver);
        // back from full screen: where it was and its size (once), the frame and the "on top"
        // the full screen gave its system window taken off again
        if (gfxRestorePending) {
            if (gfxRestore[2] >= 320 && gfxRestore[3] >= 240) {
                ImGui::SetNextWindowPos(ImVec2(gfxRestore[0], gfxRestore[1]));
                ImGui::SetNextWindowSize(ImVec2(gfxRestore[2], gfxRestore[3]));
            } else {
                ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + std::max(0.0f, vp->Size.x - 680), vp->Pos.y + 60));
                ImGui::SetNextWindowSize(ImVec2(640, 480));
            }
            gfxRestorePending = false;
        }
        ImGui::SetNextWindowSizeConstraints(ImVec2(320, 240), ImVec2(FLT_MAX, FLT_MAX));
        // The window the picture's size (its context menu): as many times its rows high, as
        // wide as its shape makes that, the title bar above. It follows the mode.
        const V9990Picture* fitPic = gfxWindowScale > 0 && host.gfx9000Monitor != "video9000" ? gfxPictureTexture() : nullptr;
        if (fitPic && fitPic->height > 0) {
            const float ph = (float)(fitPic->height * gfxWindowScale);
            ImGui::SetNextWindowSize(ImVec2(std::round(ph * gfxPictureAspect(*fitPic)), ph + ImGui::GetFrameHeight()));
        }
        ImGuiWindowClass wc;
        wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        wc.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_TopMost;
        ImGui::SetNextWindowClass(&wc);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    const bool visible = ImGui::Begin("GFX9000 screen", &open, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (visible) {
        gfxScreenFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // full screen asked for: on the display the window is on now; its place and size
        // kept for the way back
        if (gfxScreenFull && gfxScreenMonitor < 0) {
            gfxScreenMonitor = std::max(0, (int)static_cast<ImGuiViewportP*>(ImGui::GetWindowViewport())->PlatformMonitor);
            const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
            gfxRestore[0] = p.x; gfxRestore[1] = p.y; gfxRestore[2] = sz.x; gfxRestore[3] = sz.y;
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail(), at = ImGui::GetCursorScreenPos();
        float w = avail.x, h = avail.y;
        if (host.gfxLook.maintainAspect) { h = std::min(avail.y, avail.x * 0.75f); w = h * 4.0f / 3.0f; }
        const float x = at.x + (avail.x - w) * 0.5f, y = at.y + (avail.y - h) * 0.5f;   // the Video9000's 4:3 set
        ImDrawList* dl = ImGui::GetWindowDrawList();
        int vw = 0, vh = 0;
        if (!host.v9990Enabled) {
            const char* msg = "No GFX9000 fitted (Settings > Expansions)";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2(at.x + (avail.x - ts.x) * 0.5f, at.y + (avail.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
        } else if (host.gfx9000Monitor == "video9000" && uploadTexture && host.video9000Picture(video9000Pixels, vw, vh)) {
            video9000Texture = uploadTexture(3, video9000Pixels.data(), vw, vh);
            dl->AddImage((ImTextureID)(intptr_t)video9000Texture, ImVec2(x, y), ImVec2(x + w, y + h));
        } else {
            drawGfxMonitor(dl, at.x, at.y, avail.x, avail.y);
        }
        ImGui::InvisibleButton("##gfxscreen", ImVec2(std::max(1.0f, avail.x), std::max(1.0f, avail.y)));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && host.emulatedMouseActive() && !host.mouseCaptured) mouseCaptureRequested = true;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !host.emulatedMouseActive()) toggleGfxScreenFull();
        if (ImGui::BeginPopupContextItem("##gfxscreenmenu")) {
            if (ImGui::MenuItem(gfxScreenFull ? "Leave full screen" : "Full screen on this display", "F11")) toggleGfxScreenFull();
            ImGui::Separator();
            // the window the picture's size: it follows a change of mode
            if (ImGui::MenuItem("Window: free size", nullptr, gfxWindowScale == 0)) gfxWindowScale = 0;
            for (int n = 1; n <= 4; n++) {
                char item[48];
                std::snprintf(item, sizeof item, "Window: the picture x%d", n);
                if (ImGui::MenuItem(item, nullptr, gfxWindowScale == n)) gfxWindowScale = n;
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    if (!open && gfxScreenFull) toggleGfxScreenFull();     // closed while full: it opens framed next time
}

void GuiShell::toggleGfxScreenFull() {
    gfxScreenFull = !gfxScreenFull;
    gfxScreenMonitor = -1;
    if (!gfxScreenFull) gfxRestorePending = true;
}

// Everything of the GFX9000's in one window: fitted or not, where its picture goes, the
// picture, and the V9990's state, registers, palette and VRAM.
void GuiShell::windowGfx9000() {
    if (!panelOpen("GFX9000")) return;
    if (beginTool("GFX9000")) {
        WrapText wrapText;                     // text wraps at the window's edge
        bool gfx = host.v9990Enabled;
        FlowRow head;
        if (head.checkbox("Fitted", &gfx)) host.setV9990(gfx);
        head.spacing = 20.0f;
        static const char* places[] = { "beside", "window", "switch", "video9000" };
        static const char* placeNames[] = { "beside the CPC's screen", "its own GFX9000 screen window", "one monitor, switched", "one monitor, via a Video9000" };
        int place = 0;
        for (int i = 0; i < 4; i++) if (host.gfx9000Monitor == places[i]) place = i;
        head.field(ImGui::GetFontSize() * 18, "picture");
        if (ImGui::Combo("picture", &place, placeNames, 4)) {
            host.gfx9000Monitor = places[place];
            if (host.gfx9000Monitor == "window") panelOpen("GFX9000 screen") = true;
        }
        if (head.button(panelOpen("GFX9000 screen") ? "Show the screen window" : "Open the screen in its own window")) {
            panelOpen("GFX9000 screen") = true;
            focusWindowNextFrame = "GFX9000 screen";
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The GFX9000's picture alone, in a window you can drag anywhere, onto another display,\n"
                              "and make full screen there (F11 or a double-click).");
        if (!host.v9990Enabled) ImGui::TextDisabled("Not fitted.");
        else if (ImGui::BeginTabBar("##gfxtabs")) {
          if (ImGui::BeginTabItem("Picture")) {
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float h = std::max(1.0f, std::min(avail.y, avail.x * 0.75f)), w = h * 4.0f / 3.0f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            drawGfxMonitor(ImGui::GetWindowDrawList(), at.x + (avail.x - w) * 0.5f, at.y, w, h);
            ImGui::InvisibleButton("##gfxarea", ImVec2(std::max(1.0f, avail.x), h));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && host.emulatedMouseActive() && !host.mouseCaptured)
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
// One argument for a Windows command line, as CommandLineToArgvW / the C runtime split it:
// backslashes are literal except before a quote, so those before a quote -- and before
// the closing one -- are doubled ("C:\shots\" would otherwise swallow the next argument).
static std::string quoteArg(const std::string& s) {
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : s) {
        if (c == '\\') { slashes++; continue; }
        if (c == '"') { out.append(slashes * 2 + 1, '\\'); out += '"'; }
        else { out.append(slashes, '\\'); out += c; }
        slashes = 0;
    }
    out.append(slashes * 2, '\\');
    return out + "\"";
}
#endif

void GuiShell::windowCslScripts() {
    bool& open = panelOpen("CSL scripts");
    if (!open) return;
    if (ImGui::Begin("CSL scripts", &open)) {
        WrapText wrapText;                     // text wraps at the window's edge
        const bool busy = cslRun && cslRun->running;
        ImGui::BeginDisabled(busy);
        // The fields line up at x 110 when the window has room, else go under their label.
        const bool besideLabel = ImGui::GetContentRegionAvail().x >= 110.0f + 220.0f;
        auto pathRow = [&](const char* label, std::string& value, const char* button, auto pick) {
            ImGui::TextUnformatted(label);
            if (besideLabel) ImGui::SameLine(110);
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
        if (besideLabel) ImGui::SameLine(110);
        ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x));
        const char* crtcs[] = { "As the script says", "0", "1", "2", "3", "4" };
        int sel = cslCrtc + 1;
        if (ImGui::Combo("##cslcrtc", &sel, crtcs, 6)) cslCrtc = sel - 1;
        checkboxWrapped("Follow csl_load chains", &cslChain);
        checkboxWrapped("Apply published-script errata", &cslErrata);
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
        ImGui::BeginDisabled(!busy);
        if (after().button("Stop")) {
#ifdef _WIN32
            if (cslRun && cslRun->process) TerminateProcess(cslRun->process, 1);
#else
            if (cslRun && cslRun->pid > 0) kill(cslRun->pid, SIGTERM);
#endif
        }
        ImGui::EndDisabled();
        if (after().button("Open screenshot folder")) {
#ifdef _WIN32
            std::error_code ec;
            const std::string abs = std::filesystem::absolute(cslOut, ec).string();
            ShellExecuteA(nullptr, "open", abs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
            std::error_code ec;
            std::filesystem::create_directories(cslOut, ec);
            spawnDetached({ desktopOpener(), std::filesystem::absolute(cslOut, ec).string() });
#endif
        }
        if (cslRun) {
            if (cslRun->running) after().textColored(kAccent, "running...");
            else {
                char s[48];
                if (cslRun->exitCode == 0) std::snprintf(s, sizeof s, "finished");
                else std::snprintf(s, sizeof s, "stopped (exit %d)", (int)cslRun->exitCode);
                after().textDisabled(s);
            }
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

// External debugging (devserver.h): the GDB server for DeZog, the command API for scripts
// and cpcse-ctl, and a build's output reloaded when it changes.
void GuiShell::sectionDevelopment(bool asMenu) {
    DevServer& d = *dev;
    if (asMenu) {
        bool gdbOn = d.gdbListening(), apiOn = d.apiListening();
        char label[64];
        std::snprintf(label, sizeof(label), "GDB server (port %d)", d.gdbPortSetting);
        if (ImGui::MenuItem(label, nullptr, &gdbOn)) { d.gdbAtStart = gdbOn; if (gdbOn) d.startGdb(d.gdbPortSetting); else d.stopGdb(); }
        std::snprintf(label, sizeof(label), "Command API (port %d)", d.apiPortSetting);
        if (ImGui::MenuItem(label, nullptr, &apiOn)) { d.apiAtStart = apiOn; if (apiOn) d.startApi(d.apiPortSetting); else d.stopApi(); }
        if (d.watched() && ImGui::MenuItem("Stop watching the file")) d.watch(LoadRequest{});
        if (ImGui::MenuItem("Settings...")) { panelOpen("External debugging") = true; focusWindowNextFrame = "External debugging"; }
        return;
    }
    auto server = [&](const char* id, bool listening, bool& atStart, int& port, bool (DevServer::*start)(int), void (DevServer::*stop)(),
                      const std::string& error) {
        ImGui::PushID(id);
        bool on = listening;
        if (ImGui::Checkbox("Listen", &on)) {
            atStart = on;
            if (on) (d.*start)(port); else (d.*stop)();
        }
        after().item(ImGui::GetFontSize() * 6 + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Port").x);
        fitNextItemWidth(ImGui::GetFontSize() * 6, "Port");
        ImGui::InputInt("Port", &port, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit()) {   // applied when the field is left
            port = std::clamp(port, 1, 65535);
            if (listening) (d.*start)(port);
        }
        if (!error.empty() && !listening) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", error.c_str());
        ImGui::PopID();
    };

    sectionHeading("GDB server (DeZog in VS Code, gdb)");
    server("gdb", d.gdbListening(), d.gdbAtStart, d.gdbPortSetting, &DevServer::startGdb, &DevServer::stopGdb, d.gdbError);
    if (d.gdbListening())
        ImGui::TextDisabled("127.0.0.1:%d, %s", d.gdbPort(), d.gdbConnected() ? "a debugger is attached" : "waiting for a debugger");
    else ImGui::TextDisabled("DeZog: remoteType \"mame\", port %d", d.gdbPortSetting);

    sectionHeading("Command API (cpcse-ctl, scripts)");
    server("api", d.apiListening(), d.apiAtStart, d.apiPortSetting, &DevServer::startApi, &DevServer::stopApi, d.apiError);
    if (d.apiListening()) ImGui::TextDisabled("127.0.0.1:%d, %d client%s", d.apiPort(), d.apiClients(), d.apiClients() == 1 ? "" : "s");
    else ImGui::TextDisabled("JSON lines; cpcse-ctl help lists the commands");

    sectionHeading("Reload a file when it changes");
    const LoadRequest* w = d.watched();
    if (w) {
        ImGui::TextWrapped("Watching %s", w->path.c_str());
        ImGui::TextDisabled("Reloaded %d time%s", d.reloads(), d.reloads() == 1 ? "" : "s");
        if (ImGui::SmallButton("Stop watching")) d.watch(LoadRequest{});
    } else {
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
        ImGui::InputTextWithHint("##path", ".bin, .sna, .dsk, .cpr or a tape", devWatchPath, sizeof(devWatchPath));
        if (after().smallButton("Browse..."))
            browser.open("File to watch", host.mediaDir, { ".bin", ".sna", ".dsk", ".hfe", ".cpr", ".cro", ".cdt", ".tzx", ".wav" },
                         [this](const std::string& p) { std::snprintf(devWatchPath, sizeof(devWatchPath), "%s", p.c_str()); });
        fitNextItemWidth(ImGui::GetFontSize() * 7, "Load at");
        ImGui::InputTextWithHint("Load at", "header", devWatchAddr, sizeof(devWatchAddr));
        after().item(ImGui::GetFontSize() * 7 + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Start at").x);
        fitNextItemWidth(ImGui::GetFontSize() * 7, "Start at");
        ImGui::InputTextWithHint("Start at", "entry / -", devWatchRun, sizeof(devWatchRun));
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
        ImGui::InputTextWithHint("Then type", "RUN\"GAME\\n (disc, tape)", devWatchCommand, sizeof(devWatchCommand));
        ImGui::Checkbox("Reset first", &devWatchReset);
        if (after().button("Watch") && devWatchPath[0]) {
            LoadRequest r;
            r.path = devWatchPath;
            r.reset = devWatchReset;
            r.address = devWatchAddr[0] ? debugger.parseAddress(devWatchAddr) : -1;
            const std::string run = devWatchRun;
            if (run.empty() || run == "entry") r.run = LoadRequest::RUN_ENTRY;   // the hint: entry
            else if (run != "-") r.run = debugger.parseAddress(run);
            std::string cmd;
            for (const char* c = devWatchCommand; *c; c++) {
                if (c[0] == '\\' && c[1] == 'n') { cmd += '\n'; c++; } else cmd += *c;
            }
            r.command = cmd;
            if (devWatchAddr[0] && r.address < 0) d.lastEvent = std::string("Load at: '") + devWatchAddr + "' is not an address or a label";
            else if (run != "-" && r.run == -1) d.lastEvent = "Start at: '" + run + "' is not an address or a label (entry, or - for none)";
            else d.watch(r);
        }
    }
    if (!d.lastEvent.empty()) ImGui::TextDisabled("%s", d.lastEvent.c_str());
}

} // namespace cpcse
