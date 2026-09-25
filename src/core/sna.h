// CPCSyntaxError — CPC SNA snapshot codec.
// Reads/writes V1-V3, RASM supersnapshot ROMs and REMU debug metadata.
#pragma once
#include "common.h"
#include "debug_symbols.h"

namespace cpcse {

class GX4000;

struct Snapshot {
    Bytes header;
    Bytes ram;
    Bytes plus;
    bool hasPlus = false;
    DebugMetadata debugMetadata;
    int version = 0;
    int cpcType = 0;
};

Snapshot parseSna(const Bytes& input);
Bytes createSna(GX4000* emulator, bool compressed = false);
void applySna(GX4000* emulator, const Snapshot& snapshot);

} // namespace cpcse
