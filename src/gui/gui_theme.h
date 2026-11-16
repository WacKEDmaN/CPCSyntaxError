// CPCSyntaxError: the colours of the GUI (Window > Colours and themes).
//
// A theme is every colour Dear ImGui draws with (ImGuiCol_*: text, backgrounds, borders,
// input fields, title bars, menus, scroll bars, buttons, headers, tabs, tables ...) and the
// program's own named colours (the accent, the debugger's PC line, and the green, red, amber
// and blue of status texts: kAccent, kPcColour, kGood, kBad, kWarn, kInfo in gui_widgets.h).
//
// A theme file (.cpctheme, in the themes folder) is text, one colour a line:
//     # a comment
//     name=My theme
//     base=CPCSyntaxError          (a built-in theme: the colours the file does not name)
//     Text=#E6E6E6FF               (Dear ImGui's own name for the colour, #RRGGBBAA)
//     Program.Accent=#FFC740FF
// A line the reader does not know is passed over, so a file from another version still loads.
#pragma once
#include <string>
#include <vector>

#include "imgui.h"

namespace cpcse {

enum AppColour { APP_ACCENT, APP_PC, APP_GOOD, APP_BAD, APP_WARN, APP_INFO, APP_COLOUR_COUNT };

struct GuiTheme {
    std::string name;
    std::string base;          // the built-in theme it starts from (a built-in: its own name)
    ImVec4 imgui[ImGuiCol_COUNT];
    ImVec4 app[APP_COLOUR_COUNT];
};

// The key of a program colour in a theme file ("Accent" ...) and what it is for.
const char* appColourKey(int i);
const char* appColourLabel(int i);
// A colour's name as the window shows it: Dear ImGui's name in words ("Title bg active" ->
// "Title background, active").
std::string imguiColourLabel(int i);
// The group a Dear ImGui colour is shown in (Text, Backgrounds, Borders ...).
const char* imguiColourGroup(int i);
std::vector<const char*> imguiColourGroups();

// The themes in the program; the first is the default (the look the GUI always had).
const std::vector<GuiTheme>& builtInThemes();
const GuiTheme* builtInTheme(const std::string& name);

std::string colourToHex(const ImVec4& c);
bool hexToColour(const std::string& text, ImVec4& out);

// A theme as file text, and back. fromText starts from the built-in theme its "base=" names
// (else the default), then takes every colour the text names; false (and why) when the text
// names no colour at all.
std::string themeToText(const GuiTheme& t);
bool themeFromText(const std::string& text, GuiTheme& out, std::string& why);
bool saveThemeFile(const GuiTheme& t, const std::string& path, std::string& why);
bool loadThemeFile(const std::string& path, GuiTheme& out, std::string& why);

// The theme into Dear ImGui's style and the program colours.
void applyTheme(const GuiTheme& t);

} // namespace cpcse
