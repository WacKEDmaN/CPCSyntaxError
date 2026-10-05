// CPCSyntaxError GUI — small pieces shared by the shell's windows.
#pragma once
#include <functional>
#include <initializer_list>
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

// A ROW THAT WRAPS: each item goes on the line of the one before when it fits in the
// window's width, else on the next -- so toolbars and button rows stay whole in a narrow
// window instead of running off its edge. Each call places one item; `place(w)` places
// anything else of width w (call it just before drawing that item).
struct FlowRow {
    bool first = true;
    float spacing = -1.0f;                     // between items; -1 = the style's
    void place(float width);
    bool button(const char* label, float width = 0.0f);   // width 0: the label's own
    bool smallButton(const char* label);
    bool checkbox(const char* label, bool* v);
    bool radio(const char* label, bool active);
    void text(const char* text);               // one piece of text, kept whole
    void textDisabled(const char* text);
    void textColored(const ImVec4& colour, const char* text);
    // An item whose width is set with SetNextItemWidth: place it, then draw it.
    void item(float width) { place(width); ImGui::SetNextItemWidth(width); }
    // An input, combo or slider `fieldWidth` wide with its label after it: placed, its
    // width set; the caller draws it next.
    void field(float fieldWidth, const char* label) {
        const float text = ImGui::CalcTextSize(label, nullptr, true).x;
        place(fieldWidth + (text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f));
        ImGui::SetNextItemWidth(fieldWidth);
    }
    void newRow() { first = true; }
};
// A row that goes on from the item just drawn: `if (after().smallButton("Get it..."))`
// is SameLine-and-button that moves to the next line when the button would not fit.
inline FlowRow after() { FlowRow r; r.first = false; return r; }
// Text in this window wraps at its edge (Text, TextDisabled, TextColored...), from here
// to the end of the scope -- which must close before the window's End(). Child windows
// (lists, hex views, editors) keep their own unwrapped lines.
struct WrapText {
    WrapText() { ImGui::PushTextWrapPos(0.0f); }
    ~WrapText() { ImGui::PopTextWrapPos(); }
    WrapText(const WrapText&) = delete;
    WrapText& operator=(const WrapText&) = delete;
};

// LEDs in columns `column` wide, as many to a row as the window has room for.
struct LedItem {
    bool on; const char* label;
    LedItem(long long v, const char* l) : on(v != 0), label(l) {}
};
void ledGrid(std::initializer_list<LedItem> items, float column);
// Text that wraps at the window's edge (Text/TextDisabled/TextColored do not by default).
void textWrappedDisabled(const char* fmt, ...);

// Writes a file whole or not at all: to a temporary file beside it, then renamed over it,
// so a failed write (a full disc) leaves the old file as it was instead of cut short.
// false (and `why`) when it could not.
bool writeFileSafely(const std::string& path, const void* data, size_t size, std::string* why = nullptr);

// A modal file picker (no OS dialog dependency).
// The drives a path can start on ("C:\", "D:\" ...): Windows' lettered drives; elsewhere
// just "/".
std::vector<std::string> fileSystemRoots();

// The folder part of both dialogs: the drives (on Windows), the folder's path, [..], its
// folders (a click goes in) and the files with one of `exts` (any when empty). The listing
// is read when the folder changes, and once a second: not every frame -- a folder of
// thousands of files, or one on a network drive, would stall the UI.
struct FolderView {
    std::string listedDir;
    double listedAt = -1.0;
    std::vector<std::string> dirNames, fileNames;
    // Draws it in a child `height` high (<= 0: what is left above `reserve` pixels); a file
    // clicked returns true with its name (and whether it was a double click).
    bool draw(std::string& cwd, const std::vector<std::string>& exts, const char* selected,
              float reserve, std::string& clicked, bool& doubleClicked);
    void relist() { listedDir.clear(); }
};

// What the dialogs make of a typed name: a folder (absolute, relative, or "D:") to go into,
// or a file to take. True when `typed` named a folder, and `cwd` is now it.
bool enterTypedFolder(std::string& cwd, const std::string& typed);

struct FileBrowser {
    bool visible = false;
    bool requestOpen = false;      // defer OpenPopup to draw() so it shares the modal's ID scope
    bool pickDir = false;          // choose a folder rather than a file
    std::string title = "Open";
    std::string cwd;
    std::vector<std::string> exts;
    std::function<void(const std::string&)> onPick;
    char nameBuf[512] = {0};
    FolderView view;
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
    FolderView view;
    void open(const std::string& t, const std::string& startDir, const std::string& defName,
              std::function<void(const std::string&)> cb);
    void draw();
};

} // namespace cpcse
