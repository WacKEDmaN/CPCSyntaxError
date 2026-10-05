// CPCSyntaxError GUI — shared widgets. See gui_widgets.h.
#include "gui_widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>   // GetLogicalDrives: the file picker's drives
#endif

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

bool writeFileSafely(const std::string& path, const void* data, size_t size, std::string* why) {
    namespace fs = std::filesystem;
    const fs::path target(path), temp(path + ".cpcse-new");   // narrow names, as every other file here
    {
        std::FILE* f = std::fopen(temp.string().c_str(), "wb");
        if (!f) { if (why) *why = "cannot write " + path; return false; }
        const bool wrote = (size == 0 || std::fwrite(data, 1, size, f) == size);
        const bool closed = std::fclose(f) == 0;
        if (!wrote || !closed) {
            std::error_code ec;
            fs::remove(temp, ec);
            if (why) *why = "could not write all of " + path + " (disc full?); the old file is as it was";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, target, ec);   // replaces the old file in one step
    if (ec) {
        fs::remove(temp, ec);
        if (why) *why = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

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


static std::string lowerCase(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

std::vector<std::string> fileSystemRoots() {
    std::vector<std::string> roots;
#ifdef _WIN32
    const DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++)
        if (drives & (1u << d)) roots.push_back(std::string(1, (char)('A' + d)) + ":\\");
#else
    roots.push_back("/");
#endif
    return roots;
}

bool enterTypedFolder(std::string& cwd, const std::string& typedIn) {
    namespace fs = std::filesystem;
    std::string typed = typedIn;
    while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\t')) typed.pop_back();
    if (typed.empty()) return false;
#ifdef _WIN32
    if (typed.size() == 2 && typed[1] == ':') typed += '\\';     // "D:" is that drive's root
#endif
    std::error_code ec;
    fs::path p = fs::path(cwd) / fs::path(typed);                // an absolute name replaces cwd
    if (!fs::is_directory(p, ec)) return false;
    fs::path full = fs::absolute(p, ec).lexically_normal();
    if (ec) return false;
    cwd = full.string();
    return true;
}

bool FolderView::draw(std::string& cwd, const std::vector<std::string>& exts, const char* selected,
                      float reserve, std::string& clicked, bool& doubleClicked) {
    namespace fs = std::filesystem;
    bool picked = false;
    // the drives: on Windows the only way off the one cwd started on
    const std::vector<std::string> roots = fileSystemRoots();
    if (roots.size() > 1) {
        const std::string here = lowerCase(fs::path(cwd).root_path().string());
        FlowRow drives;
        drives.spacing = 4.0f;
        for (const std::string& r : roots) {
            const std::string label = r.substr(0, r.size() - 1);  // "C:"
            const bool on = lowerCase(r) == here;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (drives.smallButton(label.c_str())) cwd = r;
            if (on) ImGui::PopStyleColor();
        }
    }
    {
        WrapText wrap;
        ImGui::TextUnformatted(cwd.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("list", ImVec2(0, -reserve), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("[..]")) {
        const fs::path here(cwd), parent = here.parent_path();
        if (!parent.empty() && parent != here && here != here.root_path()) cwd = parent.string();
    }
    if (cwd != listedDir || ImGui::GetTime() - listedAt > 1.0) {
        // Stepped with the error code: a range-for's ++ throws on an entry it cannot read
        // (a protected folder, a file gone mid-listing, an empty card reader), and nothing
        // would catch it.
        auto matches = [&](const fs::path& p) {
            if (exts.empty()) return true;
            const std::string e = lowerCase(p.extension().string());
            for (const auto& x : exts) if (e == x) return true;
            return false;
        };
        dirNames.clear(); fileNames.clear();
        std::error_code ec;
        fs::directory_iterator it(cwd, ec), end;
        for (int n = 0; !ec && it != end && n < 100000; it.increment(ec), n++) {
            std::error_code ec2;
            if (it->is_directory(ec2)) dirNames.push_back(it->path().filename().string());
            else if (it->is_regular_file(ec2) && matches(it->path())) fileNames.push_back(it->path().filename().string());
        }
        auto byName = [](const std::string& a, const std::string& b) { return lowerCase(a) < lowerCase(b); };
        std::sort(dirNames.begin(), dirNames.end(), byName); std::sort(fileNames.begin(), fileNames.end(), byName);
        listedDir = cwd;
        listedAt = ImGui::GetTime();
    }
    std::string into;
    for (const auto& d : dirNames) { std::string l = "[" + d + "]"; if (ImGui::Selectable(l.c_str())) into = d; }
    for (const auto& name : fileNames) {
        if (ImGui::Selectable(name.c_str(), selected && name == selected)) { clicked = name; picked = true; doubleClicked = false; }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) { clicked = name; picked = true; doubleClicked = true; }
    }
    ImGui::EndChild();
    if (!into.empty()) cwd = (fs::path(cwd) / into).string();
    return picked;
}

void FileBrowser::open(const std::string& t, const std::string& startDir,
                       std::vector<std::string> extensions, std::function<void(const std::string&)> cb) {
    title = t; exts = std::move(extensions); onPick = std::move(cb); pickDir = false;
    std::error_code ec;
    cwd = std::filesystem::exists(startDir, ec) ? std::filesystem::absolute(startDir, ec).string()
                                                : std::filesystem::current_path(ec).string();
    nameBuf[0] = 0; visible = true; requestOpen = true;
    view.relist();                      // listed afresh: these extensions
}
void FileBrowser::openDir(const std::string& t, const std::string& startDir, std::function<void(const std::string&)> cb) {
    title = t; exts.clear(); onPick = std::move(cb); pickDir = true;
    std::error_code ec;
    cwd = std::filesystem::exists(startDir, ec) ? std::filesystem::absolute(startDir, ec).string()
                                                : std::filesystem::current_path(ec).string();
    nameBuf[0] = 0; visible = true; requestOpen = true;
    view.relist();
}
void FileBrowser::draw() {
    if (requestOpen) { ImGui::OpenPopup(title.c_str()); requestOpen = false; }
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(640, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(title.c_str(), &visible)) return;
    namespace fs = std::filesystem;
    std::string clicked;
    bool twice = false;
    if (view.draw(cwd, exts, nameBuf, ImGui::GetFrameHeightWithSpacing() * 2, clicked, twice)) {
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", clicked.c_str());
        if (twice && !pickDir) {
            if (onPick) onPick((fs::path(cwd) / clicked).string());
            visible = false; ImGui::CloseCurrentPopup();
        }
    }
    if (pickDir) {
        if (ImGui::Button("Select this folder", ImVec2(-80, 0))) {
            if (onPick) onPick(cwd);
            visible = false; ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::SetNextItemWidth(-(52 + 56 + ImGui::GetStyle().ItemSpacing.x * 2 + 2));
        const bool enter = ImGui::InputTextWithHint("##name", "a file, or a folder to go to (D:\\games)", nameBuf, sizeof(nameBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Open", ImVec2(52, 0)) || enter) && nameBuf[0]) {
            if (enterTypedFolder(cwd, nameBuf)) nameBuf[0] = 0;   // a folder: go into it
            else {
                if (onPick) onPick((fs::path(cwd) / nameBuf).string());
                visible = false; ImGui::CloseCurrentPopup();
            }
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
    view.relist();
}
void SaveDialog::draw() {
    if (requestOpen) { ImGui::OpenPopup(title.c_str()); requestOpen = false; }
    if (!visible) return;
    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(title.c_str(), &visible)) return;
    namespace fs = std::filesystem;
    // the folder to save in, browsed as the Open dialog's; a file clicked takes its name
    std::string clicked;
    bool twice = false;
    if (view.draw(dir, {}, nameBuf, ImGui::GetFrameHeightWithSpacing() * 2, clicked, twice))
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", clicked.c_str());
    {   // the name field takes what its label and the two buttons leave
        const ImGuiStyle& s = ImGui::GetStyle();
        ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("File name").x + s.ItemInnerSpacing.x + 56 * 2 + s.ItemSpacing.x * 2 + 2));
    }
    const bool enter = ImGui::InputText("File name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Save", ImVec2(56, 0)) || enter) && nameBuf[0]) {
        if (enterTypedFolder(dir, nameBuf)) nameBuf[0] = 0;       // a folder: go into it
        else {
            if (onSave) onSave((fs::path(dir) / nameBuf).string());
            visible = false; ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(56, 0))) { visible = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

} // namespace cpcse
