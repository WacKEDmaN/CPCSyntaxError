// CPCSyntaxError — CP/M 2.2 system image.
// Boot loader and BDOS dumped from an Amstrad SYSTEM disk.
#pragma once
#include "common.h"

namespace cpcse {

// Returns the 9216-byte CP/M 2.2 system image.
Bytes cpm22SystemBytes();
// Size in bytes of the CP/M 2.2 system image (2 tracks x 9 sectors x 512).
int cpm22SystemSize();

} // namespace cpcse
