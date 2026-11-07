// CPCSyntaxError: the Expansion port window. See gui_expansion.h and core/expansion_bus.h.
#include "gui_expansion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "emuhost.h"
#include "gui_widgets.h"
#include "core/emulator.h"
#include "core/expansion_bus.h"
#include "core/m4.h"
#include "core/multiface.h"
#include "core/rs232.h"
#include "core/opl4.h"
#include "core/playcity.h"
#include "core/speech.h"
#include "core/symbiface_mouse.h"
#include "core/sf2_rtc.h"
#include "core/sf3.h"
#include "core/symbiface_ide.h"
#include "core/ch376.h"
#include "core/albireo_serial.h"
#include "core/hd20.h"
#include "core/mouse_interface.h"
#include "core/v9990.h"
#include "core/dac.h"

namespace cpcse {

namespace {

// The lamp colour of a pin when lit: what kind of signal it is.
ImU32 lampColour(int number, float on) {
    auto mix = [&](int r, int g, int b) {
        const float dim = 0.18f + 0.82f * on;
        return IM_COL32((int)(r * dim), (int)(g * dim), (int)(b * dim), 255);
    };
    if (number == pin::GND1 || number == pin::GND2) return IM_COL32(40, 40, 40, 255);
    if (number == pin::VCC) return mix(235, 60, 50);
    if (number >= pin::A15 && number <= pin::A0) return mix(70, 200, 255);           // address
    if (number >= pin::D7 && number <= pin::D0) return mix(120, 255, 200);           // data
    if (number == pin::INT || number == pin::NMI || number == pin::RESET || number == pin::BUSRESET
        || number == pin::READY || number == pin::BUSRQ || number == pin::BUSAK || number == pin::HALT)
        return mix(255, 90, 60);                                                     // the CPU's control
    if (number >= pin::ROMEN && number <= pin::RAMDIS) return mix(255, 200, 60);     // the memory map
    if (number == pin::SOUND) return mix(200, 120, 255);
    return mix(90, 235, 90);                                                         // bus strobes, clock
}

std::string pinLabel(const ExpansionPin& p) { return p.activeLow ? std::string("/") + p.name : std::string(p.name); }

bool boardFitted(GX4000& e, BusDevice d) {
    switch (d) {
        case BusDevice::M4: return e.m4->enabled;
        case BusDevice::Multiface: return e.multiface->enabled;
        case BusDevice::Rs232: return e.serial->enabled;
        case BusDevice::Opl4: return e.opl4->enabled;
        case BusDevice::PlayCity: return e.playcity->enabled;
        case BusDevice::Speech: return e.speech->kind != SpeechSynth::Kind::None;
        case BusDevice::Sf2Mouse: return e.symbifaceMouse->enabled;
        case BusDevice::Sf2Rtc: return e.sf2Rtc->enabled;
        case BusDevice::Sf3: return e.sf3->enabled;
        case BusDevice::Ide: return e.ide->enabled;
        case BusDevice::Albireo1: return e.albireo[0]->enabled;
        case BusDevice::Albireo2: return e.albireo[1]->enabled;
        case BusDevice::AlbireoUart: return e.albireoSerial->enabled;
        case BusDevice::Hd20: return e.hd20->enabled();
        case BusDevice::MouseInterface: return e.mouseIf.kind != MouseInterface::None;
        case BusDevice::Gfx9000: return e.v9990->enabled;
        case BusDevice::AmdrumDac: return e.dac->mode == "amdrum";
        default: return false;
    }
}

std::string decoderNames(uint32_t mask, bool boardsOnly) {
    std::string s;
    for (int d = 0; d < BUS_DEVICE_COUNT; d++) {
        if (!(mask >> d & 1) || (boardsOnly && !isBoard(d))) continue;
        if (!s.empty()) s += ", ";
        s += busDeviceName((BusDevice)d);
    }
    return s;
}

int popcount(uint32_t v) { int n = 0; while (v) { n += (int)(v & 1); v >>= 1; } return n; }

}   // namespace

ExpansionPortWindow::ExpansionPortWindow(EmuHost& h) : host(h) {}

// Which ports each fitted board decodes, and where two decode the same one. Read from the
// decoders themselves (GX4000::busDecoders), over the boards' space &F800-&FFFF, both ways.
void ExpansionPortWindow::scanDecoders() {
    GX4000* e = host.emu;
    clashes.clear();
    for (auto& s : boardPorts) s.clear();
    fittedBoards = 0;
    if (!e) return;
    for (int d = FIRST_BOARD; d < BUS_DEVICE_COUNT; d++) if (boardFitted(*e, (BusDevice)d)) fittedBoards |= 1u << d;
    // per board: the ports, as ranges, each marked by direction
    std::array<std::vector<std::pair<int, int>>, 32> dirs;       // port, 1 read 2 write 3 both
    for (int port = 0xf800; port <= 0xffff; port++) {
        const uint32_t r = e->busDecoders(port, false) & BOARD_MASK;
        const uint32_t w = e->busDecoders(port, true) & BOARD_MASK;
        for (int d = FIRST_BOARD; d < BUS_DEVICE_COUNT; d++) {
            const int how = (r >> d & 1) | ((w >> d & 1) << 1);
            if (how) dirs[(size_t)d].push_back({ port, how });
        }
        for (int wr = 0; wr < 2; wr++) {
            const uint32_t m = wr ? w : r;
            if (popcount(m) < 2) continue;
            if (!clashes.empty() && clashes.back().write == (wr != 0) && clashes.back().boards == m && clashes.back().last == port - 1)
                clashes.back().last = port;
            else clashes.push_back({ port, port, wr != 0, m });
        }
    }
    for (int d = FIRST_BOARD; d < BUS_DEVICE_COUNT; d++) {
        const auto& v = dirs[(size_t)d];
        // as ranges, each marked by direction
        std::vector<std::string> ranges;
        for (size_t i = 0; i < v.size();) {
            size_t j = i;
            while (j + 1 < v.size() && v[j + 1].first == v[j].first + 1 && v[j + 1].second == v[i].second) j++;
            char b[48];
            if (j == i) std::snprintf(b, sizeof b, "&%04X", v[i].first);
            else std::snprintf(b, sizeof b, "&%04X-&%04X", v[i].first, v[j].first);
            ranges.push_back(std::string(b) + (v[i].second == 1 ? " (in)" : v[i].second == 2 ? " (out)" : ""));
            i = j + 1;
        }
        std::string out;
        if (ranges.size() <= 6) {
            for (const std::string& r : ranges) out += (out.empty() ? "" : ", ") + r;
        } else {
            // A PARTIAL decoder answers a pattern, not a list: the bits all its ports share
            // (x: either), over the boards' space scanned (&F800-&FFFF, so A15-A11 read 1).
            int ones = 0xffff, zeros = 0xffff;
            for (const auto& pv : v) { ones &= pv.first; zeros &= ~pv.first & 0xffff; }
            char pat[24];
            int k = 0;
            for (int b = 15; b >= 0; b--) {
                pat[k++] = (ones >> b & 1) ? '1' : (zeros >> b & 1) ? '0' : 'x';
                if (b % 4 == 0 && b) pat[k++] = ' ';
            }
            pat[k] = 0;
            out = std::to_string(v.size()) + " ports: " + pat;
        }
        boardPorts[(size_t)d] = out;
    }
}

void ExpansionPortWindow::draw(bool* open) {
    focused = false;
    if (!ImGui::Begin("Expansion port", open)) { ImGui::End(); return; }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    GX4000* e = host.emu;
    if (!e) { ImGui::TextDisabled("No machine"); ImGui::End(); return; }
    ExpansionBusProbe& p = e->busProbe;
    p.enabled = true;                         // it samples while this window is open

    // the pins: this frame's share of active microseconds, held a moment so a short
    // pulse can be seen, and a second's counts for the tooltip
    const double now = ImGui::GetTime();
    const float decay = std::pow(0.02f, ImGui::GetIO().DeltaTime);          // to 2% in a second
    for (int i = 0; i < EXPANSION_PIN_COUNT; i++) {
        const float duty = p.samples ? (float)p.activeUs[(size_t)i] / (float)p.samples : (p.level[(size_t)i] ? 1.0f : 0.0f);
        const float lit = (duty > 0.0f || p.edges[(size_t)i] > 0) ? 0.45f + 0.55f * duty : 0.0f;
        glow[(size_t)i] = std::max(lit, glow[(size_t)i] * decay);
        secActive[(size_t)i] += p.activeUs[(size_t)i];
        secEdges[(size_t)i] += p.edges[(size_t)i];
    }
    secSamples += p.samples;
    if (now - secStarted >= 1.0) {
        for (int i = 0; i < EXPANSION_PIN_COUNT; i++) {
            dutyShown[(size_t)i] = secSamples ? 100.0 * (double)secActive[(size_t)i] / (double)secSamples : 0.0;
            edgesShown[(size_t)i] = (double)secEdges[(size_t)i] / std::max(0.001, now - secStarted);
        }
        secActive.fill(0); secEdges.fill(0); secSamples = 0; secStarted = now;
    }
    // the boards: lit on an access, fading
    uint32_t fittedNow = 0;
    for (int d = FIRST_BOARD; d < BUS_DEVICE_COUNT; d++) if (boardFitted(*e, (BusDevice)d)) fittedNow |= 1u << d;
    if (fittedNow != scannedFor || now - lastScan > 2.0) { scanDecoders(); scannedFor = fittedNow; lastScan = now; }
    for (int d = 0; d < BUS_DEVICE_COUNT; d++) {
        const auto& s = p.devices[(size_t)d];
        const uint64_t count = s.totalReads + s.totalWrites;
        const bool hit = count != boardSeen[(size_t)d];
        boardSeen[(size_t)d] = count;
        boardGlow[(size_t)d] = hit ? 1.0f : boardGlow[(size_t)d] * decay;
    }

    // the toolbar
    {
        FlowRow bar;
        bar.checkbox("Log the boards' space only", &p.logExpansionOnly);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("On: only &F8xx-&FFxx and anything a board decodes.\nOff: every I/O cycle, the CPC's own chips too.");
        if (bar.button("Clear log")) p.clearLog();
        if (bar.button("Stop on access...")) ImGui::OpenPopup("##busbreak");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop the CPC, as at a breakpoint, on the I/O cycle that reaches a device");
        if (ImGui::BeginPopup("##busbreak")) {
            for (int d = 0; d < BUS_DEVICE_COUNT; d++) {
                if (isBoard(d) && !(fittedBoards >> d & 1)) continue;
                bool on = (p.breakOn >> d & 1) != 0;
                if (ImGui::Checkbox(busDeviceName((BusDevice)d), &on)) p.breakOn = on ? p.breakOn | (1u << d) : p.breakOn & ~(1u << d);
                if (d == FIRST_BOARD - 1) ImGui::Separator();
            }
            ImGui::EndPopup();
        }
        if (p.breakOn) {
            bar.textColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), ("stops on: " + decoderNames(p.breakOn, false)).c_str());
        }
    }
    if (host.paused && p.breakOn && (p.lastDecoders & p.breakOn)) {
        if (!breakNoted) {
            host.breakReason = std::string("Expansion port: ") + (p.lastWrite ? "OUT " : "IN ") + decoderNames(p.lastDecoders & p.breakOn, false);
            breakNoted = true;
        }
    } else breakNoted = false;

    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float lamp = ImGui::GetTextLineHeight() * 0.36f;
    const float connectorW = ImGui::CalcTextSize("/BUS RESET 40  ").x * 2.0f + lamp * 8.0f;
    const bool sideBySide = ImGui::GetContentRegionAvail().x > connectorW + ImGui::GetFontSize() * 26.0f;

    // the connector: odd pins on the left, even on the right, as the edge connector's
    // two sides; a lamp per pin
    ImGui::BeginChild("##connector", ImVec2(sideBySide ? connectorW : 0.0f, lineH * 25.0f + ImGui::GetStyle().WindowPadding.y * 2.0f),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto& pins = expansionPins();
        const float textW = ImGui::CalcTextSize("/BUS RESET").x;
        const float numW = ImGui::CalcTextSize("50").x;
        for (int row = 0; row < 25; row++) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            for (int side = 0; side < 2; side++) {
                const ExpansionPin& pn = pins[(size_t)(row * 2 + side)];
                const std::string label = pinLabel(pn);
                char num[4]; std::snprintf(num, sizeof num, "%d", pn.number);
                const float sz = ImGui::CalcTextSize(label.c_str()).x;
                float x;
                ImVec2 centre;
                if (side == 0) {
                    x = at.x + textW - sz;
                    dl->AddText(ImVec2(x, at.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                    dl->AddText(ImVec2(at.x + textW + 6.0f, at.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), num);
                    centre = ImVec2(at.x + textW + 6.0f + numW + 6.0f + lamp, at.y + ImGui::GetTextLineHeight() * 0.5f);
                } else {
                    const float base = at.x + textW + 6.0f + numW + 6.0f + lamp * 2.0f + lamp * 2.0f;
                    centre = ImVec2(base + lamp, at.y + ImGui::GetTextLineHeight() * 0.5f);
                    dl->AddText(ImVec2(base + lamp * 2.0f + 6.0f, at.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), num);
                    dl->AddText(ImVec2(base + lamp * 2.0f + 6.0f + numW + 6.0f, at.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                }
                const float g = glow[(size_t)(pn.number - 1)];
                dl->AddCircleFilled(centre, lamp, lampColour(pn.number, g));
                if (g > 0.05f) dl->AddCircle(centre, lamp + 1.5f, lampColour(pn.number, g * 0.6f), 0, 1.5f);
                // the tooltip on the lamp and its label
                const ImVec2 hoverMin = side == 0 ? ImVec2(at.x, at.y) : ImVec2(centre.x - lamp, at.y);
                const ImVec2 hoverMax = side == 0 ? ImVec2(centre.x + lamp, at.y + lineH) : ImVec2(centre.x + lamp + 6.0f + numW + 6.0f + textW, at.y + lineH);
                if (ImGui::IsMouseHoveringRect(hoverMin, hoverMax) && ImGui::IsWindowHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("Pin %d  %s%s", pn.number, label.c_str(), pn.activeLow ? "  (active low)" : "");
                    ImGui::TextUnformatted(pn.what);
                    ImGui::TextDisabled("Active %.1f%% of the last second, %.0f times a second",
                                        dutyShown[(size_t)(pn.number - 1)], edgesShown[(size_t)(pn.number - 1)]);
                    ImGui::EndTooltip();
                }
            }
            ImGui::Dummy(ImVec2(connectorW - ImGui::GetStyle().WindowPadding.x * 2.0f, lineH));
        }
    }
    ImGui::EndChild();
    if (sideBySide) ImGui::SameLine();

    // the boards, the clashes, the log
    ImGui::BeginChild("##busright", ImVec2(0, 0), ImGuiChildFlags_None);
    {
        // the last I/O cycle
        if (p.lastPort >= 0) {
            const std::string who = decoderNames(p.lastDecoders, false);
            char line[96];
            std::snprintf(line, sizeof line, "Last I/O: %s &%04X %s &%02X at PC &%04X: ", p.lastWrite ? "OUT" : "IN", p.lastPort,
                          p.lastWrite ? "<-" : "->", p.lastValue, p.lastPc);
            const bool clashNow = popcount(p.lastDecoders & BOARD_MASK) >= 2;
            if (clashNow) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s%s", line, who.empty() ? "no device answers" : who.c_str());
            if (clashNow) ImGui::PopStyleColor();
        } else ImGui::TextDisabled("No I/O cycle yet");
        ImGui::Separator();

        // the boards on the bus
        ImGui::TextUnformatted("Boards on the bus");
        if (!fittedBoards) ImGui::TextDisabled("  None fitted (Machine > Expansions)");
        else if (ImGui::BeginTable("##boards", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("##lamp");
            ImGui::TableSetupColumn("Board");
            ImGui::TableSetupColumn("In/s");
            ImGui::TableSetupColumn("Out/s");
            ImGui::TableSetupColumn("Ports it decodes / last access", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            const float fps = std::max(1.0f, ImGui::GetIO().Framerate);
            for (int d = FIRST_BOARD; d < BUS_DEVICE_COUNT; d++) {
                if (!(fittedBoards >> d & 1)) continue;
                const auto& s = p.devices[(size_t)d];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const ImVec2 c = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(c.x + lamp + 2.0f, c.y + ImGui::GetTextLineHeight() * 0.5f), lamp,
                                                            lampColour(pin::IORQ, boardGlow[(size_t)d]));
                ImGui::Dummy(ImVec2(lamp * 2.0f + 4.0f, ImGui::GetTextLineHeight()));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(busDeviceName((BusDevice)d));
                ImGui::TableNextColumn(); ImGui::Text("%.0f", s.reads * fps);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", s.writes * fps);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", boardPorts[(size_t)d].empty() ? "(no I/O port)" : boardPorts[(size_t)d].c_str());
                ImGui::PopStyleColor();
                if (s.lastPort >= 0)
                    ImGui::TextWrapped("last %s &%04X %s &%02X", s.lastWrite ? "OUT" : "IN", s.lastPort, s.lastWrite ? "<-" : "->", s.lastValue);
            }
            ImGui::EndTable();
        }
        // the clashes
        if (!clashes.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Ports two boards both answer (a real bus has the same clash):");
            for (const Clash& c : clashes) {
                char range[24];
                if (c.first == c.last) std::snprintf(range, sizeof range, "&%04X", c.first);
                else std::snprintf(range, sizeof range, "&%04X-&%04X", c.first, c.last);
                ImGui::BulletText("%s %s: %s", range, c.write ? "(out)" : "(in)", decoderNames(c.boards, true).c_str());
            }
        } else if (popcount(fittedBoards) >= 2) ImGui::TextDisabled("No two boards answer the same port.");
        ImGui::Separator();

        // the log, newest first
        ImGui::Text("I/O log (%llu cycles)", (unsigned long long)p.logCount);
        if (ImGui::BeginTable("##buslog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, std::max(lineH * 6.0f, ImGui::GetContentRegionAvail().y)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("usec ago");
            ImGui::TableSetupColumn("PC");
            ImGui::TableSetupColumn("Cycle");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Reaches", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            const uint64_t newestUs = (uint64_t)(e->machineCycles / 4);
            const uint64_t shown = std::min<uint64_t>(p.logCount, ExpansionBusProbe::LOG_SIZE);
            for (uint64_t k = 0; k < shown; k++) {
                const ExpansionBusProbe::Access& a = p.log[(size_t)((p.logCount - 1 - k) % ExpansionBusProbe::LOG_SIZE)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)(newestUs >= a.atUs ? newestUs - a.atUs : 0));
                ImGui::TableNextColumn(); ImGui::Text("&%04X", a.pc);
                ImGui::TableNextColumn(); ImGui::Text("%s &%04X", a.write ? "OUT" : "IN ", a.port);
                ImGui::TableNextColumn(); ImGui::Text("&%02X", a.value);
                ImGui::TableNextColumn();
                const std::string who = decoderNames(a.decoders, false);
                if (who.empty()) ImGui::TextDisabled("nothing");
                else if (popcount(a.decoders & BOARD_MASK) >= 2) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "%s", who.c_str());
                else ImGui::TextUnformatted(who.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    p.take();
    ImGui::End();
}

} // namespace cpcse
