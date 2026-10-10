// CPCSyntaxError — IBM-format MFM tracks: a track's bitcells to its sectors, and back.
//
// What a µPD765A reads off a double-density track (the IBM System 34 layout the CPC uses):
//   a sync of three A1 bytes with a clock bit missing (cells 0x4489), then an address mark:
//   FE   an ID field: C, H, R, N and a CRC
//   FB   a data field (F8: deleted data) of 128 << N bytes and a CRC
// The CRC is CCITT (x^16 + x^12 + x^5 + 1, from FFFF) over the three A1s, the mark and
// the bytes. The track is a ring: a field that runs past the index wraps round to its start,
// as the drive's head does -- how an oversized (N=6) protection sector reads.
// Sectors are given what an extended DSK records of them: ST1 DE (&20) for a CRC error in
// the ID or the data, ST2 DD (&20) for one in the data, ST2 CM (&40) for deleted data, and
// ST1 MA / ST2 MD (&01) for an ID with no data field.
#pragma once
#include "common.h"
#include "dsk.h"

namespace cpcse {

// Cells are one bit a byte (0 or 1), in time order.
void mfmDecodeTrack(const std::vector<uint8_t>& cells, int cylinder, int side, Track& out);
// A standard track of these sectors: GAP 4a, the index mark, then each sector's ID and data
// fields with GAP 2 and the track's own GAP 3. At least `minCells` long (padded with 4E).
std::vector<uint8_t> mfmEncodeTrack(const Track& track, size_t minCells);
uint16_t mfmCrc(const uint8_t* data, size_t n, uint16_t crc = 0xffff);

} // namespace cpcse
