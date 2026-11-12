// CPCSyntaxError: a PNG in memory, compressed. The MCP server (devserver.cpp) returns
// screenshots to an agent as image content, and core/png.h's writer stores the pixels
// without compression (about 1.2 MB for a 768x544 picture), which is too big to send.
// This one deflates with fixed Huffman codes and a hash-chain LZ77 search: a CPC screen
// is mostly runs and repeated lines, so it comes out at a few tens of KB.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cpcse {

// An 8-bit RGB image (width*height*3 bytes, row-major) as the bytes of a PNG file.
std::vector<uint8_t> encodePng(const uint8_t* rgb, int width, int height);

// The same bytes as base64 text (RFC 4648, with padding), for JSON.
std::string base64(const std::vector<uint8_t>& bytes);

// A zlib stream (RFC 1950/1951) of the bytes: fixed Huffman blocks, LZ77 window 32 KB.
std::vector<uint8_t> zlibCompress(const std::vector<uint8_t>& data);

} // namespace cpcse
