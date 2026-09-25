// CPCSyntaxError — NearlyQ matrix-printer font data.
#pragma once
#include "common.h"

namespace cpcse {

extern const int NEARLYQ_FONT_WIDTH;
extern const int NEARLYQ_FONT_HEIGHT;
extern const int NEARLYQ_PIN_ROWS;

struct MatrixGlyph {
    std::vector<std::string> bitmap;  // 18 rows of 11 chars ('#'/'.')
    int left = 0;
    int right = 0;
};

const std::unordered_map<std::string, MatrixGlyph>& nearlyqFont();

} // namespace cpcse
