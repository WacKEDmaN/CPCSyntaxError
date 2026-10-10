// CPCSyntaxError — HxC Floppy Emulator (HFE) images: what a Gotek with FlashFloppy or HxC
// firmware takes.
//
// The format (HxC2001's "HFE file format" description, revision 0, "HXCPICFE"):
//   block 0          the header: 8-byte signature, revision, cylinders, sides, encoding,
//                    bit rate (kbit/s), RPM, interface mode, a spare byte, the track list's
//                    block, write-allowed, single-step, then the track 0 alternate encodings
//   track list       per cylinder: the first block of its data and its length in bytes (both
//                    sides together)
//   track data       512-byte blocks, the first 256 bytes side 0's and the next 256 side 1's;
//                    each byte eight MFM cells, the first in time its lowest bit
// A track is the disc's whole revolution as a stream of cells, not its sectors: mfm.h reads
// the sectors out of it. A disc written back keeps the cells of every track the CPC did not
// change and re-encodes those it did (mfmEncodeTrack), so a protected disc's untouched tracks
// go back bit for bit. HFE v3 ("HXCHFEV3", with its opcodes) is not read.
#pragma once
#include "common.h"
#include "dsk.h"

namespace cpcse {

std::shared_ptr<Disk> parseHfe(const Bytes& input);
Bytes serializeHfe(const Disk& disk);

// Any disc image this emulator reads, by its signature (DSK, extended DSK, HFE, IPF), and the
// image it is written back as (its Disk::format).
std::shared_ptr<Disk> parseDiskImage(const Bytes& input);
Bytes serializeDiskImage(const Disk& disk);

} // namespace cpcse
