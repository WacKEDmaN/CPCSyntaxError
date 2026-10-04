// CPCSyntaxError GUI — the debugging windows, grouped:
//   Debugger  the run / step toolbar, the Z80's registers, flags and stack beside the
//             disassembly, breakpoints and watchpoints below
//   Chips     a tab each: CRTC, Gate Array, monitor, Plus ASIC, PSG, PPI, keyboard
//             matrix, disc controller, tape
//   Memory    the hex editor and the memory map (gui_memory_views.cpp)
// They read the chips' own state; the only things they change are what a debugger is
// for -- registers, memory, the PC.
#include "gui_shell.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/emulator.h"
#include "core/asic.h"
#include "core/ay.h"
#include "core/crtc.h"
#include "core/dsk.h"
#include "core/fdc.h"
#include "core/gate_array.h"
#include "core/gate_array_model.h"
#include "core/keyboard.h"
#include "core/memory.h"
#include "core/monitor_renderer.h"
#include "core/ppi.h"
#include "core/tape.h"
#include "core/video.h"
#include "core/z80.h"

namespace cpcse {

static bool machineReady(EmuHost& host) {
    if (host.booted() && host.emu && host.emu->cpu && host.emu->memory) return true;
    ImGui::TextDisabled("No machine booted.");
    return false;
}

// ============================================================== toolbar
void GuiShell::debugToolbar() {
    bool running = !host.paused;
    if (ImGui::Button(running ? "Pause (F5)" : "Run (F5)")) { if (running) debugger.pause(); else debugger.run(); }
    if (after().button("Into (F7)")) debugger.stepInto();
    if (after().button("Over (F8)")) debugger.stepOver();
    if (after().button("Out (Sh+F8)")) debugger.stepOut();
    FlowRow state = after();
    state.spacing = 18.0f;
    if (host.paused) state.textColored(kAccent, host.breakReason.empty() ? "Paused" : host.breakReason.c_str());
    else state.textDisabled("Running");
}

// ============================================================== CPU
void GuiShell::cpuContent() {
    {
        GX4000* e = host.emu;
        Z80* c = e->cpu;
        const bool edit = host.paused;
        sectionHeading(edit ? "REGISTERS (editable while paused)" : "REGISTERS");

        auto pair16 = [&](const char* name, int& hi, int& lo) {
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", name);
            ImGui::TableNextColumn();
            int v = ((hi & 0xff) << 8) | (lo & 0xff);
            if (edit) {
                ImGui::SetNextItemWidth(52);
                ImGui::PushID(name);
                if (ImGui::InputScalar("##v", ImGuiDataType_S32, &v, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal)) {
                    hi = (v >> 8) & 0xff; lo = v & 0xff;
                }
                ImGui::PopID();
            } else ImGui::Text("%04X", v);
        };
        auto reg16 = [&](const char* name, int& r) {
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", name);
            ImGui::TableNextColumn();
            if (edit) {
                int v = r & 0xffff;
                ImGui::SetNextItemWidth(52);
                ImGui::PushID(name);
                if (ImGui::InputScalar("##v", ImGuiDataType_S32, &v, nullptr, nullptr, "%04X", ImGuiInputTextFlags_CharsHexadecimal)) r = v & 0xffff;
                ImGui::PopID();
            } else ImGui::Text("%04X", r & 0xffff);
        };
        auto reg8 = [&](const char* name, int& r) {
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", name);
            ImGui::TableNextColumn();
            if (edit) {
                int v = r & 0xff;
                ImGui::SetNextItemWidth(52);
                ImGui::PushID(name);
                if (ImGui::InputScalar("##v", ImGuiDataType_S32, &v, nullptr, nullptr, "%02X", ImGuiInputTextFlags_CharsHexadecimal)) r = v & 0xff;
                ImGui::PopID();
            } else ImGui::Text("%02X", r & 0xff);
        };
        if (ImGui::BeginTable("##regs", 4, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("n1", ImGuiTableColumnFlags_WidthFixed, 28);
            ImGui::TableSetupColumn("v1", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("n2", ImGuiTableColumnFlags_WidthFixed, 28);
            ImGui::TableSetupColumn("v2", ImGuiTableColumnFlags_WidthFixed, 60);
            reg16("PC", c->pc); reg16("SP", c->sp);
            pair16("AF", c->a, c->f); pair16("AF'", c->ap, c->fp);
            pair16("BC", c->b, c->c); pair16("BC'", c->bp, c->cp);
            pair16("DE", c->d, c->e); pair16("DE'", c->dp, c->ep);
            pair16("HL", c->h, c->l); pair16("HL'", c->hp, c->lp);
            reg16("IX", c->ix); reg16("IY", c->iy);
            reg8("I", c->i); reg8("R", c->r);
            ImGui::EndTable();
        }
        // Flags, clickable while paused.
        const char* names = "SZ5H3PNC";
        ImGui::TextDisabled("F");
        for (int b = 0; b < 8; b++) {
            ImGui::SameLine();
            int mask = 0x80 >> b;
            bool on = (c->f & mask) != 0;
            char label[8]; std::snprintf(label, sizeof(label), "%c##f%d", names[b], b);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? kAccent : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (ImGui::SmallButton(label) && edit) c->f ^= mask;
            ImGui::PopStyleColor();
        }
        ImGui::Text("IM %d   IFF1 %d  IFF2 %d  %s", c->im, c->iff1 ? 1 : 0, c->iff2 ? 1 : 0, c->halted ? "HALT" : "");
        if (edit) {
            if (after().smallButton(c->iff1 ? "DI" : "EI")) { c->iff1 = c->iff2 = !c->iff1; }
        }

        sectionHeading("STACK");
        for (int i = 0; i < 8; i++) {
            int a = (c->sp + i * 2) & 0xffff;
            int w = e->memory->readMapped(a) | (e->memory->readMapped((a + 1) & 0xffff) << 8);
            auto lbl = debugger.labelAt.find(w);
            ImGui::PushID(i);
            char line[96];
            std::snprintf(line, sizeof(line), "+%-2d %04X  %s", i * 2, w, lbl != debugger.labelAt.end() ? lbl->second.c_str() : "");
            if (ImGui::Selectable(line)) showInDisassembly(w);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click: show &%04X in the disassembly", w);
            ImGui::PopID();
        }

        sectionHeading("TIMING");
        ImGui::Text("machine T  %lld", (long long)e->machineCycles);
        ImGui::Text("picture    %d   beam %d,%d", e->classicMonitorFrame,
                    e->classicMonitorCharacter, e->classicMonitorLine);
    }
}

// ============================================================== Disassembly
// The instruction that ends exactly at `address`: decode forward from a little way back
// and keep the chain that lands on it (Z80 code cannot be decoded backwards).
int GuiShell::disasmPrevious(int address) {
    GX4000* e = host.emu;
    auto read = [e](int a) { return e->memory->readMapped(a & 0xffff); };
    address &= 0xffff;
    for (int back = 16; back >= 1; back--) {
        int s = (address - back) & 0xffff, last = -1;
        for (int guard = 0; guard < 16; guard++) {
            int dist = (address - s) & 0xffff;
            if (dist == 0) return last;
            if (dist > 16) break;
            last = s;
            DisasmResult d = debugger.disassembler.disassemble(read, s);
            s = d.next & 0xffff;
        }
    }
    return (address - 1) & 0xffff;
}

void GuiShell::showInDisassembly(int address) {
    panelOpen("Debugger") = true;
    disasmFollow = false;
    disasmTop = address & 0xffff;
    ImGui::SetWindowFocus("Debugger");
}

void GuiShell::showInMemory(int address) {
    panelOpen("Memory") = true;
    memView = 0;
    memFollow = false;
    memSelected = address & 0xffff;
    memTop = address & 0xfff0;
    memScrollTo = true;
    memTabRequest = 0;
    ImGui::SetWindowFocus("Memory");
}

// "&BB5A" in an operand becomes "TXT_OUTPUT" when the assembler named that address.
static std::string withLabels(const std::string& mnem, const std::unordered_map<int, std::string>& labels) {
    if (labels.empty()) return mnem;
    std::string out;
    for (size_t i = 0; i < mnem.size(); i++) {
        if (mnem[i] == '&' && i + 4 < mnem.size() + 1 && i + 5 <= mnem.size()) {
            bool hex4 = true;
            for (int k = 1; k <= 4; k++) hex4 = hex4 && std::isxdigit((unsigned char)mnem[i + k]);
            bool longer = i + 5 < mnem.size() && std::isxdigit((unsigned char)mnem[i + 5]);
            if (hex4 && !longer) {
                auto it = labels.find(std::stoi(mnem.substr(i + 1, 4), nullptr, 16));
                if (it != labels.end()) { out += it->second; i += 4; continue; }
            }
        }
        out += mnem[i];
    }
    return out;
}

void GuiShell::disassemblyContent() {
    {
        GX4000* e = host.emu;
        const int pc = e->cpu->pc & 0xffff;
        auto read = [e](int a) { return e->memory->readMapped(a & 0xffff); };

        ImGui::Checkbox("Follow PC", &disasmFollow);
        after().field(120, "##goto");
        if (ImGui::InputTextWithHint("##goto", "address / label", disasmGoto, sizeof(disasmGoto), ImGuiInputTextFlags_EnterReturnsTrue)) {
            int a = debugger.parseAddress(disasmGoto);
            if (a >= 0) { disasmTop = a; disasmFollow = false; }
        }
        if (after().smallButton("PC")) { disasmFollow = true; disasmLastPc = -1; }

        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##dis", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const int rows = std::max(4, (int)(ImGui::GetContentRegionAvail().y / lineH));

        // Keep the PC in view: re-anchor a few instructions above it when it leaves.
        if (disasmFollow && pc != disasmLastPc) {
            bool visible = false;
            int a = disasmTop;
            for (int i = 0; i < rows - 2; i++) { if (a == pc) { visible = true; break; } a = debugger.disassembler.disassemble(read, a).next & 0xffff; }
            if (!visible) { int t = pc; for (int i = 0; i < 3; i++) t = disasmPrevious(t); disasmTop = t; }
            disasmLastPc = pc;
        }
        if (ImGui::IsWindowHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) disasmFollow = false;
            for (int i = 0; i < (int)std::fabs(wheel) * 3; i++)
                disasmTop = wheel > 0 ? disasmPrevious(disasmTop) : (debugger.disassembler.disassemble(read, disasmTop).next & 0xffff);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float gutter = ImGui::GetTextLineHeight() + 6;
        int addr = disasmTop;
        for (int row = 0; row < rows; row++) {
            auto label = debugger.labelAt.find(addr);
            if (label != debugger.labelAt.end()) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gutter);
                ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s:", label->second.c_str());
                if (++row >= rows) break;
            }
            DisasmResult d = debugger.disassembler.disassemble(read, addr);
            ImGui::PushID(addr);
            // Gutter: click to toggle a breakpoint.
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##bp", ImVec2(gutter, ImGui::GetTextLineHeight()))) debugger.toggleBreakpoint(addr);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle breakpoint at &%04X", addr);
            float r = ImGui::GetTextLineHeight() * 0.32f;
            ImVec2 centre(p.x + gutter * 0.45f, p.y + ImGui::GetTextLineHeight() * 0.5f);
            for (const auto& bp : debugger.breakpoints) {
                if ((bp.address & 0xffff) != addr) continue;
                if (bp.enabled) dl->AddCircleFilled(centre, r, IM_COL32(220, 50, 50, 255));
                else dl->AddCircle(centre, r, IM_COL32(220, 50, 50, 255));
                break;
            }
            if (addr == pc) {
                ImVec2 a0(p.x + 1, p.y + 2), a1(p.x + gutter - 2, centre.y), a2(p.x + 1, p.y + ImGui::GetTextLineHeight() - 2);
                dl->AddTriangleFilled(a0, a1, a2, ImGui::GetColorU32(kPcColour));
            }
            ImGui::SameLine(0, 0);
            std::string bytes;
            for (int b : d.bytes) { char t[4]; std::snprintf(t, sizeof(t), "%02X ", b & 0xff); bytes += t; }
            char line[200];
            std::snprintf(line, sizeof(line), "%04X  %-12s %s", addr, bytes.c_str(), withLabels(d.mnem, debugger.labelAt).c_str());
            if (addr == pc) ImGui::PushStyleColor(ImGuiCol_Text, kPcColour);
            ImGui::Selectable(line, false, ImGuiSelectableFlags_AllowDoubleClick);
            if (addr == pc) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) debugger.toggleBreakpoint(addr);
            if (ImGui::BeginPopupContextItem("##ctx")) {
                ImGui::TextDisabled("&%04X", addr);
                if (ImGui::MenuItem("Toggle breakpoint")) debugger.toggleBreakpoint(addr);
                if (ImGui::MenuItem("Run to here")) debugger.runTo(addr);
                if (ImGui::MenuItem("Set PC here")) debugger.setPc(addr);
                if (ImGui::MenuItem("Show in memory")) showInMemory(addr);
                if (d.bytes.size() >= 3) {
                    int target = (d.bytes[d.bytes.size() - 2] & 0xff) | ((d.bytes.back() & 0xff) << 8);
                    char l[48]; std::snprintf(l, sizeof(l), "Follow &%04X", target);
                    if (ImGui::MenuItem(l)) showInDisassembly(target);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            addr = d.next & 0xffff;
        }
        ImGui::EndChild();
    }
}

// ============================================================== Memory
void GuiShell::memoryHexContent() {
    {
        GX4000* e = host.emu;
        GXMemory* m = e->memory;
        static const char* views[] = { "CPU view (64K as the Z80 sees it)", "Physical RAM", "ASIC RAM (Plus)" };
        ImGui::SetNextItemWidth(230);
        if (ImGui::Combo("##view", &memView, views, IM_ARRAYSIZE(views))) { memSelected = -1; memEditing = -1; memTop = 0; memScrollTo = true; }
        after().field(120, "##goto");
        if (ImGui::InputTextWithHint("##goto", "address / label", memGoto, sizeof(memGoto), ImGuiInputTextFlags_EnterReturnsTrue)) {
            int a = debugger.parseAddress(memGoto);
            if (a >= 0) { memSelected = a; memTop = a & ~15; memScrollTo = true; memFollow = false; }
        }
        if (after().checkbox("Follow PC", &memFollow) && memFollow) memView = 0;
        if (memFollow) {
            int pc = e->cpu->pc & 0xffff;
            if (pc != memSelected) { memSelected = pc; memTop = pc & ~15; memScrollTo = true; }
        }

        const int size = memView == 0 ? 0x10000 : memView == 1 ? (int)m->ram.size() : (int)m->asicRam.size();
        auto readAt = [&](int a) -> int {
            if (a < 0 || a >= size) return 0;
            if (memView == 0) return m->readMapped(a);
            if (memView == 1) return m->ram[(size_t)a];
            return m->asicRam[(size_t)a];
        };
        auto writeAt = [&](int a, int v) {
            if (a < 0 || a >= size) return;
            if (memView == 0) m->write(a, v);
            else if (memView == 1) m->ram[(size_t)a] = (uint8_t)v;
            else m->asicRam[(size_t)a] = (uint8_t)v;
        };

        if (memSelected >= 0 && memSelected < size) {
            int v = readAt(memSelected), w = v | (readAt(memSelected + 1) << 8);
            auto lbl = memView == 0 ? debugger.labelAt.find(memSelected) : debugger.labelAt.end();
            ImGui::Text("&%05X = &%02X  %3d  '%c'   word &%04X %s", memSelected, v, v, (v >= 32 && v < 127) ? v : '.', w,
                        lbl != debugger.labelAt.end() ? lbl->second.c_str() : "");
        } else {
            ImGui::TextDisabled("Click a byte to select it, double-click to edit it.");
        }

        ImGui::BeginChild("##mem", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        if (memScrollTo) { ImGui::SetScrollY((memTop / 16) * rowH); memScrollTo = false; }
        const float cellW = ImGui::CalcTextSize("00").x;
        const int pc = e->cpu->pc & 0xffff, sp = e->cpu->sp & 0xffff;
        ImGuiListClipper clip;
        clip.Begin((size + 15) / 16, rowH);
        while (clip.Step()) {
            for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                int base = row * 16;
                ImGui::TextDisabled(size > 0x10000 ? "%05X" : "%04X", base);
                for (int i = 0; i < 16; i++) {
                    int a = base + i;
                    ImGui::SameLine(0, i == 8 ? 12.0f : 6.0f);
                    ImGui::PushID(a);
                    if (memEditing == a) {
                        ImGui::SetNextItemWidth(cellW + 6);
                        ImGui::SetKeyboardFocusHere();
                        if (ImGui::InputText("##e", memEdit, 3, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                                                     ImGuiInputTextFlags_AutoSelectAll)) {
                            if (memEdit[0]) writeAt(a, (int)std::strtol(memEdit, nullptr, 16) & 0xff);
                            memEditing = a + 1 < size ? a + 1 : -1;
                            memSelected = a + 1;
                            std::snprintf(memEdit, sizeof(memEdit), "%02X", readAt(a + 1));
                        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                            memEditing = -1;
                        }
                    } else {
                        int v = readAt(a);
                        char t[4]; std::snprintf(t, sizeof(t), "%02X", v);
                        bool mark = memView == 0 && (a == pc || a == sp);
                        if (mark) ImGui::PushStyleColor(ImGuiCol_Text, a == pc ? kPcColour : ImVec4(0.55f, 0.8f, 1.0f, 1.0f));
                        else if (v == 0) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        if (ImGui::Selectable(t, memSelected == a, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(cellW, 0))) {
                            memSelected = a;
                            if (ImGui::IsMouseDoubleClicked(0)) { memEditing = a; std::snprintf(memEdit, sizeof(memEdit), "%02X", v); }
                        }
                        if (mark || v == 0) ImGui::PopStyleColor();
                        if (memView == 0 && ImGui::BeginPopupContextItem("##ctx")) {
                            ImGui::TextDisabled("&%04X", a);
                            if (ImGui::MenuItem("Show in disassembly")) showInDisassembly(a);
                            if (ImGui::MenuItem("Watch writes here")) { GuiWatchpoint w; w.start = w.end = a; w.onWrite = true; debugger.watchpoints.push_back(w); }
                            if (ImGui::MenuItem("Watch reads here")) { GuiWatchpoint w; w.start = w.end = a; w.onRead = true; w.onWrite = false; debugger.watchpoints.push_back(w); }
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::PopID();
                }
                char text[17];
                for (int i = 0; i < 16; i++) { int v = readAt(base + i); text[i] = (v >= 32 && v < 127) ? (char)v : '.'; }
                text[16] = 0;
                ImGui::SameLine(0, 12.0f);
                ImGui::TextDisabled("%s", text);
            }
        }
        ImGui::EndChild();
    }
}

// ============================================================== Breakpoints
void GuiShell::breakpointsContent() {
    {
        sectionHeading("BREAKPOINTS");
        FlowRow bp;
        bp.field(110, "##bpaddr");
        bool add = ImGui::InputTextWithHint("##bpaddr", "address/label", bpAddress, sizeof(bpAddress), ImGuiInputTextFlags_EnterReturnsTrue);
        // the condition takes what the line has left, but never less than 140 (then it wraps)
        {
            const float addW = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine();
            const float left = ImGui::GetContentRegionAvail().x - addW;
            if (left < 140.0f) ImGui::NewLine();
            ImGui::SetNextItemWidth(std::max(140.0f, ImGui::GetContentRegionAvail().x - addW));
        }
        add |= ImGui::InputTextWithHint("##bpcond", "condition (optional)", bpCondition, sizeof(bpCondition), ImGuiInputTextFlags_EnterReturnsTrue);
        bp.first = false;
        add |= bp.button("Add##bp");
        if (add) {
            int a = debugger.parseAddress(bpAddress);
            if (a >= 0) { debugger.addBreakpoint(a, bpCondition); bpAddress[0] = 0; bpCondition[0] = 0; }
        }
        if (ImGui::BeginTable("##bps", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("On");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Label");
            ImGui::TableSetupColumn("Condition", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Hits");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int remove = -1;
            for (size_t i = 0; i < debugger.breakpoints.size(); i++) {
                GuiBreakpoint& bp = debugger.breakpoints[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##on", &bp.enabled)) debugger.breakpointsChanged();
                ImGui::TableNextColumn();
                char a[16]; std::snprintf(a, sizeof(a), "&%04X", bp.address & 0xffff);
                if (ImGui::Selectable(a)) showInDisassembly(bp.address);
                ImGui::TableNextColumn();
                auto lbl = debugger.labelAt.find(bp.address & 0xffff);
                ImGui::TextUnformatted(lbl != debugger.labelAt.end() ? lbl->second.c_str() : (bp.fromAssembler ? "(BRK)" : ""));
                ImGui::TableNextColumn();
                char cond[128]; std::snprintf(cond, sizeof(cond), "%s", bp.condition.c_str());
                ImGui::SetNextItemWidth(-1);
                if (!bp.error.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                ImGui::InputTextWithHint("##cond", "always", cond, sizeof(cond));
                if (!bp.error.empty()) ImGui::PopStyleColor();
                if (ImGui::IsItemDeactivatedAfterEdit()) debugger.setCondition(bp, cond);
                if (!bp.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bp.error.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d", bp.hits);
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) remove = (int)i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (remove >= 0) debugger.removeBreakpoint((size_t)remove);
        }
        if (debugger.breakpoints.empty()) ImGui::TextDisabled("Click the gutter in the disassembly, or add one above.");
        if (ImGui::TreeNode("Condition syntax")) {
            ImGui::TextWrapped("Registers A F B C D E H L AF BC DE HL IX IY SP PC I R IM, AF2..HL2 (the shadow set), "
                               "flags SF ZF HF PF NF CF, HITS, BANK, ROM, MMR. Memory: [addr], MEM(addr), WORD(addr), BIT(v,n). "
                               "Numbers &FF, $FF or decimal. Operators == != < > <= >= && || ! + - * / & | ^ << >>.");
            ImGui::TextDisabled("e.g.   A==&3F && HITS>2     [&BE80]!=0     WORD(SP)==&0038");
            ImGui::TreePop();
        }

        sectionHeading("WATCHPOINTS (memory)");
        FlowRow wp;
        wp.field(100, "##wps");
        ImGui::InputTextWithHint("##wps", "from", wpStart, sizeof(wpStart));
        wp.field(100, "##wpe");
        ImGui::InputTextWithHint("##wpe", "to (optional)", wpEnd, sizeof(wpEnd));
        after().checkbox("read", &wpRead);
        after().checkbox("write", &wpWrite);
        if (after().button("Add##wp")) {
            int s = debugger.parseAddress(wpStart), t = wpEnd[0] ? debugger.parseAddress(wpEnd) : s;
            if (s >= 0 && t >= 0 && (wpRead || wpWrite)) {
                GuiWatchpoint w; w.start = std::min(s, t); w.end = std::max(s, t); w.onRead = wpRead; w.onWrite = wpWrite;
                debugger.watchpoints.push_back(w);
                wpStart[0] = wpEnd[0] = 0;
            }
        }
        if (ImGui::BeginTable("##wpt", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("On");
            ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("R");
            ImGui::TableSetupColumn("W");
            ImGui::TableSetupColumn("Hits");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            int remove = -1;
            for (size_t i = 0; i < debugger.watchpoints.size(); i++) {
                GuiWatchpoint& w = debugger.watchpoints[i];
                ImGui::PushID(1000 + (int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Checkbox("##on", &w.enabled);
                ImGui::TableNextColumn();
                char r[32];
                if (w.start == w.end) std::snprintf(r, sizeof(r), "&%04X", w.start); else std::snprintf(r, sizeof(r), "&%04X-&%04X", w.start, w.end);
                if (ImGui::Selectable(r)) showInMemory(w.start);
                ImGui::TableNextColumn(); ImGui::Checkbox("##r", &w.onRead);
                ImGui::TableNextColumn(); ImGui::Checkbox("##w", &w.onWrite);
                ImGui::TableNextColumn(); ImGui::Text("%d", w.hits);
                ImGui::TableNextColumn(); if (ImGui::SmallButton("x")) remove = (int)i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (remove >= 0) debugger.watchpoints.erase(debugger.watchpoints.begin() + remove);
        }
        ImGui::TextDisabled("A watchpoint stops the machine after the instruction that made the access.");
    }
}

// ============================================================== Video
static const char* CRTC_REGISTER_NAMES[18] = {
    "Horizontal total", "Horizontal displayed", "HSYNC position", "Sync widths (V|H)",
    "Vertical total", "Vertical total adjust", "Vertical displayed", "VSYNC position",
    "Interlace & skew", "Max raster address", "Cursor start", "Cursor end",
    "Start address H", "Start address L", "Cursor H", "Cursor L", "Light pen H", "Light pen L" };

void GuiShell::chipTabsVideo() {
    {
        GX4000* e = host.emu;
        {
            CRTC6845* cr = e->crtc;
            if (cr && ImGui::BeginTabItem("CRTC")) {
                ImGui::BeginChild("##crtc");
                ImGui::Text("Type %d   %s", cr->type, e->plusHardware ? "(inside the ASIC)" : "");
                sectionHeading("COUNTERS");
                if (beginFacts("##crtccount", 150)) {
                    fact("C0  character", "%3d  of R0=%d", cr->horizontal, cr->registers[0]);
                    fact("C3l HSYNC width", "%3d  of %d", cr->hsyncCounter, cr->registers[3] & 15);
                    fact("C4  row", "%3d  of R4=%d", cr->vertical, cr->registers[4]);
                    fact("C9  raster", "%3d  of R9=%d", cr->raster, cr->registers[9]);
                    fact("C3h VSYNC line", "%3d  of %d", cr->vsyncCounter, (cr->registers[3] >> 4) ? (cr->registers[3] >> 4) : 16);
                    fact("C5  adjust", "%3d  of R5=%d%s", cr->verticalAdjust, cr->registers[5], cr->verticalAdjustState ? "  (in adjust)" : "");
                    fact("MA", "&%04X   row start &%04X", cr->memoryAddress(), cr->memoryAddressBase());
                    fact("R12/R13 start", "&%04X", cr->screenAddress());
                    fact("cursor", "&%04X", cr->cursorAddress());
                    fact("frame / field", "%d  field %d", cr->frame, cr->interlaceField);
                    endFacts();
                }
                sectionHeading("STATE");
                ledGrid({ { cr->hDisplay, "HDISP" }, { cr->vDisplay, "VDISP" } }, 110.0f);
                ledGrid({ { cr->lastFrameLine, "last line" }, { cr->vsyncGhost, "ghost VSYNC" } }, 110.0f);
                ledGrid({ { cr->verticalAdjustState, "adjust" }, { cr->r9Match, "C9=R9" } }, 110.0f);
                sectionHeading("OUTPUT PINS");
                ledGrid({ { cr->displayOutputEnabled(), "DISPEN" }, { cr->cursorOutput(), "CURSOR" } }, 110.0f);
                ledGrid({ { cr->hsync, "HSYNC" }, { cr->vsync, "VSYNC" } }, 110.0f);
                ImGui::TextUnformatted("MA"); ImGui::SameLine(40);
                for (int b = 13; b >= 0; b--) { led((cr->memoryAddress() >> b) & 1); ImGui::SameLine(0, 1); }
                ImGui::NewLine();
                ImGui::TextUnformatted("RA"); ImGui::SameLine(40);
                for (int b = 4; b >= 0; b--) { led((cr->raster >> b) & 1); ImGui::SameLine(0, 1); }
                ImGui::NewLine();
                sectionHeading("REGISTERS");
                if (ImGui::BeginTable("##crtcregs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("R");
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Hex");
                    ImGui::TableSetupColumn("Dec");
                    ImGui::TableHeadersRow();
                    for (int r = 0; r < 18; r++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (r == cr->selected) ImGui::TextColored(kAccent, "R%d", r); else ImGui::Text("R%d", r);
                        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", CRTC_REGISTER_NAMES[r]);
                        ImGui::TableNextColumn(); ImGui::Text("&%02X", cr->registers[r]);
                        ImGui::TableNextColumn(); ImGui::Text("%3d", cr->registers[r]);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("The selected register is highlighted.");
                ImGui::TextDisabled("CRTC behaviour sourced from the \"Amstrad CPC CRTC Compendium\" by Longshot (CC BY-NC-ND)");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            GateArray* ga = e->gateArray;
            if (ga && ImGui::BeginTabItem("Gate Array")) {
                ImGui::BeginChild("##ga");
                ImGui::Text("%s", ga->model ? ga->model->name() : "Gate Array");
                if (beginFacts("##gafacts", 150)) {
                    if (ga->newMode != ga->mode) fact("mode", "%d  (%d from the next HSYNC)", ga->mode, ga->newMode);
                    else fact("mode", "%d", ga->mode);
                    fact("R52 (interrupt)", "%2d%s", ga->interruptCounter, ga->interruptRaiseIn ? "  (raising)" : "");
                    fact("selected pen", ga->paletteIndex == 16 ? "BORDER" : "%d", ga->paletteIndex);
                    fact("RMR (ROM config)", "&%02X", ga->romConfig & 0xff);
                    static const int bankTable[8][4] = { {0,1,2,3},{0,1,2,7},{4,5,6,7},{0,3,2,7},{0,4,2,3},{0,5,2,3},{0,6,2,3},{0,7,2,3} };
                    const int* bk = bankTable[e->memory->ramConfig & 7];
                    fact("RAM config", "&%02X  banks %d %d %d %d", e->memory->ramConfig & 0xff, bk[0], bk[1], bk[2], bk[3]);
                    fact("lower ROM", "%s", e->memory->lowerEnabled ? "enabled" : "disabled");
                    fact("upper ROM", "%s  (#%d)", e->memory->upperEnabled ? "enabled" : "disabled", e->memory->upperRom);
                    fact("VSYNC to monitor", "%s  V26 %d", ga->sigGaVsync ? "on" : "off", ga->v26);
                    fact("HSYNC to monitor", "%s", ga->sigGaHsync ? "on" : "off");
                    endFacts();
                }
                sectionHeading("INKS (hardware colour number)");
                if (host.video) {
                    for (int i = 0; i < 17; i++) {
                        ImGui::PushID(i);
                        ImGui::BeginGroup();
                        ImGui::ColorButton("##ink", rgbToVec(host.video->penColor(i)), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(26, 20));
                        if (i < 16) ImGui::Text("%2d:%02d", i, ga->gaPalette[i] & 0x1f); else ImGui::Text("B :%02d", ga->gaPalette[16] & 0x1f);
                        ImGui::EndGroup();
                        if (ImGui::IsItemHovered()) {
                            if (i < 16) ImGui::SetTooltip("pen %d, hardware colour %d", i, ga->gaPalette[i] & 0x1f);
                            else ImGui::SetTooltip("border, hardware colour %d", ga->gaPalette[16] & 0x1f);
                        }
                        ImGui::PopID();
                        if (i % 9 != 8 && i != 16) ImGui::SameLine();
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            CtmMonitor* mon = e->monitorRenderer;
            if (mon && ImGui::BeginTabItem("Monitor")) {
                ImGui::BeginChild("##mon");
                ImGui::Text("%s", mon->model ? mon->model->name : "CTM 640/644 (reference calibration)");
                sectionHeading("HORIZONTAL (flywheel)");
                if (beginFacts("##monh", 170)) {
                    fact("oscillator period", "%d Pixel-M2 (%.2f usec)", mon->hsyncLimit, mon->hsyncLimit / 16.0);
                    fact("measured sync period", "%d  (%.2f usec)", mon->measuredPeriod, mon->measuredPeriod / 16.0);
                    fact("line offset", "%d", mon->lineOffset);
                    fact("sync pulse width", "%d characters", mon->lastPulseWidth);
                    endFacts();
                }
                sectionHeading("VERTICAL");
                if (beginFacts("##monv", 170)) {
                    fact("locked field", "%d lines", mon->lockedLines);
                    fact("last field", "%d half lines", mon->lastVsyncPeriod);
                    fact("vertical limit", "%d half lines", mon->vsyncLimit);
                    fact("half-line offset", "%d", mon->verticalHalfLine);
                    fact("vertical sync seen", "%s", mon->vsyncSeparated ? "yes" : "no");
                    endFacts();
                }
                sectionHeading("PICTURE");
                ImGui::Text("beam  character %d  line %d", e->classicMonitorCharacter, e->classicMonitorLine);
                ImGui::Text("refresh %.2f Hz", host.cpcRefreshHz);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            PlusAsic* asic = e->asic;
            if (asic && e->plusHardware && ImGui::BeginTabItem("Plus ASIC")) {
                ImGui::BeginChild("##asic");
                if (beginFacts("##asicf", 160)) {
                    fact("state", "%s", asic->locked ? "locked" : "UNLOCKED");
                    fact("ASIC page", "%s", e->memory->asicRamEnabled ? "mapped at &4000" : "not mapped");
                    fact("raster interrupt", "line %d", asic->rasterInterruptLine);
                    fact("split screen", "line %d -> &%04X", asic->rasterSplitLine, asic->splitAddress);
                    fact("soft scroll", "&%02X  (h %d, v %d)", asic->softScrollControl, asic->horizontalScroll, asic->verticalScroll);
                    fact("interrupt vector", "&%02X", asic->interruptVector);
                    endFacts();
                }
                sectionHeading("DMA (sound list channels)");
                if (ImGui::BeginTable("##dma", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (const char* h : { "Ch", "Active", "Pointer", "Loop", "Pause", "Prescale" }) ImGui::TableSetupColumn(h);
                    ImGui::TableHeadersRow();
                    for (int ch = 0; ch < 3; ch++) {
                        const DmaChannel& d = asic->dma[ch];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%d", ch);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.active ? "yes" : "no");
                        ImGui::TableNextColumn(); ImGui::Text("&%04X", d.pointer & 0xffff);
                        ImGui::TableNextColumn(); ImGui::Text("%d", d.loopCount);
                        ImGui::TableNextColumn(); ImGui::Text("%d", d.pause);
                        ImGui::TableNextColumn(); ImGui::Text("%d", d.prescale);
                    }
                    ImGui::EndTable();
                }
                sectionHeading("PALETTE (inks 0-15, border, sprite pens 1-15)");
                for (int i = 0; i < 32; i++) {
                    ImGui::PushID(100 + i);
                    ImGui::ColorButton("##p", rgbToVec(asic->colorValue(asic->palette[i])), ImGuiColorEditFlags_NoDragDrop, ImVec2(18, 18));
                    ImGui::PopID();
                    if (i % 16 != 15) ImGui::SameLine();
                }
                sectionHeading("SPRITES");
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float px = 3.0f;
                for (int s = 0; s < 16; s++) {
                    XY pos = spriteCoordinates(e->memory->asicRam, 0x2000 + s * 8);
                    int mag = asic->spriteMagnification[s];
                    ImGui::BeginGroup();
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(p, ImVec2(p.x + 16 * px, p.y + 16 * px), IM_COL32(20, 20, 24, 255));
                    for (int y = 0; y < 16; y++)
                        for (int x = 0; x < 16; x++) {
                            int pen = asic->sprites[s][y * 16 + x] & 15;
                            if (!pen) continue;
                            ImVec4 c = rgbToVec(asic->colorValue(asic->palette[16 + pen]));
                            dl->AddRectFilled(ImVec2(p.x + x * px, p.y + y * px), ImVec2(p.x + (x + 1) * px, p.y + (y + 1) * px), ImGui::GetColorU32(c));
                        }
                    ImGui::Dummy(ImVec2(16 * px, 16 * px));
                    ImGui::TextDisabled("%2d %s", s, (mag >> 2 & 3) && (mag & 3) ? "on" : "off");
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("sprite %d at %d,%d  magnification x%d y%d", s, pos.x, pos.y, mag >> 2 & 3, mag & 3);
                    if (s % 8 != 7) ImGui::SameLine();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
    }
}

// ============================================================== Audio & I/O
static const char* AY_REGISTER_NAMES[16] = {
    "Tone A fine", "Tone A coarse", "Tone B fine", "Tone B coarse", "Tone C fine", "Tone C coarse",
    "Noise period", "Mixer (0 = on)", "Volume A", "Volume B", "Volume C",
    "Envelope fine", "Envelope coarse", "Envelope shape", "Port A (keyboard)", "Port B" };

// The CPC keyboard matrix, row by row, bit 0 first (Amstrad's own table).
static const char* KEY_NAMES[10][8] = {
    { "Up", "Right", "Down", "f9", "f6", "f3", "Enter", "f." },
    { "Left", "Copy", "f7", "f8", "f5", "f1", "f2", "f0" },
    { "Clr", "[", "Return", "]", "f4", "Shift", "\\", "Ctrl" },
    { "^", "-", "@", "P", ";", ":", "/", "." },
    { "0", "9", "O", "I", "L", "K", "M", "," },
    { "8", "7", "U", "Y", "H", "J", "N", "Space" },
    { "6", "5", "R", "T", "G", "F", "B", "V" },
    { "4", "3", "E", "W", "S", "D", "C", "X" },
    { "1", "2", "Esc", "Q", "Tab", "A", "Caps", "Z" },
    { "J Up", "J Down", "J Left", "J Right", "Fire 2", "Fire 1", "(9.6)", "Del" } };

static const char* fdcCommandName(int command) {
    switch (command & 0x1f) {
        case 0x02: return "READ TRACK";
        case 0x03: return "SPECIFY";
        case 0x04: return "SENSE DRIVE STATUS";
        case 0x05: return "WRITE DATA";
        case 0x06: return "READ DATA";
        case 0x07: return "RECALIBRATE";
        case 0x08: return "SENSE INTERRUPT STATUS";
        case 0x09: return "WRITE DELETED DATA";
        case 0x0a: return "READ ID";
        case 0x0c: return "READ DELETED DATA";
        case 0x0d: return "FORMAT TRACK";
        case 0x0f: return "SEEK";
        case 0x11: return "SCAN EQUAL";
        case 0x19: return "SCAN LOW OR EQUAL";
        case 0x1d: return "SCAN HIGH OR EQUAL";
        default:   return "INVALID";
    }
}

void GuiShell::chipTabsIo() {
    {
        GX4000* e = host.emu;
        {
            AY38912* ay = e->ay;
            if (ay && ImGui::BeginTabItem("PSG")) {
                ImGui::BeginChild("##psg");
                ImGui::TextDisabled("AY-3-8912, 1 MHz");
                const auto& r = ay->registers;
                const char* ch = "ABC";
                if (ImGui::BeginTable("##chans", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (const char* h : { "Ch", "Period", "Frequency", "Tone", "Noise", "Volume" }) ImGui::TableSetupColumn(h, h[0] == 'V' ? ImGuiTableColumnFlags_WidthStretch : 0);
                    ImGui::TableHeadersRow();
                    for (int c = 0; c < 3; c++) {
                        int period = r[c * 2] | ((r[c * 2 + 1] & 15) << 8);
                        bool tone = !(r[7] & (1 << c)), noise = !(r[7] & (8 << c));
                        int vol = r[8 + c];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%c", ch[c]);
                        ImGui::TableNextColumn(); ImGui::Text("%4d", period);
                        ImGui::TableNextColumn();
                        if (period) ImGui::Text("%8.1f Hz", 1000000.0 / (16.0 * period)); else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn(); led(tone);
                        ImGui::TableNextColumn(); led(noise);
                        ImGui::TableNextColumn();
                        char l[24];
                        if (vol & 16) std::snprintf(l, sizeof(l), "envelope (%d)", ay->envelopeVolume); else std::snprintf(l, sizeof(l), "%d", vol & 15);
                        ImGui::ProgressBar(((vol & 16) ? ay->envelopeVolume : (vol & 15)) / 15.0f, ImVec2(-1, 0), l);
                    }
                    ImGui::EndTable();
                }
                int envPeriod = r[11] | (r[12] << 8);
                ImGui::Text("Noise period %d   Envelope period %d (%.2f Hz)  shape &%X%s", r[6] & 31, envPeriod,
                            envPeriod ? 1000000.0 / (256.0 * envPeriod) : 0.0, r[13] & 15, ay->envelopeHolding ? "  holding" : "");
                sectionHeading("REGISTERS");
                if (ImGui::BeginTable("##ayregs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    for (int i = 0; i < 16; i++) {
                        ImGui::TableNextColumn();
                        if (i == ay->selected) ImGui::TextColored(kAccent, "R%-2d", i); else ImGui::Text("R%-2d", i);
                        ImGui::SameLine(); ImGui::Text("&%02X", r[i]);
                        after().textDisabled(AY_REGISTER_NAMES[i]);
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("The selected register is highlighted.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            PPI8255* ppi = e->ppi;
            if (ppi && ImGui::BeginTabItem("PPI")) {
                ImGui::BeginChild("##ppi");
                ImGui::TextDisabled("8255 PPI");
                int portB = ppi->read(0xf500);
                if (beginFacts("##ppif", 150)) {
                    fact("control", "&%02X", ppi->control & 0xff);
                    fact("port A", "&%02X  %s", ppi->portA & 0xff, ppi->portAInput ? "input (PSG data)" : "output (PSG data)");
                    fact("port B", "&%02X  %s", portB & 0xff, ppi->portBInput ? "input" : "output (latched)");
                    fact("port C", "&%02X  %s", ppi->portC & 0xff, "output");
                    endFacts();
                }
                sectionHeading("PORT B (reads)");
                ledGrid({ { portB & 1, "VSYNC" }, { portB & 0x80, "cassette in" } }, 160.0f);
                ledGrid({ { portB & 0x40, "printer busy" }, { portB & 0x20, "/EXP" } }, 160.0f);
                led(portB & 0x10, "50 Hz (LK4)");
                static const char* makers[8] = { "Isp", "Triumph", "Saisho", "Solavox", "Awa", "Schneider", "Orion", "Amstrad" };
                { char m[48]; std::snprintf(m, sizeof m, "maker (LK1-3): %s", makers[(portB >> 1) & 7]); after().text(m); }
                sectionHeading("PORT C (writes)");
                static const char* psgFunction[4] = { "inactive", "read", "write", "select register" };
                ImGui::Text("keyboard row %d", ppi->portC & 15);
                ledGrid({ { ppi->portC & 0x10, "cassette motor" }, { ppi->portC & 0x20, "cassette out" } }, 160.0f);
                ImGui::Text("PSG bus: %s", psgFunction[(ppi->portC >> 6) & 3]);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            KeyboardMatrix* kb = e->keyboard;
            if (kb && ImGui::BeginTabItem("Keyboard")) {
                ImGui::BeginChild("##kbd");
                ImGui::TextDisabled("The matrix as the PPI scans it; lit = pressed.");
                if (ImGui::BeginTable("##matrix", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn("row");
                    for (int b = 0; b < 8; b++) { char h[8]; std::snprintf(h, sizeof(h), "bit %d", b); ImGui::TableSetupColumn(h); }
                    ImGui::TableHeadersRow();
                    for (int row = 0; row < 10; row++) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (row == (ppi ? (ppi->portC & 15) : -1)) ImGui::TextColored(kAccent, "%d", row); else ImGui::Text("%d", row);
                        for (int b = 0; b < 8; b++) {
                            ImGui::TableNextColumn();
                            bool down = !(kb->rows[row] & (1 << b));
                            if (down) ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(60, 150, 70, 200));
                            ImGui::TextUnformatted(KEY_NAMES[row][b]);
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TextDisabled("The row the PPI is selecting is highlighted.");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Disc")) {
                ImGui::BeginChild("##fdc");
                ImGui::TextDisabled("uPD765A disc controller");
                if (e->hasFdc && e->fdc) {
                    UPD765A* fd = e->fdc;
                    static const char* phases[] = { "command", "execution", "result" };
                    if (beginFacts("##fdcf", 150)) {
                        fact("phase", "%s", fd->phase >= 0 && fd->phase < 3 ? phases[fd->phase] : "?");
                        fact("command", "&%02X  %s", fd->command & 0xff, fdcCommandName(fd->command));
                        std::string ps;
                        for (int p : fd->params) { char t[4]; std::snprintf(t, sizeof(t), "%02X ", p & 0xff); ps += t; }
                        fact("parameters", "%s", ps.empty() ? "-" : ps.c_str());
                        fact("transfer", "%d / %d bytes", fd->transferIndex, (int)fd->transfer.size());
                        fact("motor", "%s", fd->motor ? "on" : "off");
                        fact("interrupt status", "&%02X", fd->interruptState & 0xff);
                        fact("active drive", "%c", 'A' + (fd->activeDriveIndex & 1));
                        endFacts();
                    }
                    sectionHeading("DRIVES");
                    for (int d = 0; d < 2; d++) {
                        auto disk = fd->drives[d];
                        ImGui::Text("%c:  track %2d   %s", 'A' + d, fd->tracks[d], host.diskName[d].empty() ? "(empty)" : host.diskName[d].c_str());
                        if (disk) {
                            int sect = 0, bytes = 0;
                            for (auto& col : disk->trackData) { for (auto& t : col) if (t && !t->sectors.empty()) { sect = (int)t->sectors.size(); bytes = 128 << (t->sectors[0].n & 7); break; } if (sect) break; }
                            ImGui::TextDisabled("    %d tracks, %d side%s, %d sectors of %d bytes%s", disk->tracks, disk->sides, disk->sides == 1 ? "" : "s",
                                                sect, bytes, disk->modified ? ", modified" : "");
                        }
                    }
                } else {
                    ImGui::TextDisabled("This machine has no disc controller.");
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            CPCTapeDrive* tape = e->tape;
            if (tape && ImGui::BeginTabItem("Tape")) {
                ImGui::BeginChild("##tape");
                if (beginFacts("##tapef", 150)) {
                    fact("tape", "%s", tape->loaded ? tape->name.c_str() : "(none)");
                    if (tape->loaded) {
                        fact("format", "%s", tape->format.c_str());
                        fact("block", "%d of %d%s", tape->currentBlock + 1, (int)tape->blocks.size(), tape->tapeEnded ? "  (end)" : "");
                        fact("counter", "%03d", tape->counter());
                    }
                    fact("deck", "%s", tape->playing ? (tape->paused ? "PAUSE" : "PLAY") : "stopped");
                    fact("motor relay", "%s", tape->motorOn ? "on" : "off");
                    fact("signal", "%s", tape->getPortBBit() ? "high" : "low");
                    endFacts();
                }
                tapeDeckControls(false);   // the same deck as the Media window's
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
    }
}

// ============================================================== the grouped windows
// Debugger: the toolbar; the registers beside the disassembly; breakpoints and
// watchpoints below them (their height the user's: drag the line between).
void GuiShell::windowDebugger() {
    if (!panelOpen("Debugger")) return;
    if (beginTool("Debugger", ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) && machineReady(host)) {
        WrapText wrapText;                     // text wraps at the window's edge
        debugToolbar();
        const float avail = ImGui::GetContentRegionAvail().y;
        if (debuggerTopH <= 0) debuggerTopH = avail * 0.68f;
        debuggerTopH = std::clamp(debuggerTopH, 120.0f, std::max(120.0f, avail - 60.0f));
        ImGui::BeginChild("##dbgtop", ImVec2(0, debuggerTopH), ImGuiChildFlags_ResizeY, ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginChild("##dbgregs", ImVec2(ImGui::GetFontSize() * 18.0f, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        cpuContent();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##dbgcode", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        disassemblyContent();
        ImGui::EndChild();
        debuggerTopH = ImGui::GetWindowHeight();
        ImGui::EndChild();
        ImGui::BeginChild("##dbgbps", ImVec2(0, 0), ImGuiChildFlags_Borders);
        breakpointsContent();
        ImGui::EndChild();
    }
    ImGui::End();
}

// Chips: one tab a chip.
void GuiShell::windowChips() {
    if (!panelOpen("Chips")) return;
    if (beginTool("Chips") && machineReady(host)) {
        WrapText wrapText;                     // text wraps at the window's edge
        if (ImGui::BeginTabBar("##chips", ImGuiTabBarFlags_FittingPolicyScroll)) {
            chipTabsVideo();
            chipTabsIo();
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// Memory: the hex editor and the map.
void GuiShell::windowMemory() {
    memMapShown = false;
    if (!panelOpen("Memory")) return;
    if (beginTool("Memory") && machineReady(host)) {
        WrapText wrapText;                     // text wraps at the window's edge
        if (ImGui::BeginTabBar("##memtabs")) {
            if (ImGui::BeginTabItem("Hex", nullptr, memTabRequest == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { memoryHexContent(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Map", nullptr, memTabRequest == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { memMapShown = true; memoryMapContent(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        memTabRequest = -1;
    }
    ImGui::End();
}

} // namespace cpcse
