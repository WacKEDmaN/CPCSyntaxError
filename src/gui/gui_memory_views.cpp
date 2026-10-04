// CPCSyntaxError GUI — the memory map (the Memory window's Map tab) and the GFX9000's
// internals (the GFX9000 window's tabs).
//
// Memory map: one 64K block of RAM at a time (the base 64K, or an expansion bank picked
// from a list as long as the RAM fitted), as a picture filling the window, one pixel a
// byte, 256 bytes a row, coloured by what the Z80 used it for since the map was
// cleared: opcode fetches (M1), operand bytes, data reads, data writes. The CPU tags its
// own accesses (Z80Memory::noteAccess); the map only records while this window is open.
//
// GFX9000 internals: the V9990's ports, registers (named after the Application Manual's
// 12.1/12.2, p.74-87), palette, command engine and VRAM -- as a picture of the image
// space and as bytes at the addresses the CPU gives P#0.
#include "gui_shell.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/emulator.h"
#include "core/crtc.h"
#include "core/memory.h"
#include "core/v9990.h"
#include "core/z80.h"

namespace cpcse {

static constexpr int MAP_CHUNK = 0x10000;

// In rising priority: a byte shows the highest class it earned.
enum MapClass { MAP_NONE, MAP_READ, MAP_WRITE, MAP_READWRITE, MAP_OPERAND, MAP_OPCODE, MAP_WRITTEN_CODE, MAP_CLASSES };
struct MapLegend { const char* name; int r, g, b; };
static const MapLegend MAP_LEGEND[MAP_CLASSES] = {
    { "untouched", 0, 0, 0 },
    { "data read", 70, 185, 90 },
    { "data written", 75, 125, 240 },
    { "read + written", 70, 205, 215 },
    { "operand", 240, 155, 50 },
    { "opcode (M1)", 240, 70, 60 },
    { "written code", 220, 90, 220 },
};

static int mapClass(int flags, bool writtenCode) {
    const bool code = flags & (Z80Memory::ACCESS_OPCODE | Z80Memory::ACCESS_OPERAND);
    const bool read = flags & Z80Memory::ACCESS_READ, write = flags & Z80Memory::ACCESS_WRITE;
    if (code && write && writtenCode) return MAP_WRITTEN_CODE;
    if (flags & Z80Memory::ACCESS_OPCODE) return MAP_OPCODE;
    if (flags & Z80Memory::ACCESS_OPERAND) return MAP_OPERAND;
    if (read && write) return MAP_READWRITE;
    if (write) return MAP_WRITE;
    if (read) return MAP_READ;
    return MAP_NONE;
}

static uint32_t abgr(int r, int g, int b) {
    return 0xff000000u | (uint32_t)std::clamp(b, 0, 255) << 16 | (uint32_t)std::clamp(g, 0, 255) << 8 | (uint32_t)std::clamp(r, 0, 255);
}

// A byte's colour: its class, a little brighter the bigger its value, so the contents
// still show their shape; untouched bytes are a dim grey of their value.
static uint32_t mapColour(int cls, int value) {
    if (cls == MAP_NONE) {
        const int g = value ? 34 + value * 40 / 255 : 14;
        return abgr(g, g, g + 4);
    }
    const MapLegend& l = MAP_LEGEND[cls];
    const int k = 175 + value * 80 / 255;          // 69%..100%
    return abgr(l.r * k / 255, l.g * k / 255, l.b * k / 255);
}

static ImU32 legendColour(int cls) {
    const MapLegend& l = MAP_LEGEND[cls];
    return cls == MAP_NONE ? IM_COL32(40, 40, 46, 255) : IM_COL32(l.r, l.g, l.b, 255);
}

// The 64K chunk's name: the base RAM, or the &7Fxx value that pages the bank in (C4-C7).
static std::string chunkLabel(const GXMemory& m, int chunk) {
    if (chunk == 0) return "Base 64K";
    char b[48];
    const int k = chunk - 1;
    if (m.segmentedExpansion()) std::snprintf(b, sizeof b, "Bank %d  &%02X%02X", chunk, 0x7f - k / 8, 0xc4 | (k % 8) << 3);
    else std::snprintf(b, sizeof b, "Bank %d  &7F%02X", chunk, 0xc4 | (k & 7) << 3);
    return b;
}

// Where the CPU sees a byte of RAM right now, for reads (-1: not paged in, or a ROM over it).
static int cpuAddressOf(const GXMemory& m, int physical) {
    for (int slot = 0; slot < 8; slot++) {
        const uint32_t o = m.readMap[slot];
        if (o != 0xffffffff && physical >= (int)o && physical < (int)o + SLOT_SIZE) {
            const int a = slot * SLOT_SIZE + (physical - (int)o);
            if (m.physicalAddress(a, false) == physical) return a;
        }
    }
    for (int slot = 0; slot < 8; slot++) {            // write-only: under a ROM
        const uint32_t o = m.writeMap[slot];
        if (o != 0xffffffff && physical >= (int)o && physical < (int)o + SLOT_SIZE) return slot * SLOT_SIZE + (physical - (int)o);
    }
    return -1;
}

// ============================================================== Memory map
void GuiShell::memoryMapContent() {
    if (!host.booted() || !host.emu || !host.emu->memory) { ImGui::TextDisabled("No machine booted."); return; }
    GXMemory& m = *host.emu->memory;
    if (m.accessMap.size() != m.ram.size()) m.accessMap.resize(m.ram.size(), 0);
    const int chunks = std::max(1, (int)(m.ram.size() / MAP_CHUNK));
    memMapChunk = std::clamp(memMapChunk, 0, chunks - 1);

    // Which 64K blocks the CPU has paged in, and where.
    auto pagedIn = [&](int chunk) {
        std::string at;
        for (int q = 0; q < 4; q++) {
            const int cpu = cpuAddressOf(m, chunk * MAP_CHUNK + q * 0x4000);
            if (cpu < 0) continue;
            char b[12]; std::snprintf(b, sizeof b, "%s&%04X", at.empty() ? "" : " ", cpu & 0xc000);
            at += b;
        }
        return at;
    };
    auto blockName = [&](int chunk) {
        const std::string at = pagedIn(chunk);
        return chunkLabel(m, chunk) + (at.empty() ? "" : "   (CPU " + at + ")");
    };
    // one 64K at a time: as many as the RAM fitted has
    ImGui::SetNextItemWidth(std::min(320.0f, ImGui::GetContentRegionAvail().x * 0.45f));
    if (ImGui::BeginCombo("##block", blockName(memMapChunk).c_str())) {
        for (int ch = 0; ch < chunks; ch++) {
            ImGui::PushID(ch);
            if (ImGui::Selectable(blockName(ch).c_str(), ch == memMapChunk)) memMapChunk = ch;
            if (ch == memMapChunk) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d K of RAM: %d blocks of 64K", (int)(m.ram.size() / 1024), chunks);
    after().checkbox("Record", &memMapRecord);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tag every memory access the Z80 makes (only while this window is open).");
    if (after().button("Clear")) m.clearAccessMap();
    after().checkbox("Live", &memMapLive);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show only what was touched since the window last drew: the activity of each moment.");
    after().checkbox("Mark written code", &memMapWrittenCode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Code that was also written: self-modifying code, or code loaded while recording.");

    // the legend, with how many bytes of this 64K are in each class
    int counts[MAP_CLASSES] = {};
    const size_t base = (size_t)memMapChunk * MAP_CHUNK;
    for (int i = 0; i < MAP_CHUNK && base + i < m.ram.size(); i++) counts[mapClass(m.accessMap[base + i], memMapWrittenCode)]++;
    FlowRow legend;                            // swatch + name, kept together; the legend wraps
    legend.spacing = 14.0f;
    for (int c = 0; c < MAP_CLASSES; c++) {
        if (c == MAP_WRITTEN_CODE && !memMapWrittenCode) continue;
        char name[64];
        std::snprintf(name, sizeof name, "%s %d", MAP_LEGEND[c].name, counts[c]);
        const float s = ImGui::GetTextLineHeight();
        legend.place(s + ImGui::CalcTextSize(name).x);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + s - 4, p.y + s - 2), legendColour(c));
        ImGui::Dummy(ImVec2(s - 2, s));
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled("%s", name);
    }
    memMapPixels.assign((size_t)256 * 256, 0);
    for (int i = 0; i < MAP_CHUNK; i++) {
        const size_t at = base + i;
        memMapPixels[(size_t)i] = at < m.ram.size() ? mapColour(mapClass(m.accessMap[at], memMapWrittenCode), m.ram[at]) : 0xff000000u;
    }
    unsigned detail = uploadTexture ? uploadTexture(4, memMapPixels.data(), 256, 256) : 0;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float labelW = ImGui::CalcTextSize("&C000-&FFFF ").x;
    const float side = std::max(64.0f, std::min(avail.x - labelW - 8, avail.y - 4));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ix = p.x + labelW;
    if (detail) dl->AddImage((ImTextureID)(intptr_t)detail, ImVec2(ix, p.y), ImVec2(ix + side, p.y + side));
    // the 16K quarters, with where the CPU sees each (if it does)
    for (int q = 0; q < 4; q++) {
        const float y = p.y + q * side / 4;
        if (q) dl->AddLine(ImVec2(ix, y), ImVec2(ix + side, y), IM_COL32(255, 255, 255, 60));
        const int cpu = cpuAddressOf(m, (int)base + q * 0x4000);
        char t[40];
        std::snprintf(t, sizeof t, "+&%04X", q * 0x4000);
        dl->AddText(ImVec2(p.x, y + 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), t);
        if (cpu >= 0) {
            std::snprintf(t, sizeof t, "CPU &%04X", cpu & 0xc000);
            dl->AddText(ImVec2(p.x, y + 2 + ImGui::GetTextLineHeight()), ImGui::GetColorU32(kAccent), t);
        }
    }
    // the CPC's screen: the CRTC's R12/R13 page in the base 64K (32K with R12 bits 3-2 = 3)
    if (memMapChunk == 0 && host.emu->crtc) {
        const int r12 = host.emu->crtc->registers[12];
        const int page = (r12 >> 4) & 3, size = ((r12 >> 2) & 3) == 3 ? 2 : 1;
        for (int k = 0; k < size; k++) {
            const int q = (page + k) & 3;
            dl->AddRect(ImVec2(ix + 1, p.y + q * side / 4 + 1), ImVec2(ix + side - 1, p.y + (q + 1) * side / 4 - 1), IM_COL32(255, 255, 255, 150), 0, 0, 1.5f);
        }
        const float y = p.y + page * side / 4 + 2 * ImGui::GetTextLineHeight() + 2;
        dl->AddText(ImVec2(p.x, y), IM_COL32(255, 255, 255, 170), "screen");
    }
    ImGui::SetCursorScreenPos(ImVec2(ix, p.y));
    ImGui::InvisibleButton("##detail", ImVec2(side, side));
    if (ImGui::IsItemHovered()) {
        const ImVec2 mp = ImGui::GetMousePos();
        const int bx = std::clamp((int)((mp.x - ix) * 256 / side), 0, 255), by = std::clamp((int)((mp.y - p.y) * 256 / side), 0, 255);
        const int offset = by * 256 + bx;
        const int phys = (int)base + offset;
        if (phys < (int)m.ram.size()) {
            const int f = m.accessMap[(size_t)phys], cpu = cpuAddressOf(m, phys);
            std::string kinds;
            if (f & Z80Memory::ACCESS_OPCODE) kinds += " opcode";
            if (f & Z80Memory::ACCESS_OPERAND) kinds += " operand";
            if (f & Z80Memory::ACCESS_READ) kinds += " read";
            if (f & Z80Memory::ACCESS_WRITE) kinds += " written";
            char cpuText[24] = "not paged in";
            if (cpu >= 0) std::snprintf(cpuText, sizeof cpuText, "CPU &%04X", cpu);
            auto lbl = cpu >= 0 ? debugger.labelAt.find(cpu) : debugger.labelAt.end();
            ImGui::SetTooltip("physical &%05X  (+&%04X)\n%s%s%s\n&%02X  %d\n%s\nclick: Memory, double-click: Disassembly", phys, offset, cpuText,
                              lbl != debugger.labelAt.end() ? "  " : "", lbl != debugger.labelAt.end() ? lbl->second.c_str() : "",
                              m.ram[(size_t)phys], m.ram[(size_t)phys], kinds.empty() ? "untouched" : kinds.c_str() + 1);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && cpu >= 0) {
                showInDisassembly(cpu);
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (cpu >= 0) showInMemory(cpu);
                else {
                    panelOpen("Memory") = true;
                    memView = 1; memFollow = false;
                    memSelected = phys; memTop = phys & ~15; memScrollTo = true;
                    ImGui::SetWindowFocus("Memory");
                }
            }
        }
    }
    // "Live": what the next refresh shows is what happened after this one.
    if (memMapLive && !host.paused) m.clearAccessMap();
}

// ============================================================== GFX9000 internals
// 12.2 (p.79-87): each register's fields.
static const char* const V9990_REG_NAMES[64] = {
    "VRAM write address A7-0", "VRAM write address A15-8", "AII | write address A18-16",
    "VRAM read address A7-0", "VRAM read address A15-8", "AII | read address A18-16",
    "Screen mode: DSPM DCKM XIMM CLRM", "Screen mode: C25M SM1 SM PAL EO IL HSCN",
    "Control: DISP SPD YSE VWTE VWM DMAE VSL", "Interrupt enable: IECE IEH IEV",
    "Interrupt line IL7-0", "IEHM | IL9-8", "Interrupt X: IX3-0",
    "Palette control: PLTM YAE PLTAIH PLTO", "Palette pointer: PLTA PLTP", "Back drop colour",
    "Display adjust: ADJV ADJH", "Scroll A: SCAY7-0", "Scroll A: R512 R256 SCAY12-8",
    "Scroll A: SCAX2-0", "Scroll A: SCAX10-3", "Scroll B: SCBY7-0", "Scroll B: SCBY8",
    "Scroll B: SCBX2-0", "Scroll B: SCBX8-3", "Sprite pattern generator base",
    "LCD control: VRI PNSL PLVO PDUAL PNEN", "Priority: PRY PRX", "Sprite palette offset",
    nullptr, nullptr, nullptr,
    "SX7-0 / SA7-0 / KA7-0", "SX10-8", "SY7-0 / SA15-8 / KA15-8", "SY11-8 / SA18-16 / KA17-16",
    "DX7-0 / DA7-0", "DX10-8", "DY7-0 / DA15-8", "DY11-8 / DA18-16",
    "NX7-0 / NA7-0 / MJ7-0", "NX10-8 / MJ11-8", "NY7-0 / NA15-8 / MI7-0", "NY11-8 / NA18-16 / MI11-8",
    "Argument: DIY DIX NEQ MAJ", "Logical op: TP LOP", "Write mask WM7-0", "Write mask WM15-8",
    "Font colour FC7-0", "Font colour FC15-8", "Back colour BC7-0", "Back colour BC15-8",
    "Op code: OP AYM AYE AXM AXE", "Border X BX7-0 (read)", "Border X BX10-8 (read)",
};
// 11.3 (p.54)
static const char* const V9990_COMMANDS[16] = {
    "STOP", "LMMC", "LMMV", "LMCM", "LMMM", "CMMC", "CMMK", "CMMM",
    "BMXL", "BMLX", "BMLL", "LINE", "SRCH", "POINT", "PSET", "ADVN",
};

// The GFX9000 window's tabs after its picture (inside its tab bar).
void GuiShell::gfxInternalsTabs() {
    V9990* v = host.emu ? host.emu->v9990 : nullptr;
    if (!v || !host.v9990Enabled) return;
    const auto& r = v->registers;
    auto word = [&](int i) { return r[i] | r[i + 1] << 8; };

    auto tabFlags = [this](int tab) { return gfxInternalsTab == tab ? ImGuiTabItemFlags_SetSelected : 0; };
    {
        // ------------------------------------------------------ state
        if (ImGui::BeginTabItem("State", nullptr, tabFlags(0))) {
            const int st = v->readStatus();
            if (ImGui::BeginTable("##v99state", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                sectionHeading("DISPLAY");
                if (beginFacts("##v99d", 120)) {
                    fact("Mode", "%s%s", v9990ModeName(v->mode()), v->systemReset ? "  (SRS: reset held)" : "");
                    fact("Image space", "%d x %d, %d bit/dot", v->imageWidth(), v->imageHeight(), v->bitsPerDot());
                    fact("VRAM", "%dK (R#8 VSL %d)", v->vramBytes() / 1024, r[8] & 3);
                    fact("Display", "%s, sprites %s", (r[8] & 0x80) ? "on (DISP)" : "off: back drop", (r[8] & 0x40) ? "off (SPD)" : "on");
                    fact("Clock", "%s (P#7 MCS)", v->mcs ? "MCKIN 14.318 MHz" : "XTAL 21.477 MHz");
                    fact("Raster", "line %d of %d, field %d", v->line, v->raster.totalLines, v->field);
                    fact("Scroll A", "X %d  Y %d", (r[19] & 7) | r[20] << 3, r[17] | (r[18] & 0x1f) << 8);
                    fact("Scroll B", "X %d  Y %d", (r[23] & 7) | (r[24] & 0x3f) << 3, r[21] | (r[22] & 1) << 8);
                    fact("Back drop", "colour %d", r[15] & 63);
                    fact("Palette", "PLTM %d, pointer %d.%d", r[13] >> 6, r[14] >> 2, r[14] & 3);
                    fact("P#F output", "&%02X%s", v->outputControl, v->outputControlWritten ? "" : " (not set)");
                    endFacts();
                }
                ImGui::TableNextColumn();
                sectionHeading("PORTS");
                if (beginFacts("##v99p", 120)) {
                    fact("P#4 select", "R#%d%s%s", v->regSelect & 63, (v->regSelect & 0x80) ? " WII" : "", (v->regSelect & 0x40) ? " RII" : "");
                    fact("P#5 status", "&%02X  %s%s%s%s%s%s%s", st, (st & 0x80) ? "TR " : "", (st & 0x40) ? "VR " : "", (st & 0x20) ? "HR " : "",
                         (st & 0x10) ? "BD " : "", (st & 0x04) ? "MCS " : "", (st & 0x02) ? "EO " : "", (st & 0x01) ? "CE" : "");
                    fact("P#6 flags", "%s%s%s  (R#9 enables %s%s%s)", (v->pendingIrq & 4) ? "CE " : "", (v->pendingIrq & 2) ? "HI " : "",
                         (v->pendingIrq & 1) ? "VI" : "", (r[9] & 4) ? "CE " : "", (r[9] & 2) ? "HI " : "", (r[9] & 1) ? "VI" : "");
                    fact("/INT", "%s", v->intAsserted() ? "LOW (asserted)" : "high");
                    fact("Line IRQ", "%s, X %d", (r[11] & 0x80) ? "every line" : std::to_string(r[10] | (r[11] & 3) << 8).c_str(), r[12] & 15);
                    fact("VRAM write", "&%05X%s", v->vramWriteAddress(), (r[2] & 0x80) ? " (AII: held)" : " +1");
                    fact("VRAM read", "&%05X%s", v->vramReadAddress(), (r[5] & 0x80) ? " (AII: held)" : " +1");
                    endFacts();
                }
                ImGui::EndTable();
            }
            sectionHeading("COMMAND");
            if (beginFacts("##v99c", 120)) {
                fact("Op code", "%s  (R#52 &%02X)%s", V9990_COMMANDS[r[52] >> 4], r[52], (v->commandStatus & 1) ? "  executing (CE)" : "");
                fact("Source", "X %d  Y %d   linear &%05X", word(32) & 0x7ff, word(34) & 0xfff, (r[32] | (word(34) & 0x7ff) << 8) & (V9990_VRAM_SIZE - 1));
                fact("Destination", "X %d  Y %d   linear &%05X", word(36) & 0x7ff, word(38) & 0xfff, (r[36] | (word(38) & 0x7ff) << 8) & (V9990_VRAM_SIZE - 1));
                fact("Size", "NX %d  NY %d", word(40) & 0x7ff, word(42) & 0xfff);
                fact("Pointer", "X %d  Y %d  (PSET / ADVN / LINE)", v->pointerX, v->pointerY);
                fact("ARG / LOP", "&%02X%s%s%s%s / &%02X%s", r[44], (r[44] & 8) ? " DIY" : "", (r[44] & 4) ? " DIX" : "", (r[44] & 2) ? " NEQ" : "",
                     (r[44] & 1) ? " MAJ" : "", r[45], (r[45] & 0x10) ? " TP" : "");
                fact("Write mask", "&%04X", word(46));
                fact("Colours", "font &%04X  back &%04X", word(48), word(50));
                fact("Engine", "%s%s", V9990::commandName(v->engine.op),
                     v->engine.wantsByte ? ", waiting for the CPU's byte (TR)" : !v->engine.out.empty() ? ", a byte for the CPU (TR)" : "");
                endFacts();
            }
            ImGui::EndTabItem();
        }
        // ------------------------------------------------------ registers
        if (ImGui::BeginTabItem("Registers", nullptr, tabFlags(1))) {
            if (ImGui::BeginTable("##v99regs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("R#");
                ImGui::TableSetupColumn("hex");
                ImGui::TableSetupColumn("bits");
                ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                for (int i = 0; i < 55; i++) {
                    if (!V9990_REG_NAMES[i]) continue;
                    const int value = i == 53 ? v->borderX & 0xff : i == 54 ? (v->borderX >> 8) & 7 : r[i];
                    char bits[9];
                    for (int b = 0; b < 8; b++) bits[b] = (value >> (7 - b)) & 1 ? '1' : '0';
                    bits[8] = 0;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("%2d", i);
                    ImGui::TableNextColumn(); ImGui::Text("&%02X", value);
                    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", bits);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(V9990_REG_NAMES[i]);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        // ------------------------------------------------------ palette
        if (ImGui::BeginTabItem("Palette", nullptr, tabFlags(2))) {
            const float s = std::clamp((ImGui::GetContentRegionAvail().x - 16 * 4) / 16, 14.0f, 40.0f);
            for (int i = 0; i < 64; i++) {
                if (i % 16) ImGui::SameLine(0, 4);
                const uint32_t c = v->paletteEntry(i);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + s, p.y + s), c);
                if (i == (r[15] & 63)) ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + s, p.y + s), ImGui::GetColorU32(kAccent), 0, 0, 2.0f);
                ImGui::PushID(i);
                ImGui::InvisibleButton("##pal", ImVec2(s, s));
                if (ImGui::IsItemHovered()) {
                    const int at = i * 4;
                    ImGui::SetTooltip("colour %d\nR %d  G %d  B %d  (5 bits)%s%s", i, v->palette[at] & 31, v->palette[at + 1] & 31, v->palette[at + 2] & 31,
                                      (v->palette[at] & 0x80) ? "\nYS: transparent" : "", i == (r[15] & 63) ? "\nback drop" : "");
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        // ------------------------------------------------------ VRAM as a picture
        if (ImGui::BeginTabItem("VRAM image", nullptr, tabFlags(3))) {
            const int w = v->imageViewWidth(), h = v->imageViewHeight();
            const bool layers = v->mode() == V9990Mode::P1;
            ImGui::SetNextItemWidth(120);
            ImGui::SliderInt("Zoom", &gfxVramZoom, 1, 4);
            {
                char l[128];
                if (layers) std::snprintf(l, sizeof l, "layer A | layer B, 256 x %d each, P1, as the display colours it", h);
                else std::snprintf(l, sizeof l, "%d x %d image space, %s, as the display colours it", w, h, v9990ModeName(v->mode()));
                after().textDisabled(l);
            }
            ImGui::BeginChild("##v99img", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            const float z = (float)gfxVramZoom;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float scrollY = ImGui::GetScrollY();
            const float viewH = ImGui::GetWindowHeight();
            ImGui::Dummy(ImVec2(w * z, h * z));            // the whole image's size, for the scroll bars
            // only the rows in view are drawn
            const int first = std::clamp((int)(scrollY / z), 0, std::max(0, h - 1));
            const int count = std::clamp((int)(viewH / z) + 2, 1, std::max(1, h - first));
            gfxVramPixels.resize((size_t)w * count);
            for (int y = 0; y < count; y++) v->imageViewLine(first + y, &gfxVramPixels[(size_t)y * w]);
            if (uploadTexture) {
                const unsigned t = uploadTexture(5, gfxVramPixels.data(), w, count);
                const ImVec2 p0(at.x, at.y + first * z);
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)(intptr_t)t, p0, ImVec2(p0.x + w * z, p0.y + count * z));
                if (layers)                                 // the seam between layer A and B
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x + 256 * z, p0.y), ImVec2(at.x + 256 * z, p0.y + count * z),
                                                        ImGui::GetColorU32(ImGuiCol_Separator));
            }
            if (ImGui::IsWindowHovered()) {
                const ImVec2 mp = ImGui::GetMousePos();
                const int c = (int)((mp.x - at.x) / z), y = (int)((mp.y - at.y) / z);
                if (c >= 0 && c < w && y >= 0 && y < h) {
                    const int x = v->imageViewX(c);
                    if (layers) ImGui::SetTooltip("layer %c  X %d  Y %d\ndot %d  (VRAM &%05X)", c >= 256 ? 'B' : 'A', x & 255, y,
                                                  v->getDot(x, y), v->dotAddress(x, y));
                    else ImGui::SetTooltip("X %d  Y %d\ndot %d  (VRAM &%05X)", x, y, v->getDot(x, y), v->dotAddress(x, y));
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ------------------------------------------------------ VRAM bytes
        if (ImGui::BeginTabItem("VRAM bytes", nullptr, tabFlags(4))) {
            ImGui::SetNextItemWidth(120);
            bool jump = ImGui::InputTextWithHint("##v99goto", "address (hex)", gfxVramGoto, sizeof gfxVramGoto,
                                                 ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
            {
                char l[128];
                std::snprintf(l, sizeof l, "addresses as the CPU gives them to P#0 (%s mapping); write &%05X, read &%05X",
                              v9990ModeName(v->mode()), v->vramWriteAddress(), v->vramReadAddress());
                after().textDisabled(l);
            }
            ImGui::BeginChild("##v99hex", ImVec2(0, 0), ImGuiChildFlags_Borders);
            const float rowH = ImGui::GetTextLineHeightWithSpacing();
            if (jump && gfxVramGoto[0]) ImGui::SetScrollY((float)((std::strtol(gfxVramGoto, nullptr, 16) & (V9990_VRAM_SIZE - 1)) / 16) * rowH);
            const int wa = v->vramWriteAddress(), ra = v->vramReadAddress();
            ImGuiListClipper clip;
            clip.Begin(V9990_VRAM_SIZE / 16, rowH);
            while (clip.Step()) {
                for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
                    const int b = row * 16;
                    ImGui::TextDisabled("%05X", b);
                    char text[17];
                    for (int i = 0; i < 16; i++) {
                        const int value = v->vram[(size_t)v->vramIndex(b + i)];
                        ImGui::SameLine(0, i == 8 ? 12.0f : 6.0f);
                        const bool mark = b + i == wa || b + i == ra;
                        if (mark) ImGui::TextColored(b + i == wa ? kPcColour : ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%02X", value);
                        else if (value == 0) ImGui::TextDisabled("00");
                        else ImGui::Text("%02X", value);
                        text[i] = value >= 32 && value < 127 ? (char)value : '.';
                    }
                    text[16] = 0;
                    ImGui::SameLine(0, 12.0f);
                    ImGui::TextDisabled("%s", text);
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }
    gfxInternalsTab = -1;
}

} // namespace cpcse
