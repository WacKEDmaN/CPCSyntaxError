// CPCSyntaxError — CPR cartridge loader.
// Parses RIFF AMS! cartridges and raw cartridge images into 16 KiB banks.
#pragma once
#include "common.h"

namespace cpcse {

inline constexpr int BANK_SIZE = 0x4000;
inline constexpr int BANK_COUNT = 32;

// { format, banks, bankCount } — banks[i] empty == null.
struct Cartridge {
    std::string format;
    std::vector<Bytes> banks;
    int bankCount = 0;
};

// Parses native CPR files as implemented by JavaCPC's CPRLoader.
Cartridge parseCartridge(const Bytes& input);

} // namespace cpcse
