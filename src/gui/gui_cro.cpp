// CPCSyntaxError GUI — the CRO window: a selector for the groups of the CRO in the machine, and
// an editor and builder of CRO files (core/cpr_loader.h: parseCro, serializeCro).
//
// A CRO (github.com/Logon-System/CRO-Format) is a RIFF file of groups (GRRO), each a number
// (GNUM), a label (GLBL), an address mask (GMSK) and ROMs, each with a name (RID), a type
// (RTYP: 0 low, 1 high, 2 bankable, 3 Multiface II), a logical number (RLOG: the upper ROM
// number for a high ROM), a physical number (RPHY: the page for a bankable one) and its bytes.
// A group of bankable ROMs is a Plus cartridge; a group of low and high ROMs is a CPC's ROM set.
#include "gui_shell.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "core/dskfs.h"

namespace cpcse {

namespace {
const char* kRomTypes[] = { "Low (the OS)", "High (upper ROM)", "Bankable (cartridge page)", "Multiface II" };
const char* kShortTypes[] = { "low", "high", "page", "MF2" };
// Logon-System's CRO manager offers these masks
const uint32_t kMasks[] = { 0xffffffff, 0x3fff, 0x7fff, 0xffff, 0x1ffff, 0x3ffff, 0x7ffff };
const char* kMaskNames[] = { "&FFFFFFFF (none)", "&00003FFF (16K)", "&00007FFF (32K)", "&0000FFFF (64K)",
                             "&0001FFFF (128K)", "&0003FFFF (256K)", "&0007FFFF (512K)" };

bool readAll(const std::string& path, Bytes& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
std::string stem(const std::string& path) { return std::filesystem::u8path(path).stem().u8string(); }
std::string fileName(const std::string& path) { return std::filesystem::u8path(path).filename().u8string(); }

// A list of `count` lines (up to 6 shown) in a child window with a frame: a long line scrolls
// in it, not across the window. EndChild() follows, whatever this returns.
bool listBox(const char* id, size_t count) {
    const float lines = (float)std::clamp<size_t>(count, 1, 6);
    return ImGui::BeginChild(id, ImVec2(0, lines * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2),
                             ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
}
// A field's label on its own line, the field the window's width under it.
void fieldLabel(const char* label) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
}

void GuiShell::croOpen(const std::string& path) {
    Bytes data;
    if (!readAll(path, data)) { cro.message = "Could not read " + path; return; }
    try {
        cro.groups = parseCro(data);
        cro.path = path;
        cro.group = cro.groups.empty() ? -1 : 0;
        cro.rom = -1;
        cro.dirty = false;
        cro.message = "Opened " + fileName(path);
    } catch (const std::exception& ex) { cro.message = std::string("Not a CRO file: ") + ex.what(); }
}

void GuiShell::croSave(const std::string& path) {
    const Bytes data = serializeCro(cro.groups);
    std::string why;
    if (!writeFileSafely(path, data.data(), data.size(), &why)) { cro.message = why; return; }
    cro.path = path;
    cro.dirty = false;
    cro.message = "Saved " + fileName(path) + " (" + std::to_string(data.size()) + " bytes)";
}

// A ROM file into the group: a cartridge group gets it as pages (16K each, on the next free
// pages); a CPC group gets it as a high ROM on the lowest free upper ROM number.
void GuiShell::croAddRomFile(const std::string& path) {
    if (cro.group < 0 || cro.group >= (int)cro.groups.size()) return;
    Bytes data;
    if (!readAll(path, data) || data.empty()) { cro.message = "Could not read " + path; return; }
    if (hasAmsdosHeader(data)) data.erase(data.begin(), data.begin() + 128);   // a ROM saved from a CPC
    CroGroup& g = cro.groups[(size_t)cro.group];
    const std::string name = stem(path).substr(0, 16);   // Logon-System's manager keeps 16 characters
    if (g.hasBankable()) {
        std::vector<bool> used(32, false);
        for (const CroRom& r : g.roms) if (r.type == CRO_ROM_BANKABLE && r.physical < 32) used[r.physical] = true;
        int added = 0;
        for (size_t at = 0; at < data.size(); at += 16384) {
            int page = 0;
            while (page < 32 && used[(size_t)page]) page++;
            if (page == 32) { cro.message = "The cartridge's 32 pages are full."; break; }
            used[(size_t)page] = true;
            CroRom r;
            r.id = (data.size() > 16384 ? name.substr(0, 13) + "+" + std::to_string(at / 16384) : name);
            r.type = CRO_ROM_BANKABLE;
            r.physical = (uint32_t)page;
            r.logical = page == 3 ? 7 : 1;
            r.data.assign(data.begin() + (long)at, data.begin() + (long)std::min(data.size(), at + 16384));
            g.roms.push_back(std::move(r));
            added++;
        }
        if (added) cro.message = "Added " + name + " as " + std::to_string(added) + " cartridge page(s)";
    } else {
        std::vector<bool> used(256, false);
        for (const CroRom& r : g.roms) if (r.type == CRO_ROM_HIGH && r.logical < 256) used[r.logical] = true;
        int number = 1;
        while (number < 256 && used[(size_t)number]) number++;
        CroRom r;
        r.id = name;
        r.type = CRO_ROM_HIGH;
        r.logical = (uint32_t)(number < 256 ? number : 255);
        r.data = std::move(data);
        g.roms.push_back(std::move(r));
        cro.message = "Added " + name + " as upper ROM " + std::to_string(number);
    }
    cro.rom = (int)g.roms.size() - 1;
    cro.dirty = true;
}

// A cartridge (CPR or raw) as a new group: its pages, a mask that repeats them as the chip would.
void GuiShell::croImportCartridge(const std::string& path) {
    Bytes data;
    if (!readAll(path, data)) { cro.message = "Could not read " + path; return; }
    try {
        CroGroup g;
        g.roms = croRomsFromCartridge(data);
        uint32_t top = 0;
        for (const CroRom& r : g.roms) top = std::max(top, r.physical);
        g.mask = croMaskForPages((int)top + 1);
        g.label = fileName(path);
        uint32_t number = 0;
        for (const CroGroup& o : cro.groups) number = std::max(number, o.number + 1);
        g.number = number;
        cro.groups.push_back(std::move(g));
        cro.group = (int)cro.groups.size() - 1;
        cro.rom = -1;
        cro.dirty = true;
        cro.message = "Imported " + fileName(path) + " as group " + std::to_string(number);
    } catch (const std::exception& ex) { cro.message = ex.what(); }
}

// Runs a group of what the editor holds, saved or not.
void GuiShell::croRun(int index) {
    if (index < 0 || index >= (int)cro.groups.size()) return;
    host.croGroup = (int)cro.groups[(size_t)index].number;
    const std::string path = cro.dirty ? std::string() : cro.path;   // the file on disc only when it is what is here
    host.croGroupPath = path;
    host.loadCartridgeData(serializeCro(cro.groups), path);
    cro.message = host.status;
}

// "Choose a CRO group": a CRO of several groups was loaded with none picked for it.
void GuiShell::croChooser() {
    if (host.croAskGroup) { host.croAskGroup = false; ImGui::OpenPopup("CRO groups##chooser"); }
    if (ImGui::BeginPopupModal("CRO groups##chooser", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This CRO holds several groups. Which one?");
        for (size_t i = 0; i < host.croGroupLabels.size() && i < host.croGroupNumbers.size(); i++) {
            const bool now = host.croCurrentGroup == host.croGroupNumbers[i];
            if (ImGui::Selectable((host.croGroupLabels[i] + (now ? "  (running)" : "") + "##choose" + std::to_string(i)).c_str(), now)) {
                if (!now) host.loadCroGroup(host.croGroupNumbers[i]);
                else { host.croGroup = (int)host.croGroupNumbers[i]; host.croGroupPath = host.cartPath; }
                ImGui::CloseCurrentPopup();
            }
        }
        if (ImGui::Button("Keep the one running")) {
            host.croGroup = (int)host.croCurrentGroup;   // asked once a file
            host.croGroupPath = host.cartPath;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GuiShell::windowCroEditor() {
    bool& open = panelOpen("CRO editor");
    if (!open) return;
    if (!ImGui::Begin("CRO editor", &open)) { ImGui::End(); return; }
    croEditorBody();
    ImGui::End();
}

void GuiShell::croEditorBody() {
    WrapText wrapText;

    // ---- the selector: the groups of the CRO in the machine
    sectionHeading("In the machine");
    if (host.croGroupLabels.empty()) ImGui::TextDisabled("No CRO is loaded (Media: cartridge, a .cro file).");
    else {
        if (listBox("##running", host.croGroupLabels.size())) {
            for (size_t i = 0; i < host.croGroupLabels.size() && i < host.croGroupNumbers.size(); i++) {
                const bool now = host.croCurrentGroup == host.croGroupNumbers[i];
                if (ImGui::Selectable((host.croGroupLabels[i] + (now ? "  (running)" : "") + "##run" + std::to_string(i)).c_str(), now) && !now)
                    host.loadCroGroup(host.croGroupNumbers[i]);
            }
        }
        ImGui::EndChild();
        if (!host.cartPath.empty() && FlowRow().smallButton("Edit this file")) croOpen(host.cartPath);
    }

    // ---- the file
    sectionHeading("Editor");
    {
        FlowRow row;
        if (row.smallButton("New")) { cro = CroEditor{}; cro.groups.push_back(CroGroup{}); cro.groups[0].label = "Group 0"; cro.group = 0; cro.dirty = true; }
        if (row.smallButton("Open...")) browser.open("Open a CRO", host.mediaDir, { ".cro" }, [this](const std::string& p) { croOpen(p); });
        ImGui::BeginDisabled(cro.groups.empty());
        bool saveAs = false;
        if (row.smallButton("Save")) { if (cro.path.empty()) saveAs = true; else croSave(cro.path); }
        if (row.smallButton("Save as...") || saveAs)
            saver.open("Save the CRO", cro.path.empty() ? host.mediaDir : std::filesystem::u8path(cro.path).parent_path().u8string(),
                       cro.path.empty() ? "new.cro" : fileName(cro.path), [this](const std::string& p) { croSave(p); });
        ImGui::EndDisabled();
    }
    if (cro.path.empty() && !cro.groups.empty()) ImGui::TextDisabled("Not saved yet: Save as... names it.");
    else if (!cro.path.empty()) ImGui::TextDisabled("%s%s", fileName(cro.path).c_str(), cro.dirty ? "  (changed)" : "");

    // ---- groups
    ImGui::TextUnformatted("Groups");
    if (listBox("##groups", cro.groups.size())) {
        for (size_t i = 0; i < cro.groups.size(); i++) {
            const CroGroup& g = cro.groups[i];
            char line[160];
            std::snprintf(line, sizeof line, "%u: %s  (%zu ROM%s, %s)##grp%zu", g.number, g.label.empty() ? "(no label)" : g.label.c_str(),
                          g.roms.size(), g.roms.size() == 1 ? "" : "s", g.hasBankable() ? "cartridge" : "CPC ROMs", i);
            if (ImGui::Selectable(line, cro.group == (int)i)) { cro.group = (int)i; cro.rom = -1; }
        }
    }
    ImGui::EndChild();
    {
        FlowRow row;
        if (row.smallButton("Add group")) {
            CroGroup g;
            for (const CroGroup& o : cro.groups) g.number = std::max(g.number, o.number + 1);
            g.label = "Group " + std::to_string(g.number);
            cro.groups.push_back(g);
            cro.group = (int)cro.groups.size() - 1; cro.rom = -1; cro.dirty = true;
        }
        if (row.smallButton("Import a cartridge..."))
            browser.open("Import a cartridge as a group", host.mediaDir, { ".cpr", ".bin" }, [this](const std::string& p) { croImportCartridge(p); });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A CPR (its pages) or a raw cartridge image, as a new group");
        const bool sel = cro.group >= 0 && cro.group < (int)cro.groups.size();
        ImGui::BeginDisabled(!sel);
        if (row.smallButton("Remove##grp") && sel) { cro.groups.erase(cro.groups.begin() + cro.group); cro.group = std::min(cro.group, (int)cro.groups.size() - 1); cro.rom = -1; cro.dirty = true; }
        if (row.smallButton("Up##grp") && sel && cro.group > 0) { std::swap(cro.groups[(size_t)cro.group], cro.groups[(size_t)cro.group - 1]); cro.group--; cro.dirty = true; }
        if (row.smallButton("Down##grp") && sel && cro.group + 1 < (int)cro.groups.size()) { std::swap(cro.groups[(size_t)cro.group], cro.groups[(size_t)cro.group + 1]); cro.group++; cro.dirty = true; }
        if (row.smallButton("Run##grp") && sel) croRun(cro.group);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("This group in the machine now (what is here, saved or not)");
        ImGui::EndDisabled();
    }

    if (cro.group >= 0 && cro.group < (int)cro.groups.size()) {
        CroGroup& g = cro.groups[(size_t)cro.group];
        ImGui::Separator();
        int number = (int)g.number;
        fieldLabel("Number (GNUM)");
        if (ImGui::InputInt("##gnum", &number)) { g.number = (uint32_t)std::max(0, number); cro.dirty = true; }
        char label[128];
        std::snprintf(label, sizeof label, "%s", g.label.c_str());
        fieldLabel("Label (GLBL)");
        if (ImGui::InputText("##glbl", label, sizeof label)) { g.label = label; cro.dirty = true; }
        int maskIndex = -1;
        for (int k = 0; k < 7; k++) if (kMasks[k] == g.mask) maskIndex = k;
        char other[32];
        std::snprintf(other, sizeof other, "&%08X", g.mask);
        fieldLabel("Mask (GMSK)");
        if (ImGui::BeginCombo("##gmsk", maskIndex >= 0 ? kMaskNames[maskIndex] : other)) {
            for (int k = 0; k < 7; k++) if (ImGui::Selectable(kMaskNames[k], k == maskIndex)) { g.mask = kMasks[k]; cro.dirty = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The cartridge address is ANDed with it: a 64K cartridge with &FFFF repeats\nthrough the 512K, as its ROM chip does; with &FFFFFFFF the pages past it read 0");

        // ---- the group's ROMs
        ImGui::TextUnformatted("ROMs");
        if (listBox("##roms", g.roms.size())) {
            for (size_t i = 0; i < g.roms.size(); i++) {
                const CroRom& r = g.roms[i];
                char line[160];
                const char* type = r.type <= CRO_ROM_MF2 ? kShortTypes[r.type] : "?";
                if (r.type == CRO_ROM_BANKABLE) std::snprintf(line, sizeof line, "%s  page %u  %zuK##rom%zu", r.id.c_str(), r.physical, r.data.size() / 1024, i);
                else if (r.type == CRO_ROM_HIGH) std::snprintf(line, sizeof line, "%s  ROM %u  %zuK##rom%zu", r.id.c_str(), r.logical, r.data.size() / 1024, i);
                else std::snprintf(line, sizeof line, "%s  %s  %zuK##rom%zu", r.id.c_str(), type, r.data.size() / 1024, i);
                if (ImGui::Selectable(line, cro.rom == (int)i)) cro.rom = (int)i;
            }
        }
        ImGui::EndChild();
        {
            FlowRow row;
            if (row.smallButton("Add a ROM file..."))
                browser.open("Add a ROM", host.romDir, { ".rom", ".bin" }, [this](const std::string& p) { croAddRomFile(p); });
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("To a cartridge group: as pages (16K each, the next free).\nTo a CPC group: as an upper ROM (the lowest free number).");
            const bool sel = cro.rom >= 0 && cro.rom < (int)g.roms.size();
            ImGui::BeginDisabled(!sel);
            if (row.smallButton("Remove##rom") && sel) { g.roms.erase(g.roms.begin() + cro.rom); cro.rom = std::min(cro.rom, (int)g.roms.size() - 1); cro.dirty = true; }
            if (row.smallButton("Up##rom") && sel && cro.rom > 0) { std::swap(g.roms[(size_t)cro.rom], g.roms[(size_t)cro.rom - 1]); cro.rom--; cro.dirty = true; }
            if (row.smallButton("Down##rom") && sel && cro.rom + 1 < (int)g.roms.size()) { std::swap(g.roms[(size_t)cro.rom], g.roms[(size_t)cro.rom + 1]); cro.rom++; cro.dirty = true; }
            ImGui::EndDisabled();
        }
        if (cro.rom >= 0 && cro.rom < (int)g.roms.size()) {
            CroRom& r = g.roms[(size_t)cro.rom];
            ImGui::Separator();
            char id[64];
            std::snprintf(id, sizeof id, "%s", r.id.c_str());
            fieldLabel("Name (RID)");
            if (ImGui::InputText("##rid", id, 17)) { r.id = id; cro.dirty = true; }
            int type = (int)std::min<uint32_t>(r.type, 3);
            fieldLabel("Type (RTYP)");
            if (ImGui::Combo("##rtyp", &type, kRomTypes, 4)) { r.type = (uint32_t)type; cro.dirty = true; }
            int logical = (int)r.logical, physical = (int)r.physical;
            fieldLabel("Logical (RLOG): a high ROM's upper ROM number");
            if (ImGui::InputInt("##rlog", &logical)) { r.logical = (uint32_t)std::clamp(logical, 0, 255); cro.dirty = true; }
            fieldLabel("Physical (RPHY): a bankable ROM's cartridge page");
            if (ImGui::InputInt("##rphy", &physical)) { r.physical = (uint32_t)std::max(0, physical); cro.dirty = true; }
            ImGui::TextDisabled("%zu bytes", r.data.size());
            FlowRow row;
            if (row.smallButton("Replace the bytes..."))
                browser.open("The ROM's bytes", host.romDir, { ".rom", ".bin" }, [this](const std::string& p) {
                    Bytes data;
                    if (cro.group < 0 || cro.group >= (int)cro.groups.size()) return;
                    CroGroup& gg = cro.groups[(size_t)cro.group];
                    if (cro.rom < 0 || cro.rom >= (int)gg.roms.size()) return;
                    if (!readAll(p, data)) { cro.message = "Could not read " + p; return; }
                    if (hasAmsdosHeader(data)) data.erase(data.begin(), data.begin() + 128);
                    gg.roms[(size_t)cro.rom].data = std::move(data);
                    cro.dirty = true;
                });
            if (row.smallButton("Save the bytes...")) {
                const Bytes data = r.data;
                saver.open("Save the ROM", host.romDir, r.id + ".rom", [this, data](const std::string& p) {
                    std::string why;
                    cro.message = writeFileSafely(p, data.data(), data.size(), &why) ? "Saved " + fileName(p) : why;
                });
            }
        }
    }

    // ---- what is wrong with it
    if (!cro.groups.empty()) {
        const std::vector<std::string> problems = croProblems(cro.groups);
        ImGui::Separator();
        if (problems.empty()) ImGui::TextDisabled("No problems found.");
        for (const std::string& p : problems) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", p.c_str());
    }
    if (!cro.message.empty()) ImGui::TextDisabled("%s", cro.message.c_str());
}

} // namespace cpcse
