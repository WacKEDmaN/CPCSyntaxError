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
#include <string>

#include "core/asic.h"
#include "core/crtc.h"
#include "core/emulator.h"
#include "core/gate_array.h"
#include "core/memory.h"
#include "core/png.h"

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
} // namespace

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
    ImGui::Dummy(ImVec2(cols * cellW, rows * cellH));
    const bool hovered = ImGui::IsItemHovered();
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
    if (hovered) {
        const ImGuiIO& io = ImGui::GetIO();
        const int c = (int)((io.MousePos.x - origin.x) / cellW), r = (int)((io.MousePos.y - origin.y) / cellH);
        const int x = (int)((io.MousePos.x - origin.x - c * cellW) / pw), y = (int)((io.MousePos.y - origin.y - r * cellH) / ph);
        if (c >= 0 && c < cols && r >= 0 && r < rows && x >= 0 && x < tileW && y >= 0 && y < tileH) {
            const int start = gfx.address + (r * cols + c) * tileBytes;
            const int a = gfxAddress(start, x / ppb, y);
            ImGui::SetTooltip("&%04X  (pixel %d, line %d of the tile at &%04X)\npen %d\nClick: select the tile", a % std::max(1, limit), x, y,
                              start % std::max(1, limit), pixelPen(gfx.format, gfxRead(a % std::max(1, limit)), x % ppb));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) gfx.selected = start;
        }
        if (io.MouseWheel != 0) {
            if (io.KeyCtrl) gfx.zoom = std::clamp(gfx.zoom + (io.MouseWheel > 0 ? 1 : -1), 1, 8);
            else gfx.address -= (int)io.MouseWheel * (io.KeyShift ? 1 : gfx.screenLayout ? gfx.widthBytes : cols * tileBytes);
        }
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
        ImGui::EndDisabled();
    }
}

} // namespace cpcse
