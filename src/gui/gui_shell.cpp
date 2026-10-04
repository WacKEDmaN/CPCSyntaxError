// CPCSyntaxError GUI — the desktop shell: menus, layout, the settings windows and the
// screen. The debugger windows are in gui_debug_views.cpp. See gui_shell.h.
#include "gui_shell.h"

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder, BeginViewportSideBar

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "gui_assembler.h"
#include "gui_dsk_editor.h"
#include "devserver.h"

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
        { "Media", "win_media", true, true },
        { "Debugger", "win_debugger", false, false },
        { "Chips", "win_chips", false, false },
        { "Memory", "win_memory", false, false },
        { "GFX9000", "win_gfx9000", false, false },
        { "Assembler", "win_asm", false, false },
        { "DSK editor", "win_dsk_editor", false, false },
        { "CSL scripts", "win_csl", false, false },
        { "Printer", "win_printer", false, false },
        { "Settings", "win_settings", false, false },
    };
    assembler = std::make_unique<AssemblerWindow>(host, debugger, browser, saver);
    assembler->showInDisassembly = [this](int a) { showInDisassembly(a); };
    dskEditor = std::make_unique<DskEditorWindow>(host, browser, saver);
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
    auto us = ini.find("ui_scale");
    if (us != ini.end()) uiScale = std::clamp((float)std::atof(us->second.c_str()), 0.75f, 2.5f);
    auto str = [&](const char* k, std::string& v) { auto i = ini.find(k); if (i != ini.end()) v = i->second; };
    str("csl_script", cslScript); str("csl_out", cslOut); str("csl_diskdir", cslDiskDir);
    auto ci = ini.find("csl_crtc"); if (ci != ini.end()) cslCrtc = std::atoi(ci->second.c_str());
    assembler->loadSettings(ini);
    dev->loadSettings(ini);
}

void GuiShell::saveSettings(std::ostream& out) const {
    out << "layout_version=" << LAYOUT_VERSION << "\n";
    for (const auto& p : panels) out << p.iniKey << "=" << (p.open ? 1 : 0) << "\n";
    out << "statusbar=" << (showStatusBar ? 1 : 0) << "\n";
    out << "ui_scale=" << uiScale << "\n";
    out << "csl_script=" << cslScript << "\n" << "csl_out=" << cslOut << "\n"
        << "csl_diskdir=" << cslDiskDir << "\n" << "csl_crtc=" << cslCrtc << "\n";
    assembler->saveSettings(out);
    dev->saveSettings(out);
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
    // The debugger's keys belong to the debugger while one of its windows has the focus,
    // or while the machine is stopped; otherwise they are the CPC's function keys.
    if (!toolFocused && !host.paused) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) { if (host.paused) debugger.run(); else debugger.pause(); }
    if (ImGui::IsKeyPressed(ImGuiKey_F7)) debugger.stepInto();
    if (ImGui::IsKeyPressed(ImGuiKey_F8)) { if (io.KeyShift) debugger.stepOut(); else debugger.stepOver(); }
}

void GuiShell::draw(const ShellFrameInfo& info) {
    debugger.attach();
    handleShortcuts();
    toolFocusedNow = false;

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
    windowPrinter();
    romPrompt();
    windowGfx9000();
    windowCslScripts();
    if (panelOpen("DSK editor")) {
        dskEditor->draw(&panelOpen("DSK editor"));
        if (dskEditor->focused) toolFocusedNow = true;
    }
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
    ImGui::DockBuilderDockWindow("Machine", left);
    ImGui::DockBuilderDockWindow("Media", leftBottom);
    ImGui::DockBuilderFinish(id);
    // what was placed before goes back to its group when next opened
    placedThisSession.clear();
    groupNode[0] = groupNode[1] = 0;
    for (const auto& p : panels)
        if (!p.byDefault) ImGui::ClearWindowSettings(p.title);
}

void GuiShell::placeNewlyOpened(unsigned dockspaceId) {
    struct Group { std::vector<const char*> titles; ImGuiDir dir; float ratio; };
    static const Group groups[] = {
        { { "Debugger", "Chips", "Memory", "GFX9000" }, ImGuiDir_Right, 0.42f },
        { { "Assembler", "DSK editor", "CSL scripts", "Printer", "Settings" }, ImGuiDir_Down, 0.42f },
    };
    auto placed = [&](const char* t) { return std::find(placedThisSession.begin(), placedThisSession.end(), t) != placedThisSession.end(); };
    bool changed = false;
    for (int gi = 0; gi < 2; gi++) {
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
    if (visible && info.screenTexture && info.textureWidth > 0) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        // With a GFX9000 fitted its own monitor stands beside the CPC's: the two share the
        // window, the same height, the GFX9000's a 4:3 set.
        const bool video9000 = host.v9990Enabled && host.gfx9000Monitor == "video9000";
        const bool gfxOnMain = host.gfx9000OnMainScreen() || video9000;
        const bool beside = host.v9990Enabled && host.gfx9000Monitor == "beside";
        const float gap = beside ? 6.0f : 0.0f;
        const float gfxAspect = 4.0f / 3.0f;
        float cpcAvailW = beside ? (avail.x - gap) * 0.5f : avail.x;
        float drawW = cpcAvailW, drawH = avail.y;
        if (host.maintainAspect) {
            const float cpcAspect = (float)info.textureWidth / (float)info.textureHeight;
            float h = beside ? std::min(avail.y, (avail.x - gap) / (cpcAspect + gfxAspect)) : avail.y;
            float scale = std::min((beside ? h * cpcAspect : avail.x) / (float)info.textureWidth, h / (float)info.textureHeight);
            if (host.integerScale && scale > 1.0f) scale = std::floor(scale);
            drawW = info.textureWidth * scale; drawH = info.textureHeight * scale;
        }
        const float gfxW = beside ? (host.maintainAspect ? drawH * gfxAspect : cpcAvailW) : 0.0f;
        const float totalW = drawW + gap + gfxW;
        float ox = origin.x + (avail.x - totalW) * 0.5f, oy = origin.y + (avail.y - drawH) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImTextureID tex = (ImTextureID)(intptr_t)info.screenTexture;
        if (gfxOnMain) {
            // One monitor, switched to the GFX9000: its picture, a 4:3 set filling the window.
            float gw = avail.x, gh = avail.y;
            if (host.maintainAspect) { gh = std::min(avail.y, avail.x * 0.75f); gw = gh * 4.0f / 3.0f; }
            const float gx = origin.x + (avail.x - gw) * 0.5f, gy = origin.y + (avail.y - gh) * 0.5f;
            int vw = 0, vh = 0;
            if (video9000 && uploadTexture && host.video9000Picture(video9000Pixels, vw, vh)) {
                // one monitor through the Video9000: the composed picture
                video9000Texture = uploadTexture(3, video9000Pixels.data(), vw, vh);
                dl->AddImage((ImTextureID)(intptr_t)video9000Texture, ImVec2(gx, gy), ImVec2(gx + gw, gy + gh));
            } else {
                drawGfxMonitor(dl, gx, gy, gw, gh);
            }
        } else
        dl->AddImage(tex, ImVec2(ox, oy), ImVec2(ox + drawW, oy + drawH));
        // CRT effect: a scanline overlay (dark lines every 2 source rows) and a faint
        // additive bloom pass (the image drawn again, brighter, on top). Cheap, and off
        // unless turned on.
        if (host.crtEffect && !gfxOnMain) {
            if (host.crtBloom > 0.001f) {
                float g = host.crtBloom * 6.0f;
                int a = (int)(host.crtBloom * 70.0f);
                dl->AddImage(tex, ImVec2(ox - g, oy - g), ImVec2(ox + drawW + g, oy + drawH + g),
                             ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
            }
            if (host.crtScanline > 0.001f) {
                ImU32 col = IM_COL32(0, 0, 0, (int)(host.crtScanline * 150.0f));
                float step = drawH / (float)info.textureHeight * 2.0f;
                if (step >= 2.0f)
                    for (float y = oy; y < oy + drawH; y += step)
                        dl->AddLine(ImVec2(ox, y), ImVec2(ox + drawW, y), col, std::max(1.0f, step * 0.4f));
            }
        }
        if (beside) drawGfxMonitor(dl, ox + drawW + gap, oy, gfxW, drawH);
        if (host.paused && host.booted()) {
            const char* label = host.breakReason.empty() ? "PAUSED" : host.breakReason.c_str();
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddRectFilled(ImVec2(ox + 6, oy + 6), ImVec2(ox + 14 + ts.x, oy + 10 + ts.y), IM_COL32(0, 0, 0, 170));
            dl->AddText(ImVec2(ox + 10, oy + 8), ImGui::GetColorU32(kAccent), label);
        }
        ImGui::InvisibleButton("##screenarea", ImVec2(std::max(1.0f, avail.x), std::max(1.0f, avail.y)));
        screenHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && host.symbifaceMouseActive() && !host.lightgunActive() && !host.mouseCaptured)
            mouseCaptureRequested = true;
        if (host.lightgunActive() && !gfxOnMain) lightgunFromScreen(ox, oy, drawW, drawH, info.textureWidth, info.textureHeight);
        if (screenHovered && ImGui::IsMouseDoubleClicked(0) && !host.symbifaceMouseActive() && !host.lightgunActive()
            && toggleFullscreen) toggleFullscreen();
    }
    ImGui::End();
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
        ImGui::TextUnformatted(host.paused ? "PAUSED" : (host.turbo ? "TURBO" : "RUNNING"));
        if (host.mouseCaptured) { after().text("  MOUSE CAPTURED (F12 or middle button releases)"); }
        else if (host.symbifaceMouseActive()) { after().textDisabled("  click the picture to use the mouse"); }
        ImGui::PopStyleColor();
        after().textDisabled("|"); after().text(host.status.c_str());
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
        if (ImGui::MenuItem("Disk into drive A...")) browser.open("Insert disk A", host.romDir, { ".dsk", ".edsk" }, [this](const std::string& p) { host.loadDiskFile(p, 0); });
        if (ImGui::MenuItem("Disk into drive B...")) browser.open("Insert disk B", host.romDir, { ".dsk", ".edsk" }, [this](const std::string& p) { host.loadDiskFile(p, 1); });
        if (ImGui::MenuItem("Tape...")) browser.open("Insert tape", host.romDir, { ".cdt", ".tzx", ".tap", ".wav" }, [this](const std::string& p) { host.loadTapeFile(p); });
        if (ImGui::MenuItem("Cartridge (.cpr)...")) browser.open("Load cartridge", host.romDir, { ".cpr", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); });
        if (ImGui::MenuItem("Snapshot (.sna)...")) browser.open("Load snapshot", host.romDir, { ".sna" }, [this](const std::string& p) { host.loadSnapshot(p); });
        ImGui::Separator();
        if (ImGui::MenuItem("Assembler source...")) { panelOpen("Assembler") = true; assembler->requestOpen(); }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Save snapshot...", nullptr, false, host.booted()))
        saver.open("Save snapshot", host.romDir, "snapshot.sna", [this](const std::string& p) { host.saveSnapshot(p); });
    if (ImGui::MenuItem("Save screenshot...", nullptr, false, host.booted()))
        saver.open("Save screenshot", host.romDir, "screenshot.bmp", [this](const std::string& p) { host.saveScreenshotBmp(p); });
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
    if (ImGui::BeginMenu("Monitor")) { monitorSetItems(true); ImGui::EndMenu(); }
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
    ImGui::EndMenu();
}

void GuiShell::menuMedia() {
    if (!ImGui::BeginMenu("Media")) return;
    for (int d = 0; d < 2; d++) {
        char label[32]; std::snprintf(label, sizeof(label), "Drive %c", 'A' + d);
        if (ImGui::BeginMenu(label)) {
            ImGui::TextDisabled("%s", host.diskName[d].empty() ? "(empty)" : host.diskName[d].c_str());
            if (ImGui::MenuItem("Insert...")) browser.open(std::string("Insert disk ") + char('A' + d), host.romDir, { ".dsk", ".edsk" }, [this, d](const std::string& p) { host.loadDiskFile(p, d); });
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
        if (ImGui::MenuItem("Load cartridge (.cpr)...")) browser.open("Load cartridge", host.romDir, { ".cpr", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); });
        if (ImGui::MenuItem("Eject", nullptr, false, !host.cartName.empty())) host.ejectCartridge();
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
    ImGui::EndMenu();
}

void GuiShell::menuTools() {
    if (!ImGui::BeginMenu("Tools")) return;
    ImGui::MenuItem("DSK editor", nullptr, &panelOpen("DSK editor"));
    ImGui::MenuItem("CSL scripts", nullptr, &panelOpen("CSL scripts"));
    ImGui::MenuItem("Printer output", nullptr, &panelOpen("Printer"));
    ImGui::MenuItem("GFX9000", nullptr, &panelOpen("GFX9000"));
    ImGui::Separator();
    ImGui::MenuItem("Assembler", nullptr, &panelOpen("Assembler"));
    if (ImGui::MenuItem("Assemble", "F9")) { panelOpen("Assembler") = true; assembler->assemble(false); }
    if (ImGui::MenuItem("Assemble and run", "Ctrl+F9")) { panelOpen("Assembler") = true; assembler->assemble(true); }
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
    for (const char* w : { "Debugger", "Chips", "Memory", "GFX9000", "Assembler" })
        ImGui::MenuItem(w, nullptr, &panelOpen(w));
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
    ImGui::Separator();
    for (const char* w : { "Machine", "Media", "Settings" }) ImGui::MenuItem(w, nullptr, &panelOpen(w));
    ImGui::Separator();
    for (const char* w : { "Debugger", "Chips", "Memory", "GFX9000" }) ImGui::MenuItem(w, nullptr, &panelOpen(w));
    ImGui::Separator();
    for (const char* w : { "Assembler", "DSK editor", "CSL scripts", "Printer" }) ImGui::MenuItem(w, nullptr, &panelOpen(w));
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
            ImGui::TextUnformatted("Monitor"); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1);
            {
                const MonitorModel* set = host.monitorSetId.empty() ? nullptr : monitorModelFor(host.monitorSetId);
                if (ImGui::BeginCombo("##machinemonitor", set ? set->name : "As shipped with the machine")) { monitorSetItems(false); ImGui::EndCombo(); }
            }
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
        }
        if (ImGui::CollapsingHeader("ROMs")) sectionRoms(false);
    }
    ImGui::End();
}

void GuiShell::windowMedia() {
    if (!panelOpen("Media")) return;
    if (ImGui::Begin("Media", &panelOpen("Media"))) {
        WrapText wrapText;                     // text wraps at the window's edge
        // Every slot the same way: what is in it (wrapping), then its buttons (a row that
        // wraps), Eject among them.
        auto loaded = [](const std::string& name, const char* none) {
            if (name.empty()) textWrappedDisabled("%s", none);
            else { ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(name.c_str()); ImGui::PopTextWrapPos(); }
        };
        if (ImGui::CollapsingHeader("Disk", ImGuiTreeNodeFlags_DefaultOpen)) {
            for (int d = 0; d < 2; d++) {
                ImGui::PushID(d);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(kAccent, "%c:", 'A' + d);
                ImGui::SameLine();
                loaded(host.diskName[d], "(empty)");
                FlowRow row;
                if (row.smallButton("Insert..."))
                    browser.open(std::string("Insert disk ") + char('A' + d), host.romDir, { ".dsk", ".edsk" }, [this, d](const std::string& p) { host.loadDiskFile(p, d); });
                ImGui::BeginDisabled(host.diskName[d].empty());
                if (row.smallButton("Eject")) host.ejectDisk(d);
                if (row.smallButton("Edit")) { dskEditor->takeFromDrive(d); panelOpen("DSK editor") = true; }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Open the disc in the DSK editor");
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            textWrappedDisabled("Drop a .DSK or .EDSK onto the window");
        }
        if (ImGui::CollapsingHeader("Tape", ImGuiTreeNodeFlags_DefaultOpen)) {
            loaded(host.tapeName, "(no tape)");
            if (ImGui::SmallButton("Insert tape...")) browser.open("Insert tape", host.romDir, { ".cdt", ".tzx", ".tap", ".wav" }, [this](const std::string& p) { host.loadTapeFile(p); });
            tapeDeckControls(false);
            ImGui::PushID("tapeopts"); sectionTape(false); ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Cartridge")) {
            loaded(host.cartName, "(no cartridge)");
            FlowRow row;
            if (row.smallButton("Load .CPR..."))
                browser.open("Load cartridge", host.romDir, { ".cpr", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); });
            ImGui::BeginDisabled(host.cartName.empty());
            if (row.smallButton("Eject")) host.ejectCartridge();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Out of the slot: the machine boots without it (a Plus on its system cartridge)");
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader("Snapshot")) {
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
        }
    }
    ImGui::End();
}

void GuiShell::windowSettings() {
    if (!panelOpen("Settings")) return;
    if (ImGui::Begin("Settings", &panelOpen("Settings"))) {
        WrapText wrapText;                     // text wraps at the window's edge
        if (ImGui::CollapsingHeader("Video", ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::PushID("video"); sectionVideo(false); ImGui::PopID(); }
        if (ImGui::CollapsingHeader("Audio")) { ImGui::PushID("audio"); sectionAudio(false); ImGui::PopID(); }
        if (ImGui::CollapsingHeader("Input")) { ImGui::PushID("input"); sectionInput(false); ImGui::PopID(); }
        if (ImGui::CollapsingHeader("Expansions")) { ImGui::PushID("exp"); sectionExpansions(false); ImGui::PopID(); }
        if (ImGui::CollapsingHeader("External debugging")) { ImGui::PushID("dev"); sectionDevelopment(false); ImGui::PopID(); }
    }
    ImGui::End();
}

void GuiShell::windowAbout() {
    if (!showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About CPCSyntaxError", &showAbout, ImGuiWindowFlags_NoDocking)) {
        WrapText wrapText;                     // text wraps at the window's edge
        ImGui::TextColored(kAccent, "CPCSyntaxError");
        ImGui::TextDisabled("Amstrad CPC / Plus / GX4000 emulator");
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
