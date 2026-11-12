// CPCSyntaxError: PicoGX: see picogx.h.
#include "picogx.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace cpcse {

void PicoGX::launch(const std::vector<Bytes>& cartridgePages, const std::string& saveFile) {
    pages = cartridgePages;
    if ((int)pages.size() > MAX_PAGES) pages.resize(MAX_PAGES);
    block = 0;
    mode = NORMAL;
    sequence = 0;
    writePos = writeMark = 0;
    save.assign(SAVE_SIZE, 0);
    savePath = saveFile;
    // "Each time a cpr is launched, the save is created in the pico memory and corresponding
    // .SAV file loaded from the SD card if exists."
    if (!savePath.empty()) {
        std::ifstream f(std::filesystem::u8path(savePath), std::ios::binary);
        if (f) {
            f.read((char*)save.data(), SAVE_SIZE);
            lastEvent = "save loaded from " + std::filesystem::u8path(savePath).filename().u8string();
        }
    }
}

std::vector<Bytes> PicoGX::blockPages(int which) const {
    std::vector<Bytes> out((size_t)BLOCK_PAGES);
    for (int i = 0; i < BLOCK_PAGES; i++) {
        const size_t k = (size_t)(which * BLOCK_PAGES + i);
        if (k < pages.size()) out[(size_t)i] = pages[k];
    }
    return out;
}

void PicoGX::writeSaveFile() {
    if (savePath.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(savePath).parent_path(), ec);
    std::ofstream f(std::filesystem::u8path(savePath), std::ios::binary | std::ios::trunc);
    if (f && f.write((const char*)save.data(), SAVE_SIZE)) {
        savesWritten++;
        lastEvent = "save written to " + std::filesystem::u8path(savePath).filename().u8string();
    } else lastEvent = "could not write " + savePath;
}

void PicoGX::command(int number) {
    switch (number) {
        case CMD_READ: mode = SAVE_READ; lastEvent = "reading the save"; break;
        case CMD_WRITE:
            mode = SAVE_WRITE; writePos = 0; lastEvent = "writing the save";
            for (auto& w : recent) w = { -1, 0 };
            break;
        case CMD_STOP:
            // "It is saved when the write command is ended by a stop command."
            if (mode == SAVE_WRITE) writeSaveFile();
            mode = NORMAL;
            break;
        default: break;   // the menu's own commands (3 is its "cd"): nothing here to do
    }
}

int PicoGX::onRead(int page, int offset, int value) {
    const int address = (page & 0x1f) << 14 | (offset & 0x3fff);
    const bool bank0 = page == 0;
    int answer = value;
    // Bank 0 as the mode presents it. A read that is part of the contact sequence is a read
    // like any other here; a write mode's bytes from the closing sequence are taken back
    // when its stop arrives (writeMark).
    if (bank0 && mode == SAVE_READ) answer = save[(size_t)(offset & 0x3fff)];
    else if (bank0 && mode == SAVE_WRITE && writePos < SAVE_SIZE) {
        recent[recentNext] = { writePos, save[(size_t)writePos] };   // for the stop to take back
        recentNext = (recentNext + 1) % 3;
        save[(size_t)writePos++] = (uint8_t)(offset & 0xff);
    }

    // The contact sequence: &0133C, &025C4, then the command, with no other cartridge read
    // between them.
    if (sequence == 2) {
        sequence = 0;
        const bool stop = bank0 && (offset & 0x3fff) == CMD_STOP;
        if (stop && mode == SAVE_WRITE) {   // the stop's own three reads are not data
            for (const auto& w : recent) if (w.first >= writeMark && w.first >= 0) save[(size_t)w.first] = w.second;
            writePos = writeMark;
        }
        if (bank0) command(offset & 0x3fff);
    } else if (address == CONTACT2 && sequence == 1) {
        sequence = 2;
    } else if (address == CONTACT1) {
        sequence = 1;
        writeMark = mode == SAVE_WRITE ? writePos - (bank0 ? 1 : 0) : 0;
    } else sequence = 0;

    // A large cartridge: bank 0's last four addresses choose the 512 KB block.
    if (bank0 && (offset & 0x3ffc) == 0x3ffc && large()) {
        static const int BLOCK_FOR_LOW_BITS[4] = { -1, 2, 1, 0 };
        const int want = BLOCK_FOR_LOW_BITS[offset & 3];
        if (want >= 0 && want * BLOCK_PAGES < (int)pages.size() && want != block) {
            block = want;
            lastEvent = "512 KB block " + std::to_string(block + 1) + " on the bus";
            if (mapBlock) mapBlock(blockPages(block));
        }
    }
    return answer;
}

std::vector<Bytes> picoGxPages(const Bytes& file) {
    std::vector<Bytes> pages;
    const bool cpr = file.size() >= 12 && std::string(file.begin(), file.begin() + 4) == "RIFF"
                     && std::string(file.begin() + 8, file.begin() + 12) == "AMS!";
    if (!cpr) {
        if (file.empty() || file.size() % 0x4000 || file.size() > (size_t)PicoGX::MAX_PAGES * 0x4000)
            throw std::runtime_error("a .BIN cartridge is 16 KB to 1.5 MB, in 16 KB pages");
        for (size_t k = 0; k < file.size(); k += 0x4000) pages.emplace_back(file.begin() + (long)k, file.begin() + (long)k + 0x4000);
        return pages;
    }
    size_t at = 12;
    while (at + 8 <= file.size()) {
        const std::string id(file.begin() + (long)at, file.begin() + (long)at + 4);
        const uint32_t length = file[at + 4] | file[at + 5] << 8 | file[at + 6] << 16 | (uint32_t)file[at + 7] << 24;
        at += 8;
        if (length > file.size() - at) throw std::runtime_error("a chunk runs past the end of the file");
        if ((id[0] == 'c' || id[0] == 'C') && (id[1] == 'b' || id[1] == 'B')
            && std::isdigit((unsigned char)id[2]) && std::isdigit((unsigned char)id[3])) {
            const int n = (id[2] - '0') * 10 + (id[3] - '0');
            if (n < PicoGX::MAX_PAGES) {
                if ((int)pages.size() <= n) pages.resize((size_t)n + 1);
                Bytes page(0x4000, 0);
                std::copy(file.begin() + (long)at, file.begin() + (long)at + std::min<long>((long)length, 0x4000), page.begin());
                pages[(size_t)n] = page;
            }
        }
        at += length;   // as parseCartridge: cbNN chunks are 16 KB, never odd
    }
    if (pages.empty()) throw std::runtime_error("the CPR holds no cbNN page");
    return pages;
}

} // namespace cpcse
