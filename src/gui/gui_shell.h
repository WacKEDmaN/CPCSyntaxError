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
#include <set>
#include <string>
#include <vector>

#include "emuhost.h"
#include "gui_debugger.h"
#include "gui_widgets.h"
#include "core/profiler.h"

namespace cpcse {

class AssemblerWindow;
class BasicWindow;
class DskEditorWindow;
class CheatsWindow;
class LogicAnalyserWindow;
class DevServer;

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

    // The look: Dear ImGui's dark theme, square windows, the orange accent; DejaVu Sans
    // Mono (embedded) at 15 px times uiScale, and room between things.
    void applyStyle();
    float uiScale = 1.0f;
    void loadSettings(const std::map<std::string, std::string>& ini);
    void saveSettings(std::ostream& out) const;

    // The whole UI for one frame: menu bar, dockspace, every open window, status bar.
    void draw(const ShellFrameInfo& info);

    std::function<void()> toggleFullscreen;   // set by main (it owns the SDL window)
    // Set by main (it owns GL): put `rgba` (0xAABBGGRR, w x h) into texture `slot`,
    // creating or resizing it as needed; returns the texture id. Slot 1 GFX9000, 2 printer.
    std::function<unsigned(int slot, const uint32_t* rgba, int w, int h)> uploadTexture;
    // Set by main (it owns GL): a draw callback after which draws ADD their light (the CRT
    // effect's bloom); ImDrawCallback_ResetRenderState puts the usual blending back. Null:
    // the bloom blends as other draws do.
    ImDrawCallback additiveBlend = nullptr;
    // Set by main (it owns GL): the emulated screen through the advanced CRT filter, into a
    // texture of outW x outH pixels; newFrame moves its rolling lines on. 0: not drawn
    // (the plain picture is drawn instead).
    std::function<unsigned(int outW, int outH, float pixelScale, bool newFrame)> crtFilterRender;
    uint64_t crtFilterFrame = ~0ull;   // the emulated frame the filter last drew
    // ...and one after which draws MULTIPLY what is under them (the CRT effect's slot mask).

    bool quitRequested = false;
    bool mouseCaptureRequested = false;        // a click on a picture asks main to grab the mouse
    bool screenHovered = false;               // the mouse is over the emulated picture (Symbiface mouse)
    // Keys go to the CPC unless a debugger or assembler window has the focus.
    bool keyboardToCpc() const { return !toolFocused; }
    // Where the Screen window's picture was drawn last (main-viewport coordinates, the
    // picture and a GFX9000 beside it), and whether in the main window: the recorder's
    // "screen" source reads it back after the frame is drawn.
    float screenRect[4] = { 0, 0, 0, 0 };
    bool screenRectMain = false;
    // F11: the GFX9000 screen's full screen while it has the focus, nothing while a tool
    // window has the keys (the assembler's F11 steps a line), else the main window's.
    bool fullscreenPictureOnly = true;
    bool gfxScreenFull = false, gfxScreenFocused = false;
    void f11();
    // External development (devserver.h): the GDB server, the command API, file watching.
    // main() polls it every iteration.
    DevServer& devServer() { return *dev; }

private:
    friend struct GuiShellCheck;              // tools/gui_shell_check draws every section headless
    EmuHost& host;
    FileBrowser browser;
    SaveDialog saver;
    Debugger debugger;
    std::unique_ptr<AssemblerWindow> assembler;
    std::unique_ptr<BasicWindow> basicWindow;
    std::unique_ptr<DskEditorWindow> dskEditor;
    std::unique_ptr<CheatsWindow> cheats;
    std::unique_ptr<LogicAnalyserWindow> logicAnalyser;
    std::unique_ptr<DevServer> dev;
    void sectionDevelopment(bool asMenu);   // Settings > External debugging (gui_panels.cpp)
    char devWatchPath[512] = {}, devWatchAddr[32] = {}, devWatchRun[32] = {}, devWatchCommand[128] = {};
    bool devWatchReset = false;
    bool resetLayout = false;
    bool showStatusBar = true;
    bool showAbout = false;
    bool showImGuiDemo = false;
    bool toolFocused = false, toolFocusedNow = false;

    // Every window the shell can show, by its title (which is also its dock identity).
    struct Panel { const char* title; const char* iniKey; bool open; bool byDefault; };
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
    // A window opened for the first time joins its group: the debugging windows to the
    // right of the screen, the tools below it.
    void placeNewlyOpened(unsigned dockspaceId);
    std::vector<std::string> placedThisSession;
    unsigned groupNode[4] = { 0, 0, 0, 0 };   // the dock node each group opened into
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
    void screenPicture(const ShellFrameInfo& info);   // the Screen window's inside: the picture and its effects
    // Full screen with the picture alone (fullscreenPictureOnly): one borderless window over
    // the whole display, no menus, panels or status bar; F11 or a double-click goes back.
    void drawFullscreenPicture(const ShellFrameInfo& info);
    double fullscreenHintUntil = 0.0;
    bool fullscreenWas = false;
    void windowMachine();
    void windowMedia();                 // the media windows (MEDIA_WINDOWS), each dockable on its own
    // What is in the machine: a window each, tabbed together by default where "Media" was.
    static constexpr const char* MEDIA_WINDOWS[] = { "Discs", "Tape", "Cartridge", "Snapshot" };
    void windowSettings();
    void openAllSettings();
    // The settings sections' windows, in windowSettings()'s order.
    static constexpr const char* SETTINGS_WINDOWS[] = {
        "Video settings", "Audio settings", "Input settings", "Expansions", "External debugging" };
    // A window to bring to the front as soon as it exists (it may be opened this frame).
    const char* focusWindowNextFrame = nullptr;
    void windowAbout();
    void sectionRoms(bool asMenu);
    // the quick save slots: a grid of their pictures (the Snapshot window), or menu items
    void quickSlots(bool asMenu, bool save);
    unsigned slotTexture[EmuHost::SLOT_COUNT + 1] = {};
    long slotTextureRevision[EmuHost::SLOT_COUNT + 1] = {};
    unsigned slotPicture(int slot);           // its picture's texture, or 0
    void recentFilesMenu();
    void recordMenu();
    void startRecordingDialog(bool video);

    // settings sections and tool windows (gui_panels.cpp)
    void sectionVideo(bool asMenu);
    void crtFilterItems(bool asMenu);   // the advanced CRT filter's settings, in Video
    void sectionAudio(bool asMenu);
    void sectionInput(bool asMenu);
    void joystickDeviceItems(int port, bool asMenu);
    void joystickSettings();
    void sectionExpansions(bool asMenu);
    void sectionTape(bool asMenu);
    void tapeDeckControls(bool asMenu);       // counter, time, block, motor, transport buttons
    void printerPortItems(bool asMenu);
    void lightgunItems(bool asMenu);
    void windowPrinter();
    void windowSf3Oled();
    void windowGfx9000();
    // The GFX9000's picture alone, in a window of its own that can float onto another
    // display; its own full screen covers the display it is on, borderless, on top.
    void windowGfxScreen();
    int gfxScreenMonitor = -1;
    // its place and size before full screen, put back when it leaves (once)
    float gfxRestore[4] = { 0, 0, 0, 0 };
    bool gfxRestorePending = false;
public:
    void toggleGfxScreenFull();
private:
    void windowCslScripts();
    // A ROM a device needs and the ROM folder lacks: fetch it (a URL) or choose it.
    void romPrompt();
    std::string romPromptFor;          // "opl4" / "m4" / "sp0256" while the prompt is up
    std::string romChooseStatus;       // a chosen file that would not fit, for the prompt to show
    void romInstalled(const std::string& key);
    char romUrl[512] = {};
    struct RomFetch;
    std::shared_ptr<RomFetch> romFetch;
    // the lightgun: where the mouse is over the picture, in framebuffer pixels
    void lightgunFromScreen(float ox, float oy, float drawW, float drawH, int texW, int texH);

    // printer / GFX9000 textures
    unsigned printerTexture = 0, gfxTexture = 0;
    unsigned maskTexture = 0;           // the picture through the slot mask (texture slot 4)
    std::vector<uint32_t> maskPixels;
    // made again only for a new emulated frame, a new size or a new setting
    struct MaskKey {
        uint64_t frame = ~0ull; int w = 0, h = 0; float strength = -1; int stripe = 0; bool turned = false; float blur = 0; bool soft = false;
        bool operator==(const MaskKey& o) const {
            return frame == o.frame && w == o.w && h == o.h && strength == o.strength && stripe == o.stripe && turned == o.turned && blur == o.blur
                && soft == o.soft;
        }
    };
    MaskKey maskKey;
    // the picture softened (CRT effect, blur), once a frame
    std::vector<uint32_t> blurPixels;
    unsigned blurTexture = 0;
    uint64_t blurFrame = ~0ull;
    float blurAmount = -1;
    unsigned logoTexture = 0;        // the logo (tools/make_logo.py, style A), uploaded once
    void drawLogo();
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

    // the code profiler (gui_profiler.cpp)
    void windowProfiler();
    CodeProfiler profiler;
    bool profilerRecording = false;
    std::vector<uint32_t> profilerMap;
    unsigned profilerTexture = 0;

    // the on-screen CPC keyboard (gui_keyboard.cpp)
    void windowCpcKeyboard();
    int keyboardLook = 0;                     // 0 as the machine; 1 464, 2 664, 3 6128, 4 Plus
    int keyboardHeld = -1;                    // row << 3 | bit held by the mouse
    std::set<int> keyboardLatched;            // held by a right click

    // the graphics finder (gui_gfx_finder.cpp)
    void windowGraphicsFinder();
    struct GfxFinder {
        int source = 0, lastSource = 0, format = 1, palette = 0;
        int address = 0xc000, widthBytes = 80, height = 200, zoom = 2;
        bool screenLayout = true, cpcShape = true, grid = true;
        int selected = -1;
    } gfx;
    std::vector<uint32_t> gfxPixels;
    unsigned gfxFinderTexture = 0;
    int gfxRead(int address) const;
    int gfxAddress(int start, int x, int y) const;
    uint32_t gfxInk(int pen) const;

    // debugger windows (gui_debug_views.cpp)
    void windowDebugger();          // toolbar, registers, disassembly, breakpoints
    void windowChips();             // CRTC, Gate Array, monitor, ASIC, PSG, PPI, keyboard, disc, tape
    void windowMemory();            // hex editor, memory map
    void cpuContent();
    void disassemblyContent();
    void breakpointsContent();
    void memoryHexContent();
    void chipTabsVideo();
    void chipTabsIo();
    float debuggerTopH = -1;
    int memTabRequest = -1;         // 0 Hex, 1 Map: brought to the front, then -1
    bool memMapShown = false;       // the Map tab was drawn (it records only then)
    void debugToolbar();
    // Memory map + GFX9000 internals (gui_memory_views.cpp)
    void memoryMapContent();
    void gfxInternalsTabs();
    int memMapChunk = 0;
    bool memMapRecord = true, memMapLive = false, memMapWrittenCode = false;
    std::vector<uint32_t> memMapPixels;
    int gfxVramZoom = 1;
    int gfxInternalsTab = -1;        // a tab to bring to the front (then -1)
    char gfxVramGoto[16] = {0};
    std::vector<uint32_t> gfxVramPixels;
    void showInDisassembly(int address);
    void showInMemory(int address);
    int disasmPrevious(int address);

    // shared widgets
    void modelItems(bool asMenu);
    void crtcItems(bool asMenu);
    void gateArrayItems(bool asMenu);
    void z80Items(bool asMenu);
    void ramItems(bool asMenu);
    void monitorSetItems(bool asMenu);
    void monitorSyncItems();
    void monitorFrontItems();
};

} // namespace cpcse
