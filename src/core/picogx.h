// CPCSyntaxError: PicoGX, the SD-card cartridge for the 464 Plus, 6128 Plus and GX4000
// (github.com/Neo2003/PicoGX, by Rodrik Studio). A Raspberry Pi Pico 2 on the cartridge
// port answers the cartridge's address bus from a file it loaded off its SD card.
//
// What is emulated is what the project documents for software (Programming.md):
//
//   * The "contact" sequence. A cartridge has no write line, so the machine talks to the
//     Pico by READING: cartridge address &0133C, then &025C4, with no other cartridge read
//     between, and the next cartridge read's address is a command. Code that does this runs
//     from RAM (a fetch from the cartridge would come between).
//       5  read:  bank 0 reads answer the 16 KB save instead of the cartridge
//       6  write: every bank 0 read writes its address's low byte into the save, in order
//                 from the start of the save
//       7  stop:  bank 0 is the cartridge again; after a write the save goes to the SD card
//                 (saves/<cartridge name>.SAV), and it is read from there when the
//                 cartridge is launched
//   * Cartridges larger than 512 KB (up to 1.5 MB). A read of cartridge address &03FFC-
//     &03FFF (bank 0, whatever RMR2 maps) chooses the 512 KB block the bus shows from its
//     two low bits: 11 the first, 10 the second, 01 the third, 00 nothing. Only for a
//     cartridge larger than 512 KB.
//
// THE MENU. The menu the CPC shows is a Z80 cartridge page inside the Pico's firmware (the
// .uf2 file). It is not published and not shipped here: menuFromFirmware() takes it out of the
// user's own .uf2 when they start it, and this class plays the Pico's part of it (read off the
// menu's Z80 code, firmware 1.2.1; scratch notes picogx_work/notes.txt):
//   * The page is whole in the flash, from its &0000 (LD BC,&7F8A / OUT (C),C / JP &004E:
//     mode 2 and the lower ROM on, then the start-up code at &004E).
//   * Contact sequence, command 0-4, then ONE more cartridge read whose ADDRESS is the
//     argument (the menu does LD H,0 / LD L,row / LD A,(HL)). Then it polls &3FFF until 0.
//       1 (arg 0 / 1) list the folder: previous / next page (0 at start-up: the first page)
//       3 (row) enter the folder on that row      4 go up a folder
//       0 (row) the row's information text        2 (row) launch the row's file
//   * The data window in page 0: &3D84 first row of the information text, &3D85-&3D87
//     loading colours, &3D88-&3D89 menu colours (INKR values), &3D8A+row 1 = a folder,
//     &3D9A 1 = row 0 is the last game (drawn inverted), &3D9B 1 = the page changed,
//     &3D9C the page text, &3DA8 the rows' names (0-terminated, an empty one ends them; a
//     first byte of 1 means "launch directly" at start-up), &3900 the information text
//     (0-terminated lines, an empty one ends them), &3FFF busy / launch progress.
#pragma once
#include "common.h"

#include <functional>
#include <string>
#include <vector>

namespace cpcse {

class PicoGX {
public:
    static constexpr int BLOCK_PAGES = 32;                // 512 KB of 16 KB pages
    static constexpr int MAX_PAGES = 3 * BLOCK_PAGES;     // 1.5 MB
    static constexpr int SAVE_SIZE = 0x4000;
    static constexpr int CONTACT1 = 0x0133c, CONTACT2 = 0x025c4;
    enum Command { CMD_READ = 5, CMD_WRITE = 6, CMD_STOP = 7 };
    enum Mode { NORMAL, SAVE_READ, SAVE_WRITE };

    // The cartridge the Pico holds: every 16 KB page of the file, more than 32 for a large one.
    // Pages it does not have are empty.
    std::vector<Bytes> pages;
    int block = 0;                    // the 512 KB block on the bus (0-2)
    Mode mode = NORMAL;
    Bytes save = Bytes(SAVE_SIZE, 0);
    int writePos = 0;
    std::string savePath;             // the .SAV file on the SD card ("": no file)
    int savesWritten = 0;             // how many times the save went to the card
    std::string lastEvent;            // for the user interface: "save read", "block 2" ...

    // Called with the 32 pages of a block when the block on the bus changes.
    std::function<void(const std::vector<Bytes>&)> mapBlock;

    // A new cartridge: its pages and its save file. The save is read if the file exists.
    void launch(const std::vector<Bytes>& cartridgePages, const std::string& saveFile);
    // The 32 pages of a block (empty pages past the end of the file).
    std::vector<Bytes> blockPages(int which) const;
    bool large() const { return (int)pages.size() > BLOCK_PAGES; }

    // A cartridge read by the CPU on page `page` (A14-A18) at `offset` (A0-A13), whose ROM
    // byte is `value`. Answers the byte the bus carries.
    int onRead(int page, int offset, int value);

    // ---- the menu
    static constexpr int ROWS = 13;           // rows 4, 6 ... 28 of the menu's screen
    struct Entry { std::string name, relative; bool folder = false, last = false; };
    bool menuActive = false;
    std::string sdRoot;                       // the card (a folder on this computer)
    std::string menuDir;                      // the folder shown, relative to the card
    int menuPage = 0;
    std::vector<Entry> entries;               // the folder's rows, every page
    std::string lastGame;                     // relative to the card ("": none)
    std::string pendingLaunch;                // set by command 2: the host launches it
    uint8_t menuColours[2] = { 0, 0 }, loadColours[3] = { 0, 0, 0 };   // 0: the image's own
    // The menu page (16 KB) taken out of a .uf2 file; empty with the reason when not found.
    static Bytes menuFromFirmware(const Bytes& uf2, std::string& why);
    // Starts the menu on the card: pages = { the menu page }.
    void startMenu(const Bytes& image, const std::string& card, const std::string& last);
    void readFolder();                        // entries of menuDir
    const std::vector<Entry*> pageEntries();

private:
    int argCommand = -1;                      // a menu command waiting for its argument read
    Bytes window;                             // page 0 as the menu reads it (menu mode)
    void menuCommand(int command, int argument);
    void fillWindow(bool pageChanged);
    void putInfo(const Entry& e);
    int sequence = 0;                 // 0, 1 after &0133C, 2 after &025C4
    int writeMark = 0;                // writePos when the sequence started (see onRead)
    // The last three bytes written, with what they replaced: a stop sequence's own reads
    // are written before they can be told from data, and the stop takes them back.
    std::pair<int, uint8_t> recent[3] = { { -1, 0 }, { -1, 0 }, { -1, 0 } };
    int recentNext = 0;
    void command(int number);
    void writeSaveFile();
};

// Splits a PicoGX cartridge file into 16 KB pages: a CPR's cbNN chunks (any NN, so more
// than 32 for a large cartridge) or a raw .BIN image of 16 KB pages. Throws with a reason.
std::vector<Bytes> picoGxPages(const Bytes& file);

} // namespace cpcse
