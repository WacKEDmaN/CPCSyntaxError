// CPCSyntaxError — video and audio recording (see media_recorder.h).
#include "media_recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cpcse {

// ============================================================== JPEG (baseline, 4:4:4)
// ITU T.81 Annex K: the example quantisation tables (in natural order) and Huffman tables.
namespace {
const uint8_t kZigzag[64] = { 0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
const uint8_t kLumQ[64] = { 16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99 };
const uint8_t kChromQ[64] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };
const uint8_t kDcLumBits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
const uint8_t kDcChromBits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
const uint8_t kDcValues[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
const uint8_t kAcLumBits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
const uint8_t kAcLumValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };
const uint8_t kAcChromBits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
const uint8_t kAcChromValues[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };
// AAN's scale of each of the 8 outputs of the fast DCT below
const float kAan[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f, 1.0f, 0.785694958f, 0.541196100f, 0.275899379f };

struct Huffman { uint16_t code[256] = {}; uint8_t size[256] = {}; };
Huffman makeHuffman(const uint8_t bits[16], const uint8_t* values) {
    Huffman h;
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++, k++) { h.code[values[k]] = (uint16_t)code++; h.size[values[k]] = (uint8_t)len; }
        code <<= 1;
    }
    return h;
}
struct JpegTables {
    Huffman dcLum, dcChrom, acLum, acChrom;
    JpegTables()
        : dcLum(makeHuffman(kDcLumBits, kDcValues)), dcChrom(makeHuffman(kDcChromBits, kDcValues)),
          acLum(makeHuffman(kAcLumBits, kAcLumValues)), acChrom(makeHuffman(kAcChromBits, kAcChromValues)) {}
};
const JpegTables& jpegTables() { static const JpegTables t; return t; }

struct JpegBits {
    std::vector<uint8_t>& out;
    uint32_t acc = 0; int n = 0;
    explicit JpegBits(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t bits, int count) {      // MSB first, 0xFF stuffed
        acc = acc << count | (bits & ((1u << count) - 1)); n += count;
        while (n >= 8) {
            const uint8_t b = (uint8_t)(acc >> (n - 8));
            out.push_back(b);
            if (b == 0xff) out.push_back(0);
            n -= 8;
        }
    }
    void flush() { if (n) put(0x7f, 8 - n); }
};

// IJG's floating-point AAN forward DCT, one row or column of 8 (stride apart)
inline void fdct8(float* d, int s) {
    const float t0 = d[0] + d[7 * s], t7 = d[0] - d[7 * s], t1 = d[s] + d[6 * s], t6 = d[s] - d[6 * s];
    const float t2 = d[2 * s] + d[5 * s], t5 = d[2 * s] - d[5 * s], t3 = d[3 * s] + d[4 * s], t4 = d[3 * s] - d[4 * s];
    float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
    d[0] = t10 + t11; d[4 * s] = t10 - t11;
    const float z1 = (t12 + t13) * 0.707106781f;
    d[2 * s] = t13 + z1; d[6 * s] = t13 - z1;
    t10 = t4 + t5; t11 = t5 + t6; t12 = t6 + t7;
    const float z5 = (t10 - t12) * 0.382683433f, z2 = 0.541196100f * t10 + z5, z4 = 1.306562965f * t12 + z5, z3 = t11 * 0.707106781f;
    const float z11 = t7 + z3, z13 = t7 - z3;
    d[5 * s] = z13 + z2; d[3 * s] = z13 - z2; d[s] = z11 + z4; d[7 * s] = z11 - z4;
}

void jpegBlock(JpegBits& bits, float* block, const float* divisor, int& dc, const Huffman& dcH, const Huffman& acH) {
    for (int r = 0; r < 8; r++) fdct8(block + r * 8, 1);
    for (int c = 0; c < 8; c++) fdct8(block + c, 8);
    int q[64];
    for (int k = 0; k < 64; k++) {
        const float v = block[kZigzag[k]] * divisor[kZigzag[k]];
        q[k] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    auto category = [](int v) { int a = v < 0 ? -v : v, c = 0; while (a) { c++; a >>= 1; } return c; };
    auto magnitude = [](int v, int c) { return (uint32_t)(v < 0 ? v + (1 << c) - 1 : v); };
    const int diff = q[0] - dc;
    dc = q[0];
    const int dcCat = category(diff);
    bits.put(dcH.code[dcCat], dcH.size[dcCat]);
    if (dcCat) bits.put(magnitude(diff, dcCat), dcCat);
    int last = 63;
    while (last > 0 && q[last] == 0) last--;
    int run = 0;
    for (int k = 1; k <= last; k++) {
        if (q[k] == 0) { run++; continue; }
        while (run >= 16) { bits.put(acH.code[0xf0], acH.size[0xf0]); run -= 16; }
        const int c = category(q[k]);
        const int sym = run << 4 | c;
        bits.put(acH.code[sym], acH.size[sym]);
        bits.put(magnitude(q[k], c), c);
        run = 0;
    }
    if (last < 63) bits.put(acH.code[0], acH.size[0]);
}
} // namespace

std::vector<uint8_t> encodeJpeg(const uint32_t* pixels, int width, int height, int quality) {
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t lq[64], cq[64];
    float lDiv[64], cDiv[64];
    for (int i = 0; i < 64; i++) {
        lq[i] = (uint8_t)std::clamp((kLumQ[i] * scale + 50) / 100, 1, 255);
        cq[i] = (uint8_t)std::clamp((kChromQ[i] * scale + 50) / 100, 1, 255);
        const float aan = kAan[i / 8] * kAan[i % 8] * 8.0f;
        lDiv[i] = 1.0f / (lq[i] * aan);
        cDiv[i] = 1.0f / (cq[i] * aan);
    }
    std::vector<uint8_t> out;
    out.reserve((size_t)width * height / 2 + 1024);
    auto u8 = [&](int v) { out.push_back((uint8_t)v); };
    auto u16 = [&](int v) { u8(v >> 8); u8(v & 0xff); };
    u16(0xffd8);
    // DQT: both tables, in zigzag order
    u16(0xffdb); u16(2 + 2 * 65);
    u8(0); for (int k = 0; k < 64; k++) u8(lq[kZigzag[k]]);
    u8(1); for (int k = 0; k < 64; k++) u8(cq[kZigzag[k]]);
    // SOF0: 8-bit, three components, none subsampled
    u16(0xffc0); u16(17); u8(8); u16(height); u16(width); u8(3);
    u8(1); u8(0x11); u8(0);
    u8(2); u8(0x11); u8(1);
    u8(3); u8(0x11); u8(1);
    // DHT: the four tables
    auto dht = [&](int cls, int id, const uint8_t bitsArr[16], const uint8_t* vals) {
        int n = 0; for (int i = 0; i < 16; i++) n += bitsArr[i];
        u16(0xffc4); u16(2 + 1 + 16 + n); u8(cls << 4 | id);
        for (int i = 0; i < 16; i++) u8(bitsArr[i]);
        for (int i = 0; i < n; i++) u8(vals[i]);
    };
    dht(0, 0, kDcLumBits, kDcValues); dht(1, 0, kAcLumBits, kAcLumValues);
    dht(0, 1, kDcChromBits, kDcValues); dht(1, 1, kAcChromBits, kAcChromValues);
    // SOS
    u16(0xffda); u16(12); u8(3);
    u8(1); u8(0x00); u8(2); u8(0x11); u8(3); u8(0x11);
    u8(0); u8(63); u8(0);
    const JpegTables& t = jpegTables();
    JpegBits bits(out);
    int dcY = 0, dcCb = 0, dcCr = 0;
    float y[64], cb[64], cr[64];
    for (int by = 0; by < height; by += 8)
        for (int bx = 0; bx < width; bx += 8) {
            for (int r = 0; r < 8; r++) {
                const int py = std::min(by + r, height - 1);
                const uint32_t* row = pixels + (size_t)py * width;
                for (int c = 0; c < 8; c++) {
                    const uint32_t p = row[std::min(bx + c, width - 1)];
                    const float R = (float)(p & 0xff), G = (float)(p >> 8 & 0xff), B = (float)(p >> 16 & 0xff);
                    y[r * 8 + c] = 0.299f * R + 0.587f * G + 0.114f * B - 128.0f;
                    cb[r * 8 + c] = -0.168735892f * R - 0.331264108f * G + 0.5f * B;
                    cr[r * 8 + c] = 0.5f * R - 0.418687589f * G - 0.081312411f * B;
                }
            }
            jpegBlock(bits, y, lDiv, dcY, t.dcLum, t.acLum);
            jpegBlock(bits, cb, cDiv, dcCb, t.dcChrom, t.acChrom);
            jpegBlock(bits, cr, cDiv, dcCr, t.dcChrom, t.acChrom);
        }
    bits.flush();
    u16(0xffd9);
    return out;
}

// ============================================================== deflate (RFC 1951)
namespace {
struct DeflateBits {
    std::vector<uint8_t>& out;
    uint32_t acc = 0; int n = 0;
    explicit DeflateBits(std::vector<uint8_t>& o) : out(o) {}
    void put(uint32_t bits, int count) {      // LSB first
        acc |= bits << n; n += count;
        while (n >= 8) { out.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void align() { if (n) put(0, 8 - n); }
};
uint32_t reverseBits(uint32_t v, int count) { uint32_t r = 0; for (int i = 0; i < count; i++) { r = r << 1 | (v & 1); v >>= 1; } return r; }
struct FixedCodes {
    uint16_t lit[288]; uint8_t litLen[288];
    uint16_t dist[30];
    uint8_t lengthCode[259], lengthExtra[259]; uint16_t lengthBase[29];
    FixedCodes() {
        for (int v = 0; v < 288; v++) {
            int code, len;
            if (v < 144) { code = 0x30 + v; len = 8; }
            else if (v < 256) { code = 0x190 + v - 144; len = 9; }
            else if (v < 280) { code = v - 256; len = 7; }
            else { code = 0xc0 + v - 280; len = 8; }
            lit[v] = (uint16_t)reverseBits((uint32_t)code, len); litLen[v] = (uint8_t)len;
        }
        for (int d = 0; d < 30; d++) dist[d] = (uint16_t)reverseBits((uint32_t)d, 5);
        static const uint16_t base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const uint8_t extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        for (int c = 0; c < 29; c++) {
            lengthBase[c] = base[c];
            const int top = c == 28 ? 258 : (c == 27 ? 257 : base[c + 1] - 1);
            for (int l = base[c]; l <= top && l <= 258; l++) { lengthCode[l] = (uint8_t)c; lengthExtra[l] = extra[c]; }
        }
    }
};
const FixedCodes& fixedCodes() { static const FixedCodes f; return f; }
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
int distCode(int d) { int c = 29; while (kDistBase[c] > d) c--; return c; }
} // namespace

void deflateSyncFlush(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    const FixedCodes& f = fixedCodes();
    DeflateBits bits(out);
    if (size) {
        bits.put(0, 1); bits.put(1, 2);          // not final; fixed Huffman codes
        constexpr int HASH_BITS = 15, WINDOW = 32768, MAX_CHAIN = 24;
        std::vector<int> head((size_t)1 << HASH_BITS, -1), prev(size, -1);
        auto hash = [&](size_t p) { return (int)(((uint32_t)data[p] << 10 ^ (uint32_t)data[p + 1] << 5 ^ data[p + 2]) & ((1u << HASH_BITS) - 1)); };
        auto insert = [&](size_t p) { if (p + 2 < size) { const int h = hash(p); prev[p] = head[(size_t)h]; head[(size_t)h] = (int)p; } };
        size_t i = 0;
        while (i < size) {
            int bestLen = 0, bestDist = 0;
            if (i + 2 < size) {
                const size_t maxLen = std::min<size_t>(258, size - i);
                int cand = head[(size_t)hash(i)];
                for (int chain = MAX_CHAIN; cand >= 0 && (int)i - cand <= WINDOW && chain > 0; chain--, cand = prev[(size_t)cand]) {
                    const uint8_t* a = data + cand; const uint8_t* b = data + i;
                    if (a[bestLen] != b[bestLen]) continue;
                    size_t l = 0;
                    while (l < maxLen && a[l] == b[l]) l++;
                    if ((int)l > bestLen) { bestLen = (int)l; bestDist = (int)i - cand; if (l == maxLen) break; }
                }
            }
            if (bestLen >= 3) {
                const int lc = f.lengthCode[bestLen];
                bits.put(f.lit[257 + lc], f.litLen[257 + lc]);
                if (f.lengthExtra[bestLen]) bits.put((uint32_t)(bestLen - f.lengthBase[lc]), f.lengthExtra[bestLen]);
                const int dc = distCode(bestDist);
                bits.put(f.dist[dc], 5);
                if (kDistExtra[dc]) bits.put((uint32_t)(bestDist - kDistBase[dc]), kDistExtra[dc]);
                for (int k = 0; k < bestLen; k++) insert(i + (size_t)k);
                i += (size_t)bestLen;
            } else {
                bits.put(f.lit[data[i]], f.litLen[data[i]]);
                insert(i);
                i++;
            }
        }
        bits.put(f.lit[256], f.litLen[256]);     // end of block
    }
    // the sync flush: an empty stored block, ending on a byte
    bits.put(0, 1); bits.put(0, 2);
    bits.align();
    out.push_back(0); out.push_back(0); out.push_back(0xff); out.push_back(0xff);
}

// ============================================================== the recorder
namespace {
constexpr uint64_t SPLIT_BYTES = 1000ull * 1024 * 1024;   // a new file past this
constexpr int ZMBV_BLOCK = 16;
constexpr uint64_t ZMBV_KEY_EVERY = 300;                 // frames between key frames (DOSBox's)
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void putId(std::vector<uint8_t>& b, const char* id) { b.insert(b.end(), id, id + 4); }
} // namespace

// ============================================================== ffmpeg's process
// A program with its standard input a pipe of ours (what it prints is thrown away); on
// Windows without a console window of its own.
struct MediaRecorder::Encoder {
#ifdef _WIN32
    HANDLE process = nullptr, input = nullptr;
    static std::wstring widen(const std::string& s) {
        if (s.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }
    // one argument as the C runtime reads it back: quoted when it holds a space or a quote
    static std::wstring quote(const std::wstring& a) {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
        std::wstring q = L"\"";
        size_t slashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { slashes++; continue; }
            if (c == L'"') q.append(slashes * 2 + 1, L'\\'); else q.append(slashes, L'\\');
            slashes = 0;
            q.push_back(c);
        }
        q.append(slashes * 2, L'\\');
        q.push_back(L'"');
        return q;
    }
    bool start(const std::vector<std::string>& args) {
        std::wstring line;
        for (const std::string& a : args) { if (!line.empty()) line += L' '; line += quote(widen(a)); }
        SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
        HANDLE readEnd = nullptr;
        if (!CreatePipe(&readEnd, &input, &sa, 1 << 20)) return false;
        SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = readEnd;
        si.hStdOutput = nul;
        si.hStdError = nul;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd(line.begin(), line.end());
        cmd.push_back(0);
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(readEnd);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { CloseHandle(input); input = nullptr; return false; }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        return true;
    }
    bool write(const void* data, size_t n) {
        const char* p = (const char*)data;
        while (n) {
            DWORD put = 0;
            if (!input || !WriteFile(input, p, (DWORD)std::min<size_t>(n, 1 << 20), &put, nullptr) || put == 0) return false;
            p += put; n -= put;
        }
        return true;
    }
    int finish() {
        if (input) { CloseHandle(input); input = nullptr; }
        if (!process) return -1;
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        process = nullptr;
        return (int)code;
    }
#else
    std::FILE* pipe = nullptr;
    bool start(const std::vector<std::string>& args) {
        std::string line;
        for (const std::string& a : args) {
            std::string q = "'";
            for (char c : a) { if (c == '\'') q += "'\\''"; else q += c; }
            line += (line.empty() ? "" : " ") + q + "'";
        }
        pipe = popen((line + " >/dev/null 2>&1").c_str(), "w");
        return pipe != nullptr;
    }
    bool write(const void* data, size_t n) { return pipe && std::fwrite(data, 1, n, pipe) == n; }
    int finish() {
        if (!pipe) return -1;
        const int status = pclose(pipe);
        pipe = nullptr;
        return status;
    }
#endif
    ~Encoder() { finish(); }
};

std::string MediaRecorder::findFfmpeg(const std::string& given) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!given.empty() && given != "ffmpeg" && fs::is_regular_file(fs::u8path(given), ec)) return given;
#ifdef _WIN32
    const char* names[] = { "ffmpeg.exe" };
    const char sep = ';';
#else
    const char* names[] = { "ffmpeg" };
    const char sep = ':';
#endif
    const char* env = std::getenv("PATH");
    const std::string path = env ? env : "";
    size_t a = 0;
    while (a <= path.size()) {
        size_t b = path.find(sep, a);
        if (b == std::string::npos) b = path.size();
        if (b > a)
            for (const char* n : names) {
                const fs::path f = fs::u8path(path.substr(a, b - a)) / n;
                if (fs::is_regular_file(f, ec)) return f.u8string();
            }
        a = b + 1;
    }
    return "";
}

// The pictures through ffmpeg into a video-only .mp4, the sound in a WAV: joined at the end.
bool MediaRecorder::finishH264() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const int code = encoder ? encoder->finish() : -1;
    encoder.reset();
    if (code != 0) { error = "ffmpeg could not encode the video (exit " + std::to_string(code) + ")"; return false; }
    Encoder mux;
    const bool started = mux.start({ ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", tempVideo, "-i", tempAudio,
                                     "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart", basePath });
    const int joined = started ? mux.finish() : -1;
    if (joined != 0) { error = "ffmpeg could not join the picture and the sound (exit " + std::to_string(joined) + "); they are in " + tempVideo + " and " + tempAudio; return false; }
    fs::remove(fs::u8path(tempVideo), ec);      // this recording's own temporary files, and only them
    fs::remove(fs::u8path(tempAudio), ec);
    totalBytes = (uint64_t)fs::file_size(fs::u8path(basePath), ec);
    return true;
}

MediaRecorder::MediaRecorder() = default;
MediaRecorder::~MediaRecorder() { stop(); }

bool MediaRecorder::start(const std::string& path, Format format, int width, int height, int sampleRate, double frameMicroseconds, int jpegQuality) {
    stop();
    error.clear();
    fmt = format; basePath = path; w = width; h = height; rate = sampleRate; frameUs = frameMicroseconds; quality = jpegQuality;
    framesAdded = 0; nextToWrite = 0; totalBytes = 0; fileIndex = 0; silenceCarry = 0;
    lastPicture.reset();
    todo.clear(); finished.clear();
    quit = false; splitQueued = false;
    if (fmt == Format::H264) {
        const std::string program = findFfmpeg(ffmpeg);
        if (program.empty()) { error = "ffmpeg was not found (install it, or name its program in the recording settings)"; return false; }
        ffmpeg = program;
        tempVideo = path + ".video.mp4";
        tempAudio = path + ".sound.wav";
        char fps[32];
        std::snprintf(fps, sizeof fps, "%.6f", 1e6 / frameMicroseconds);
        encoder = std::make_unique<Encoder>();
        const bool started = encoder->start({ ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgba",
                                              "-video_size", std::to_string(width) + "x" + std::to_string(height), "-framerate", fps, "-i", "-",
                                              "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-tune", "animation", "-pix_fmt", "yuv420p", tempVideo });
        if (!started) { encoder.reset(); error = "ffmpeg could not be started (" + ffmpeg + ")"; return false; }
    }
    if (!openFile()) { if (encoder) { encoder->finish(); encoder.reset(); } return false; }
    running = true;
    if (fmt != Format::Wav && fmt != Format::H264) {
        const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
        const unsigned n = std::min(4u, cores - 1);
        for (unsigned i = 0; i < n; i++) workers.emplace_back([this] { work(); });
    }
    return true;
}

void MediaRecorder::addFrame(const uint32_t* pixels, const int16_t* audio, size_t audioValues) {
    if (!running) return;
    auto job = std::make_shared<Job>();
    job->index = framesAdded++;
    // the sound, or the frame's length of silence
    if (audioValues) job->audio.assign(audio, audio + audioValues);
    else {
        silenceCarry += rate * frameUs / 1e6;
        const size_t n = (size_t)silenceCarry;
        silenceCarry -= (double)n;
        job->audio.assign(n * 2, 0);
    }
    if (fmt == Format::Wav || fmt == Format::H264) {
        // pixels 0xAABBGGRR are the bytes R, G, B, A: ffmpeg's "rgba"
        if (fmt == Format::H264 && encoder && !encoder->write(pixels, (size_t)w * h * 4)) {
            error = "ffmpeg stopped taking the pictures";
            running = false;
        }
        std::lock_guard<std::mutex> g(lock);
        writeChunk("01wb", job->audio.data(), (uint32_t)(job->audio.size() * 2), true);
        return;
    }
    job->picture = std::make_shared<std::vector<uint32_t>>(pixels, pixels + (size_t)w * h);
    job->previous = lastPicture;
    lastPicture = job->picture;
    std::unique_lock<std::mutex> g(lock);
    // a new file past the limit, beginning on a key frame
    if (!splitQueued && fileBytes + (uint64_t)(job->index - nextToWrite) * (uint64_t)w * h / 4 > SPLIT_BYTES) {
        job->newFile = true; job->key = true; splitQueued = true;
    }
    if (fmt == Format::Mjpeg || job->index % ZMBV_KEY_EVERY == 0 || !job->previous) job->key = true;
    // the machine waits when the encoders are this far behind
    done.wait(g, [&] { return job->index - nextToWrite < workers.size() * 2 + 2 || !running; });
    todo.push_back(job);
    wake.notify_one();
}

void MediaRecorder::work() {
    for (;;) {
        std::shared_ptr<Job> j;
        {
            std::unique_lock<std::mutex> g(lock);
            wake.wait(g, [&] { return quit || !todo.empty(); });
            if (todo.empty()) return;
            j = todo.front(); todo.pop_front();
        }
        encode(*j);
        std::lock_guard<std::mutex> g(lock);
        finished[j->index] = j;
        writeReady();
        done.notify_all();
    }
}

void MediaRecorder::encode(Job& j) {
    if (fmt == Format::Mjpeg) { j.video = encodeJpeg(j.picture->data(), w, h, quality); return; }
    // ZMBV: 32-bit pixels are B, G, R, 0
    const std::vector<uint32_t>& now = *j.picture;
    auto bgr0 = [](uint32_t p) { return (p & 0xff) << 16 | (p & 0xff00) | (p >> 16 & 0xff); };
    std::vector<uint8_t> raw;
    if (j.key) {
        raw.resize(now.size() * 4);
        for (size_t i = 0; i < now.size(); i++) { const uint32_t v = bgr0(now[i]); std::memcpy(&raw[i * 4], &v, 4); }
        j.video = { 1, 0, 1, 1, 8, ZMBV_BLOCK, ZMBV_BLOCK };   // key; version 0.1; zlib; 32 bpp; block size
        j.video.push_back(0x78); j.video.push_back(0x01);     // the zlib header: a new stream
    } else {
        // each block: its vector (0, 0) and whether XOR data follows; then that data
        const std::vector<uint32_t>& before = *j.previous;
        const int bx = (w + ZMBV_BLOCK - 1) / ZMBV_BLOCK, by = (h + ZMBV_BLOCK - 1) / ZMBV_BLOCK;
        raw.assign(((size_t)bx * by * 2 + 3) & ~(size_t)3, 0);
        for (int b = 0; b < bx * by; b++) {
            const int x0 = b % bx * ZMBV_BLOCK, y0 = b / bx * ZMBV_BLOCK;
            const int bw = std::min(ZMBV_BLOCK, w - x0), bh = std::min(ZMBV_BLOCK, h - y0);
            bool changed = false;
            for (int y = 0; y < bh && !changed; y++)
                changed = std::memcmp(&now[(size_t)(y0 + y) * w + x0], &before[(size_t)(y0 + y) * w + x0], (size_t)bw * 4) != 0;
            if (!changed) continue;
            raw[(size_t)b * 2] = 1;
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    const size_t i = (size_t)(y0 + y) * w + x0 + x;
                    const uint32_t v = bgr0(now[i]) ^ bgr0(before[i]);
                    const uint8_t* s = (const uint8_t*)&v;
                    raw.insert(raw.end(), s, s + 4);
                }
        }
        j.video = { 0 };
    }
    deflateSyncFlush(raw.data(), raw.size(), j.video);
}

void MediaRecorder::writeReady() {
    for (auto it = finished.find(nextToWrite); it != finished.end(); it = finished.find(nextToWrite)) {
        Job& j = *it->second;
        if (j.newFile && file) { closeFile(); openFile(); splitQueued = false; }
        if (file) {
            writeChunk("00dc", j.video.data(), (uint32_t)j.video.size(), j.key);
            writeChunk("01wb", j.audio.data(), (uint32_t)(j.audio.size() * 2), true);
        }
        finished.erase(it);
        nextToWrite++;
    }
}

void MediaRecorder::writeChunk(const char id[4], const void* data, uint32_t size, bool key) {
    if (!file) return;
    if (fmt == Format::Wav || fmt == Format::H264) {
        if (std::fwrite(data, 1, size, file) != size) error = "the disc is full";
        fileBytes += size; totalBytes += size;
        fileAudioSamples += size / 4;
        return;
    }
    IndexEntry e;
    std::memcpy(e.id, id, 4);
    e.flags = key ? 0x10 : 0;
    e.offset = (uint32_t)(std::ftell(file) - moviStart);
    e.size = size;
    fileIndexEntries.push_back(e);
    std::vector<uint8_t> head;
    putId(head, id); put32(head, size);
    std::fwrite(head.data(), 1, 8, file);
    if (std::fwrite(data, 1, size, file) != size) error = "the disc is full";
    if (size & 1) std::fputc(0, file);
    const uint64_t n = 8 + size + (size & 1);
    fileBytes += n; totalBytes += n;
    if (id[1] == '0') fileVideoFrames++; else fileAudioSamples += size / 4;
}

bool MediaRecorder::openFile() {
    std::string name = fmt == Format::H264 ? tempAudio : basePath;
    if (fileIndex > 0 && fmt != Format::H264) {
        const size_t dot = name.rfind('.');
        const std::string stem = dot == std::string::npos ? name : name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot);
        name = stem + "_" + std::to_string(fileIndex + 1) + ext;
    }
    file = std::fopen(name.c_str(), "wb");
    if (!file) { error = "cannot write " + name; running = false; return false; }
    fileIndex++;
    fileBytes = 0; fileVideoFrames = 0; fileAudioSamples = 0;
    fileIndexEntries.clear();
    std::vector<uint8_t> b;
    if (fmt == Format::Wav || fmt == Format::H264) {
        putId(b, "RIFF"); put32(b, 0); putId(b, "WAVE");
        putId(b, "fmt "); put32(b, 16); put16(b, 1); put16(b, 2); put32(b, (uint32_t)rate); put32(b, (uint32_t)rate * 4); put16(b, 4); put16(b, 16);
        putId(b, "data"); put32(b, 0);
        std::fwrite(b.data(), 1, b.size(), file);
        return true;
    }
    // the header; the counts and sizes are written again by closeFile
    const char* codec = fmt == Format::Mjpeg ? "MJPG" : "ZMBV";
    putId(b, "RIFF"); put32(b, 0); putId(b, "AVI ");
    putId(b, "LIST"); put32(b, 4 + 8 + 56 + (12 + 8 + 56 + 8 + 40) + (12 + 8 + 56 + 8 + 18)); putId(b, "hdrl");
    putId(b, "avih"); put32(b, 56);
    put32(b, (uint32_t)std::lround(frameUs));               // microseconds per frame
    put32(b, (uint32_t)(w * h * 4 / frameUs * 1e6));          // most bytes a second (a guess)
    put32(b, 0); put32(b, 0x10 | 0x100);                       // has an index; interleaved
    put32(b, 0); put32(b, 0); put32(b, 2);                     // frames (later), initial, streams
    put32(b, (uint32_t)(w * h * 4)); put32(b, (uint32_t)w); put32(b, (uint32_t)h);
    for (int i = 0; i < 4; i++) put32(b, 0);
    // the picture
    putId(b, "LIST"); put32(b, 4 + 8 + 56 + 8 + 40); putId(b, "strl");
    putId(b, "strh"); put32(b, 56);
    putId(b, "vids"); putId(b, codec); put32(b, 0); put16(b, 0); put16(b, 0); put32(b, 0);
    put32(b, (uint32_t)std::lround(frameUs)); put32(b, 1000000);  // scale / rate = frames a second
    put32(b, 0); put32(b, 0);                                  // start, length (later)
    put32(b, (uint32_t)(w * h * 4)); put32(b, 0xffffffffu); put32(b, 0);
    put16(b, 0); put16(b, 0); put16(b, (uint16_t)w); put16(b, (uint16_t)h);
    putId(b, "strf"); put32(b, 40);
    put32(b, 40); put32(b, (uint32_t)w); put32(b, (uint32_t)h);
    if (fmt == Format::Mjpeg) { put16(b, 1); put16(b, 24); } else put32(b, 0);   // ZMBV: 0, as DOSBox writes it
    putId(b, codec); put32(b, (uint32_t)(w * h * 4)); put32(b, 0); put32(b, 0); put32(b, 0); put32(b, 0);
    // the sound
    putId(b, "LIST"); put32(b, 4 + 8 + 56 + 8 + 18); putId(b, "strl");
    putId(b, "strh"); put32(b, 56);
    putId(b, "auds"); put32(b, 0); put32(b, 0); put16(b, 0); put16(b, 0); put32(b, 0);
    put32(b, 4); put32(b, (uint32_t)rate * 4);                 // a sample frame is 4 bytes
    put32(b, 0); put32(b, 0);                                  // start, length (later)
    put32(b, (uint32_t)rate); put32(b, 0xffffffffu); put32(b, 4);
    put16(b, 0); put16(b, 0); put16(b, 0); put16(b, 0);
    putId(b, "strf"); put32(b, 18);
    put16(b, 1); put16(b, 2); put32(b, (uint32_t)rate); put32(b, (uint32_t)rate * 4); put16(b, 4); put16(b, 16); put16(b, 0);
    putId(b, "LIST"); put32(b, 0); putId(b, "movi");
    std::fwrite(b.data(), 1, b.size(), file);
    moviStart = (long)b.size() - 4;                            // the 'movi' fourcc
    return true;
}

void MediaRecorder::closeFile() {
    if (!file) return;
    auto patch = [&](long at, uint32_t v) { std::fseek(file, at, SEEK_SET); uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; std::fwrite(b, 1, 4, file); };
    if (fmt == Format::Wav || fmt == Format::H264) {
        const long end = std::ftell(file);
        patch(4, (uint32_t)(end - 8));
        patch(40, (uint32_t)(end - 44));
    } else {
        const long moviEnd = std::ftell(file);
        std::vector<uint8_t> idx;
        putId(idx, "idx1"); put32(idx, (uint32_t)(fileIndexEntries.size() * 16));
        for (const IndexEntry& e : fileIndexEntries) { putId(idx, e.id); put32(idx, e.flags); put32(idx, e.offset); put32(idx, e.size); }
        std::fwrite(idx.data(), 1, idx.size(), file);
        const long end = std::ftell(file);
        patch(4, (uint32_t)(end - 8));
        patch(moviStart - 4, (uint32_t)(moviEnd - moviStart));
        // avih total frames; the picture's and the sound's lengths
        const long avih = 12 + 12 + 8;
        patch(avih + 16, fileVideoFrames);
        const long videoStrh = avih + 56 + 12 + 8;
        patch(videoStrh + 32, fileVideoFrames);
        const long audioStrh = videoStrh + 56 + 8 + 40 + 12 + 8;
        patch(audioStrh + 32, fileAudioSamples);
    }
    std::fclose(file);
    file = nullptr;
}

void MediaRecorder::stop() {
    if (!running && workers.empty() && !file) return;
    {
        std::unique_lock<std::mutex> g(lock);
        // every frame given is written
        done.wait(g, [&] { return nextToWrite >= framesAdded || fmt == Format::Wav || fmt == Format::H264 || !file; });
        quit = true;
    }
    wake.notify_all();
    for (std::thread& t : workers) t.join();
    workers.clear();
    running = false;
    done.notify_all();
    std::lock_guard<std::mutex> g(lock);
    closeFile();
    if (fmt == Format::H264 && encoder) finishH264();
}

} // namespace cpcse
