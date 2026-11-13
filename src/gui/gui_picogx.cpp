// CPCSyntaxError: the Cartridge window's PicoGX part (core/picogx.h): the cartridge's SD
// card as a folder on this computer, browsed the way the PicoGX's own menu shows it, and a
// file from it launched as the PicoGX would flash it.
//
// The PicoGX's README (github.com/Neo2003/PicoGX) for its menu: folders of any depth, only
// folders with a cartridge somewhere inside are shown, the last cartridge loaded is at the
// top, and a .txt file beside a cartridge (same name) is its information text. PicoGX.cfg
// at the card's root may name an AutoFlash cartridge ([Special] AutoFlash=Dev/Game.cpr).
#include "gui_shell.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "emuhost.h"
#include "gui_widgets.h"

namespace cpcse {

namespace {

namespace fs = std::filesystem;

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().u8string();
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool cartridgeFile(const fs::path& p) {
    const std::string e = lowerExt(p);
    return e == ".cpr" || e == ".bin" || e == ".dsk" || e == ".cro";
}

// "The PicoGX will display only folders with one or more CPR or BIN file inside at any
// depth" (a DSK or CRO since firmware 1.2.0 too).
bool holdsCartridge(const fs::path& dir, int depth = 0) {
    std::error_code ec;
    if (depth > 12) return false;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && cartridgeFile(it->path())) return true;
        if (it->is_directory(ec) && holdsCartridge(it->path(), depth + 1)) return true;
    }
    return false;
}

std::string relativeTo(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    return fs::relative(p, root, ec).generic_u8string();
}

} // namespace

void GuiShell::picoGxPanel() {
    ImGui::Separator();
    sectionHeading("PicoGX (SD card cartridge)");
    bool enabled = host.picoGxEnabled;
    if (checkboxWrapped("Use a PicoGX", &enabled)) host.setPicoGxEnabled(enabled);
    if (!host.picoGxEnabled) {
        textWrappedDisabled("Off: the machine has no PicoGX. Turn it on to choose its SD card folder and launch from it.");
        return;
    }
    FlowRow row;
    if (row.smallButton(host.picoGxSd.empty() ? "Choose its SD card folder..." : "SD card folder..."))
        browser.openDir("PicoGX SD card folder", host.picoGxSd.empty() ? host.romDir : host.picoGxSd,
                        [this](const std::string& dir) { host.picoGxSd = dir; picoGxDir.clear(); picoGxPick.clear(); picoGxInfo.clear(); });
    if (host.picoGxSd.empty()) {
        textWrappedDisabled("The PicoGX loads .CPR, .BIN, .DSK and .CRO files from its SD card. Choose a folder to be the card.");
        return;
    }
    const fs::path root = fs::u8path(host.picoGxSd);
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { textWrappedDisabled("The SD card folder is not there: %s", host.picoGxSd.c_str()); return; }
    textWrappedDisabled("Card: %s", host.picoGxSd.c_str());

    // The PicoGX's own menu: it is inside its firmware, which is not shipped here. The user's
    // own .uf2 file (from the PicoGX releases) gives it.
    {
        FlowRow fw;
        if (fw.smallButton(host.picoGxFirmware.empty() ? "Firmware (.uf2)..." : "Firmware..."))
            browser.open("PicoGX firmware", host.picoGxFirmware.empty() ? host.picoGxSd : fs::u8path(host.picoGxFirmware).parent_path().u8string(),
                         { ".uf2" }, [this](const std::string& p) { host.picoGxFirmware = p; });
        ImGui::BeginDisabled(host.picoGxFirmware.empty());
        if (fw.smallButton("Start the PicoGX menu")) host.startPicoGxMenu();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The PicoGX's own menu on the CPC screen, taken from its firmware file\n"
                              "(github.com/Neo2003/PicoGX releases). Choose the firmware first.");
        if (!host.picoGxFirmware.empty())
            textWrappedDisabled("Firmware: %s", fs::u8path(host.picoGxFirmware).filename().u8string().c_str());
    }

    if (host.picoGxActive()) {
        const PicoGX& p = *host.picoGx;
        const char* mode = p.mode == PicoGX::SAVE_READ ? "reading its save" : p.mode == PicoGX::SAVE_WRITE ? "writing its save" : "cartridge";
        if (p.large()) ImGui::TextDisabled("In the slot: %d KB, block %d of %d, %s", (int)p.pages.size() * 16, p.block + 1,
                                           ((int)p.pages.size() + PicoGX::BLOCK_PAGES - 1) / PicoGX::BLOCK_PAGES, mode);
        else ImGui::TextDisabled("In the slot: %d KB, %s", (int)p.pages.size() * 16, mode);
        if (!p.lastEvent.empty()) textWrappedDisabled("Last: %s", p.lastEvent.c_str());
    }

    // The card's AutoFlash (developer mode) and the last cartridge, first, as the menu has it.
    const std::string autoFlash = host.picoGxConfig("Special", "AutoFlash");
    FlowRow top;
    if (!autoFlash.empty() && top.smallButton(("AutoFlash: " + autoFlash).c_str())) host.launchPicoGx(autoFlash);
    if (!host.picoGxLast.empty() && fs::exists(root / fs::u8path(host.picoGxLast), ec)) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        if (top.smallButton(("Last: " + fs::u8path(host.picoGxLast).filename().u8string()).c_str())) host.launchPicoGx(host.picoGxLast);
        ImGui::PopStyleColor(2);
    }

    // The folder shown: its sub-folders (those with a cartridge inside) and its files.
    fs::path dir = root / fs::u8path(picoGxDir);
    if (!fs::is_directory(dir, ec)) { picoGxDir.clear(); dir = root; }
    std::vector<fs::path> folders, files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().u8string();
        if (name.empty() || name[0] == '.') continue;
        if (it->is_directory(ec)) { if (holdsCartridge(it->path())) folders.push_back(it->path()); }
        else if (it->is_regular_file(ec) && cartridgeFile(it->path())) files.push_back(it->path());
    }
    auto byName = [](const fs::path& a, const fs::path& b) {
        std::string x = a.filename().u8string(), y = b.filename().u8string();
        for (char& c : x) c = (char)std::tolower((unsigned char)c);
        for (char& c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    ImGui::TextDisabled("/%s  (%d folder%s, %d file%s)", picoGxDir.c_str(), (int)folders.size(), folders.size() == 1 ? "" : "s",
                        (int)files.size(), files.size() == 1 ? "" : "s");
    const float listHeight = ImGui::GetTextLineHeightWithSpacing() * 12;
    if (ImGui::BeginChild("##picogx_list", ImVec2(0, listHeight), true)) {
        if (!picoGxDir.empty() && ImGui::Selectable("..")) {
            picoGxDir = fs::u8path(picoGxDir).parent_path().generic_u8string();
            picoGxPick.clear(); picoGxInfo.clear();
        }
        for (const fs::path& f : folders) {
            if (ImGui::Selectable(("[" + f.filename().u8string() + "]").c_str())) {
                picoGxDir = relativeTo(root, f);
                picoGxPick.clear(); picoGxInfo.clear();
            }
        }
        for (const fs::path& f : files) {
            const std::string rel = relativeTo(root, f);
            fs::path txt = f;
            txt.replace_extension(".txt");
            const bool hasInfo = fs::exists(txt, ec);
            const std::string label = (hasInfo ? "(i) " : "    ") + f.filename().u8string();
            if (ImGui::Selectable(label.c_str(), picoGxPick == rel, ImGuiSelectableFlags_AllowDoubleClick)) {
                picoGxPick = rel;
                picoGxInfo.clear();
                if (hasInfo) {
                    std::ifstream t(txt, std::ios::binary);
                    std::string s((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
                    picoGxInfo = s.size() > 4000 ? s.substr(0, 4000) : s;
                }
                if (ImGui::IsMouseDoubleClicked(0)) host.launchPicoGx(rel);
            }
        }
    }
    ImGui::EndChild();
    FlowRow act;
    ImGui::BeginDisabled(picoGxPick.empty());
    if (act.smallButton("Launch")) host.launchPicoGx(picoGxPick);
    ImGui::EndDisabled();
    if (!picoGxInfo.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", picoGxInfo.c_str());
    }
    textWrappedDisabled("Software can use the PicoGX's 16 KB save (saves/<name>.SAV on the card) and cartridges up to 1.5 MB, "
                        "as its Programming.md describes.");
}

} // namespace cpcse
