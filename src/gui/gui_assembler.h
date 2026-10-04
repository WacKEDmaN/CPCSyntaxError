// CPCSyntaxError GUI — the built-in assembler: RASM (Edouard BERGE, MIT) itself, linked
// in as a library, with an editor window around it. Being RASM, every directive, macro,
// cruncher and expression it has works as it does from the command line; the only
// difference is where the result goes -- into the running machine's memory.
#pragma once
#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "asm_highlight.h"

struct ImFont;
struct ImGuiInputTextCallbackData;

namespace cpcse {

class EmuHost;
class Debugger;
struct FileBrowser;
struct SaveDialog;

class AssemblerWindow {
public:
    AssemblerWindow(EmuHost& host, Debugger& debugger, FileBrowser& browser, SaveDialog& saver);

    void draw(bool* open);
    bool focused = false;                       // the window (or its editor) has focus this frame
    std::function<void(int address)> showInDisassembly;

    void assemble(bool thenRun);                // F9 / Ctrl+F9
    void setSource(const std::string& text) { source = text; modified = true; sourceRevision++; }
    const std::string& resultSummary() const { return summary; }
    bool assembledOk() const { return lastOk; }
    int errorCount() const { return (int)messages.size(); }
    bool errorOnLine(int line) const { return errorLines.count(line) != 0; }
    std::string messageLog() const {
        std::string s;
        for (const auto& m : messages) s += "    [" + m.file + ":" + std::to_string(m.line) + "] " + m.text + "\n";
        return s;
    }
    void newFile();
    void openFile(const std::string& path);
    void saveFile(const std::string& path);
    void requestOpen();
    void requestSave(bool saveAs);

    void loadSettings(const std::map<std::string, std::string>& ini);
    void saveSettings(std::ostream& out) const;

private:
    EmuHost& host;
    Debugger& debugger;
    FileBrowser& browser;
    SaveDialog& saver;

    std::string source;
    std::string path;                           // "" = never saved
    bool modified = false;
    int syntax = 0;                             // RASM_SYNTAX_*
    bool brkLabels = true;                      // BRK labels become breakpoints, as rasm -eb
    int lastStart = -1, lastRun = -1, lastLength = 0;
    int lineCount = 1;

    struct Message { int line = 0; std::string file; std::string text; bool inEditor = false; };
    std::vector<Message> messages;
    std::set<int> errorLines;
    std::vector<std::pair<std::string, int>> symbols;
    std::string summary = "Nothing assembled yet.";
    bool lastOk = false;
    char symbolFilter[64] = {0};

    // Syntax colouring (asm_highlight.h): the text box draws its text invisibly and the
    // coloured spans are drawn over it, so editing, selection and undo stay ImGui's own.
    bool highlight = true;
    AsmColours colours = defaultAsmColours();
    uint64_t sourceRevision = 0, indexedRevision = ~0ull;
    std::vector<int> lineStarts;                // offset of each line in `source`
    std::vector<uint8_t> lineInComment;         // the line starts inside /* */
    void indexLines();
    void drawHighlighted(const char* childName, float lineH, float editorH);
    void coloursPopup();

    int gotoLine = -1;                          // move the editor's cursor here next frame
    static int editorCallback(ImGuiInputTextCallbackData* data);
    int lineStartOffset(int line) const;
    std::string displayName() const;
};

} // namespace cpcse
