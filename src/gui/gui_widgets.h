// CPCSyntaxError GUI — small pieces shared by the shell's windows.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

namespace cpcse {

// The front end's one accent colour (titles, the PC, section names).
inline const ImVec4 kAccent(1.0f, 0.78f, 0.25f, 1.0f);
inline const ImVec4 kPcColour(1.0f, 0.85f, 0.2f, 1.0f);

// A muted, spaced section label.
void sectionHeading(const char* text);
ImVec4 rgbToVec(int rgb);
// A two-column "name  value" row inside a table started with beginFacts().
bool beginFacts(const char* id, float nameWidth = 110.0f);
void fact(const char* name, const char* fmt, ...);
void endFacts();
// A small LED, lit or not, followed by its label on the same line.
void led(bool on, const char* label = nullptr);

// A modal file picker (no OS dialog dependency).
struct FileBrowser {
    bool visible = false;
    bool requestOpen = false;      // defer OpenPopup to draw() so it shares the modal's ID scope
    bool pickDir = false;          // choose a folder rather than a file
    std::string title = "Open";
    std::string cwd;
    std::vector<std::string> exts;
    std::function<void(const std::string&)> onPick;
    char nameBuf[512] = {0};
    void open(const std::string& t, const std::string& startDir,
              std::vector<std::string> extensions, std::function<void(const std::string&)> cb);
    void openDir(const std::string& t, const std::string& startDir, std::function<void(const std::string&)> cb);
    void draw();
};

struct SaveDialog {
    bool visible = false;
    bool requestOpen = false;
    std::string title = "Save", dir;
    char nameBuf[256] = {0};
    std::function<void(const std::string&)> onSave;
    void open(const std::string& t, const std::string& startDir, const std::string& defName,
              std::function<void(const std::string&)> cb);
    void draw();
};

} // namespace cpcse
