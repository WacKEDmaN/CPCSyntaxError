// CPCSyntaxError GUI: the Assembler and C windows' editing tools (user 2026-11-17, "add em
// all"): find and replace, go to a definition and the Labels tab, a comment on or off for the
// selected lines, and Tab / Shift+Tab to move lines in or out. See gui_assembler.h.
#include "gui_assembler.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "gui_widgets.h"

namespace cpcse {

namespace {

int linesIn(const std::string& s) { return 1 + (int)std::count(s.begin(), s.end(), '\n'); }

bool nameChar(char ch, bool c) {
    const unsigned char u = (unsigned char)ch;
    return std::isalnum(u) || ch == '_' || (!c && (ch == '.' || ch == '@'));
}

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// The start of the line that holds pos, and the end (the '\n' or the end of the text).
int lineStart(const std::string& s, int pos) {
    while (pos > 0 && s[(size_t)pos - 1] != '\n') pos--;
    return pos;
}
int lineEnd(const std::string& s, int pos) {
    while (pos < (int)s.size() && s[(size_t)pos] != '\n') pos++;
    return pos;
}

// The lines a selection a..b covers: the first line's start and the last line's end. A
// selection that ends at the start of a line does not take that line.
void selectedLines(const std::string& s, int a, int b, int& first, int& last) {
    a = std::clamp(a, 0, (int)s.size());
    b = std::clamp(b, a, (int)s.size());
    if (b > a && s[(size_t)b - 1] == '\n') b--;
    first = lineStart(s, a);
    last = lineEnd(s, b);
}

bool blank(const std::string& line) {
    for (char ch : line) if (ch != ' ' && ch != '\t' && ch != '\r') return false;
    return true;
}

// Words that start an assembly line but name no label (a line at column 0 with no ':').
bool asmKeyword(const std::string& w) {
    static const char* const words[] = {
        "adc", "add", "and", "bit", "call", "ccf", "cp", "cpd", "cpdr", "cpi", "cpir", "cpl", "daa", "dec", "di", "djnz", "ei", "ex", "exx",
        "halt", "im", "in", "inc", "ind", "indr", "ini", "inir", "jp", "jr", "ld", "ldd", "lddr", "ldi", "ldir", "neg", "nop", "or", "otdr",
        "otir", "out", "outd", "outi", "pop", "push", "res", "ret", "reti", "retn", "rl", "rla", "rlc", "rlca", "rld", "rr", "rra", "rrc",
        "rrca", "rrd", "rst", "sbc", "scf", "set", "sla", "sll", "sl1", "sra", "srl", "sub", "xor", "org", "db", "dw", "ds", "defb", "defw",
        "defs", "defm", "byte", "word", "text", "str", "include", "incbin", "read", "write", "save", "run", "align", "assert", "print",
        "if", "ifdef", "ifndef", "ifnot", "else", "elseif", "endif", "end", "repeat", "rend", "rept", "endr", "until", "while", "wend",
        "endm", "mend", "endmacro", "struct", "endstruct", "ends", "bank", "bankset", "limit", "protect", "nolist", "list", "let", "brk",
        "charset", "buildsna", "buildcpr", "buildrom", "relocate_start", "relocate_end", "relocate_table", "switch", "case", "default",
        "break", "endswitch", "module", "endmodule", "nocode", "code", "fail", "stop", "breakpoint", "ticker", "undef", "ld", "macro" };
    const std::string l = lower(w);
    for (const char* k : words) if (l == k) return true;
    return false;
}

// What `text` defines, line by line (lines from 1).
std::vector<AssemblerWindow::Definition> definitionsOf(const std::string& text, bool c) {
    std::vector<AssemblerWindow::Definition> out;
    int line = 0;
    bool inBlock = false;                                    // C: inside /* */
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        line++;
        p = e + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t i = 0;
        auto skipSpace = [&]() { while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) i++; };
        auto name = [&]() { const size_t s = i; while (i < l.size() && nameChar(l[i], c)) i++; return l.substr(s, i - s); };
        if (c) {
            if (inBlock) { const size_t k = l.find("*/"); if (k == std::string::npos) continue; inBlock = false; l.erase(0, k + 2); }
            if (const size_t k = l.find("//"); k != std::string::npos) l.erase(k);
            if (const size_t k = l.find("/*"); k != std::string::npos) { if (l.find("*/", k + 2) == std::string::npos) inBlock = true; l.erase(k); }
            if (l.empty()) continue;
            skipSpace();
            if (i < l.size() && l[i] == '#') {                // #define NAME
                i++;
                skipSpace();
                if (name() != "define") continue;
                skipSpace();
                const std::string n = name();
                if (!n.empty()) out.push_back({ n, line, "define" });
                continue;
            }
            if (l[0] == ' ' || l[0] == '\t' || l[0] == '}' || l[0] == '{') continue;   // in a function
            if (l.rfind("extern", 0) == 0 || l.rfind("return", 0) == 0) continue;
            const size_t paren = l.find('(');
            std::string head = l.substr(0, paren == std::string::npos ? l.size() : paren);
            if (paren == std::string::npos) {                 // a global: the name before = [ ;
                const size_t cut = head.find_first_of("=[;");
                if (cut == std::string::npos || head.find("typedef") == 0) continue;
                head.erase(cut);
            }
            while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.pop_back();
            size_t s = head.size();
            while (s > 0 && nameChar(head[s - 1], true)) s--;
            const std::string n = head.substr(s);
            if (n.empty() || std::isdigit((unsigned char)n[0]) || s == 0) continue;   // (a name alone on a line is no definition)
            static const char* const keywords[] = { "if", "while", "for", "switch", "return", "sizeof", "else", "do", "case" };
            bool keyword = false;
            for (const char* k : keywords) keyword = keyword || n == k;
            if (keyword) continue;
            out.push_back({ n, line, paren == std::string::npos ? "variable" : "function" });
            continue;
        }
        // assembly
        const bool column0 = !l.empty() && l[0] != ' ' && l[0] != '\t';
        skipSpace();
        if (i >= l.size() || l[i] == ';' || l[i] == '*') continue;
        const std::string first = name();
        if (first.empty()) continue;
        if (lower(first) == "macro") {                        // MACRO name,parameters
            skipSpace();
            const std::string n = name();
            if (!n.empty()) out.push_back({ n, line, "macro" });
            continue;
        }
        const bool colon = i < l.size() && l[i] == ':';
        if (colon) i++;
        skipSpace();
        const size_t at = i;
        const std::string next = lower(name());
        if (next == "equ" || (at < l.size() && l[at] == '=')) { out.push_back({ first, line, "equ" }); continue; }
        if (next == "macro") { out.push_back({ first, line, "macro" }); continue; }
        if (colon || (column0 && !asmKeyword(first))) out.push_back({ first, line, "label" });
    }
    return out;
}

} // namespace

std::vector<AssemblerWindow::Definition> AssemblerWindow::definitions() const {
    return definitionsOf(source, cSyntax());
}

std::string AssemblerWindow::wordAt(int pos) const {
    const bool c = cSyntax();
    int a = std::clamp(pos, 0, (int)source.size()), b = a;
    while (a > 0 && nameChar(source[(size_t)a - 1], c)) a--;
    while (b < (int)source.size() && nameChar(source[(size_t)b], c)) b++;
    std::string w = source.substr((size_t)a, (size_t)(b - a));
    if (!c) while (!w.empty() && w.back() == '.') w.pop_back();
    return w;
}

// The shown file first (the same spelling, then any case: RASM's names are not case-sensitive),
// then the other files of the program, then the last build's symbols (in the disassembly).
bool AssemblerWindow::goToDefinition(const std::string& word) {
    if (word.empty()) return false;
    auto look = [&](const std::vector<Definition>& defs) -> int {
        for (const Definition& d : defs) if (d.name == word) return d.line;
        if (!cSyntax()) for (const Definition& d : defs) if (lower(d.name) == lower(word)) return d.line;
        return -1;
    };
    if (const int l = look(definitions()); l > 0) {
        gotoLine = l;
        findMessage = word + ": line " + std::to_string(l);
        return true;
    }
    for (int t = 0; t < (int)tabs.size(); t++) {
        if (t == activeTab || tabs[(size_t)t].program != activeProgram()) continue;
        const std::string p = tabPath(t);
        std::string ext = lower(std::filesystem::path(p).extension().string());
        const bool c = ext == ".c" || ext == ".h";
        for (const Definition& d : definitionsOf(tabSource(t), c))
            if (d.name == word || (!c && lower(d.name) == lower(word))) {
                selectTab = t;
                gotoLine = d.line;
                findMessage = word + ": " + std::filesystem::path(p).filename().string() + " line " + std::to_string(d.line);
                return true;
            }
    }
    for (const auto& s : symbols)
        if (s.first == word || lower(s.first) == lower(word)) {
            if (showInDisassembly) showInDisassembly(s.second & 0xffff);
            char b[96];
            std::snprintf(b, sizeof b, ": not in the open files; &%04X, shown in the disassembly", s.second & 0xffff);
            findMessage = word + b;
            return true;
        }
    findMessage = word + ": no definition found";
    return false;
}

// ---- the selection and changes of the whole text
void AssemblerWindow::select(int a, int b) {
    a = std::clamp(a, 0, (int)source.size());
    b = std::clamp(b, a, (int)source.size());
    selA = a; selB = b; cursorPos = b;
    if (editorActiveNow || focusEditor) { selectPending = true; editSelA = a; editSelB = b; }
}

void AssemblerWindow::changeText(std::string text, int a, int b) {
    if (text == source) { select(a, b); return; }
    undoSteps.push_back({ source, cursorPos });
    while (undoSteps.size() > UNDO_STEPS) undoSteps.erase(undoSteps.begin());
    redoSteps.clear();
    source = std::move(text);
    modified = true;
    sourceRevision++;
    lineCount = linesIn(source);
    undoText = source;
    lastEditTime = -1.0;
    lastEditBoundary = false;
    a = std::clamp(a, 0, (int)source.size());
    b = std::clamp(b, a, (int)source.size());
    selA = a; selB = b; cursorPos = b;
    findA = findB = -1;
    // the text box with the keyboard keeps its own copy: the same text goes in through its callback
    if (editorActiveNow || focusEditor) { editPending = true; editText = source; editSelA = a; editSelB = b; selectPending = false; }
}

// ---- find and replace
void AssemblerWindow::setFind(const std::string& what, const std::string& with, bool matchCase, bool wholeWord) {
    std::snprintf(findText, sizeof findText, "%s", what.c_str());
    std::snprintf(replaceText, sizeof replaceText, "%s", with.c_str());
    findCase = matchCase;
    findWord = wholeWord;
}

std::vector<std::pair<int, int>> AssemblerWindow::matches() const {
    std::vector<std::pair<int, int>> out;
    const std::string what = findText;
    if (what.empty()) return out;
    const bool c = cSyntax();
    const std::string hay = findCase ? source : lower(source);
    const std::string needle = findCase ? what : lower(what);
    for (size_t at = hay.find(needle); at != std::string::npos && out.size() < 100000; at = hay.find(needle, at + needle.size())) {
        if (findWord) {
            const bool before = at > 0 && nameChar(source[at - 1], c);
            const bool after = at + needle.size() < source.size() && nameChar(source[at + needle.size()], c);
            if (before || after) continue;
        }
        out.push_back({ (int)at, (int)(at + needle.size()) });
    }
    return out;
}

void AssemblerWindow::showMatch(int a, int b) {
    findA = a; findB = b;
    select(a, b);
    scrollToLine = (int)std::count(source.begin(), source.begin() + a, '\n') + 1;
}

bool AssemblerWindow::findNext(bool backward) {
    const auto ms = matches();
    if (ms.empty()) { findA = findB = -1; findMessage = findText[0] ? "not found" : ""; return false; }
    size_t k = 0;
    if (!backward) {
        k = ms.size();
        for (size_t i = 0; i < ms.size(); i++) if (ms[i].first >= selB && !(ms[i].first == selA && ms[i].second == selB && selA != selB)) { k = i; break; }
        if (k == ms.size()) k = 0;                                // round to the first
    } else {
        k = ms.size();
        for (size_t i = ms.size(); i-- > 0;) if (ms[i].first < selA) { k = i; break; }
        if (k == ms.size()) k = ms.size() - 1;                    // round to the last
    }
    showMatch(ms[k].first, ms[k].second);
    findMessage = std::to_string(k + 1) + " of " + std::to_string(ms.size());
    return true;
}

bool AssemblerWindow::replaceOne() {
    const auto ms = matches();
    bool onMatch = false;
    for (const auto& m : ms) onMatch = onMatch || (m.first == selA && m.second == selB);
    if (!onMatch) return findNext(false);                         // first find one, then replace it
    std::string text = source;
    const std::string with = replaceText;
    text.replace((size_t)selA, (size_t)(selB - selA), with);
    const int at = selA + (int)with.size();
    changeText(std::move(text), at, at);
    findNext(false);
    return true;
}

int AssemblerWindow::replaceAll() {
    const auto ms = matches();
    if (ms.empty()) { findMessage = "not found"; return 0; }
    std::string text;
    text.reserve(source.size());
    const std::string with = replaceText;
    int from = 0;
    for (const auto& m : ms) { text.append(source, (size_t)from, (size_t)(m.first - from)); text += with; from = m.second; }
    text.append(source, (size_t)from, std::string::npos);
    const int at = ms.front().first;
    changeText(std::move(text), at, at);
    findMessage = std::to_string(ms.size()) + " replaced";
    return (int)ms.size();
}

// ---- comment, indent
void AssemblerWindow::toggleComment() {
    const std::string marker = cSyntax() ? "//" : ";";
    int first, last;
    selectedLines(source, selA, selB, first, last);
    // the lines, each without its '\n'
    std::vector<std::string> lines;
    for (size_t p = (size_t)first;;) {
        size_t e = source.find('\n', p);
        if (e == std::string::npos || (int)e > last) e = (size_t)last;
        lines.push_back(source.substr(p, e - p));
        if ((int)e >= last) break;
        p = e + 1;
    }
    bool allCommented = true, any = false;
    size_t indent = std::string::npos;
    for (const std::string& l : lines) {
        if (blank(l)) continue;
        any = true;
        const size_t s = l.find_first_not_of(" \t");
        indent = std::min(indent, s);
        if (l.compare(s, marker.size(), marker) != 0) allCommented = false;
    }
    if (!any) return;
    std::string block;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string l = lines[i];
        if (!blank(l)) {
            if (allCommented) {
                const size_t s = l.find_first_not_of(" \t");
                size_t n = marker.size();
                if (s + n < l.size() && l[s + n] == ' ') n++;
                l.erase(s, n);
            } else l.insert(indent, marker + " ");
        }
        block += l;
        if (i + 1 < lines.size()) block += '\n';
    }
    std::string text = source.substr(0, (size_t)first) + block + source.substr((size_t)last);
    changeText(std::move(text), first, first + (int)block.size());
}

void AssemblerWindow::indentLines(bool out) {
    int first, last;
    selectedLines(source, selA, selB, first, last);
    std::string block;
    for (size_t p = (size_t)first;;) {
        size_t e = source.find('\n', p);
        if (e == std::string::npos || (int)e > last) e = (size_t)last;
        std::string l = source.substr(p, e - p);
        if (!out) { if (!blank(l)) l.insert(0, "\t"); }
        else if (!l.empty() && l[0] == '\t') l.erase(0, 1);
        else { size_t n = 0; while (n < 4 && n < l.size() && l[n] == ' ') n++; l.erase(0, n); }
        block += l;
        if ((int)e >= last) break;
        block += '\n';
        p = e + 1;
    }
    std::string text = source.substr(0, (size_t)first) + block + source.substr((size_t)last);
    const bool wasRange = selB > selA;
    if (wasRange) changeText(std::move(text), first, first + (int)block.size());
    else {
        // one line, no selection: the cursor stays at its place in the line
        const int moved = (int)block.size() - (last - first);
        const int at = std::max(first, cursorPos + moved);
        changeText(std::move(text), at, at);
    }
}

// While the text box has the keyboard (draw() calls this just before it).
void AssemblerWindow::editorKeys() {
    const ImGuiID editorId = ImGui::GetID("##asmsrc");
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Slash)) toggleComment();
    // Shift+Tab: claimed here, or Dear ImGui's tabbing takes the keyboard to the item before
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Tab, ImGuiInputFlags_Repeat, editorId)) indentLines(true);
    else if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Tab, true)) {
        const bool lines = selB > selA && source.find('\n', (size_t)selA) < (size_t)selB;
        if (lines) { indentLines(false); swallowTab = true; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) goToDefinition(wordAt(cursorPos));
}

// ---- the find bar, over the editor
void AssemblerWindow::findBar() {
    ImGui::PushID("##findbar");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Find");
    after().field(220, "##find");
    if (findFocus) { ImGui::SetKeyboardFocusHere(); findFocus = false; }
    const bool entered = ImGui::InputTextWithHint("##find", "text to find", findText, sizeof findText, ImGuiInputTextFlags_EnterReturnsTrue);
    // (Enter again, just after the box took the keyboard back, is not always given as a return: read the key too)
    const bool enter = entered || (ImGui::IsItemActive() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)));
    if (ImGui::IsItemDeactivatedAfterEdit() || ImGui::IsItemEdited()) { findA = findB = -1; findMessage.clear(); }
    if (enter) { findNext(ImGui::GetIO().KeyShift); findFocus = true; }   // (Enter: the next one, the focus kept)
    if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        findOpen = false;
        focusEditor = true;
        if (findA >= 0) select(findA, findB);
    }
    if (after().smallButton("<")) findNext(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The match before (Shift+F3, Shift+Enter)");
    if (after().smallButton(">")) findNext(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The next match (F3, Enter)");
    after().checkbox("Aa", &findCase);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match the case");
    after().checkbox("Word", &findWord);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Whole words only");
    if (!replaceOpen) { if (after().smallButton("Replace...")) replaceOpen = true; }
    if (after().smallButton("x")) { findOpen = false; findA = findB = -1; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Close (Esc in the box)");
    if (!findMessage.empty()) after().textDisabled(findMessage.c_str());
    if (replaceOpen) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("With");
        after().field(220, "##with");
        ImGui::InputTextWithHint("##with", "replace with", replaceText, sizeof replaceText);
        if (after().smallButton("Replace")) replaceOne();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replace the match on show, then find the next one");
        if (after().smallButton("Replace all")) replaceAll();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every match: one step for Undo");
    }
    ImGui::PopID();
}

// Every match in view drawn over the text; the one on show stronger (the selection is not
// drawn while the keyboard is in the find box).
void AssemblerWindow::drawFindMatches(const char* childName, float lineH, float editorH) {
    if (!findOpen || !findText[0]) return;
    ImGuiWindow* child = ImGui::FindWindowByName(childName);
    if (!child) return;
    indexLines();
    const auto ms = matches();
    if (ms.empty()) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 frame = ImGui::GetItemRectMin();
    const ImVec2 origin(frame.x + style.FramePadding.x - child->Scroll.x, frame.y + style.FramePadding.y - child->Scroll.y);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const int first = std::max(0, (int)(child->Scroll.y / lineH) - 1);
    const int last = first + (int)(editorH / lineH) + 3;
    ImDrawList* dl = child->DrawList;
    dl->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
    const ImVec4 acc = kAccent;
    const ImU32 all = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.25f));
    const ImU32 shown = ImGui::GetColorU32(ImVec4(acc.x, acc.y, acc.z, 0.55f));
    for (const auto& m : ms) {
        const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), m.first);
        const int l = (int)(it - lineStarts.begin()) - 1;
        if (l < first) continue;
        if (l > last) break;
        const char* base = source.data() + lineStarts[(size_t)l];
        const float x0 = origin.x + font->CalcTextSizeA(size, FLT_MAX, 0.0f, base, source.data() + m.first).x;
        const float x1 = origin.x + font->CalcTextSizeA(size, FLT_MAX, 0.0f, base, source.data() + std::min(m.second, lineEnd(source, m.first))).x;
        const float y = origin.y + (float)l * lineH;
        dl->AddRectFilled(ImVec2(x0, y), ImVec2(std::max(x1, x0 + 2.0f), y + lineH), m.first == findA ? shown : all, 2.0f);
    }
    dl->PopClipRect();
}

// ---- the Labels tab: what the shown file defines, a click goes to its line
void AssemblerWindow::labelsTab() {
    char* filter = labelFilter;
    const std::vector<Definition> defs = definitions();
    char label[48];
    std::snprintf(label, sizeof label, "Labels (%d)###labels", (int)defs.size());
    if (!ImGui::BeginTabItem(label)) return;
    fitNextItemWidth(200, "##labelfilter");
    ImGui::InputTextWithHint("##labelfilter", "filter", labelFilter, sizeof labelFilter);
    after().textDisabled("the names this file defines: a click goes to the line (F12 / Ctrl+click on a name does too)");
    if (ImGui::BeginTable("##labeltable", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("function  ").x);
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Line 00000").x);
        ImGui::TableHeadersRow();
        const std::string f = lower(filter);
        for (size_t i = 0; i < defs.size(); i++) {
            const Definition& d = defs[i];
            if (!f.empty() && lower(d.name).find(f) == std::string::npos) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::Selectable(d.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) gotoLine = d.line;
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", d.kind.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", d.line);
        }
        ImGui::EndTable();
    }
    ImGui::EndTabItem();
}

} // namespace cpcse
