// CPCSyntaxError: three debugger tools (user 2026-11-17, "add em all").
//   Watches: addresses or labels, each read as a byte, word, signed byte or text, live; a byte
//     or word can be changed. Kept in cpcse.ini (watch.N).
//   Memory > Search: bytes (hex, ?? for any), text (any case), or a word, in the Z80's 64K or in
//     all RAM; each hit shown in the hex view with a click.
//   Debugger > Export .asm: a range as RASM source that assembles back to the same bytes
//     (disasm_export.h).
#include "gui_shell.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "imgui.h"
#include "disasm_export.h"
#include "gui_assembler.h"
#include "gui_widgets.h"
#include "core/emulator.h"
#include "core/file_write.h"
#include "core/memory.h"

namespace cpcse {

namespace {
const char* const WATCH_TYPES[] = { "byte", "word", "signed byte", "text (16)" };

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lowerCase(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// A number as CPC programs write them: &FF #FF $FF 0xFF, FFh, or decimal. -1 when it is not one.
long parseNumber(const std::string& textIn) {
    std::string t = trim(textIn);
    if (t.empty()) return -1;
    int base = 10;
    if (t[0] == '&' || t[0] == '#' || t[0] == '$') { base = 16; t.erase(0, 1); }
    else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { base = 16; t.erase(0, 2); }
    else if (t.size() > 1 && (t.back() == 'h' || t.back() == 'H')) { base = 16; t.pop_back(); }
    if (t.empty() || t.size() > 8) return -1;
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, base);
    return end && *end == 0 ? v : -1;
}
} // namespace

// An address from a watch's text: a number, or a label (the debugger's symbols, any case),
// with "+n" or "-n" after it. -1 when nothing names it.
int GuiShell::watchAddress(const std::string& exprIn) const {
    const std::string expr = trim(exprIn);
    if (const long n = parseNumber(expr); n >= 0) return (int)(n & 0xffff);
    size_t op = expr.find_first_of("+-", 1);
    const std::string name = trim(op == std::string::npos ? expr : expr.substr(0, op));
    long offset = 0;
    if (op != std::string::npos) {
        offset = parseNumber(expr.substr(op + 1));
        if (offset < 0) return -1;
        if (expr[op] == '-') offset = -offset;
    }
    const std::string want = lowerCase(name);
    for (const auto& s : debugger.symbols) if (lowerCase(s.first) == want) return (int)((s.second + offset) & 0xffff);
    return -1;
}

// ---------------------------------------------------------------- Watches
void GuiShell::windowWatches() {
    if (!panelOpen("Watches")) return;
    if (!beginTool("Watches") || !host.booted() || !host.emu) { ImGui::End(); return; }
    {
    WrapText wrapText;                         // (ends with this block, before the window does)
    GX4000* e = host.emu;
    {
        FlowRow row;
        row.item(ImGui::GetFontSize() * 10);
        bool add = ImGui::InputTextWithHint("##watchexpr", "&4000, #BE00 or a label", watchInput, sizeof(watchInput), ImGuiInputTextFlags_EnterReturnsTrue);
        row.item(ImGui::GetFontSize() * 7);
        ImGui::Combo("##watchtype", &watchType, WATCH_TYPES, IM_ARRAYSIZE(WATCH_TYPES));
        add = row.smallButton("Add##watch") || add;
        if (add && trim(watchInput) != "") {
            watches.push_back({ trim(watchInput), watchType });
            watchInput[0] = 0;
        }
    }
    if (watches.empty()) textWrappedDisabled("A watch reads memory each frame as the Z80 sees it: an address (&4000, #4000, 16384) or a "
                                             "label of the program assembled or loaded (score, score+2). A changed value is shown in "
                                             "colour for a second. A byte or a word can be changed: type a value and press Enter.");
    const double now = ImGui::GetTime();
    int remove = -1;
    if (!watches.empty() && ImGui::BeginTable("##watches", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Watch");
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("&0000").x + 8);
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < watches.size(); i++) {
            Watch& w = watches[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s  ", w.expr.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", WATCH_TYPES[std::clamp(w.type, 0, 3)]);
            const int a = watchAddress(w.expr);
            ImGui::TableNextColumn();
            if (a < 0) { ImGui::TextDisabled("?"); ImGui::TableNextColumn(); ImGui::TextDisabled("no such address or label"); }
            else {
                ImGui::Text("&%04X", a);
                ImGui::TableNextColumn();
                auto rd = [&](int x) { return e->memory->readMapped(x & 0xffff) & 0xff; };
                int value = rd(a);
                std::string shown;
                char b[48];
                switch (w.type) {
                    case 1: value = rd(a) | rd(a + 1) << 8; std::snprintf(b, sizeof b, "&%04X  %d", value, value); shown = b; break;
                    case 2: std::snprintf(b, sizeof b, "&%02X  %d", value, value > 127 ? value - 256 : value); shown = b; break;
                    case 3: {
                        value = 0;
                        for (int k = 0; k < 16; k++) { const int c = rd(a + k) & 0x7f; value = value * 31 + c; shown += (c >= 32 && c < 127) ? (char)c : '.'; }
                        break;
                    }
                    default: std::snprintf(b, sizeof b, "&%02X  %d", value, value); shown = b; break;
                }
                if (w.last != value) { if (w.last != -1) w.changedAt = now; w.last = value; }
                const bool recent = now - w.changedAt < 1.0;
                if (w.type == 0 || w.type == 1 || w.type == 2) {
                    // shown as text; Enter with a value typed in writes it
                    char edit[24];
                    std::snprintf(edit, sizeof edit, "%s", shown.c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (recent) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
                    if (ImGui::InputText("##value", edit, sizeof edit, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        std::string t = trim(edit);
                        const size_t sp = t.find(' ');
                        if (sp != std::string::npos) t = t.substr(0, sp);
                        long v = parseNumber(t);
                        if (v < 0 && !t.empty() && t[0] == '-') { const long n = parseNumber(t.substr(1)); if (n >= 0) v = (-n) & 0xffff; }
                        if (v >= 0) {
                            e->memory->write(a, (int)(v & 0xff));
                            if (w.type == 1) e->memory->write((a + 1) & 0xffff, (int)(v >> 8 & 0xff));
                            host.timelineEdited();
                            host.status = "Watch " + w.expr + " changed";
                        } else host.status = "Not a value: " + t;
                    }
                    if (recent) ImGui::PopStyleColor();
                } else if (recent) ImGui::TextColored(kWarn, "%s", shown.c_str());
                else ImGui::TextUnformatted(shown.c_str());
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove = (int)i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this watch");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) watches.erase(watches.begin() + remove);
    }
    ImGui::End();
}

// ---------------------------------------------------------------- Memory > Search
// The search itself, with what the Search tab holds: memSearchHits and memSearchWhy.
void GuiShell::memorySearch() {
    GX4000* e = host.emu;
    memSearchHits.clear();
    if (!e || !e->memory) { memSearchWhy = "No machine"; return; }
    // the pattern: -1 = any byte
    std::vector<int> pattern;
    memSearchWhy.clear();
    const std::string text = memSearchText;
    if (memSearchKind == 0) {
        std::string t;
        for (char c : text) if (c != ',' && c != ' ' && c != '&' && c != '#' && c != '$') t += c;
        if (t.size() % 2) memSearchWhy = "Bytes are two hex digits each";
        for (size_t k = 0; k + 1 < t.size() && memSearchWhy.empty(); k += 2) {
            if (t[k] == '?' && t[k + 1] == '?') { pattern.push_back(-1); continue; }
            if (!std::isxdigit((unsigned char)t[k]) || !std::isxdigit((unsigned char)t[k + 1])) { memSearchWhy = "Not hex: " + t.substr(k, 2); break; }
            pattern.push_back((int)std::strtol(t.substr(k, 2).c_str(), nullptr, 16));
        }
    } else if (memSearchKind == 1) {
        for (char c : text) pattern.push_back((unsigned char)c);
    } else {
        const long v = parseNumber(text);
        if (v < 0 || v > 0xffff) memSearchWhy = "Not a 16-bit value: " + text;
        else pattern = { (int)(v & 0xff), (int)(v >> 8) };
    }
    memSearchHits.clear();
    if (pattern.empty() && memSearchWhy.empty()) memSearchWhy = "Nothing to search for";
    if (memSearchWhy.empty()) {
        const bool anyCase = memSearchKind == 1 && memSearchAnyCase;
        auto same = [&](int byte, int want) {
            if (want < 0) return true;
            if (anyCase) return std::tolower(byte & 0xff) == std::tolower(want);
            return (byte & 0xff) == want;
        };
        const size_t size = memSearchScope == 0 ? 0x10000 : e->memory->ram.size();
        auto byteAt = [&](size_t a) { return memSearchScope == 0 ? e->memory->readMapped((int)a) & 0xff : (int)e->memory->ram[a]; };
        for (size_t a = 0; a + pattern.size() <= size && memSearchHits.size() < 1000; a++) {
            size_t k = 0;
            while (k < pattern.size() && same(byteAt(a + k), pattern[k])) k++;
            if (k == pattern.size()) memSearchHits.push_back((int)a);
        }
        memSearchWhy = std::to_string(memSearchHits.size()) + (memSearchHits.size() >= 1000 ? "+ (the first 1000)" : "") + " found";
    }
}

void GuiShell::memorySearchContent() {
    static const char* const kinds[] = { "Bytes (hex, ?? = any)", "Text", "Word (16-bit)" };
    static const char* const scopes[] = { "The Z80's 64K", "All RAM" };
    {
        FlowRow row;
        row.item(ImGui::GetFontSize() * 11);
        bool go = ImGui::InputTextWithHint("##memfind", memSearchKind == 0 ? "3E 01 ?? CD" : memSearchKind == 1 ? "text" : "&BB5A",
                                           memSearchText, sizeof(memSearchText), ImGuiInputTextFlags_EnterReturnsTrue);
        row.item(ImGui::GetFontSize() * 9);
        ImGui::Combo("##memfindkind", &memSearchKind, kinds, 3);
        row.item(ImGui::GetFontSize() * 7);
        ImGui::Combo("##memfindscope", &memSearchScope, scopes, 2);
        if (memSearchKind == 1) row.checkbox("Any case", &memSearchAnyCase);
        go = row.smallButton("Search##mem") || go;
        if (go) memorySearch();
    }
    if (!memSearchWhy.empty()) textWrappedDisabled("%s", memSearchWhy.c_str());
    ImGui::BeginChild("##memhits", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (size_t i = 0; i < memSearchHits.size(); i++) {
        const int a = memSearchHits[i];
        char line[64];
        if (memSearchScope == 0) std::snprintf(line, sizeof line, "&%04X", a);
        else std::snprintf(line, sizeof line, "RAM &%05X  (bank %d, &%04X in it)", a, a / 0x4000, a & 0x3fff);
        ImGui::PushID((int)i);
        // a hit in the 64K (or in the first 64K of RAM): the hex view goes to it
        if (ImGui::Selectable(line) && (memSearchScope == 0 || a < 0x10000)) showInMemory(a);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- Debugger > Export .asm
void GuiShell::disassemblyExportPopup() {
    if (exportOpen) { ImGui::OpenPopup("Export as RASM source"); exportOpen = false; }
    if (!ImGui::BeginPopup("Export as RASM source")) return;
    textWrappedDisabled("The code in a range, as RASM source with labels for its jumps and calls. "
                        "It is assembled again here: what does not give back the same bytes is written as DB.");
    FlowRow row;
    row.textDisabled("From");
    row.item(ImGui::GetFontSize() * 5);
    ImGui::InputText("##expfrom", exportFrom, sizeof(exportFrom), ImGuiInputTextFlags_CharsHexadecimal);
    row.textDisabled("to");
    row.item(ImGui::GetFontSize() * 5);
    ImGui::InputText("##expto", exportTo, sizeof(exportTo), ImGuiInputTextFlags_CharsHexadecimal);
    row.textDisabled("(hex, the last byte)");
    const bool busy = assembler && assembler->building();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Save as .asm...")) {
        const int from = (int)std::strtol(exportFrom, nullptr, 16) & 0xffff, last = (int)std::strtol(exportTo, nullptr, 16) & 0xffff;
        char name[32];
        std::snprintf(name, sizeof name, "export_%04X.asm", from);
        saver.open("Export as RASM source", host.romDir, name, [this, from, last](const std::string& path) {
            if (!host.emu) return;
            GX4000* e = host.emu;
            const DisasmExport x = exportDisassembly([e](int a) { return e->memory->readMapped(a & 0xffff) & 0xff; }, from, last + 1, debugger.labelAt);
            std::string why;
            if (x.text.empty()) { host.status = "Not exported: " + x.why; return; }
            if (!writeFileSafely(path, x.text.data(), x.text.size(), &why)) { host.status = "Not saved: " + why; return; }
            host.status = "Exported " + std::to_string(x.instructions) + " instructions, " + std::to_string(x.dataBytes) + " bytes of DB, " +
                          std::to_string(x.labels) + " labels" + (x.verified ? " (RASM gives back every byte)" : " (NOT checked: " + x.why + ")");
        });
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (busy) textWrappedDisabled("(the assembler is busy: export when it has finished)");
    ImGui::EndPopup();
}

} // namespace cpcse
