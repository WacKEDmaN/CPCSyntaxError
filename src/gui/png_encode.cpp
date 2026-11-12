// CPCSyntaxError: see png_encode.h. Deflate (RFC 1951) with fixed Huffman codes only:
// one block, literal/length codes 0-287 and 5-bit distance codes, so no code tables have
// to be built or sent. The match search keeps the last position of each 3-byte hash and a
// chain of earlier ones, and tries at most 64 of them.
#include "png_encode.h"

#include <algorithm>
#include <array>

namespace cpcse {
namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int count = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    // Deflate packs values from the least significant bit up.
    void bits(uint32_t value, int n) {
        acc |= value << count;
        count += n;
        while (count >= 8) { out.push_back((uint8_t)acc); acc >>= 8; count -= 8; }
    }
    // A Huffman code goes in most significant bit first.
    void code(uint32_t value, int n) {
        uint32_t r = 0;
        for (int i = 0; i < n; i++) r |= ((value >> i) & 1u) << (n - 1 - i);
        bits(r, n);
    }
    void flush() { if (count > 0) { out.push_back((uint8_t)acc); acc = 0; count = 0; } }
};

// RFC 1951 3.2.6: the fixed literal/length code.
void literal(BitWriter& w, int sym) {
    if (sym < 144) w.code(0x30 + sym, 8);
    else if (sym < 256) w.code(0x190 + (sym - 144), 9);
    else if (sym < 280) w.code(sym - 256, 7);
    else w.code(0xc0 + (sym - 280), 8);
}

const int LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                           67, 83, 99, 115, 131, 163, 195, 227, 258 };
const int LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const int DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const int DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

void match(BitWriter& w, int length, int distance) {
    int l = 28;
    while (LEN_BASE[l] > length) l--;
    literal(w, 257 + l);
    if (LEN_EXTRA[l]) w.bits((uint32_t)(length - LEN_BASE[l]), LEN_EXTRA[l]);
    int d = 29;
    while (DIST_BASE[d] > distance) d--;
    w.code((uint32_t)d, 5);
    if (DIST_EXTRA[d]) w.bits((uint32_t)(distance - DIST_BASE[d]), DIST_EXTRA[d]);
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t>& png, const char type[4], const std::vector<uint8_t>& data) {
    put32(png, (uint32_t)data.size());
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    put32(png, crc32(body.data(), body.size()));
}

} // namespace

std::vector<uint8_t> zlibCompress(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = { 0x78, 0x01 };   // deflate, 32 KB window, no dictionary
    BitWriter w(out);
    w.bits(1, 1);   // BFINAL
    w.bits(1, 2);   // BTYPE 01: fixed Huffman codes
    const size_t n = data.size();
    const int HASH = 1 << 15, WINDOW = 32768, MAXLEN = 258, TRIES = 64;
    std::vector<int> head(HASH, -1), prev(n, -1);
    auto hashAt = [&](size_t i) { return ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & (HASH - 1); };
    auto insert = [&](size_t i) { if (i + 2 < n) { const int h = hashAt(i); prev[i] = head[h]; head[h] = (int)i; } };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int cand = head[hashAt(i)];
            const size_t limit = std::min((size_t)MAXLEN, n - i);
            for (int t = 0; t < TRIES && cand >= 0 && (int)i - cand <= WINDOW; t++, cand = prev[(size_t)cand]) {
                size_t len = 0;
                while (len < limit && data[(size_t)cand + len] == data[i + len]) len++;
                if ((int)len > bestLen) { bestLen = (int)len; bestDist = (int)i - cand; if (len == limit) break; }
            }
        }
        if (bestLen >= 3) {
            match(w, bestLen, bestDist);
            for (int k = 0; k < bestLen; k++) insert(i + (size_t)k);
            i += (size_t)bestLen;
        } else {
            literal(w, data[i]);
            insert(i);
            i++;
        }
    }
    literal(w, 256);   // end of block
    w.flush();
    uint32_t a = 1, b = 0;   // Adler-32 of the uncompressed bytes
    for (uint8_t c : data) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put32(out, (b << 16) | a);
    return out;
}

std::vector<uint8_t> encodePng(const uint8_t* rgb, int width, int height) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)height * ((size_t)width * 3 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);   // filter: none (runs and repeated rows are the LZ77's to find)
        raw.insert(raw.end(), rgb + (size_t)y * width * 3, rgb + (size_t)(y + 1) * width * 3);
    }
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    std::vector<uint8_t> ihdr;
    put32(ihdr, (uint32_t)width);
    put32(ihdr, (uint32_t)height);
    ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 });   // 8-bit RGB, deflate, filter set 0, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", zlibCompress(raw));
    chunk(png, "IEND", {});
    return png;
}

std::string base64(const std::vector<uint8_t>& bytes) {
    static const char* const T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint32_t)bytes[i] << 16 | (uint32_t)bytes[i + 1] << 8 | bytes[i + 2];
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = (uint32_t)bytes[i] << 16;
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (uint32_t)bytes[i] << 16 | (uint32_t)bytes[i + 1] << 8;
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

} // namespace cpcse
