// CPCSyntaxError GUI — cheats: a walkthrough that finds a game's lives (or energy, or time)
// and makes them last, a search for those who know what they are after, and the list of
// cheats (POKEs) with their .pok file.
//
// The walkthrough asks only what the player can see: what the number on the screen says,
// and again after it has changed -- or, for a bar, whether it went down, up or stayed. The
// finder (core/cheats.h) tries every way a game might keep it at once; when a few places are
// left, it holds them and asks whether the number stayed, then tries them one at a time.
#pragma once
#include <string>
#include <vector>

#include "emuhost.h"
#include "gui_widgets.h"

namespace cpcse {

class CheatsWindow {
public:
    CheatsWindow(EmuHost& host, FileBrowser& browser, SaveDialog& saver);
    void draw(bool* open);
    bool focused = false;

    // the walkthrough's steps (public: cpcse-gui-shell-check walks it)
    enum Step { CHOOSE, NUMBER_FIRST, NUMBER_AGAIN, BAR_FIRST, BAR_AGAIN, PICK, TEST, ONE_BY_ONE, NAME, TYPE_POKES, DONE };
    Step step = CHOOSE;
    bool barMode = false;
    int seen = 0, wanted = 0;            // the number the player read; the one to hold it at
    int rounds = 0;
    size_t trying = 0;                   // ONE_BY_ONE: which place
    std::string message;
    char nameBuf[48] = "Infinite lives";
    char pokeText[2048] = "";

    void search(int value);              // NUMBER_FIRST / NUMBER_AGAIN
    void remember();                     // BAR_FIRST
    void barChanged(CheatFinder::Change change);
    void holdAll();                      // PICK -> TEST
    void tryOne(size_t index);           // ONE_BY_ONE
    void keep();                         // NAME -> DONE
    void restart();

private:
    EmuHost& host;
    FileBrowser& browser;
    SaveDialog& saver;
    int testIndex = -1;                  // the cheat being tried, in the host's list
    // search tab
    int searchValue = 0, searchDelta = 1;
    bool encBinary = true, encOneLess = false, encBcd = false;
    int selected = -1;

    void tabWizard();
    void tabSearch();
    void tabCheats();
    void setTestCheat(const std::vector<CheatFinder::Candidate>& places, bool atCurrentValue);
    void dropTestCheat();
    bool pauseButton();
};

} // namespace cpcse
