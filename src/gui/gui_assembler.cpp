// CPCSyntaxError GUI — the RASM editor window. See gui_assembler.h.
#include "gui_assembler.h"

#include "imgui.h"
#include "imgui_internal.h"   // the editor's child window, for the line-number gutter

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "emuhost.h"
#include "gui_debugger.h"
#include "gui_widgets.h"
#include "rasm_bridge.h"
#include "core/emulator.h"
#include "core/gate_array.h"
#include "core/memory.h"
#include "core/z80.h"

extern "C" {
// Only the two result structures are needed from rasm.h; its s_parameter stays in C.
struct s_debug_error { char* filename; int line; char* msg; int lenmsg, lenfilename; };
struct s_debug_symbol { char* name; int v; };
struct s_rasm_info {
    struct s_debug_error* error; int nberror, maxerror, warnerr;
    struct s_debug_symbol* symbol; int nbsymbol, maxsymbol;
    int run, start;
    unsigned char* emuram; int lenram;
    unsigned char* emurom; int lenrom;
};
}

namespace cpcse {

static const char* SAMPLE_SOURCE =
    "; CPCSyntaxError -- the built-in assembler is RASM.\n"
    "; F9 assembles into the machine's memory, Ctrl+F9 also jumps to it.\n"
    "; Labels named BRK... become breakpoints (as rasm -eb).\n"
    "\n"
    "        org #4000\n"
    "        run start\n"
    "\n"
    "start:  ld hl,message\n"
    ".next:  ld a,(hl)\n"
    "        or a\n"
    "        jr z,.done\n"
    "        call #bb5a          ; TXT OUTPUT\n"
    "        inc hl\n"
    "        jr .next\n"
    ".done:  jr .done\n"
    "\n"
    "message: defb \"Hello from RASM!\",13,10,0\n";

static const char* SYNTAX_NAMES[] = { "RASM", "Maxam (-m)", "AS80 (-ass)", "UZ80 (-uz)", "DAMS (-dams)", "Pasmo (-pasmo)" };

AssemblerWindow::AssemblerWindow(EmuHost& h, Debugger& d, FileBrowser& b, SaveDialog& s)
    : host(h), debugger(d), browser(b), saver(s) {
    newFile();
}

std::string AssemblerWindow::displayName() const {
    std::string n = path.empty() ? std::string("untitled.asm") : std::filesystem::path(path).filename().string();
    return modified ? n + " *" : n;
}

static int countLines(const std::string& s) { return 1 + (int)std::count(s.begin(), s.end(), '\n'); }

static bool readText(const std::string& p, std::string& text) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return true;
}

static std::string canonical(const std::string& p) {
    std::error_code ec;
    std::filesystem::path c = std::filesystem::weakly_canonical(std::filesystem::path(p), ec);
    std::string s = (ec ? std::filesystem::path(p) : c).generic_string();
#if defined(_WIN32)
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
#endif
    return s;
}

void AssemblerWindow::newFile() {
    source = SAMPLE_SOURCE;
    sourceRevision++;
    path.clear();
    modified = false;
    lineCount = countLines(source);
    tabs.assign(1, Tab{});
    activeTab = 0; selectTab = 0;
    messages.clear(); errorLines.clear();
}

// Opening a file makes it the main file: its tab first, then a tab for each file it links,
// and each of theirs.
void AssemblerWindow::openFile(const std::string& p) {
    std::string text;
    if (!readText(p, text)) { summary = "Cannot open " + p; return; }
    source = text;
    sourceRevision++;
    path = p;
    modified = false;
    lineCount = countLines(source);
    tabs.assign(1, Tab{});
    activeTab = 0; selectTab = 0;
    messages.clear(); errorLines.clear();
    openLinked(source, path, 0);
    summary = "Opened " + p;
    if (tabs.size() > 1) summary += " and " + std::to_string(tabs.size() - 1) + " linked file" + (tabs.size() == 2 ? "" : "s");
}

void AssemblerWindow::saveFile(const std::string& p) {
    std::string why;
    if (!writeFileSafely(p, source.data(), source.size(), &why)) { summary = "Not saved: " + why; return; }
    path = p;
    modified = false;
    summary = "Saved " + p;
    if (activeTab == 0) openLinked(source, path, 0);     // a new INCLUDE opens its file
}

// The files a source links: INCLUDE "name" and READ "name" (rasm's two for source; INCBIN
// is binary and gets no tab). A name is looked for beside the file that names it, then
// beside the main file, because rasm assembles in the main file's folder.
std::vector<std::string> AssemblerWindow::linkedFiles(const std::string& text, const std::string& fromPath) const {
    namespace fs = std::filesystem;
    std::vector<std::string> found;
    const fs::path fromDir = fromPath.empty() ? fs::path() : fs::path(fromPath).parent_path();
    const std::string mainPath = activeTab == 0 ? path : (tabs.empty() ? std::string() : tabs[0].path);
    const fs::path mainDir = mainPath.empty() ? fs::path() : fs::path(mainPath).parent_path();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // the code part of the line: up to a ; or // outside quotes
        std::string code;
        bool quoted = false;
        for (size_t i = 0; i < line.size(); i++) {
            const char c = line[i];
            if (c == '"') quoted = !quoted;
            if (!quoted && (c == ';' || (c == '/' && i + 1 < line.size() && line[i + 1] == '/'))) break;
            code += c;
        }
        std::string upper = code;
        for (char& c : upper) c = (char)std::toupper((unsigned char)c);
        for (const char* word : { "INCLUDE", "READ" }) {
            const size_t wordLength = std::string(word).size();
            size_t at = upper.find(word);
            while (at != std::string::npos) {
                const size_t end = at + wordLength;
                const bool startOk = at == 0 || !(std::isalnum((unsigned char)upper[at - 1]) || upper[at - 1] == '_' || upper[at - 1] == '.');
                const bool endOk = end >= upper.size() || std::isspace((unsigned char)upper[end]) || upper[end] == '"';
                if (startOk && endOk) {
                    const size_t q1 = code.find('"', end);
                    const size_t q2 = q1 == std::string::npos ? q1 : code.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        const std::string name = code.substr(q1 + 1, q2 - q1 - 1);
                        std::error_code ec;
                        fs::path candidate = fromDir / name;
                        if (!fs::exists(candidate, ec)) candidate = mainDir / name;
                        if (fs::is_regular_file(candidate, ec)) found.push_back(candidate.string());
                    }
                }
                at = upper.find(word, at + 1);
            }
        }
    }
    return found;
}

void AssemblerWindow::openLinked(const std::string& text, const std::string& fromPath, int depth) {
    if (depth > 16) return;                              // a file that includes itself
    const std::string mainPath = activeTab == 0 ? path : tabs[0].path;
    for (const std::string& file : linkedFiles(text, fromPath)) {
        if (!mainPath.empty() && canonical(file) == canonical(mainPath)) continue;
        if (tabFor(file) >= 0) continue;
        Tab tab;
        if (!readText(file, tab.source)) continue;
        tab.path = file;
        tab.lineCount = countLines(tab.source);
        tabs.push_back(tab);
        const std::string linkedText = tab.source;
        openLinked(linkedText, file, depth + 1);
    }
}

int AssemblerWindow::tabFor(const std::string& file) const {
    namespace fs = std::filesystem;
    if (file.empty()) return -1;
    std::vector<std::string> wanted = { canonical(file) };
    const std::string mainPath = activeTab == 0 ? path : tabs[0].path;
    if (!mainPath.empty() && fs::path(file).is_relative()) wanted.push_back(canonical((fs::path(mainPath).parent_path() / file).string()));
    for (size_t i = 0; i < tabs.size(); i++) {
        const std::string p = (int)i == activeTab ? path : tabs[i].path;
        if (p.empty()) continue;
        const std::string c = canonical(p);
        for (const std::string& w : wanted) if (c == w) return (int)i;
    }
    return -1;
}

int AssemblerWindow::openFileTab(const std::string& file) {
    const int have = tabFor(file);
    if (have >= 0) return have;
    Tab tab;
    if (!readText(file, tab.source)) return -1;
    tab.path = file;
    tab.lineCount = countLines(tab.source);
    tabs.push_back(tab);
    return (int)tabs.size() - 1;
}

void AssemblerWindow::setCLines(const std::vector<CSourceLine>& lines) {
    std::vector<Debugger::SourceLine> table;
    lineFirstAddress.clear();
    lineFileKey.clear();
    for (const CSourceLine& l : lines) {
        table.push_back({ l.start, l.end, l.file, l.line });
        auto key = lineFileKey.find(l.file);
        if (key == lineFileKey.end()) key = lineFileKey.emplace(l.file, canonical(l.file)).first;
        auto at = lineFirstAddress.find({ key->second, l.line });
        if (at == lineFirstAddress.end() || l.start < at->second) lineFirstAddress[{ key->second, l.line }] = l.start;
    }
    debugger.setSourceLines(std::move(table));
    followPc = -1;
}

// The machine stopped (a step, a breakpoint, Pause) on a C line: its tab shown, the line
// brought into view. This happens once per stop, so the editor can be used while it stays stopped.
void AssemblerWindow::followStoppedLine() {
    if (!host.booted() || !host.emu || !host.paused || debugger.sourceLines.empty()) { if (!host.paused) followPc = -1; return; }
    const int pc = host.emu->cpu->pc & 0xffff;
    if (pc == followPc) return;
    followPc = pc;
    const Debugger::SourceLine* l = debugger.lineAt(pc);
    if (!l) return;
    const int tab = openFileTab(l->file);
    if (tab < 0) return;
    selectTab = tab;
    scrollToLine = l->line;
}

void AssemblerWindow::setCursorLine(int line) {
    int at = 0;
    for (int l = 1; l < line && at < (int)source.size(); at++) if (source[(size_t)at] == '\n') l++;
    cursorPos = at;
}

int AssemblerWindow::asmLineAddress(int line, std::string& why) {
    namespace fs = std::filesystem;
    if (activeTab != 0) { why = "for assembly it works in the main file's tab (the one F9 assembles)"; return -1; }
    // the source with the marker before the line: an alias (EQU, its $ taken where it is,
    // and exported with the labels; a variable "=" is not exported), not a label, so the
    // local labels' scope (.name after a global label) stays as it is; no byte is added
    std::string text = source;
    size_t at = 0;
    for (int l = 1; l < line && at < text.size(); at++) if (text[at] == '\n') l++;
    text.insert(at, "__cpcse_cursor__ equ $\n");
    std::error_code ec;
    const fs::path previous = fs::current_path(ec);
    if (!path.empty()) fs::current_path(fs::path(path).parent_path(), ec);
    unsigned char* out = nullptr; int outLength = 0; s_rasm_info* info = nullptr; int fatal = 0;
    const int ret = rasm_bridge_assemble(text.data(), (int)text.size(), syntax, &out, &outLength, &info, &fatal);
    fs::current_path(previous, ec);
    int address = -1;
    for (int i = 0; info && i < info->nbsymbol; i++) {
        if (!info->symbol[i].name) continue;
        std::string n = info->symbol[i].name;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n == "__cpcse_cursor__") address = info->symbol[i].v & 0xffff;
    }
    rasm_bridge_free(out, info);
    if (ret != 0 || fatal) why = "the source does not assemble with the marker on that line (inside a macro or a loop?)";
    else if (address < 0) why = "rasm gave no address for that line";
    return ret != 0 || fatal ? -1 : address;
}

void AssemblerWindow::runToCursor() {
    runToLine(1 + (int)std::count(source.begin(), source.begin() + std::min<size_t>((size_t)std::max(cursorPos, 0), source.size()), '\n'));
}

// The name under the mouse in the editor (and the line, 1 = the first, even when there is
// no name): the editor's text as drawHighlighted places it. Called right after the text box.
bool AssemblerWindow::wordUnderMouse(const char* childName, float lineH, std::string& word, int& line) {
    word.clear();
    line = -1;
    ImGuiWindow* child = ImGui::FindWindowByName(childName);
    if (!child) return false;
    indexLines();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 frame = ImGui::GetItemRectMin(), mouse = ImGui::GetIO().MousePos;
    const float ox = frame.x + style.FramePadding.x - child->Scroll.x, oy = frame.y + style.FramePadding.y - child->Scroll.y;
    const int row = (int)std::floor((mouse.y - oy) / lineH);
    if (row < 0 || row >= (int)lineStarts.size()) return false;
    line = row + 1;
    const int a = lineStarts[(size_t)row];
    const int b = row + 1 < (int)lineStarts.size() ? lineStarts[(size_t)row + 1] - 1 : (int)source.size();
    if (b <= a) return false;
    const std::string text = source.substr((size_t)a, (size_t)(b - a));
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    int col = -1;
    float x = ox;
    for (int i = 0; i < (int)text.size(); i++) {
        const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data() + i, text.data() + i + 1).x;
        if (mouse.x >= x && mouse.x < x + w) { col = i; break; }
        x += w;
    }
    const bool c = cSyntax();
    auto inWord = [c](char ch) { return std::isalnum((unsigned char)ch) || ch == '_' || (!c && (ch == '.' || ch == '@')); };
    if (col < 0 || !inWord(text[(size_t)col])) return false;
    int s = col, e = col;
    while (s > 0 && inWord(text[(size_t)s - 1])) s--;
    while (e < (int)text.size() && inWord(text[(size_t)e])) e++;
    word = text.substr((size_t)s, (size_t)(e - s));
    if (std::isdigit((unsigned char)word[0])) word.clear();   // a number, not a name
    return !word.empty();
}

std::string AssemblerWindow::hoverText(const std::string& word) {
    if (word.empty() || !lastOk || !host.booted() || !host.emu || !host.emu->memory || !host.emu->cpu) return "";
    GX4000* e = host.emu;
    const Z80& z = *e->cpu;
    auto peek = [e](int a) { return e->memory->readMapped(a & 0xffff) & 0xff; };
    auto bytesAt = [&](int a, int n) {
        std::string s;
        char b[8];
        for (int i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%s%02X", i ? " " : "", peek(a + i)); s += b; }
        return s;
    };
    std::string lw = word;
    for (char& ch : lw) ch = (char)std::tolower((unsigned char)ch);
    const bool c = cSyntax();
    char b[320];
    if (!c) {
        // a register: its value now (and, for a pair, what it points at)
        struct Reg { const char* name; int value; bool pair; };
        const Reg regs[] = { { "a", z.a, false }, { "f", z.f, false }, { "b", z.b, false }, { "c", z.c, false }, { "d", z.d, false },
                             { "e", z.e, false }, { "h", z.h, false }, { "l", z.l, false }, { "i", z.i, false }, { "r", z.r, false },
                             { "ixh", z.ix >> 8, false }, { "ixl", z.ix & 0xff, false }, { "iyh", z.iy >> 8, false }, { "iyl", z.iy & 0xff, false },
                             { "af", z.a << 8 | z.f, true }, { "bc", z.b << 8 | z.c, true }, { "de", z.d << 8 | z.e, true },
                             { "hl", z.h << 8 | z.l, true }, { "ix", z.ix, true }, { "iy", z.iy, true }, { "sp", z.sp, true }, { "pc", z.pc, true } };
        for (const Reg& r : regs) {
            if (lw != r.name) continue;
            if (r.pair) std::snprintf(b, sizeof b, "%s = &%04X (%d)\n(%s) = %s", word.c_str(), r.value & 0xffff, r.value & 0xffff, word.c_str(), bytesAt(r.value, 8).c_str());
            else std::snprintf(b, sizeof b, "%s = &%02X (%d)", word.c_str(), r.value & 0xff, r.value & 0xff);
            return b;
        }
    }
    // the symbol: in assembly as written (any case); in C its "_name" (SDCC's and z88dk's), then as written
    const std::pair<std::string, int>* found = nullptr;
    for (const std::string& want : c ? std::vector<std::string>{ "_" + word, word } : std::vector<std::string>{ word }) {
        for (const auto& s : symbols) {
            bool same = s.first == want;
            if (!same && !c && s.first.size() == want.size()) {
                same = true;
                for (size_t i = 0; i < want.size() && same; i++) same = std::tolower((unsigned char)s.first[i]) == std::tolower((unsigned char)want[i]);
            }
            if (same) { found = &s; break; }
        }
        if (found) break;
    }
    if (!found) return "";
    const int at = found->second & 0xffff;
    const auto sz = symbolSizes.find(found->first);
    const int size = sz == symbolSizes.end() ? -1 : sz->second;
    const Debugger::SourceLine* code = debugger.lineAt(at);
    const std::string name = word + (c && found->first != word ? "  (" + found->first + ")" : "");   // C: the compiler's _name
    if (code && code->start == at) {                    // a function
        std::snprintf(b, sizeof b, "%s\nfunction at &%04X%s", name.c_str(), at, size > 0 ? (", " + std::to_string(size) + " bytes of code").c_str() : "");
        return b;
    }
    if (!c) {                                           // an assembly label or constant
        std::snprintf(b, sizeof b, "%s = &%04X (%d)\nat &%04X: %s", name.c_str(), at, at, at, bytesAt(at, 8).c_str());
        return b;
    }
    const int v8 = peek(at), v16 = v8 | peek(at + 1) << 8;
    const long v32 = (long)v16 | (long)peek(at + 2) << 16 | (long)peek(at + 3) << 24;
    if (size == 1) std::snprintf(b, sizeof b, "%s\n&%04X, 1 byte: &%02X = %d (signed %d)", name.c_str(), at, v8, v8, (int)(int8_t)v8);
    else if (size == 2) std::snprintf(b, sizeof b, "%s\n&%04X, 2 bytes: &%04X = %d (signed %d)", name.c_str(), at, v16, v16, (int)(int16_t)v16);
    else if (size == 4) std::snprintf(b, sizeof b, "%s\n&%04X, 4 bytes: &%08lX = %lu (signed %ld)", name.c_str(), at, (unsigned long)v32, (unsigned long)v32, (long)(int32_t)v32);
    else if (size > 4) std::snprintf(b, sizeof b, "%s\n&%04X, %d bytes: %s%s", name.c_str(), at, size, bytesAt(at, std::min(size, 16)).c_str(), size > 16 ? " ..." : "");
    else std::snprintf(b, sizeof b, "%s\n&%04X: byte &%02X = %d, word &%04X = %d\n%s", name.c_str(), at, v8, v8, v16, v16, bytesAt(at, 8).c_str());
    return b;
}

void AssemblerWindow::symbolTooltip(const std::string& word) {
    const std::string text = hoverText(word);
    if (!text.empty()) ImGui::SetTooltip("%s", text.c_str());
}

void AssemblerWindow::runToLine(int line) {
    if (!host.booted()) return;
    std::string why;
    int address = -1;
    if (!lineFirstAddress.empty() && !path.empty() && cSyntax()) {
        // the C line, or the first line after it that has code
        const std::string key = canonical(path);
        for (int l = line; l < line + 200 && address < 0; l++) {
            auto at = lineFirstAddress.find({ key, l });
            if (at != lineFirstAddress.end()) address = at->second;
        }
        if (address < 0) why = "no code on that line or after it in this file";
    } else if (cMode()) {
        why = "build the C program first (F9): its lines' addresses come from the build";
    } else {
        address = asmLineAddress(line, why);
    }
    char b[200];
    if (address < 0) { std::snprintf(b, sizeof b, "Run to line %d: %s", line, why.c_str()); summary = b; return; }
    // built but not started (F9 only: the machine is still at the prompt): started first, as
    // a debugger starts the program it is asked to run to a line of
    std::string started;
    if (!programStarted && !pcInProgram()) started = startForRun();
    debugger.runToPassingBreakpoints(address);
    std::snprintf(b, sizeof b, "Running to line %d (&%04X), past the breakpoints%s", line, address & 0xffff,
                  started.empty() ? "" : ("; the program started (" + started + ")").c_str());
    summary = b;
}

bool AssemblerWindow::pcInProgram() const {
    if (!host.booted() || !host.emu || !host.emu->cpu || !host.emu->gateArray || lastLength <= 0 || lastStart < 0) return false;
    const int pc = host.emu->cpu->pc & 0xffff;
    if (pc < lastStart || pc >= lastStart + lastLength) return false;
    // the RMR: bit 2 set = the lower ROM off, bit 3 set = the upper ROM off
    const int rmr = host.emu->gateArray->romConfig;
    if (pc < 0x4000 && !(rmr & 0x04)) return false;
    if (pc >= 0xc000 && !(rmr & 0x08)) return false;
    return true;
}

std::string AssemblerWindow::startForRun() {
    if (!host.booted() || lastLength <= 0) return "";
    std::string how;
    if (!lastBytes.empty()) {
        host.startProgram(lastBytes, lastStart, lastRun, how);    // C: from BASIC, MC START PROGRAM
    } else {
        const int entry = lastRun >= 0 ? lastRun : lastStart;     // assembly: at its entry, the ROMs over it off
        pageForRun(lastStart, lastLength);
        debugger.setPc(entry);
        char b[32];
        std::snprintf(b, sizeof b, "at &%04X", entry & 0xffff);
        how = b;
    }
    programStarted = true;
    return how;
}

void AssemblerWindow::storeActive() {
    if (activeTab < 0 || activeTab >= (int)tabs.size()) return;
    Tab& t = tabs[(size_t)activeTab];
    t.source = source; t.path = path; t.modified = modified; t.lineCount = lineCount;
}

void AssemblerWindow::showTab(int index) {
    if (index < 0 || index >= (int)tabs.size() || index == activeTab) return;
    storeActive();
    // an editor with the focus keeps its own copy of the text and would write it back over
    // the new tab's
    if (ImGui::GetCurrentContext()) ImGui::ClearActiveID();
    activeTab = index;
    const Tab& t = tabs[(size_t)index];
    source = t.source; path = t.path; modified = t.modified; lineCount = t.lineCount;
    sourceRevision++;
    refreshErrorLines();
}

// The gutter marks the errors of the tab on show.
void AssemblerWindow::refreshErrorLines() {
    errorLines.clear();
    for (const Message& m : messages) if (m.tab == activeTab && m.line > 0) errorLines.insert(m.line);
}

void AssemblerWindow::requestOpen() {
    std::string dir = path.empty() ? host.romDir : std::filesystem::path(path).parent_path().string();
    browser.open("Open source", dir, { ".asm", ".z80", ".s", ".txt", ".inc", ".mac", ".c", ".h" },
                 [this](const std::string& p) { openFile(p); });
}

void AssemblerWindow::requestSave(bool saveAs) {
    if (!saveAs && !path.empty()) { saveFile(path); return; }
    std::string dir = path.empty() ? host.romDir : std::filesystem::path(path).parent_path().string();
    std::string name = path.empty() ? std::string("program.asm") : std::filesystem::path(path).filename().string();
    saver.open("Save source", dir, name, [this](const std::string& p) { saveFile(p); });
}

void AssemblerWindow::loadSettings(const std::map<std::string, std::string>& ini) {
    auto it = ini.find("asm_syntax");
    if (it != ini.end()) syntax = std::clamp(std::atoi(it->second.c_str()), 0, 5);
    it = ini.find("asm_brk");
    if (it != ini.end()) brkLabels = std::atoi(it->second.c_str()) != 0;
    it = ini.find("asm_highlight");
    if (it != ini.end()) highlight = std::atoi(it->second.c_str()) != 0;
    for (size_t k = 0; k < colours.size(); k++) {
        it = ini.find(std::string("asm_colour_") + asmTokenKey((AsmToken)k));
        if (it == ini.end() || it->second.size() != 6) continue;
        char* end = nullptr;
        const unsigned long v = std::strtoul(it->second.c_str(), &end, 16);
        if (end && !*end) colours[k] = (uint32_t)v & 0xffffff;
    }
    if ((it = ini.find("asm_sdcc")) != ini.end()) ctools.sdccFolder = it->second;
    if ((it = ini.find("asm_cpctelera")) != ini.end()) ctools.cpcteleraFolder = it->second;
    if ((it = ini.find("asm_cflags")) != ini.end()) ctools.extraFlags = it->second;
    if ((it = ini.find("asm_ccompiler")) != ini.end()) ctools.compiler = it->second == "z88dk" ? "z88dk" : "sdcc";
    if ((it = ini.find("asm_z88dk")) != ini.end()) ctools.z88dkFolder = it->second;
    if ((it = ini.find("asm_z88dk_sccz80")) != ini.end()) ctools.z88dkSccz80 = std::atoi(it->second.c_str()) != 0;
    if ((it = ini.find("asm_codeloc")) != ini.end()) ctools.codeLoc = (int)std::strtol(it->second.c_str(), nullptr, 0) & 0xffff;
    it = ini.find("asm_file");
    if (it != ini.end() && !it->second.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(it->second, ec)) openFile(it->second);
    }
}

void AssemblerWindow::saveSettings(std::ostream& out) const {
    out << "asm_syntax=" << syntax << "\n";
    out << "asm_brk=" << (brkLabels ? 1 : 0) << "\n";
    out << "asm_file=" << (activeTab == 0 || tabs.empty() ? path : tabs[0].path) << "\n";   // the main file: it brings its links
    out << "asm_highlight=" << (highlight ? 1 : 0) << "\n";
    out << "asm_sdcc=" << ctools.sdccFolder << "\n";
    out << "asm_cpctelera=" << ctools.cpcteleraFolder << "\n";
    out << "asm_cflags=" << ctools.extraFlags << "\n";
    out << "asm_ccompiler=" << ctools.compiler << "\n";
    out << "asm_z88dk=" << ctools.z88dkFolder << "\n";
    out << "asm_z88dk_sccz80=" << (ctools.z88dkSccz80 ? 1 : 0) << "\n";
    char loc[16];
    std::snprintf(loc, sizeof loc, "0x%04X", ctools.codeLoc & 0xffff);
    out << "asm_codeloc=" << loc << "\n";
    for (size_t k = 0; k < colours.size(); k++) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%06X", (unsigned)colours[k] & 0xffffff);
        out << "asm_colour_" << asmTokenKey((AsmToken)k) << "=" << hex << "\n";
    }
}

// Where each line starts, and whether it starts inside /* */ -- again only when the text
// has changed.
void AssemblerWindow::indexLines() {
    const bool c = cSyntax();
    if (indexedRevision == sourceRevision && indexedC == c) return;
    indexedRevision = sourceRevision;
    indexedC = c;
    lineStarts.assign(1, 0);
    lineInComment.assign(1, 0);
    bool inComment = false;
    size_t start = 0;
    for (;;) {
        const size_t nl = source.find('\n', start);
        const std::string line = source.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (c) highlightCLine(line, inComment);
        else highlightAsmLine(line, inComment);
        if (nl == std::string::npos) break;
        start = nl + 1;
        lineStarts.push_back((int)start);
        lineInComment.push_back(inComment ? 1 : 0);
    }
}

// The visible lines' spans, drawn where the text box drew its (invisible) text: the same
// font, the same origin, its scroll; each span's x is the width of the line before it, as
// the box measures (tabs included).
void AssemblerWindow::drawHighlighted(const char* childName, float lineH, float editorH) {
    ImGuiWindow* child = ImGui::FindWindowByName(childName);
    if (!child) return;
    indexLines();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 frame = ImGui::GetItemRectMin();
    const ImVec2 origin(frame.x + style.FramePadding.x - child->Scroll.x, frame.y + style.FramePadding.y - child->Scroll.y);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    ImDrawList* dl = child->DrawList;
    dl->PushClipRect(child->InnerClipRect.Min, child->InnerClipRect.Max, true);
    const int lines = (int)lineStarts.size();
    const int first = std::max(0, (int)(child->Scroll.y / lineH) - 1);
    const int last = std::min(lines, first + (int)(editorH / lineH) + 3);
    ImU32 col[(size_t)AsmToken::Count];
    for (size_t k = 0; k < colours.size(); k++) {
        const uint32_t c = colours[k];
        col[k] = IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 255);
    }
    for (int l = first; l < last; l++) {
        const int a = lineStarts[(size_t)l];
        const int b = l + 1 < lines ? lineStarts[(size_t)l + 1] - 1 : (int)source.size();
        if (b <= a) continue;
        const std::string line = source.substr((size_t)a, (size_t)(b - a));
        bool inComment = lineInComment[(size_t)l] != 0;
        const float y = origin.y + (float)l * lineH;
        float x = origin.x;
        for (const AsmSpan& s : indexedC ? highlightCLine(line, inComment) : highlightAsmLine(line, inComment)) {
            const char* begin = line.data() + s.start;
            const char* end = begin + s.length;
            dl->AddText(font, size, ImVec2(x, y), col[(size_t)s.kind], begin, end);
            x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, begin, end).x;
        }
    }
    dl->PopClipRect();
}

void AssemblerWindow::coloursPopup() {
    if (!ImGui::BeginPopup("Code colours")) return;
    ImGui::Checkbox("Colour the code", &highlight);
    ImGui::Separator();
    ImGui::BeginDisabled(!highlight);
    for (size_t k = 0; k < colours.size(); k++) {
        const uint32_t c = colours[k];
        float rgb[3] = { ((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f };
        ImGui::PushID((int)k);
        if (ImGui::ColorEdit3("##c", rgb, ImGuiColorEditFlags_NoInputs))
            colours[k] = (uint32_t)(rgb[0] * 255.0f + 0.5f) << 16 | (uint32_t)(rgb[1] * 255.0f + 0.5f) << 8 | (uint32_t)(rgb[2] * 255.0f + 0.5f);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f), "%s", cSyntax() ? cTokenLabel((AsmToken)k) : asmTokenLabel((AsmToken)k));
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::Button("Defaults")) colours = defaultAsmColours();
    ImGui::EndPopup();
}


// A message as rasm stored it: `length` bytes (it is not reliably terminated), with
// terminal colour codes, and sometimes a second line quoting the source. Kept: the
// printable text, lines joined with " | ".
static std::string cleanMessage(const char* raw, int length) {
    std::string out;
    for (int i = 0; raw && i < length && raw[i]; i++) {
        char ch = raw[i];
        if (ch == 0x1b) { while (i < length && raw[i] && raw[i] != 'm') i++; continue; }
        if (ch == '\n' || ch == '\r') { if (!out.empty() && out.back() != ' ') out += " | "; continue; }
        if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7f) continue;
        out += ch;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '|' || out.back() == ':')) out.pop_back();
    return out;
}

// The shown file's own type decides its colours: a .c or .h tab as C, the others (a .s or
// .asm beside the C files) as assembly; an unsaved tab follows the main file.
bool AssemblerWindow::cSyntax() const {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext.empty()) return cMode();
    return ext == ".c" || ext == ".h";
}

bool AssemblerWindow::cMode() const {
    const std::string mainPath = activeTab == 0 || tabs.empty() ? path : tabs[0].path;
    std::string ext = std::filesystem::path(mainPath).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".c";
}

// F9 on a C file: every edited tab saved (SDCC reads its files from disc), then the build
// on a thread (c_build.h).
void AssemblerWindow::startCBuild(bool thenRun, bool assetsOnly) {
    namespace fs = std::filesystem;
    if (cRun && !cRun->done) return;            // one build at a time
    if (cRun) finishCBuild();
    storeActive();
    std::string notSaved;
    for (size_t i = 0; i < tabs.size(); i++) {
        Tab& t = tabs[i];
        if (!t.modified || t.path.empty()) continue;
        std::string why;
        if (writeFileSafely(t.path, t.source.data(), t.source.size(), &why)) {
            t.modified = false;
            if ((int)i == activeTab) modified = false;
        } else notSaved += " " + fs::path(t.path).filename().string();
    }
    messages.clear(); errorLines.clear();
    if (!notSaved.empty()) { Message m; m.text = "Could not save before building:" + notSaved; messages.push_back(m); }
    const std::string mainPath = tabs[0].path;
    std::error_code ec;
    const std::string cache = (fs::temp_directory_path(ec) / "cpcse-cbuild").u8string();
    auto run = std::make_shared<CRun>();
    run->thenRun = thenRun;
    run->assetsOnly = assetsOnly;
    CRun* r = run.get();
    CToolchain tools = ctools;
    tools.assetsOnly = assetsOnly;           // every asset step again, then stop
    if (!assetsOnly) tools.makeSnapshot = host.programSnapshotMaker();
    run->worker = std::thread([r, tools, mainPath, cache]() {
        CBuildResult result = buildC(tools, mainPath, cache, [r](const std::string& line) {
            std::lock_guard<std::mutex> g(r->lock);
            r->progress = line;
        });
        std::lock_guard<std::mutex> g(r->lock);
        r->result = std::move(result);
        r->done = true;
    });
    cRun = run;
    lastOk = false;
    summary = assetsOnly ? "Converting assets..." : "Building...";
}

// The build's result, taken in on the UI thread: messages on their tabs, the bytes into the
// machine, the map's symbols to the debugger, and the run from _main when asked.
void AssemblerWindow::finishCBuild() {
    if (!cRun) return;
    if (cRun->worker.joinable()) cRun->worker.join();
    const CBuildResult res = cRun->result;
    const bool thenRun = cRun->thenRun, assetsOnly = cRun->assetsOnly;
    cRun.reset();
    cLog = res.log;
    for (const CBuildMessage& cm : res.messages) {
        Message m;
        m.file = cm.file;
        m.line = cm.line;
        m.text = cm.text;
        m.tab = tabFor(cm.file);
        messages.push_back(m);
    }
    refreshErrorLines();
    if (assetsOnly) {
        // the log's "Assets: N converted, M up to date" line is the summary; nothing to load
        lastOk = res.ok;
        const size_t at = res.log.rfind("Assets: ");
        std::string line = at == std::string::npos ? std::string() : res.log.substr(at, res.log.find('\n', at) - at);
        if (res.ok) summary = line.empty() ? "Assets converted" : line;
        else {
            int errors = 0;
            for (const CBuildMessage& m : res.messages) if (m.error) errors++;
            summary = "Asset conversion failed: " + std::to_string(errors) + " error" + (errors == 1 ? "" : "s");
        }
        return;
    }
    symbols = res.symbols;
    lastOk = res.ok;
    lastStart = res.loadAddress; lastRun = res.runAddress; lastLength = (int)res.bytes.size();
    lastBytes = res.bytes;
    programStarted = false;
    if (!res.ok) {
        int errors = 0;
        for (const CBuildMessage& m : res.messages) if (m.error) errors++;
        summary = "Build failed: " + std::to_string(errors) + " error" + (errors == 1 ? "" : "s");
        return;
    }
    char b[200];
    if (host.booted() && host.emu && host.emu->memory) {
        for (size_t i = 0; i < res.bytes.size(); i++) host.emu->memory->write((res.loadAddress + (int)i) & 0xffff, res.bytes[i]);
        host.timelineEdited();
        std::snprintf(b, sizeof b, "Built %s: %d bytes, loaded at &%04X-&%04X", res.project.c_str(), (int)res.bytes.size(),
                      res.loadAddress & 0xffff, (res.loadAddress + (int)res.bytes.size() - 1) & 0xffff);
    } else {
        std::snprintf(b, sizeof b, "Built %s: %d bytes at &%04X (no machine to load them into)", res.project.c_str(),
                      (int)res.bytes.size(), res.loadAddress & 0xffff);
    }
    summary = b;
    std::string images;
    for (const std::string* p : { &res.dskPath, &res.cdtPath, &res.snaPath })
        if (!p->empty()) images += (images.empty() ? "" : ", ") + std::filesystem::u8path(*p).filename().u8string();
    if (!images.empty()) summary += "; wrote " + images;
    debugger.setSymbols(symbols);
    setCLines(res.lines);
    symbolSizes = res.symbolSizes;
    if (thenRun && host.booted() && !res.bytes.empty()) {
        // as a CPC runs a binary from BASIC (EmuHost::startProgram): a reset to the prompt,
        // the bytes put back, MC START PROGRAM to _main
        std::string how;
        insertBuiltDisc(res);
        host.startProgram(res.bytes, res.loadAddress, res.runAddress, how);
        programStarted = true;
        debugger.run();
        std::snprintf(b, sizeof b, "  -- running from _main &%04X (%s)", res.runAddress & 0xffff, how.c_str());
        summary += b;
    }
}

// A project whose disc holds more than the program (its dsk/ files, FILE2SECTOR's data)
// reads that disc as it runs: the new image goes into drive A first, as the disc would be
// in the real drive. A machine with no disc drive is left alone.
void AssemblerWindow::insertBuiltDisc(const CBuildResult& res) {
    if (!res.discNeeded || res.dskPath.empty() || !host.booted() || !host.emu || !host.emu->hasFdc) return;
    host.loadDiskFile(res.dskPath, 0);
}

// A program run where a ROM sits over it would run the ROM: the firmware itself keeps its
// ROMs paged out and pages them in only inside its own calls, and a binary started from
// BASIC (MC START PROGRAM) runs from RAM. So before the jump the GATE ARRAY's RMR (&7Fxx,
// bits 7-6 = 10) gets bit 2 (lower ROM off) when the program reaches into &0000-&3FFF and
// bit 3 (upper ROM off) when it reaches into &C000-&FFFF; the mode bits stay as they are.
void AssemblerWindow::pageForRun(int start, int length) {
    if (!host.booted() || !host.emu || !host.emu->gateArray || length <= 0) return;
    const int first = start & 0xffff, last = std::min(0xffff, first + length - 1);
    int rmr = host.emu->gateArray->romConfig & 0x0f;
    if (first < 0x4000) rmr |= 0x04;
    if (last >= 0xc000) rmr |= 0x08;
    if (rmr != (host.emu->gateArray->romConfig & 0x0f)) host.emu->writePort(0x7f00, 0x80 | rmr);
}

void AssemblerWindow::cSettingsPopup() {
    if (!ImGui::BeginPopup("C settings")) return;
    ImGui::TextUnformatted("C programs: SDCC (with CPCtelera's library), or z88dk");
    ImGui::Separator();
    int which = ctools.compiler == "z88dk" ? 1 : 0;
    if (ImGui::RadioButton("SDCC / CPCtelera", which == 0)) ctools.compiler = "sdcc";
    ImGui::SameLine();
    if (ImGui::RadioButton("z88dk", which == 1)) ctools.compiler = "z88dk";
    if (ctools.compiler == "z88dk") {
        static char z88Buf[512];
        if (ImGui::IsWindowAppearing()) std::snprintf(z88Buf, sizeof z88Buf, "%s", ctools.z88dkFolder.c_str());
        fitNextItemWidth(360, "z88dk folder");
        if (ImGui::InputTextWithHint("z88dk folder", "empty: C:\\z88dk, beside this program, or the PATH", z88Buf, sizeof z88Buf)) ctools.z88dkFolder = z88Buf;
        if (after().smallButton("Browse...##z88dk"))
            browser.openDir("z88dk folder", ctools.z88dkFolder.empty() ? host.romDir : ctools.z88dkFolder,
                            [this](const std::string& d) { ctools.z88dkFolder = d; std::snprintf(z88Buf, sizeof z88Buf, "%s", d.c_str()); });
        const std::string z = findZ88dk(ctools.z88dkFolder);
        ImGui::TextDisabled("  %s", z.empty() ? "z88dk not found (z88dk.org: unpack it, then name its folder)" : ("found: " + z).c_str());
        bool zsdcc = !ctools.z88dkSccz80;
        if (ImGui::Checkbox("Compile with zsdcc (SDCC in z88dk)", &zsdcc)) ctools.z88dkSccz80 = !zsdcc;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("zsdcc: C lines can be stepped in the debugger.\nOff: z88dk's own sccz80 (no line stepping: its line markers come a line late).");
        int zloc = ctools.codeLoc;
        fitNextItemWidth(120, "Code address##z88");
        if (ImGui::InputInt("Code address##z88", &zloc, 0x100, 0x1000, ImGuiInputTextFlags_CharsHexadecimal)) ctools.codeLoc = std::clamp(zloc, 0, 0xffff);
        static char zflags[256];
        if (ImGui::IsWindowAppearing()) std::snprintf(zflags, sizeof zflags, "%s", ctools.extraFlags.c_str());
        fitNextItemWidth(360, "Extra zcc flags");
        if (ImGui::InputText("Extra zcc flags", zflags, sizeof zflags)) ctools.extraFlags = zflags;
        ImGui::EndPopup();
        return;
    }
    static char sdccBuf[512], cpctBuf[512], flagsBuf[256];
    static bool loaded = false;
    if (!loaded || ImGui::IsWindowAppearing()) {
        std::snprintf(sdccBuf, sizeof sdccBuf, "%s", ctools.sdccFolder.c_str());
        std::snprintf(cpctBuf, sizeof cpctBuf, "%s", ctools.cpcteleraFolder.c_str());
        std::snprintf(flagsBuf, sizeof flagsBuf, "%s", ctools.extraFlags.c_str());
        loaded = true;
    }
    fitNextItemWidth(360, "SDCC folder");
    if (ImGui::InputTextWithHint("SDCC folder", "empty: C:\\Program Files\\SDCC, or the PATH", sdccBuf, sizeof sdccBuf)) ctools.sdccFolder = sdccBuf;
    if (after().smallButton("Browse...##sdcc"))
        browser.openDir("SDCC folder", ctools.sdccFolder.empty() ? host.romDir : ctools.sdccFolder,
                        [this](const std::string& d) { ctools.sdccFolder = d; std::snprintf(sdccBuf, sizeof sdccBuf, "%s", d.c_str()); });
    const std::string bin = findSdccBin(ctools.sdccFolder);
    ImGui::TextDisabled("  %s", bin.empty() ? "SDCC not found (sdcc.sourceforge.net: install it, then name its folder)" : ("found: " + bin).c_str());
    fitNextItemWidth(360, "CPCtelera folder");
    if (ImGui::InputTextWithHint("CPCtelera folder", "empty: plain SDCC, no library", cpctBuf, sizeof cpctBuf)) ctools.cpcteleraFolder = cpctBuf;
    if (after().smallButton("Browse...##cpct"))
        browser.openDir("CPCtelera folder", ctools.cpcteleraFolder.empty() ? host.romDir : ctools.cpcteleraFolder,
                        [this](const std::string& d) { ctools.cpcteleraFolder = d; std::snprintf(cpctBuf, sizeof cpctBuf, "%s", d.c_str()); });
    if (!ctools.cpcteleraFolder.empty()) {
        const std::string cpct = findCpctelera(ctools.cpcteleraFolder);
        ImGui::TextDisabled("  %s", cpct.empty() ? "no src/cpctelera.h there" : ("library from: " + cpct + "/src (built once, no Cygwin needed)").c_str());
    }
    int loc = ctools.codeLoc;
    fitNextItemWidth(120, "Code address");
    if (ImGui::InputInt("Code address", &loc, 0x100, 0x1000, ImGuiInputTextFlags_CharsHexadecimal)) ctools.codeLoc = std::clamp(loc, 0, 0xffff);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where the code goes (hex). A CPCtelera project's cfg/build_config.mk (Z80CODELOC) wins.");
    fitNextItemWidth(360, "Extra sdcc flags");
    if (ImGui::InputText("Extra sdcc flags", flagsBuf, sizeof flagsBuf)) ctools.extraFlags = flagsBuf;
    ImGui::EndPopup();
}

void AssemblerWindow::assemble(bool thenRun) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (cMode()) { startCBuild(thenRun); return; }
    // rasm reads the linked files from disk: an edited tab is saved first, so it assembles
    // what is on show. The main file goes in from its tab, saved or not.
    storeActive();
    std::string notSaved;
    for (size_t i = 1; i < tabs.size(); i++) {
        Tab& t = tabs[i];
        if (!t.modified || t.path.empty()) continue;
        std::string why;
        if (writeFileSafely(t.path, t.source.data(), t.source.size(), &why)) {
            t.modified = false;
            if ((int)i == activeTab) modified = false;
        } else notSaved += " " + fs::path(t.path).filename().string();
    }
    const std::string mainSource = activeTab == 0 ? source : tabs[0].source;
    const std::string mainPath = activeTab == 0 ? path : tabs[0].path;
    // INCLUDE / INCBIN / SAVE resolve against the main file's own folder, as from the command line.
    fs::path previous = fs::current_path(ec);
    if (!mainPath.empty()) fs::current_path(fs::path(mainPath).parent_path(), ec);

    unsigned char* out = nullptr; int outLength = 0; s_rasm_info* info = nullptr; int fatal = 0;
    int ret = rasm_bridge_assemble(mainSource.data(), (int)mainSource.size(), syntax, &out, &outLength, &info, &fatal);
    fs::current_path(previous, ec);
    openLinked(mainSource, mainPath, 0);                // a file linked since it was opened

    messages.clear(); errorLines.clear(); symbols.clear();
    lastOk = false; lastStart = -1; lastRun = -1; lastLength = 0;
    if (info) {
        for (int i = 0; i < info->nberror; i++) {
            Message m;
            m.line = info->error[i].line;
            m.file = info->error[i].filename ? info->error[i].filename : "";
            m.text = cleanMessage(info->error[i].msg, info->error[i].lenmsg);
            // The editor's text is fed to rasm as a stream; its "file" is the current
            // directory. Anything else came from an INCLUDE.
            m.inEditor = m.file.empty() || m.file == "<internal>" || m.file.back() == '/' || m.file.back() == '\\';
            m.tab = m.inEditor ? 0 : tabFor(m.file);
            messages.push_back(m);
        }
        for (int i = 0; i < info->nbsymbol; i++)
            if (info->symbol[i].name) symbols.emplace_back(info->symbol[i].name, info->symbol[i].v);
        lastRun = info->run;
        lastStart = info->start;
    }
    if (fatal) {
        Message m; m.text = "rasm stopped (fatal error " + std::to_string(fatal) + ")"; messages.push_back(m);
    }
    if (!notSaved.empty()) { Message m; m.text = "Could not save before assembling:" + notSaved; messages.push_back(m); }
    refreshErrorLines();
    lastLength = outLength;

    if (ret == 0 && !fatal) {
        lastOk = true;
        char b[160];
        if (outLength > 0 && host.booted() && host.emu && host.emu->memory) {
            for (int i = 0; i < outLength; i++) host.emu->memory->write((lastStart + i) & 0xffff, out[i]);
            host.timelineEdited();
            std::snprintf(b, sizeof(b), "Assembled %d bytes, loaded at &%04X-&%04X", outLength,
                          lastStart & 0xffff, (lastStart + outLength - 1) & 0xffff);
        } else if (outLength > 0) {
            std::snprintf(b, sizeof(b), "Assembled %d bytes at &%04X (no machine to load them into)", outLength, lastStart & 0xffff);
        } else {
            std::snprintf(b, sizeof(b), "Assembled; no binary output (snapshot/cartridge builds write their files)");
        }
        summary = b;
        debugger.setSymbols(symbols);
        setCLines({});                              // an assembled program has no C lines
        symbolSizes.clear();
        if (brkLabels) {
            debugger.clearAssemblerBreakpoints();
            for (const auto& s : symbols) {
                std::string u = s.first;
                for (char& c : u) c = (char)std::toupper((unsigned char)c);
                bool brk = (u.rfind("BRK", 0) == 0 && u.find('.', 3) == std::string::npos) ||
                           u.rfind("@BRK", 0) == 0 || u.find(".BRK") != std::string::npos;
                if (brk) debugger.addBreakpoint(s.second, "", true);
            }
        }
        programStarted = false;
        lastBytes.clear();
        if (thenRun && outLength > 0 && host.booted()) {
            int entry = lastRun >= 0 ? lastRun : lastStart;
            programStarted = true;
            pageForRun(lastStart, outLength);
            debugger.setPc(entry);
            debugger.run();
            std::snprintf(b, sizeof(b), "  -- running from &%04X", entry & 0xffff);
            summary += b;
        }
    } else {
        int errors = 0;
        for (const auto& m : messages) if (!m.text.empty()) errors++;
        summary = "Assembly failed: " + std::to_string(errors) + " message" + (errors == 1 ? "" : "s");
    }
    rasm_bridge_free(out, info);
}

int AssemblerWindow::editorCallback(ImGuiInputTextCallbackData* data) {
    auto* self = static_cast<AssemblerWindow*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        self->source.resize((size_t)data->BufTextLen);
        data->Buf = self->source.data();
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && self->gotoLine <= 0) {
        self->cursorPos = data->CursorPos;              // for Run to cursor
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && self->gotoLine > 0) {
        int start = 0, l = 1;
        for (int i = 0; i < data->BufTextLen && l < self->gotoLine; i++) if (data->Buf[i] == '\n') { l++; start = i + 1; }
        int end = start;
        while (end < data->BufTextLen && data->Buf[end] != '\n') end++;
        data->CursorPos = start;
        data->SelectionStart = start;
        data->SelectionEnd = end;
        self->gotoLine = -1;
    }
    return 0;
}

void AssemblerWindow::draw(bool* open) {
    focused = false;
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assembler", open)) { ImGui::End(); return; }
    ImGui::PushTextWrapPos(0.0f);             // text wraps at the window's edge
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGuiIO& io = ImGui::GetIO();
    if (focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) assemble(io.KeyCtrl);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) requestSave(io.KeyShift);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) requestOpen();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F10, false)) runToCursor();
        else if (!debugger.sourceLines.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) debugger.stepLine(true);
            if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) { if (io.KeyShift) debugger.stepOut(); else debugger.stepLine(false); }
        }
    }
    followStoppedLine();

    if (cRun) {
        bool done;
        { std::lock_guard<std::mutex> g(cRun->lock); done = cRun->done; }
        if (done) finishCBuild();
    }
    const bool c = cMode();
    const bool building = cRun != nullptr;

    // ---- toolbar
    if (ImGui::Button("New")) newFile();
    if (after().button("Open...")) requestOpen();
    if (after().button("Save")) requestSave(false);
    if (after().button("Save as...")) requestSave(true);
    after().textDisabled("|");
    ImGui::BeginDisabled(building);
    if (after().button(c ? "Build (F9)" : "Assemble (F9)")) assemble(false);
    if (after().button(c ? "Build + run (Ctrl+F9)" : "Assemble + run (Ctrl+F9)")) assemble(true);
    if (after().button("Run to cursor (Ctrl+F10)")) runToCursor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Run until the code of the cursor's line, past every breakpoint and watchpoint on the way,\n"
                          "and pause there. A line with no code: the next line that has some.");
    if (c && ctools.compiler != "z88dk") {
        if (after().button("Convert assets")) startCBuild(false, true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs every asset step of the CPCtelera project again (images, maps, music, .bin files),\n"
                              "as its cfg/*.mk files name them. F9 runs only the steps whose outputs are older than their inputs.");
    }
    ImGui::EndDisabled();
    if (c) {
        if (after().button("C settings...")) ImGui::OpenPopup("C settings");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("SDCC's folder, CPCtelera's folder, the code address");
        cSettingsPopup();
        if (!debugger.sourceLines.empty()) {
            after().textDisabled("|");
            if (after().button("Line (F11)")) debugger.stepLine(false);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run to the next C line, into the functions it calls");
            if (after().button("Over (F10)")) debugger.stepLine(true);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run to the next C line of this function (calls run through)");
            if (after().button("Out (Shift+F11)")) debugger.stepOut();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run until the function returns");
        }
    } else {
        after().field(130, "##syntax");
        ImGui::Combo("##syntax", &syntax, SYNTAX_NAMES, IM_ARRAYSIZE(SYNTAX_NAMES));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("rasm's compatibility switches");
        after().checkbox("BRK labels", &brkLabels);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Labels named BRK..., @BRK... or ....BRK become breakpoints (rasm -eb)");
    }
    if (after().button("Colours...")) ImGui::OpenPopup("Code colours");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Syntax colouring: on or off, and a colour for each kind of word");
    coloursPopup();
    {
        FlowRow file;
        file.textColored(kAccent, displayName().c_str());
        char n[32];
        std::snprintf(n, sizeof n, "%d lines", lineCount);
        file.textDisabled(n);
    }

    // ---- the open files, a tab each (the first is the main file, the one assembled)
    if (selectTab >= 0) showTab(selectTab);
    if (ImGui::BeginTabBar("##asmfiles", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < (int)tabs.size(); i++) {
            const std::string& p = i == activeTab ? path : tabs[(size_t)i].path;
            const bool changed = i == activeTab ? modified : tabs[(size_t)i].modified;
            std::string label = p.empty() ? std::string("untitled.asm") : std::filesystem::path(p).filename().string();
            if (changed) label += " *";
            label += "###asmtab" + std::to_string(i);
            const ImGuiTabItemFlags flags = selectTab == i ? ImGuiTabItemFlags_SetSelected : 0;
            const bool shown = ImGui::BeginTabItem(label.c_str(), nullptr, flags);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", i == 0 ? "The main file: F9 assembles it, with the files it links"
                                               : "A linked file (INCLUDE / READ): F9 saves it if edited, then assembles the main file");
            if (shown) {
                if (i != activeTab && selectTab < 0) showTab(i);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    selectTab = -1;

    // ---- editor with a line-number gutter
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const float bottomH = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.28f);
    const float editorH = std::max(80.0f, ImGui::GetContentRegionAvail().y - bottomH);
    char digits[16]; std::snprintf(digits, sizeof(digits), "%d", std::max(lineCount, 999));
    const float gutterW = ImGui::CalcTextSize(digits).x + 12.0f;

    ImVec2 gutterPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(gutterW, editorH));
    ImGui::SameLine(0, 0);
    const ImGuiID editorId = ImGui::GetID("##asmsrc");
    char childName[512];
    std::snprintf(childName, sizeof(childName), "%s/%s_%08X", ImGui::GetCurrentWindow()->Name, "##asmsrc", editorId);
    if (gotoLine > 0) ImGui::SetKeyboardFocusHere();
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize |
                                ImGuiInputTextFlags_CallbackAlways;
    // Coloured: the box's own text invisible (its cursor and selection are not), the
    // spans drawn over it.
    if (highlight) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
    if (ImGui::InputTextMultiline("##asmsrc", source.data(), source.capacity() + 1, ImVec2(-1, editorH), flags,
                                  &AssemblerWindow::editorCallback, this)) {
        modified = true;
        sourceRevision++;
        lineCount = countLines(source);
    }
    if (highlight) ImGui::PopStyleColor();
    if (highlight) drawHighlighted(childName, lineH, editorH);
    // the mouse over a name: its address and value; the right-click menu for the line under it
    if (ImGui::IsItemHovered()) {
        std::string word;
        int line = -1;
        wordUnderMouse(childName, lineH, word, line);
        if (!word.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) symbolTooltip(word);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && line > 0) { menuLine = line; ImGui::OpenPopup("##editormenu"); }
    }
    if (ImGui::BeginPopup("##editormenu")) {
        char item[64];
        std::snprintf(item, sizeof item, "Run to line %d", menuLine);
        if (ImGui::MenuItem(item, nullptr, false, host.booted())) runToLine(menuLine);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run until this line's code, past every breakpoint, and pause there");
        if (ImGui::MenuItem("Run to cursor", "Ctrl+F10", false, host.booted())) runToCursor();
        if (!lineFirstAddress.empty() && !path.empty() && cSyntax()) {
            auto at = lineFirstAddress.find({ canonical(path), menuLine });
            if (ImGui::MenuItem("Breakpoint on this line", nullptr, false, at != lineFirstAddress.end()) && at != lineFirstAddress.end())
                debugger.toggleBreakpoint(at->second);
        }
        ImGui::EndPopup();
    }
    float scrollY = 0.0f;
    ImGuiWindow* editorChild = ImGui::FindWindowByName(childName);
    if (editorChild && scrollToLine > 0) {
        // a stop on a line out of view: bring it to a third of the way down
        const float y = (float)(scrollToLine - 1) * lineH;
        if (y < editorChild->Scroll.y || y + lineH * 3 > editorChild->Scroll.y + editorH)
            ImGui::SetScrollY(editorChild, std::max(0.0f, y - editorH / 3));
        scrollToLine = -1;
    }
    if (editorChild) scrollY = editorChild->Scroll.y;
    // the shown file as the C line table names it, the line the machine stopped on, the
    // lines with a breakpoint
    std::string tabKey;
    if (!lineFirstAddress.empty() && !path.empty()) {
        static std::string keyPath, keyCanonical;
        if (keyPath != path) { keyPath = path; keyCanonical = canonical(path); }
        tabKey = keyCanonical;
    }
    int stoppedLine = -1;
    if (!tabKey.empty() && host.booted() && host.emu && host.paused) {
        if (const Debugger::SourceLine* l = debugger.lineAt(host.emu->cpu->pc)) {
            auto k = lineFileKey.find(l->file);
            if (k != lineFileKey.end() && k->second == tabKey) stoppedLine = l->line;
        }
    }
    if (stoppedLine > 0 && editorChild) {
        ImDrawList* cdl = editorChild->DrawList;
        const float y = ImGui::GetItemRectMin().y + style.FramePadding.y - scrollY + (float)(stoppedLine - 1) * lineH;
        cdl->PushClipRect(editorChild->InnerClipRect.Min, editorChild->InnerClipRect.Max, true);
        cdl->AddRectFilled(ImVec2(editorChild->InnerClipRect.Min.x, y), ImVec2(editorChild->InnerClipRect.Max.x, y + lineH), IM_COL32(255, 199, 64, 40));
        cdl->PopClipRect();
    }
    std::unordered_set<int> breakAt;
    if (!tabKey.empty()) for (const GuiBreakpoint& bp : debugger.breakpoints) if (bp.enabled) breakAt.insert(bp.address & 0xffff);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 g0 = gutterPos, g1(gutterPos.x + gutterW - 4, gutterPos.y + editorH);
        dl->AddRectFilled(g0, g1, ImGui::GetColorU32(ImGuiCol_FrameBg, 0.6f));
        dl->PushClipRect(g0, g1, true);
        const float top = gutterPos.y + style.FramePadding.y - scrollY;
        int first = std::max(0, (int)((scrollY - style.FramePadding.y) / lineH) - 1);
        int last = std::min(lineCount, first + (int)(editorH / lineH) + 3);
        for (int i = first; i < last; i++) {
            char n[16]; std::snprintf(n, sizeof(n), "%d", i + 1);
            float w = ImGui::CalcTextSize(n).x;
            bool err = errorLines.count(i + 1) != 0;
            ImU32 col = err ? IM_COL32(255, 90, 80, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
            float y = top + i * lineH;
            if (err) dl->AddRectFilled(ImVec2(g0.x, y), ImVec2(g1.x, y + lineH), IM_COL32(150, 30, 30, 120));
            if (!tabKey.empty()) {
                auto at = lineFirstAddress.find({ tabKey, i + 1 });
                if (at != lineFirstAddress.end() && breakAt.count(at->second))
                    dl->AddCircleFilled(ImVec2(g0.x + lineH * 0.4f, y + lineH * 0.5f), lineH * 0.3f, IM_COL32(230, 60, 50, 255));
            }
            if (i + 1 == stoppedLine)
                dl->AddTriangleFilled(ImVec2(g0.x + 2, y + 2), ImVec2(g0.x + 2, y + lineH - 2), ImVec2(g0.x + lineH * 0.6f, y + lineH * 0.5f), IM_COL32(255, 199, 64, 255));
            dl->AddText(ImVec2(g1.x - w - 4, y), col, n);
        }
        dl->PopClipRect();
        // a click on a line number: a breakpoint on the first address of that C line
        if (!tabKey.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(g0, g1)) {
            const int line = (int)((ImGui::GetIO().MousePos.y - top) / lineH) + 1;
            auto at = lineFirstAddress.find({ tabKey, line });
            if (at != lineFirstAddress.end()) {
                ImGui::SetTooltip("Click: breakpoint on line %d (&%04X)", line, at->second & 0xffff);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) debugger.toggleBreakpoint(at->second);
            }
        }
    }

    // ---- result
    {
        FlowRow result;
        std::string shown = summary;
        if (building) { std::lock_guard<std::mutex> g(cRun->lock); if (!cRun->progress.empty()) shown = "Building: " + cRun->progress; }
        result.textColored(lastOk ? ImVec4(0.5f, 0.9f, 0.5f, 1) : (messages.empty() ? ImVec4(0.7f, 0.7f, 0.7f, 1) : ImVec4(1.0f, 0.45f, 0.4f, 1)),
                           shown.c_str());
        if (lastOk && lastRun >= 0) { char r[16]; std::snprintf(r, sizeof r, "RUN &%04X", lastRun & 0xffff); result.textDisabled(r); }
    }
    if (ImGui::BeginTabBar("##asmresult")) {
        char label[48];
        std::snprintf(label, sizeof(label), "Messages (%d)###msgs", (int)messages.size());
        if (ImGui::BeginTabItem(label)) {
            ImGui::BeginChild("##msglist", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (size_t i = 0; i < messages.size(); i++) {
                const Message& m = messages[i];
                char line[640];
                if (m.line > 0) std::snprintf(line, sizeof(line), "%s%d: %s", m.inEditor ? "line " : (m.file + ":").c_str(), m.line, m.text.c_str());
                else std::snprintf(line, sizeof(line), "%s", m.text.c_str());
                ImGui::PushID((int)i);
                if (ImGui::Selectable(line) && m.tab >= 0 && m.line > 0) { selectTab = m.tab; gotoLine = m.line; }
                ImGui::PopID();
            }
            if (messages.empty()) ImGui::TextDisabled(lastOk ? "No errors, no warnings." : "Assemble with F9.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        std::snprintf(label, sizeof(label), "Symbols (%d)###syms", (int)symbols.size());
        if (ImGui::BeginTabItem(label)) {
            fitNextItemWidth(200, "##symfilter");
            ImGui::InputTextWithHint("##symfilter", "filter", symbolFilter, sizeof(symbolFilter));
            after().textDisabled("click a symbol to show it in the disassembly");
            if (ImGui::BeginTable("##symtab", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                                 ImGuiTableFlags_Sortable, ImVec2(0, 0))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();
                if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
                    if (sort->SpecsDirty && sort->SpecsCount > 0) {
                        const auto spec = sort->Specs[0];
                        std::sort(symbols.begin(), symbols.end(), [&](const auto& a, const auto& b) {
                            bool lt = spec.ColumnIndex == 0 ? a.first < b.first : a.second < b.second;
                            bool gt = spec.ColumnIndex == 0 ? b.first < a.first : b.second < a.second;
                            return spec.SortDirection == ImGuiSortDirection_Ascending ? lt : gt; });
                        sort->SpecsDirty = false;
                    }
                }
                std::string filter = symbolFilter;
                for (char& c : filter) c = (char)std::toupper((unsigned char)c);
                for (size_t i = 0; i < symbols.size(); i++) {
                    const auto& s = symbols[i];
                    if (!filter.empty()) {
                        std::string u = s.first; for (char& c : u) c = (char)std::toupper((unsigned char)c);
                        if (u.find(filter) == std::string::npos) continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable(s.first.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && showInDisassembly)
                        showInDisassembly(s.second & 0xffff);
                    ImGui::PopID();
                    ImGui::TableNextColumn();
                    ImGui::Text("&%04X", s.second & 0xffff);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (c && ImGui::BeginTabItem("Build log###clog")) {
            ImGui::BeginChild("##clog", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(cLog.empty() ? "Every command of the last build and what it printed." : cLog.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

} // namespace cpcse
