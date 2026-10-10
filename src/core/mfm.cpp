// CPCSyntaxError — IBM-format MFM tracks. See mfm.h.
#include "mfm.h"

namespace cpcse {

uint16_t mfmCrc(const uint8_t* data, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++) crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

namespace {

const uint16_t SYNC_A1 = 0x4489;    // A1 with the clock between bits 4 and 5 missing
const uint16_t SYNC_C2 = 0x5224;    // C2 likewise (the index address mark)

struct Ring {
    const std::vector<uint8_t>& c;
    size_t n;
    uint8_t at(size_t i) const { return c[i % n]; }
    // The data byte whose 16 cells start at i: the data bits are every second cell.
    int byte(size_t i) const {
        int v = 0;
        for (int k = 0; k < 8; k++) v = v << 1 | at(i + 2 * (size_t)k + 1);
        return v;
    }
    uint16_t word(size_t i) const {
        uint16_t v = 0;
        for (int k = 0; k < 16; k++) v = (uint16_t)(v << 1 | at(i + (size_t)k));
        return v;
    }
};

} // namespace

void mfmDecodeTrack(const std::vector<uint8_t>& cells, int cylinder, int side, Track& out) {
    out.cylinder = cylinder;
    out.side = side;
    out.sectors.clear();
    if (cells.size() < 1024) return;
    const Ring r{ cells, cells.size() };
    struct Mark { size_t pos; int mark; };   // pos: the first cell after the mark byte
    std::vector<Mark> marks;
    uint16_t shift = 0;
    for (size_t i = 0; i < cells.size(); i++) {
        shift = (uint16_t)(shift << 1 | cells[i]);
        if (shift != SYNC_A1) continue;
        // the end of one A1; more may follow, then the mark
        size_t p = i + 1;
        int syncs = 1;
        while (syncs < 8 && r.word(p) == SYNC_A1) { p += 16; syncs++; }
        if (syncs < 3) continue;                       // a real mark has three
        marks.push_back({ p + 16, r.byte(p) });
        i = p + 15;
        shift = 0;
    }
    const uint8_t a1[3] = { 0xa1, 0xa1, 0xa1 };
    for (size_t m = 0; m < marks.size(); m++) {
        if (marks[m].mark != 0xfe) continue;
        const size_t p = marks[m].pos;
        uint8_t id[7];
        for (int k = 0; k < 6; k++) id[k] = (uint8_t)r.byte(p + 16 * (size_t)k);
        uint8_t field[5] = { 0xfe, id[0], id[1], id[2], id[3] };
        const uint16_t crc = mfmCrc(field, 5, mfmCrc(a1, 3));
        const bool idOk = crc == (uint16_t)(id[4] << 8 | id[5]);
        Sector s;
        s.c = id[0]; s.h = id[1]; s.r = id[2]; s.n = id[3];
        s.idPos = (int)((p + 16 * 6) / 16 % (cells.size() / 16));   // C H R N and the CRC read
        const size_t size = (size_t)128 << (s.n & 7);
        // its data field: the next mark, if it is a data mark and close (GAP 2 is 22 bytes)
        const Mark* data = nullptr;
        if (idOk && m + 1 < marks.size() && (marks[m + 1].mark == 0xfb || marks[m + 1].mark == 0xf8) &&
            marks[m + 1].pos - p < 16 * 60)
            data = &marks[m + 1];
        Bytes bytes(size, 0xe5);
        if (!idOk) {
            s.st1 = 0x20;                                // CRC error in the ID field
        } else if (!data) {
            s.st1 = 0x01; s.st2 = 0x01;                  // no data address mark
        } else {
            s.dataPos = (int)(data->pos / 16 % (cells.size() / 16));
            for (size_t k = 0; k < size; k++) bytes[k] = (uint8_t)r.byte(data->pos + 16 * k);
            const uint16_t stored = (uint16_t)(r.byte(data->pos + 16 * size) << 8 | r.byte(data->pos + 16 * (size + 1)));
            const uint8_t dm = (uint8_t)data->mark;
            const uint16_t sum = mfmCrc(bytes.data(), size, mfmCrc(&dm, 1, mfmCrc(a1, 3)));
            if (sum != stored) { s.st1 |= 0x20; s.st2 |= 0x20; }
            if (dm == 0xf8) s.st2 |= 0x40;
        }
        s.data = { bytes };
        out.sectors.push_back(s);
    }
    if (!out.sectors.empty()) out.sizeCode = out.sectors[0].n & 7;
    out.lengthBytes = (int)(cells.size() / 16);
}

namespace {

struct Cells {
    std::vector<uint8_t> c;
    int last = 0;                        // the previous data bit, for the next clock
    void raw(uint16_t w) { for (int k = 15; k >= 0; k--) c.push_back((uint8_t)(w >> k & 1)); last = w & 1; }
    void byte(int v) {
        for (int k = 7; k >= 0; k--) {
            const int bit = v >> k & 1;
            c.push_back((uint8_t)(!last && !bit));       // the clock: 1 only between two zeros
            c.push_back((uint8_t)bit);
            last = bit;
        }
    }
    void bytes(int v, int n) { for (int i = 0; i < n; i++) byte(v); }
};

} // namespace

std::vector<uint8_t> mfmEncodeTrack(const Track& track, size_t minCells) {
    Cells o;
    o.bytes(0x4e, 80);                                   // GAP 4a
    o.bytes(0x00, 12);
    for (int i = 0; i < 3; i++) o.raw(SYNC_C2);
    o.byte(0xfc);                                        // the index address mark
    o.bytes(0x4e, 50);                                   // GAP 1
    const uint8_t a1[3] = { 0xa1, 0xa1, 0xa1 };
    const int gap3 = track.gap3 > 0 && track.gap3 < 255 ? track.gap3 : 0x4e;
    for (const Sector& s : track.sectors) {
        o.bytes(0x00, 12);
        for (int i = 0; i < 3; i++) o.raw(SYNC_A1);
        const uint8_t id[5] = { 0xfe, (uint8_t)s.c, (uint8_t)s.h, (uint8_t)s.r, (uint8_t)s.n };
        uint16_t crc = mfmCrc(id, 5, mfmCrc(a1, 3));
        const bool idBad = (s.st1 & 0x20) && !(s.st2 & 0x20);
        if (idBad) crc ^= 0xffff;                        // keep a recorded ID CRC error
        o.byte(0xfe);
        for (int k = 1; k < 5; k++) o.byte(id[k]);
        o.byte(crc >> 8); o.byte(crc & 0xff);
        o.bytes(0x4e, 22);                               // GAP 2
        if ((s.st1 & 0x01) && (s.st2 & 0x01)) { o.bytes(0x4e, gap3); continue; }   // no data field
        o.bytes(0x00, 12);
        for (int i = 0; i < 3; i++) o.raw(SYNC_A1);
        const uint8_t dm = (s.st2 & 0x40) ? 0xf8 : 0xfb;
        o.byte(dm);
        const size_t size = (size_t)128 << (s.n & 7);
        Bytes data(size, (uint8_t)track.filler);
        if (!s.data.empty()) for (size_t k = 0; k < size && k < s.data[0].size(); k++) data[k] = s.data[0][k];
        for (uint8_t b : data) o.byte(b);
        uint16_t dcrc = mfmCrc(data.data(), size, mfmCrc(&dm, 1, mfmCrc(a1, 3)));
        if (s.st2 & 0x20) dcrc ^= 0xffff;                // keep a recorded data CRC error
        o.byte(dcrc >> 8); o.byte(dcrc & 0xff);
        o.bytes(0x4e, gap3);                             // GAP 3
    }
    while (o.c.size() < minCells) o.byte(0x4e);          // GAP 4b, to the index
    return o.c;
}

} // namespace cpcse
