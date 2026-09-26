// CPCSyntaxError GUI — the desktop shell.
//
// A main menu bar across the top carries every option, in fly-out submenus. Every panel
// is its own window in a DockSpace over the main viewport: resizable, dockable, tabbable,
// and able to float out of the main window into an OS window of its own (Dear ImGui's
// docking branch, multi-viewport). The emulated screen is one of those windows too. The
// layout persists in cpcse_layout.ini next to the executable; Window > Reset layout
// rebuilds the default one.
//
// The windows, grouped by what they are about:
//   Machine   what the machine IS: model, RAM, CRTC, control, ROMs
//   Media     what is in it: drives, tape, cartridge, snapshots
//   Settings  how it is presented: display, sound, input, expansions
//   Screen    the picture
//   CPU, Disassembly, Memory, Breakpoints        the debugger (gui_debugger.h)
//   Video     CRTC, Gate Array, monitor, Plus ASIC internals
//   Audio & I/O  PSG, PPI + keyboard matrix, disc controller, tape
//   Assembler RASM (gui_assembler.h)
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "emuhost.h"
#include "gui_debugger.h"
#include "gui_widgets.h"

namespace cpcse {

class AssemblerWindow;

// What the main loop hands the shell each frame.
struct ShellFrameInfo {
    unsigned screenTexture = 0;   // GL texture holding the emulated screen
    int textureWidth = 0, textureHeight = 0;
    float emuSpeedPct = 0.0f;     // emulated frames vs real time
    float uiFramerate = 0.0f;
    bool soundOpen = false;       // an audio device exists
    float soundQueuedMs = 0.0f;
};

class GuiShell {
public:
    explicit GuiShell(EmuHost& host);
    ~GuiShell();

    void applyStyle();
    void loadSettings(const std::map<std::string, std::string>& ini);
    void saveSettings(std::ostream& out) const;

    // The whole UI for one frame: menu bar, dockspace, every open window, status bar.
    void draw(const ShellFrameInfo& info);

    std::function<void()> toggleFullscreen;   // set by main (it owns the SDL window)
    bool quitRequested = false;
    bool screenHovered = false;               // the mouse is over the emulated picture (Symbiface mouse)
    // Keys go to the CPC unless a debugger or assembler window has the focus.
    bool keyboardToCpc() const { return !toolFocused; }

private:
    EmuHost& host;
    FileBrowser browser;
    SaveDialog saver;
    Debugger debugger;
    std::unique_ptr<AssemblerWindow> assembler;
    bool resetLayout = false;
    bool showStatusBar = true;
    bool showAbout = false;
    bool showImGuiDemo = false;
    bool toolFocused = false, toolFocusedNow = false;

    // Every window the shell can show, by its title (which is also its dock identity).
    struct Panel { const char* title; const char* iniKey; bool open; };
    std::vector<Panel> panels;
    bool& panelOpen(const char* title);
    bool beginTool(const char* title, ImGuiWindowFlags flags = 0);   // Begin() for a debugger window

    // debugger view state
    int disasmTop = 0;
    bool disasmFollow = true;
    int disasmLastPc = -1;
    char disasmGoto[64] = {0};
    int memView = 0;                 // 0 CPU view, 1 physical RAM, 2 ASIC page
    int memTop = 0, memSelected = -1, memEditing = -1;
    bool memFollow = false, memScrollTo = false;
    char memGoto[64] = {0};
    char memEdit[8] = {0};
    char bpAddress[64] = {0}, bpCondition[128] = {0};
    char wpStart[64] = {0}, wpEnd[64] = {0};
    bool wpRead = false, wpWrite = true;

    void drawMenuBar();
    void drawStatusBar(const ShellFrameInfo& info);
    void buildDefaultLayout(unsigned dockspaceId);
    void handleShortcuts();

    // menus
    void menuFile();
    void menuMachine();
    void menuMedia();
    void menuSettings();
    void menuDebug();
    void menuWindow();
    void menuHelp();

    // settings windows (gui_shell.cpp)
    void windowScreen(const ShellFrameInfo& info);
    void windowMachine();
    void windowMedia();
    void windowSettings();
    void windowAbout();
    void sectionDisplay(bool asMenu);
    void sectionSound(bool asMenu);
    void sectionInput(bool asMenu);
    void sectionExpansions(bool asMenu);
    void sectionRoms(bool asMenu);

    // debugger windows (gui_debug_views.cpp)
    void windowCpu();
    void windowDisassembly();
    void windowMemory();
    void windowBreakpoints();
    void windowVideo();
    void windowAudioIo();
    void debugToolbar();
    void showInDisassembly(int address);
    void showInMemory(int address);
    int disasmPrevious(int address);

    // shared widgets
    void modelItems(bool asMenu);
    void crtcItems(bool asMenu);
    void gateArrayItems(bool asMenu);
    void ramItems(bool asMenu);
    void monitorSetItems(bool asMenu);
};

} // namespace cpcse
