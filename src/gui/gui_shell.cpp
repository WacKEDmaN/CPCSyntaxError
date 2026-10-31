// CPCSyntaxError GUI — the desktop shell: menus, layout, the settings windows and the
// screen. The debugger windows are in gui_debug_views.cpp. See gui_shell.h.
#include "gui_shell.h"

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder, BeginViewportSideBar

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "gui_assembler.h"
#include "gui_basic.h"
#include "gui_dsk_editor.h"
#include "gui_cheats.h"
#include "gui_logic.h"
#include "devserver.h"
#include "version.h"

// DejaVu Sans Mono (Bitstream Vera licence: third_party/dejavu/LICENSE.txt)
#include "font_dejavu_mono.inc"
// The logo, run-length coded (tools/make_logo.py, tools/embed_image.py)
#include "logo.inc"
#include "core/emulator.h"
#include "core/memory.h"
#include "core/video.h"
#include "core/z80.h"
#include "core/monitor_model.h"

namespace cpcse {

// ACCC §11.6 splits the UM6845R: a "CRTC 1-B" takes RFD#10 (R5=#10 disables the parity
// in the C9=R9 test), a "1-A" does not. Same part, 3 of 7 machines tested.
static const char* CRTC_NAMES[] = { "0  HD6845S / UM6845", "1  UM6845R (1-A)", "2  MC6845",
                                    "3  ASIC (Plus)", "4  Pre-ASIC", "5  UM6845R (1-B, RFD#10)" };
// Past 576K (64K + a 512K board) each step is one more 512K segment of a 4 MB-style board.
static const int RAM_SIZES[] = { 64, 128, 256, 320, 512, 576, 1088, 1600, 2112, 2624, 3136, 3648, 4160 };
// Bump when the set of windows or the default layout changes.
static const int LAYOUT_VERSION = 5;

GuiShell::GuiShell(EmuHost& h) : host(h), debugger(h) {
    // Minimal by default: the screen, the machine and its media. The rest opens from the
    // menus and docks with its group (placeNewlyOpened).
    panels = {
        { "Screen", "win_screen", true, true },
        { "Machine", "win_machine", true, true },
        { "ROMs", "win_roms", true, true },
        { "Discs", "win_discs", true, true },
        { "Tape", "win_tape", true, true },
        { "Cartridge", "win_cartridge", true, true },
        { "Snapshot", "win_snapshot", true, true },
        { "Debugger", "win_debugger", false, false },
        { "Chips", "win_chips", false, false },
        { "Memory", "win_memory", false, false },
        { "Logic analyser", "win_logic", false, false },
        { "Profiler", "win_profiler", false, false },
        { "Graphics finder", "win_gfx_finder", false, false },
        { "CRO editor", "win_cro", false, false },
        { "CPC keyboard", "win_cpc_keyboard", false, false },
        { "GFX9000", "win_gfx9000", false, false },
        { "GFX9000 screen", "win_gfx9000screen", false, false },
        { "Assembler", "win_asm", false, false },
        { "BASIC", "win_basic", false, false },
        { "DSK editor", "win_dsk_editor", false, false },
        { "Cheats", "win_cheats", false, false },
        { "CSL scripts", "win_csl", false, false },
        { "Printer", "win_printer", false, false },
        { "SF3 OLED", "win_sf3oled", false, false },
        // The settings, one window per section: they open docked together as tabs, and a
        // tab can be dragged out to float or dock anywhere.
        { "Video settings", "win_set_video", false, false },
        { "Audio settings", "win_set_audio", false, false },
        { "Input settings", "win_set_input", false, false },
        { "Expansions", "win_set_expansions", false, false },
        { "External debugging", "win_set_dev", false, false },
    };
    assembler = std::make_unique<AssemblerWindow>(host, debugger, browser, saver);
    assembler->showInDisassembly = [this](int a) { showInDisassembly(a); };
    basicWindow = std::make_unique<BasicWindow>(host, browser, saver);
    dskEditor = std::make_unique<DskEditorWindow>(host, browser, saver);
    cheats = std::make_unique<CheatsWindow>(host, browser, saver);
    logicAnalyser = std::make_unique<LogicAnalyserWindow>(host, debugger);
    dev = std::make_unique<DevServer>(host, debugger);
    dev->onQuit = [this]() { quitRequested = true; };
}

GuiShell::~GuiShell() = default;

bool& GuiShell::panelOpen(const char* title) {
    for (auto& p : panels) if (std::string(p.title) == title) return p.open;
    static bool dummy = false;
    return dummy;
}

void GuiShell::loadSettings(const std::map<std::string, std::string>& ini) {
    for (auto& p : panels) {
        auto it = ini.find(p.iniKey);
        if (it != ini.end()) p.open = std::atoi(it->second.c_str()) != 0;
    }
    auto sb = ini.find("statusbar");
    if (sb != ini.end()) showStatusBar = std::atoi(sb->second.c_str()) != 0;
    // A saved dock layout from an older set of windows would leave the new ones floating.
    auto lv = ini.find("layout_version");
    if (lv == ini.end() || std::atoi(lv->second.c_str()) < LAYOUT_VERSION) {
        resetLayout = true;
        for (auto& p : panels) p.open = p.byDefault;
    }
    if (auto tn = ini.find("timing_nops"); tn != ini.end()) timingNops = std::atoi(tn->second.c_str()) != 0;
    if (auto gw = ini.find("gfx_window_scale"); gw != ini.end()) gfxWindowScale = std::clamp(std::atoi(gw->second.c_str()), 0, 4);
    auto fp = ini.find("fullscreenpicture");
    if (fp != ini.end()) fullscreenPictureOnly = std::atoi(fp->second.c_str()) != 0;
    auto us = ini.find("ui_scale");
    if (us != ini.end()) uiScale = std::clamp((float)std::atof(us->second.c_str()), 0.75f, 2.5f);
    auto str = [&](const char* k, std::string& v) { auto i = ini.find(k); if (i != ini.end()) v = i->second; };
    str("csl_script", cslScript); str("csl_out", cslOut); str("csl_diskdir", cslDiskDir);
    auto ci = ini.find("csl_crtc"); if (ci != ini.end()) cslCrtc = std::atoi(ci->second.c_str());
    assembler->loadSettings(ini);
    basicWindow->loadSettings(ini);
    dev->loadSettings(ini);
    loadRememberedFolders(ini);
}

void GuiShell::saveSettings(std::ostream& out) const {
    out << "layout_version=" << LAYOUT_VERSION << "\n";
    for (const auto& p : panels) out << p.iniKey << "=" << (p.open ? 1 : 0) << "\n";
    out << "statusbar=" << (showStatusBar ? 1 : 0) << "\n";
    out << "fullscreenpicture=" << (fullscreenPictureOnly ? 1 : 0) << "\n";
    out << "timing_nops=" << (timingNops ? 1 : 0) << "\n";
    out << "gfx_window_scale=" << gfxWindowScale << "\n";
    out << "ui_scale=" << uiScale << "\n";
    out << "csl_script=" << cslScript << "\n" << "csl_out=" << cslOut << "\n"
        << "csl_diskdir=" << cslDiskDir << "\n" << "csl_crtc=" << cslCrtc << "\n";
    assembler->saveSettings(out);
    basicWindow->saveSettings(out);
    dev->saveSettings(out);
    saveRememberedFolders(out);
}

// The front end's look: Dear ImGui's dark theme, square windows, the orange accent --
// with a smooth monospace font and more room between things than ImGui's defaults.
void GuiShell::applyStyle() {
    ImGuiIO& io = ImGui::GetIO();
    static ImFont* mono = nullptr;
    if (!mono || io.Fonts->Fonts.empty() || io.Fonts->Fonts[0] != mono) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;    // the embedded array stays ours
        mono = io.Fonts->AddFontFromMemoryTTF((void*)kDejaVuSansMono, (int)kDejaVuSansMonoSize, 15.0f, &cfg);
    }
    io.FontDefault = mono;
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.Colors[ImGuiCol_WindowBg].w = 1.0f;   // floating OS windows must be opaque
    s.Colors[ImGuiCol_TabSelectedOverline] = kAccent;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(7, 4);
    s.ItemSpacing = ImVec2(9, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(7, 3);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 14;
    s.GrabMinSize = 12;
    s.TabBarBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(10, 4);
    s.ScaleAllSizes(uiScale);
    s.FontSizeBase = 15.0f * uiScale;
}

// ============================================================== frame
bool GuiShell::beginTool(const char* title, ImGuiWindowFlags flags) {
    bool& open = panelOpen(title);
    bool visible = ImGui::Begin(title, &open, flags);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) toolFocusedNow = true;
    return visible;
}

void GuiShell::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !host.booted()) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false)) host.reset();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) { if (host.paused) debugger.run(); else debugger.pause(); }
    // the quick save slots: Ctrl+1..9 loads, Ctrl+Shift+1..9 saves
    if (io.KeyCtrl && !io.KeyAlt)
        for (int s = 1; s <= EmuHost::SLOT_COUNT; s++)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + s - 1), false)) { if (io.KeyShift) host.saveSlot(s); else host.loadSlot(s); }
    // Ctrl+Left: a second back (held: on going back); the machine runs on from there
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && host.timelineEnabled) {
        host.timelineRewind(1.0);
        host.status = host.timelineMessage;
    }
    // The debugger's keys belong to the debugger while one of its windows has the focus,
    // or while the machine is stopped; otherwise they are the CPC's function keys.
    if (!toolFocused && !host.paused) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !io.KeyShift) { if (host.paused) debugger.run(); else debugger.pause(); }
    if (ImGui::IsKeyPressed(ImGuiKey_F7) && io.KeyShift && host.timelineEnabled) {
        if (!host.paused) debugger.pause();
        host.timelineStepBack();
        host.breakReason = host.timelineMessage;
    } else if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && io.KeyShift && host.timelineEnabled) {
        if (!host.paused) debugger.pause();
        host.timelineRunBack();
        host.breakReason = host.timelineMessage;
        return;
    } else if (ImGui::IsKeyPressed(ImGuiKey_F7)) debugger.stepInto();
    if (ImGui::IsKeyPressed(ImGuiKey_F8)) { if (io.KeyShift) debugger.stepOut(); else debugger.stepOver(); }
}

void GuiShell::draw(const ShellFrameInfo& info) {
    debugger.attach();
    handleShortcuts();
    toolFocusedNow = false;

    // full screen, the picture alone: nothing else drawn (the windows keep their places)
    const bool pictureOnly = host.fullscreen && fullscreenPictureOnly;
    if (pictureOnly && !fullscreenWas) fullscreenHintUntil = ImGui::GetTime() + 3.0;
    fullscreenWas = pictureOnly;
    if (pictureOnly) {
        drawFullscreenPicture(info);
        windowGfxScreen();
        toolFocused = false;
        return;
    }

    drawMenuBar();
    if (showStatusBar) drawStatusBar(info);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dockId = ImGui::GetID("CPCSyntaxErrorDock");
    if (resetLayout || ImGui::DockBuilderGetNode(dockId) == nullptr) {
        buildDefaultLayout(dockId);
        resetLayout = false;
    }
    placeNewlyOpened(dockId);
    ImGui::DockSpaceOverViewport(dockId, vp);
    if (focusWindowNextFrame && ImGui::FindWindowByName(focusWindowNextFrame)) {
        ImGui::SetWindowFocus(focusWindowNextFrame);
        focusWindowNextFrame = nullptr;
    }

    windowScreen(info);
    windowMachine();
    windowMedia();
    windowSettings();
    windowDebugger();
    windowChips();
    // The memory map records only while it is shown (and Record is on).
    if (host.emu && host.emu->memory) host.emu->memory->trackAccess = panelOpen("Memory") && memMapShown && memMapRecord;
    windowMemory();
    if (panelOpen("Assembler")) {
        assembler->draw(&panelOpen("Assembler"));
        if (assembler->focused) toolFocusedNow = true;
    }
    if (panelOpen("BASIC")) {
        basicWindow->draw(&panelOpen("BASIC"));
        if (basicWindow->focused) toolFocusedNow = true;   // keys go to the editor, not the CPC
    }
    windowPrinter();
    windowSf3Oled();
    romPrompt();
    windowGfx9000();
    windowGfxScreen();
    windowProfiler();
    windowGraphicsFinder();
    windowCroEditor();
    croChooser();
    windowCpcKeyboard();
    windowCslScripts();
    if (panelOpen("DSK editor")) {
        dskEditor->draw(&panelOpen("DSK editor"));
        if (dskEditor->focused) toolFocusedNow = true;
    }
    if (panelOpen("Cheats")) {
        cheats->draw(&panelOpen("Cheats"));
        if (cheats->focused) toolFocusedNow = true;
    }
    if (panelOpen("Logic analyser")) {
        logicAnalyser->draw(&panelOpen("Logic analyser"));
        if (logicAnalyser->focused) toolFocusedNow = true;
    }
    // the analyser samples only while its window is open
    else if (host.emu && host.emu->analyser.enabled) host.emu->analyser.enabled = false;
    windowAbout();
    if (showImGuiDemo) ImGui::ShowDemoWindow(&showImGuiDemo);

    browser.draw();
    saver.draw();
    toolFocused = toolFocusedNow;
}

// The screen with the machine and its media to its left; nothing else until it is asked for.
void GuiShell::buildDefaultLayout(unsigned id) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
    ImGuiID center = id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.45f, nullptr, &left);
    ImGui::DockBuilderDockWindow("Screen", center);
    ImGui::DockBuilderDockWindow("ROMs", left);      // a tab beside Machine (the last docked is in front)
    ImGui::DockBuilderDockWindow("Machine", left);
    for (const char* w : MEDIA_WINDOWS) ImGui::DockBuilderDockWindow(w, leftBottom);   // tabs
    ImGui::DockBuilderFinish(id);
    focusWindowNextFrame = MEDIA_WINDOWS[0];   // Discs the tab in front
    // what was placed before goes back to its group when next opened
    placedThisSession.clear();
    groupNode[0] = groupNode[1] = 0;
    for (const auto& p : panels)
        if (!p.byDefault) ImGui::ClearWindowSettings(p.title);
}

void GuiShell::placeNewlyOpened(unsigned dockspaceId) {
    struct Group { std::vector<const char*> titles; ImGuiDir dir; float ratio; };
    static const Group groups[] = {
        { { "Debugger", "Chips", "Memory", "GFX9000", "Logic analyser", "Profiler" }, ImGuiDir_Right, 0.42f },
        { { "Assembler", "BASIC", "DSK editor", "CSL scripts", "Printer", "Cheats", "Graphics finder", "CRO editor", "CPC keyboard",
            "Video settings", "Audio settings", "Input settings", "Expansions", "External debugging" }, ImGuiDir_Down, 0.42f },
        // the media windows: tabs together, below the Machine window (a layout saved before
        // they were windows of their own has only "Media")
        { { "Discs", "Tape", "Cartridge", "Snapshot" }, ImGuiDir_Down, 0.45f },
        // the ROMs: a tab beside the Machine window (it was a section of it)
        { { "ROMs" }, ImGuiDir_None, 0.0f },
    };
    auto placed = [&](const char* t) { return std::find(placedThisSession.begin(), placedThisSession.end(), t) != placedThisSession.end(); };
    bool changed = false;
    for (int gi = 0; gi < 4; gi++) {
        const Group& g = groups[gi];
        for (const char* t : g.titles) {
            if (!panelOpen(t)) { placedThisSession.erase(std::remove(placedThisSession.begin(), placedThisSession.end(), t), placedThisSession.end()); continue; }
            if (placed(t)) continue;
            placedThisSession.push_back(t);
            // a window the saved layout places already (docked, or floated by hand) stays put
            if (ImGui::FindWindowSettingsByID(ImHashStr(t)) || ImGui::FindWindowByName(t)) continue;
            ImGuiID target = groupNode[gi] && ImGui::DockBuilderGetNode(groupNode[gi]) ? groupNode[gi] : 0;
            for (const char* other : g.titles) {
                if (target) break;
                ImGuiWindow* w = ImGui::FindWindowByName(other);
                if (other != t && w && w->DockNode && panelOpen(other)) { target = w->DockNode->ID; break; }
            }
            if (!target && gi == 3) {
                if (ImGuiWindow* machine = ImGui::FindWindowByName("Machine"); machine && machine->DockNode) target = machine->DockNode->ID;
                else continue;                 // the Machine window is not docked: the ROMs window floats
            }
            if (!target && gi == 2)
                if (ImGuiWindow* machine = ImGui::FindWindowByName("Machine"); machine && machine->DockNode) {
                    ImGuiID rest = 0;
                    target = ImGui::DockBuilderSplitNode(machine->DockNode->ID, g.dir, g.ratio, nullptr, &rest);
                }
            if (!target) {
                ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId);
                if (!central) continue;
                ImGuiID rest = 0;
                target = ImGui::DockBuilderSplitNode(central->ID, g.dir, g.ratio, nullptr, &rest);
            }
            groupNode[gi] = target;
            ImGui::DockBuilderDockWindow(t, target);
            changed = true;
        }
    }
    if (changed) ImGui::DockBuilderFinish(dockspaceId);
}

// ============================================================== screen
void GuiShell::windowScreen(const ShellFrameInfo& info) {
    bool& open = panelOpen("Screen");
    screenHovered = false;
    if (!open) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.067f, 0.090f, 1.0f));
    bool visible = ImGui::Begin("Screen", &open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (visible) screenPicture(info);
    ImGui::End();
}

void GuiShell::screenPicture(const ShellFrameInfo& info) {
    if (info.screenTexture && info.textureWidth > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        // With a GFX9000 fitted its own monitor stands beside the CPC's: the two share the
        // window, the same height, the GFX9000's as wide as its picture's shape makes it.
        const ScreenLook& look = host.cpcLook;
        const bool video9000 = host.v9990Enabled && host.gfx9000Monitor == "video9000";
        const bool gfxOnMain = host.gfx9000OnMainScreen() || video9000;
        const bool beside = host.v9990Enabled && host.gfx9000Monitor == "beside";
        const float gap = beside ? 6.0f : 0.0f;
        const V9990Picture* gfxPic = beside ? gfxPictureTexture() : nullptr;
        const float gfxAspect = gfxPic ? gfxPictureAspect(*gfxPic) : 4.0f / 3.0f;
        float cpcAvailW = beside ? (avail.x - gap) * 0.5f : avail.x;
        float drawW = cpcAvailW, drawH = avail.y;
        if (look.maintainAspect) {
            const float cpcAspect = (float)info.textureWidth / (float)info.textureHeight;
            float h = beside ? std::min(avail.y, (avail.x - gap) / (cpcAspect + gfxAspect)) : avail.y;
            float scale = std::min((beside ? h * cpcAspect : avail.x) / (float)info.textureWidth, h / (float)info.textureHeight);
            if (look.integerScale && scale > 1.0f) scale = std::floor(scale);
            drawW = info.textureWidth * scale; drawH = info.textureHeight * scale;
        }
        const float gfxW = beside ? (look.maintainAspect ? drawH * gfxAspect : cpcAvailW) : 0.0f;
        const float totalW = drawW + gap + gfxW;
        float ox = origin.x + (avail.x - totalW) * 0.5f, oy = origin.y + (avail.y - drawH) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // the picture's rectangle, for the recorder's "screen" source
        if (gfxOnMain) {
            gfxPictureRect(origin.x, origin.y, avail.x, avail.y, screenRect);
        } else {
            screenRect[0] = ox; screenRect[1] = oy; screenRect[2] = totalW; screenRect[3] = drawH;
        }
        screenRectMain = ImGui::GetWindowViewport() == ImGui::GetMainViewport();
        if (gfxOnMain) {
            // One monitor, switched to the GFX9000: its picture, as the GFX9000 screen's settings say.
            int vw = 0, vh = 0;
            if (video9000 && uploadTexture && host.video9000Picture(video9000Pixels, vw, vh)) {
                // one monitor through the Video9000: the composed picture, a 4:3 set
                float gw = avail.x, gh = avail.y;
                if (host.gfxLook.maintainAspect) { gh = std::min(avail.y, avail.x * 0.75f); gw = gh * 4.0f / 3.0f; }
                const float gx = origin.x + (avail.x - gw) * 0.5f, gy = origin.y + (avail.y - gh) * 0.5f;
                video9000Texture = uploadTexture(3, video9000Pixels.data(), vw, vh);
                dl->AddImage((ImTextureID)(intptr_t)video9000Texture, ImVec2(gx, gy), ImVec2(gx + gw, gy + gh));
            } else {
                drawGfxMonitor(dl, origin.x, origin.y, avail.x, avail.y);
            }
        } else {
            // the CPC's picture, through the CPC screen's CRT filter or CRT effect
            LookPicture pic;
            pic.screen = 0;
            pic.texture = info.screenTexture;
            pic.pixels = host.video && !host.video->pixels.empty() ? host.video->pixels.data() : nullptr;
            pic.width = info.textureWidth; pic.height = info.textureHeight;
            pic.rowStep = 2;                             // each CPC line is two rows
            pic.lines = info.textureHeight / 2;
            pic.frame = host.framesRun;
            pic.colourTube = host.video && host.video->monitorColourTube;
            drawThroughLook(dl, look, cpcLookState, pic, ox, oy, drawW, drawH);
        }
        if (beside) drawGfxMonitor(dl, ox + drawW + gap, oy, gfxW, drawH);
        // the raster timing overlay's legend, in the picture's top right corner
        if (host.rasterOverlay && !gfxOnMain) {
            static const struct { const char* name; ImU32 colour; } KEY[] = {
                { "CRTC select", IM_COL32(255, 255, 0, 255) }, { "CRTC data", IM_COL32(255, 154, 0, 255) },
                { "GA pen", IM_COL32(255, 255, 255, 255) }, { "GA ink", IM_COL32(255, 64, 255, 255) },
                { "GA mode/ROM", IM_COL32(0, 224, 255, 255) }, { "GA RAM", IM_COL32(128, 128, 255, 255) },
                { "PPI/AY", IM_COL32(64, 255, 64, 255) }, { "Plus palette", IM_COL32(255, 96, 128, 255) },
            };
            const float lh = ImGui::GetTextLineHeight();
            float w = 0;
            for (const auto& k : KEY) w = std::max(w, ImGui::CalcTextSize(k.name).x);
            const ImVec2 at(ox + drawW - w - lh - 14, oy + 6);
            dl->AddRectFilled(at, ImVec2(ox + drawW - 4, at.y + 8 * lh + 8), IM_COL32(0, 0, 0, 170));
            for (int i = 0; i < 8; i++) {
                const float y = at.y + 4 + i * lh;
                dl->AddRectFilled(ImVec2(at.x + 4, y + 3), ImVec2(at.x + 4 + lh * 0.6f, y + lh - 3), KEY[i].colour);
                dl->AddText(ImVec2(at.x + 8 + lh * 0.6f, y), ImGui::GetColorU32(ImGuiCol_Text), KEY[i].name);
            }
        }
        if (host.paused && host.booted()) {
            const char* label = host.breakReason.empty() ? "PAUSED" : host.breakReason.c_str();
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddRectFilled(ImVec2(ox + 6, oy + 6), ImVec2(ox + 14 + ts.x, oy + 10 + ts.y), IM_COL32(0, 0, 0, 170));
            dl->AddText(ImVec2(ox + 10, oy + 8), ImGui::GetColorU32(kAccent), label);
        }
        ImGui::InvisibleButton("##screenarea", ImVec2(std::max(1.0f, avail.x), std::max(1.0f, avail.y)));
        screenHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && host.emulatedMouseActive() && !host.lightgunActive() && !host.mouseCaptured)
            mouseCaptureRequested = true;
        if (host.lightgunActive() && !gfxOnMain) lightgunFromScreen(ox, oy, drawW, drawH, info.textureWidth, info.textureHeight);
        if (screenHovered && ImGui::IsMouseDoubleClicked(0) && !host.emulatedMouseActive() && !host.lightgunActive()
            && toggleFullscreen) toggleFullscreen();
    }
}

// One screen's picture in a box: through the advanced CRT filter (drawn by the GPU at the size
// it is shown, one texel a screen pixel, so on whole pixels), or as it is with the CRT effect's
// blur, phosphor mask, bloom and scanlines over it.
void GuiShell::drawThroughLook(ImDrawList* dl, const ScreenLook& look, LookState& st, const LookPicture& pic,
                               float ox, float oy, float drawW, float drawH) {
    ImTextureID tex = (ImTextureID)(intptr_t)pic.texture;
    if (look.crtFilterActive() && crtFilterRender && pic.texture) {
        const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
        const int outW = (int)std::lround(drawW * fbScale.x), outH = (int)std::lround(drawH * fbScale.y);
        const bool newFrame = st.filterFrame != pic.frame;
        if (const unsigned t = crtFilterRender(pic.screen, pic.texture, pic.width, pic.height, pic.rowStep, outW, outH, fbScale.y,
                                               newFrame, look.crtFilter)) {
            st.filterFrame = pic.frame;
            ox = std::round(ox); oy = std::round(oy);
            dl->AddImage((ImTextureID)(intptr_t)t, ImVec2(ox, oy), ImVec2(ox + drawW, oy + drawH));
            return;
        }
    }
    const bool havePixels = pic.pixels && uploadTexture && pic.width > 0 && pic.height > 0;
    const size_t count = (size_t)std::max(0, pic.width) * (size_t)std::max(0, pic.height);
    // CRT effect, blur: the picture softened, for the plain picture, the slot mask and the bloom
    if (look.crtEffect && look.crtBlur > 0.001f && havePixels) {
        if (!st.blurTexture || st.blurFrame != pic.frame || st.blurAmount != look.crtBlur || st.blurPixels.size() != count) {
            softenRender(pic.pixels, pic.width, pic.height, look.crtBlur, st.blurPixels);
            st.blurTexture = uploadTexture(st.blurSlot, st.blurPixels.data(), pic.width, pic.height);
            st.blurFrame = pic.frame;
            st.blurAmount = look.crtBlur;
        }
        tex = (ImTextureID)(intptr_t)st.blurTexture;
    }
    dl->AddImage(tex, ImVec2(ox, oy), ImVec2(ox + drawW, oy + drawH));
    if (!look.crtEffect) return;
    // CRT effect: the tube's pattern (a slot mask on a colour tube, scanlines, that is a dark
    // line between two of the picture's lines, on a green or grey one, or as chosen) and a
    // faint bloom. Cheap, and off unless turned on.
    const bool softMask = look.crtPattern == "soft";
    if ((look.slotMask(pic.colourTube) || softMask) && look.crtMask > 0.001f && havePixels) {
        // the picture through the slot mask, each phosphor slot lit whole, at the picture's
        // drawn size (to 2560 x 2048), over the plain picture
        const int w = std::min(2560, (int)std::lround(drawW)), h = std::min(2048, (int)std::lround(drawH));
        const bool soft = look.crtBlur > 0.001f && st.blurPixels.size() == count;
        const MaskKey key { pic.frame, w, h, look.crtMask, look.crtMaskSize, look.crtMaskTurned, soft ? look.crtBlur : 0.0f, softMask };
        if (w > 0 && h > 0 && (!st.maskTexture || !(key == st.maskKey))) {
            // the phosphor slots (each lit whole, its own colour), or the soft mask over the whole picture
            (softMask ? softMaskRender : slotMaskRender)(soft ? st.blurPixels.data() : pic.pixels, pic.width, pic.height, w, h,
                                                         look.crtMask, look.crtMaskSize, look.crtMaskTurned, st.maskPixels);
            st.maskTexture = uploadTexture(st.maskSlot, st.maskPixels.data(), w, h);
            st.maskKey = key;
        }
        if (st.maskTexture) dl->AddImage((ImTextureID)(intptr_t)st.maskTexture, ImVec2(ox, oy), ImVec2(ox + drawW, oy + drawH));
    }
    if (look.crtBloom > 0.001f) {
        // A glow round each bright pixel: the picture again, offset the same distance in
        // eight directions, so the light spreads evenly and stays centred on its pixel. (It
        // was one copy ENLARGED by g on each side: that moved every glow away from the
        // screen's centre: up and left in the top-left quarter.) Each copy adds its light
        // where the renderer can, as a bloom does.
        const float g = look.crtBloom * 6.0f, d = g * 0.7071f;
        const ImVec2 taps[8] = { { g, 0 }, { -g, 0 }, { 0, g }, { 0, -g }, { d, d }, { -d, d }, { d, -d }, { -d, -d } };
        const int a = std::max(1, (int)(look.crtBloom * (additiveBlend ? 16.0f : 9.0f)));
        if (additiveBlend) dl->AddCallback(additiveBlend, nullptr);
        for (const ImVec2& t : taps)
            dl->AddImage(tex, ImVec2(ox + t.x, oy + t.y), ImVec2(ox + drawW + t.x, oy + drawH + t.y),
                         ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
        if (additiveBlend) dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }
    if (look.scanlines(pic.colourTube) && look.crtScanline > 0.001f && pic.lines > 0) {
        ImU32 col = IM_COL32(0, 0, 0, (int)(look.crtScanline * 150.0f));
        float step = drawH / (float)pic.lines;
        if (step >= 2.0f)
            for (float y = oy; y < oy + drawH; y += step)
                dl->AddLine(ImVec2(ox, y), ImVec2(ox + drawW, y), col, std::max(1.0f, step * 0.4f));
    }
}

void GuiShell::drawFullscreenPicture(const ShellFrameInfo& info) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##fullscreenpicture", nullptr, flags)) {
        screenPicture(info);
        if (ImGui::GetTime() < fullscreenHintUntil) {
            const char* hint = "F11 or a double-click: back to the window";
            const ImVec2 ts = ImGui::CalcTextSize(hint);
            ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
            const ImVec2 at(vp->Pos.x + (vp->Size.x - ts.x) * 0.5f, vp->Pos.y + vp->Size.y - ts.y - 24);
            dl->AddRectFilled(ImVec2(at.x - 10, at.y - 6), ImVec2(at.x + ts.x + 10, at.y + ts.y + 6), IM_COL32(0, 0, 0, 190), 4.0f);
            dl->AddText(at, ImGui::GetColorU32(kAccent), hint);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

void GuiShell::f11() {
    if (gfxScreenFocused) { toggleGfxScreenFull(); return; }
    if (toolFocused) return;                    // the tool's own key (the assembler: step a line)
    if (toggleFullscreen) toggleFullscreen();
}

// ============================================================== status bar
void GuiShell::drawStatusBar(const ShellFrameInfo& info) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 3));
    bool open = ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, ImGui::GetFrameHeight(), flags);
    ImGui::PopStyleVar();
    if (open) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(host.paused ? "PAUSED" : (host.turbo ? "TURBO" : host.tapeTurboActive() ? "TAPE TURBO" : "RUNNING"));
        if (host.mouseCaptured) { after().text("  MOUSE CAPTURED (F12 or middle button releases)"); }
        else if (host.emulatedMouseActive()) { after().textDisabled("  click the picture to use the mouse"); }
        if (host.recording()) {
            char rec[64];
            const int s = (int)host.recorder.seconds();
            std::snprintf(rec, sizeof rec, "  REC %d:%02d %.0f MB", s / 60, s % 60, host.recorder.bytesWritten() / (1024.0 * 1024.0));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 70, 60, 255));
            after().text(rec);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Recording to %s\nFile > Record > Stop recording", host.recorder.path().c_str());
        }
        if (host.movieRecording()) after().text("  REC MOVIE");
        else if (host.moviePlaying()) after().text("  PLAY MOVIE");
        ImGui::PopStyleColor();
        // while a movie runs, where it is (and whether in step) in place of the last message
        after().textDisabled("|"); after().text(host.movieRecording() || host.moviePlaying() ? host.movieStatus.c_str() : host.status.c_str());
        char snd[32];
        if (!host.audioEnabled || !info.soundOpen) std::snprintf(snd, sizeof(snd), "SND off");
        else std::snprintf(snd, sizeof(snd), "SND %3.0f ms", info.soundQueuedMs);
        char right[128];
        std::snprintf(right, sizeof(right), "CPC %4.1f Hz  |  SPD %3.0f%%  |  %s", host.cpcRefreshHz, info.emuSpeedPct, snd);
        float w = ImGui::CalcTextSize(right).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20.0f, ImGui::GetWindowWidth() - w - 12.0f));
        ImGui::TextUnformatted(right);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("CPC Hz: emulated display refresh (VSYNC rate)\nSPD: emulation speed against real time\n"
                              "SND: audio queued\nUI render rate: %.0f FPS", info.uiFramerate);
    }
    ImGui::End();
}

// ============================================================== shared items
void GuiShell::modelItems(bool asMenu) {
    for (int i = 0; i < (int)host.models.size(); i++) {
        const auto& m = host.models[i];
        bool selected = host.currentModel == i;
        if (asMenu) {
            if (ImGui::MenuItem(m.label.c_str(), nullptr, selected, m.available)) host.bootModel(i);
        } else {
            if (!m.available) ImGui::BeginDisabled();
            if (ImGui::Selectable(m.label.c_str(), selected)) host.bootModel(i);
            if (!m.available) ImGui::EndDisabled();
        }
        if (!m.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("ROMs not found in %s", host.romDir.c_str());
    }
}
void GuiShell::ramItems(bool asMenu) {
    int cur = host.currentModel;
    // A GX4000: 64K as sold, or 128K with the RAM mod (it then banks like a 6128).
    if (cur >= 0 && !host.models[(size_t)cur].computer) {
        const char* items[2] = { "64 KB  (as sold)", "128 KB  (RAM mod)" };
        for (int k = 0; k < 2; k++) {
            const bool sel = host.gx4000Ram128 == (k == 1);
            if (asMenu ? ImGui::MenuItem(items[k], nullptr, sel) : ImGui::Selectable(items[k], sel))
                if (!sel) host.setGx4000Ram128(k == 1);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(k ? "The 128K RAM mod: 128K software runs, as on a 6128 Plus.\nThe cartridge restarts." : "The GX4000 as Amstrad sold it.");
        }
        return;
    }
    for (int r : RAM_SIZES) {
        std::string l = std::to_string(r) + " KB";
        if (r >= 576) {   // the expansion board's own size, which is how boards were sold
            char b[48];
            const int board = r - 64;
            if (board % 1024 == 0) std::snprintf(b, sizeof b, "  (64K + %d MB)", board / 1024);
            else if (board > 1024) std::snprintf(b, sizeof b, "  (64K + %d.5 MB)", board / 1024);
            else std::snprintf(b, sizeof b, "  (64K + %dK)", board);
            l += b;
        }
        bool sel = host.ramKiB == r;
        if (asMenu ? ImGui::MenuItem(l.c_str(), nullptr, sel, cur >= 0) : ImGui::Selectable(l.c_str(), sel))
            if (cur >= 0) host.bootModel(cur, r, host.crtcType);
    }
    // A Plus's RAM beyond its own: a classic board, or an upgrade inside (EmuHost::plusExpansionClassic)
    if (cur >= 0 && host.models[(size_t)cur].plus) {
        ImGui::Separator();
        const char* kinds[2] = { "Expansion: a classic board", "Expansion: an internal upgrade" };
        const char* tips[2] = {
            "A RAM board on the expansion port, as sold for the CPC. It does not know the ASIC:\n"
            "with its RAM paged at &4000 and the ASIC page in, a write goes to both.",
            "RAM added inside the Plus: it behaves as the Plus's own RAM\n"
            "(a write under the ASIC page goes to the ASIC only)." };
        for (int k = 0; k < 2; k++) {
            const bool sel = host.plusExpansionClassic == (k == 0);
            if (asMenu ? ImGui::MenuItem(kinds[k], nullptr, sel) : ImGui::Selectable(kinds[k], sel))
                host.setPlusExpansionClassic(k == 0);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[k]);
        }
    }
}
void GuiShell::crtcItems(bool asMenu) {
    int cur = host.currentModel;
    for (int t = 0; t < 6; t++) {
        bool sel = host.crtcType == t;
        if (asMenu ? ImGui::MenuItem(CRTC_NAMES[t], nullptr, sel, cur >= 0) : ImGui::Selectable(CRTC_NAMES[t], sel))
            if (cur >= 0) host.bootModel(cur, host.ramKiB, t);
    }
}
// ACCC 9 (p.44-46): five GATE ARRAY parts. On a classic machine it is a separate chip and
// any of the three discrete ones may be fitted; on CRTC 3 and 4 the ASIC IS the GATE ARRAY.
struct GateArrayPartInfo { int part; const char* label; const char* notes; };
static const GateArrayPartInfo GA_PARTS[] = {
    { 40010, "40010  (most 6128s, later 464/664)",
      "The reference part: every ACCC timing is measured on it.\nBits already consumed at a mid-byte mode switch read 0 (9.3.4.3)." },
    { 40007, "40007  (early 464s, rare 6128 MC0057A)",
      "Decoder 1/16 usec ahead of the 40010 at a mode switch (9 p.46, 9.3.4.3 p.59).\n"
      "Consumed bits read 1. HSYNC black 1 Pixel-M2 longer (14.5.4 p.139)." },
    { 40008, "40008  (mostly 664s)",
      "Pin-compatible with the 40007 and the same timing: decoder lead,\nconsumed bits read 1, HSYNC black 1 Pixel-M2 longer." },
};
void GuiShell::gateArrayItems(bool asMenu) {
    const bool asic = host.crtcType == 3 || host.crtcType == 4 || (host.emu && host.emu->plusHardware);
    if (asic) {
        ImGui::TextDisabled(host.crtcType == 4 ? "40226 ASIC -- it IS the CRTC 4" : "40489 ASIC -- it IS the CRTC 3");
        ImGui::TextDisabled("(no separate Gate Array to choose)");
        return;
    }
    const int fitted = host.fittedGateArrayPart();
    for (const auto& g : GA_PARTS) {
        bool sel = fitted == g.part;
        if (asMenu ? ImGui::MenuItem(g.label, nullptr, sel) : ImGui::Selectable(g.label, sel))
            host.setGateArrayPart(g.part);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.notes);
    }
}
// The Z80's chip process (Z80::cmos). Every CPC shipped with the NMOS part.
void GuiShell::z80Items(bool asMenu) {
    struct Z80Part { bool cmos; const char* label; const char* notes; };
    static const Z80Part parts[] = {
        { false, "NMOS Z80A  (every CPC as shipped)", "The part every CPC, Plus and GX4000 shipped with." },
        { true,  "CMOS Z84C00  (a replacement part)",
          "OUT (C),0 writes &FF, not 0. Most music players use it to set the PSG to\n"
          "inactive, so on this part they latch an address instead: music goes wrong and\n"
          "the tape motor relay clicks, as on a real CPC with a CMOS Z80.\n"
          "LD A,I / LD A,R: no P/V fault when an interrupt follows." },
    };
    for (const auto& p : parts) {
        if (asMenu ? ImGui::MenuItem(p.label, nullptr, host.z80Cmos == p.cmos) : ImGui::Selectable(p.label, host.z80Cmos == p.cmos))
            host.setZ80Cmos(p.cmos);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.notes);
    }
}
void GuiShell::monitorSetItems(bool asMenu) {
    const auto& sets = monitorModels();
    const MonitorModel* current = host.monitorSetId.empty() ? nullptr : monitorModelFor(host.monitorSetId);
    const char* shipped = "As shipped with the machine";
    if (asMenu ? ImGui::MenuItem(shipped, nullptr, current == nullptr) : ImGui::Selectable(shipped, current == nullptr))
        host.setMonitorSet("");
    for (const MonitorModel* m : sets)
        if (asMenu ? ImGui::MenuItem(m->name, nullptr, current == m) : ImGui::Selectable(m->name, current == m))
            host.setMonitorSet(m->id);
}
// The two hold controls on the back of the set (docs/ service manuals).
void GuiShell::monitorSyncItems() {
    int hhold = host.monitorHHold, vhold = host.monitorVHold;
    fitNextItemWidth(160, "H. Hold");
    if (ImGui::SliderInt("H. Hold", &hhold, -79, 79)) host.setMonitorHolds(hhold, vhold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The horizontal oscillator's free-running period against\n"
                          "the CPC's 64 usec line, in Pixel-M2 per line. 0 is a set\n"
                          "aligned as the service manual says (15625 Hz); the end\n"
                          "of the control is 14500 Hz (+79). Turned too far, the\n"
                          "set can no longer hold the line.");
    fitNextItemWidth(160, "V. Hold");
    if (ImGui::SliderInt("V. Hold", &vhold, -50, 50)) host.setMonitorHolds(hhold, vhold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The vertical oscillator's free-running period, in lines.\n"
                          "0 is the set as calibrated. Turned too far, it no longer\n"
                          "holds the CPC's 312 lines and the picture rolls.");
    if (ImGui::SmallButton("Centre both")) host.setMonitorHolds(0, 0);
}
// The front controls: Brightness on every set, Contrast only where the set has it (the
// monochrome sets, per the service manuals' parts lists).
void GuiShell::monitorFrontItems() {
    const MonitorModel* set = host.monitorSetId.empty() ? (host.emu ? host.emu->shippedMonitorModel() : nullptr)
                                                        : monitorModelFor(host.monitorSetId);
    TubeKnobs k = host.monitorKnobs;
    bool changed = false;
    fitNextItemWidth(160, "Brightness");
    changed |= ImGui::SliderInt("Brightness", &k.brightness, 0, 100);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(set && set->hasContrast
                          ? "The knob on the front of the set: it moves the black level.\n"
                            "50 is the set as calibrated. Turn it up to light the dark\n"
                            "colours (BASIC's blue paper becomes a lit green); at 100\n"
                            "a black screen glows clearly."
                          : "The knob on the front of the set: it moves the black level.\n"
                            "50 is the set as calibrated. At 100 a black screen just\n"
                            "glows (CM14 manual, CUT OFF); lower darkens the dark colours.");
    if (set && set->hasContrast) {
        fitNextItemWidth(160, "Contrast");
        changed |= ImGui::SliderInt("Contrast", &k.contrast, 0, 100);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The monochrome sets' second knob: the picture's gain.\n"
                              "100 is full. With brightness and contrast both at 0 the\n"
                              "picture can still be seen slightly (MM12 manual, SUB BRIGHT).");
    }
    if (changed) host.setMonitorKnobs(k);
    if (ImGui::SmallButton("Reset knobs")) host.setMonitorKnobs(TubeKnobs());
    // The output's colour, per set: a trim on the light of each channel to match a real
    // monitor. Not a part of the set, and the tube model is not touched.
    const MonitorModel* active = monitorModelFor(host.activeMonitorSetId());
    if (ImGui::TreeNode("Output colour###outputcolour")) {
        textWrappedDisabled("For the %s only.", active ? active->name : "set plugged in now");
        std::array<int, 3> rgb = host.activeMonitorIntensity();
        bool moved = false;
        static const char* names[3] = { "Red", "Green", "Blue" };
        for (size_t c = 0; c < 3; c++) {
            fitNextItemWidth(160, names[c]);
            moved |= ImGui::SliderInt(names[c], &rgb[c], 0, 200, "%d %%");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("This channel's intensity on the output, kept for this\n"
                                  "monitor set only. 100 %% is unchanged. Ctrl+click to type.");
        }
        if (moved) host.setMonitorIntensity(rgb);
        if (ImGui::SmallButton("Reset colour")) host.setMonitorIntensity(TubeKnobs().intensity);
        ImGui::TreePop();
    }
}

// ============================================================== settings sections
// Video / Audio / Input / Expansions / tape options are in gui_panels.cpp.
void GuiShell::sectionRoms(bool asMenu) {
    int cur = host.currentModel;
    if (cur >= 0 && !host.models[cur].plus) {
        auto romRow = [&](const char* label, int which, const std::string& name) {
            ImGui::PushID(which);
            ImGui::TextUnformatted(label);
            if (after().smallButton("Replace"))
                browser.open(std::string("Replace ") + label, host.romDir, { ".rom", ".bin" },
                             [this, which](const std::string& p) { host.setFirmwareRom(which, p); });
            if (after().smallButton("Default")) host.clearFirmwareRom(which);
            ImGui::TextDisabled("  %s", name.empty() ? "(none)" : name.c_str());
            ImGui::PopID();
        };
        romRow("Lower ROM (OS)", 0, host.osRomName);
        romRow("ROM 0 (BASIC)", 1, host.basicRomName);
        if (host.models[cur].amsdos) romRow("ROM 7 (disc DOS)", 2, host.amsdosRomName);
        ImGui::TextDisabled("AMSDOS, or ParaDOS/ROMDOS for bigger formats");
    } else {
        ImGui::TextDisabled("(cartridge machine -- no firmware ROMs)");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Expansion ROM slots:");
    for (int slot = 1; slot < 16; slot++) {
        if (slot == 7 && cur >= 0 && !host.models[cur].plus && host.models[cur].amsdos) continue;  // shown above
        std::string nm = host.expansionRomName(slot);
        ImGui::PushID(1000 + slot);
        ImGui::Text("%2d", slot);
        ImGui::SameLine(36);
        ImGui::TextUnformatted(nm.empty() ? "(empty)" : nm.c_str());
        // a menu sizes itself: a column there; in a window the buttons follow and wrap
        bool fit;
        if (asMenu) { ImGui::SameLine(200.0f); fit = ImGui::SmallButton("Fit"); }
        else fit = after().smallButton("Fit");
        if (fit)
            browser.open("Fit ROM slot " + std::to_string(slot), host.romDir, { ".rom", ".bin" },
                         [this, slot](const std::string& p) { host.fitExpansionRom(slot, p); });
        if (!nm.empty()) { if (after().smallButton("Clr")) host.clearExpansionRom(slot); }
        ImGui::PopID();
    }
}

// ============================================================== menus
void GuiShell::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    menuFile();
    menuMachine();
    menuMedia();
    menuSection("Video", &GuiShell::sectionVideo);
    menuSection("Audio", &GuiShell::sectionAudio);
    menuSection("Input", &GuiShell::sectionInput);
    menuSection("Expansions", &GuiShell::sectionExpansions);
    menuTools();
    menuDebug();
    menuWindow();
    menuHelp();
    ImGui::EndMainMenuBar();
}

void GuiShell::menuFile() {
    if (!ImGui::BeginMenu("File")) return;
    if (ImGui::BeginMenu("Open")) {
        if (ImGui::MenuItem("Disk into drive A...")) browser.open("Insert disk A", host.romDir, { ".dsk", ".edsk", ".hfe", ".ipf" }, [this](const std::string& p) { host.loadDiskFile(p, 0); });
        if (ImGui::MenuItem("Disk into drive B...")) browser.open("Insert disk B", host.romDir, { ".dsk", ".edsk", ".hfe", ".ipf" }, [this](const std::string& p) { host.loadDiskFile(p, 1); });
        if (ImGui::MenuItem("Tape...")) browser.open("Insert tape", host.romDir, { ".cdt", ".tzx", ".tap", ".wav" }, [this](const std::string& p) { host.loadTapeFile(p); });
        if (ImGui::MenuItem("Cartridge (.cpr, .cro)...")) browser.open("Load cartridge", host.romDir, { ".cpr", ".cro", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); });
        if (ImGui::MenuItem("Snapshot (.sna)...")) browser.open("Load snapshot", host.romDir, { ".sna" }, [this](const std::string& p) { host.loadSnapshot(p); });
        ImGui::Separator();
        if (ImGui::MenuItem("Assembler source...")) { panelOpen("Assembler") = true; assembler->requestOpen(); }
        if (ImGui::MenuItem("BASIC program (text)...")) { panelOpen("BASIC") = true; basicWindow->requestOpen(); }
        ImGui::EndMenu();
    }
    recentFilesMenu();
    ImGui::Separator();
    if (ImGui::BeginMenu("Quick save", host.booted())) { quickSlots(true, true); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Quick load", host.booted())) { quickSlots(true, false); ImGui::EndMenu(); }
    if (ImGui::MenuItem("Undo the last load", nullptr, false, host.canUndoLoad())) host.undoSlotLoad();
    ImGui::Separator();
    if (ImGui::MenuItem("Save snapshot...", nullptr, false, host.booted()))
        saver.open("Save snapshot", host.romDir, "snapshot.sna", [this](const std::string& p) { host.saveSnapshot(p); });
    if (ImGui::MenuItem("Save screenshot...", nullptr, false, host.booted()))
        saver.open("Save screenshot", host.romDir, "screenshot.bmp", [this](const std::string& p) { host.saveScreenshotBmp(p); });
    recordMenu();
    ImGui::Separator();
    if (ImGui::MenuItem("Run CSL script...")) panelOpen("CSL scripts") = true;
    ImGui::Separator();
    if (ImGui::MenuItem("ROM folder...")) browser.openDir("ROM folder", host.romDir, [this](const std::string& d) { host.setRomDir(d); });
    ImGui::Separator();
    if (ImGui::MenuItem("Exit", "Alt+F4")) quitRequested = true;
    ImGui::EndMenu();
}

void GuiShell::menuMachine() {
    if (!ImGui::BeginMenu("Machine")) return;
    if (ImGui::BeginMenu("Model")) { modelItems(true); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("RAM", host.currentModel >= 0)) { ramItems(true); ImGui::EndMenu(); }
    ImGui::Separator();
    ImGui::TextDisabled("Video chips");
    if (ImGui::BeginMenu("CRTC", host.currentModel >= 0)) { crtcItems(true); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Gate Array", host.currentModel >= 0)) { gateArrayItems(true); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Z80")) { z80Items(true); ImGui::EndMenu(); }
    ImGui::Separator();
    if (ImGui::BeginMenu("ROMs")) { sectionRoms(true); ImGui::EndMenu(); }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset", "Ctrl+R", false, host.booted())) host.reset();
    if (ImGui::MenuItem("Pause", "Ctrl+P", host.paused, host.booted())) { if (host.paused) debugger.run(); else debugger.pause(); }
    ImGui::MenuItem("Unlimited speed", nullptr, &host.turbo);
    if (ImGui::BeginMenu("Speed")) {
        const float speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
        for (float s : speeds) {
            char l[16]; std::snprintf(l, sizeof(l), "%g%%", s * 100.0f);
            if (ImGui::MenuItem(l, nullptr, std::fabs(host.speed - s) < 0.001f)) host.speed = s;
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    // input movies: from power-on, every input; played back, the same run
    if (ImGui::BeginMenu("Movie", host.booted())) {
        if (!host.movieRecording() && !host.moviePlaying()) {
            if (ImGui::MenuItem("Record from power-on")) { std::string why; if (!host.movieRecord(why)) host.status = why; }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The machine is reset (the disc drives and the tape as at power-on),\n"
                                  "then every key, joystick, mouse and tape button is recorded.\n"
                                  "Played back with the same machine and media, it is the same run.");
            if (ImGui::MenuItem("Play..."))
                browser.open("Play a movie", host.romDir, { ".cpcmovie" }, [this](const std::string& p) {
                    std::string why;
                    if (!host.moviePlay(p, why)) host.status = why;
                });
        }
        if (host.movieRecording()) {
            if (ImGui::MenuItem("Stop and save..."))
                saver.open("Save the movie", host.romDir, "movie.cpcmovie", [this](const std::string& p) {
                    std::string why;
                    if (!host.movieSave(p, why)) host.status = why;
                });
            if (ImGui::MenuItem("Stop (not saved)")) host.movieStop();
        }
        if (host.moviePlaying() && ImGui::MenuItem("Stop playing")) host.movieStop();
        if (!host.movieStatus.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", host.movieStatus.c_str()); }
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void GuiShell::menuMedia() {
    if (!ImGui::BeginMenu("Media")) return;
    ImGui::MenuItem("Save disc and tape writes to their files", nullptr, &host.diskWriteBack);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What a program writes to a disc goes back into its image file.\nOff: the changes stay in memory (save them from the DSK editor).");
    ImGui::Separator();
    for (int d = 0; d < 2; d++) {
        char label[32]; std::snprintf(label, sizeof(label), "Drive %c", 'A' + d);
        if (ImGui::BeginMenu(label)) {
            ImGui::TextDisabled("%s", host.diskName[d].empty() ? "(empty)" : host.diskName[d].c_str());
            if (ImGui::MenuItem("Insert...")) browser.open(std::string("Insert disk ") + char('A' + d), host.romDir, { ".dsk", ".edsk", ".hfe", ".ipf" }, [this, d](const std::string& p) { host.loadDiskFile(p, d); });
            if (ImGui::MenuItem("Eject", nullptr, false, !host.diskName[d].empty())) host.ejectDisk(d);
            if (ImGui::MenuItem("Open in the DSK editor", nullptr, false, !host.diskName[d].empty())) {
                dskEditor->takeFromDrive(d);
                panelOpen("DSK editor") = true;
            }
            ImGui::EndMenu();
        }
    }
    if (ImGui::BeginMenu("Tape")) {
        ImGui::TextDisabled("%s", host.tapeName.empty() ? "(no tape)" : host.tapeName.c_str());
        if (ImGui::MenuItem("Insert...")) browser.open("Insert tape", host.romDir, { ".cdt", ".tzx", ".tap", ".wav" }, [this](const std::string& p) { host.loadTapeFile(p); });
        tapeDeckControls(true);
        ImGui::Separator();
        sectionTape(true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Cartridge")) {
        if (!host.cartName.empty()) ImGui::TextDisabled("%s", host.cartName.c_str());
        if (ImGui::MenuItem("Load cartridge (.cpr, .cro)...")) browser.open("Load cartridge", host.romDir, { ".cpr", ".cro", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); });
        if (ImGui::MenuItem("Eject", nullptr, false, !host.cartName.empty())) host.ejectCartridge();
        // A CRO with more than one group: which one the machine runs.
        if (host.croGroupLabels.size() > 1 && ImGui::BeginMenu("CRO group")) {
            for (size_t i = 0; i < host.croGroupLabels.size(); i++) {
                const bool on = host.croCurrentGroup == host.croGroupNumbers[i];
                if (ImGui::MenuItem((host.croGroupLabels[i] + "##crogroup" + std::to_string(i)).c_str(), nullptr, on))
                    host.loadCroGroup(host.croGroupNumbers[i]);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Snapshot")) {
        if (!host.snapshotName.empty()) ImGui::TextDisabled("%s", host.snapshotName.c_str());
        if (ImGui::MenuItem("Load .SNA...")) browser.open("Load snapshot", host.romDir, { ".sna" }, [this](const std::string& p) { host.loadSnapshot(p); });
        if (ImGui::MenuItem("Save .SNA...", nullptr, false, host.booted())) saver.open("Save snapshot", host.romDir, "snapshot.sna", [this](const std::string& p) { host.saveSnapshot(p); });
        if (ImGui::MenuItem("Eject", nullptr, false, !host.snapshotName.empty())) host.ejectSnapshot();
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void GuiShell::menuSection(const char* title, void (GuiShell::*section)(bool)) {
    if (!ImGui::BeginMenu(title)) return;
    (this->*section)(true);
    // The same section as a window of its own ("Video" -> "Video settings").
    for (const char* w : SETTINGS_WINDOWS) {
        if (std::string(w).rfind(title, 0) != 0) continue;
        ImGui::Separator();
        if (ImGui::MenuItem("Open as a window")) { panelOpen(w) = true; focusWindowNextFrame = w; }
        break;
    }
    ImGui::EndMenu();
}

void GuiShell::menuTools() {
    if (!ImGui::BeginMenu("Tools")) return;
    ImGui::MenuItem("DSK editor", nullptr, &panelOpen("DSK editor"));
    ImGui::MenuItem("Cheats (POKE finder)", nullptr, &panelOpen("Cheats"));
    ImGui::MenuItem("Graphics finder", nullptr, &panelOpen("Graphics finder"));
    ImGui::MenuItem("CRO editor", nullptr, &panelOpen("CRO editor"));
    ImGui::MenuItem("CSL scripts", nullptr, &panelOpen("CSL scripts"));
    ImGui::MenuItem("Printer output", nullptr, &panelOpen("Printer"));
    ImGui::MenuItem("GFX9000", nullptr, &panelOpen("GFX9000"));
    ImGui::Separator();
    ImGui::MenuItem("Assembler", nullptr, &panelOpen("Assembler"));
    if (ImGui::MenuItem("Assemble", "F9")) { panelOpen("Assembler") = true; assembler->assemble(false); }
    if (ImGui::MenuItem("Assemble and run", "Ctrl+F9")) { panelOpen("Assembler") = true; assembler->assemble(true); }
    ImGui::Separator();
    ImGui::MenuItem("BASIC", nullptr, &panelOpen("BASIC"));
    ImGui::EndMenu();
}

void GuiShell::menuDebug() {
    if (!ImGui::BeginMenu("Debug")) return;
    bool b = host.booted();
    if (ImGui::MenuItem(host.paused ? "Run" : "Pause", "F5", false, b)) { if (host.paused) debugger.run(); else debugger.pause(); }
    if (ImGui::MenuItem("Step into", "F7", false, b)) debugger.stepInto();
    if (ImGui::MenuItem("Step over", "F8", false, b)) debugger.stepOver();
    if (ImGui::MenuItem("Step out", "Shift+F8", false, b)) debugger.stepOut();
    ImGui::Separator();
    if (ImGui::MenuItem("Toggle breakpoint at PC", nullptr, false, b)) debugger.toggleBreakpoint(host.emu->cpu->pc);
    if (ImGui::MenuItem("Remove all breakpoints", nullptr, false, !debugger.breakpoints.empty())) {
        while (!debugger.breakpoints.empty()) debugger.removeBreakpoint(0);
    }
    if (ImGui::MenuItem("Remove all watchpoints", nullptr, false, !debugger.watchpoints.empty())) debugger.watchpoints.clear();
    ImGui::Separator();
    for (const char* w : { "Debugger", "Chips", "Memory", "Logic analyser", "Profiler", "GFX9000", "Assembler", "BASIC" })
        ImGui::MenuItem(w, nullptr, &panelOpen(w));
    {
        bool overlay = host.rasterOverlay;
        if (ImGui::MenuItem("Raster timing overlay", nullptr, &overlay)) host.setRasterOverlay(overlay);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A tick on the picture where each OUT was made, as the beam swept it:\n"
                              "yellow CRTC register select, orange CRTC data, white GA pen, magenta GA ink,\n"
                              "cyan GA mode / ROMs, blue GA RAM bank, green PPI / AY, pink Plus palette.");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Assemble", "F9")) { panelOpen("Assembler") = true; assembler->assemble(false); }
    if (ImGui::MenuItem("Assemble and run", "Ctrl+F9")) { panelOpen("Assembler") = true; assembler->assemble(true); }
    ImGui::Separator();
    if (ImGui::BeginMenu("External debugging")) { sectionDevelopment(true); ImGui::EndMenu(); }
    ImGui::EndMenu();
}

void GuiShell::menuWindow() {
    if (!ImGui::BeginMenu("Window")) return;
    ImGui::MenuItem("Screen", nullptr, &panelOpen("Screen"));
    ImGui::MenuItem("CPC keyboard", nullptr, &panelOpen("CPC keyboard"));
    ImGui::Separator();
    ImGui::MenuItem("Machine", nullptr, &panelOpen("Machine"));
    ImGui::MenuItem("ROMs", nullptr, &panelOpen("ROMs"));
    if (ImGui::BeginMenu("Media")) {
        if (ImGui::MenuItem("All media")) for (const char* w : MEDIA_WINDOWS) panelOpen(w) = true;
        ImGui::Separator();
        for (const char* w : MEDIA_WINDOWS) ImGui::MenuItem(w, nullptr, &panelOpen(w));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("All settings")) openAllSettings();
        ImGui::Separator();
        for (const char* w : SETTINGS_WINDOWS) ImGui::MenuItem(w, nullptr, &panelOpen(w));
        ImGui::EndMenu();
    }
    ImGui::Separator();
    for (const char* w : { "Debugger", "Chips", "Memory", "Logic analyser", "Profiler", "GFX9000", "GFX9000 screen" }) ImGui::MenuItem(w, nullptr, &panelOpen(w));
    ImGui::Separator();
    for (const char* w : { "Assembler", "BASIC", "DSK editor", "Cheats", "Graphics finder", "CRO editor", "CSL scripts", "Printer" }) ImGui::MenuItem(w, nullptr, &panelOpen(w));
    ImGui::Separator();
    if (ImGui::BeginMenu("Interface size")) {
        for (int pct : { 90, 100, 110, 125, 150, 175, 200 }) {
            char label[16]; std::snprintf(label, sizeof label, "%d%%", pct);
            if (ImGui::MenuItem(label, nullptr, std::fabs(uiScale * 100 - pct) < 1)) { uiScale = pct / 100.0f; applyStyle(); }
        }
        ImGui::EndMenu();
    }
    ImGui::MenuItem("Status bar", nullptr, &showStatusBar);
    if (ImGui::MenuItem("Reset layout")) {
        for (auto& p : panels) p.open = p.byDefault;
        resetLayout = true;
    }
    ImGui::EndMenu();
}

void GuiShell::menuHelp() {
    if (!ImGui::BeginMenu("Help")) return;
    if (ImGui::BeginMenu("Keyboard shortcuts")) {
        ImGui::TextUnformatted("F11          fullscreen (or double-click the screen)");
        ImGui::TextUnformatted("Ctrl+R       reset");
        ImGui::TextUnformatted("Ctrl+P       pause / run");
        ImGui::TextUnformatted("Ctrl+1..9    load quick save slot 1..9");
        ImGui::TextUnformatted("Ctrl+Shift+1..9  save it");
        ImGui::TextUnformatted("Ctrl+Left    rewind a second");
        ImGui::Separator();
        ImGui::TextDisabled("in a debugger window, or while paused:");
        ImGui::TextUnformatted("F5           run / pause");
        ImGui::TextUnformatted("F7           step into");
        ImGui::TextUnformatted("F8           step over");
        ImGui::TextUnformatted("Shift+F8     step out");
        ImGui::Separator();
        ImGui::TextDisabled("in the assembler:");
        ImGui::TextUnformatted("F9           assemble into memory");
        ImGui::TextUnformatted("Ctrl+F9      assemble and run");
        ImGui::TextUnformatted("Ctrl+S / O   save / open");
        ImGui::Separator();
        ImGui::TextDisabled("Keys go to the CPC unless a debugger or");
        ImGui::TextDisabled("assembler window has the focus.");
        ImGui::EndMenu();
    }
    ImGui::MenuItem("About CPCSyntaxError", nullptr, &showAbout);
    ImGui::MenuItem("Dear ImGui demo", nullptr, &showImGuiDemo);
    ImGui::EndMenu();
}

// The logo across the top of the Machine window: its natural size times the interface size,
// never wider than the window. (Without a renderer to upload it to, the name in text.)
void GuiShell::drawLogo() {
    if (!logoTexture && uploadTexture) {
        std::vector<uint32_t> px((size_t)kLogoWidth * kLogoHeight);
        size_t at = 0;
        for (size_t i = 0; i + 4 < sizeof kLogoRuns && at < px.size(); i += 5)
            for (int n = 0; n < kLogoRuns[i] && at < px.size(); n++)
                px[at++] = (uint32_t)kLogoRuns[i + 1] | (uint32_t)kLogoRuns[i + 2] << 8 |
                           (uint32_t)kLogoRuns[i + 3] << 16 | (uint32_t)kLogoRuns[i + 4] << 24;
        logoTexture = uploadTexture(9, px.data(), kLogoWidth, kLogoHeight);
    }
    if (!logoTexture) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.4f);
        ImGui::TextColored(kAccent, "CPCSyntaxError");
        ImGui::PopFont();
        return;
    }
    const float w = std::min(ImGui::GetContentRegionAvail().x, kLogoWidth * 0.5f * uiScale);
    ImGui::Image((ImTextureID)(intptr_t)logoTexture, ImVec2(w, w * kLogoHeight / kLogoWidth));
}

// ============================================================== quick save slots
unsigned GuiShell::slotPicture(int slot) {
    const EmuHost::SlotInfo& s = host.slotInfo(slot);
    if (s.thumb.empty() || !uploadTexture) return 0;
    if (slotTextureRevision[slot] != s.revision || !slotTexture[slot]) {
        slotTexture[slot] = uploadTexture(100 + slot, s.thumb.data(), s.thumbW, s.thumbH);
        slotTextureRevision[slot] = s.revision;
    }
    return slotTexture[slot];
}

void GuiShell::quickSlots(bool asMenu, bool save) {
    if (asMenu) {
        ImGui::TextDisabled("for %s", host.slotProgram().c_str());
        for (int s = 1; s <= EmuHost::SLOT_COUNT; s++) {
            const EmuHost::SlotInfo& info = host.slotInfo(s);
            char label[96], keys[24];
            std::snprintf(label, sizeof label, "Slot %d   %s", s, info.used ? info.when.c_str() : "(empty)");
            std::snprintf(keys, sizeof keys, save ? "Ctrl+Shift+%d" : "Ctrl+%d", s);
            if (ImGui::MenuItem(label, keys, false, save || info.used)) { if (save) host.saveSlot(s); else host.loadSlot(s); }
            if (info.used && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (const unsigned tex = slotPicture(s)) {
                    ImGui::BeginTooltip();
                    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2((float)info.thumbW, (float)info.thumbH));
                    ImGui::EndTooltip();
                }
            }
        }
        return;
    }
    // the window: a card per slot, as many across as fit
    ImGui::TextColored(kAccent, "Quick save slots");
    textWrappedDisabled("for %s.  Ctrl+1..9 loads, Ctrl+Shift+1..9 saves.", host.slotProgram().c_str());
    const ImGuiStyle& st = ImGui::GetStyle();
    const float cardW = std::max(96.0f, 140.0f * uiScale);
    const float avail = ImGui::GetContentRegionAvail().x;
    const int across = std::max(1, (int)((avail + st.ItemSpacing.x) / (cardW + st.ItemSpacing.x)));
    const float w = std::min(avail, (avail - st.ItemSpacing.x * (across - 1)) / across);
    const float h = w * 136.0f / 192.0f;
    ImGui::BeginDisabled(!host.booted());
    for (int s = 1; s <= EmuHost::SLOT_COUNT; s++) {
        if ((s - 1) % across) ImGui::SameLine();
        const EmuHost::SlotInfo& info = host.slotInfo(s);
        ImGui::PushID(s);
        ImGui::BeginGroup();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (const unsigned tex = info.used ? slotPicture(s) : 0) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(w, h));
        else {
            ImGui::Dummy(ImVec2(w, h));
            dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg));
            const char* t = info.used ? "(no picture)" : "empty";
            const ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText(ImVec2(at.x + (w - ts.x) / 2, at.y + (h - ts.y) / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
        }
        // the slot's number in its corner
        char n[4]; std::snprintf(n, sizeof n, "%d", s);
        dl->AddRectFilled(at, ImVec2(at.x + ImGui::GetFontSize() * 1.2f, at.y + ImGui::GetFontSize() * 1.2f), IM_COL32(0, 0, 0, 170));
        dl->AddText(ImVec2(at.x + ImGui::GetFontSize() * 0.3f, at.y + ImGui::GetFontSize() * 0.1f), ImGui::GetColorU32(kAccent), n);
        if (info.used && ImGui::IsItemHovered()) ImGui::SetTooltip("Saved %s\nDouble-click to load", info.when.c_str());
        if (info.used && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) host.loadSlot(s);
        const float bw = (w - st.ItemSpacing.x) / 2;
        if (ImGui::Button("Save", ImVec2(bw, 0))) host.saveSlot(s);
        ImGui::SameLine();
        ImGui::BeginDisabled(!info.used);
        if (ImGui::Button("Load", ImVec2(bw, 0))) host.loadSlot(s);
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!host.canUndoLoad());
    if (ImGui::SmallButton("Undo the last load")) host.undoSlotLoad();
    ImGui::EndDisabled();
}

// a file named for the program and the time, in the recordings folder
void GuiShell::startRecordingDialog(bool video) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(host.recordDir), ec);
    const std::time_t t = std::time(nullptr);
    char when[32] = "";
    if (const std::tm* tm = std::localtime(&t)) std::strftime(when, sizeof when, "_%Y%m%d_%H%M%S", tm);
    const std::string name = host.slotProgram() + when + (!video ? ".wav" : host.recordFormat == "h264" ? ".mp4" : ".avi");
    saver.open(video ? "Record video" : "Record sound", host.recordDir, name, [this](const std::string& p) {
        std::string why;
        if (!host.startRecording(p, why)) host.status = why;
    });
}

void GuiShell::recordMenu() {
    if (!ImGui::BeginMenu("Record", host.booted())) return;
    ImGui::BeginDisabled(host.recording());
    if (ImGui::MenuItem(host.recordFormat == "h264" ? "Video and sound (.mp4)..." : "Video and sound (.avi)...")) startRecordingDialog(true);
    if (ImGui::MenuItem("Sound only (.wav)...")) startRecordingDialog(false);
    ImGui::EndDisabled();
    if (ImGui::MenuItem("Stop recording", nullptr, false, host.recording())) host.stopRecording();
    ImGui::Separator();
    ImGui::TextDisabled("Picture");
    {
        static const char* const sources[][3] = {
            { "As shown", "shown", "As the screen shows it: the GFX9000's picture when it is on the monitor,\nthe two side by side when it has its own picture (beside the CPC's, or its own window)." },
            { "CPC", "cpc", "The CPC's own picture only." },
            { "GFX9000", "gfx", "The GFX9000's picture only (its Video9000 mix when it has one), at 4:3 in the CPC's frame." },
            { "Both, side by side", "both", "The CPC's picture and the GFX9000's next to it: a frame twice as wide." },
            { "Screen, with its effects", "screen", "The Screen window's picture as it is drawn: the CRT effect or the advanced\nCRT filter included, at the size it is shown when the recording starts\n(the Screen window in the main window, or full screen)." } };
        for (const auto& src : sources) {
            const bool needsGfx = std::string(src[1]) == "gfx" || std::string(src[1]) == "both";
            if (ImGui::MenuItem(src[0], nullptr, host.recordSource == src[1], !host.recording() && (!needsGfx || host.v9990Enabled)))
                host.recordSource = src[1];
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", src[2]);
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("Video format");
    {
        static bool looked = false;
        static std::string found;
        if (!looked || ImGui::IsWindowAppearing()) { found = MediaRecorder::findFfmpeg(host.ffmpegPath); looked = true; }
        if (ImGui::MenuItem("H.264 MP4 (small; every player)", nullptr, host.recordFormat == "h264", !host.recording() && !found.empty())) host.recordFormat = "h264";
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", found.empty() ? "Needs ffmpeg (ffmpeg.org): install it, or put ffmpeg=<its program> in cpcse.ini."
                                                  : ("x264 by ffmpeg (" + found + "): a minute of CPC is some 10-30 MB.").c_str());
    }
    if (ImGui::MenuItem("MJPEG (plays in most players)", nullptr, host.recordFormat == "mjpeg", !host.recording())) host.recordFormat = "mjpeg";
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every frame a JPEG: Windows' own player plays it. About 9 MB a second.");
    ImGui::EndMenu();
}

void GuiShell::recentFilesMenu() {
    if (!ImGui::BeginMenu("Recent files", !host.recentFiles.empty())) return;
    std::string load;
    for (size_t i = 0; i < host.recentFiles.size(); i++) {
        const std::string& p = host.recentFiles[i];
        std::error_code ec;
        const std::filesystem::path path = std::filesystem::u8path(p);
        const bool there = std::filesystem::is_regular_file(path, ec);
        std::string ext = path.extension().u8string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        const char* kind = ext == ".cpr" || ext == ".cro" ? "cartridge" : ext == ".sna" ? "snapshot"
                         : (ext == ".cdt" || ext == ".tzx" || ext == ".tap" || ext == ".wav") ? "tape" : "disc";
        char label[400];
        std::snprintf(label, sizeof label, "%zu  %s##recent%zu", i + 1, path.filename().u8string().c_str(), i);
        if (ImGui::MenuItem(label, kind, false, there)) load = p;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s%s", p.c_str(), there ? "" : "\n(the file is not there now)");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear the list")) host.clearRecent();
    ImGui::EndMenu();
    if (!load.empty()) host.loadByExtension(load);    // after the loop: it changes the list
}

// ============================================================== settings windows
void GuiShell::windowMachine() {
    if (!panelOpen("Machine")) return;
    if (ImGui::Begin("Machine", &panelOpen("Machine"))) {
        WrapText wrapText;                     // text wraps at the window's edge
        drawLogo();

        const float labelW = ImGui::GetFontSize() * 4.8f;   // the labels' column
        int cur = host.currentModel;
        if (ImGui::CollapsingHeader("Machine", ImGuiTreeNodeFlags_DefaultOpen)) {
            std::string curLabel = cur >= 0 ? host.models[cur].label
                                            : (host.cartName.empty() ? "(select a machine)" : "Plus / GX4000 cartridge");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##model", curLabel.c_str())) { modelItems(false); ImGui::EndCombo(); }
            if (cur >= 0) {
                const auto& m = host.models[cur];
                ImGui::TextDisabled("%dK RAM%s%s", host.ramKiB, m.amsdos ? ", disc drive" : ", tape only",
                                    m.plus ? ", boots its system cartridge" : "");
            }
            ImGui::BeginDisabled(cur < 0);
            ImGui::TextUnformatted("RAM"); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##ram", (std::to_string(host.ramKiB) + " KB").c_str())) { ramItems(false); ImGui::EndCombo(); }
            ImGui::TextUnformatted("CRTC"); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##crtc", CRTC_NAMES[host.crtcType % 6])) { crtcItems(false); ImGui::EndCombo(); }
            ImGui::TextUnformatted("GA"); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1);
            {
                const bool asic = host.crtcType == 3 || host.crtcType == 4;
                const char* gaLabel = host.crtcType == 4 ? "40226 ASIC (is the CRTC 4)"
                                    : host.crtcType == 3 ? "40489 ASIC (is the CRTC 3)" : "40010";
                const int fitted = host.fittedGateArrayPart();
                for (const auto& g : GA_PARTS) if (!asic && g.part == fitted) gaLabel = g.label;
                if (ImGui::BeginCombo("##gatearray", gaLabel)) { gateArrayItems(false); ImGui::EndCombo(); }
            }
            ImGui::TextUnformatted("Z80"); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##z80", host.z80Cmos ? "CMOS Z84C00  (a replacement part)" : "NMOS Z80A  (every CPC as shipped)")) { z80Items(false); ImGui::EndCombo(); }
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Control", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::Button("Reset machine", ImVec2(-1, 0))) host.reset();
            bool paused = host.paused;
            if (ImGui::Checkbox("Paused", &paused)) { if (paused) debugger.pause(); else debugger.run(); }
            after().checkbox("Unlimited speed", &host.turbo);
            float pct = host.speed * 100.0f;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##speed", &pct, 25.0f, 400.0f, "speed %.0f%%", ImGuiSliderFlags_Logarithmic)) host.speed = pct / 100.0f;
            // the timeline: rewind (Ctrl+Left) and the debugger's way back
            checkboxWrapped("Timeline: rewind (Ctrl+Left) and step back", &host.timelineEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A snapshot of the machine every second and every input since, so it can go back:\n"
                                  "Ctrl+Left a second back; in the debugger Back, Frame back, Run back, Last write.\n"
                                  "A reset, a change of the machine or its media, or an edit in the debugger starts it again.\n"
                                  "The M4, OPL4, IDE, serial and speech cards are not rewound, nor what is written to a disc.");
            if (host.timelineEnabled) {
                const EmuHost::TimelineInfo t = host.timelineInfo();
                FlowRow row;
                row.field(150.0f, "##tlbudget");
                ImGui::SliderInt("##tlbudget", &host.timelineBudgetMB, 32, 1024, "up to %d MB", ImGuiSliderFlags_Logarithmic);
                char rec[96];
                std::snprintf(rec, sizeof rec, "%zu s held, %.0f MB", t.snapshots * (size_t)host.timelineInterval / 50, (double)t.bytes / (1024.0 * 1024.0));
                row.textDisabled(rec);
            }
        }
    }
    ImGui::End();
    if (panelOpen("ROMs")) {
        if (ImGui::Begin("ROMs", &panelOpen("ROMs"))) {
            WrapText wrapText;                 // text wraps at the window's edge
            sectionRoms(false);
        }
        ImGui::End();
    }
}

void GuiShell::windowMedia() {
    // Every slot the same way: what is in it (wrapping), then its buttons (a row that wraps),
    // Eject among them.
    auto loaded = [](const std::string& name, const char* none) {
        if (name.empty()) textWrappedDisabled("%s", none);
        else { ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(name.c_str()); ImGui::PopTextWrapPos(); }
    };
    if (panelOpen("Discs")) {
        if (ImGui::Begin("Discs", &panelOpen("Discs"))) {
            WrapText wrapText;                 // text wraps at the window's edge
            for (int d = 0; d < 2; d++) {
                ImGui::PushID(d);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kAccent, "%c:", 'A' + d);
                ImGui::SameLine();
                loaded(host.diskName[d], "(empty)");
                FlowRow row;
                if (row.smallButton("Insert..."))
                    browser.open(std::string("Insert disk ") + char('A' + d), host.romDir, { ".dsk", ".edsk", ".hfe", ".ipf" }, [this, d](const std::string& p) { host.loadDiskFile(p, d); });
                ImGui::BeginDisabled(host.diskName[d].empty());
                if (row.smallButton("Eject")) host.ejectDisk(d);
                if (row.smallButton("Edit")) { dskEditor->takeFromDrive(d); panelOpen("DSK editor") = true; }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Open the disc in the DSK editor");
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            textWrappedDisabled("Drop a .DSK or .EDSK onto the window");
            checkboxWrapped("Save disc and tape writes to their files", &host.diskWriteBack);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("What a program writes to a disc goes back into its image file.\nOff: the changes stay in memory (save them from the DSK editor).");
            if (!host.diskWriteError.empty()) textWrappedDisabled("Not saved: %s", host.diskWriteError.c_str());
        }
        ImGui::End();
    }
    if (panelOpen("Tape")) {
        if (ImGui::Begin("Tape", &panelOpen("Tape"))) {
            WrapText wrapText;
            loaded(host.tapeName, "(no tape)");
            if (ImGui::SmallButton("Insert tape...")) browser.open("Insert tape", host.romDir, { ".cdt", ".tzx", ".tap", ".wav" }, [this](const std::string& p) { host.loadTapeFile(p); });
            tapeDeckControls(false);
            ImGui::PushID("tapeopts"); sectionTape(false); ImGui::PopID();
        }
        ImGui::End();
    }
    if (panelOpen("Cartridge")) {
        if (ImGui::Begin("Cartridge", &panelOpen("Cartridge"))) {
            WrapText wrapText;
            loaded(host.cartName, "(no cartridge)");
            FlowRow row;
            if (row.smallButton("Load .CPR..."))
                browser.open("Load cartridge", host.romDir, { ".cpr", ".cro", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); });
            ImGui::BeginDisabled(host.cartName.empty());
            if (row.smallButton("Eject")) host.ejectCartridge();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Out of the slot: the machine boots without it (a Plus on its system cartridge)");
            ImGui::EndDisabled();
        }
        ImGui::End();
    }
    if (panelOpen("Snapshot")) {
        if (ImGui::Begin("Snapshot", &panelOpen("Snapshot"))) {
            WrapText wrapText;
            loaded(host.snapshotName, "(no snapshot loaded)");
            FlowRow row;
            if (row.smallButton("Load .SNA...")) browser.open("Load snapshot", host.romDir, { ".sna" }, [this](const std::string& p) { host.loadSnapshot(p); });
            ImGui::BeginDisabled(!host.booted());
            if (row.smallButton("Save .SNA...")) saver.open("Save snapshot", host.romDir, "snapshot.sna", [this](const std::string& p) { host.saveSnapshot(p); });
            ImGui::EndDisabled();
            ImGui::BeginDisabled(host.snapshotName.empty());
            if (row.smallButton("Eject")) host.ejectSnapshot();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Let the snapshot go: the machine boots afresh");
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!host.booted());
            if (row.smallButton("Screenshot .bmp...")) saver.open("Save screenshot", host.romDir, "screenshot.bmp", [this](const std::string& p) { host.saveScreenshotBmp(p); });
            ImGui::EndDisabled();
            ImGui::Separator();
            quickSlots(false, false);
        }
        ImGui::End();
    }
}

// The settings: one window per section (SETTINGS_WINDOWS), each dockable on its own.
void GuiShell::windowSettings() {
    static void (GuiShell::* const sections[])(bool) = {
        &GuiShell::sectionVideo, &GuiShell::sectionAudio, &GuiShell::sectionInput,
        &GuiShell::sectionExpansions, &GuiShell::sectionDevelopment };
    static_assert(sizeof(sections) / sizeof(sections[0]) == sizeof(SETTINGS_WINDOWS) / sizeof(SETTINGS_WINDOWS[0]),
                  "one section per settings window");
    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        const char* title = SETTINGS_WINDOWS[i];
        if (!panelOpen(title)) continue;
        if (ImGui::Begin(title, &panelOpen(title))) {
            WrapText wrapText;                 // text wraps at the window's edge
            (this->*sections[i])(false);
        }
        ImGui::End();
    }
}
// Every settings window, docked together as tabs (placeNewlyOpened), the first in front.
void GuiShell::openAllSettings() {
    for (const char* w : SETTINGS_WINDOWS) panelOpen(w) = true;
    focusWindowNextFrame = SETTINGS_WINDOWS[0];
}

void GuiShell::windowAbout() {
    if (!showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About CPCSyntaxError", &showAbout, ImGuiWindowFlags_NoDocking)) {
        WrapText wrapText;                     // text wraps at the window's edge
        ImGui::TextColored(kAccent, "CPCSyntaxError %s", CPCSE_VERSION);
        ImGui::TextDisabled("Amstrad CPC / Plus / GX4000 emulator");
        ImGui::Separator();
        ImGui::TextUnformatted("Author and prompter: WacKEDmaN");
        ImGui::TextUnformatted("Code written with Claude (Anthropic)");
        ImGui::TextDisabled("github.com/WacKEDmaN/CPCSyntaxError");
        ImGui::Separator();
        // ACCC §2.2 "Attribution Directive": the compendium the CRTC emulation is built
        // from asks for its mention "within the About / Credits window or documentation
        // if the output relates to a compiled application or emulator".
        ImGui::TextWrapped("CRTC behaviour sourced from the \"Amstrad CPC CRTC Compendium\" by Longshot "
                           "(CC BY-NC-ND), and checked against his SHAKER test suite.");
        ImGui::TextWrapped("Built-in assembler: RASM by Edouard BERGE (MIT), with the crunchers of "
                           "Yann Collet (LZ4), Einar Saukas (ZX7), Magnus Lind (Exomizer) and "
                           "Emmanuel Marty (LZSA, apultra, salvador).");
        ImGui::TextWrapped("User interface: Dear ImGui (MIT) on SDL2.");
    }
    ImGui::End();
}

} // namespace cpcse
