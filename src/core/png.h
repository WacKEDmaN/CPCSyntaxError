// Minimal PNG writer.
//
// Not emulation - but it lives in the core because the headless front end must
// be able to produce a picture without depending on anything outside it, and
// "anything" includes libpng and zlib. A capture path that needs a third-party
// library is a capture path that stops working on someone else's machine.
//
// The deflate stream uses STORED blocks only - no compression. That is a
// legitimate deflate stream and every PNG reader accepts it; the files are
// roughly the size of the raw pixels, which for a 768x272 screenshot is about
// 600K. Perfectly acceptable for something that exists to be looked at once,
// and it keeps this file at a size where it can be read and trusted rather
// than being a compressor nobody will audit.

#pragma once
#include <cstdint>
#include <string>

namespace cpcse {

// Write an 8-bit RGB image. `pixels` is width*height*3 bytes, row-major, no
// padding. Returns true on success.
bool writePng(const std::string& path, const uint8_t* pixels, int width, int height);

} // namespace cpcse
