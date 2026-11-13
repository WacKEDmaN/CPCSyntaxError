// CPCSyntaxError: PicoGX: see picogx.h.
#include "picogx.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace cpcse {

void PicoGX::launch(const std::vector<Bytes>& cartridgePages, const std::string& saveFile) {
    menuActive = false;
    window.clear();
    argCommand = -1;
    pendingLaunch.clear();
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
    if (menuActive && bank0 && window.size() == 0x4000) {
        // The busy flag: set while the host has a launch to do (the menu draws it as progress).
        window[0x3fff] = pendingLaunch.empty() ? 0 : 0x28;
        answer = window[(size_t)(offset & 0x3fff)];
    }
    if (argCommand >= 0) {                // the menu command's argument: this read's address
        const int c = argCommand;
        argCommand = -1;
        sequence = 0;
        menuCommand(c, offset & 0x3fff);
        if (menuActive && bank0 && window.size() == 0x4000) answer = window[(size_t)(offset & 0x3fff)];
        return answer;
    }
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
        const int number = offset & 0x3fff;
        if (bank0 && menuActive && number <= 4) argCommand = number;   // the argument read comes next
        else if (bank0) command(number);
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

// ================================================================ the menu
namespace {

std::string lowerCase(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool cartridgeFile(const std::filesystem::path& p) {
    const std::string e = lowerCase(p.extension().u8string());
    return e == ".cpr" || e == ".bin" || e == ".dsk" || e == ".cro";
}

bool holdsCartridge(const std::filesystem::path& dir, int depth = 0) {
    std::error_code ec;
    if (depth > 12) return false;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && cartridgeFile(it->path())) return true;
        if (it->is_directory(ec) && holdsCartridge(it->path(), depth + 1)) return true;
    }
    return false;
}

} // namespace

// The UF2 file (512-byte blocks, 476 bytes of data each at a flash address) flattened, then
// the menu found by its own bytes: the RST stubs it starts with at &0028 (C9 and seven 0s,
// three times) and its "Select a game" text after them.
Bytes PicoGX::menuFromFirmware(const Bytes& uf2, std::string& why) {
    std::vector<std::pair<uint32_t, size_t>> blocks;
    uint32_t lo = 0xffffffffu, hi = 0;
    auto u32 = [&](size_t at) { return (uint32_t)uf2[at] | (uint32_t)uf2[at + 1] << 8 | (uint32_t)uf2[at + 2] << 16 | (uint32_t)uf2[at + 3] << 24; };
    for (size_t off = 0; off + 512 <= uf2.size(); off += 512) {
        if (u32(off) != 0x0A324655u || u32(off + 4) != 0x9E5D5157u || u32(off + 508) != 0x0AB16F30u) continue;
        if (u32(off + 8) & 1) continue;                   // not for the main flash
        const uint32_t addr = u32(off + 12), size = u32(off + 16);
        if (size > 476 || addr < 0x10000000u || addr >= 0x11000000u) continue;
        blocks.push_back({ addr, off });
        lo = std::min(lo, addr);
        hi = std::max(hi, addr + size);
    }
    if (blocks.empty()) { why = "not a UF2 firmware file"; return {}; }
    Bytes flash((size_t)(hi - lo), 0xff);
    for (const auto& b : blocks) {
        const uint32_t size = u32(b.second + 16);
        std::copy(uf2.begin() + (long)b.second + 32, uf2.begin() + (long)b.second + 32 + size, flash.begin() + (b.first - lo));
    }
    // The page starts at its own &0000: LD BC,&7F8A / OUT (C),C / JP &004E (mode 2, lower ROM
    // on), then the RST stubs at &0008-&0030 (C9 and seven 0s each); "Select a game" follows.
    static const uint8_t start[] = { 0x01, 0x8a, 0x7f, 0xed, 0x49, 0xc3, 0x4e, 0x00,
                                     0xc9, 0, 0, 0, 0, 0, 0, 0, 0xc9, 0, 0, 0, 0, 0, 0, 0, 0xc9 };
    static const char text[] = "Select a game";
    for (size_t at = 0; at + sizeof(start) < flash.size(); at++) {
        if (!std::equal(start, start + sizeof(start), flash.begin() + (long)at)) continue;
        const size_t end = std::min(flash.size(), at + 0x2000);
        if (std::search(flash.begin() + (long)at, flash.begin() + (long)end, text, text + sizeof(text) - 1) == flash.begin() + (long)end) continue;
        Bytes page(0x4000, 0);
        const size_t n = std::min<size_t>(0x4000, flash.size() - at);
        std::copy(flash.begin() + (long)at, flash.begin() + (long)(at + n), page.begin());
        return page;
    }
    why = "no PicoGX menu in this firmware";
    return {};
}

void PicoGX::startMenu(const Bytes& image, const std::string& card, const std::string& last) {
    pages = { image };
    block = 0;
    mode = NORMAL;
    sequence = 0;
    argCommand = -1;
    savePath.clear();
    sdRoot = card;
    menuDir.clear();
    menuPage = 0;
    lastGame = last;
    pendingLaunch.clear();
    window = image;
    menuActive = true;
    readFolder();
    fillWindow(false);
    lastEvent = "menu started";
}

void PicoGX::readFolder() {
    namespace fs = std::filesystem;
    entries.clear();
    std::error_code ec;
    const fs::path root = fs::u8path(sdRoot), dir = root / fs::u8path(menuDir);
    std::vector<Entry> folders, files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (name.empty() || name[0] == '.') continue;
        Entry e;
        e.name = name;
        e.relative = fs::relative(it->path(), root, ec).generic_u8string();
        if (it->is_directory(ec)) { if (lowerCase(name) == "saves" || !holdsCartridge(it->path())) continue; e.folder = true; folders.push_back(e); }
        else if (it->is_regular_file(ec) && cartridgeFile(it->path())) files.push_back(e);
    }
    auto byName = [](const Entry& a, const Entry& b) { return lowerCase(a.name) < lowerCase(b.name); };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    // "Last loaded cartridge is always on top of the list with inverted colors" (README).
    if (menuDir.empty() && !lastGame.empty() && fs::exists(root / fs::u8path(lastGame), ec)) {
        Entry e;
        e.name = fs::u8path(lastGame).filename().u8string();
        e.relative = lastGame;
        e.last = true;
        entries.push_back(e);
    }
    for (auto& e : folders) entries.push_back(e);
    for (auto& e : files) entries.push_back(e);
}

const std::vector<PicoGX::Entry*> PicoGX::pageEntries() {
    std::vector<Entry*> out;
    for (size_t i = (size_t)menuPage * ROWS; i < entries.size() && out.size() < (size_t)ROWS; i++) out.push_back(&entries[i]);
    return out;
}

void PicoGX::fillWindow(bool pageChanged) {
    if (window.size() != 0x4000) return;
    if (menuColours[0]) window[0x3d88] = menuColours[0];
    if (menuColours[1]) window[0x3d89] = menuColours[1];
    for (int i = 0; i < 3; i++) if (loadColours[i]) window[(size_t)(0x3d85 + i)] = loadColours[i];
    std::fill(window.begin() + 0x3d8a, window.begin() + 0x3d9c, 0);
    const auto rows = pageEntries();
    for (size_t i = 0; i < rows.size(); i++) window[0x3d8a + i] = rows[i]->folder ? 1 : 0;
    window[0x3d9a] = !rows.empty() && rows[0]->last ? 1 : 0;
    window[0x3d9b] = pageChanged ? 1 : 0;
    const int pagesTotal = std::max(1, (int)((entries.size() + ROWS - 1) / ROWS));
    std::string pageText = std::to_string(menuPage + 1) + "/" + std::to_string(pagesTotal);
    std::fill(window.begin() + 0x3d9c, window.begin() + 0x3fff, 0);
    std::copy(pageText.begin(), pageText.end(), window.begin() + 0x3d9c);
    size_t at = 0x3da8;
    for (const Entry* e : rows) {
        std::string name = e->folder ? "[" + e->name + "]" : e->name;
        for (char& c : name) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
        if (name.size() > 44) name.resize(44);
        if (at + name.size() + 2 >= 0x3fff) break;
        std::copy(name.begin(), name.end(), window.begin() + (long)at);
        at += name.size() + 1;            // and its 0
    }
}

void PicoGX::putInfo(const Entry& e) {
    namespace fs = std::filesystem;
    std::fill(window.begin() + 0x3900, window.begin() + 0x3d84, 0);
    fs::path txt = fs::u8path(sdRoot) / fs::u8path(e.relative);
    txt.replace_extension(".txt");
    std::ifstream f(txt, std::ios::binary);
    std::vector<std::string> lines;
    for (std::string line; f && std::getline(f, line) && lines.size() < 24;) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        for (char& c : line) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = ' ';
        while (line.size() > 42) { lines.push_back(line.substr(0, 42)); line.erase(0, 42); }
        lines.push_back(line.empty() ? " " : line);   // an empty line would end the text
    }
    size_t at = 0x3900;
    for (const std::string& l : lines) {
        if (at + l.size() + 2 >= 0x3d84) break;
        std::copy(l.begin(), l.end(), window.begin() + (long)at);
        at += l.size() + 1;
    }
    window[0x3d84] = (uint8_t)std::max(4, 17 - (int)lines.size() / 2);   // centred on the screen
}

void PicoGX::menuCommand(int command, int argument) {
    const auto rows = pageEntries();
    const Entry* row = argument < (int)rows.size() ? rows[(size_t)argument] : nullptr;
    if (std::getenv("CPCSE_TRACE_PICOGX")) std::fprintf(stderr, "[picogx] command %d argument %d (%s)\n", command, argument, row ? row->name.c_str() : "-");
    switch (command) {
        case 1: {                          // a page: 0 previous (and start-up), 1 next
            const int pagesTotal = std::max(1, (int)((entries.size() + ROWS - 1) / ROWS));
            const int before = menuPage;
            if (argument == 1 && menuPage + 1 < pagesTotal) menuPage++;
            else if (argument == 0 && menuPage > 0) menuPage--;
            fillWindow(menuPage != before);
            break;
        }
        case 3:                            // into the row's folder
            if (row && row->folder) { menuDir = row->relative; menuPage = 0; readFolder(); fillWindow(true); lastEvent = "folder " + menuDir; }
            break;
        case 4: {                          // up a folder
            if (!menuDir.empty()) {
                const std::filesystem::path p = std::filesystem::u8path(menuDir).parent_path();
                menuDir = p.generic_u8string();
                menuPage = 0;
                readFolder();
            }
            fillWindow(true);
            break;
        }
        case 0:                            // the row's information text
            if (row) putInfo(*row);
            break;
        case 2:                            // launch: the host does it (pendingLaunch)
            if (row && !row->folder) { pendingLaunch = row->relative; lastEvent = "launching " + row->name; }
            break;
        default: break;
    }
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
