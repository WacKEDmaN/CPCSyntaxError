#include "png.h"
#include <cstdio>
#include <vector>

namespace cpcse {

// ---------------------------------------------------------------- checksums
// PNG needs two different ones and they are easy to confuse: CRC-32 guards each
// chunk, Adler-32 guards the zlib stream inside the IDAT chunk. Using the wrong
// one produces a file that some readers accept and others reject, which is a
// miserable thing to debug.
static uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}

static void chunk(std::vector<uint8_t>& out, const char* type,
                  const uint8_t* data, size_t n) {
    put32(out, uint32_t(n));
    const size_t start = out.size();
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(type[i]));
    out.insert(out.end(), data, data + n);
    put32(out, crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

bool writePng(const std::string& path, const uint8_t* pixels, int width, int height) {
    if (width <= 0 || height <= 0) return false;

    // The raw image: each row prefixed by a filter byte. 0 means "no filter",
    // which is what makes the stored-block approach viable - a filtered row
    // would need the compressor to earn its keep.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), pixels + size_t(y) * width * 3,
                              pixels + size_t(y + 1) * width * 3);
    }

    // zlib wrapper: 0x78 0x01 is "deflate, 32K window, no preset dictionary,
    // fastest". Then stored blocks of at most 65535 bytes, each with its length
    // and the one's complement of its length, and the final block flagged.
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t n = (raw.size() - pos > 65535) ? 65535 : (raw.size() - pos);
        const bool last = (pos + n) >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));       z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));      z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
    }
    const uint32_t ad = adler32(raw.data(), raw.size());
    z.push_back(uint8_t(ad >> 24)); z.push_back(uint8_t(ad >> 16));
    z.push_back(uint8_t(ad >> 8));  z.push_back(uint8_t(ad));

    std::vector<uint8_t> out;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    out.insert(out.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = uint8_t(width >> 24);  ihdr[1] = uint8_t(width >> 16);
    ihdr[2] = uint8_t(width >> 8);   ihdr[3] = uint8_t(width);
    ihdr[4] = uint8_t(height >> 24); ihdr[5] = uint8_t(height >> 16);
    ihdr[6] = uint8_t(height >> 8);  ihdr[7] = uint8_t(height);
    ihdr[8]  = 8;    // bits per channel
    ihdr[9]  = 2;    // colour type 2 = truecolour RGB
    ihdr[10] = 0;    // deflate
    ihdr[11] = 0;    // adaptive filtering
    ihdr[12] = 0;    // no interlace
    chunk(out, "IHDR", ihdr, sizeof ihdr);
    chunk(out, "IDAT", z.data(), z.size());
    chunk(out, "IEND", nullptr, 0);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;   // the last of it is written at the close
}

} // namespace cpcse
