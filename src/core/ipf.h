// CPCSyntaxError — IPF disc images (the Software Preservation Society's preservation format),
// through the SPS decoder library (third_party/capsimage; its licence: non-commercial use).
//
// The library hands over each track as the cells of one revolution; mfm.h reads the sectors
// out of them. A track the library marks flakey (weak bits: a protection that reads
// differently each time) is read over several revolutions, and a sector whose bytes differ
// between them keeps each version as a copy, as an extended DSK keeps a weak sector -- the
// FDC hands out a different copy on each read. An IPF is an original's image: it is write
// protected, and never written back.
#pragma once
#include "common.h"
#include "dsk.h"

namespace cpcse {

std::shared_ptr<Disk> parseIpf(const Bytes& input);

} // namespace cpcse
