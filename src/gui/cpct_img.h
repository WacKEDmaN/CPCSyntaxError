// CPCSyntaxError GUI — CPCtelera's image conversion (IMG2SP / IMG2SPRITES) without img2cpc.
//
// CPCtelera converts its PNGs with img2cpc (Retroworks), called by cpct_img2tileset; on
// Windows that program is built from source by CPCtelera's Cygwin setup. When it is not
// there, this does what those two do, from their behaviour (img2cpc's README, options and
// output; cpct_img2tileset's command line):
//   * the image cut into tiles of tileW x tileH pixels, left to right, top to bottom; the
//     last ones narrower / shorter when the image does not divide;
//   * each pixel the palette entry nearest in A, R, G, B (straight distance; two fully
//     transparent colours count as equal), the palette being the firmware colours given,
//     each at img2cpc's RGB (components 0, 128, 255);
//   * pixels packed as the GATE ARRAY reads them: mode 0 two a byte (bits 7/3/5/1 pixel 0,
//     6/2/4/0 pixel 1), mode 1 four (bits 7/3, 6/2, 5/1, 4/0), mode 2 eight;
//   * a transparent palette index (-t, or 0 with interlaced masks) gives mask bytes: all
//     1s where transparent; interlaced as MASK, DATA pairs, or separate _mask arrays;
//   * rows in order, gray-code order (-g), every odd row mirrored (-z), or the screen's own
//     layout (-scr: eight 2K blocks);
//   * names: the base name, then "_" and the file's own name unless absolute (-abn), then
//     "_NN" (as many digits as the tile count needs) when the image holds several tiles;
//   * the output: a .c and .h (the arrays, a <base>_tileset unless -nt, a <base>_palette in
//     hardware values when asked, _W/_H defines with -osz), or a .bin with .h and .h.s
//     offsets; then cpct_img2tileset's renames of <base>_palette / <base>_tileset.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cpcse {

struct ImgConvert {
    std::string image;               // the PNG
    std::string outBase;             // the output, without extension (".c" / ".h" ... added)
    std::string label;               // the image as the .mk wrote it, no extension (the header guard)
    int mode = 0;
    std::vector<int> firmwarePalette = { 1, 24, 20, 6, 26, 0, 2, 8, 10, 12, 14, 16, 18, 22, 24, 16 };
    std::vector<uint32_t> rgbPalette;    // img2cpc -rgbp (IMG2SP SET_PALETTE_PLUS): 0xRRGGBB, used instead when set
    bool hardwareSprites = false;        // img2cpc -sh (cpctelera_next 1.04a): CPC+ 16x16 sprites, a pen a byte
    int tileW = 4, tileH = 4;        // pixels
    std::string baseName = "g_tile";
    bool absoluteBaseName = false;
    int transparentIndex = -1;
    bool interlacedMasks = false;
    bool tileset = true;
    std::string tilesetName;         // "" = <base>_tileset
    bool hardwarePalette = false;    // output the palette in hardware values
    std::string paletteName;         // "" = <base>_palette
    bool binary = false;             // .bin + .h + .h.s instead of .c + .h
    bool grayCode = false, zigZag = false, screen = false;
};

// Writes the files; false and `why` when it cannot.
bool convertImage(const ImgConvert& job, std::string& why, std::vector<std::string>* written = nullptr);

// CPCtelera's firmware-to-hardware colour table (cfg/modules/utils.mk _HWPAL).
int firmwareToHardware(int fw);

} // namespace cpcse
