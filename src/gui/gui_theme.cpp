// CPCSyntaxError: see gui_theme.h.
#include "gui_theme.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "core/file_write.h"
#include "gui_widgets.h"

namespace cpcse {

namespace {

const char* const APP_KEYS[APP_COLOUR_COUNT] = { "Accent", "ProgramCounter", "Good", "Bad", "Warning", "Info" };
const char* const APP_LABELS[APP_COLOUR_COUNT] = {
    "Accent (titles, section names, the selected tab's line)",
    "Program counter (the debugger's current line)",
    "Good (a status that is right: green)",
    "Bad (an error: red)",
    "Warning (amber)",
    "Information (blue)",
};

ImVec4 rgb(int v, float a = 1.0f) { return ImVec4(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, a); }
ImVec4 withAlpha(ImVec4 c, float a) { c.w = a; return c; }

// Dear ImGui's colours of the moment as a theme (after one of its StyleColors* calls).
GuiTheme fromStyle(const char* name, const ImGuiStyle& s) {
    GuiTheme t;
    t.name = name;
    for (int i = 0; i < ImGuiCol_COUNT; i++) t.imgui[i] = s.Colors[i];
    t.imgui[ImGuiCol_WindowBg].w = 1.0f;             // windows that leave the main window are opaque
    return t;
}

void darkProgramColours(GuiTheme& t) {
    t.app[APP_ACCENT] = ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
    t.app[APP_PC] = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
    t.app[APP_GOOD] = ImVec4(0.45f, 0.9f, 0.45f, 1.0f);
    t.app[APP_BAD] = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
    t.app[APP_WARN] = ImVec4(0.95f, 0.8f, 0.35f, 1.0f);
    t.app[APP_INFO] = ImVec4(0.55f, 0.8f, 1.0f, 1.0f);
}

// A theme from a few colours: the backgrounds, the text, the controls and the accent. Every
// Dear ImGui colour comes from these, as the dark theme's come from its own few.
GuiTheme paletteTheme(const char* name, ImVec4 bg, ImVec4 bgDeep, ImVec4 text, ImVec4 textDim, ImVec4 control,
                      ImVec4 controlHot, ImVec4 controlActive, ImVec4 accent) {
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    GuiTheme t = fromStyle(name, s);
    ImVec4* c = t.imgui;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = withAlpha(bgDeep, 0.96f);
    c[ImGuiCol_Border] = withAlpha(textDim, 0.5f);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = withAlpha(control, 0.54f);
    c[ImGuiCol_FrameBgHovered] = withAlpha(controlHot, 0.4f);
    c[ImGuiCol_FrameBgActive] = withAlpha(controlActive, 0.67f);
    c[ImGuiCol_TitleBg] = bgDeep;
    c[ImGuiCol_TitleBgActive] = control;
    c[ImGuiCol_TitleBgCollapsed] = withAlpha(bgDeep, 0.51f);
    c[ImGuiCol_MenuBarBg] = bgDeep;
    c[ImGuiCol_ScrollbarBg] = withAlpha(bgDeep, 0.53f);
    c[ImGuiCol_ScrollbarGrab] = withAlpha(control, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = controlHot;
    c[ImGuiCol_ScrollbarGrabActive] = controlActive;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_CheckboxSelectedBg] = withAlpha(control, 0.54f);
    c[ImGuiCol_TreeLines] = withAlpha(textDim, 0.5f);
    c[ImGuiCol_DragDropTargetBg] = withAlpha(accent, 0.2f);
    c[ImGuiCol_UnsavedMarker] = text;
    c[ImGuiCol_SliderGrab] = withAlpha(accent, 0.8f);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = withAlpha(control, 0.6f);
    c[ImGuiCol_ButtonHovered] = controlHot;
    c[ImGuiCol_ButtonActive] = controlActive;
    c[ImGuiCol_Header] = withAlpha(control, 0.5f);
    c[ImGuiCol_HeaderHovered] = withAlpha(controlHot, 0.8f);
    c[ImGuiCol_HeaderActive] = controlActive;
    c[ImGuiCol_Separator] = withAlpha(textDim, 0.5f);
    c[ImGuiCol_SeparatorHovered] = withAlpha(controlHot, 0.78f);
    c[ImGuiCol_SeparatorActive] = controlActive;
    c[ImGuiCol_ResizeGrip] = withAlpha(control, 0.2f);
    c[ImGuiCol_ResizeGripHovered] = withAlpha(controlHot, 0.67f);
    c[ImGuiCol_ResizeGripActive] = withAlpha(controlActive, 0.95f);
    c[ImGuiCol_InputTextCursor] = text;
    c[ImGuiCol_TabHovered] = controlHot;
    c[ImGuiCol_Tab] = withAlpha(control, 0.86f);
    c[ImGuiCol_TabSelected] = controlActive;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = withAlpha(bgDeep, 0.97f);
    c[ImGuiCol_TabDimmedSelected] = withAlpha(control, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(accent, 0.5f);
    c[ImGuiCol_DockingPreview] = withAlpha(controlHot, 0.7f);
    c[ImGuiCol_DockingEmptyBg] = bgDeep;
    c[ImGuiCol_PlotLines] = textDim;
    c[ImGuiCol_PlotLinesHovered] = accent;
    c[ImGuiCol_PlotHistogram] = withAlpha(accent, 0.8f);
    c[ImGuiCol_PlotHistogramHovered] = accent;
    c[ImGuiCol_TableHeaderBg] = withAlpha(control, 0.8f);
    c[ImGuiCol_TableBorderStrong] = withAlpha(textDim, 0.7f);
    c[ImGuiCol_TableBorderLight] = withAlpha(textDim, 0.35f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = withAlpha(text, 0.06f);
    c[ImGuiCol_TextLink] = controlHot;
    c[ImGuiCol_TextSelectedBg] = withAlpha(controlHot, 0.35f);
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_NavCursor] = controlHot;
    c[ImGuiCol_NavWindowingHighlight] = withAlpha(text, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = withAlpha(bgDeep, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = withAlpha(bgDeep, 0.35f);
    darkProgramColours(t);
    t.app[APP_ACCENT] = accent;
    return t;
}

std::vector<GuiTheme> makeBuiltIns() {
    std::vector<GuiTheme> out;
    ImGuiStyle s;
    // 1 the look the GUI always had: Dear ImGui's dark theme with the orange accent
    ImGui::StyleColorsDark(&s);
    GuiTheme dark = fromStyle("CPCSyntaxError", s);
    darkProgramColours(dark);
    dark.imgui[ImGuiCol_TabSelectedOverline] = dark.app[APP_ACCENT];
    out.push_back(dark);
    // 2 Dear ImGui's light theme; the program colours dark enough to read on white
    ImGui::StyleColorsLight(&s);
    GuiTheme light = fromStyle("Light", s);
    light.app[APP_ACCENT] = ImVec4(0.72f, 0.42f, 0.0f, 1.0f);
    light.app[APP_PC] = ImVec4(0.65f, 0.45f, 0.0f, 1.0f);
    light.app[APP_GOOD] = ImVec4(0.1f, 0.5f, 0.1f, 1.0f);
    light.app[APP_BAD] = ImVec4(0.8f, 0.15f, 0.1f, 1.0f);
    light.app[APP_WARN] = ImVec4(0.65f, 0.42f, 0.0f, 1.0f);
    light.app[APP_INFO] = ImVec4(0.1f, 0.35f, 0.75f, 1.0f);
    light.imgui[ImGuiCol_TabSelectedOverline] = light.app[APP_ACCENT];
    out.push_back(light);
    // 3 Dear ImGui's classic theme
    ImGui::StyleColorsClassic(&s);
    GuiTheme classic = fromStyle("Classic", s);
    darkProgramColours(classic);
    out.push_back(classic);
    // 4 the CPC's own: BASIC's blue paper, bright yellow pen, the firmware's blues and cyan
    GuiTheme cpc = paletteTheme("CPC 6128", rgb(0x000080), rgb(0x000050), rgb(0xffff80), rgb(0xb0b070),
                                rgb(0x0000ff), rgb(0x0080ff), rgb(0x00c0ff), rgb(0xff8000));
    cpc.app[APP_PC] = rgb(0xffff00);
    cpc.app[APP_GOOD] = rgb(0x80ff80);
    cpc.app[APP_BAD] = rgb(0xff8080);
    cpc.app[APP_WARN] = rgb(0xffff00);
    cpc.app[APP_INFO] = rgb(0x80ffff);
    out.push_back(cpc);
    // 5 the green screen of the GT64 and GT65 monitors
    GuiTheme green = paletteTheme("Green screen", rgb(0x020a02), rgb(0x000500), rgb(0x55ff55), rgb(0x2f8f2f),
                                  rgb(0x0c3a0c), rgb(0x1a6a1a), rgb(0x2a9a2a), rgb(0x9cff9c));
    green.app[APP_PC] = rgb(0xc8ffc8);
    green.app[APP_GOOD] = rgb(0x9cff9c);
    green.app[APP_BAD] = rgb(0xffffff);
    green.app[APP_WARN] = rgb(0xc8ffc8);
    green.app[APP_INFO] = rgb(0x70e070);
    out.push_back(green);
    for (GuiTheme& t : out) t.base = t.name;
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
    return s.substr(a);
}

} // namespace

const char* appColourKey(int i) { return i >= 0 && i < APP_COLOUR_COUNT ? APP_KEYS[i] : ""; }
const char* appColourLabel(int i) { return i >= 0 && i < APP_COLOUR_COUNT ? APP_LABELS[i] : ""; }

std::string imguiColourLabel(int i) {
    const char* raw = ImGui::GetStyleColorName(i);
    std::string words;
    for (const char* p = raw; *p; p++) {
        if (p != raw && std::isupper((unsigned char)*p)) words += ' ';
        words += *p;
    }
    // "Bg" is a background; the first word keeps its capital, the rest are lower case
    std::string out;
    std::istringstream in(words);
    for (std::string w; in >> w;) {
        if (w == "Bg") w = "background";
        if (!out.empty()) for (char& c : w) c = (char)std::tolower((unsigned char)c);
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

const char* imguiColourGroup(int i) {
    const std::string n = ImGui::GetStyleColorName(i);
    auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
    if (starts("Text")) return "Text";
    if (starts("WindowBg") || starts("ChildBg") || starts("PopupBg") || starts("MenuBarBg") || starts("ModalWindowDimBg"))
        return "Backgrounds";
    if (starts("Border") || starts("TableBorder")) return "Borders";
    if (starts("FrameBg") || starts("CheckMark") || starts("Checkbox") || starts("SliderGrab") || starts("InputText"))
        return "Fields and sliders";
    if (starts("TitleBg")) return "Title bars";
    if (starts("Scrollbar")) return "Scroll bars";
    if (starts("Button")) return "Buttons";
    if (starts("Header")) return "Headers and menus";
    if (starts("Separator") || starts("ResizeGrip")) return "Separators, grips";
    if (starts("Table")) return "Tables";                // before "Tab": "Table..." starts with it
    if (starts("Tab")) return "Tabs";
    if (starts("Docking")) return "Docking";
    if (starts("Plot")) return "Graphs";
    return "Other";
}

std::vector<const char*> imguiColourGroups() {
    return { "Text", "Backgrounds", "Borders", "Fields and sliders", "Title bars", "Scroll bars", "Buttons",
             "Headers and menus", "Separators, grips", "Tabs", "Docking", "Graphs", "Tables",
             "Other" };
}

const std::vector<GuiTheme>& builtInThemes() {
    static const std::vector<GuiTheme> themes = makeBuiltIns();
    return themes;
}

const GuiTheme* builtInTheme(const std::string& name) {
    for (const GuiTheme& t : builtInThemes()) if (t.name == name) return &t;
    return nullptr;
}

std::string colourToHex(const ImVec4& c) {
    auto b = [](float v) { return (int)(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f); };
    char s[16];
    std::snprintf(s, sizeof s, "#%02X%02X%02X%02X", b(c.x), b(c.y), b(c.z), b(c.w));
    return s;
}

bool hexToColour(const std::string& textIn, ImVec4& out) {
    std::string t = trim(textIn);
    if (!t.empty() && t[0] == '#') t.erase(0, 1);
    if (t.size() != 6 && t.size() != 8) return false;
    for (char c : t) if (!std::isxdigit((unsigned char)c)) return false;
    const unsigned long v = std::strtoul(t.c_str(), nullptr, 16);
    if (t.size() == 6) out = ImVec4(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, 1.0f);
    else out = ImVec4(((v >> 24) & 255) / 255.0f, ((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f);
    return true;
}

std::string themeToText(const GuiTheme& t) {
    std::ostringstream o;
    o << "# CPCSyntaxError GUI theme (Window > Colours and themes). Colours are #RRGGBBAA.\n";
    o << "name=" << t.name << "\n";
    if (!t.base.empty()) o << "base=" << t.base << "\n";
    for (int i = 0; i < APP_COLOUR_COUNT; i++) o << "Program." << APP_KEYS[i] << "=" << colourToHex(t.app[i]) << "\n";
    for (int i = 0; i < ImGuiCol_COUNT; i++) o << ImGui::GetStyleColorName(i) << "=" << colourToHex(t.imgui[i]) << "\n";
    return o.str();
}

bool themeFromText(const std::string& text, GuiTheme& out, std::string& why) {
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    const GuiTheme* base = nullptr;
    if (auto b = kv.find("base"); b != kv.end()) base = builtInTheme(b->second);
    out = base ? *base : builtInThemes().front();
    out.base = out.name;                                  // the built-in it started from
    out.name = kv.count("name") && !kv["name"].empty() ? kv["name"].substr(0, 64) : "Unnamed theme";
    int taken = 0;
    for (int i = 0; i < ImGuiCol_COUNT; i++)
        if (auto it = kv.find(ImGui::GetStyleColorName(i)); it != kv.end() && hexToColour(it->second, out.imgui[i])) taken++;
    for (int i = 0; i < APP_COLOUR_COUNT; i++)
        if (auto it = kv.find(std::string("Program.") + APP_KEYS[i]); it != kv.end() && hexToColour(it->second, out.app[i])) taken++;
    if (taken == 0) { why = "it names no colour (lines such as Text=#E6E6E6FF)"; return false; }
    return true;
}

bool saveThemeFile(const GuiTheme& t, const std::string& path, std::string& why) {
    const std::string text = themeToText(t);
    return writeFileSafely(path, text.data(), text.size(), &why);
}

bool loadThemeFile(const std::string& path, GuiTheme& out, std::string& why) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { why = "could not read " + path; return false; }
    std::string text;
    char buf[4096];
    // a theme is a few kilobytes: a large file is not one
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        text.append(buf, (size_t)f.gcount());
        if (text.size() > (1u << 20)) { why = "the file is too large to be a theme"; return false; }
    }
    return themeFromText(text, out, why);
}

void applyTheme(const GuiTheme& t) {
    ImGuiStyle& s = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; i++) s.Colors[i] = t.imgui[i];
    kAccent = t.app[APP_ACCENT];
    kPcColour = t.app[APP_PC];
    kGood = t.app[APP_GOOD];
    kBad = t.app[APP_BAD];
    kWarn = t.app[APP_WARN];
    kInfo = t.app[APP_INFO];
}

} // namespace cpcse
