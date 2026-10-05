// CPCSyntaxError — Monitor colour transforms.
// The TUBE: colour, green phosphor or monochrome, as one transform on the RGB pins.
#pragma once
#include "common.h"

namespace cpcse {

extern const std::string MONITOR_MODE_COLOUR;
extern const std::string MONITOR_MODE_GREEN;
extern const std::string MONITOR_MODE_MONO;

std::string normaliseMonitorMode(const std::string& mode);
// What the tube shows for the RGB the GATE ARRAY is driving. There is no ink-indexed
// version of this: which colour a pen is belongs to that chip (ACCC 9.1), and the tube
// only ever sees three signals.
int monitorTransformRgb(int rgb, const std::string& mode);
// The green/mono set's displayed level (0..255) for a GATE ARRAY RGB: the CPC's LUM pin
// through the tube. Exposed for the oracle that checks the documented firmware order.
int monitorLumLevel(int rgb);

} // namespace cpcse
