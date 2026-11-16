// CPCSyntaxError GUI — the BASIC window, beside the Assembler: a Locomotive BASIC program as
// text. It lists the program in the machine (core/basic.h's decoder), edits it, and puts it
// back into the machine at once: tokenized as BASIC tokenizes a typed line (checked byte for
// byte against BASIC itself by cpcse-basic-check) and written at &0170 with BASIC's pointers
// set (EmuHost::injectBasic), optionally followed by a typed RUN.
#pragma once
#include <map>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "core/basic.h"

struct ImGuiInputTextCallbackData;

namespace cpcse {

class EmuHost;
struct FileBrowser;
struct SaveDialog;

class BasicWindow {
public:
    BasicWindow(EmuHost& host, FileBrowser& browser, SaveDialog& saver);
    void draw(bool* open);
    bool focused = false;                       // the window (or its editor) has focus this frame

    bool fromMachine();                         // the program in memory into the editor
    bool intoMachine(bool thenRun);             // the editor's program into memory (and RUN)
    void newFile();
    void openFile(const std::string& path);
    void saveFile(const std::string& path);
    void requestOpen();
    void requestSave(bool saveAs);
    void setSource(const std::string& text) {
        source = text;
        if (editorActive) { editPending = true; editText = source; editSelA = editSelB = 0; }
        findA = findB = -1; selA = selB = 0;
    }
    const std::string& text() const { return source; }
    const std::string& status() const { return message; }

    void loadSettings(const std::map<std::string, std::string>& ini);
    void saveSettings(std::ostream& out) const;

    // FIND AND REPLACE (user 2026-11-17, "add em all"): Ctrl+F, Ctrl+H, F3 / Shift+F3, Enter
    // in the box. A replace keeps the text before it: "Undo the replace" gives it back.
    void setFind(const std::string& what, const std::string& with, bool matchCase, bool wholeWord);
    bool findNext(bool backward);
    bool replaceOne();
    int replaceAll();
    bool undoReplace();
    int matchStart() const { return findA; }
    const std::string& findStatus() const { return findMessage; }

private:
    bool findOpen = false, replaceOpen = false, findFocus = false, findCase = false, findWord = false;
    char findText[128] = {0}, replaceText[128] = {0};
    int findA = -1, findB = -1;
    std::string findMessage;
    int selA = 0, selB = 0;                     // the editor's selection, as its callback last saw it
    bool editorActive = false, focusEditor = false;
    bool editPending = false, selectPending = false;
    std::string editText;
    int editSelA = 0, editSelB = 0;
    std::string beforeReplace;                  // the text before the last replace
    bool canUndoReplace = false;
    std::vector<std::pair<int, int>> matches() const;
    void select(int a, int b);
    void changeText(std::string text, int a, int b);
    void findBar();
    void drawMatches(const char* childName);
    static int editorCallback(ImGuiInputTextCallbackData* data);
    EmuHost& host;
    FileBrowser& browser;
    SaveDialog& saver;
    BasicDecoder basic;
    std::string source = "10 PRINT \"HELLO\"\n";
    std::string path;                           // "" = never saved
    std::string message = "Type a program, or take the one in the machine (From the machine).";
    bool error = false;
    int tabRequest = -1;                        // 0 Editor, 1 In memory: brought to the front once
    void toolbar();
    void editorTab();
    void memoryTab();
};

} // namespace cpcse
