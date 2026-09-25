// CPCSyntaxError — CPC disk-image codec.
// Parses and serializes standard and extended DSK track/sector structures.
#pragma once
#include "common.h"

namespace cpcse {

struct Sector {
    int c = 0, h = 0, r = 0, n = 0, st1 = 0, st2 = 0;
    std::vector<Bytes> data;   // one buffer per stored copy (weak sectors: >1)
    int weakIndex = 0;
};

struct Track {
    int cylinder = 0, side = 0;
    std::vector<Sector> sectors;
};

struct Disk {
    std::string creator;
    int tracks = 0, sides = 0;
    // trackData[cylinder][side] — nullptr for an unformatted track.
    std::vector<std::vector<std::shared_ptr<Track>>> trackData;
    bool modified = false;
};

std::shared_ptr<Disk> parseDsk(const Bytes& input);
Bytes serializeDsk(const Disk& disk);

} // namespace cpcse
