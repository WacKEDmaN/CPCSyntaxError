// CPCSyntaxError — HFE disc images. See hfe.h.
#include "hfe.h"
#include "mfm.h"
#include "ipf.h"
#include <cstring>
#include <stdexcept>

namespace cpcse {

namespace {

const char SIGNATURE[] = "HXCPICFE";
const int CPC_DD_FLOPPYMODE = 6;     // the header's interface mode for a CPC drive

// What a disc read from an HFE keeps of it: the header, and per track its cells and the
// sectors read from them -- a track whose sectors are still those goes back as its cells.
struct HfeSource : DiskSource {
    Bytes header;
    struct Side { std::vector<uint8_t> cells; std::shared_ptr<Track> read; };
    std::vector<std::vector<Side>> tracks;     // [cylinder][side]
    int bitRate = 250, rpm = 300;
};

int u16(const Bytes& d, size_t o) { return d[o] | d[o + 1] << 8; }

bool sameTrack(const Track* a, const Track* b) {
    if (!a || !b) return a == b;
    if (a->sectors.size() != b->sectors.size()) return false;
    for (size_t i = 0; i < a->sectors.size(); i++) {
        const Sector& x = a->sectors[i];
        const Sector& y = b->sectors[i];
        if (x.c != y.c || x.h != y.h || x.r != y.r || x.n != y.n || x.st1 != y.st1 || x.st2 != y.st2 || x.data != y.data) return false;
    }
    return true;
}

// GAP 3 as the track was formatted: from the end of a data field's CRC to the next ID's
// mark, less that ID's 12 zeros, three A1s and the mark.
int measureGap3(const std::vector<uint8_t>& cells) {
    // the positions (cells) just after each mark byte, and the mark
    std::vector<std::pair<size_t, int>> marks;
    uint16_t shift = 0;
    for (size_t i = 0; i + 16 < cells.size(); i++) {
        shift = (uint16_t)(shift << 1 | cells[i]);
        if (shift != 0x4489) continue;
        size_t p = i + 1;
        auto word = [&](size_t q) { uint16_t v = 0; for (int k = 0; k < 16 && q + k < cells.size(); k++) v = (uint16_t)(v << 1 | cells[q + k]); return v; };
        int syncs = 1;
        while (syncs < 8 && word(p) == 0x4489) { p += 16; syncs++; }
        if (syncs < 3 || p + 16 > cells.size()) continue;
        int v = 0;
        for (int k = 0; k < 8; k++) v = v << 1 | cells[p + 2 * (size_t)k + 1];
        marks.push_back({ p + 16, v });
        i = p + 15;
        shift = 0;
    }
    for (size_t m = 0; m + 2 < marks.size(); m++) {
        if (marks[m].second != 0xfe || (marks[m + 1].second != 0xfb && marks[m + 1].second != 0xf8) || marks[m + 2].second != 0xfe) continue;
        int n = 0;
        for (int k = 0; k < 8; k++) n = n << 1 | cells[marks[m].first + 16 * 3 + 2 * (size_t)k + 1];
        const size_t end = marks[m + 1].first + 16 * (((size_t)128 << (n & 7)) + 2);
        if (end >= marks[m + 2].first) continue;
        const long gap = (long)((marks[m + 2].first - end) / 16) - 16;
        if (gap > 0 && gap < 255) return (int)gap;
    }
    return 0x4e;
}

} // namespace

std::shared_ptr<Disk> parseHfe(const Bytes& d) {
    if (d.size() < 512 || std::memcmp(d.data(), "HXCHFEV3", 8) == 0)
        throw std::runtime_error("HFE v3 images are not supported (save it as HFE revision 0)");
    if (std::memcmp(d.data(), SIGNATURE, 8) != 0) throw std::runtime_error("not an HFE image");
    if (d[8] != 0) throw std::runtime_error("HFE revision " + std::to_string(d[8]) + " is not supported");
    const int cylinders = d[9], sides = d[10];
    if (cylinders < 1 || sides < 1 || sides > 2) throw std::runtime_error("HFE: bad geometry");
    if (d[11] != 0 && d[11] != 0xff) throw std::runtime_error("HFE: only MFM (ISO/IBM) tracks are read");
    const size_t lut = (size_t)u16(d, 18) * 512;
    if (lut + (size_t)cylinders * 4 > d.size()) throw std::runtime_error("HFE: the track list is past the end of the file");

    auto src = std::make_shared<HfeSource>();
    src->header.assign(d.begin(), d.begin() + 512);
    src->bitRate = u16(d, 12) ? u16(d, 12) : 250;
    src->rpm = u16(d, 14) ? u16(d, 14) : 300;
    src->tracks.resize((size_t)cylinders, std::vector<HfeSource::Side>((size_t)sides));

    auto disk = std::make_shared<Disk>();
    disk->creator = "HFE";
    disk->tracks = cylinders;
    disk->sides = sides;
    disk->format = "hfe";
    disk->extended = true;
    disk->writeProtected = d[20] == 0;          // write_allowed
    disk->trackData.assign((size_t)cylinders, std::vector<std::shared_ptr<Track>>((size_t)sides));
    for (int c = 0; c < cylinders; c++) {
        const size_t offset = (size_t)u16(d, lut + (size_t)c * 4) * 512;
        const size_t bytes = (size_t)u16(d, lut + (size_t)c * 4 + 2) / 2;     // per side
        for (int s = 0; s < sides; s++) {
            std::vector<uint8_t>& cells = src->tracks[(size_t)c][(size_t)s].cells;
            cells.reserve(bytes * 8);
            for (size_t i = 0; i < bytes; i++) {
                const size_t at = offset + (i / 256) * 512 + (size_t)s * 256 + i % 256;
                if (at >= d.size()) break;              // a short last block
                for (int b = 0; b < 8; b++) cells.push_back((uint8_t)(d[at] >> b & 1));
            }
            auto t = std::make_shared<Track>();
            mfmDecodeTrack(cells, c, s, *t);
            if (t->sectors.empty()) continue;           // unformatted
            t->gap3 = measureGap3(cells);
            disk->trackData[(size_t)c][(size_t)s] = t;
            src->tracks[(size_t)c][(size_t)s].read = std::make_shared<Track>(*t);
        }
    }
    disk->source = src;
    return disk;
}

Bytes serializeHfe(const Disk& disk) {
    const HfeSource* src = dynamic_cast<const HfeSource*>(disk.source.get());
    const int cylinders = std::min(disk.tracks, 255), sides = std::clamp(disk.sides, 1, 2);
    const int bitRate = src ? src->bitRate : 250, rpm = src ? src->rpm : 300;
    // a revolution's cells: two per data bit
    const size_t revolution = (size_t)bitRate * 1000 * 2 * 60 / (size_t)rpm;

    std::vector<std::vector<std::vector<uint8_t>>> cells((size_t)cylinders, std::vector<std::vector<uint8_t>>((size_t)sides));
    for (int c = 0; c < cylinders; c++) {
        for (int s = 0; s < sides; s++) {
            const Track* now = (size_t)c < disk.trackData.size() && (size_t)s < disk.trackData[(size_t)c].size()
                ? disk.trackData[(size_t)c][(size_t)s].get() : nullptr;
            const HfeSource::Side* was = src && (size_t)c < src->tracks.size() && (size_t)s < src->tracks[(size_t)c].size()
                ? &src->tracks[(size_t)c][(size_t)s] : nullptr;
            std::vector<uint8_t>& out = cells[(size_t)c][(size_t)s];
            if (was && sameTrack(now, was->read.get())) out = was->cells;
            else if (now) out = mfmEncodeTrack(*now, was && !was->cells.empty() ? was->cells.size() : revolution);
            else { Track blank; out = mfmEncodeTrack(blank, revolution); }   // unformatted: gap bytes
        }
    }

    Bytes header = src ? src->header : Bytes(512, 0xff);
    if (!src) {
        std::memcpy(header.data(), SIGNATURE, 8);
        header[8] = 0;
        header[11] = 0;                                  // ISOIBM_MFM_ENCODING
        header[12] = (uint8_t)(bitRate & 0xff); header[13] = (uint8_t)(bitRate >> 8);
        header[14] = (uint8_t)(rpm & 0xff); header[15] = (uint8_t)(rpm >> 8);
        header[16] = CPC_DD_FLOPPYMODE;
        header[17] = 0;
        header[20] = 0xff;                               // write allowed
        header[21] = 0xff;                               // single step
    }
    header[9] = (uint8_t)cylinders;
    header[10] = (uint8_t)sides;
    header[18] = 1; header[19] = 0;                      // the track list in block 1
    const size_t lutBlocks = ((size_t)cylinders * 4 + 511) / 512;

    Bytes out = header;
    out.resize(512 * (1 + lutBlocks), 0xff);
    for (int c = 0; c < cylinders; c++) {
        size_t bytes = 0;
        for (int s = 0; s < sides; s++) bytes = std::max(bytes, (cells[(size_t)c][(size_t)s].size() + 7) / 8);
        if (bytes * 2 > 0xffff) throw std::runtime_error("HFE: track " + std::to_string(c) + " is too long");
        const size_t block = out.size() / 512;
        out[512 + (size_t)c * 4] = (uint8_t)(block & 0xff);
        out[512 + (size_t)c * 4 + 1] = (uint8_t)(block >> 8);
        out[512 + (size_t)c * 4 + 2] = (uint8_t)(bytes * 2 & 0xff);
        out[512 + (size_t)c * 4 + 3] = (uint8_t)(bytes * 2 >> 8);
        const size_t blocks = (bytes + 255) / 256;
        const size_t base = out.size();
        out.resize(base + blocks * 512, 0x55);           // what HxC pads a short chunk with
        for (int s = 0; s < 2; s++) {
            const std::vector<uint8_t>* t = s < sides ? &cells[(size_t)c][(size_t)s] : nullptr;
            for (size_t i = 0; i < bytes; i++) {
                uint8_t v = 0x55;
                if (t) { v = 0; for (int b = 0; b < 8; b++) { const size_t k = i * 8 + (size_t)b; v |= (uint8_t)((k < t->size() ? (*t)[k] : (b & 1)) << b); } }
                out[base + (i / 256) * 512 + (size_t)s * 256 + i % 256] = v;
            }
        }
        if (block > 0xffff) throw std::runtime_error("HFE: the image is too large");
    }
    return out;
}

std::shared_ptr<Disk> parseDiskImage(const Bytes& input) {
    if (input.size() >= 8 && (std::memcmp(input.data(), SIGNATURE, 8) == 0 || std::memcmp(input.data(), "HXCHFEV3", 8) == 0))
        return parseHfe(input);
    if (input.size() >= 4 && std::memcmp(input.data(), "CAPS", 4) == 0)
        return parseIpf(input);
    return parseDsk(input);
}

Bytes serializeDiskImage(const Disk& disk) {
    if (disk.format == "hfe") return serializeHfe(disk);
    if (disk.format == "ipf") throw std::runtime_error("an IPF original is not written to");
    return serializeDsk(disk, !disk.extended && dskFitsStandard(disk));
}

} // namespace cpcse
