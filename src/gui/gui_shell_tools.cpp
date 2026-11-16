// CPCSyntaxError: three shell tools (user 2026-11-17, "add em all").
//   The command palette (Ctrl+Shift+P): type part of a command, Enter runs it.
//   The screen to the clipboard (Ctrl+Shift+C): the CPC's picture as an image (Windows); on
//     other systems a PNG in the recordings folder, its path on the clipboard.
//   The keyboard shortcuts window (Help > Keyboard shortcuts): every key, by where it works.
#include "gui_shell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>

#include <SDL.h>

#include "imgui.h"
#include "gui_assembler.h"
#include "gui_widgets.h"
#include "core/png.h"
#include "core/video.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cpcse {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
}

// ---------------------------------------------------------------- the screen to the clipboard
bool GuiShell::copyScreenToClipboard() {
    if (!host.booted() || !host.video || host.video->pixels.empty()) { host.status = "No picture to copy"; return false; }
    const int w = host.video->width, h = host.video->height;
    const std::vector<uint32_t>& px = host.video->pixels;     // 0xAABBGGRR
#ifdef _WIN32
    // CF_DIB: a BITMAPINFOHEADER, then 32-bit BGRX rows, bottom row first
    const size_t bytes = sizeof(BITMAPINFOHEADER) + (size_t)w * h * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) { host.status = "Not copied: no memory for the picture"; return false; }
    auto* p = (uint8_t*)GlobalLock(mem);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = w; bih.biHeight = h; bih.biPlanes = 1; bih.biBitCount = 32;
    bih.biCompression = BI_RGB; bih.biSizeImage = (DWORD)((size_t)w * h * 4);
    std::memcpy(p, &bih, sizeof(bih));
    uint8_t* d = p + sizeof(bih);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t c = px[(size_t)(h - 1 - y) * w + x];
            *d++ = (uint8_t)(c >> 16); *d++ = (uint8_t)(c >> 8); *d++ = (uint8_t)c; *d++ = 0xff;
        }
    GlobalUnlock(mem);
    if (!OpenClipboard(nullptr)) { GlobalFree(mem); host.status = "Not copied: the clipboard is in use"; return false; }
    EmptyClipboard();
    const bool ok = SetClipboardData(CF_DIB, mem) != nullptr;
    CloseClipboard();
    if (!ok) { GlobalFree(mem); host.status = "Not copied: the clipboard refused the picture"; return false; }
    host.status = "The screen is on the clipboard (" + std::to_string(w) + " x " + std::to_string(h) + "): paste it";
    return true;
#else
    // no image clipboard SDL can reach here: a PNG file, and its path on the clipboard
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(host.recordDir), ec);
    char name[64];
    std::snprintf(name, sizeof name, "screen-%lld.png", (long long)std::time(nullptr));
    const std::string path = (std::filesystem::u8path(host.recordDir) / name).u8string();
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; i++) { rgb[i * 3] = (uint8_t)px[i]; rgb[i * 3 + 1] = (uint8_t)(px[i] >> 8); rgb[i * 3 + 2] = (uint8_t)(px[i] >> 16); }
    if (!writePng(path, rgb.data(), w, h)) { host.status = "Not copied: could not write " + path; return false; }
    SDL_SetClipboardText(path.c_str());
    host.status = "The screen is in " + path + " (its path is on the clipboard)";
    return true;
#endif
}

// ---------------------------------------------------------------- the command palette
std::vector<GuiShell::PaletteCommand> GuiShell::paletteCommands() {
    std::vector<PaletteCommand> c;
    auto add = [&](std::string name, std::string keys, std::function<void()> run) { c.push_back({ std::move(name), std::move(keys), std::move(run) }); };
    // the machine
    add("Machine: reset", "Ctrl+R", [this] { host.reset(); });
    add("Machine: pause or run", "Ctrl+P", [this] { if (host.paused) debugger.run(); else debugger.pause(); });
    add("Machine: unlimited speed on or off", "", [this] { host.turbo = !host.turbo; });
    add("View: full screen", "F11", [this] { if (toggleFullscreen) toggleFullscreen(); });
    for (int i = 0; i < (int)host.models.size(); i++)
        if (host.models[(size_t)i].available) add("Machine: start a " + host.models[(size_t)i].label, "", [this, i] { host.bootModel(i); });
    for (const auto& prof : host.machineProfiles()) { const std::string p = prof.second; add("Machine profile: " + prof.first, "", [this, p] { host.openMachineProfile(p); }); }
    // media
    for (int d = 0; d < 2; d++) {
        const std::string drive(1, (char)('A' + d));
        add("Disc: insert into drive " + drive + "...", "", [this, d, drive] { browser.open("Insert disk " + drive, host.romDir, { ".dsk", ".edsk", ".hfe", ".ipf" }, [this, d](const std::string& p) { host.loadDiskFile(p, d); }); });
        add("Disc: eject drive " + drive, "", [this, d] { host.ejectDisk(d); });
    }
    add("Disc: turbo loading on or off", "", [this] { host.discTurbo = host.discTurbo > 1 ? 1 : EmuHost::TAPE_TURBO_UNLIMITED; });
    add("Disc: instant loading on or off", "", [this] { host.discInstantLoad = !host.discInstantLoad; host.applyTapeOptions(); });
    add("Tape: insert...", "", [this] { browser.open("Insert tape", host.romDir, { ".cdt", ".tzx", ".tap", ".wav" }, [this](const std::string& p) { host.loadTapeFile(p); }); });
    add("Tape: play", "", [this] { host.tapePlay(); });
    add("Tape: stop", "", [this] { host.tapeStop(); });
    add("Tape: rewind to the start", "", [this] { host.tapeRewind(); });
    add("Tape: turbo loading on or off", "", [this] { host.tapeTurbo = host.tapeTurbo > 1 ? 1 : EmuHost::TAPE_TURBO_UNLIMITED; });
    add("Cartridge: load...", "", [this] { browser.open("Load cartridge", host.romDir, { ".cpr", ".cro", ".bin" }, [this](const std::string& p) { host.loadCartridgeFile(p); }); });
    add("Snapshot: load...", "", [this] { browser.open("Load snapshot", host.romDir, { ".sna" }, [this](const std::string& p) { host.loadSnapshot(p); }); });
    add("Snapshot: save...", "", [this] { saver.open("Save snapshot", host.romDir, "snapshot.sna", [this](const std::string& p) { host.saveSnapshot(p); }); });
    for (int s = 1; s <= EmuHost::SLOT_COUNT; s++) {
        add("Quick save to slot " + std::to_string(s), "Ctrl+Shift+" + std::to_string(s), [this, s] { host.saveSlot(s); });
        add("Quick load slot " + std::to_string(s), "Ctrl+" + std::to_string(s), [this, s] { host.loadSlot(s); });
    }
    add("Screen: copy to the clipboard", "Ctrl+Shift+C", [this] { copyScreenToClipboard(); });
    add("Screen: save a screenshot...", "", [this] { saver.open("Save screenshot", host.romDir, "screenshot.bmp", [this](const std::string& p) { host.saveScreenshotBmp(p); }); });
    // the debugger
    add("Debugger: step into", "F7", [this] { debugger.stepInto(); });
    add("Debugger: step over", "F8", [this] { debugger.stepOver(); });
    add("Debugger: step out", "Shift+F8", [this] { debugger.stepOut(); });
    add("Debugger: export the disassembly as .asm...", "", [this] {
        panelOpen("Debugger") = true;
        focusWindowNextFrame = "Debugger";
        std::snprintf(exportFrom, sizeof exportFrom, "%04X", disasmTop & 0xffff);
        std::snprintf(exportTo, sizeof exportTo, "%04X", (disasmTop + 0xff) & 0xffff);
        exportOpen = true;
    });
    add("Debugger: watches (add a watch)", "", [this] { panelOpen("Watches") = true; focusWindowNextFrame = "Watches"; });
    add("Memory: search for bytes, text or a word", "", [this] { panelOpen("Memory") = true; focusWindowNextFrame = "Memory"; memTabRequest = 2; });
    add("PSG: channel scopes and mute", "", [this] { panelOpen("Chips") = true; focusWindowNextFrame = "Chips"; });
    add("Assembler: find", "Ctrl+F", [this] { panelOpen("Assembler") = true; focusWindowNextFrame = "Assembler"; assembler->openFind(false); });
    add("Assembler: find and replace", "Ctrl+H", [this] { panelOpen("Assembler") = true; focusWindowNextFrame = "Assembler"; assembler->openFind(true); });
    add("Assembler: assemble", "F9", [this] { panelOpen("Assembler") = true; assembler->assemble(false); });
    add("Assembler: assemble and run", "Ctrl+F9", [this] { panelOpen("Assembler") = true; assembler->assemble(true); });
    // every window
    for (const Panel& p : panels) {
        const char* t = p.title;
        add(std::string("Window: ") + t, "", [this, t] { panelOpen(t) = true; focusWindowNextFrame = t; });
    }
    add("Window: all settings", "", [this] { openAllSettings(); });
    add("Help: keyboard shortcuts", "", [this] { showShortcuts = true; });
    add("Help: about CPCSyntaxError", "", [this] { showAbout = true; });
    return c;
}

// A command matches when each word typed is in its name (or its keys), in any order.
static bool paletteMatches(const std::string& name, const std::string& keys, const std::string& filter) {
    const std::string n = lower(name + " " + keys);
    std::istringstream words(lower(filter));
    for (std::string w; words >> w;) if (n.find(w) == std::string::npos) return false;
    return true;
}

void GuiShell::commandPalette() {
    if (!paletteOpen) return;
    const char* id = "##commandpalette";
    if (!ImGui::IsPopupOpen(id)) {
        ImGui::OpenPopup(id);
        paletteFilter[0] = 0;
        paletteSelected = 0;
        paletteList = paletteCommands();
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(620.0f * uiScale, vp->WorkSize.x - 40.0f);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - width) * 0.5f, vp->WorkPos.y + 40.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    int run = -1;
    if (ImGui::BeginPopup(id, ImGuiWindowFlags_NoMove)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##paletteinput", "a command: type part of its name (Enter runs it, Esc closes)", paletteFilter, sizeof(paletteFilter)))
            paletteSelected = 0;
        std::vector<int> shown;
        for (int i = 0; i < (int)paletteList.size(); i++) if (paletteMatches(paletteList[(size_t)i].name, paletteList[(size_t)i].keys, paletteFilter)) shown.push_back(i);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) paletteSelected = std::min(paletteSelected + 1, (int)shown.size() - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) paletteSelected = std::max(paletteSelected - 1, 0);
        paletteSelected = std::clamp(paletteSelected, 0, std::max(0, (int)shown.size() - 1));
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) if (!shown.empty()) run = shown[(size_t)paletteSelected];
        const float rows = std::min(14.0f, (float)std::max<size_t>(1, shown.size()));
        ImGui::BeginChild("##palettelist", ImVec2(0, rows * ImGui::GetTextLineHeightWithSpacing() + 8));
        for (int k = 0; k < (int)shown.size(); k++) {
            const PaletteCommand& pc = paletteList[(size_t)shown[(size_t)k]];
            ImGui::PushID(k);
            if (ImGui::Selectable(pc.name.c_str(), k == paletteSelected)) run = shown[(size_t)k];
            if (k == paletteSelected && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) ImGui::SetScrollHereY();
            if (!pc.keys.empty()) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(pc.keys.c_str()).x);
                ImGui::TextDisabled("%s", pc.keys.c_str());
            }
            ImGui::PopID();
        }
        if (shown.empty()) ImGui::TextDisabled("No command has those words.");
        ImGui::EndChild();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        if (run >= 0) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!ImGui::IsPopupOpen(id)) paletteOpen = false;      // closed: Esc, a click outside, or a command
    if (run >= 0 && run < (int)paletteList.size()) {
        const std::function<void()> f = paletteList[(size_t)run].run;   // (the list may change in it)
        host.status = "Command: " + paletteList[(size_t)run].name;
        f();
    }
}

// ---------------------------------------------------------------- the keyboard shortcuts
void GuiShell::windowShortcuts() {
    if (!showShortcuts) return;
    ImGui::SetNextWindowSize(ImVec2(560 * uiScale, 560 * uiScale), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard shortcuts", &showShortcuts, ImGuiWindowFlags_NoDocking)) { ImGui::End(); return; }
    WrapText wrapText;
    struct Key { const char* keys; const char* what; };
    auto table = [&](const char* heading, const char* note, std::initializer_list<Key> keys) {
        sectionHeading(heading);
        if (note) textWrappedDisabled("%s", note);
        if (ImGui::BeginTable(heading, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Ctrl+Shift+1..9  ").x);
            ImGui::TableSetupColumn("what");
            for (const Key& k : keys) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(kAccent, "%s", k.keys);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", k.what);
            }
            ImGui::EndTable();
        }
    };
    table("EVERYWHERE", "Keys go to the CPC, except these, and except while a debugger, assembler or other tool window has the focus.", {
        { "Ctrl+Shift+P", "the command palette: any command by its name" },
        { "Ctrl+Shift+C", "copy the CPC's screen to the clipboard" },
        { "F11", "full screen (or double-click the screen)" },
        { "F12", "let the mouse go (when the CPC has it: a SYMBiFACE or mouse interface)" },
        { "F10", "the Multiface II's STOP button (when it is fitted)" },
        { "Ctrl+R", "reset the machine" },
        { "Ctrl+P", "pause or run" },
        { "Ctrl+1..9", "load quick save slot 1 to 9" },
        { "Ctrl+Shift+1..9", "save to quick save slot 1 to 9" },
        { "Ctrl+Left", "rewind a second (when the timeline is on)" },
    });
    table("DEBUGGER", "In a debugger window, or while the machine is paused.", {
        { "F5", "run or pause" }, { "F7", "step into" }, { "F8", "step over" }, { "Shift+F8", "step out" },
        { "Shift+F7", "step back (timeline on)" }, { "Shift+F5", "run back to the last stop (timeline on)" },
    });
    table("ASSEMBLER AND C", nullptr, {
        { "F9", "assemble (or build) into the machine's memory" }, { "Ctrl+F9", "assemble (or build) and run" },
        { "Ctrl+F10", "run to the cursor's line" }, { "F10 / F11", "C: next line over / into calls" }, { "Shift+F11", "C: run until the function returns" },
        { "Ctrl+S / Ctrl+Shift+S", "save / save as" }, { "Ctrl+O", "open" },
        { "Ctrl+Z", "undo" }, { "Ctrl+Y or Ctrl+Shift+Z", "redo" },
        { "Ctrl+F", "find (the selected word goes in the box)" }, { "Ctrl+H", "find and replace" },
        { "F3 / Shift+F3", "the next / the match before (Enter / Shift+Enter in the box too; Esc closes it)" },
        { "F12 or Ctrl+click", "go to the definition of the name under the cursor (the Labels tab lists them)" },
        { "Ctrl+/", "a comment on or off for the selected lines (; or //)" },
        { "Tab / Shift+Tab", "move the selected lines in / out (Shift+Tab: also the cursor's line)" },
    });
    table("GRAPHICS FINDER", nullptr, {
        { "Left drag", "draw with the chosen pen and tool: Pen, Line, Box, Filled box, Fill (Draw on)" },
        { "Right click", "take the pen of a pixel" }, { "Right click a pen", "change its ink" },
        { "Ctrl+Z / Ctrl+Y", "undo / redo a stroke, a shape, a fill, a paste or an import" },
        { "Mouse wheel", "move a row of tiles (Shift: a byte; Ctrl: zoom)" },
    });
    table("BASIC", nullptr, { { "Ctrl+F / Ctrl+H", "find / find and replace" }, { "F3 / Shift+F3", "the next / the match before" } });
    ImGui::End();
}

} // namespace cpcse
