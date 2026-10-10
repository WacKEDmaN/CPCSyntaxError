// CPCSyntaxError GUI — cheats. See gui_cheats.h.
#include "gui_cheats.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

namespace cpcse {

static const ImVec4 kGood(0.45f, 0.9f, 0.45f, 1.0f);
static const ImVec4 kNote(0.95f, 0.8f, 0.35f, 1.0f);

CheatsWindow::CheatsWindow(EmuHost& h, FileBrowser& b, SaveDialog& s) : host(h), browser(b), saver(s) {}

static const char* encodingName(uint8_t e) { return e == CheatFinder::BINARY ? "as it is" : e == CheatFinder::ONE_LESS ? "one less" : "BCD"; }

void CheatsWindow::draw(bool* open) {
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cheats", open)) { focused = false; ImGui::End(); return; }
    focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!host.booted()) { ImGui::TextDisabled("Start a machine first."); ImGui::End(); return; }
    if (ImGui::BeginTabBar("##cheattabs")) {
        if (ImGui::BeginTabItem("Wizard")) { tabWizard(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Search")) { tabSearch(); ImGui::EndTabItem(); }
        char label[32];
        std::snprintf(label, sizeof label, "Cheats (%d)###cheatlist", (int)host.cheatList.cheats.size());
        if (ImGui::BeginTabItem(label)) { tabCheats(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

bool CheatsWindow::pauseButton() {
    const bool was = host.paused;
    if (ImGui::Button(host.paused ? "Carry on playing" : "Pause the game")) host.paused = !host.paused;
    return was != host.paused;
}

// ---------------------------------------------------------------- the walkthrough steps

void CheatsWindow::restart() {
    dropTestCheat();
    step = CHOOSE;
    rounds = 0;
    trying = 0;
    message.clear();
    host.cheatFinder = CheatFinder{};
}

void CheatsWindow::search(int value) {
    Bytes* ram = host.ram();
    if (!ram) return;
    CheatFinder& f = host.cheatFinder;
    if (step == NUMBER_FIRST) {
        f = CheatFinder{};
        f.width = value > 255 ? 2 : 1;
        f.start(*ram);
        f.keepEqual(*ram, value);
        seen = value;
        wanted = std::max(value, 9);
        rounds = 1;
        step = NUMBER_AGAIN;
    } else {
        f.keepEqual(*ram, value);
        rounds++;
        wanted = std::max(wanted, value);
    }
    if (f.size() == 0) message = "Nothing holds that number now. Some games keep it another way: start again, or try the bar way.";
    else if (f.size() <= 8) { message.clear(); step = PICK; }
    else message.clear();
}

void CheatsWindow::remember() {
    Bytes* ram = host.ram();
    if (!ram) return;
    host.cheatFinder = CheatFinder{};
    host.cheatFinder.start(*ram, 1u << CheatFinder::BINARY);
    rounds = 0;
    step = BAR_AGAIN;
    message.clear();
}

void CheatsWindow::barChanged(CheatFinder::Change change) {
    Bytes* ram = host.ram();
    if (!ram) return;
    host.cheatFinder.keepChange(*ram, change);
    rounds++;
    const size_t n = host.cheatFinder.size();
    if (n == 0) message = "Nothing changed that way. Start again -- and pause at the moments the bar really moves.";
    else if (n <= 8 && rounds >= 3) { message.clear(); step = PICK; }
    else message.clear();
}

void CheatsWindow::setTestCheat(const std::vector<CheatFinder::Candidate>& places, bool atCurrentValue) {
    dropTestCheat();
    Bytes* ram = host.ram();
    Cheat c;
    c.name = "(being tried)";
    c.freeze = true;
    c.enabled = true;
    const CheatFinder& f = host.cheatFinder;
    for (const CheatFinder::Candidate& p : places) {
        Poke k;
        k.address = p.address;
        k.width = f.width;
        k.value = atCurrentValue && ram ? f.stored(*ram, p.address) : f.encode(p, wanted);
        c.pokes.push_back(k);
    }
    host.cheatList.cheats.push_back(c);
    testIndex = (int)host.cheatList.cheats.size() - 1;
}

void CheatsWindow::dropTestCheat() {
    if (testIndex >= 0 && testIndex < (int)host.cheatList.cheats.size() && host.cheatList.cheats[(size_t)testIndex].name == "(being tried)")
        host.cheatList.cheats.erase(host.cheatList.cheats.begin() + testIndex);
    testIndex = -1;
}

void CheatsWindow::holdAll() {
    setTestCheat(host.cheatFinder.list(), barMode);
    step = TEST;
    host.paused = false;
}

void CheatsWindow::tryOne(size_t index) {
    const auto& all = host.cheatFinder.list();
    if (index >= all.size()) { dropTestCheat(); step = PICK; message = "None of them on its own: it may need two of them together. Try holding them all again, or start again."; return; }
    trying = index;
    setTestCheat({ all[index] }, barMode);
    step = ONE_BY_ONE;
    host.paused = false;
}

void CheatsWindow::keep() {
    if (testIndex < 0 || testIndex >= (int)host.cheatList.cheats.size()) { step = CHOOSE; return; }
    Cheat& c = host.cheatList.cheats[(size_t)testIndex];
    c.name = nameBuf[0] ? nameBuf : "Cheat";
    testIndex = -1;
    if (!host.cheatsPath.empty()) host.saveCheats();
    step = DONE;
}

void CheatsWindow::tabWizard() {
    const float wrap = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    CheatFinder& f = host.cheatFinder;
    switch (step) {
    case CHOOSE:
        ImGui::TextUnformatted("What would you like to cheat?");
        ImGui::Spacing();
        if (ImGui::Button("A number on the screen", ImVec2(-1, 0))) { barMode = false; step = NUMBER_FIRST; seen = 3; }
        ImGui::TextDisabled("  lives, ammo, time left, credits, bombs...");
        ImGui::Spacing();
        if (ImGui::Button("A bar or a meter", ImVec2(-1, 0))) { barMode = true; step = BAR_FIRST; }
        ImGui::TextDisabled("  energy, fuel, oxygen -- something with no number to read");
        ImGui::Spacing();
        if (ImGui::Button("Type in a POKE", ImVec2(-1, 0))) step = TYPE_POKES;
        ImGui::TextDisabled("  from a magazine or a website: POKE &1234,0");
        break;
    case NUMBER_FIRST:
        ImGui::TextUnformatted("Step 1 of 3");
        ImGui::Separator();
        ImGui::TextUnformatted("Load the game and play until you can see the number. Then pause it.");
        pauseButton();
        ImGui::Spacing();
        ImGui::TextUnformatted("What does the number say now?");
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##seen", &seen);
        seen = std::clamp(seen, 0, 65535);
        ImGui::SameLine();
        if (ImGui::Button("Search")) search(seen);
        break;
    case NUMBER_AGAIN:
        ImGui::Text("Step 2 of 3 -- %d place%s could be it", (int)f.size(), f.size() == 1 ? "" : "s");
        ImGui::Separator();
        ImGui::TextUnformatted("Carry on playing until the number changes -- lose a life, fire a shot, let the clock run. Then pause again.");
        pauseButton();
        ImGui::Spacing();
        ImGui::TextUnformatted("What does it say now?");
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##seen2", &seen);
        seen = std::clamp(seen, 0, 65535);
        ImGui::SameLine();
        if (ImGui::Button("Search again")) search(seen);
        if (f.size() > 0 && f.size() <= 40) { ImGui::SameLine(); if (ImGui::Button("Show me these")) step = PICK; }
        ImGui::TextDisabled("Each round leaves fewer. Two or three are usually enough.");
        break;
    case BAR_FIRST:
        ImGui::TextUnformatted("Step 1 of 3");
        ImGui::Separator();
        ImGui::TextUnformatted("Load the game and play until you can see the bar. Pause it, then press Remember.");
        pauseButton();
        ImGui::SameLine();
        if (ImGui::Button("Remember")) remember();
        break;
    case BAR_AGAIN:
        ImGui::Text("Step 2 of 3 -- %d place%s could be it", (int)f.size(), f.size() == 1 ? "" : "s");
        ImGui::Separator();
        ImGui::TextUnformatted("Carry on playing for a moment, pause, and tell me what the bar did since last time:");
        pauseButton();
        ImGui::Spacing();
        if (ImGui::Button("It went down")) barChanged(CheatFinder::DECREASED);
        ImGui::SameLine();
        if (ImGui::Button("It went up")) barChanged(CheatFinder::INCREASED);
        ImGui::SameLine();
        if (ImGui::Button("It stayed the same")) barChanged(CheatFinder::UNCHANGED);
        ImGui::TextDisabled("Tip: a few \"stayed the same\" rounds (pause while nothing happens to it) narrow it down fast.");
        if (f.size() > 0 && f.size() <= 40) { if (ImGui::Button("Show me these")) step = PICK; }
        break;
    case PICK: {
        ImGui::Text("Step 3 of 3 -- %d place%s left", (int)f.size(), f.size() == 1 ? "" : "s");
        ImGui::Separator();
        Bytes* ram = host.ram();
        if (ImGui::BeginTable("##places", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("where");
            ImGui::TableSetupColumn("kept");
            ImGui::TableSetupColumn("now");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < f.list().size() && i < 40; i++) {
                const auto& c = f.list()[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(CheatList::addressText(c.address).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(encodingName(c.encoding));
                ImGui::TableNextColumn(); ImGui::Text("%d", ram ? f.shown(*ram, c) : 0);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (barMode) {
            ImGui::TextUnformatted("Get the bar full (or where you want it), pause, then:");
            pauseButton();
            ImGui::SameLine();
            if (ImGui::Button("Hold it where it is")) holdAll();
        } else {
            ImGui::TextUnformatted("Keep the number at:");
            ImGui::SetNextItemWidth(120);
            ImGui::InputInt("##wanted", &wanted);
            wanted = std::clamp(wanted, 0, f.width == 1 ? 255 : 65535);
            ImGui::SameLine();
            if (ImGui::Button("Hold it there")) holdAll();
        }
        break;
    }
    case TEST:
        ImGui::TextUnformatted("Trying it");
        ImGui::Separator();
        ImGui::TextUnformatted(barMode ? "Play on. Does the bar stay where it was?"
                                       : "Play on. Does the number stay put when it should change? (Some games only redraw it when it changes: lose one more to see.)");
        ImGui::Spacing();
        if (ImGui::Button("Yes, it works!")) step = NAME;
        ImGui::SameLine();
        if (ImGui::Button("No")) {
            if (f.size() > 1) tryOne(0);
            else { dropTestCheat(); step = PICK; message = "That place does not do it. Start again, or try the other way."; }
        }
        ImGui::SameLine();
        if (ImGui::Button("It went strange")) {
            // holding several places at once can break a game: try them one at a time
            if (f.size() > 1) tryOne(0);
        }
        break;
    case ONE_BY_ONE:
        ImGui::Text("Trying them one at a time: %d of %d", (int)trying + 1, (int)f.size());
        ImGui::Separator();
        ImGui::TextUnformatted(CheatList::addressText(f.list()[std::min(trying, f.size() - 1)].address).c_str());
        ImGui::TextUnformatted("Play on. Does this one hold it?");
        if (ImGui::Button("Yes, it works!")) step = NAME;
        ImGui::SameLine();
        if (ImGui::Button("No, try the next")) tryOne(trying + 1);
        break;
    case NAME:
        ImGui::TextUnformatted("Found it. Give the cheat a name:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##name", nameBuf, sizeof nameBuf);
        if (ImGui::Button("Keep this cheat")) keep();
        ImGui::TextDisabled(host.cheatsPath.empty() ? "It goes in the Cheats list (save it from there)."
                                                    : "It goes in the Cheats list, and into the game's .pok file beside it.");
        break;
    case TYPE_POKES: {
        ImGui::TextUnformatted("Type the POKEs, one or more to a line:");
        ImGui::InputTextMultiline("##pokes", pokeText, sizeof pokeText, ImVec2(-1, 120));
        ImGui::TextUnformatted("Name:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##pokename", nameBuf, sizeof nameBuf);
        if (ImGui::Button("Add it")) {
            std::vector<Cheat> found;
            std::string error;
            if (!CheatList::parse(std::string("[") + nameBuf + "]\n" + pokeText, found, error)) message = error;
            else if (found.empty()) message = "There is no POKE in that.";
            else {
                for (Cheat& c : found) { c.enabled = true; if (!c.freeze) if (Bytes* r = host.ram()) CheatList::poke(*r, c); host.cheatList.cheats.push_back(c); }
                if (!host.cheatsPath.empty()) host.saveCheats();
                pokeText[0] = 0;
                step = DONE;
            }
        }
        break;
    }
    case DONE:
        ImGui::TextColored(kGood, "Done: the cheat is on.");
        ImGui::TextUnformatted("Switch it off and on in the Cheats tab.");
        if (ImGui::Button("Find another")) restart();
        break;
    }
    if (!message.empty()) { ImGui::Spacing(); ImGui::TextColored(kNote, "%s", message.c_str()); }
    if (step != CHOOSE && step != DONE) {
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Start again")) restart();
        if ((step == NUMBER_AGAIN || step == BAR_AGAIN || step == PICK) && f.started) {
            ImGui::SameLine();
            if (ImGui::Button("Undo the last step")) { f.undo(); if (step == PICK) step = barMode ? BAR_AGAIN : NUMBER_AGAIN; message.clear(); }
        }
    }
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- the search

void CheatsWindow::tabSearch() {
    CheatFinder& f = host.cheatFinder;
    Bytes* ram = host.ram();
    if (!ram) return;
    ImGui::TextUnformatted("Size:");
    ImGui::SameLine();
    if (ImGui::RadioButton("8 bits", f.width == 1) && !f.started) f.width = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("16 bits", f.width == 2) && !f.started) f.width = 2;
    ImGui::TextUnformatted("Kept:");
    ImGui::SameLine(); ImGui::Checkbox("as it is", &encBinary);
    ImGui::SameLine(); ImGui::Checkbox("one less", &encOneLess);
    ImGui::SameLine(); ImGui::Checkbox("BCD", &encBcd);
    const unsigned enc = (encBinary ? 1u : 0u) | (encOneLess ? 2u : 0u) | (encBcd ? 4u : 0u);
    if (ImGui::Button("New search")) { const int w = f.width; f = CheatFinder{}; f.width = w; f.start(*ram, enc ? enc : 1u); selected = -1; }
    ImGui::SameLine();
    pauseButton();
    ImGui::Text("%d candidate%s", (int)f.size(), f.size() == 1 ? "" : "s");
    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##sv", &searchValue);
    ImGui::SameLine();
    if (ImGui::Button("Is this value")) { if (!f.started) f.start(*ram, enc ? enc : 1u); f.keepEqual(*ram, searchValue); }
    if (ImGui::Button("Changed")) f.keepChange(*ram, CheatFinder::CHANGED);
    ImGui::SameLine();
    if (ImGui::Button("Unchanged")) f.keepChange(*ram, CheatFinder::UNCHANGED);
    ImGui::SameLine();
    if (ImGui::Button("Went up")) f.keepChange(*ram, CheatFinder::INCREASED);
    ImGui::SameLine();
    if (ImGui::Button("Went down")) f.keepChange(*ram, CheatFinder::DECREASED);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("##sd", &searchDelta);
    ImGui::SameLine();
    if (ImGui::Button("Changed by this")) f.keepChangedBy(*ram, searchDelta);
    ImGui::SameLine();
    if (ImGui::Button("Undo")) f.undo();
    ImGui::Separator();
    if (f.size() > 2000) { ImGui::TextDisabled("Narrow it down to see them (under 2000)."); return; }
    if (ImGui::BeginTable("##found", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders, ImVec2(0, 200))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("where"); ImGui::TableSetupColumn("kept"); ImGui::TableSetupColumn("now"); ImGui::TableSetupColumn("raw");
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)f.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const auto& c = f.list()[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(CheatList::addressText(c.address).c_str(), selected == i, ImGuiSelectableFlags_SpanAllColumns)) selected = i;
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(encodingName(c.encoding));
                ImGui::TableNextColumn(); ImGui::Text("%d", f.shown(*ram, c));
                ImGui::TableNextColumn(); ImGui::Text("&%0*X", f.width * 2, f.stored(*ram, c.address));
            }
        ImGui::EndTable();
    }
    if (selected >= 0 && selected < (int)f.size()) {
        static int hold = 9;
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##hold", &hold);
        ImGui::SameLine();
        if (ImGui::Button("Hold the selected one at this")) {
            const auto& c = f.list()[(size_t)selected];
            Cheat ch;
            ch.name = "Cheat at " + CheatList::addressText(c.address);
            ch.pokes.push_back({ c.address, f.encode(c, hold), f.width });
            ch.enabled = true;
            host.cheatList.cheats.push_back(ch);
        }
    }
}

// ---------------------------------------------------------------- the list

void CheatsWindow::tabCheats() {
    auto& list = host.cheatList.cheats;
    if (list.empty()) ImGui::TextDisabled("No cheats yet: the Wizard finds them, or type POKEs in there.");
    int remove = -1;
    for (size_t i = 0; i < list.size(); i++) {
        Cheat& c = list[i];
        ImGui::PushID((int)i);
        bool on = c.enabled;
        if (ImGui::Checkbox("##on", &on)) host.enableCheat(i, on);
        ImGui::SameLine();
        ImGui::TextUnformatted(c.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", c.freeze ? "(held)" : "(once)");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) remove = (int)i;
        std::string pokes;
        for (const Poke& p : c.pokes) {
            char b[64];
            std::snprintf(b, sizeof b, "%sPOKE %s,%d", pokes.empty() ? "" : ": ", CheatList::addressText(p.address).c_str(), p.value);
            pokes += b;
        }
        ImGui::TextDisabled("    %s", pokes.c_str());
        ImGui::PopID();
    }
    if (remove >= 0) list.erase(list.begin() + remove);
    ImGui::Separator();
    if (ImGui::Button("Load...")) browser.open("Load cheats", host.mediaDir, { ".pok" }, [this](const std::string& p) { host.loadCheats(p); });
    ImGui::SameLine();
    if (ImGui::Button("Save")) host.saveCheats();
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) saver.open("Save cheats", host.mediaDir, "cheats.pok", [this](const std::string& p) { host.saveCheats(p); });
    if (!host.cheatsPath.empty()) ImGui::TextDisabled("%s", host.cheatsPath.c_str());
}

} // namespace cpcse
