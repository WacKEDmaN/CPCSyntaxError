// CPCSyntaxError — CPC disk-image codec.
// Parses and serializes standard ("MV - CPCEMU") and extended ("EXTENDED CPC DSK") images,
// track by track and sector by sector, keeping everything the formats hold: each track's
// header fields and each sector's ID, status bytes and every stored copy (weak sectors).
#pragma once
#include "common.h"

namespace cpcse {

struct Sector {
    int c = 0, h = 0, r = 0, n = 0, st1 = 0, st2 = 0;
    std::vector<Bytes> data;   // one buffer per stored copy (weak sectors: >1)
    int weakIndex = 0;
    // Where it lies on the track, in bytes from the index (-1: not known -- a DSK keeps no
    // positions; an HFE or IPF bitstream does): its ID field read (CRC and all), its first
    // data byte. The FDC times the disc from these when it has them (fdc.h).
    int idPos = -1, dataPos = -1;
};

struct Track {
    // Track-Info (the DSK specification): &10 track number, &11 side number, &12 data rate
    // and &13 recording mode (the extended format's later fields; 0 = not stated), &14 the
    // sector size N used to format it, &16 GAP#3, &17 the filler byte.
    int cylinder = 0, side = 0;
    int dataRate = 0, recordingMode = 0;
    int sizeCode = 2;
    int gap3 = 0x4e, filler = 0xe5;
    std::vector<Sector> sectors;
    int lengthBytes = 0;       // the whole track in bytes, index to index (0: not known)
};

// The image a disc was read from, kept by formats that are written back track by track
// (HFE: the original bitstream of every track the CPC has not written to).
struct DiskSource { virtual ~DiskSource() = default; };

struct Disk {
    std::string creator;
    int tracks = 0, sides = 0;
    // trackData[cylinder][side] — nullptr for an unformatted track.
    std::vector<std::vector<std::shared_ptr<Track>>> trackData;
    bool modified = false;
    bool extended = true;      // the image it came from was the extended format
    std::string format = "dsk";               // "dsk", "hfe" or "ipf": what it is written back as
    std::shared_ptr<DiskSource> source;      // that image, for a format that needs it
    bool writeProtected = false;             // the tab (IPF originals): writes end with NW
};

std::shared_ptr<Disk> parseDsk(const Bytes& input);
// The extended format, or (standard = true) the original one -- which can hold only a disc
// whose formatted tracks are all one size, with one full-size copy of every sector and no
// unformatted track; dskFitsStandard says whether it can, and why not.
Bytes serializeDsk(const Disk& disk, bool standard = false);
bool dskFitsStandard(const Disk& disk, std::string* why = nullptr);

} // namespace cpcse
