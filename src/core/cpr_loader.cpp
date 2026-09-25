// CPCSyntaxError — CPR cartridge loader.
#include "cpr_loader.h"
#include <stdexcept>
#include <cctype>

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

Cartridge parseCartridge(const Bytes& input) {
    const Bytes& bytes = input;
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
        if (length < 0 || offset + length > (int)bytes.size()) {
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
