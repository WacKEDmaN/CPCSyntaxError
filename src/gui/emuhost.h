// CPCSyntaxError GUI — emulator host.
//
// Wraps the headless GX4000 core with everything a desktop front end needs:
// firmware/ROM discovery, media loading (cartridge / disk / tape / snapshot),
// framing, audio draining, host-key translation and a small pile of settings
// the UI edits directly. It owns exactly one GX4000 and one CpcVideo; changing
// model/RAM/CRTC reconfigures the same machine via loadClassicFirmware().
#pragma once
#include <SDL.h>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "core/common.h"
#include "core/cheats.h"
#include "crt_filter.h"
#include "core/timeline.h"
#include "joypads.h"
#include <deque>
#include "core/monitor_palette.h"
#include "core/media_recorder.h"

namespace cpcse {

class GX4000;
class CPCTapeDrive;
struct Disk;
class CpcVideo;
class MatrixPrinter;
struct V9990Picture;
class HostSerial;

struct ModelProfile {
    std::string id;
    std::string label;
    std::string osKey;      // case-insensitive filename keyword for the OS ROM
    std::string basicKey;   // …and the BASIC ROM
    bool amsdos = false;    // needs AMSDOS.ROM + a floppy controller
    int defaultRam = 64;
    int defaultCrtc = 1;
    bool plus = false;         // a CPC Plus: boots from a system cartridge (ASIC on)
    std::string cartKey;       // keyword to locate the Plus system cartridge (.cpr)
    bool available = false;    // ROMs / cartridge were found
    bool computer = true;      // true = has a keyboard (464/6128 + Plus); false = GX4000 console
};

class EmuHost {
public:
    GX4000* emu = nullptr;
    CpcVideo* video = nullptr;

    // configuration the UI edits
    std::string romDir = "roms";
    std::string mediaDir = "media";   // where cartridges (incl. the Plus system cart) live
    std::vector<ModelProfile> models;
    int currentModel = -1;
    int ramKiB = 64;
    int crtcType = 1;
    // WHICH MONITOR IS PLUGGED IN (src/core/monitor_model.h), by id -- "" means the set
    // the machine shipped with, which is the only pairing ACCC 15.1 calls centred.
    std::string monitorSetId;
    // THE HOLD CONTROLS on the back of the set (CtmMonitor::hHold, vHold): H. Hold in
    // Pixel-M2 per line, V. Hold in lines. 0 is the set as calibrated.
    int monitorHHold = 0;
    int monitorVHold = 0;
    // THE SET'S FRONT CONTROLS (CpcVideo::setMonitorKnobs): Brightness and Contrast. Its
    // intensity field is not used here: the output intensity is kept PER SET below.
    TubeKnobs monitorKnobs;
    // R, G, B output intensity (0..200 %) for each monitor set, by monitor_model.h id, to
    // match a real monitor. A set not listed is at 100 %.
    std::map<std::string, std::array<int, 3>> monitorIntensity;
    std::string activeMonitorSetId() const;          // the set plugged in now
    std::array<int, 3> activeMonitorIntensity() const;
    void setMonitorIntensity(const std::array<int, 3>& rgb);   // for the set plugged in now
    // WHICH GATE ARRAY IS FITTED (src/core/gate_array_model.h): 40007, 40008 or 40010 on a
    // classic machine -- ACCC 9 (p.46) names all three in 464s and 6128s. 0 is the 40010,
    // the part most 6128s carry. On CRTC 3/4 there is no choice: the ASIC (40489/40226)
    // IS the GATE ARRAY, so this is ignored there.
    int gateArrayPart = 0;
    // WHICH Z80 IS FITTED (Z80::cmos): false is the NMOS Z80A that every CPC shipped with;
    // true is a CMOS Z84C00 as fitted in some repaired machines.
    bool z80Cmos = false;
    // ...and an explicit tube override for looking at a colour program in green or grey.
    // "" takes the tube from the set, which is the physical answer.
    std::string monitorMode;
    bool paused = false;
    bool audioEnabled = true;
    // The machine's mechanical noises (core/machine_sounds.h): the disc drive's motor,
    // head steps and insert, and the keyboard's clicks. Not the CPC's sound chip.
    bool driveSounds = true;
    bool keySounds = false;
    float mechanicsVolume = 0.5f;
    std::array<bool, 512> keyHeld{};   // SDL scancodes down, so a key repeat does not click
    float masterVolume = 0.6f;
    // Each sound source's own level under the master volume, 0..2 (1 = as the device puts it
    // out): the CPC's AY, the printer-port DAC, the speech synthesisers, LambdaSpeak 3's MP3
    // module, the SYMBiFACE III's sound card, the PlayCity and the OPL4.
    float volAy = 1, volDac = 1, volSpeech = 1, volLs3Mp3 = 1, volSf3 = 1, volPlayCity = 1, volOpl4 = 1;
    struct SoundLevel { const char* key; const char* name; float EmuHost::* level; };
    static const std::vector<SoundLevel>& soundLevels();
    bool soundFitted(const char* key) const;   // is that source in the machine now
    int sampleRate = 44100;

    // input / expansion options
    bool joystickEnabled = true;
    std::string keyboardRegion = "uk";      // uk / fr / es
    // "cpc": each host key is the CPC key in its place (the CPC layout); "pc": a key that
    // types a character presses the CPC key that types it on this CPC keyboard (keyboardRegion)
    std::string keyboardMapping = "cpc";
    void setKeyChar(SDL_Scancode sc, const std::string& text);   // the PC layout: the key that typed `text`
    // the host's joysticks and joypads (joypads.h), polled into the CPC's every UI frame
    JoypadManager pads;
    void pollPads();
    // THE PRINTER PORT holds one device at a time: none / digiblaster / amdrum (sound DACs)
    // / printer (a text printer) / matrix (an Epson-style dot-matrix printer, page image).
    std::string dacType = "none";
    // THE JOYSTICK PORT'S LIGHTGUN: none / trojan (Trojan Light Phazer, on the CRTC's light
    // pen input) / gunstick (Loriciel Gunstick) / westphaser (Loriciel West Phaser). Aimed
    // with the mouse over the Screen window, left button to fire.
    std::string lightgunType = "none";
    // GFX9000: the Yamaha V9990 cartridge at &FF60. Its picture goes to its own monitor --
    // the GFX9000 window.
    bool v9990Enabled = false;
    // AN OPL4 SOUND CARD on the AMSDAP (YMF278B: OPL3 FM + wavetable, &FFC4-7/&FF7E-F),
    // MoonSound-style. Its General MIDI samples are Yamaha's YRW801 ROM (yrw801*.rom in
    // the ROM folder); without it the wavetable plays only what programs load into RAM.
    bool opl4Enabled = false;
    int opl4RamKiB = 2048;
    bool playCityEnabled = false;      // TotO's PlayCity (two YMZ294 + a Z80 CTC)
    std::string speechKind = "none";   // "none" / "ssa1" / "dktronics" / "lambdaspeak3" (core/speech.h)
    std::string mp3Card;               // LambdaSpeak 3's MP3 module: its micro-SD card folder
    bool opl4HasRom() const;
    bool m4HasRom() const { return !findRom("m4rom").empty() || !findRom("m4").empty(); }
    // Copies a ROM file into the ROM folder as destName, after checking its size; the
    // reason on failure.
    bool installRom(const std::string& source, const std::string& destName, size_t size, std::string& why);
    // Where its monitor is: "beside" the CPC's in the Screen window, in a "window" of its
    // own, or "switch" -- one monitor, showing the GFX9000's picture while it displays one
    // and the CPC's otherwise, as a Video9000 passes the computer's video through.
    std::string gfx9000Monitor = "switch";   // + "video9000": one monitor through a Video9000
    // One monitor, and the GFX9000 has it now: its display on, and no program has handed
    // the picture back through P#F (&FF6F bit 4, as Video9000 programs do on exit).
    bool gfx9000OnMainScreen() const;
    // THE VIDEO9000 (Sunrise; its manual, docs/reference/Video9000-manual.pdf, ch.5): the
    // GFX9000's genlock and superimposer, its control register at &FF6F --
    //   b6 S1, b5 S0  input: 0x = RGB (the computer's own picture), 10 = CVBS, 11 = S-VHS
    //   b4 GEN        genlock on (off: the GFX9000's picture alone)
    //   b3 TRAN       0: the GFX9000 wholly transparent (the input alone); 1: where its YS
    //                 bits say (R#8 YSE on, palette R bit 7 / BD16 bit 15 / BD8 dot 0)
    //   b1 YMIX       the two mixed, each at half brightness (appendix B, MIX)
    //   b0 YM         half-tone: the input at half brightness where it shows through
    // At power-on &10 (p.13): genlock on, the GFX9000 transparent -- the CPC's picture.
    // The monitor's picture, at the GFX9000's own geometry (false: nothing to compose).
    bool video9000Picture(std::vector<uint32_t>& out, int& width, int& height) const;
    int video9000Control() const;
    // The host mouse is grabbed for the CPC's mouse (click the picture; F12 releases).
    bool mouseCaptured = false;
    // The tape deck: whether the tape follows the cassette motor relay (as on a CPC), and
    // the relay's start-up delay.
    bool tapeFollowsMotor = true;
    bool tapeRelayDelay = true;
    // Fast loading (CPCTapeDrive::fastLoad): the blocks only the firmware reads play at
    // tapeFastBaud; the others (custom and protected loaders) at their own speed.
    bool tapeFastLoad = true;
    int tapeFastBaud = 3000;
    // Instant loading (CPCTapeDrive::instantLoad): the firmware's records straight into memory.
    bool tapeInstantLoad = false;

    // speed / display preferences (read by the shell)
    float speed = 1.0f;                     // 0.25 .. 4.0
    bool turbo = false;                      // run uncapped
    // Tape turbo: while the tape is moving (PLAY or REC down, the motor on) the whole machine
    // runs this many times faster -- the loader sees nothing different, only the clock on the
    // wall does. 1 = off; TAPE_TURBO_UNLIMITED = as fast as this computer goes.
    static constexpr int TAPE_TURBO_UNLIMITED = 21;
    int tapeTurbo = 1;
    bool tapeTurboActive() const;
    bool fullscreen = false;
    // How each screen is shown (crt_filter.h): the CPC's monitor and the GFX9000's have
    // their own scaling, CRT effect and advanced CRT filter. The GFX9000's starts as a copy
    // of the CPC's, until it is set.
    ScreenLook cpcLook, gfxLook;
    bool crtSlotMask() const;            // the CPC's pattern, "auto" as its set's tube
    bool crtScanlines() const;
    bool crtFilterActive() const { return cpcLook.crtFilterActive(); }

    // media state (for display)
    std::string diskName[2];
    std::string tapeName;
    std::string cartName;
    std::string cartPath;                  // the inserted cartridge's file ("" = none)
    // A CRO file (core/cpr_loader.h) holds groups of ROMs. A group of bankable ROMs is a
    // Plus cartridge; a group of low / high / Multiface II ROMs fits a classic CPC: its OS,
    // its upper ROM slots (RLOG) and the Multiface's ROM, until the next boot or eject.
    // croGroup picks a group by its GNUM (-1: GNUM 0, else the first).
    int croGroup = -1;
    std::vector<std::string> croGroupLabels;   // the loaded CRO's groups ("GNUM: label")
    std::vector<uint32_t> croGroupNumbers;
    uint32_t croCurrentGroup = 0;              // the GNUM of the group running now
    struct CroRomSet { Bytes lower; std::vector<std::pair<int, Bytes>> upper; Bytes mf2; };
    CroRomSet croSet;
    bool loadCroGroup(uint32_t number);        // the same file, another group
    std::string croGroupPath;                  // the file croGroup was picked for ("" = any: --cro-group)
    bool croAskGroup = false;                  // a CRO of several groups loaded with none picked: the GUI asks
    // A cartridge (CPR, CRO, raw) from bytes: the CRO editor runs what it holds. `path` names
    // it ("" for one not saved yet).
    bool loadCartridgeData(const Bytes& data, const std::string& path);
    // THE GX4000 128K RAM MOD: a GX4000 has 64K; with the mod it banks 128K like a 6128
    // (the 6128 Plus's banking), so 128K software runs. Changing it restarts the cartridge.
    bool gx4000Ram128 = false;
    void setGx4000Ram128(bool on);
    // A PLUS'S RAM BEYOND ITS OWN (GXMemory::classicExpansion): a classic board on the
    // expansion port (true: writes under the ASIC's page reach its RAM too, as on the real
    // machine) or an internal upgrade (false: it behaves as the Plus's own RAM). Changing it
    // restarts a Plus.
    bool plusExpansionClassic = true;
    void setPlusExpansionClassic(bool classic);
    std::string snapshotName;               // the .SNA last loaded ("" once the machine reboots)

    // firmware ROM overrides (empty = auto-find by keyword). Let the user see
    // exactly which files are installed and swap them.
    std::string osRomPath, basicRomPath, amsdosRomPath;   // override full paths
    std::string osRomName, basicRomName, amsdosRomName;    // resolved filenames (display)

    // expansion ROMs fitted into upper-ROM slots (persist across reboots)
    struct FittedRom { int slot; std::string name; Bytes data; std::string path; };   // path: kept in cpcse.ini
    std::vector<FittedRom> expansionRoms;

    // M4 board (SD storage backed by a host folder) — classic CPC only.
    bool m4Enabled = false;
    std::string m4Folder;
    bool m4Network = true;       // its WiFi is the host's network (off: no network at all)
    bool m4NetworkLan = false;   // programs that listen take connections from other computers
    void applyM4Network();
    // Symbiface II / III: mouse, RTC and the IDE/CF interface.
    // Amstrad's RS232C serial interface (DART + 8253), its line plugged into the host:
    // serialKind "tcp" (serialTarget host:port), "listen" (a port), "com" (COM3,
    // /dev/ttyUSB0) or "loopback". Its ROM, if wanted, goes in a ROM slot like any other.
    bool serialEnabled = false;
    std::string serialKind = "listen", serialTarget = "2323";
    bool serialLan = false;                  // "listen": from other computers too
    bool setSerial(bool on);                 // fit/remove the card, (re)connecting its line
    bool connectSerial();                    // the line again, with the current kind/target
    std::string serialStatus() const;
    std::shared_ptr<HostSerial> serialLink;

    // Romantic Robot's Multiface II (its ROM from roms/, not shipped); F10 is STOP.
    bool multifaceEnabled = false;
    bool setMultiface(bool on);
    void multifaceStop();
    std::string multifaceRom() const;
    std::string symbifaceModule = "none";   // none / sf2 / sf3
    // Both cards' IDE/CF interface: this folder is its disc ("symide" beside the program,
    // set by main). Made when the card is turned on and the folder is not there yet.
    std::string ideFolder;
    // The SYMBiFACE III's USB stick (drive 1:) and SD card (drive 0:) for its SE-ONE player
    // and samples: folders on this computer ("" = none).
    std::string sf3UsbFolder, sf3SdFolder;
    std::string ls3EepromFile;       // LambdaSpeak 3's 128 KB EEPROM, kept between sessions ("" = not kept)
    void setIdeFolder(const std::string& folder);
    // The Dobbertin HD20's image file ("" = none: then a SYMBiFACE III's USB stick
    // HD20/HD20_01.IMG, as the real SF3 finds it).
    std::string hd20Image;
    // The SYMBiFACE III's ROM bank: SD card CPC/ROMTABLE/BANKnnn, its ROMnnn.BIN in upper ROM
    // slot nnn (ROM127 the lower ROM), loaded at power-on as the real SF3 does (the folder's
    // README.TXT). 0 = none, -1 = automatic: the bank TMTLOGIC's card has for this machine
    // (2 "CUBE 6128", 3 "CUBE 6128 PLUS", 6 "CUBE 464 v2").
    int sf3RomBank = -1;
    int sf3BankInUse() const;         // the bank number used now (0 = none)
    struct Sf3Bank { int number = 0; std::string name; };
    std::vector<Sf3Bank> sf3Banks() const;
    void setSf3RomBank(int bank);
    bool applySf3Roms();              // true when the ROMs changed (the CPC must restart to see them)
    void showSf3Bank();               // the bank's name on the SF3's OLED
    std::vector<int> sf3RomSlots;     // the slots the bank filled
    int sf3BankApplied = 0;           // the bank whose ROMs are in (0 = none)
    Bytes sf3LowerSaved;              // the lower ROM a ROM127 replaced
    void setHd20Image(const std::string& image);
    void applyHd20();
    void setSf3Folder(bool usb, const std::string& folder);   // the USB stick (true) or the SD card
    void rescanIde();
    float mouseSensitivity = 1.0f;

    // last operation status line for the UI
    std::string status = "No machine booted.";

    // drained audio for the current frame (interleaved stereo S16)
    std::vector<int16_t> audioOut;

    EmuHost();
    ~EmuHost();

    void setRomDir(const std::string& dir);
    void scanModels();                       // mark which profiles have ROMs
    bool bootModel(int index, int ram = -1, int crtc = -1);
    bool booted() const { return currentModel >= 0 || !cartName.empty(); }

    bool loadCartridgeFile(const std::string& path);
    bool loadDiskFile(const std::string& path, int unit);
    void ejectDisk(int unit);
    // The DSK editor's disc into drive A (0) or B (1), shared: what the CPC writes, the
    // editor sees, and the other way round.
    bool insertDisk(std::shared_ptr<Disk> disk, int unit, const std::string& name);
    std::shared_ptr<Disk> driveDisk(int unit) const;
    // Where the disc in a drive came from (empty for one made in the editor).
    std::string diskPath[2];
    // What a program writes to a disc goes back into its image file (the GUI's setting,
    // diskwriteback; off for headless and CSL runs, so test media never change): once the
    // drive has been idle a second, and on eject, insert, boot and exit. A disc with no
    // file (made in the DSK editor) stays the editor's to save.
    bool diskWriteBack = false;
    bool diskDirty[2] = { false, false };
    long long diskLastWrite[2] = { 0, 0 };
    std::string diskWriteError;          // the last write-back that failed, or ""
    void flushDisk(int unit);
    void flushDisks() { flushDisk(0); flushDisk(1); }
    bool loadTapeFile(const std::string& path);
    // THE TAPE DECK (core/tape.h): its buttons. Whether it plays is asked of the deck --
    // a tape stops by itself too (a stop block, its end).
    CPCTapeDrive* tapeDeck() const;           // null with no tape in
    bool tapePlaying() const;                 // PLAY down and not paused
    bool tapePaused() const;
    void tapePlay();
    void tapePause();                         // toggles PAUSE
    void tapeStop();
    void tapeRewind();                        // to the start
    void tapeRewindBlock();                   // to this block's start, or the one before
    void tapeFastForward();                   // to the next block
    void tapeSeekBlock(int index);
    void tapeEject();
    void tapeResetCounter();
    // CHEATS (core/cheats.h): the finder's search over RAM and the cheats held every frame.
    // A game's cheats live beside it as <name>.pok -- loaded (switched off) when it is, and
    // saved there by saveCheats() unless another file is given.
    CheatFinder cheatFinder;
    CheatList cheatList;
    std::string cheatsPath;                   // the .pok file of the game loaded last
    Bytes* ram();                             // the machine's RAM (null when nothing is booted)
    void loadCheatsBeside(const std::string& mediaPath);
    bool loadCheats(const std::string& path);
    bool saveCheats(const std::string& path = "");
    void enableCheat(size_t index, bool on);  // a "once" cheat is written as it is switched on

    // RECORD (REC with PLAY): what the CPC SAVEs goes onto the tape from the head on. STOP
    // (or EJECT, or a new tape) ends it; with diskWriteBack on, a .cdt/.tzx file then takes
    // it, as a disc's file takes its writes. A blank tape has no file until Save tape as.
    void tapeRecord();
    bool tapeRecording() const;
    void tapeNewBlank();
    bool tapeSaveAs(const std::string& path);
    void flushTape();                         // a finished recording into its file
    std::string tapePath;                     // the file the tape came from ("" for a blank one)

    // Out of the slot: the machine as it boots without it (a Plus on its system
    // cartridge). false when there is none.
    bool ejectCartridge();
    // The snapshot let go: the machine booted afresh. false when none is loaded.
    bool ejectSnapshot();
    bool saveSnapshot(const std::string& path);
    bool loadSnapshot(const std::string& path);
    // A built program (the assembler's C build) started as a CPC starts a binary from BASIC:
    // the machine reset to the BASIC prompt, the bytes put in RAM, then the firmware's MC
    // START PROGRAM (&BD16; HL = the entry, C = &FF: both ROMs off), which sets up the
    // firmware and the stack and jumps to it. A GX4000 has no firmware: the ROMs over the program are paged
    // out and the PC set. `how` says which. False with no machine.
    bool startProgram(const std::vector<uint8_t>& bytes, int load, int run, std::string& how);
    // The C build's snapshot maker (CToolchain::makeSnapshot): a second machine, apart from
    // the shown one, made from the ROMs of the current model (of a 6128 when the current one
    // has no firmware or is a Plus), taken to the BASIC prompt, the program put in and MC
    // START PROGRAM set up as startProgram does; its .sna is the result. The ROMs are read
    // now; the function itself may run on another thread. Empty when the ROMs are missing.
    std::function<bool(const std::vector<uint8_t>&, int, int, std::vector<uint8_t>&, std::string&)> programSnapshotMaker();

    // ---- RECORDING (core/media_recorder.h): every frame's picture and sound to a file ----
    // "mjpeg" (an AVI most players play), "zmbv" (a lossless AVI) or, by the file's name, a
    // .wav of the sound alone.
    std::string recordFormat = "mjpeg";   // "mjpeg", "zmbv" or "h264" (ffmpeg)
    std::string ffmpegPath;               // ffmpeg's program for H.264; "" = on the PATH
    // The picture a video recording takes: "shown" (as the screen shows it: the GFX9000's on
    // a switched monitor or through a Video9000, both side by side when it has a picture of its
    // own, beside the CPC's or in the GFX9000 screen window), "cpc", "gfx" (the GFX9000's, scaled to the CPC's frame at 4:3, black bars), or
    // "both" (side by side, twice as wide). The frame's size is set when it starts.
    std::string recordSource = "shown";
    int recordWidth = 0, recordHeight = 0;
    std::vector<uint32_t> recordCanvas;
    // "screen": the picture as drawn, read back by main after each frame (recordWidth x
    // recordHeight, pixels 0xAABBGGRR); screenPictureSize is its drawn size in pixels, which
    // the recording takes when it starts.
    std::vector<uint32_t> screenCapture;
    int screenPictureW = 0, screenPictureH = 0;
    const uint32_t* recordPicture();      // this frame's picture for the recorder
    std::string recordDir = "recordings";    // where the save dialog starts (set by main)
    MediaRecorder recorder;
    void pollRecorder();               // an MP4 finished in the background: its result in `status`
    bool startRecording(const std::string& path, std::string& why);
    void stopRecording();
    bool recording() const { return recorder.active(); }

    // ---- RECENT FILES: the discs, tapes, cartridges and snapshots loaded, newest first ----
    std::vector<std::string> recentFiles;
    static constexpr size_t RECENT_MAX = 12;
    void noteRecent(const std::string& path);
    void clearRecent() { recentFiles.clear(); }

    // ---- QUICK SAVE SLOTS ---------------------------------------------------------------
    // SLOT_COUNT slots for the program in use (the disc, tape, cartridge or snapshot loaded
    // last; else the model): each one a .sna file and a small picture in
    // stateDir/<program>/, so they stay from one session to the next. In this session a slot
    // also holds an exact copy of the machine (timeline.h); a load takes that copy when the
    // machine and its media are still the same, and the .sna file when not.
    std::string stateDir = "states";
    static constexpr int SLOT_COUNT = 9;
    struct SlotInfo {
        bool used = false;
        std::string when;                     // "2026-10-17 14:03"
        std::vector<uint32_t> thumb;          // 0xAABBGGRR, thumbW x thumbH
        int thumbW = 0, thumbH = 0;
        long revision = 0;                    // changes when the picture does
    };
    std::string slotProgram() const;          // the folder: a name, no path
    const SlotInfo& slotInfo(int slot);       // slot 1..SLOT_COUNT
    bool saveSlot(int slot);
    bool loadSlot(int slot);
    bool canUndoLoad() const { return undoLoad != nullptr; }
    bool undoSlotLoad();                      // the machine as it was before the last load

    void reset();

    // ---- THE TIMELINE (core/timeline.h): rewind and reverse debugging ----------------
    // A snapshot of the machine every timelineInterval frames, as far back as the budget
    // holds, and every input since the oldest one (InputEvent, stamped with the instruction
    // it came before). Going back is: the snapshot before the point, then the machine run on
    // to it with the inputs as they came. Running on from there replays the inputs that
    // came after, until a new one comes: from then the old future is gone. A change of the
    // machine's setup (model, RAM, chips, cards, media) or a reset starts it again.
    bool timelineEnabled = false;          // off by default: it holds up to timelineBudgetMB of snapshots
    int timelineInterval = 50;            // frames between snapshots (a second)
    int timelineBudgetMB = 256;
    struct TimelineInfo { uint64_t oldest = 0, now = 0, liveEnd = 0; size_t snapshots = 0, inputs = 0, bytes = 0; };
    TimelineInfo timelineInfo() const;
    void timelineClear();
    bool timelineGoTo(uint64_t instruction);   // the machine as it was before that instruction
    bool timelineStepBack();                   // one instruction back
    bool timelineFrameBack();                  // to the start of the frame before
    bool timelineRewind(double seconds);       // that much machine time back (at least one frame)
    bool timelineRunBack();                    // to the last breakpoint hit before now
    bool timelineLastWrite(int start, int end);   // to the instruction that last wrote there
    std::string timelineMessage;               // what the last of these did, or why not
    // The machine changed by hand (the debugger's registers, memory, PC; the assembler; a
    // cheat's poke; the BASIC injector): that is not an input a replay could repeat, so
    // the timeline starts again from here.
    void timelineEdited();

    // ---- INPUT MOVIES ---------------------------------------------------------------
    // From power-on (the machine reset, the disc drives' heads and turn from the start, the
    // tape rewound), every input with the instruction it came before, and a hash of the
    // machine every second. Played back from the same power-on with the same machine and
    // media, it is the same run; the hashes say whether it stayed in step, and where not.
    bool movieRecord(std::string& why);                            // power on, then record
    bool movieSave(const std::string& path, std::string& why);     // stop recording, to a file
    bool moviePlay(const std::string& path, std::string& why);     // power on, then play the file
    void movieStop();                                              // stop recording (nothing saved) or playing
    bool movieRecording() const { return movieRec; }
    bool moviePlaying() const { return moviePlay_; }
    std::string movieStatus;                                       // where it is, in step or not
    // While a movie plays, the host's own input does not reach the machine.
    bool liveInputBlocked() const { return moviePlay_ && !applyingInput; }
    uint64_t timelineLiveEnd() const { return liveEnd; }
    void stepInstruction(bool redraw = true);   // false: leave the picture to the caller (many steps)
    bool pictureChanged = false;                // render() made a new picture: the front end uploads it
    // THE RASTER TIMING OVERLAY: each OUT to the CRTC, the GATE ARRAY, the PPI and the Plus
    // palette marked on the picture where the beam was when it was made. Off: nothing noted.
    bool rasterOverlay = false;
    void setRasterOverlay(bool on);
    void runFrame();                         // advance one frame + drain audio
    // TYPING: text typed on the CPC's keyboard by runFrame, a key at a time (held 4 frames,
    // let go for 4); "\n" is Enter. A character no key types is skipped.
    void typeText(const std::string& text) { typed += text; }
    bool typing() const { return typedAt < typed.size(); }
    // THE BASIC PROGRAM (core/basic.h). basicProgramEnd: where the program at &0170 ends, and
    // whether BASIC 1.0 (the 464) or 1.1 holds it -- told by which of BASIC's pointers holds
    // that end (or, for a LOADed program, a point after it: the file's own end); false when
    // neither does (BASIC is not at its prompt). injectBasic: a
    // tokenized program (BasicDecoder::tokenizeProgram) written at &0170 and BASIC's
    // pointers set to its end, as if it had been typed; false and `why` when BASIC is not at
    // its prompt or the program is too long.
    bool basicProgramEnd(int& end, bool& basic10) const;
    bool injectBasic(const Bytes& program, std::string& why);
    uint64_t framesRun = 0;                  // frames runFrame has run (not counting paused calls)

    // Debugger hooks. A breakpoint, a watchpoint or a reached step target stops the
    // machine by setting `paused` -- the same pause the UI's Pause shows -- and says why
    // in `breakReason`. stopAfter, when set, is asked after every instruction (with the
    // PC that instruction started at) and stops the machine once it answers true; it is
    // cleared when it fires. watchArmed arms the memory watch hook while the machine runs.
    std::function<bool(int previousPc)> stopAfter;
    bool watchArmed = false;
    std::string breakReason;
    // Where the instruction being executed began. The Z80's PC has moved past its
    // operands by the time it touches memory, so a watchpoint names the instruction by
    // this. Kept only while stopAfter or a watchpoint is armed (-1 otherwise).
    int instructionPc = -1;
    void render();                           // CpcVideo::render() into video->pixels
    double cpcRefreshHz = 50.0;              // emulated CPC display refresh (VSYNC rate)
    bool beamRenderer = false;               // beam-driven CRT renderer (pin-accurate)
    void setBeamRenderer(bool on);           // classic path only; Plus keeps legacy

    void setKey(SDL_Scancode sc, bool pressed);
    void setMonitorMode(const std::string& mode);
    // Plug a set in, by monitor_model.h id; "" restores the one the machine shipped with.
    void setMonitorSet(const std::string& id);
    void setMonitorHolds(int hHold, int vHold);
    void setMonitorKnobs(const TubeKnobs& knobs);
    void setZ80Cmos(bool cmos);
    void applyMonitorSet();
    // Fit a GATE ARRAY part (40007/40008/40010, 0 = the 40010). Live: the chip reads its
    // model on every pixel, so no reset is needed.
    void setGateArrayPart(int part);
    void applyGateArrayPart();
    int fittedGateArrayPart() const;   // what is actually fitted, ASIC included

    void setJoystickEnabled(bool on);
    void setKeyboardRegion(const std::string& region);
    void setDacType(const std::string& type);   // the printer-port device
    bool printerAttached() const { return dacType == "printer" || dacType == "matrix"; }

    // Printer output. The text printer's characters, and the dot-matrix printer's pages.
    std::string printerText;
    MatrixPrinter* matrixPrinter = nullptr;      // owned; created with the host
    int printerRevision = 0;                     // bumps on every byte printed
    void clearPrinter();
    bool savePrinterText(const std::string& path) const;
    bool savePrinterPageBmp(const std::string& path) const;
    bool savePrinterPageSvg(const std::string& path) const;

    // Lightgun.
    void setLightgun(const std::string& type);
    bool lightgunActive() const { return lightgunType != "none"; }
    // Where the gun points, in screen-framebuffer pixels (x < 0: off the screen), and
    // whether the trigger is held.
    void lightgunAim(double x, double y, bool trigger);

    // GFX9000 (V9990).
    void setV9990(bool on);
    void setOpl4(bool on);
    void setPlayCity(bool on);
    void setSpeech(const std::string& kind);
    void applySpeech();
    bool speechHasRom() const { return !findRom("sp0256 al2").empty(); }
    void applyPlayCity();
    void applyOpl4();
    // The V9990's own monitor: its last field, or null while it shows nothing (not
    // fitted, in stand-by, or never displayed).
    const V9990Picture* v9990Picture() const;

    // Tape deck options.
    void applyTapeOptions();

    // CPC Plus analogue port (ASIC ADC0-7, &6808-&680F): 0..63 per channel.
    int analogue(int channel) const;
    void setAnalogue(int channel, int value);

    bool fitExpansionRom(int slot, const std::string& path);
    void clearExpansionRom(int slot);
    std::string expansionRomName(int slot) const;   // "" if none fitted

    // firmware ROM manager: which = 0 OS / 1 BASIC / 2 AMSDOS. Overriding or
    // resetting re-boots the current model so the change takes effect.
    void setFirmwareRom(int which, const std::string& path);
    void clearFirmwareRom(int which);

    bool setM4(bool enabled, const std::string& folder);
    void rescanM4();

    void setSymbiface(const std::string& module);    // none / sf2 / sf3
    void setMouseSensitivity(float s);
    bool symbifaceMouseActive() const;
    // A MOUSE INTERFACE: "none", "amx" (joystick port), "kempston", "multiplay-a" or
    // "multiplay-b" (the MultiPlay port the mouse is in). A Symbiface II or III has its own
    // mouse: while one is fitted the interface is kept out (mouseInterfaceActive false).
    std::string mouseInterface = "none";
    void setMouseInterface(const std::string& kind);
    void applyMouseInterface();
    bool mouseInterfaceActive() const;
    bool emulatedMouseActive() const { return symbifaceMouseActive() || mouseInterfaceActive(); }   // the host mouse is the CPC's
    float mouseFracX = 0, mouseFracY = 0;      // the interfaces' fractions of a count
    void mouseMove(float dx, float dy);
    void mouseButton(int index, bool pressed);
    void mouseScroll(float delta);

    bool saveScreenshotBmp(const std::string& path);
    bool loadByExtension(const std::string& path);   // drag-and-drop dispatch

    static Bytes readFile(const std::string& path, bool& ok);

private:
    // the quick save slots (see saveSlot)
    std::string lastProgram;                  // the stem of the media loaded last
    std::string slotsFor;                     // slotProgram() when `slots` was read
    std::array<SlotInfo, SLOT_COUNT + 1> slots{};
    long slotRevision = 0;
    struct ExactSlot {
        std::unique_ptr<MachineSnapshot> machine;
        std::string setup;                    // slotProgram() + timelineFingerprint()
        std::vector<uint32_t> picture;        // the screen as it was
    };
    std::array<ExactSlot, SLOT_COUNT + 1> exactSlots;
    std::unique_ptr<MachineSnapshot> undoLoad;
    std::string undoSetup;
    std::vector<uint32_t> undoPicture;
    std::string slotFile(int slot, const char* extension) const;
    void readSlot(int slot);
    bool applySnaFile(const std::string& path, std::string& why);
    void restoreExact(const MachineSnapshot& s, const std::vector<uint32_t>& picture);
    // the movie (see movieRecord)
    bool movieRec = false, moviePlay_ = false;
    uint64_t movieStartAt = 0, movieEndAt = 0;          // GX4000::instructions
    long long movieStartCycles = 0;
    std::vector<InputEvent> movieEvents;                // recording: as they came (absolute .at)
    struct MovieCheck { uint64_t at; uint64_t hash; };  // a hash of the machine at a frame's end
    std::vector<MovieCheck> movieChecks;
    size_t movieCheckAt = 0;
    int movieFrames = 0, movieChecked = 0;
    uint64_t movieStartHash = 0;
    bool movieOutOfStep = false;
    uint64_t movieStateHash() const;
    void moviePowerOn();
    void movieFrameEnd();
    // the timeline (see timelineGoTo)
    std::deque<std::unique_ptr<MachineSnapshot>> snapshots;
    std::vector<InputEvent> inputLog;        // in the order they came (their .at never goes down)
    std::deque<uint64_t> frameStarts;        // GX4000::instructions at the start of each frame
    size_t replayAt = 0;                     // the next logged input a replay applies
    bool applyingInput = false;              // a replayed input is going in: not logged again
    uint64_t liveEnd = 0;                    // how far the machine had run before it went back
    size_t snapshotBytes = 0;
    int framesSinceSnapshot = 0;
    std::string timelineSetup;               // timelineFingerprint() when it began
    long timelineEpoch = 0, timelineEpochSeen = -1;
    std::string timelineFingerprint() const;
    void timelineFrameStart();
    void timelineFrameEnd();
    void timelineLog(InputEvent e);
    void timelineApplyDue();
    void timelineApply(const InputEvent& e);
    void timelineTruncate();                 // a new input: the future after now goes
    bool timelineSearch(const std::function<void(uint64_t& hit)>& beforeStep, int watchStart, int watchEnd,
                        uint64_t& lastHit);
    std::string typed;                       // typeText's queue, the character at typedAt next
    size_t typedAt = 0;
    int typedPhase = 0;                      // 0-3 the key is down, 4-7 up
    void drainAudio();
    void applySettings();                    // re-apply region/joystick/DAC after a boot
    void applyExpansionRoms();               // re-fit slot ROMs after a boot
    bool croBoot = false;                    // bootModel keeps croSet (loadCroRomSet)
    bool loadCroRomSet(const CroRomSet& set, const std::string& path, const std::string& what);
    void applyM4();
    void applySerial();                      // re-fit the serial card after a boot
    void applyMultiface();                   // re-fit the Multiface II after a boot                          // re-enable M4 after a boot
    void applySymbiface();
    void applyLightgun();
    void applyPrinter();
    std::string findRom(const std::string& keyword) const;
    std::string findCart(const std::string& keyword) const;   // search mediaDir/romDir for a .cpr
    void applyAudioRate();
};

} // namespace cpcse
