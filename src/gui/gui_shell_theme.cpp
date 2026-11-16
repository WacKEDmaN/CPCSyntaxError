// CPCSyntaxError: Settings > Colours and themes (GuiShell::sectionTheme). Every colour of the
// GUI (gui_theme.h): the program's own named colours first, then Dear ImGui's, in groups. A
// change shows at once and goes into cpcse.ini; a theme file (.cpctheme) keeps a theme of its
// own, to load again or give to someone.
#include "gui_shell.h"

#include <cctype>
#include <filesystem>
#include <string>

#include <SDL.h>

#include "imgui.h"
#include "gui_theme.h"
#include "gui_widgets.h"

namespace cpcse {

namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// A theme's name as a file name: letters, digits, space, '-' and '_' kept, the rest '_'.
std::string fileNameFor(const std::string& name) {
    std::string out;
    for (char c : name) out += (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string("theme") : out;
}

bool sameColours(const GuiTheme& a, const GuiTheme& b) {
    for (int i = 0; i < ImGuiCol_COUNT; i++) if (colourToHex(a.imgui[i]) != colourToHex(b.imgui[i])) return false;
    for (int i = 0; i < APP_COLOUR_COUNT; i++) if (colourToHex(a.app[i]) != colourToHex(b.app[i])) return false;
    return true;
}

} // namespace

void GuiShell::scanThemeFiles() {
    themeFiles.clear();
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(themeDir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || lower(it->path().extension().u8string()) != ".cpctheme") continue;
        GuiTheme t;
        std::string why;
        const std::string path = it->path().u8string();
        themeFiles.push_back({ loadThemeFile(path, t, why) ? t.name : it->path().stem().u8string(), path });
    }
    std::sort(themeFiles.begin(), themeFiles.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
}

void GuiShell::useTheme(const GuiTheme& t) {
    theme = themeChosen = t;
    applyTheme(theme);
    host.status = "Theme: " + theme.name;
}

void GuiShell::sectionTheme(bool) {
    sectionHeading("Theme");
    fitNextItemWidth(280, "##theme");
    if (ImGui::BeginCombo("##theme", theme.name.c_str())) {
        scanThemeFiles();                                    // the folder as it is now
        ImGui::TextDisabled("Built in");
        for (const GuiTheme& t : builtInThemes())
            if (ImGui::Selectable(t.name.c_str(), theme.name == t.name)) useTheme(t);
        if (!themeFiles.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Theme files");
            for (const auto& f : themeFiles) {
                if (!ImGui::Selectable((f.first + "##" + f.second).c_str(), false)) continue;
                GuiTheme t;
                std::string why;
                if (loadThemeFile(f.second, t, why)) useTheme(t);
                else host.status = "Theme not loaded: " + why;
            }
        }
        ImGui::EndCombo();
    }
    {
        FlowRow row;
        if (row.smallButton("Save as a theme file..."))
            saver.open("Save theme", themeDir, fileNameFor(theme.name) + ".cpctheme", [this](const std::string& p) {
                std::error_code ec;
                fs::create_directories(fs::u8path(p).parent_path(), ec);
                std::string why;
                if (saveThemeFile(theme, p, why)) { themeChosen = theme; host.status = "Theme saved: " + p; }
                else host.status = "Theme not saved: " + why;
            });
        if (row.smallButton("Load a theme file..."))
            browser.open("Load theme", themeDir, { ".cpctheme" }, [this](const std::string& p) {
                GuiTheme t;
                std::string why;
                if (loadThemeFile(p, t, why)) useTheme(t);
                else host.status = "Theme not loaded: " + why;
            });
        if (row.smallButton("Open the themes folder")) {
            std::error_code ec;
            fs::create_directories(fs::u8path(themeDir), ec);
            const std::string url = "file:///" + fs::absolute(fs::u8path(themeDir), ec).generic_u8string();
            SDL_OpenURL(url.c_str());
        }
        const bool changed = !sameColours(theme, themeChosen);
        ImGui::BeginDisabled(!changed);
        if (row.smallButton("Undo the changes")) { theme = themeChosen; applyTheme(theme); }
        ImGui::EndDisabled();
    }
    char name[65] = {};
    theme.name.copy(name, sizeof(name) - 1);
    fitNextItemWidth(280, "##themename");
    if (ImGui::InputTextWithHint("##themename", "the theme's name", name, sizeof(name))) theme.name = name;
    textWrappedDisabled("A change shows at once, and cpcse.ini keeps it. Save as a theme file to keep the colours "
                        "as a theme of their own, or to give them to someone. Click a colour to change it.");

    ImGui::Separator();
    fitNextItemWidth(220, "##colourfilter");
    ImGui::InputTextWithHint("##colourfilter", "filter: a colour's name", themeFilter, sizeof(themeFilter));
    const std::string filter = lower(themeFilter);
    auto matches = [&](const std::string& label) { return filter.empty() || lower(label).find(filter) != std::string::npos; };
    bool changed = false;
    // one colour: its swatch (click: the picker), its name, and Reset when it is not the theme's
    auto colourRow = [&](const char* id, const std::string& label, ImVec4& c, const ImVec4& chosen) {
        ImGui::PushID(id);
        if (ImGui::ColorEdit4("##colour", &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                                                ImGuiColorEditFlags_AlphaPreviewHalf))
            changed = true;
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
        if (colourToHex(c) != colourToHex(chosen)) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) { c = chosen; changed = true; }
        }
        ImGui::PopID();
    };

    int shown = 0;
    {
        int n = 0;
        for (int i = 0; i < APP_COLOUR_COUNT; i++) n += matches(appColourLabel(i));
        if (n) {
            if (!filter.empty()) ImGui::SetNextItemOpen(true);
            if (ImGui::CollapsingHeader("Program colours", ImGuiTreeNodeFlags_DefaultOpen))
                for (int i = 0; i < APP_COLOUR_COUNT; i++)
                    if (matches(appColourLabel(i))) { colourRow(appColourKey(i), appColourLabel(i), theme.app[i], themeChosen.app[i]); shown++; }
        }
    }
    for (const char* group : imguiColourGroups()) {
        int n = 0;
        for (int i = 0; i < ImGuiCol_COUNT; i++) n += std::string(imguiColourGroup(i)) == group && matches(imguiColourLabel(i));
        if (!n) continue;
        if (!filter.empty()) ImGui::SetNextItemOpen(true);
        if (!ImGui::CollapsingHeader(group)) continue;
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            if (std::string(imguiColourGroup(i)) != group || !matches(imguiColourLabel(i))) continue;
            colourRow(ImGui::GetStyleColorName(i), imguiColourLabel(i), theme.imgui[i], themeChosen.imgui[i]);
            shown++;
        }
    }
    if (!filter.empty() && shown == 0) textWrappedDisabled("No colour has \"%s\" in its name.", themeFilter);
    if (changed) applyTheme(theme);
}

} // namespace cpcse
