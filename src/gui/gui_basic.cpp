// CPCSyntaxError GUI — the BASIC window. See gui_basic.h.
#include "gui_basic.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "imgui.h"
#include "imgui_internal.h"   // the editor's child window, for the match marks
#include "emuhost.h"
#include "gui_widgets.h"
#include "core/emulator.h"
#include "core/memory.h"

namespace cpcse {

BasicWindow::BasicWindow(EmuHost& h, FileBrowser& b, SaveDialog& s) : host(h), browser(b), saver(s) {}

bool BasicWindow::fromMachine() {
    int end = 0;
    bool basic10 = false;
    if (!host.basicProgramEnd(end, basic10)) {
        message = "No BASIC program to take: BASIC is not at its Ready prompt.";
        error = true;
        return false;
    }
    const Bytes& ram = host.emu->memory->ram;
    std::string text;
    for (const BasicLine& l : basic.listProgram([&](int a) { return (int)ram[(size_t)(a & 0xffff)]; }, BASIC_PROGRAM_START))
        text += std::to_string(l.num) + " " + l.text + "\n";
    source = text;
    message = text.empty() ? "The machine has no program (BASIC " + std::string(basic10 ? "1.0" : "1.1") + ")."
                           : "Taken from the machine (BASIC " + std::string(basic10 ? "1.0" : "1.1") + ", " +
                                 std::to_string(end - BASIC_PROGRAM_START) + " bytes).";
    error = false;
    tabRequest = 0;
    return true;
}

bool BasicWindow::intoMachine(bool thenRun) {
    std::string why;
    const Bytes program = basic.tokenizeProgram(source, why);
    if (program.empty()) { message = "Not put in: " + why; error = true; return false; }
    if (!host.injectBasic(program, why)) { message = "Not put in: " + why; error = true; return false; }
    if (thenRun) host.typeText("RUN\n");
    message = "In the machine: " + std::to_string(program.size()) + " bytes" + (thenRun ? ", RUN typed." : ". Type RUN to start it.");
    error = false;
    return true;
}

void BasicWindow::newFile() {
    source.clear();
    path.clear();
    message = "A new program.";
    error = false;
}

// An ASCII .bas or a tokenized one as BASIC saves it, with or without an AMSDOS header
// (BasicDecoder::fileToText). A tokenized file opens as its listing; Save writes text.
void BasicWindow::openFile(const std::string& p) {
    std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
    if (!f) { message = "Cannot open " + p; error = true; return; }
    std::error_code ec;
    if (std::filesystem::file_size(std::filesystem::u8path(p), ec) > (16u << 20)) { message = "Not opened: " + p + " is too large for a BASIC program"; error = true; return; }
    const Bytes bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string text, what;
    if (!basic.fileToText(bytes, text, what)) { message = "Not opened: " + p + " is " + what + "."; error = true; return; }
    const bool tokenized = what.find("tokenized") != std::string::npos;
    source = text;
    path = tokenized ? std::string() : p;            // Save writes text: not over the tokenized file
    message = "Opened " + p + " (" + what + ")" + (tokenized ? ". Save as... writes it as text." : ".");
    error = false;
    tabRequest = 0;
}

void BasicWindow::saveFile(const std::string& p) {
    std::string why;
    if (!writeFileSafely(p, source.data(), source.size(), &why)) { message = "Not saved: " + why; error = true; return; }
    path = p;
    message = "Saved " + p;
    error = false;
}

void BasicWindow::requestOpen() {
    const std::string dir = path.empty() ? host.romDir : std::filesystem::u8path(path).parent_path().u8string();
    browser.open("Open a BASIC program (text, or as BASIC saves it)", dir, { ".bas", ".txt", "" }, [this](const std::string& p) { openFile(p); });
}

void BasicWindow::requestSave(bool saveAs) {
    if (!saveAs && !path.empty()) { saveFile(path); return; }
    const std::string dir = path.empty() ? host.romDir : std::filesystem::u8path(path).parent_path().u8string();
    const std::string name = path.empty() ? std::string("program.bas") : std::filesystem::u8path(path).filename().u8string();
    saver.open("Save the BASIC program (text)", dir, name, [this](const std::string& p) { saveFile(p); });
}

void BasicWindow::loadSettings(const std::map<std::string, std::string>& ini) {
    auto it = ini.find("basic_path");
    if (it != ini.end()) path = it->second;
}

void BasicWindow::saveSettings(std::ostream& out) const {
    if (!path.empty()) out << "basic_path=" << path << "\n";
}

int BasicWindow::editorCallback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<BasicWindow*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        self->source.resize((size_t)data->BufTextLen);
        data->Buf = self->source.data();
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) return 0;
    if (self->editPending) {
        // a replace into the text box with the keyboard (it keeps its own copy of the text)
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, self->editText.data(), self->editText.data() + self->editText.size());
        self->editPending = false;
        self->editText.clear();
        self->selectPending = true;
    }
    if (self->selectPending) {
        data->SelectionStart = std::clamp(self->editSelA, 0, data->BufTextLen);
        data->CursorPos = data->SelectionEnd = std::clamp(self->editSelB, 0, data->BufTextLen);
        self->selectPending = false;
    }
    self->selA = std::min(data->SelectionStart, data->SelectionEnd);
    self->selB = std::max(data->SelectionStart, data->SelectionEnd);
    if (self->selA == self->selB) self->selA = self->selB = data->CursorPos;
    return 0;
}

// ---- find and replace
namespace {
std::string lowerText(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool basicNameChar(char c) { return std::isalnum((unsigned char)c) || c == '$' || c == '%' || c == '!' || c == '.'; }
}

void BasicWindow::setFind(const std::string& what, const std::string& with, bool matchCase, bool wholeWord) {
    std::snprintf(findText, sizeof findText, "%s", what.c_str());
    std::snprintf(replaceText, sizeof replaceText, "%s", with.c_str());
    findCase = matchCase;
    findWord = wholeWord;
}

std::vector<std::pair<int, int>> BasicWindow::matches() const {
    std::vector<std::pair<int, int>> out;
    if (!findText[0]) return out;
    const std::string hay = findCase ? source : lowerText(source);
    const std::string needle = findCase ? std::string(findText) : lowerText(findText);
    for (size_t at = hay.find(needle); at != std::string::npos && out.size() < 100000; at = hay.find(needle, at + needle.size())) {
        if (findWord && ((at > 0 && basicNameChar(source[at - 1])) || (at + needle.size() < source.size() && basicNameChar(source[at + needle.size()])))) continue;
        out.push_back({ (int)at, (int)(at + needle.size()) });
    }
    return out;
}

void BasicWindow::select(int a, int b) {
    a = std::clamp(a, 0, (int)source.size());
    b = std::clamp(b, a, (int)source.size());
    selA = a; selB = b;
    if (editorActive || focusEditor) { selectPending = true; editSelA = a; editSelB = b; }
}

void BasicWindow::changeText(std::string text, int a, int b) {
    if (text == source) return;
    beforeReplace = source;
    canUndoReplace = true;
    source = std::move(text);
    findA = findB = -1;
    selA = selB = 0;
    select(a, b);
    if (editorActive || focusEditor) { editPending = true; editText = source; editSelA = selA; editSelB = selB; }
}

bool BasicWindow::findNext(bool backward) {
    const auto ms = matches();
    if (ms.empty()) { findA = findB = -1; findMessage = findText[0] ? "not found" : ""; return false; }
    size_t k = ms.size();
    if (!backward) { for (size_t i = 0; i < ms.size(); i++) if (ms[i].first >= selB) { k = i; break; } if (k == ms.size()) k = 0; }
    else { for (size_t i = ms.size(); i-- > 0;) if (ms[i].first < selA) { k = i; break; } if (k == ms.size()) k = ms.size() - 1; }
    findA = ms[k].first;
    findB = ms[k].second;
    select(findA, findB);
    findMessage = std::to_string(k + 1) + " of " + std::to_string(ms.size());
    return true;
}

bool BasicWindow::replaceOne() {
    bool onMatch = false;
    for (const auto& m : matches()) onMatch = onMatch || (m.first == selA && m.second == selB);
    if (!onMatch) return findNext(false);
    std::string text = source;
    const std::string with = replaceText;
    text.replace((size_t)selA, (size_t)(selB - selA), with);
    const int at = selA + (int)with.size();
    changeText(std::move(text), at, at);
    findNext(false);
    return true;
}

int BasicWindow::replaceAll() {
    const auto ms = matches();
    if (ms.empty()) { findMessage = "not found"; return 0; }
    std::string text;
    const std::string with = replaceText;
    int from = 0;
    for (const auto& m : ms) { text.append(source, (size_t)from, (size_t)(m.first - from)); text += with; from = m.second; }
    text.append(source, (size_t)from, std::string::npos);
    changeText(std::move(text), ms.front().first, ms.front().first);
    findMessage = std::to_string(ms.size()) + " replaced";
    return (int)ms.size();
}

bool BasicWindow::undoReplace() {
    if (!canUndoReplace) return false;
    std::string text = beforeReplace;
    changeText(std::move(text), 0, 0);
    canUndoReplace = false;                    // (one step: the replace undone, not this)
    findMessage = "the replace undone";
    return true;
}

void BasicWindow::findBar() {
    ImGui::PushID("##basicfind");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Find");
    after().field(200, "##find");
    if (findFocus) { ImGui::SetKeyboardFocusHere(); findFocus = false; }
    const bool entered = ImGui::InputTextWithHint("##find", "text to find", findText, sizeof findText, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool enter = entered || (ImGui::IsItemActive() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)));
    if (ImGui::IsItemEdited()) { findA = findB = -1; findMessage.clear(); }
    if (enter) { findNext(ImGui::GetIO().KeyShift); findFocus = true; }
    if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { findOpen = false; focusEditor = true; if (findA >= 0) select(findA, findB); }
    if (after().smallButton("<")) findNext(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The match before (Shift+F3, Shift+Enter)");
    if (after().smallButton(">")) findNext(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The next match (F3, Enter)");
    after().checkbox("Aa", &findCase);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match the case");
    after().checkbox("Word", &findWord);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Whole words only");
    if (!replaceOpen && after().smallButton("Replace...")) replaceOpen = true;
    if (after().smallButton("x")) { findOpen = false; findA = findB = -1; }
    if (!findMessage.empty()) after().textDisabled(findMessage.c_str());
    if (replaceOpen) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("With");
        after().field(200, "##with");
        ImGui::InputTextWithHint("##with", "replace with", replaceText, sizeof replaceText);
        if (after().smallButton("Replace")) replaceOne();
        if (after().smallButton("Replace all")) replaceAll();
        ImGui::BeginDisabled(!canUndoReplace);
        if (after().smallButton("Undo the replace")) undoReplace();
        ImGui::EndDisabled();
    }
    ImGui::PopID();
}

// Every match in view marked over the text; the one on show stronger.
void BasicWindow::drawMatches(const char* childName) {
    if (!findOpen || !findText[0]) return;
    ImGuiWindow* child = ImGui::FindWindowByName(childName);
    if (!child) return;
    const auto ms = matches();
    if (ms.empty()) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 frame = ImGui::GetItemRectMin();
    const ImVec2 origin(frame.x + style.FramePadding.x - child->Scroll.x, frame.y + style.FramePadding.y - child->Scroll.y);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const int first = std::max(0, (int)(child->Scroll.y / lineH) - 1);
    const int last = first + (int)(child->InnerRect.GetHeight() / lineH) + 3;
    ImDrawList* dl = child->DrawList;
    dl->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
    const ImU32 all = ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f));
    const ImU32 shown = ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f));
    int line = 0;
    size_t counted = 0, lineAt = 0;
    for (const auto& m : ms) {
        for (; counted < (size_t)m.first; counted++) if (source[counted] == '\n') { line++; lineAt = counted + 1; }
        if (line < first) continue;
        if (line > last) break;
        const char* base = source.data() + lineAt;
        const float x0 = origin.x + font->CalcTextSizeA(size, FLT_MAX, 0.0f, base, source.data() + m.first).x;
        const float x1 = origin.x + font->CalcTextSizeA(size, FLT_MAX, 0.0f, base, source.data() + m.second).x;
        const float y = origin.y + (float)line * lineH;
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x1, x0 + 2.0f), y + lineH), m.first == findA ? shown : all, 2.0f);
    }
    dl->PopClipRect();
}

void BasicWindow::toolbar() {
    FlowRow row;
    if (row.button("New")) newFile();
    if (row.button("Open...")) requestOpen();
    if (row.button("Save...")) requestSave(true);
    row.spacing = 16.0f;
    const bool ready = host.booted();
    ImGui::BeginDisabled(!ready);
    if (row.button("From the machine")) fromMachine();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The program in the machine's memory, listed into the editor.");
    row.spacing = -1.0f;
    if (row.button("Into the machine")) intoMachine(false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The editor's program into the machine's memory, at once, as if it had been typed.\n"
                          "It takes the place of the program there. BASIC must be at its Ready prompt.");
    if (row.button("Into the machine and RUN")) intoMachine(true);
    ImGui::EndDisabled();
}

void BasicWindow::editorTab() {
    if (findOpen) findBar();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways;
    const ImGuiID editorId = ImGui::GetID("##basicsrc");
    char childName[512];
    std::snprintf(childName, sizeof childName, "%s/%s_%08X", ImGui::GetCurrentWindow()->Name, "##basicsrc", editorId);
    if (focusEditor) ImGui::SetKeyboardFocusHere();
    focusEditor = false;
    editorActive = ImGui::GetActiveID() == editorId;
    ImGui::InputTextMultiline("##basicsrc", source.data(), source.capacity() + 1, ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()),
                              flags, &BasicWindow::editorCallback, this);
    drawMatches(childName);
    if (findA >= 0 && !editorActive) {
        // the match on show out of view (the keyboard in the find box): scrolled to a third of the way down
        if (ImGuiWindow* child = ImGui::FindWindowByName(childName)) {
            const float lineH = ImGui::GetTextLineHeight();
            const float y = (float)std::count(source.begin(), source.begin() + std::min(findA, (int)source.size()), '\n') * lineH;
            static int shownMatch = -1;
            if (shownMatch != findA && (y < child->Scroll.y || y + lineH * 2 > child->Scroll.y + child->InnerRect.GetHeight()))
                ImGui::SetScrollY(child, std::max(0.0f, y - child->InnerRect.GetHeight() / 3));
            shownMatch = findA;
        }
    }
    focused = focused || ImGui::IsItemActive();
    ImGui::TextDisabled("One numbered line a line; keywords in any case. Lines are sorted by number.");
}

void BasicWindow::memoryTab() {
    int end = 0;
    bool basic10 = false;
    if (!host.booted() || !host.basicProgramEnd(end, basic10)) {
        ImGui::TextDisabled("BASIC is not at its Ready prompt: no program to list.");
        return;
    }
    const Bytes& ram = host.emu->memory->ram;
    const std::vector<BasicLine> lines = basic.listProgram([&](int a) { return (int)ram[(size_t)(a & 0xffff)]; }, BASIC_PROGRAM_START);
    ImGui::TextDisabled("BASIC %s, %d bytes, %zu lines (live).", basic10 ? "1.0" : "1.1", end - BASIC_PROGRAM_START, lines.size());
    if (ImGui::BeginTable("##basiclist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("at", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("&0000").x);
        ImGui::TableSetupColumn("line", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("65535").x);
        ImGui::TableSetupColumn("text");
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)lines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const BasicLine& l = lines[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("&%04X", l.addr);
                ImGui::TableNextColumn(); ImGui::Text("%d", l.num);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(l.text.c_str());
            }
        ImGui::EndTable();
    }
}

void BasicWindow::draw(bool* open) {
    focused = false;
    ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("BASIC", open)) { ImGui::End(); return; }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused) {
        const bool ctrlF = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F), ctrlH = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_H);
        if (ctrlF || ctrlH) {
            if (selB > selA && selB <= (int)source.size() && source.find('\n', (size_t)selA) >= (size_t)selB)
                std::snprintf(findText, sizeof findText, "%s", source.substr((size_t)selA, (size_t)(selB - selA)).c_str());
            findOpen = findFocus = true;
            replaceOpen = ctrlH;
            tabRequest = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false) && findText[0]) { findOpen = true; findNext(ImGui::GetIO().KeyShift); }
    }
    {
        WrapText wrapText;                     // text wraps at the window's edge
        toolbar();
        if (error) ImGui::TextColored(kBad, "%s", message.c_str());
        else ImGui::TextDisabled("%s", message.c_str());
        if (ImGui::BeginTabBar("##basictabs")) {
            if (ImGui::BeginTabItem("Editor", nullptr, tabRequest == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { editorTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("In memory", nullptr, tabRequest == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { memoryTab(); ImGui::EndTabItem(); }
            tabRequest = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace cpcse
