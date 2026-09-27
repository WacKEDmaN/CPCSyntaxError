// CPCSyntaxError GUI — the desktop shell.
//
// A main menu bar across the top carries every option, in fly-out submenus. Every panel
// is its own window in a DockSpace over the main viewport: resizable, dockable, tabbable,
// and able to float out of the main window into an OS window of its own (Dear ImGui's
// docking branch, multi-viewport). The emulated screen is one of those windows too. The
// layout persists in cpcse_layout.ini next to the executable; Window > Reset layout
// rebuilds the default one.
//
// The menu bar and the windows are grouped the same way, by what they are about:
//   Machine     what the machine IS: model, RAM, the video chips (CRTC, Gate Array,
//               monitor set), control, ROMs
//   Media       what is in it: drives, tape (and the deck's options), cartridge, snapshots
//   Video       how the picture is presented: tube, renderer, scaling, CRT effect
//   Audio       sound, and a DAC on the printer port
//   Input       keyboard layout, joystick, mouse, lightgun, the Plus analogue port
//   Expansions  what is plugged into the back: M4, Symbiface, the printer port, GFX9000
//   Tools       CSL scripts, printer output, GFX9000 output, assembler
//   (Settings is one window holding Video, Audio, Input and Expansions)
//   Screen      the picture
//   CPU, Disassembly, Memory, Breakpoints        the debugger (gui_debugger.h)
//   Video/Audio & I/O internals, Assembler (gui_debug_views.cpp, gui_assembler.h)
// The settings sections and the Printer / GFX9000 / CSL windows are in gui_panels.cpp.
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
    // Set by main (it owns GL): put `rgba` (0xAABBGGRR, w x h) into texture `slot`,
    // creating or resizing it as needed; returns the texture id. Slot 1 GFX9000, 2 printer.
    std::function<unsigned(int slot, const uint32_t* rgba, int w, int h)> uploadTexture;
    bool quitRequested = false;
    bool mouseCaptureRequested = false;        // a click on a picture asks main to grab the mouse
    bool screenHovered = false;               // the mouse is over the emulated picture (Symbiface mouse)
    // Keys go to the CPC unless a debugger or assembler window has the focus.
    bool keyboardToCpc() const { return !toolFocused; }

private:
    friend struct GuiShellCheck;              // tools/gui_shell_check draws every section headless
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
    void menuSection(const char* title, void (GuiShell::*section)(bool));
    void menuTools();
    void menuDebug();
    void menuWindow();
    void menuHelp();

    // settings windows (gui_shell.cpp)
    void windowScreen(const ShellFrameInfo& info);
    void windowMachine();
    void windowMedia();
    void windowSettings();
    void windowAbout();
    void sectionRoms(bool asMenu);

    // settings sections and tool windows (gui_panels.cpp)
    void sectionVideo(bool asMenu);
    void sectionAudio(bool asMenu);
    void sectionInput(bool asMenu);
    void sectionExpansions(bool asMenu);
    void sectionTape(bool asMenu);
    void printerPortItems(bool asMenu);
    void lightgunItems(bool asMenu);
    void windowPrinter();
    void windowGfx9000();
    void windowCslScripts();
    // the lightgun: where the mouse is over the picture, in framebuffer pixels
    void lightgunFromScreen(float ox, float oy, float drawW, float drawH, int texW, int texH);

    // printer / GFX9000 textures
    unsigned printerTexture = 0, gfxTexture = 0;
    long long gfxTextureFields = -1;
    unsigned video9000Texture = 0;
    std::vector<uint32_t> video9000Pixels;
    // The GFX9000's last field in gfxTexture (uploaded when a new one is complete); its
    // picture, or null while its monitor has no signal.
    const V9990Picture* gfxPictureTexture();
    // draws the GFX9000 monitor into a box: its picture, or "no signal"
    void drawGfxMonitor(ImDrawList* dl, float x, float y, float w, float h);
    int printerTextureRevision = -1;
    std::string printerTabDevice;     // the printer-port device the Printer window last showed
    // CSL scripts window
    std::string cslScript, cslOut = "screenshots", cslDiskDir;
    int cslCrtc = -1;                 // -1 = as the script says
    bool cslChain = true, cslErrata = true;
    struct CslRun;
    std::shared_ptr<CslRun> cslRun;

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
