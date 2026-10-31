// CPCSyntaxError GUI — small pieces shared by the shell's windows.
#pragma once
#include "core/file_write.h"
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <ostream>
#include <string>
#include <cstdint>
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
    // A field narrower than asked when even a line of its own is too short for it: never
    // past the line's end, never under three characters.
    static float fitted(float width, float besideIt) {
        return std::max(ImGui::GetFontSize() * 3.0f, std::min(width, ImGui::GetContentRegionAvail().x - besideIt));
    }
    void item(float width) { place(width); ImGui::SetNextItemWidth(fitted(width, 0.0f)); }
    // An input, combo or slider `fieldWidth` wide with its label after it: placed, its
    // width set (less in a window too narrow for it); the caller draws it next.
    void field(float fieldWidth, const char* label) {
        const float text = ImGui::CalcTextSize(label, nullptr, true).x;
        const float labelW = text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f;
        place(fieldWidth + labelW);
        ImGui::SetNextItemWidth(fitted(fieldWidth, labelW));
    }
    void newRow() { first = true; }
};
// The next item's width: `width`, or less where what is left of the line (with the item's
// label after it) is shorter; never under three characters. For a combo, input or slider
// that would otherwise run past a narrow window's edge.
inline void fitNextItemWidth(float width, const char* label = "") {
    const float text = ImGui::CalcTextSize(label, nullptr, true).x;
    ImGui::SetNextItemWidth(FlowRow::fitted(width, text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f));
}
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
// `bits` LEDs for a value, the highest bit first, in a row that wraps in a narrow window.
void ledRow(int value, int bits);
// Text that wraps at the window's edge (Text/TextDisabled/TextColored do not by default).
void textWrappedDisabled(const char* fmt, ...);
// A checkbox whose label wraps at the window's edge (ImGui's label is always one line, so a
// long one runs off a narrow window). A click on the label toggles it, as on ImGui's. A
// "##" in the label starts the part that is only an ID, as in ImGui.
bool checkboxWrapped(const char* label, bool* v);

// THE PICTURE THROUGH A SLOT MASK, as a colour tube shows it (the CTM's 3701B22 is a
// slot-mask tube): src (sw x sh, 0xAABBGGRR) drawn w x h. Each slot of the mask lets the three
// beams onto three phosphor stripes side by side -- red, green, blue, each `stripe` pixels
// wide, so a triad is 3 x stripe -- in slots 3 x stripe high, with a dark bridge of `stripe`
// rows between slots; the slots of neighbouring triads are staggered by half that period.
// From 3 pixels a stripe, a 1-pixel black matrix separates the stripes. A PHOSPHOR SLOT IS
// LIT WHOLE: its one colour, at the average of the picture's colour over the slot (no slot is
// ever half lit), driven harder by the light the mask stops (clipped at full). strength 0..1
// mixes this with the picture as it is (0: the picture unchanged). turned: the pattern turned
// 90 degrees (stripes across; x and y swapped).
void slotMaskRender(const uint32_t* src, int sw, int sh, int w, int h, float strength, int stripe, bool turned,
                    std::vector<uint32_t>& out);

// THE SOFT SLOT MASK, the look ACE-DL gives a CTM: each pixel keeps its colour whole, under
// fine dark lines (one column in three stripes) and faint staggered slot bridges, brightened
// back by the light they take (clipped at full). strength: how dark the lines are (bridges
// half of it); stripe: the lines' spacing (a line every 3 x stripe pixels); turned: across.
void softMaskRender(const uint32_t* src, int sw, int sh, int w, int h, float strength, int stripe, bool turned,
                    std::vector<uint32_t>& out);

// The beam's spot is round and soft, so a pixel's edges on a tube are not hard: the picture
// (w x h, RGBA) through a Gaussian blur, across and down. amount 0..1 gives a spread (sigma)
// of 0 to 1 source pixel: at 1, about one mode 2 pixel across and half a scanline down.
void softenRender(const uint32_t* src, int w, int h, float amount, std::vector<uint32_t>& out);

// writeFileSafely: core/file_write.h (in the core, as the check programs use it without the GUI)

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

// WHERE EACH KIND OF DIALOG WAS LAST. Keyed by what the dialog is for: a file dialog by
// its first extension (so every disc dialog shares one folder, every tape dialog another),
// a save dialog by its default name's extension (so "Save snapshot" shares with "Load
// snapshot"), a folder dialog or one with no extension by its title. A dialog writes its folder here while it is
// open, so even a cancelled browse is kept; the next open of that kind starts there if the
// folder still exists, else at the caller's folder. Saved in the .ini as dir.<key>=<path>.
std::map<std::string, std::string>& rememberedFolders();
std::string dialogFolderKey(const std::string& title, const std::vector<std::string>& exts);
// The folder to start in: the remembered one if it still exists, else `fallback`.
std::string dialogStartFolder(const std::string& key, const std::string& fallback);
void loadRememberedFolders(const std::map<std::string, std::string>& ini);
void saveRememberedFolders(std::ostream& out);

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
    std::string folderKey;         // rememberedFolders() key of this open
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
    std::string folderKey;
    void open(const std::string& t, const std::string& startDir, const std::string& defName,
              std::function<void(const std::string&)> cb);
    void draw();
};

} // namespace cpcse
