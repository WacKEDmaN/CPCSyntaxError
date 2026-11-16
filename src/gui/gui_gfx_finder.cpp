// CPCSyntaxError GUI — the graphics finder.
//
// Memory shown as CPC graphics, to find a program's sprites, fonts and screens: the bytes
// decoded as the GATE ARRAY decodes them in mode 0, 1 or 2, or as Plus sprite data (a pixel
// a nibble), laid out a tile at a time (a sprite stored line after line) or as the CPC's
// screen (eight blocks of &800). The width in bytes and the height of a tile are set until
// the picture lines up; the inks are the machine's own, the Plus sprite inks, or greys.
// A tile clicked can be saved as a PNG or copied as DB lines for an assembler.
#include "gui_shell.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/asic.h"
#include "core/crtc.h"
#include "core/emulator.h"
#include "core/gate_array.h"
#include "core/memory.h"
#include "core/png.h"
#include "png_decode.h"

namespace cpcse {

namespace {
const char* kFormats[] = { "Mode 0 (2 pixels a byte, 16 inks)", "Mode 1 (4 pixels a byte, 4 inks)", "Mode 2 (8 pixels a byte, 2 inks)",
                           "Nibbles (2 pixels a byte, high first)", "Plus sprite (1 pixel a byte, low nibble)" };
const int kPixelsPerByte[] = { 2, 4, 8, 2, 1 };
const char* kSources[] = { "CPU memory (as the Z80 sees it)", "RAM (every bank)", "Plus sprite RAM (ASIC)" };
const char* kPalettes[] = { "The machine's inks", "Plus sprite inks", "Greys" };

// the pen of pixel i of a byte, as the GATE ARRAY reads it (ACCC 9.2.2)
int pixelPen(int format, int v, int i) {
    switch (format) {
        case 0: {
            const int s = i ? 6 : 7;     // pixel 0: bits 7 5 3 1 (as pen bits 0 2 1 3); pixel 1: 6 4 2 0
            return (v >> s & 1) | (v >> (s - 4) & 1) << 1 | (v >> (s - 2) & 1) << 2 | (v >> (s - 6) & 1) << 3;
        }
        case 1: return (v >> (7 - i) & 1) | (v >> (3 - i) & 1) << 1;
        case 2: return v >> (7 - i) & 1;
        case 3: return i ? v & 15 : v >> 4 & 15;
        default: return v & 15;
    }
}

// the byte with pixel i set to `pen`: the inverse of pixelPen, the other pixels kept
int withPixelPen(int format, int v, int i, int pen) {
    switch (format) {
        case 0: {
            const int s = i ? 6 : 7;
            const int bits[4] = { s, s - 4, s - 2, s - 6 };   // pen bits 0 1 2 3
            for (int b = 0; b < 4; b++) v = (v & ~(1 << bits[b])) | ((pen >> b & 1) << bits[b]);
            return v & 0xff;
        }
        case 1: {
            v = (v & ~(1 << (7 - i))) | ((pen & 1) << (7 - i));
            return ((v & ~(1 << (3 - i))) | ((pen >> 1 & 1) << (3 - i))) & 0xff;
        }
        case 2: return ((v & ~(1 << (7 - i))) | ((pen & 1) << (7 - i))) & 0xff;
        case 3: return i ? (v & 0xf0) | (pen & 15) : (v & 0x0f) | (pen & 15) << 4;
        default: return (v & 0xf0) | (pen & 15);
    }
}

// The firmware's 27 inks (INK n), as hardware colours (ACCC 9.1, the firmware's table).
const int kFirmwareInks[27] = { 0x14, 0x04, 0x15, 0x1c, 0x18, 0x1d, 0x0c, 0x05, 0x0d, 0x16, 0x06, 0x17, 0x1e, 0x00,
                                0x1f, 0x0e, 0x07, 0x0f, 0x12, 0x02, 0x13, 0x1a, 0x19, 0x1b, 0x0a, 0x03, 0x0b };

// The pixels of a line, a box or a filled box from (x0, y0) to (x1, y1), in picture pixels.
std::vector<std::pair<int, int>> shapePoints(int tool, int x0, int y0, int x1, int y1) {
    std::vector<std::pair<int, int>> out;
    if (tool == 1) {
        const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (int guard = 0; guard < 100000; guard++) {
            out.push_back({ x0, y0 });
            if (x0 == x1 && y0 == y1) break;
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
        return out;
    }
    const int ax = std::min(x0, x1), bx = std::max(x0, x1), ay = std::min(y0, y1), by = std::max(y0, y1);
    if ((long long)(bx - ax + 1) * (by - ay + 1) > 4000000) return out;
    for (int y = ay; y <= by; y++)
        for (int x = ax; x <= bx; x++)
            if (tool == 3 || y == ay || y == by || x == ax || x == bx) out.push_back({ x, y });
    return out;
}
} // namespace

int GuiShell::gfxPenCount() const { return gfx.format == 1 ? 4 : gfx.format == 2 ? 2 : 16; }

int GuiShell::gfxLimit() const {
    const GX4000* e = host.emu;
    return gfx.source == 0 ? 0x10000 : gfx.source == 1 ? (e && e->memory ? std::max(1, (int)e->memory->ram.size()) : 1) : 0x1000;
}

// A byte of the stroke: its first value before and its last value after are kept for undo.
void GuiShell::gfxStrokeByte(int at, int nv) {
    nv &= 0xff;
    const int v = gfxRead(at);
    if (nv == v) return;
    const long long key = (long long)gfx.source << 32 | (unsigned)at;
    auto found = gfxStrokeIndex.find(key);
    if (found == gfxStrokeIndex.end()) {
        gfxStrokeIndex[key] = gfxStroke.edits.size();
        gfxStroke.edits.push_back({ gfx.source, at, (uint8_t)v, (uint8_t)nv });
    } else gfxStroke.edits[found->second].after = (uint8_t)nv;
    gfxWrite(at, nv);
}

void GuiShell::gfxEndStroke() {
    if (!gfxStroke.edits.empty()) {
        gfxUndo.push_back(std::move(gfxStroke));
        if (gfxUndo.size() > 200) gfxUndo.erase(gfxUndo.begin());
        gfxRedo.clear();
    }
    gfxStroke = GfxStroke{};
    gfxStrokeIndex.clear();
}

// The selected tile's bytes, line after line, into the finder's own clipboard.
bool GuiShell::gfxCopyTile() {
    if (gfx.selected < 0 || !host.booted()) return false;
    const int limit = gfxLimit();
    gfxClip.clear();
    for (int y = 0; y < gfx.height; y++)
        for (int x = 0; x < gfx.widthBytes; x++) gfxClip.push_back((uint8_t)gfxRead(gfxAddress(gfx.selected, x, y) % limit));
    gfxClipW = gfx.widthBytes;
    gfxClipH = gfx.height;
    return true;
}

// The tile copied into the selected tile (as much of it as fits): one undo step.
bool GuiShell::gfxPasteTile() {
    if (gfx.selected < 0 || gfxClip.empty() || !host.booted()) return false;
    const int limit = gfxLimit();
    for (int y = 0; y < std::min(gfx.height, gfxClipH); y++)
        for (int x = 0; x < std::min(gfx.widthBytes, gfxClipW); x++) {
            const int a = gfxAddress(gfx.selected, x, y);
            if (gfx.source != 0 && a >= limit) continue;
            gfxStrokeByte(a % limit, gfxClip[(size_t)y * gfxClipW + x]);
        }
    gfxEndStroke();
    return true;
}

// A PNG into the selected tile: each pixel the pen whose ink is nearest (a clear pixel pen 0),
// as much of the picture as the tile holds. One undo step.
std::string GuiShell::gfxImportPng(const std::vector<uint8_t>& file) {
    if (gfx.selected < 0) return "Select a tile first (Draw off, click a tile).";
    if (!host.booted()) return "No machine.";
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::string why;
    if (!decodePng(file, w, h, rgba, why)) return "Not imported: " + why;
    const int ppb = kPixelsPerByte[gfx.format];
    const int tileW = gfx.widthBytes * ppb, tileH = gfx.height, limit = gfxLimit();
    const int pens = gfxPenCount();
    uint32_t inks[16];
    for (int p = 0; p < pens; p++) inks[p] = gfxInk(p);
    std::vector<int> bytes;                                          // the tile's bytes as they become
    const int cw = std::min(w, tileW), ch = std::min(h, tileH);
    for (int y = 0; y < ch; y++)
        for (int bx = 0; bx < (cw + ppb - 1) / ppb; bx++) {
            const int a = gfxAddress(gfx.selected, bx, y);
            if (gfx.source != 0 && a >= limit) continue;
            int v = gfxRead(a % limit);
            for (int i = 0; i < ppb && bx * ppb + i < cw; i++) {
                const uint8_t* s = &rgba[((size_t)y * w + bx * ppb + i) * 4];
                int best = 0;
                if (s[3] >= 128) {
                    long bestD = -1;
                    for (int p = 0; p < pens; p++) {
                        const int dr = (int)(inks[p] & 0xff) - s[0], dg = (int)(inks[p] >> 8 & 0xff) - s[1], db = (int)(inks[p] >> 16 & 0xff) - s[2];
                        const long d = (long)dr * dr * 3 + (long)dg * dg * 4 + (long)db * db * 2;
                        if (bestD < 0 || d < bestD) { bestD = d; best = p; }
                    }
                }
                v = withPixelPen(gfx.format, v, i, best);
            }
            gfxStrokeByte(a % limit, v);
        }
    const size_t changed = gfxStroke.edits.size();
    gfxEndStroke();
    return "Imported " + std::to_string(cw) + "x" + std::to_string(ch) + " pixels of " + std::to_string(w) + "x" + std::to_string(h) +
           " into the tile (" + std::to_string(changed) + " bytes changed; Ctrl+Z takes it back)";
}

// A pen's ink: on a CPC a hardware colour (as OUT &7F00 does it), on a Plus &GRB in the ASIC;
// the Plus sprite inks (palette 1) are the ASIC's pens 16 on.
void GuiShell::gfxSetInk(int pen, int value) {
    GX4000* e = host.emu;
    if (!e || pen < 0 || pen > 15) return;
    if (gfx.palette == 1) { if (e->asic && pen > 0) e->asic->setPlusPalette(16 + pen, value & 0xfff); }
    else if (e->plusHardware && e->asic) e->asic->setPlusPalette(pen, value & 0xfff);
    else {
        e->gateArray->gaPalette[(size_t)pen] = (uint8_t)(value & 0x1f);
        if (e->asic) e->asic->setGateArrayInk(pen, value & 0x1f);
    }
    host.timelineEdited();
}

// The ink popup (a right-click on a pen): the 27 colours of a CPC, or a Plus's 4096.
void GuiShell::gfxInkPopup() {
    if (!ImGui::BeginPopup("##gfxink")) return;
    GX4000* e = host.emu;
    const int pen = gfxInkPen;
    if (!e || pen < 0) { ImGui::EndPopup(); return; }
    const bool plus = gfx.palette == 1 || (e->plusHardware && e->asic);
    if (gfx.palette == 2) ImGui::TextDisabled("Greys: no ink to change (pick the machine's inks).");
    else if (plus && e->asic) {
        ImGui::Text("Pen %d (the ASIC's palette %d)", pen, gfx.palette == 1 ? 16 + pen : pen);
        const int index = gfx.palette == 1 ? 16 + pen : pen;
        int v = e->asic->palette[(size_t)index] & 0xfff;
        int r = v >> 4 & 15, g = v >> 8 & 15, b = v & 15;
        bool changed = ImGui::SliderInt("Red", &r, 0, 15);
        changed |= ImGui::SliderInt("Green", &g, 0, 15);
        changed |= ImGui::SliderInt("Blue", &b, 0, 15);
        if (changed && !(gfx.palette == 1 && pen == 0)) gfxSetInk(pen, g << 8 | r << 4 | b);
        if (gfx.palette == 1 && pen == 0) ImGui::TextDisabled("Sprite pen 0 is clear: it has no ink.");
    } else {
        const int hwNow = e->gateArray->gaPalette[(size_t)pen] & 0x1f;
        int inkNow = -1;
        for (int k = 0; k < 27; k++) if (kFirmwareInks[k] == hwNow) inkNow = k;
        if (inkNow >= 0) ImGui::Text("Pen %d: INK %d (hardware colour &%02X)", pen, inkNow, hwNow);
        else ImGui::Text("Pen %d: hardware colour &%02X (a copy of another)", pen, hwNow);
        const float size = ImGui::GetFrameHeight();
        for (int k = 0; k < 27; k++) {
            const int hw = kFirmwareInks[k];
            const int rgb = GateArray::rgbForHardwareColour(hw);
            ImGui::PushID(k);
            if (k % 9) ImGui::SameLine();
            const bool now = (e->gateArray->gaPalette[(size_t)pen] & 0x1f) == hw;
            if (ImGui::ColorButton("##ink", ImVec4((rgb >> 16 & 0xff) / 255.0f, (rgb >> 8 & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(size, size)))
                gfxSetInk(pen, hw);
            if (now) ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(kAccent), 0, 0, 2.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("INK %d: hardware colour &%02X (#%06X)", k, hw, (unsigned)rgb);
            ImGui::PopID();
        }
        ImGui::TextDisabled("As OUT &7F00 does: the program can set it again.");
    }
    ImGui::EndPopup();
}

// Into the memory the finder shows: the Z80's view (a write goes to RAM, as the Z80's does,
// whatever ROM is paged in for reading), RAM by its own address, or the Plus sprite RAM.
void GuiShell::gfxWrite(int address, int value, int source) {
    GX4000* e = host.emu;
    if (!e || !e->memory) return;
    value &= 0xff;
    switch (source >= 0 ? source : gfx.source) {
        case 0: e->memory->write(address & 0xffff, value); break;
        case 1: if (!e->memory->ram.empty()) e->memory->ram[(size_t)address % e->memory->ram.size()] = (uint8_t)value; break;
        default:
            if (e->memory->asicRam.size() > 0xfff && e->asic) {
                e->memory->asicRam[(size_t)(address & 0xfff)] = (uint8_t)value;
                e->asic->writeAsicRam(address & 0xfff, value);
            }
            break;
    }
    host.timelineEdited();
}

// Undo and redo of a stroke: each byte it changed back to its value before (in the order
// opposite to the stroke's), or again to its value after.
void GuiShell::gfxUndoStroke() {
    if (gfxUndo.empty()) return;
    GfxStroke s = std::move(gfxUndo.back());
    gfxUndo.pop_back();
    for (auto it = s.edits.rbegin(); it != s.edits.rend(); ++it) gfxWrite(it->address, it->before, it->source);
    gfxRedo.push_back(std::move(s));
}
void GuiShell::gfxRedoStroke() {
    if (gfxRedo.empty()) return;
    GfxStroke s = std::move(gfxRedo.back());
    gfxRedo.pop_back();
    for (const GfxEdit& ed : s.edits) gfxWrite(ed.address, ed.after, ed.source);
    gfxUndo.push_back(std::move(s));
}

// The pens of the format, numbered, each in the colour the picture draws it: a click picks it.
void GuiShell::gfxPens() {
    const int n = gfxPenCount();
    const float size = ImGui::GetFrameHeight();
    FlowRow row;
    row.textDisabled("Pens");
    int inkClicked = -1;
    for (int p = 0; p < n; p++) {
        row.item(size);
        const uint32_t ink = gfxInk(p);                    // 0xAABBGGRR
        const ImVec4 colour((ink & 0xff) / 255.0f, (ink >> 8 & 0xff) / 255.0f, (ink >> 16 & 0xff) / 255.0f, 1.0f);
        ImGui::PushID(p);
        if (ImGui::ColorButton("##pen", colour, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(size, size))) gfx.pen = p;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        char num[4];
        std::snprintf(num, sizeof num, "%d", p);
        const ImVec2 ts = ImGui::CalcTextSize(num);
        const float light = colour.x * 0.3f + colour.y * 0.59f + colour.z * 0.11f;   // the number readable on its ink
        dl->AddText(ImVec2(a.x + (size - ts.x) * 0.5f, a.y + (size - ts.y) * 0.5f), light > 0.5f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255), num);
        if (gfx.pen == p) dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), ImGui::GetColorU32(kAccent), 0, 0, 2.0f);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) inkClicked = p;
        if (ImGui::IsItemHovered()) {
            const GX4000* e = host.emu;
            if (gfx.palette == 0 && e && !e->plusHardware)
                ImGui::SetTooltip("Pen %d: hardware colour &%02X (#%06X)\nClick: draw with it   Right-click: change its ink", p, e->gateArray->gaPalette[(size_t)p] & 0x1f,
                                  (unsigned)((ink & 0xff) << 16 | (ink & 0xff00) | (ink >> 16 & 0xff)));
            else ImGui::SetTooltip("Pen %d (#%06X)\nClick: draw with it   Right-click: change its ink", p,(unsigned)((ink & 0xff) << 16 | (ink & 0xff00) | (ink >> 16 & 0xff)));
        }
        ImGui::PopID();
    }
    if (inkClicked >= 0) { gfxInkPen = inkClicked; ImGui::OpenPopup("##gfxink"); }   // (outside the pen's ID)
    gfxInkPopup();
    row.checkbox("Draw", &gfx.draw);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draw on the picture with the pen chosen: the bytes change in the memory shown, as the\n"
                          "Gate Array (or the ASIC, for sprites) reads them. Right-click a pixel: its pen.\n"
                          "Off: a click selects a tile.");
    {
        static const char* const tools[] = { "Pen", "Line", "Box", "Filled box", "Fill" };
        ImGui::BeginDisabled(!gfx.draw);
        row.item(ImGui::GetFontSize() * 6.0f);
        ImGui::Combo("##gfxtool", &gfx.tool, tools, 5);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Pen: draw where the mouse goes. Line, Box, Filled box: from where the button goes down to\n"
                              "where it comes up. Fill: the pixels of one pen joined to the one clicked, in its tile.\n"
                              "Each is one step for Undo.");
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(gfxUndo.empty());
    if (row.smallButton("Undo##gfx")) gfxUndoStroke();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Take back the last stroke (Ctrl+Z): %d stroke%s", (int)gfxUndo.size(), gfxUndo.size() == 1 ? "" : "s");
    ImGui::BeginDisabled(gfxRedo.empty());
    if (row.smallButton("Redo##gfx")) gfxRedoStroke();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Draw again the stroke Undo took back (Ctrl+Y or Ctrl+Shift+Z)");
}

int GuiShell::gfxRead(int address) const {
    const GX4000* e = host.emu;
    if (!e || !e->memory) return 0;
    switch (gfx.source) {
        case 0: return e->memory->readMapped(address & 0xffff);
        case 1: return e->memory->ram.empty() ? 0 : e->memory->ram[(size_t)address % e->memory->ram.size()];
        default: return e->memory->asicRam.size() > 0xfff ? e->memory->asicRam[(size_t)(address & 0xfff)] : 0;
    }
}

// the address of byte x of line y of the tile starting at `start`
int GuiShell::gfxAddress(int start, int x, int y) const {
    if (gfx.screenLayout) {
        // the CPC's screen: the next line &800 on, every eighth line a character row on,
        // within the 2K of each block (CRTC MA0-9) and the 16K page
        const int raster = ((start & 0x3800) + ((y & 7) << 11)) & 0x3800;
        const int character = ((start & 0x7ff) + (y >> 3) * gfx.widthBytes + x) & 0x7ff;
        return (start & ~0x3fff) | raster | character;
    }
    return start + y * gfx.widthBytes + x;
}

uint32_t GuiShell::gfxInk(int pen) const {
    const GX4000* e = host.emu;
    int rgb;
    if (gfx.palette == 2 || !e) rgb = 0x111111 * (pen & 15);
    else if (gfx.palette == 1) rgb = pen == 0 ? 0x000000 : (e->asic ? e->asic->spriteColor(16 + pen) : 0);
    else if (e->plusHardware && e->asic) rgb = e->asic->color(pen & 15);
    else rgb = GateArray::rgbForHardwareColour(e->gateArray->gaPalette[(size_t)(pen & 15)] & 0x1f);
    return 0xff000000u | (uint32_t)(rgb & 0xff) << 16 | (uint32_t)(rgb & 0xff00) | (uint32_t)(rgb >> 16 & 0xff);
}

void GuiShell::windowGraphicsFinder() {
    if (!panelOpen("Graphics finder")) return;
    if (!ImGui::Begin("Graphics finder", &panelOpen("Graphics finder"))) { ImGui::End(); return; }
    struct EndWindow { ~EndWindow() { ImGui::End(); } } endWindow;   // after the wrap guard below ends
    WrapText wrapText;
    if (!host.booted()) { textWrappedDisabled("No machine."); return; }
    // the drawing history belongs to the machine as it is: a boot or a reset ends it
    if (gfxUndoMachine != host.emu || gfxUndoResets != host.emu->resetCount) {
        gfxUndo.clear(); gfxRedo.clear();
        gfxUndoMachine = host.emu; gfxUndoResets = host.emu->resetCount;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) gfxUndoStroke();
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) gfxRedoStroke();
    }
    const float field = ImGui::GetFontSize() * 9.0f;
    // the settings
    {
        FlowRow row;
        row.item(field * 1.6f);
        ImGui::Combo("##gfxsrc", &gfx.source, kSources, 3);
        row.item(field * 1.8f);
        ImGui::Combo("##gfxfmt", &gfx.format, kFormats, 5);
        row.item(field * 1.2f);
        ImGui::Combo("##gfxpal", &gfx.palette, kPalettes, 3);
    }
    if (gfx.source == 2 && gfx.format != 4 && gfx.lastSource != 2) { gfx.format = 4; gfx.widthBytes = 16; gfx.height = 16; gfx.palette = 1; gfx.address = 0; gfx.screenLayout = false; }
    gfx.lastSource = gfx.source;
    const int limit = gfx.source == 0 ? 0x10000 : gfx.source == 1 ? (int)host.emu->memory->ram.size() : 0x1000;
    {
        FlowRow row;
        row.item(field * 0.8f);
        char addr[16]; std::snprintf(addr, sizeof addr, "%04X", gfx.address);
        if (ImGui::InputText("##gfxaddr", addr, sizeof addr, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue))
            gfx.address = (int)std::strtol(addr, nullptr, 16);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where it starts (hex). The mouse wheel over the picture moves it a row\nof tiles; with Shift a byte, with Ctrl it zooms.");
        if (row.smallButton("-1")) gfx.address -= 1;
        if (row.smallButton("+1")) gfx.address += 1;
        if (row.smallButton("-line")) gfx.address -= gfx.widthBytes;
        if (row.smallButton("+line")) gfx.address += gfx.widthBytes;
        if (gfx.source == 0 && row.smallButton("Screen")) {
            // the screen as the CRTC shows it: R12/R13's start, R1's width
            const auto& r = host.emu->crtc->registers;
            gfx.address = ((r[12] & 0x30) << 10) | ((r[12] & 3) << 9 | r[13] << 1);
            gfx.widthBytes = std::max(1, r[1] * 2);
            gfx.height = std::max(8, (r[6] & 0x7f) * ((r[9] & 0x1f) + 1));
            gfx.screenLayout = true;
            gfx.format = std::clamp(host.emu->gateArray->mode, 0, 2);
            gfx.palette = 0;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The screen the CRTC shows now: its start (R12, R13), width (R1) and height");
    }
    {
        FlowRow row;
        row.item(field * 0.9f);
        ImGui::SliderInt("##gfxw", &gfx.widthBytes, 1, 96, "width %d bytes");
        row.item(field * 0.9f);
        ImGui::SliderInt("##gfxh", &gfx.height, 1, 256, "height %d lines");
        row.item(field * 0.6f);
        ImGui::SliderInt("##gfxz", &gfx.zoom, 1, 8, "zoom %dx");
        row.checkbox("CPC screen layout", &gfx.screenLayout);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lines &800 apart, a character row every eight, as the CRTC lays out the screen.\nOff: a tile's lines one after the other, as sprites are kept.");
        row.checkbox("CPC pixel shape", &gfx.cpcShape);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mode 0 pixels twice as wide, mode 2 pixels twice as tall, as the monitor shows them");
        row.checkbox("Grid", &gfx.grid);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A frame round each tile");
        row.checkbox("Pixel grid", &gfx.pixelGrid);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lines between the CPC pixels, when a pixel is 4 screen pixels or more (zoom in)");
    }
    gfx.pen = std::clamp(gfx.pen, 0, gfxPenCount() - 1);          // (a format with fewer pens)
    gfxPens();
    gfx.address = ((gfx.address % limit) + limit) % limit;
    const int ppb = kPixelsPerByte[gfx.format];
    const int tileW = gfx.widthBytes * ppb, tileH = gfx.height;
    const float sx = gfx.cpcShape && gfx.format == 0 ? 2.0f : 1.0f, sy = gfx.cpcShape && gfx.format == 2 ? 2.0f : 1.0f;
    const float pw = sx * gfx.zoom, ph = sy * gfx.zoom;   // a pixel on the window
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2;
    const int cols = gfx.screenLayout ? 1 : std::clamp((int)(avail.x / (tileW * pw + 2)), 1, 64);
    const int rows = gfx.screenLayout ? 1 : std::clamp((int)std::ceil((avail.y - footer) / (tileH * ph + 2)), 1, 64);
    const int tileBytes = gfx.widthBytes * gfx.height;
    // the picture: every tile, a pixel per texel
    const int texW = std::min(4096, cols * tileW), texH = std::min(4096, rows * tileH);
    gfxPixels.assign((size_t)texW * texH, 0xff000000u);
    uint32_t inks[16];
    for (int p = 0; p < 16; p++) inks[p] = gfxInk(p);
    for (int t = 0; t < cols * rows; t++) {
        const int start = gfx.address + t * tileBytes;
        const int ox = t % cols * tileW, oy = t / cols * tileH;
        for (int y = 0; y < tileH && oy + y < texH; y++)
            for (int x = 0; x < gfx.widthBytes; x++) {
                const int a = gfxAddress(start, x, y);
                const int v = a < limit || gfx.source == 0 ? gfxRead(a % std::max(1, limit)) : 0;
                for (int i = 0; i < ppb; i++) {
                    const int px = ox + x * ppb + i;
                    if (px < texW) gfxPixels[(size_t)(oy + y) * texW + px] = inks[pixelPen(gfx.format, v, i)];
                }
            }
    }
    if (uploadTexture) gfxFinderTexture = uploadTexture(21, gfxPixels.data(), texW, texH);
    // drawn a tile at a time, two pixels apart
    ImGui::BeginChild("##gfxview", ImVec2(0, -footer), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float cellW = tileW * pw + 2, cellH = tileH * ph + 2;
    // a button over the picture: it takes the mouse, so a drag draws (not moves the window)
    ImGui::InvisibleButton("##gfxcanvas", ImVec2(std::max(1.0f, cols * cellW), std::max(1.0f, rows * cellH)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool drawing = gfx.draw && gfx.tool == 0 && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool canvasActivated = ImGui::IsItemActivated(), canvasActive = ImGui::IsItemActive();
    for (int t = 0; t < cols * rows && gfxFinderTexture; t++) {
        const int c = t % cols, r = t / cols;
        const ImVec2 p0(origin.x + c * cellW, origin.y + r * cellH), p1(p0.x + tileW * pw, p0.y + tileH * ph);
        const ImVec2 uv0((float)(c * tileW) / texW, (float)(r * tileH) / texH), uv1((float)((c + 1) * tileW) / texW, (float)((r + 1) * tileH) / texH);
        dl->AddImage((ImTextureID)(intptr_t)gfxFinderTexture, p0, p1, uv0, uv1);
        // lines between the CPC pixels, drawn only where they can be seen
        if (gfx.pixelGrid && pw >= 4 && ph >= 4) {
            const ImVec2 clip0 = dl->GetClipRectMin(), clip1 = dl->GetClipRectMax();
            const float x0 = std::max(p0.x, clip0.x), x1 = std::min(p1.x, clip1.x);
            const float y0 = std::max(p0.y, clip0.y), y1 = std::min(p1.y, clip1.y);
            if (x0 < x1 && y0 < y1) {
                const ImU32 line = IM_COL32(128, 128, 136, 110);
                for (int k = std::max(1, (int)std::ceil((x0 - p0.x) / pw)); k < tileW && p0.x + k * pw <= x1; k++)
                    dl->AddLine(ImVec2(std::floor(p0.x + k * pw), y0), ImVec2(std::floor(p0.x + k * pw), y1), line);
                for (int k = std::max(1, (int)std::ceil((y0 - p0.y) / ph)); k < tileH && p0.y + k * ph <= y1; k++)
                    dl->AddLine(ImVec2(x0, std::floor(p0.y + k * ph)), ImVec2(x1, std::floor(p0.y + k * ph)), line);
            }
        }
        if (gfx.grid) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), IM_COL32(80, 80, 90, 255));
        if (gfx.address + t * tileBytes == gfx.selected) dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1), ImGui::GetColorU32(kAccent), 0, 0, 2.0f);
    }
    // A pixel of the picture (in picture pixels: tiles side by side, no gaps) painted with the pen.
    auto paint = [&](int px, int py) {
        if (px < 0 || py < 0) return;
        const int c = px / tileW, r = py / tileH;
        if (c >= cols || r >= rows) return;
        const int start = gfx.address + (r * cols + c) * tileBytes;
        const int a = gfxAddress(start, px % tileW / ppb, py % tileH);
        if (gfx.source != 0 && a >= limit) return;              // past the end of what is shown
        const int at = a % std::max(1, limit);
        gfxStrokeByte(at, withPixelPen(gfx.format, gfxRead(at), px % tileW % ppb, gfx.pen));
    };
    // the pen of a picture pixel (-1: none there)
    auto penAt = [&](int px, int py) -> int {
        if (px < 0 || py < 0) return -1;
        const int c = px / tileW, r = py / tileH;
        if (c >= cols || r >= rows) return -1;
        const int a = gfxAddress(gfx.address + (r * cols + c) * tileBytes, px % tileW / ppb, py % tileH);
        if (gfx.source != 0 && a >= limit) return -1;
        return pixelPen(gfx.format, gfxRead(a % std::max(1, limit)), px % tileW % ppb);
    };
    const ImGuiIO& io = ImGui::GetIO();
    // the picture pixel under the mouse (-1: none)
    int mouseX = -1, mouseY = -1;
    {
        const int c = (int)std::floor((io.MousePos.x - origin.x) / cellW), r = (int)std::floor((io.MousePos.y - origin.y) / cellH);
        const int x = (int)std::floor((io.MousePos.x - origin.x - c * cellW) / pw), y = (int)std::floor((io.MousePos.y - origin.y - r * cellH) / ph);
        if (c >= 0 && c < cols && r >= 0 && r < rows && x >= 0 && x < tileW && y >= 0 && y < tileH) { mouseX = c * tileW + x; mouseY = r * tileH + y; }
    }
    if (hovered || drawing) {
        if (mouseX >= 0) {
            const int c = mouseX / tileW, r = mouseY / tileH, x = mouseX % tileW, y = mouseY % tileH;
            const int start = gfx.address + (r * cols + c) * tileBytes;
            const int a = gfxAddress(start, x / ppb, y);
            const int penHere = pixelPen(gfx.format, gfxRead(a % std::max(1, limit)), x % ppb);
            if (gfx.draw) {
                static const char* const verbs[] = { "draw", "a line", "a box", "a filled box", "fill" };
                ImGui::SetTooltip("&%04X  (pixel %d, line %d)\npen %d\nLeft: %s with pen %d   Right: take pen %d", a % std::max(1, limit), x, y,
                                  penHere, verbs[std::clamp(gfx.tool, 0, 4)], gfx.pen, penHere);
                if (drawing) {
                    // joined to the last pixel painted, so a quick stroke has no gaps
                    if (gfx.lastX < 0) paint(mouseX, mouseY);
                    else for (const auto& pt : shapePoints(1, gfx.lastX, gfx.lastY, mouseX, mouseY)) paint(pt.first, pt.second);
                    gfx.lastX = mouseX; gfx.lastY = mouseY;
                }
                // FILL: the pixels of the clicked one's pen joined to it (up, down, left, right), in its tile
                if (gfx.tool == 4 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && penHere != gfx.pen) {
                    const int x0 = c * tileW, y0 = r * tileH;
                    std::vector<uint8_t> seen((size_t)tileW * tileH, 0);
                    std::vector<std::pair<int, int>> todo = { { mouseX, mouseY } };
                    seen[(size_t)y * tileW + x] = 1;
                    while (!todo.empty()) {
                        const auto pt = todo.back();
                        todo.pop_back();
                        paint(pt.first, pt.second);
                        const int nx[4] = { pt.first - 1, pt.first + 1, pt.first, pt.first }, ny[4] = { pt.second, pt.second, pt.second - 1, pt.second + 1 };
                        for (int k = 0; k < 4; k++) {
                            if (nx[k] < x0 || nx[k] >= x0 + tileW || ny[k] < y0 || ny[k] >= y0 + tileH) continue;
                            uint8_t& s = seen[(size_t)(ny[k] - y0) * tileW + (nx[k] - x0)];
                            if (s || penAt(nx[k], ny[k]) != penHere) continue;
                            s = 1;
                            todo.push_back({ nx[k], ny[k] });
                        }
                    }
                    gfxEndStroke();
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) gfx.pen = std::min(penHere, gfxPenCount() - 1);
            } else {
                ImGui::SetTooltip("&%04X  (pixel %d, line %d of the tile at &%04X)\npen %d\nClick: select the tile", a % std::max(1, limit), x, y,
                                  start % std::max(1, limit), penHere);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gfx.selected = start;
            }
        }
        if (io.MouseWheel != 0) {
            if (io.KeyCtrl) gfx.zoom = std::clamp(gfx.zoom + (io.MouseWheel > 0 ? 1 : -1), 1, 8);
            else gfx.address -= (int)io.MouseWheel * (io.KeyShift ? 1 : gfx.screenLayout ? gfx.widthBytes : cols * tileBytes);
        }
    }
    // LINE, BOX, FILLED BOX: from where the button went down to where it comes up, shown on
    // the way; drawn (one undo step) when it comes up
    const bool shapeTool = gfx.draw && gfx.tool >= 1 && gfx.tool <= 3;
    if (shapeTool && canvasActivated && mouseX >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        gfx.anchorX = gfx.lastX = mouseX;
        gfx.anchorY = gfx.lastY = mouseY;
    }
    if (gfx.anchorX >= 0) {
        if (!shapeTool) gfx.anchorX = gfx.anchorY = -1;                  // (the tool changed on the way)
        else {
            if (mouseX >= 0) { gfx.lastX = mouseX; gfx.lastY = mouseY; }
            const auto points = shapePoints(gfx.tool, gfx.anchorX, gfx.anchorY, gfx.lastX, gfx.lastY);
            if (canvasActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImU32 ink = (gfxInk(gfx.pen) & 0x00ffffffu) | 0xc0000000u;
                for (const auto& pt : points) {
                    const int c = pt.first / tileW, r = pt.second / tileH;
                    if (c >= cols || r >= rows) continue;
                    const ImVec2 q(origin.x + c * cellW + (pt.first % tileW) * pw, origin.y + r * cellH + (pt.second % tileH) * ph);
                    dl->AddRectFilled(q, ImVec2(q.x + pw, q.y + ph), ink);
                }
            } else {
                for (const auto& pt : points) paint(pt.first, pt.second);
                gfxEndStroke();
                gfx.anchorX = gfx.anchorY = -1;
                gfx.lastX = gfx.lastY = -1;
            }
        }
    }
    if (!drawing && gfx.anchorX < 0) {                              // the stroke ends: one undo step
        gfx.lastX = gfx.lastY = -1;
        gfxEndStroke();
    }
    ImGui::EndChild();
    // the tile selected: out as a picture, or as bytes
    {
        FlowRow row;
        const bool any = gfx.selected >= 0;
        char sel[64];
        if (any) std::snprintf(sel, sizeof sel, "tile &%04X, %dx%d", gfx.selected % std::max(1, limit), tileW, tileH);
        row.textDisabled(any ? sel : "click a tile to select it");
        ImGui::BeginDisabled(!any);
        if (row.smallButton("Save PNG...")) {
            char name[32]; std::snprintf(name, sizeof name, "graphics_%04X.png", gfx.selected % std::max(1, limit));
            const int start = gfx.selected;
            saver.open("Save graphics", host.recordDir, name, [this, start, tileW, tileH, ppb](const std::string& path) {
                std::vector<uint8_t> rgb((size_t)tileW * tileH * 3);
                for (int y = 0; y < tileH; y++)
                    for (int x = 0; x < tileW; x++) {
                        const uint32_t p = gfxInk(pixelPen(gfx.format, gfxRead(gfxAddress(start, x / ppb, y)), x % ppb));
                        uint8_t* d = &rgb[((size_t)y * tileW + x) * 3];
                        d[0] = (uint8_t)p; d[1] = (uint8_t)(p >> 8); d[2] = (uint8_t)(p >> 16);
                    }
                host.status = writePng(path, rgb.data(), tileW, tileH) ? "Saved " + path : "Could not write " + path;
            });
        }
        if (row.smallButton("Copy as DB")) {
            std::string text;
            char b[8];
            std::snprintf(b, sizeof b, "%04X", gfx.selected % std::max(1, limit));
            text = "; " + std::to_string(tileW) + "x" + std::to_string(tileH) + " from &" + b + "\n";
            for (int y = 0; y < tileH; y++) {
                text += "  db ";
                for (int x = 0; x < gfx.widthBytes; x++) {
                    std::snprintf(b, sizeof b, "&%02X", gfxRead(gfxAddress(gfx.selected, x, y)));
                    text += (x ? "," : "") + std::string(b);
                }
                text += "\n";
            }
            ImGui::SetClipboardText(text.c_str());
            host.status = "Copied the tile as DB lines";
        }
        if (gfx.source == 0 && row.smallButton("Show in memory")) { panelOpen("Memory") = true; showInMemory(gfx.selected & 0xffff); }
        if (row.smallButton("Copy tile") && gfxCopyTile()) host.status = "Copied the tile (" + std::to_string(gfxClipW) + " bytes x " + std::to_string(gfxClipH) + " lines)";
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Keep the tile's bytes, to paste them into another tile");
        ImGui::BeginDisabled(gfxClip.empty());
        if (row.smallButton("Paste tile") && gfxPasteTile()) host.status = "Pasted the tile copied (Ctrl+Z takes it back)";
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The tile copied into the selected tile, as much as fits (%d bytes x %d lines copied)", gfxClipW, gfxClipH);
        ImGui::EndDisabled();
        if (row.smallButton("Import PNG...")) {
            browser.open("Import a PNG into the tile", host.recordDir, { ".png" }, [this](const std::string& path) {
                std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
                std::error_code ec;
                if (!f || std::filesystem::file_size(std::filesystem::u8path(path), ec) > (32u << 20)) { host.status = "Cannot open " + path; return; }
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                host.status = gfxImportPng(bytes);
            });
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("A picture into the selected tile: each pixel the pen whose ink is nearest (a clear pixel pen 0).\n"
                              "One CPC pixel a picture pixel, as Save PNG writes them. One step for Undo.");
        ImGui::EndDisabled();
    }
}

} // namespace cpcse
