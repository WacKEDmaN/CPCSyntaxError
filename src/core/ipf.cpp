// CPCSyntaxError — IPF disc images. See ipf.h.
#include "ipf.h"
#include "mfm.h"
// The library's headers mark its functions cdecl, which is every function's convention off
// Windows anyway (and an attribute GCC warns about on 64-bit).
#if !defined(_WIN32) && !defined(__cdecl)
#define __cdecl
#endif
#include "CapsLibAll.h"
#include <mutex>
#include <stdexcept>

namespace cpcse {

namespace {

// The library keeps its images in tables of its own: one parse at a time.
std::mutex capsLock;

const int WEAK_REVOLUTIONS = 4;

struct CapsImage {
    SDWORD id = -1;
    explicit CapsImage(Bytes& data) {
        if (CAPSInit() != imgeOk) throw std::runtime_error("IPF: the SPS library did not start");
        id = CAPSAddImage();
        if (id < 0) { CAPSExit(); throw std::runtime_error("IPF: the SPS library has no room for an image"); }
        if (CAPSLockImageMemory(id, data.data(), (UDWORD)data.size(), DI_LOCK_MEMREF) != imgeOk) {
            CAPSRemImage(id); CAPSExit();
            throw std::runtime_error("IPF: not an image the SPS library can read");
        }
    }
    ~CapsImage() { CAPSUnlockAllTracks(id); CAPSUnlockImage(id); CAPSRemImage(id); CAPSExit(); }
};

} // namespace

std::shared_ptr<Disk> parseIpf(const Bytes& input) {
    std::lock_guard<std::mutex> hold(capsLock);
    Bytes data = input;                         // the library reads it in place (DI_LOCK_MEMREF)
    CapsImage image(data);
    if (CAPSLoadImage(image.id, DI_LOCK_TRKBIT) != imgeOk) throw std::runtime_error("IPF: the image would not load");
    CapsImageInfo info{};
    if (CAPSGetImageInfo(&info, image.id) != imgeOk) throw std::runtime_error("IPF: no image information");
    if (info.type != ciitFDD) throw std::runtime_error("IPF: not a floppy disc");
    if (info.maxcylinder > 99 || info.maxhead > 1 || info.mincylinder > info.maxcylinder) throw std::runtime_error("IPF: bad geometry");

    auto disk = std::make_shared<Disk>();
    disk->creator = "IPF";
    disk->tracks = (int)info.maxcylinder + 1;
    disk->sides = (int)info.maxhead + 1;
    disk->format = "ipf";
    disk->extended = true;
    disk->writeProtected = true;
    disk->trackData.assign((size_t)disk->tracks, std::vector<std::shared_ptr<Track>>((size_t)disk->sides));
    // cells in bits; weak areas re-made on every lock
    // DI_LOCK_INDEX: each track's cells from the index hole, so where a sector lies on them
    // is where it lies on the disc (the FDC's timing, fdc.h)
    const UDWORD flags = DI_LOCK_TRKBIT | DI_LOCK_UPDATEFD | DI_LOCK_TYPE | DI_LOCK_INDEX;
    for (UDWORD c = info.mincylinder; c <= info.maxcylinder; c++) {
        for (UDWORD h = info.minhead; h <= info.maxhead; h++) {
            std::vector<Track> reads;
            for (int rev = 0; rev < WEAK_REVOLUTIONS; rev++) {
                CapsTrackInfoT1 ti{};
                ti.type = 1;
                if (CAPSLockTrack(&ti, image.id, c, h, flags) != imgeOk) break;
                if ((ti.type & CTIT_MASK_TYPE) == ctitNoise || !ti.trackbuf) break;   // unformatted
                std::vector<uint8_t> cells(ti.tracklen);
                for (UDWORD i = 0; i < ti.tracklen; i++) cells[i] = (uint8_t)(ti.trackbuf[i / 8] >> (7 - (i & 7)) & 1);
                Track t;
                mfmDecodeTrack(cells, (int)c, (int)h, t);
                reads.push_back(std::move(t));
                if (!(ti.type & CTIT_FLAG_FLAKEY)) break;                             // one revolution is the track
            }
            CAPSUnlockTrack(image.id, c, h);
            if (reads.empty() || reads[0].sectors.empty()) continue;
            auto track = std::make_shared<Track>(reads[0]);
            // weak sectors: each revolution's different version of a sector, as one more copy
            for (size_t r = 1; r < reads.size(); r++) {
                if (reads[r].sectors.size() != track->sectors.size()) continue;
                for (size_t i = 0; i < track->sectors.size(); i++) {
                    Sector& s = track->sectors[i];
                    const Sector& o = reads[r].sectors[i];
                    if (o.r != s.r || o.data.empty()) continue;
                    bool known = false;
                    for (const Bytes& copy : s.data) known |= copy == o.data[0];
                    if (!known) s.data.push_back(o.data[0]);
                }
            }
            disk->trackData[c][h] = track;
        }
    }
    return disk;
}

} // namespace cpcse
