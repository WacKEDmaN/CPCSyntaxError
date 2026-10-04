// CPCSyntaxError GUI — shared widgets. See gui_widgets.h.
#include "gui_widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace cpcse {

void sectionHeading(const char* text) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("%s", text);
    ImGui::Separator();
}

ImVec4 rgbToVec(int rgb) {
    return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f);
}

bool beginFacts(const char* id, float nameWidth) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, nameWidth);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void fact(const char* name, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", name);
    ImGui::TableNextColumn();
    char buf[256];
    va_list args; va_start(args, fmt); std::vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    ImGui::TextUnformatted(buf);
}
void endFacts() { ImGui::EndTable(); }

void led(bool on, const char* label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight() - 3;
    p.y += 1.5f;
    dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), on ? IM_COL32(60, 230, 90, 255) : IM_COL32(40, 44, 52, 255));
    dl->AddRect(p, ImVec2(p.x + s, p.y + s), IM_COL32(90, 96, 110, 255));
    ImGui::Dummy(ImVec2(s + 2, s + 1));
    if (label) { ImGui::SameLine(); ImGui::TextUnformatted(label); }
}

// ============================================================== file dialogs
// ---- FlowRow: items on one line while they fit, the rest on the next
void FlowRow::place(float width) {
    if (!first) {
        ImGui::SameLine(0.0f, spacing);
        // what is left of the line, from where the item would go
        if (ImGui::GetContentRegionAvail().x + 0.5f < width) ImGui::NewLine();
    }
    first = false;
}

static float labelWidth(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x; }

bool FlowRow::button(const char* label, float width) {
    const float w = width > 0.0f ? width : labelWidth(label) + ImGui::GetStyle().FramePadding.x * 2.0f;
    place(w);
    return ImGui::Button(label, ImVec2(width, 0.0f));
}

bool FlowRow::smallButton(const char* label) {
    place(labelWidth(label) + ImGui::GetStyle().FramePadding.x * 2.0f);
    return ImGui::SmallButton(label);
}

bool FlowRow::checkbox(const char* label, bool* v) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float text = labelWidth(label);
    place(ImGui::GetFrameHeight() + (text > 0.0f ? s.ItemInnerSpacing.x + text : 0.0f));
    return ImGui::Checkbox(label, v);
}

bool FlowRow::radio(const char* label, bool active) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float text = labelWidth(label);
    place(ImGui::GetFrameHeight() + (text > 0.0f ? s.ItemInnerSpacing.x + text : 0.0f));
    return ImGui::RadioButton(label, active);
}

// Text in a row: on the line when it fits, else on the next -- and wrapped there at the
// window's edge if even a whole line is too short (a long path).
void FlowRow::text(const char* t) { textColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), t); }
void FlowRow::textDisabled(const char* t) { textColored(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), t); }

void FlowRow::textColored(const ImVec4& colour, const char* t) {
    place(ImGui::CalcTextSize(t).x);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void ledGrid(std::initializer_list<LedItem> items, float column) {
    const float startX = ImGui::GetCursorPosX();
    int col = 0;
    bool firstItem = true;
    for (const LedItem& it : items) {
        const float w = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(it.label).x;
        if (!firstItem) {
            ImGui::SameLine(startX + column * (float)col);
            if (ImGui::GetContentRegionAvail().x < w) { ImGui::NewLine(); col = 0; }
        }
        firstItem = false;
        led(it.on, it.label);
        col += 1;
    }
}

void textWrappedDisabled(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void textWrappedColored(const ImVec4& colour, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

static std::string lowerCase(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

void FileBrowser::open(const std::string& t, const std::string& startDir,
                       std::vector<std::string> extensions, std::function<void(const std::string&)> cb) {
    title = t; exts = std::move(extensions); onPick = std::move(cb); pickDir = false;
    std::error_code ec;
    cwd = std::filesystem::exists(startDir, ec) ? std::filesystem::absolute(startDir, ec).string()
                                                : std::filesystem::current_path(ec).string();
    nameBuf[0] = 0; visible = true; requestOpen = true;
}
void FileBrowser::openDir(const std::string& t, const std::string& startDir, std::function<void(const std::string&)> cb) {
    title = t; exts.clear(); onPick = std::move(cb); pickDir = true;
    std::error_code ec;
    cwd = std::filesystem::exists(startDir, ec) ? std::filesystem::absolute(startDir, ec).string()
                                                : std::filesystem::current_path(ec).string();
    nameBuf[0] = 0; visible = true; requestOpen = true;
}
void FileBrowser::draw() {
    if (requestOpen) { ImGui::OpenPopup(title.c_str()); requestOpen = false; }
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(title.c_str(), &visible)) return;
    namespace fs = std::filesystem; std::error_code ec;
    ImGui::TextUnformatted(cwd.c_str());
    ImGui::Separator();
    auto matches = [this](const fs::path& p) {
        if (exts.empty()) return true;
        std::string e = lowerCase(p.extension().string());
        for (const auto& x : exts) if (e == x) return true;
        return false;
    };
    ImGui::BeginChild("list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("[..]")) { fs::path parent = fs::path(cwd).parent_path(); if (!parent.empty()) cwd = parent.string(); }
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& e : fs::directory_iterator(cwd, ec)) {
        if (e.is_directory(ec)) dirs.push_back(e);
        else if (e.is_regular_file(ec) && matches(e.path())) files.push_back(e);
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return lowerCase(a.path().filename().string()) < lowerCase(b.path().filename().string()); };
    std::sort(dirs.begin(), dirs.end(), byName); std::sort(files.begin(), files.end(), byName);
    for (const auto& d : dirs) { std::string l = "[" + d.path().filename().string() + "]"; if (ImGui::Selectable(l.c_str())) cwd = d.path().string(); }
    for (const auto& f : files) {
        std::string name = f.path().filename().string();
        if (ImGui::Selectable(name.c_str(), name == nameBuf)) std::snprintf(nameBuf, sizeof(nameBuf), "%s", name.c_str());
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (onPick) onPick((fs::path(cwd) / name).string());
            visible = false; ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    if (pickDir) {
        if (ImGui::Button("Select this folder", ImVec2(-80, 0))) {
            if (onPick) onPick(cwd);
            visible = false; ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::SetNextItemWidth(-120);
        ImGui::InputText("##name", nameBuf, sizeof(nameBuf));
        ImGui::SameLine();
        if (ImGui::Button("Open", ImVec2(52, 0)) && nameBuf[0]) {
            if (onPick) onPick((fs::path(cwd) / nameBuf).string());
            visible = false; ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(56, 0))) { visible = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void SaveDialog::open(const std::string& t, const std::string& startDir, const std::string& defName,
                      std::function<void(const std::string&)> cb) {
    title = t; std::error_code ec;
    dir = std::filesystem::exists(startDir, ec) ? std::filesystem::absolute(startDir, ec).string()
                                                : std::filesystem::current_path(ec).string();
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", defName.c_str());
    onSave = std::move(cb); visible = true; requestOpen = true;
}
void SaveDialog::draw() {
    if (requestOpen) { ImGui::OpenPopup(title.c_str()); requestOpen = false; }
    if (!visible) return;
    if (!ImGui::BeginPopupModal(title.c_str(), &visible, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(dir.c_str());
    ImGui::SetNextItemWidth(360); ImGui::InputText("File name", nameBuf, sizeof(nameBuf));
    if (ImGui::Button("Save", ImVec2(80, 0)) && nameBuf[0]) {
        if (onSave) onSave((std::filesystem::path(dir) / nameBuf).string());
        visible = false; ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) { visible = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

} // namespace cpcse
