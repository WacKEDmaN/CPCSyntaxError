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
// The Pico's own menu program (a cartridge in its firmware) is not published, so the menu
// is the emulator's: the Cartridge window lists the SD folder, and launching a file loads it
// as the Pico would flash it (picogx host code in src/gui/emuhost.cpp).
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

private:
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
