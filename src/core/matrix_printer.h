// CPCSyntaxError — Matrix printer.
// Renders text, ESC/P modes, bit images to an RGBA page buffer (headless).
#pragma once
#include "common.h"

namespace cpcse {

Bytes matrixPrinterSelfTestBytes();

struct PrinterDot { int x = 0, y = 0; double r = 1.15; double ry = 0; bool hasRy = false; double opacity = 0.82; };

struct BitImageState {
    double step = 0;
    double pinStep = 0;
    bool noAutoWrap = false;
    double stretchY = 1;
    bool hardcopy = false;
    double lineFeed = 0;
};

struct EscState {
    std::string state;
    int mode = 0;
    int length = 0;
    int remaining = 0;
    std::string alias;
    BitImageState bitImage;
    bool hasBitImage = false;
};

struct DotOptions { bool record = true; bool dense = false; double stretchY = 1; };

class MatrixPrinter {
public:
    int width, height;
    double dpiX, dpiY;
    std::vector<std::vector<uint8_t>> pages;   // RGBA page buffers
    std::vector<std::vector<PrinterDot>> dotPages;
    int activePageIndex = 0;
    std::function<void(const std::string&)> onSound;
    int soundDotCounter = 0;
    bool pendingNewPageSound = false;

    // print state
    int leftMargin = 0, rightMargin = 0, topMargin = 0, bottomMargin = 0;
    double lineSpacing = 0;
    int baseCpi = 10;
    bool condensed = false, bold = false, doubleStrike = false, italic = false, underline = false;
    bool doubleWidth = false, doubleWidthOneLine = false, halfHeight = false, nlq = false;
    double charWidth = 0;
    std::optional<EscState> esc;
    std::optional<double> pendingHardcopyLineFeed;
    std::unordered_map<char, int> graphicModes;
    double x = 0, y = 0;

    explicit MatrixPrinter(int width = 1240, int height = 1754);

    int pageNumber() const { return activePageIndex + 1; }
    int pageCount() const { return (int)pages.size(); }

    void reset();
    int verticalPinStep();
    void resetPrintState(bool home = true);
    void updateCharWidth();
    std::vector<uint8_t> createBlankPage();
    void clearPage(int index, bool keepPosition = false);
    void setPixelOn(std::vector<uint8_t>& page, double x, double y, const std::array<int, 4>& rgba);
    void setPixel(double x, double y, const std::array<int, 4>& rgba);
    void emitSound(const std::string& name);
    void dot(double x, double y, DotOptions options = {});
    void carriageReturn();
    void lineFeed();
    void lineFeed(double amount);
    void reverseLineFeed();
    void reverseLineFeed(double amount);
    void formFeed();
    void writeByte(int value);
    void handleEsc(int byte);
    int modeDpi(int mode);
    double densityStep(int mode);
    struct HardcopyProfile { bool present; int sourceColumns; bool fullWidth; };
    HardcopyProfile cpcHardcopyProfile(int mode, int length);
    BitImageState beginBitImage(int mode, int length);
    void printBitColumn(int byte, int mode, BitImageState* bitImage);
    void printChar(int code);
    std::vector<uint8_t>& pageData(int index);
    std::string toSvg(int index);
    std::string toSvg() { return toSvg(activePageIndex); }
};

} // namespace cpcse
