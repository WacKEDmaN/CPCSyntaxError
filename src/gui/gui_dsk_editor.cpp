// CPCSyntaxError GUI — the DSK editor. See gui_dsk_editor.h.
#include "gui_dsk_editor.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "core/basic.h"
#include "core/disassembler.h"

namespace cpcse {

static Bytes readHost(const std::string& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    ok = (bool)f;
    return ok ? Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()) : Bytes{};
}
static bool writeHost(const std::string& p, const Bytes& data) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return (bool)f;
}
static std::string interleaveText(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}
static std::vector<int> parseInterleave(const char* s) {
    std::vector<int> v;
    while (*s) {
        while (*s && (*s < '0' || *s > '9')) s++;
        if (!*s) break;
        v.push_back(std::atoi(s));
        while (*s >= '0' && *s <= '9') s++;
    }
    return v;
}
static std::string fileKey(const DskFsFile& f) { return std::to_string(f.user) + ":" + f.displayName(); }

// A sector's (or any buffer's) bytes: 16 a row; double-click a byte to type over it.
static bool hexEditor(const char* id, Bytes& data, int& editing, char* buf, bool editable) {
    bool changed = false;
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders);
    const float cell = ImGui::CalcTextSize("00").x;
    ImGuiListClipper clip;
    clip.Begin(((int)data.size() + 15) / 16);
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
            ImGui::TextDisabled("%04X", row * 16);
            for (int i = 0; i < 16; i++) {
                const int a = row * 16 + i;
                ImGui::SameLine(0, i == 8 ? 12.0f : 6.0f);
                if (a >= (int)data.size()) { ImGui::TextUnformatted("  "); continue; }
                ImGui::PushID(a);
                if (editing == a && editable) {
                    ImGui::SetNextItemWidth(cell + 6);
                    ImGui::SetKeyboardFocusHere();
                    if (ImGui::InputText("##e", buf, 3, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                        if (buf[0]) { data[(size_t)a] = (uint8_t)std::strtol(buf, nullptr, 16); changed = true; }
                        editing = a + 1 < (int)data.size() ? a + 1 : -1;
                        if (editing >= 0) std::snprintf(buf, 4, "%02X", data[(size_t)editing]);
                    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) editing = -1;
                } else {
                    char t[4]; std::snprintf(t, sizeof t, "%02X", data[(size_t)a]);
                    if (ImGui::Selectable(t, false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cell, 0)) && ImGui::IsMouseDoubleClicked(0) && editable) {
                        editing = a; std::snprintf(buf, 4, "%02X", data[(size_t)a]);
                    }
                }
                ImGui::PopID();
            }
            char text[17];
            for (int i = 0; i < 16; i++) { const int a = row * 16 + i; const int v = a < (int)data.size() ? data[(size_t)a] : ' '; text[i] = v >= 32 && v < 127 ? (char)v : '.'; }
            text[16] = 0;
            ImGui::SameLine(0, 12.0f);
            ImGui::TextDisabled("%s", text);
        }
    ImGui::EndChild();
    return changed;
}

// ============================================================== the window
DskEditorWindow::DskEditorWindow(EmuHost& h, FileBrowser& b, SaveDialog& s) : host(h), browser(b), saver(s) {
    presets = dskPresets();
    if (!presets.empty()) { newGeometry = presets[0]; formatGeometry = presets[0]; }
    std::snprintf(newInterleave, sizeof newInterleave, "%s", interleaveText(newGeometry.interleave).c_str());
}

std::string DskEditorWindow::displayName() const { return name.empty() ? std::string("(new disc)") : name; }

std::optional<DskGeometry> DskEditorWindow::filesystem() const {
    if (!disk) return std::nullopt;
    if (fsChoice >= 0 && fsChoice < (int)presets.size()) return presets[(size_t)fsChoice];
    return dskDetect(*disk);
}

void DskEditorWindow::newDisc(const DskGeometry& g) {
    disk = dskCreate(g);
    disk->modified = true;
    path.clear(); name.clear();
    fsChoice = -1;
    for (int i = 0; i < (int)presets.size(); i++) if (presets[(size_t)i].id == g.id) fsChoice = i;
    selCyl = selSide = selSector = selCopy = 0;
    selectedKey.clear();
    status = "New disc: " + (g.name.empty() ? std::string("custom geometry") : g.name);
}

bool DskEditorWindow::openFile(const std::string& p) {
    bool ok = false;
    Bytes data = readHost(p, ok);
    if (!ok) { status = "Could not read " + p; return false; }
    try { disk = parseDsk(data); }
    catch (const std::exception& e) { status = std::string("Not a DSK image: ") + e.what(); return false; }
    disk->modified = false;
    path = p; name = std::filesystem::path(p).filename().string();
    saveStandard = !disk->extended;
    fsChoice = -1;
    selCyl = selSide = selSector = selCopy = 0;
    selectedKey.clear();
    status = "Opened " + name + (disk->extended ? " (extended)" : " (standard)");
    return true;
}

bool DskEditorWindow::saveFile(const std::string& p) {
    if (!disk) return false;
    std::string why;
    const bool standard = saveStandard && dskFitsStandard(*disk, &why);
    if (!writeHost(p, serializeDsk(*disk, standard))) { status = "Could not write " + p; return false; }
    path = p; name = std::filesystem::path(p).filename().string();
    disk->modified = false;
    status = "Saved " + name + (standard ? " (standard format)" : saveStandard ? " (extended: " + why + ")" : " (extended format)");
    return true;
}

void DskEditorWindow::takeFromDrive(int unit) {
    auto d = host.driveDisk(unit);
    if (!d) { status = std::string("Drive ") + (unit ? "B" : "A") + " is empty"; return; }
    disk = d;
    path = host.diskPath[unit];
    name = host.diskName[unit].empty() ? std::string(unit ? "drive B" : "drive A") : host.diskName[unit];
    fsChoice = -1;
    selCyl = selSide = selSector = selCopy = 0;
    selectedKey.clear();
    status = "Editing the disc in drive " + std::string(unit ? "B" : "A") + " -- the CPC and the editor share it";
}

bool DskEditorWindow::insertIntoDrive(int unit) {
    if (!disk || !host.booted()) return false;
    const bool ok = host.insertDisk(disk, unit, displayName());
    if (ok && !path.empty()) host.diskPath[unit] = path;
    status = ok ? "In drive " + std::string(unit ? "B" : "A") + ": " + displayName() + " (shared with the editor)" : "No drive";
    return ok;
}

void DskEditorWindow::draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("DSK editor", open)) { focused = false; ImGui::End(); return; }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    toolbar();
    if (!disk) {
        ImGui::Spacing();
        ImGui::TextDisabled("No disc. Make a new one, open a .DSK, or take the one in a drive.");
    } else if (ImGui::BeginTabBar("##dsktabs")) {
        if (ImGui::BeginTabItem("Files", nullptr, requestTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { tabFiles(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Tracks & sectors", nullptr, requestTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { tabTracks(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Map", nullptr, requestTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) { tabMap(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Disc", nullptr, requestTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) { tabInfo(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    requestTab = -1;
    popups();
    ImGui::End();
}

// ============================================================== toolbar
void DskEditorWindow::toolbar() {
    if (ImGui::Button("New...")) openNewPopup = true;
    ImGui::SameLine();
    if (ImGui::Button("Open...")) browser.open("Open disc image", path.empty() ? host.romDir : std::filesystem::path(path).parent_path().string(),
                                               { ".dsk", ".edsk" }, [this](const std::string& p) { openFile(p); });
    ImGui::SameLine();
    ImGui::BeginDisabled(!disk);
    if (ImGui::Button("Save")) {
        if (!path.empty()) saveFile(path);
        else saver.open("Save disc image", host.romDir, "disc.dsk", [this](const std::string& p) { saveFile(p); });
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) saver.open("Save disc image", path.empty() ? host.romDir : std::filesystem::path(path).parent_path().string(),
                                                name.empty() ? "disc.dsk" : name, [this](const std::string& p) { saveFile(p); });
    ImGui::EndDisabled();
    ImGui::SameLine(0, 18);
    for (int unit = 0; unit < 2; unit++) {
        char t[32];
        std::snprintf(t, sizeof t, "From %s", unit ? "B" : "A");
        ImGui::BeginDisabled(!host.driveDisk(unit));
        if (ImGui::Button(t)) takeFromDrive(unit);
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    for (int unit = 0; unit < 2; unit++) {
        char t[32];
        std::snprintf(t, sizeof t, "Into %s", unit ? "B" : "A");
        ImGui::BeginDisabled(!disk || !host.booted());
        if (ImGui::Button(t)) insertIntoDrive(unit);
        ImGui::EndDisabled();
        if (unit == 0) ImGui::SameLine();
    }
    if (disk) {
        const bool shared = host.driveDisk(0) == disk || host.driveDisk(1) == disk;
        ImGui::TextColored(kAccent, "%s%s", displayName().c_str(), disk->modified ? " *" : "");
        ImGui::SameLine();
        ImGui::TextDisabled("%d tracks, %d side%s%s", disk->tracks, disk->sides, disk->sides > 1 ? "s" : "",
                            shared ? "  -- in a drive, shared with the CPC" : "");
        ImGui::SameLine(0, 18);
        ImGui::SetNextItemWidth(260);
        const auto fs = filesystem();
        std::string label = fsChoice < 0 ? "Format: as the disc says (" + (fs ? fs->name : std::string("none found")) + ")" : "Format: " + presets[(size_t)fsChoice].name;
        if (ImGui::BeginCombo("##fs", label.c_str())) {
            if (ImGui::Selectable("As the disc says", fsChoice < 0)) fsChoice = -1;
            for (int i = 0; i < (int)presets.size(); i++)
                if (ImGui::Selectable(presets[(size_t)i].name.c_str(), fsChoice == i)) fsChoice = i;
            ImGui::EndCombo();
        }
    }
    if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());
}

// ============================================================== files
void DskEditorWindow::importFile(const std::string& hostPath) {
    auto g = filesystem();
    if (!g) { status = "No filesystem: choose a format first"; return; }
    bool ok = false;
    Bytes data = readHost(hostPath, ok);
    if (!ok) { status = "Could not read " + hostPath; return; }
    std::string n, e;
    dskSplitName(std::filesystem::path(hostPath).filename().string(), n, e);
    if (importHeader && !hasAmsdosHeader(data)) {
        Bytes h = dskAmsdosHeader(importUser, n, e, importType, importLoad, importExec, (int)data.size());
        data.insert(data.begin(), h.begin(), h.end());
    }
    DskFs fs(disk, *g);
    const std::string err = fs.write(importUser, n, e, data);
    status = err.empty() ? "Imported " + n + (e.empty() ? "" : "." + e) + " (" + std::to_string(data.size()) + " bytes)" : "Import: " + err;
}

void DskEditorWindow::makeView(const DskFsFile& f, const Bytes& raw) {
    viewLines.clear();
    const bool header = f.header.has_value();
    Bytes data = header && raw.size() >= 128 ? Bytes(raw.begin() + 128, raw.end()) : raw;
    if (header) {
        const int len = std::clamp(f.header->fullLength ? f.header->fullLength : f.header->logicalLength, 0, (int)data.size());
        data.resize((size_t)len);
    }
    if (viewMode == 1) {                                    // BASIC: the program as BASIC keeps it from &0170
        BasicDecoder dec;
        auto rd = [&](int a) { const int i = a - 0x170; return i >= 0 && i < (int)data.size() ? (int)data[(size_t)i] : 0; };
        for (const BasicLine& l : dec.listProgram(rd, 0x170)) viewLines.push_back(std::to_string(l.num) + " " + l.text);
        if (viewLines.empty()) viewLines.push_back("(not a tokenised BASIC program)");
    } else if (viewMode == 2) {                             // text, to the CP/M end of file
        std::string line;
        for (uint8_t c : data) {
            if (c == 0x1a) break;
            if (c == '\n') { viewLines.push_back(line); line.clear(); }
            else if (c == '\r') continue;
            else line += c >= 32 && c < 127 ? (char)c : '.';
        }
        if (!line.empty()) viewLines.push_back(line);
    } else if (viewMode == 3) {                             // Z80, from the header's load address
        Disassembler dis;
        const int base = header ? f.header->loadAddress : 0;
        auto rd = [&](int a) { const int i = (a - base) & 0xffff; return i < (int)data.size() ? (int)data[(size_t)i] : 0; };
        for (int pc = base, n = 0; (pc - base) < (int)data.size() && n < 20000; n++) {
            const DisasmResult r = dis.disassemble(rd, pc & 0xffff);
            char line[96];
            std::snprintf(line, sizeof line, "%04X  %-20s", pc & 0xffff, r.mnem.c_str());
            viewLines.push_back(line);
            const int len = ((r.next - pc) & 0xffff);
            pc += len > 0 ? len : 1;
        }
    }
}

void DskEditorWindow::tabFiles() {
    const auto g = filesystem();
    if (!g) { ImGui::TextDisabled("No filesystem found on this disc: choose a format above (or use Tracks & sectors)."); return; }
    DskFs fs(disk, *g);
    const std::vector<DskFsFile> files = fs.files();
    const int freeK = fs.freeBlocks() * g->blockSize / 1024;
    ImGui::Text("%d files, %dK free (%d of %d blocks of %dK), %d directory entries free",
                (int)files.size(), freeK, fs.freeBlocks(), fs.totalBlocks(), g->blockSize / 1024, fs.freeEntries());

    // import / export / delete / rename
    if (ImGui::Button("Import...")) browser.open("Import a file onto the disc", host.romDir, {}, [this](const std::string& p) { importFile(p); });
    ImGui::SameLine();
    ImGui::Checkbox("add an AMSDOS header", &importHeader);
    if (importHeader) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        static const char* types[] = { "BASIC", "protected", "binary" };
        int t = importType == 0 ? 0 : importType == 1 ? 1 : 2;
        if (ImGui::Combo("##itype", &t, types, 3)) importType = t == 2 ? 2 : t;
        ImGui::SameLine(); ImGui::SetNextItemWidth(60);
        ImGui::InputScalar("load", ImGuiDataType_U16, &importLoad, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine(); ImGui::SetNextItemWidth(60);
        ImGui::InputScalar("exec", ImGuiDataType_U16, &importExec, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal);
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    if (ImGui::InputInt("user", &importUser)) importUser = std::clamp(importUser, 0, 15);

    const DskFsFile* sel = nullptr;
    for (const DskFsFile& f : files) if (fileKey(f) == selectedKey) sel = &f;
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Export...") && sel) {
        const DskFsFile f = *sel;
        saver.open("Export the file", host.romDir, f.displayName(), [this, f, g](const std::string& p) {
            DskFs fs2(disk, *g);
            status = writeHost(p, fs2.read(f, exportStrip && f.header)) ? "Exported " + f.displayName() : "Could not write " + p;
        });
    }
    ImGui::SameLine();
    ImGui::Checkbox("without its header", &exportStrip);
    ImGui::SameLine();
    if (ImGui::Button("Delete") && sel) { fs.remove(*sel); status = "Deleted " + sel->displayName(); selectedKey.clear(); }
    ImGui::SameLine();
    if (ImGui::Button("Rename...") && sel) {
        std::snprintf(renameBuf, sizeof renameBuf, "%s", sel->displayName().c_str());
        renameUser = sel->user;
        openRenamePopup = true;
    }
    if (sel) {
        bool ro = sel->readOnly, sys = sel->system, arc = sel->archived;
        ImGui::SameLine(0, 18);
        bool ch = ImGui::Checkbox("read-only", &ro);
        ImGui::SameLine(); ch |= ImGui::Checkbox("system", &sys);
        ImGui::SameLine(); ch |= ImGui::Checkbox("archived", &arc);
        if (ch) fs.setAttributes(*sel, ro, sys, arc);
    }
    ImGui::EndDisabled();

    // the directory
    const float listH = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.45f);
    if (ImGui::BeginTable("##files", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : { "user", "name", "size", "attr", "type", "load", "exec", "blocks" }) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const DskFsFile& f : files) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(fileKey(f).c_str());
            if (ImGui::Selectable(std::to_string(f.user).c_str(), fileKey(f) == selectedKey, ImGuiSelectableFlags_SpanAllColumns)) selectedKey = fileKey(f);
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.displayName().c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", f.header ? (f.header->fullLength ? f.header->fullLength : f.header->logicalLength) : f.size());
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", f.readOnly ? "R" : "-", f.system ? "S" : "-", f.archived ? "A" : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.header ? f.header->typeName.c_str() : "no header");
            ImGui::TableNextColumn(); if (f.header) ImGui::Text("&%04X", f.header->loadAddress); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (f.header) ImGui::Text("&%04X", f.header->execAddress); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)f.blocks.size());
        }
        ImGui::EndTable();
    }

    // the file looked at
    if (!sel) { ImGui::TextDisabled("Click a file to look at it."); return; }
    static const char* modes[] = { "Hex", "BASIC", "Text", "Disassembly" };
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##view", &viewMode, modes, 4);
    ImGui::SameLine();
    ImGui::TextDisabled("%s: %d records, entries %d, %s", sel->displayName().c_str(), sel->records, (int)sel->entries.size(),
                        sel->header ? "AMSDOS header" : "no header");
    Bytes raw = fs.read(*sel, false);
    if (viewMode == 0) {
        int dummy = -1; char b[4];
        hexEditor("##fhex", raw, dummy, b, false);
    } else {
        const std::string key = fileKey(*sel) + "/" + std::to_string(viewMode) + "/" + std::to_string(raw.size()) + "/" + std::to_string(raw.empty() ? 0 : raw[raw.size() / 2]);
        if (key != viewKey) { makeView(*sel, raw); viewKey = key; }
        ImGui::BeginChild("##fview", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clip;
        clip.Begin((int)viewLines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(viewLines[(size_t)i].c_str());
        ImGui::EndChild();
    }
}

// ============================================================== tracks and sectors
void DskEditorWindow::tabTracks() {
    selCyl = std::clamp(selCyl, 0, disk->tracks - 1);
    selSide = std::clamp(selSide, 0, disk->sides - 1);
    // the tracks, a row a cylinder
    ImGui::BeginChild("##tracklist", ImVec2(150, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    for (int c = 0; c < disk->tracks; c++)
        for (int s = 0; s < disk->sides; s++) {
            auto t = disk->trackData[(size_t)c][(size_t)s];
            char label[48];
            if (t) std::snprintf(label, sizeof label, "%02d%s  %d sect", c, disk->sides > 1 ? (s ? "B" : "A") : "", (int)t->sectors.size());
            else std::snprintf(label, sizeof label, "%02d%s  unformatted", c, disk->sides > 1 ? (s ? "B" : "A") : "");
            ImGui::PushID(c * 2 + s);
            if (!t) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::Selectable(label, c == selCyl && s == selSide)) { selCyl = c; selSide = s; selSector = 0; selCopy = 0; hexEditing = -1; }
            if (!t) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##trackpane", ImVec2(0, 0));
    auto& slot = disk->trackData[(size_t)selCyl][(size_t)selSide];
    ImGui::Text("Cylinder %d, side %d", selCyl, selSide);
    ImGui::SameLine(0, 18);
    if (ImGui::Button("Format...")) {
        formatGeometry = filesystem().value_or(newGeometry);
        std::snprintf(formatInterleave, sizeof formatInterleave, "%s", interleaveText(formatGeometry.interleave).c_str());
        openFormatPopup = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!slot);
    if (ImGui::Button("Unformat")) { slot = nullptr; disk->modified = true; }
    ImGui::EndDisabled();
    if (!slot) { ImGui::TextDisabled("Unformatted: Format... gives it sectors."); ImGui::EndChild(); return; }
    Track& t = *slot;
    // the Track-Info's own fields
    auto byteField = [&](const char* label, int& v) {
        ImGui::SetNextItemWidth(42);
        uint8_t b = (uint8_t)v;
        if (ImGui::InputScalar(label, ImGuiDataType_U8, &b, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal)) { v = b; disk->modified = true; }
    };
    byteField("track", t.cylinder); ImGui::SameLine();
    byteField("side", t.side); ImGui::SameLine();
    byteField("N", t.sizeCode); ImGui::SameLine();
    byteField("GAP#3", t.gap3); ImGui::SameLine();
    byteField("filler", t.filler); ImGui::SameLine();
    byteField("rate", t.dataRate); ImGui::SameLine();
    byteField("mode", t.recordingMode);
    // its sectors
    if (ImGui::Button("Add sector")) {
        Sector s;
        s.c = selCyl; s.h = selSide; s.n = t.sizeCode;
        int r = 0xc1;
        for (const Sector& o : t.sectors) r = std::max(r, o.r + 1);
        s.r = r & 0xff;
        s.data.push_back(Bytes((size_t)(128 << std::min(7, s.n & 7)), (uint8_t)t.filler));
        t.sectors.push_back(s);
        selSector = (int)t.sectors.size() - 1;
        disk->modified = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(t.sectors.empty());
    if (ImGui::Button("Remove sector") && selSector < (int)t.sectors.size()) {
        t.sectors.erase(t.sectors.begin() + selSector);
        selSector = std::max(0, selSector - 1);
        disk->modified = true;
    }
    ImGui::EndDisabled();
    selSector = std::clamp(selSector, 0, std::max(0, (int)t.sectors.size() - 1));
    if (ImGui::BeginTable("##sectors", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, std::min(200.0f, 30.0f + 22.0f * (float)t.sectors.size())))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        for (const char* h : { "#", "C", "H", "R", "N", "ST1", "ST2", "bytes", "copies" }) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)t.sectors.size(); i++) {
            const Sector& s = t.sectors[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (ImGui::Selectable(std::to_string(i).c_str(), i == selSector, ImGuiSelectableFlags_SpanAllColumns)) { selSector = i; selCopy = 0; hexEditing = -1; }
            ImGui::PopID();
            for (int v : { s.c, s.h, s.r, s.n, s.st1, s.st2 }) { ImGui::TableNextColumn(); ImGui::Text("%02X", v); }
            ImGui::TableNextColumn(); ImGui::Text("%d", s.data.empty() ? 0 : (int)s.data[0].size());
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)s.data.size());
        }
        ImGui::EndTable();
    }
    if (t.sectors.empty()) { ImGui::EndChild(); return; }
    Sector& s = t.sectors[(size_t)selSector];
    ImGui::TextDisabled("Sector %d:", selSector);
    ImGui::SameLine();
    byteField("C##s", s.c); ImGui::SameLine();
    byteField("H##s", s.h); ImGui::SameLine();
    byteField("R##s", s.r); ImGui::SameLine();
    byteField("N##s", s.n); ImGui::SameLine();
    byteField("ST1##s", s.st1); ImGui::SameLine();
    byteField("ST2##s", s.st2);
    int size = s.data.empty() ? 0 : (int)s.data[0].size();
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("bytes stored", &size, 128, 512, ImGuiInputTextFlags_EnterReturnsTrue)) {
        size = std::clamp(size, 0, 0x8000);
        if (s.data.empty()) s.data.push_back({});
        for (Bytes& b : s.data) b.resize((size_t)size, (uint8_t)t.filler);
        disk->modified = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(N %d is %d)", s.n, 128 << std::min(7, s.n & 7));
    ImGui::SameLine(0, 18);
    selCopy = std::clamp(selCopy, 0, std::max(0, (int)s.data.size() - 1));
    ImGui::SetNextItemWidth(80);
    if (s.data.size() > 1) { ImGui::SliderInt("copy", &selCopy, 0, (int)s.data.size() - 1); ImGui::SameLine(); }
    if (ImGui::Button("Add a copy (weak)")) { s.data.push_back(s.data.empty() ? Bytes() : s.data[(size_t)selCopy]); selCopy = (int)s.data.size() - 1; disk->modified = true; }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.data.size() < 2);
    if (ImGui::Button("Remove this copy")) { s.data.erase(s.data.begin() + selCopy); selCopy = 0; disk->modified = true; }
    ImGui::EndDisabled();
    if (!s.data.empty() && hexEditor("##shex", s.data[(size_t)selCopy], hexEditing, hexBuf, true)) disk->modified = true;
    ImGui::EndChild();
}

// ============================================================== map
void DskEditorWindow::tabMap() {
    const auto g = filesystem();
    if (!g) { ImGui::TextDisabled("No filesystem found: choose a format above."); return; }
    DskFs fs(disk, *g);
    const std::vector<DskFsFile> files = fs.files();
    const std::vector<DskBlockUse> map = fs.blockMap(files);
    auto colourFor = [&](const DskBlockUse& u) -> ImU32 {
        switch (u.kind) {
            case DskBlockUse::Directory: return IM_COL32(90, 140, 240, 255);
            case DskBlockUse::Shared: return IM_COL32(240, 70, 60, 255);
            case DskBlockUse::File: {
                const unsigned h = (unsigned)u.file * 2654435761u;
                return IM_COL32(110 + (h >> 8) % 120, 150 + (h >> 16) % 100, 80 + (h >> 24) % 120, 255);
            }
            default: return IM_COL32(45, 47, 55, 255);
        }
    };
    ImGui::TextDisabled("Blocks (%dK each): blue the directory, colours the files, dark free, red a block two files claim.", g->blockSize / 1024);
    const float cell = 14.0f;
    const int perRow = std::max(8, (int)((ImGui::GetContentRegionAvail().x) / (cell + 2)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    for (int b = 0; b < (int)map.size(); b++) {
        const float x = p0.x + (b % perRow) * (cell + 2), y = p0.y + (b / perRow) * (cell + 2);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), colourFor(map[(size_t)b]));
        if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + cell, y + cell))) {
            std::vector<std::array<int, 3>> secs;
            fs.blockSectors(b, secs);
            std::string where;
            for (auto& s : secs) { char t[24]; std::snprintf(t, sizeof t, " %d/%d/&%02X", s[0], s[1], s[2]); where += t; }
            const DskBlockUse& u = map[(size_t)b];
            ImGui::SetTooltip("block %d: %s\nsectors (cyl/side/R):%s", b,
                              u.kind == DskBlockUse::Directory ? "directory" : u.kind == DskBlockUse::Free ? "free"
                              : u.kind == DskBlockUse::Shared ? "claimed twice" : files[(size_t)u.file].displayName().c_str(), where.c_str());
        }
    }
    ImGui::Dummy(ImVec2((float)perRow * (cell + 2), (float)(((int)map.size() + perRow - 1) / perRow) * (cell + 2)));
    // the same by track: each formatted track's sectors in their order round it
    ImGui::Spacing();
    ImGui::TextDisabled("Tracks: each sector round its track, coloured by its block's owner; grey reserved (system) tracks, hollow sectors outside the filesystem.");
    std::map<long long, int> ownerOf;     // (cyl, side, R) -> block
    for (int b = 0; b < (int)map.size(); b++) {
        std::vector<std::array<int, 3>> secs;
        fs.blockSectors(b, secs);
        for (auto& s : secs) ownerOf[((long long)s[0] << 16) | (s[1] << 8) | s[2]] = b;
    }
    ImGui::BeginChild("##trackmap", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const ImVec2 q0 = ImGui::GetCursorScreenPos();
    ImDrawList* tl = ImGui::GetWindowDrawList();
    int row = 0;
    for (int c = 0; c < disk->tracks; c++)
        for (int s = 0; s < disk->sides; s++, row++) {
            const float y = q0.y + row * (cell + 2);
            char lbl[16]; std::snprintf(lbl, sizeof lbl, "%02d%s", c, disk->sides > 1 ? (s ? "B" : "A") : "");
            tl->AddText(ImVec2(q0.x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled), lbl);
            auto t = disk->trackData[(size_t)c][(size_t)s];
            if (!t) continue;
            for (int i = 0; i < (int)t->sectors.size(); i++) {
                const float x = q0.x + 40 + i * (cell + 2);
                const Sector& sec = t->sectors[(size_t)i];
                auto it = ownerOf.find(((long long)c << 16) | (s << 8) | sec.r);
                const int logicalTrack = g->sides == 2 ? (g->sidesAlternate ? c * 2 + s : s * g->tracks + c) : c;
                if (it != ownerOf.end()) tl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), colourFor(map[(size_t)it->second]));
                else if (logicalTrack < g->reservedTracks) tl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell, y + cell), IM_COL32(110, 110, 115, 255));
                else tl->AddRect(ImVec2(x, y), ImVec2(x + cell, y + cell), IM_COL32(150, 150, 160, 255));
                if (ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(x + cell, y + cell)))
                    ImGui::SetTooltip("cyl %d side %d, sector &%02X (N %d, ST1 %02X ST2 %02X)%s", c, s, sec.r, sec.n, sec.st1, sec.st2,
                                      it != ownerOf.end() ? (" -- block " + std::to_string(it->second)).c_str() : "");
            }
        }
    ImGui::Dummy(ImVec2(40 + 30 * (cell + 2), (float)row * (cell + 2)));
    ImGui::EndChild();
}

// ============================================================== the disc itself
void DskEditorWindow::tabInfo() {
    if (creatorBuf[0] == 0) std::snprintf(creatorBuf, sizeof creatorBuf, "%s", disk->creator.c_str());
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("creator", creatorBuf, sizeof creatorBuf)) { disk->creator = creatorBuf; disk->modified = true; }
    std::string why;
    const bool fits = dskFitsStandard(*disk, &why);
    ImGui::Checkbox("Save as the standard format (MV - CPCEMU) when the disc fits it", &saveStandard);
    ImGui::TextDisabled(fits ? "This disc fits the standard format." : "This disc needs the extended format: %s.", why.c_str());
    int formatted = 0, sectors = 0, weak = 0;
    long long bytes = 0;
    for (auto& cyl : disk->trackData)
        for (auto& t : cyl) {
            if (!t) continue;
            formatted++;
            for (const Sector& s : t->sectors) { sectors++; if (s.data.size() > 1) weak++; bytes += s.data.empty() ? 0 : (long long)s.data[0].size(); }
        }
    ImGui::Text("%d of %d tracks formatted, %d sectors (%d weak), %lldK of sector data", formatted, disk->tracks * disk->sides, sectors, weak, bytes / 1024);
    if (const auto g = filesystem()) {
        DskFs fs(disk, *g);
        ImGui::Text("Filesystem: %s -- %d blocks of %dK (DSM %d), %d directory entries, %d reserved tracks, %dK free",
                    g->name.c_str(), fs.totalBlocks(), g->blockSize / 1024, fs.totalBlocks() - 1, g->dirEntries, g->reservedTracks,
                    fs.freeBlocks() * g->blockSize / 1024);
    } else {
        ImGui::TextDisabled("Filesystem: none found (track 0's sector IDs are not a CPC format's)");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Tracks and sides (new tracks unformatted, fewer drops the rest):");
    if (ImGui::IsWindowAppearing()) { resizeTracks = disk->tracks; resizeSides = disk->sides; }
    ImGui::SetNextItemWidth(90); ImGui::InputInt("tracks##rs", &resizeTracks);
    ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::InputInt("sides##rs", &resizeSides);
    ImGui::SameLine();
    if (ImGui::Button("Apply")) { dskResize(*disk, resizeTracks, resizeSides); status = "Now " + std::to_string(disk->tracks) + " tracks, " + std::to_string(disk->sides) + " side(s)"; }
    ImGui::SameLine();
    if (ImGui::Button("Format the new tracks")) {
        const DskGeometry g = filesystem().value_or(newGeometry);
        int n = 0;
        for (int c = 0; c < disk->tracks; c++)
            for (int s = 0; s < disk->sides; s++)
                if (!disk->trackData[(size_t)c][(size_t)s]) { dskFormatTrack(*disk, c, s, g); n++; }
        status = "Formatted " + std::to_string(n) + " track(s) as " + g.name;
    }
}

// ============================================================== popups
void DskEditorWindow::geometryFields(DskGeometry& g, char* interleave, size_t interleaveSize, bool withFilesystem) {
    auto num = [](const char* label, int& v, int lo, int hi) {
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt(label, &v)) v = std::clamp(v, lo, hi);
    };
    auto hex = [](const char* label, int& v) {
        ImGui::SetNextItemWidth(50);
        uint8_t b = (uint8_t)v;
        if (ImGui::InputScalar(label, ImGuiDataType_U8, &b, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal)) v = b;
    };
    num("tracks", g.tracks, 1, 255); ImGui::SameLine(); num("sides", g.sides, 1, 2);
    num("sectors a track", g.sectors, 0, 64); ImGui::SameLine(); num("N (128 << N bytes)", g.sizeCode, 0, 7);
    hex("first sector ID", g.firstSector); ImGui::SameLine(); hex("GAP#3", g.gap3); ImGui::SameLine(); hex("filler", g.filler);
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputText("interleave (sector offsets round the track)", interleave, interleaveSize)) g.interleave = parseInterleave(interleave);
    if (withFilesystem) {
        num("reserved tracks", g.reservedTracks, 0, 10); ImGui::SameLine();
        num("block size", g.blockSize, 1024, 16384); ImGui::SameLine();
        num("directory entries", g.dirEntries, 16, 1024);
    }
}

void DskEditorWindow::popups() {
    if (openNewPopup) { ImGui::OpenPopup("New disc"); openNewPopup = false; }
    if (openFormatPopup) { ImGui::OpenPopup("Format track"); openFormatPopup = false; }
    if (openRenamePopup) { ImGui::OpenPopup("Rename file"); openRenamePopup = false; }

    if (ImGui::BeginPopupModal("New disc", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(320);
        if (ImGui::BeginCombo("format", newPreset < (int)presets.size() ? presets[(size_t)newPreset].name.c_str() : "custom")) {
            for (int i = 0; i < (int)presets.size(); i++)
                if (ImGui::Selectable(presets[(size_t)i].name.c_str(), i == newPreset)) {
                    newPreset = i; newGeometry = presets[(size_t)i];
                    std::snprintf(newInterleave, sizeof newInterleave, "%s", interleaveText(newGeometry.interleave).c_str());
                }
            ImGui::EndCombo();
        }
        geometryFields(newGeometry, newInterleave, sizeof newInterleave, true);
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            if (newPreset < (int)presets.size()) {
                const DskGeometry& p = presets[(size_t)newPreset];
                const bool same = p.tracks == newGeometry.tracks && p.sides == newGeometry.sides && p.sectors == newGeometry.sectors &&
                                  p.sizeCode == newGeometry.sizeCode && p.firstSector == newGeometry.firstSector && p.blockSize == newGeometry.blockSize &&
                                  p.dirEntries == newGeometry.dirEntries && p.reservedTracks == newGeometry.reservedTracks;
                if (!same) { newGeometry.id.clear(); newGeometry.name = "custom"; }
            }
            newDisc(newGeometry);
            if (newGeometry.id.empty()) {          // a geometry of one's own: its filesystem as entered
                presets.push_back(newGeometry);
                presets.back().id = "custom"; presets.back().name = "Custom (as made)";
                fsChoice = (int)presets.size() - 1;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Format track", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Cylinder %d, side %d: every sector it has now is lost.", selCyl, selSide);
        geometryFields(formatGeometry, formatInterleave, sizeof formatInterleave, false);
        if (ImGui::Button("Format", ImVec2(120, 0)) && disk) {
            dskFormatTrack(*disk, selCyl, selSide, formatGeometry);
            selSector = 0; selCopy = 0;
            status = "Formatted cylinder " + std::to_string(selCyl) + " side " + std::to_string(selSide);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("All tracks", ImVec2(120, 0)) && disk) {
            for (int c = 0; c < disk->tracks; c++) for (int s = 0; s < disk->sides; s++) dskFormatTrack(*disk, c, s, formatGeometry);
            status = "Formatted every track";
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Rename file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(160);
        const bool enter = ImGui::InputText("name.ext", renameBuf, sizeof renameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt("user", &renameUser)) renameUser = std::clamp(renameUser, 0, 15);
        if ((ImGui::Button("Rename", ImVec2(120, 0)) || enter) && disk) {
            if (const auto g = filesystem()) {
                DskFs fs(disk, *g);
                for (const DskFsFile& f : fs.files())
                    if (fileKey(f) == selectedKey) {
                        std::string n, e;
                        dskSplitName(renameBuf, n, e);
                        const std::string err = fs.rename(f, renameUser, n, e);
                        status = err.empty() ? "Renamed to " + n + (e.empty() ? "" : "." + e) : "Rename: " + err;
                        if (err.empty()) selectedKey = std::to_string(renameUser) + ":" + n + (e.empty() ? "" : "." + e);
                        break;
                    }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace cpcse
