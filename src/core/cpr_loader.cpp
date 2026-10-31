// CPCSyntaxError — CPR cartridge loader.
#include "cpr_loader.h"
#include <stdexcept>
#include <cctype>
#include <cstdio>
#include <set>

namespace cpcse {

static std::string ascii(const Bytes& bytes, int offset, int length) {
    std::string s;
    for (int k = 0; k < length && offset + k < (int)bytes.size(); k++) s.push_back((char)bytes[offset + k]);
    return s;
}

static uint32_t readU32LE(const Bytes& bytes, int offset) {
    return (uint32_t)((bytes[offset] | (bytes[offset + 1] << 8) |
        (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)));
}

// /^cb\d\d$/i.test(id)
static bool isCbId(const std::string& id) {
    return id.size() == 4 && std::tolower((unsigned char)id[0]) == 'c' && std::tolower((unsigned char)id[1]) == 'b'
        && std::isdigit((unsigned char)id[2]) && std::isdigit((unsigned char)id[3]);
}

bool CroGroup::hasBankable() const {
    for (const CroRom& r : roms) if (r.type == CRO_ROM_BANKABLE) return true;
    return false;
}

bool isCro(const Bytes& bytes) {
    return bytes.size() >= 12 && ascii(bytes, 0, 4) == "RIFF" && ascii(bytes, 8, 4) == "CRO ";
}

// The chunks in [from, to): id, data start and data length each. A chunk whose length runs
// past its container is an error; a pad byte follows an odd length.
struct RiffChunk { std::string id; size_t start = 0, length = 0; };
static std::vector<RiffChunk> riffChunks(const Bytes& bytes, size_t from, size_t to) {
    std::vector<RiffChunk> out;
    size_t at = from;
    if (to > bytes.size()) to = bytes.size();
    while (at <= to && to - at >= 8) {             // a RIFF length below 4 puts from past to
        RiffChunk c;
        c.id = ascii(bytes, (int)at, 4);
        c.length = readU32LE(bytes, (int)at + 4);
        c.start = at + 8;
        if (c.length > to - c.start) {             // never start + length: it can overflow
            char buf[80]; std::snprintf(buf, sizeof(buf), "Invalid CRO chunk length at offset 0x%zx.", at);
            throw std::runtime_error(buf);
        }
        out.push_back(c);
        at = c.start + c.length;
        if ((c.length & 1) && at < to) at += 1;
    }
    return out;
}

static uint32_t chunkU32(const Bytes& bytes, const RiffChunk& c) {
    if (c.length < 4) throw std::runtime_error("The CRO chunk " + c.id + " is shorter than 4 bytes.");
    return readU32LE(bytes, (int)c.start);
}

static std::string chunkText(const Bytes& bytes, const RiffChunk& c) {
    std::string s = ascii(bytes, (int)c.start, (int)c.length);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::vector<CroGroup> parseCro(const Bytes& bytes) {
    if (!isCro(bytes)) throw std::runtime_error("Not a CRO file (RIFF form \"CRO \").");
    const size_t riffLength = readU32LE(bytes, 4);
    const size_t end = riffLength <= bytes.size() - 8 ? 8 + riffLength : bytes.size();
    std::vector<CroGroup> groups;
    for (const RiffChunk& g : riffChunks(bytes, 12, end)) {
        if (g.id != "GRRO") continue;
        CroGroup group;
        group.number = (uint32_t)groups.size();
        for (const RiffChunk& c : riffChunks(bytes, g.start, g.start + g.length)) {
            if (c.id == "GNUM") group.number = chunkU32(bytes, c);
            else if (c.id == "GLBL") group.label = chunkText(bytes, c);
            else if (c.id == "GMSK") group.mask = chunkU32(bytes, c);
            else if (c.id == "ROM ") {
                CroRom rom;
                for (const RiffChunk& r : riffChunks(bytes, c.start, c.start + c.length)) {
                    if (r.id == "RID ") rom.id = chunkText(bytes, r);
                    else if (r.id == "RTYP") rom.type = chunkU32(bytes, r);
                    else if (r.id == "RLOG") rom.logical = chunkU32(bytes, r);
                    else if (r.id == "RPHY") rom.physical = chunkU32(bytes, r);
                    else if (r.id == "RDT ") rom.data.assign(bytes.begin() + (long)r.start, bytes.begin() + (long)(r.start + r.length));
                }
                group.roms.push_back(std::move(rom));
            }
        }
        groups.push_back(std::move(group));
    }
    if (groups.empty()) throw std::runtime_error("The CRO contains no GRRO group.");
    return groups;
}

Bytes serializeCro(const std::vector<CroGroup>& groups) {
    Bytes out;
    auto text = [&](const std::string& s) { out.insert(out.end(), s.begin(), s.end()); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i))); };
    auto pad = [&]() { if (out.size() % 2) out.push_back(0); };
    auto patch = [&](size_t at, size_t value) { for (int i = 0; i < 4; i++) out[at + (size_t)i] = (uint8_t)(value >> (8 * i)); };
    auto number = [&](const char* id, uint32_t v) { text(id); u32(4); u32(v); };
    auto bytes = [&](const char* id, const uint8_t* p, size_t n) { text(id); u32((uint32_t)n); out.insert(out.end(), p, p + n); pad(); };
    text("RIFF"); u32(0); text("CRO ");
    for (const CroGroup& g : groups) {
        text("GRRO");
        const size_t groupSize = out.size();
        u32(0);
        number("GNUM", g.number);
        bytes("GLBL", (const uint8_t*)g.label.data(), g.label.size());
        number("GMSK", g.mask);
        for (const CroRom& r : g.roms) {
            text("ROM ");
            const size_t romSize = out.size();
            u32(0);
            bytes("RID ", (const uint8_t*)r.id.data(), r.id.size());
            number("RTYP", r.type);
            number("RLOG", r.logical);
            number("RPHY", r.physical);
            bytes("RDT ", r.data.data(), r.data.size());
            patch(romSize, out.size() - romSize - 4);
        }
        patch(groupSize, out.size() - groupSize - 4);
    }
    patch(4, out.size() - 8);
    return out;
}

std::vector<std::string> croProblems(const std::vector<CroGroup>& groups) {
    std::vector<std::string> out;
    std::set<uint32_t> numbers;
    for (const CroGroup& g : groups) {
        const std::string name = "group " + std::to_string(g.number) + (g.label.empty() ? "" : " (" + g.label + ")");
        if (!numbers.insert(g.number).second) out.push_back("Two groups have the number " + std::to_string(g.number) + ".");
        if (g.roms.empty()) out.push_back("The " + name + " holds no ROM.");
        std::set<uint32_t> pages, highs;
        int lows = 0, mf2s = 0, bankable = 0, others = 0;
        for (const CroRom& r : g.roms) {
            const std::string rom = "ROM \"" + r.id + "\" of the " + name;
            if (r.type > CRO_ROM_MF2) { out.push_back("The " + rom + " has an unknown type " + std::to_string(r.type) + "."); continue; }
            if (r.logical > 255) out.push_back("The " + rom + " has the logical number " + std::to_string(r.logical) + " (0-255).");
            if (r.data.empty()) out.push_back("The " + rom + " is empty.");
            if (r.type == CRO_ROM_BANKABLE) {
                bankable++;
                if (r.physical > 31) out.push_back("The " + rom + " is on page " + std::to_string(r.physical) + "; a Plus cartridge has pages 0-31.");
                else if (!pages.insert(r.physical).second) out.push_back("Two bankable ROMs of the " + name + " are on page " + std::to_string(r.physical) + ".");
                if (r.data.size() > 16384) out.push_back("The " + rom + " is " + std::to_string(r.data.size()) + " bytes; a page is 16384.");
            } else {
                others++;
                if (r.type == CRO_ROM_LOW && ++lows == 2) out.push_back("The " + name + " has more than one low ROM.");
                if (r.type == CRO_ROM_MF2 && ++mf2s == 2) out.push_back("The " + name + " has more than one Multiface II ROM.");
                if (r.type == CRO_ROM_HIGH && !highs.insert(r.logical).second) out.push_back("Two high ROMs of the " + name + " are ROM " + std::to_string(r.logical) + ".");
                const size_t want = r.type == CRO_ROM_MF2 ? 8192 : 16384;
                if (!r.data.empty() && r.data.size() != want && !(r.type == CRO_ROM_MF2 && r.data.size() == 16384))
                    out.push_back("The " + rom + " is " + std::to_string(r.data.size()) + " bytes; a " + (r.type == CRO_ROM_MF2 ? "Multiface II ROM is 8192" : "CPC ROM is 16384") + ".");
            }
        }
        if (bankable && others) out.push_back("The " + name + " mixes cartridge pages with CPC ROMs: this emulator runs it as a cartridge.");
    }
    if (groups.empty()) out.push_back("There is no group.");
    return out;
}

std::vector<CroRom> croRomsFromCartridge(const Bytes& file) {
    std::vector<CroRom> roms;
    auto page = [&](int n, Bytes data) {
        char id[8]; std::snprintf(id, sizeof id, "cb%02d", n);
        CroRom r; r.id = id; r.type = CRO_ROM_BANKABLE; r.physical = (uint32_t)n; r.logical = n == 3 ? 7 : 1; r.data = std::move(data);
        roms.push_back(std::move(r));
    };
    if (file.size() >= 12 && ascii(file, 0, 4) == "RIFF" && ascii(file, 8, 4) == "AMS!") {
        for (const RiffChunk& c : riffChunks(file, 12, file.size())) {
            if (c.id.size() != 4 || c.id[0] != 'c' || c.id[1] != 'b' || !std::isdigit((unsigned char)c.id[2]) || !std::isdigit((unsigned char)c.id[3])) continue;
            const int n = (c.id[2] - '0') * 10 + (c.id[3] - '0');
            if (n > 31) continue;
            page(n, Bytes(file.begin() + (long)c.start, file.begin() + (long)(c.start + c.length)));
        }
        if (roms.empty()) throw std::runtime_error("The CPR holds no cbNN page.");
        return roms;
    }
    if (file.empty() || file.size() > (size_t)BANK_COUNT * BANK_SIZE || file.size() % BANK_SIZE)
        throw std::runtime_error("Not a CPR, and not a raw cartridge (16-512 KB, in 16 KB pages).");
    for (size_t n = 0; n * BANK_SIZE < file.size(); n++)
        page((int)n, Bytes(file.begin() + (long)(n * BANK_SIZE), file.begin() + (long)((n + 1) * BANK_SIZE)));
    return roms;
}

uint32_t croMaskForPages(int pages) {
    uint32_t size = 16384;
    while ((int)(size / 16384) < pages && size < (512u << 10)) size <<= 1;
    return size - 1;
}

int croDefaultGroup(const std::vector<CroGroup>& groups) {
    for (int i = 0; i < (int)groups.size(); i++) if (groups[i].number == 0) return i;
    return groups.empty() ? -1 : 0;
}

Cartridge croCartridge(const CroGroup& group) {
    std::vector<const Bytes*> physical(BANK_COUNT, nullptr);
    int roms = 0;
    for (const CroRom& r : group.roms) {
        if (r.type != CRO_ROM_BANKABLE) continue;
        if (r.physical >= (uint32_t)BANK_COUNT) {
            throw std::runtime_error("The CRO ROM \"" + r.id + "\" has physical number " + std::to_string(r.physical)
                + "; a Plus cartridge has pages 0-31.");
        }
        physical[r.physical] = &r.data;
        roms += 1;
    }
    if (roms == 0) throw std::runtime_error("The CRO group holds no bankable (cartridge) ROMs.");
    Cartridge cart;
    cart.format = "CRO";
    cart.banks.assign(BANK_COUNT, Bytes{});
    for (int page = 0; page < BANK_COUNT; page++) {
        Bytes bank(BANK_SIZE, 0);
        for (int a = 0; a < BANK_SIZE; a++) {
            const uint32_t address = ((uint32_t)page << 14 | (uint32_t)a) & group.mask;
            const Bytes* rom = physical[address >> 14];   // below 1 << 19: page < 32
            const uint32_t offset = address & 0x3fff;
            if (rom && offset < rom->size()) bank[a] = (*rom)[offset];
        }
        cart.banks[page] = std::move(bank);
    }
    cart.bankCount = roms;
    return cart;
}

Cartridge parseCartridge(const Bytes& input) {
    const Bytes& bytes = input;
    if (isCro(bytes)) {
        const std::vector<CroGroup> groups = parseCro(bytes);
        int pick = croDefaultGroup(groups);
        if (!groups[pick].hasBankable())
            for (int i = 0; i < (int)groups.size(); i++) if (groups[i].hasBankable()) { pick = i; break; }
        if (!groups[pick].hasBankable()) throw std::runtime_error("The CRO holds CPC ROMs (low, high, Multiface II), not a cartridge.");
        return croCartridge(groups[pick]);
    }
    std::vector<Bytes> banks(BANK_COUNT);
    bool isCpr = bytes.size() >= 12 && ascii(bytes, 0, 4) == "RIFF" && ascii(bytes, 8, 4) == "AMS!";

    if (!isCpr) {
        if (bytes.size() == 0 || (int)bytes.size() > BANK_COUNT * BANK_SIZE || bytes.size() % BANK_SIZE != 0) {
            throw std::runtime_error("A raw cartridge must be 16-512 KiB and aligned to 16 KiB banks.");
        }
        int total = (int)bytes.size() / BANK_SIZE;
        for (int bank = 0; bank < total; bank += 1) {
            banks[bank] = Bytes(bytes.begin() + bank * BANK_SIZE, bytes.begin() + (bank + 1) * BANK_SIZE);
        }
        return { "RAW", banks, total };
    }

    int offset = 12;
    while (offset + 8 <= (int)bytes.size()) {
        std::string id = ascii(bytes, offset, 4);
        int length = (int)readU32LE(bytes, offset + 4);
        offset += 8;
        if (length < 0 || length > (int)bytes.size() - offset) {   // never offset + length: it can overflow
            char buf[64]; std::snprintf(buf, sizeof(buf), "Invalid CPR chunk length at offset 0x%x.", offset - 8);
            throw std::runtime_error(buf);
        }
        if (isCbId(id)) {
            int bank = std::stoi(id.substr(2), nullptr, 10);
            if (bank < BANK_COUNT) {
                Bytes content(BANK_SIZE, 0);
                int copy = std::min(length, BANK_SIZE);
                for (int k = 0; k < copy; k++) content[k] = bytes[offset + k];
                banks[bank] = content;
            }
        }
        offset += length;
    }
    int bankCount = 0; for (auto& b : banks) if (!b.empty()) bankCount++;
    if (bankCount == 0) throw std::runtime_error("The CPR contains no cbNN cartridge banks.");
    return { "CPR", banks, bankCount };
}

} // namespace cpcse
