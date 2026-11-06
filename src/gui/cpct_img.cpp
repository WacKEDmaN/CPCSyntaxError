// CPCSyntaxError GUI — CPCtelera's image conversion. See cpct_img.h.
#include "cpct_img.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "png_decode.h"

namespace fs = std::filesystem;

namespace cpcse {

// img2cpc's firmware colours: each component 0, 128 or 255.
static const uint8_t FW_RGB[27][3] = {
    { 0, 0, 0 }, { 0, 0, 128 }, { 0, 0, 255 }, { 128, 0, 0 }, { 128, 0, 128 }, { 128, 0, 255 }, { 255, 0, 0 },
    { 255, 0, 128 }, { 255, 0, 255 }, { 0, 128, 0 }, { 0, 128, 128 }, { 0, 128, 255 }, { 128, 128, 0 },
    { 128, 128, 128 }, { 128, 128, 255 }, { 255, 128, 0 }, { 255, 128, 128 }, { 255, 128, 255 }, { 0, 255, 0 },
    { 0, 255, 128 }, { 0, 255, 255 }, { 128, 255, 0 }, { 128, 255, 128 }, { 128, 255, 255 }, { 255, 255, 0 },
    { 255, 255, 128 }, { 255, 255, 255 } };
static const int HW[27] = { 0x54, 0x44, 0x55, 0x5C, 0x58, 0x5D, 0x4C, 0x45, 0x4D, 0x56, 0x46, 0x57, 0x5E, 0x40,
                            0x5F, 0x4E, 0x47, 0x4F, 0x52, 0x42, 0x53, 0x5A, 0x59, 0x5B, 0x4A, 0x43, 0x4B };

int firmwareToHardware(int fw) { return fw >= 0 && fw < 27 ? HW[fw] : 0x54; }

namespace {

std::string hex2(int v) { char b[8]; std::snprintf(b, sizeof b, "%02x", v & 0xff); return b; }

// The byte of a group of pixels (palette indices), as the GATE ARRAY reads it.
uint8_t pack(const std::vector<uint8_t>& px) {
    std::vector<uint8_t> bits;
    if (px.size() == 2) bits = { 1, 4, 2, 8 };
    else if (px.size() == 4) bits = { 1, 2 };
    else bits = { 1 };
    uint8_t mask = 0x80, out = 0;
    for (uint8_t bit : bits)
        for (uint8_t p : px) { if (p & bit) out |= mask; mask >>= 1; }
    return out;
}

std::string sanitize(std::string s) {
    for (char& c : s) if (!std::isalnum((unsigned char)c)) c = '_';
    return s;
}

struct Tile {
    std::string name;
    int width = 0, height = 0, widthBytes = 0;
    std::vector<uint8_t> data, mask;
};
} // namespace

bool convertImage(const ImgConvert& job, std::string& why, std::vector<std::string>* written) {
    std::error_code ec;
    std::ifstream in(fs::u8path(job.image), std::ios::binary);
    if (!in) { why = "cannot read " + job.image; return false; }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int iw = 0, ih = 0;
    std::vector<uint8_t> rgba;
    if (!decodePng(file, iw, ih, rgba, why)) { why = job.image + ": " + why; return false; }
    if (job.mode < 0 || job.mode > 2) { why = "mode must be 0, 1 or 2"; return false; }
    const int perByte = job.mode == 0 ? 2 : job.mode == 1 ? 4 : 8;
    const int maxColours = job.mode == 0 ? 16 : job.mode == 1 ? 4 : 2;
    // The palette's colours: the firmware ones at img2cpc's RGB, or -rgbp's own. img2cpc's
    // UpdateMaxEntries: the mode's colours (+1 with a mask), or 16 for hardware sprites.
    std::vector<std::array<uint8_t, 3>> paletteRgb;
    if (!job.rgbPalette.empty()) {
        for (uint32_t v : job.rgbPalette) {
            if (v > 0xFFFFFF) { why = "RGB colour " + std::to_string(v) + " is not 0x000000-0xFFFFFF"; return false; }
            paletteRgb.push_back({ (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v });
        }
    } else {
        for (int fw : job.firmwarePalette) {
            if (fw < 0 || fw > 26) { why = "firmware colour " + std::to_string(fw) + " is not 0-26"; return false; }
            paletteRgb.push_back({ FW_RGB[fw][0], FW_RGB[fw][1], FW_RGB[fw][2] });
        }
    }
    const int maxEntries = job.hardwareSprites ? 16 : maxColours + 1;
    if (paletteRgb.empty() || (int)paletteRgb.size() > maxEntries) { why = "the palette has too many colours for the mode"; return false; }

    // the nearest palette entry (img2cpc's TPalette::getNearestIndex and Color::Distance)
    auto nearest = [&](const uint8_t* p) {
        int best = -1;
        double bestDist = 1e300;
        for (size_t i = 0; i < paletteRgb.size(); i++) {
            // the palette's colours are opaque (A 255), so "both transparent" never applies
            const uint8_t* c = paletteRgb[i].data();
            const int da = (int)p[3] - 255, dr = (int)p[0] - c[0], dg = (int)p[1] - c[1], db = (int)p[2] - c[2];
            const double d = std::sqrt((double)(da * da + dr * dr + dg * dg + db * db));
            if (d < bestDist) { bestDist = d; best = (int)i; }
        }
        return (uint8_t)best;
    };
    const uint8_t transparent = (uint8_t)job.transparentIndex;    // as img2cpc: -1 becomes 255
    const uint8_t maskAll = (uint8_t)(maxColours - 1);

    // -sh: img2cpc cuts 16x16 whatever the tile size given (extractConversionOptions)
    const int tw = job.hardwareSprites ? 16 : job.tileW > 0 ? job.tileW : iw, th = job.hardwareSprites ? 16 : job.tileH > 0 ? job.tileH : ih;
    const int tilesX = (iw + tw - 1) / tw, tilesY = (ih + th - 1) / th, total = tilesX * tilesY;
    const int digits = (int)std::log10((double)total) + 1;
    std::string stem = fs::u8path(job.image).filename().u8string();
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot != 0) stem = stem.substr(0, dot);
    stem = sanitize(stem);
    const std::string base = sanitize(job.baseName);

    // the order rows are taken in: img2cpc's scanline order (default 0-7; -g a gray code
    // over the tile's rows, as cpct_img2tileset builds it)
    std::vector<int> order;
    if (job.grayCode) for (int i = 0; i < th; i++) order.push_back(i ^ (i >> 1));
    else for (int i = 0; i < 8; i++) order.push_back(i);

    std::vector<Tile> tiles;
    int index = 0;
    for (int py = 0; py < ih; py += th) {
        const int h = std::min(th, ih - py);
        for (int px = 0; px < iw; px += tw) {
            const int w = std::min(tw, iw - px);
            Tile t;
            t.width = w; t.height = h; t.widthBytes = w / perByte;
            t.name = base.empty() ? stem : (job.absoluteBaseName ? base : base + "_" + stem);
            if (w != iw || h != ih) { char n[16]; std::snprintf(n, sizeof n, "_%0*d", digits, index); t.name += n; }
            auto byteAt = [&](int x, int row, bool flip) {
                std::vector<uint8_t> colours, masks;
                for (int o = 0; o < perByte; o++) {
                    uint8_t c;
                    if (x + o < w) {
                        const int sx = flip ? w - x - perByte + o : x + o;
                        c = nearest(&rgba[((size_t)(py + row) * iw + (px + sx)) * 4]);
                    } else c = transparent;
                    colours.push_back(c);
                    masks.push_back(c == transparent ? maskAll : 0);
                }
                t.data.push_back(pack(colours));
                t.mask.push_back(pack(masks));
            };
            if (job.hardwareSprites) {
                // img2cpc 1.04a fillTileHardSprite: 16x16, one byte a pixel, its pen 0-15; pen 0
                // for a fully transparent pixel, for the transparent index, and outside the image;
                // -z mirrors the odd rows. No mask data (NoMaskData).
                t.width = t.height = t.widthBytes = 16;
                for (int y = 0; y < 16; y++) {
                    const bool flip = job.zigZag && (y & 1);
                    for (int x = 0; x < 16; x++) {
                        const int sx = flip ? 15 - x : x;
                        uint8_t pen = 0;
                        if (sx < w && y < h) {
                            const uint8_t* p = &rgba[((size_t)(py + y) * iw + (px + sx)) * 4];
                            if (p[3] != 0) {
                                const int idx = nearest(p);
                                pen = (job.transparentIndex >= 0 && idx == job.transparentIndex) ? 0 : (uint8_t)(idx & 0x0F);
                            }
                        }
                        t.data.push_back(pen);
                        t.mask.push_back(0);
                    }
                }
            } else if (job.screen) {
                // eight 2K blocks, one a pixel row of each character line (img2cpc fillTileSCR)
                const int lines = (int)order.size();
                const int chars = h / lines + (h % lines ? 1 : 0);
                for (int y = 0; y < lines; y++) {
                    int remaining = 0x800, row = order[(size_t)y];
                    for (int ch = 0; ch < chars; ch++) {
                        if (row < h) {
                            for (int x = 0; x < w; x += perByte) { byteAt(x, row, false); remaining--; }
                            row += lines;
                        }
                    }
                    while (remaining-- > 0) { t.data.push_back(0); t.mask.push_back(0); }
                }
            } else {
                const int lines = (int)order.size();
                int lineIdx = 0;
                for (int y = 0; y < h; y++) {
                    const int charIdx = y / lines;
                    int row;
                    do {
                        row = charIdx * lines + order[(size_t)lineIdx];
                        lineIdx = (lineIdx + 1) % lines;
                    } while (row >= h);
                    const bool flip = job.zigZag && (y & 1);
                    for (int x = 0; x < w; x += perByte) byteAt(x, row, flip);
                }
            }
            tiles.push_back(std::move(t));
            index++;
        }
    }

    const bool masks = job.transparentIndex >= 0 && !job.hardwareSprites;
    const std::string paletteId = base.empty() ? "palette" : base + "_palette";
    const std::string tilesetId = base.empty() ? "tileset" : base + "_tileset";
    std::vector<int> hwPalette;
    if (job.hardwarePalette && job.rgbPalette.empty()) for (int fw : job.firmwarePalette) hwPalette.push_back(firmwareToHardware(fw));
    if (job.hardwarePalette && !job.rgbPalette.empty()) {
        // img2cpc GetPaletteAsHW: the nearest hardware colour, lowest value first on a tie
        for (const auto& c : paletteRgb) {
            int bestHw = -1;
            double bestDist = 1e300;
            for (int hw = 0x40; hw <= 0x5F; hw++) {
                const int fw = (int)(std::find(HW, HW + 27, hw) - HW);
                const int alias = hw == 0x50 ? 1 : hw == 0x48 ? 7 : hw == 0x41 ? 13 : hw == 0x51 ? 19 : hw == 0x49 ? 25 : fw;
                if (alias >= 27) continue;
                const int dr = c[0] - FW_RGB[alias][0], dg = c[1] - FW_RGB[alias][1], db = c[2] - FW_RGB[alias][2];
                const double d = std::sqrt((double)(dr * dr + dg * dg + db * db));
                if (d < bestDist) { bestDist = d; bestHw = hw; }
            }
            hwPalette.push_back(bestHw);
        }
    }
    const std::string created = "// Data created with Img2CPC - (c) Retroworks - 2007-2017\n";
    // cpct_img2tileset's renames, done on the text before it is written
    auto renamed = [&](std::string s) {
        auto swap = [&](const std::string& from, const std::string& to) {
            if (to.empty()) return;
            for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
        };
        if (!job.binary) { swap(paletteId, job.paletteName); if (job.tileset) swap(tilesetId, job.tilesetName); }
        return s;
    };
    auto put = [&](const std::string& path, const std::string& text, bool bin) {
        std::ofstream f(fs::u8path(path), bin ? std::ios::binary : std::ios::binary);
        f << text;
        if (written) written->push_back(path);
        return (bool)f;
    };
    const std::string outName = fs::u8path(job.outBase).filename().u8string();
    // img2cpc's guard: its -o name (the image as the .mk wrote it, without extension) + ".h"
    std::string guard = sanitize((job.label.empty() ? stem : job.label) + ".h");
    for (char& c : guard) c = (char)std::toupper((unsigned char)c);

    if (job.binary) {
        std::string bin, hs = ";; Data created with Img2CPC - (c) Retroworks - 2007-2017\n", h = created;
        for (int v : hwPalette) bin.push_back((char)v);
        unsigned total = (unsigned)hwPalette.size();
        std::string pal = paletteId;
        for (char& c : pal) c = (char)std::toupper((unsigned char)c);
        hs += ";; Palete constants\n" + pal + "_OFF  == 0\n" + pal + "_SIZE == " + std::to_string(total) + "\n\n";
        h += "// Palete constants\n#define " + pal + "_OFF    0\n#define " + pal + "_SIZE   " + std::to_string(total) + "\n\n";
        if (job.tileset) {
            const std::string a = " No tileset has been generated, because tilesets require absolute pointers and binary generation can only generate offsets.\n";
            const std::string b = " However, you can calculate your own tileset by adding up offsets of tiles to the address where the first tile starts.\n\n";
            hs += ";; " + a + ";; " + b;
            h += "// " + a + "// " + b;
        }
        for (const Tile& t : tiles) {
            const unsigned n = (unsigned)t.data.size();
            if (!n) continue;
            std::string def = t.name;
            for (char& c : def) c = (char)std::toupper((unsigned char)c);
            const std::string info = "Tile " + t.name + " - " + std::to_string(t.width) + "x" + std::to_string(t.height) + " pixels, " +
                                     std::to_string(t.widthBytes) + "x" + std::to_string(t.height) + (job.interlacedMasks && masks ? "x2" : "") + " bytes.\n";
            hs += ";; " + info;
            h += "// " + info;
            // cpct_img2tileset passes -osz: the sizes and offsets
            hs += def + "_OFF      == " + std::to_string(total) + "\n" + def + "_SIZE     == " + std::to_string(n) + "\n" + def + "_W        == " +
                  std::to_string(t.widthBytes) + "\n" + def + "_H        == " + std::to_string(t.height) + "\n";
            h += "#define " + def + "_OFF        " + std::to_string(total) + "\n#define " + def + "_SIZE       " + std::to_string(n) + "\n#define " + def +
                 "_W          " + std::to_string(t.widthBytes) + "\n#define " + def + "_H          " + std::to_string(t.height) + "\n";
            if (job.interlacedMasks && masks) {
                hs += ";; Mask data is interlaced (MASK BYTE, DATA BYTE).\n";
                h += "// Mask data is interlaced (MASK BYTE, DATA BYTE).\n";
                for (unsigned i = 0; i < n; i++) { bin.push_back((char)t.mask[i]); bin.push_back((char)t.data[i]); }
            } else {
                for (unsigned i = 0; i < n; i++) bin.push_back((char)t.data[i]);
                if (masks) {
                    total += n;
                    hs += def + "_MASK_OFF == " + std::to_string(total) + "\n";
                    h += "#define " + def + "_MASK_OFF   " + std::to_string(total) + "\n";
                    for (unsigned i = 0; i < n; i++) bin.push_back((char)t.mask[i]);
                }
            }
            total += n;
        }
        return put(job.outBase + ".h.s", hs, false) && put(job.outBase + ".h", h, false) && put(job.outBase + ".bin", bin, true);
    }

    // ---- .h
    std::ostringstream h;
    h << created << "#ifndef _" << guard << "_\n#define _" << guard << "_\n\n#include <types.h>\n";
    if (!hwPalette.empty()) h << "extern const u8 " << paletteId << "[" << hwPalette.size() << "];\n\n";
    if (job.tileset) {
        h << "extern u8* const " << tilesetId << "[" << tiles.size() << "];\n\n";
        if (!job.interlacedMasks && masks) h << "extern u8* const " << (base.empty() ? "" : base + "_") << "masks_tileset[" << tiles.size() << "];\n\n";
    }
    for (const Tile& t : tiles) {
        if (t.data.empty()) continue;
        std::string def = t.name;
        for (char& c : def) c = (char)std::toupper((unsigned char)c);
        h << "#define " << def << "_W " << t.widthBytes << "\n#define " << def << "_H " << t.height << "\n";
        if (job.interlacedMasks && masks) h << "extern const u8 " << t.name << "[2 * " << t.widthBytes << " * " << t.height << "];\n";
        else {
            h << "extern const u8 " << t.name << "[" << t.widthBytes << " * " << t.height << "];\n";
            if (masks) h << "extern const u8 " << t.name << "_mask[" << t.widthBytes << " * " << t.height << "];\n";
        }
    }
    h << "\n#endif\n";

    // ---- .c
    std::ostringstream c;
    c << "#include \"" << outName << ".h\"\n" << created;
    if (!hwPalette.empty()) {
        c << "// Palette uses hardware values.\nconst u8 " << paletteId << "[" << hwPalette.size() << "] = { ";
        for (size_t i = 0; i < hwPalette.size(); i++) c << (i ? ", " : "") << "0x" << hex2(hwPalette[i]);
        c << " };\n\n";
    }
    if (job.tileset && !tiles.empty()) {
        c << "u8* const " << tilesetId << "[" << tiles.size() << "] = { \n\t";
        for (size_t i = 0; i < tiles.size(); i++) c << (i ? ", " : "") << tiles[i].name;
        c << "\n};\n";
        if (masks && !job.interlacedMasks) {
            c << "u8* const " << base << "_masks_tileset[" << tiles.size() << "] = { \n\t";
            for (size_t i = 0; i < tiles.size(); i++) c << (i ? ", " : "") << tiles[i].name << "_mask";
            c << "\n};\n";
        }
    }
    auto rows = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>* b, int perRow, int count) {
        size_t at = 0;
        for (int y = 0; y < count; y++) {
            c << "\t";
            for (int x = 0; x < perRow; x++) {
                if (x) c << ", ";
                if (b) c << "0x" << hex2((*b)[at]) << ", ";
                c << "0x" << hex2(a[at]);
                at++;
            }
            if (at < a.size()) c << ",";
            c << "\n";
        }
    };
    for (const Tile& t : tiles) {
        if (t.data.empty()) continue;
        const bool inter = job.interlacedMasks && masks;
        c << "// Tile " << t.name << ": " << t.width << "x" << t.height << " pixels, " << t.widthBytes << "x" << t.height << (inter ? "x2" : "") << " bytes.\n";
        const int perRow = t.widthBytes, count = t.height;
        if (inter) {
            c << "// Mask data is interlaced (MASK BYTE, DATA BYTE).\nconst u8 " << t.name << "[2 * " << t.widthBytes << " * " << t.height << "] = {\n";
            rows(t.data, &t.mask, perRow, count);
            c << "};\n";
        } else {
            c << "const u8 " << t.name << "[" << t.widthBytes << " * " << t.height << "] = {\n";
            rows(t.data, nullptr, perRow, count);
            c << "};\n";
            if (masks) {
                c << "const u8 " << t.name << "_mask[" << t.widthBytes << " * " << t.height << "] = {\n";
                rows(t.mask, nullptr, perRow, count);
                c << "};\n";
            }
        }
        c << "\n";
    }
    return put(job.outBase + ".c", renamed(c.str()), false) && put(job.outBase + ".h", renamed(h.str()), false);
}

} // namespace cpcse
