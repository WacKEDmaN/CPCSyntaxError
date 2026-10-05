// CPCSyntaxError — Matrix printer.
#include "matrix_printer.h"
#include "matrix_font_nearlyq.h"

namespace cpcse {

static const std::array<int, 4> PAPER = { 248, 246, 238, 255 };
static const std::array<int, 4> DOT = { 22, 22, 20, 255 };
static const std::array<int, 4> SOFT_DOT = { 90, 88, 82, 210 };
static const double A4_WIDTH_IN = 8.27;
static const double A4_HEIGHT_IN = 11.69;

static int jsRound(double x) { return (int)std::floor(x + 0.5); }

static std::string printerCharForByte(int code) {
    int byte = code & 0x7f;
    if (byte < 0x20 || byte == 0x7f) return "?";
    return std::string(1, (char)byte);
}
static const MatrixGlyph* glyphFor(int code) {
    const auto& font = nearlyqFont();
    std::string ch = printerCharForByte(code);
    auto it = font.find(ch);
    if (it != font.end()) return &it->second;
    it = font.find("?"); if (it != font.end()) return &it->second;
    it = font.find(" "); return it != font.end() ? &it->second : nullptr;
}
static std::string escapeXml(const std::string& value) {
    std::string out;
    for (char ch : value) {
        if (ch == '&') out += "&amp;";
        else if (ch == '<') out += "&lt;";
        else if (ch == '>') out += "&gt;";
        else if (ch == '"') out += "&quot;";
        else out += ch;
    }
    return out;
}


MatrixPrinter::MatrixPrinter(int width, int height) : width(width), height(height) {
    dpiX = width / A4_WIDTH_IN;
    dpiY = height / A4_HEIGHT_IN;
    activePageIndex = 0;
    soundDotCounter = 0;
    pendingNewPageSound = false;
    reset();
}
void MatrixPrinter::reset() {
    leftMargin = std::max(20, jsRound(width * 0.065));
    rightMargin = width - leftMargin;
    topMargin = jsRound(dpiY * 0.35);
    bottomMargin = height - topMargin;
    pages.assign(1, createBlankPage());
    dotPages.assign(1, {});
    activePageIndex = 0;
    pagesDropped = 0;
    pendingNewPageSound = false;
    resetPrintState(true);
}
int MatrixPrinter::verticalPinStep() { return std::max(1, jsRound(dpiY / 72)); }
void MatrixPrinter::resetPrintState(bool home) {
    lineSpacing = verticalPinStep() * 12;
    baseCpi = 10;
    condensed = false; bold = false; doubleStrike = false; italic = false; underline = false;
    doubleWidth = false; doubleWidthOneLine = false; halfHeight = false; nlq = false;
    updateCharWidth();
    esc.reset();
    pendingHardcopyLineFeed.reset();
    graphicModes = { {'K', 0}, {'L', 1}, {'Y', 2}, {'Z', 3} };
    if (home) { x = leftMargin; y = topMargin; }
}
void MatrixPrinter::updateCharWidth() {
    double printable = std::max(1, rightMargin - leftMargin);
    int columns;
    if (condensed) columns = baseCpi >= 12 ? 160 : 132;
    else if (baseCpi >= 15) columns = 120;
    else if (baseCpi >= 12) columns = 96;
    else columns = 80;
    charWidth = std::max(4.0, printable / columns);
}
std::vector<uint8_t> MatrixPrinter::createBlankPage() {
    std::vector<uint8_t> page((size_t)width * height * 4);
    for (size_t i = 0; i < page.size(); i += 4) {
        page[i] = (uint8_t)PAPER[0]; page[i + 1] = (uint8_t)PAPER[1]; page[i + 2] = (uint8_t)PAPER[2]; page[i + 3] = (uint8_t)PAPER[3];
    }
    int edge = std::max(4, jsRound(leftMargin * 0.5));
    for (int yy = 60; yy < height - 60; yy += 28) {
        setPixelOn(page, edge, yy, SOFT_DOT); setPixelOn(page, width - edge - 1, yy, SOFT_DOT);
        setPixelOn(page, edge + 1, yy, SOFT_DOT); setPixelOn(page, width - edge - 2, yy, SOFT_DOT);
    }
    return page;
}
void MatrixPrinter::setPixelOn(std::vector<uint8_t>& page, double xf, double yf, const std::array<int, 4>& rgba) {
    int xi = jsRound(xf), yi = jsRound(yf);
    if (xi < 0 || yi < 0 || xi >= width || yi >= height) return;
    size_t o = ((size_t)yi * width + xi) * 4;
    page[o] = (uint8_t)rgba[0]; page[o + 1] = (uint8_t)rgba[1]; page[o + 2] = (uint8_t)rgba[2]; page[o + 3] = (uint8_t)rgba[3];
}
void MatrixPrinter::setPixel(double xf, double yf, const std::array<int, 4>& rgba) {
    setPixelOn(pages[activePageIndex], xf, yf, rgba);
}
void MatrixPrinter::emitSound(const std::string& name) { if (onSound) onSound(name); }
void MatrixPrinter::dot(double xf, double yf, DotOptions options) {
    if (options.record && pendingNewPageSound) {
        pendingNewPageSound = false;
        emitSound("carret");
    }
    int stretch = std::max(1, jsRound(options.stretchY));
    setPixel(xf, yf, DOT);
    setPixel(xf + 1, yf, SOFT_DOT);
    setPixel(xf, yf + 1, options.dense ? DOT : SOFT_DOT);
    if (stretch > 2) {
        for (int sy = 2; sy < stretch; sy += 1) {
            setPixel(xf, yf + sy, options.dense ? DOT : SOFT_DOT);
            setPixel(xf + 1, yf + sy, SOFT_DOT);
        }
    }
    // A dot off the paper is not kept, nor more than a page could hold: a hardcopy's head
    // never wraps, and a program streaming its graphics would grow the list without end.
    const bool onPaper = xf > -2 && yf > -2 && xf < width + 1 && yf < height + 1;
    if (options.record && onPaper && dotPages[activePageIndex].size() < (size_t)width * height / 4) {
        PrinterDot d;
        if (stretch > 2) { d.x = jsRound(xf); d.y = jsRound(yf + (stretch - 1) / 2.0); d.r = 1.15; d.ry = std::max(1.15, stretch / 2.0); d.hasRy = true; d.opacity = 0.82; }
        else { d.x = jsRound(xf); d.y = jsRound(yf); d.r = 1.15; d.opacity = 0.82; }
        dotPages[activePageIndex].push_back(d);
        soundDotCounter = (soundDotCounter + 1) & 0x0f;
        if (soundDotCounter == 0) emitSound("needle");
    }
}
void MatrixPrinter::carriageReturn() {
    x = leftMargin;
    if (doubleWidthOneLine) { doubleWidth = false; doubleWidthOneLine = false; }
}
void MatrixPrinter::lineFeed() { lineFeed(-1e18); }
void MatrixPrinter::lineFeed(double amount) {
    double step;
    if (amount != -1e18) step = amount;
    else if (pendingHardcopyLineFeed) { step = *pendingHardcopyLineFeed; pendingHardcopyLineFeed.reset(); }
    else step = halfHeight ? std::max(1.0, lineSpacing / 2) : lineSpacing;
    y += std::max(1.0, step);
    if (y >= bottomMargin) formFeed();
}
void MatrixPrinter::reverseLineFeed() { reverseLineFeed(lineSpacing); }
void MatrixPrinter::reverseLineFeed(double amount) { y = std::max((double)topMargin, y - std::max(1.0, amount)); }
void MatrixPrinter::formFeed() {
    if ((int)pages.size() >= MAX_PAGES) {           // the oldest page goes
        pages.erase(pages.begin());
        dotPages.erase(dotPages.begin());
        pagesDropped += 1;
        activePageIndex -= 1;
    }
    activePageIndex += 1;
    pages.push_back(createBlankPage());
    dotPages.push_back({});
    x = leftMargin;
    y = topMargin;
    esc.reset();
    pendingHardcopyLineFeed.reset();
    pendingNewPageSound = true;
}
void MatrixPrinter::writeByte(int value) {
    int byte = value & 0xff;
    if (esc) { handleEsc(byte); return; }
    if (byte == 0x1b) { esc = EscState{}; esc->state = "command"; return; }
    if (byte == 0x0e) { doubleWidth = true; doubleWidthOneLine = true; return; }
    if (byte == 0x0f) { condensed = true; updateCharWidth(); return; }
    if (byte == 0x12) { condensed = false; updateCharWidth(); return; }
    if (byte == 0x14) { doubleWidth = false; doubleWidthOneLine = false; return; }
    if (byte == 0x13) { halfHeight = true; return; }
    if (byte == 0x11) { halfHeight = false; return; }
    if (byte == 0x0d) { carriageReturn(); return; }
    if (byte == 0x0a) { lineFeed(); return; }
    if (byte == 0x0c) { formFeed(); return; }
    if (byte == 0x08) { x = std::max((double)leftMargin, x - charWidth); return; }
    if (byte == 0x09) { x = leftMargin + std::ceil((x - leftMargin + 1) / (charWidth * 8)) * (charWidth * 8); return; }
    if (byte >= 0x20) printChar(byte);
}
void MatrixPrinter::handleEsc(int byte) {
    EscState& e = *esc;
    if (e.state == "command") {
        char ch = (char)byte;
        if (ch == '@') { resetPrintState(false); esc.reset(); return; }
        if (ch == 'E') { bold = true; esc.reset(); return; }
        if (ch == 'F') { bold = false; esc.reset(); return; }
        if (ch == 'G') { doubleStrike = true; esc.reset(); return; }
        if (ch == 'H') { doubleStrike = false; esc.reset(); return; }
        if (ch == '4') { italic = true; esc.reset(); return; }
        if (ch == '5') { italic = false; esc.reset(); return; }
        if (ch == 'P') { baseCpi = 10; updateCharWidth(); esc.reset(); return; }
        if (ch == 'M') { baseCpi = 12; updateCharWidth(); esc.reset(); return; }
        if (ch == 'g') { baseCpi = 15; updateCharWidth(); esc.reset(); return; }
        if (ch == '!') { e.state = "masterSelect"; return; }
        if (ch == 'x') { e.state = "nlq"; return; }
        if (ch == 'S') { e.state = "halfHeight"; return; }
        if (ch == 'T') { halfHeight = false; esc.reset(); return; }
        if (ch == '0') { lineSpacing = verticalPinStep() * 9; esc.reset(); return; }
        if (ch == '1') { lineSpacing = verticalPinStep() * 7; esc.reset(); return; }
        if (ch == '2') { lineSpacing = verticalPinStep() * 12; esc.reset(); return; }
        if (ch == '3') { e.state = "lineSpacing216"; return; }
        if (ch == 'A') { e.state = "lineSpacing72"; return; }
        if (ch == 'J') { e.state = "lineFeed216"; return; }
        if (ch == 'j') { e.state = "reverseFeed216"; return; }
        if (ch == '-') { e.state = "underline"; return; }
        if (ch == 'W') { e.state = "doubleWidth"; return; }
        if (ch == '?') { e.state = "aliasModeCommand"; return; }
        if (ch == '*') { e.state = "bitMode"; return; }
        if (ch == 'K' || ch == 'L' || ch == 'Y' || ch == 'Z') { e.state = "bitLenLo"; e.mode = graphicModes.count(ch) ? graphicModes[ch] : 0; return; }
        esc.reset();
        return;
    }
    if (e.state == "underline") { underline = (byte & 1) != 0; esc.reset(); return; }
    if (e.state == "doubleWidth") { doubleWidth = (byte & 1) != 0; doubleWidthOneLine = false; esc.reset(); return; }
    if (e.state == "nlq") { nlq = (byte & 1) != 0; esc.reset(); return; }
    if (e.state == "halfHeight") { halfHeight = true; esc.reset(); return; }
    if (e.state == "masterSelect") {
        baseCpi = (byte & 0x01) ? 12 : 10;
        condensed = !!(byte & 0x04);
        bold = !!(byte & 0x08);
        doubleStrike = !!(byte & 0x10);
        doubleWidth = !!(byte & 0x20);
        italic = !!(byte & 0x40);
        underline = !!(byte & 0x80);
        doubleWidthOneLine = false;
        updateCharWidth();
        esc.reset();
        return;
    }
    if (e.state == "lineSpacing72") { lineSpacing = std::max(1, jsRound(verticalPinStep() * byte)); esc.reset(); return; }
    if (e.state == "lineSpacing216") { lineSpacing = std::max(1, jsRound(verticalPinStep() * byte / 3.0)); esc.reset(); return; }
    if (e.state == "lineFeed216") { lineFeed(std::max(1, jsRound(verticalPinStep() * byte / 3.0))); esc.reset(); return; }
    if (e.state == "reverseFeed216") { reverseLineFeed(std::max(1, jsRound(verticalPinStep() * byte / 3.0))); esc.reset(); return; }
    if (e.state == "aliasModeCommand") { e.alias = std::string(1, (char)byte); e.state = "aliasModeValue"; return; }
    if (e.state == "aliasModeValue") { if (e.alias == "K" || e.alias == "L" || e.alias == "Y" || e.alias == "Z") graphicModes[e.alias[0]] = byte & 0x3f; esc.reset(); return; }
    if (e.state == "bitMode") { e.mode = byte; e.state = "bitLenLo"; return; }
    if (e.state == "bitLenLo") { e.length = byte; e.state = "bitLenHi"; return; }
    if (e.state == "bitLenHi") {
        e.length += byte << 8;
        e.remaining = e.length;
        e.bitImage = beginBitImage(e.mode, e.length); e.hasBitImage = true;
        e.state = "bitData";
        if (e.remaining <= 0) esc.reset();
        return;
    }
    if (e.state == "bitData") {
        printBitColumn(byte, e.mode, &e.bitImage);
        e.remaining -= 1;
        if (e.remaining <= 0) esc.reset();
    }
}
int MatrixPrinter::modeDpi(int mode) {
    switch (mode) {
        case 0: return 60; case 1: return 120; case 2: return 120; case 3: return 240;
        case 4: return 80; case 5: return 72; case 6: return 90; default: return 60;
    }
}
double MatrixPrinter::densityStep(int mode) { return dpiX / modeDpi(mode); }
MatrixPrinter::HardcopyProfile MatrixPrinter::cpcHardcopyProfile(int mode, int length) {
    int m = mode;
    if (m == 1 && length >= 600 && length <= 700) return { true, length, false };
    if (m == 0 && (length == 100 || length == 120)) return { true, 320, true };
    return { false, 0, false };
}
BitImageState MatrixPrinter::beginBitImage(int mode, int length) {
    HardcopyProfile profile = cpcHardcopyProfile(mode, length);
    if (!profile.present) { BitImageState b; b.step = densityStep(mode); b.pinStep = verticalPinStep(); b.noAutoWrap = false; b.stretchY = 1; return b; }
    double printable = std::max(1, rightMargin - leftMargin);
    double available = std::max(1.0, rightMargin - x);
    double targetWidth = profile.fullWidth ? printable : std::min(printable, available);
    double sourceRowStep = std::max(1.0, (printable / 640) * 2.4);
    BitImageState result;
    result.hardcopy = true;
    result.step = targetWidth / std::max(1, profile.sourceColumns);
    result.pinStep = sourceRowStep;
    result.lineFeed = sourceRowStep * 7;
    result.stretchY = std::max(2.0, std::ceil(sourceRowStep));
    result.noAutoWrap = true;
    pendingHardcopyLineFeed = result.lineFeed;
    return result;
}
void MatrixPrinter::printBitColumn(int byte, int mode, BitImageState* bitImage) {
    double xx = x;
    double pinStep = bitImage ? bitImage->pinStep : verticalPinStep();
    if (byte & 0x80) {
        for (int bit = 0; bit < 8; bit += 1) if (byte & (0x80 >> bit)) { DotOptions o; o.dense = true; o.stretchY = bitImage ? bitImage->stretchY : 1; dot(xx, y + bit * pinStep, o); }
    } else {
        for (int bit = 0; bit < 7; bit += 1) if (byte & (0x40 >> bit)) { DotOptions o; o.dense = true; o.stretchY = bitImage ? bitImage->stretchY : 1; dot(xx, y + bit * pinStep, o); }
    }
    x += bitImage ? bitImage->step : densityStep(mode);
    if (!(bitImage && bitImage->noAutoWrap) && x > rightMargin + 0.001) { carriageReturn(); lineFeed(); }
}
void MatrixPrinter::printChar(int code) {
    const MatrixGlyph* glyph = glyphFor(code);
    const std::vector<std::string>* bitmapPtr = glyph ? &glyph->bitmap : &nearlyqFont().at("?").bitmap;
    const std::vector<std::string>& bitmap = *bitmapPtr;
    int fontWidth = std::max(1, bitmap.empty() ? NEARLYQ_FONT_WIDTH : (int)bitmap[0].size());
    double rowStep = halfHeight ? 0.5 : 1;
    int scaleX = doubleWidth ? 2 : 1;
    double cellWidth = charWidth * scaleX;
    int italicRoom = italic ? 3 : 0;
    int gap = condensed ? 3 : 2;
    double availableWidth = std::max(1.0, cellWidth - gap - italicRoom);
    double glyphXScale = availableWidth / std::max(1, fontWidth - 1);
    int baseX = jsRound(x), baseY = jsRound(y);
    int bands = (int)std::ceil((double)bitmap.size() / NEARLYQ_PIN_ROWS);
    for (int band = 0; band < bands; band += 1) {
        int rowBase = band * NEARLYQ_PIN_ROWS;
        for (int pin = 0; pin < NEARLYQ_PIN_ROWS; pin += 1) {
            int gy = rowBase + pin;
            if (gy >= (int)bitmap.size()) break;
            const std::string& row = bitmap[gy];
            int italicShift = italic ? std::max(0, jsRound((NEARLYQ_FONT_HEIGHT - 1 - gy) * 0.18)) : 0;
            int py = baseY + jsRound(gy * rowStep);
            for (int gx = 0; gx < (int)row.size(); gx += 1) {
                if (row[gx] != '#') continue;
                int px = baseX + jsRound(gx * glyphXScale) + italicShift;
                dot(px, py);
                if (nlq && !halfHeight) {
                    int qx = px + ((gy & 1) ? 1 : 0);
                    dot(qx, py + 1);
                    if (bold || doubleStrike) dot(qx + 1, py + 1);
                }
                if (bold || doubleStrike) dot(px + 1, py);
            }
        }
    }
    if (underline) {
        int underlineY = baseY + jsRound((NEARLYQ_FONT_HEIGHT - 1) * rowStep);
        int underlineWidth = std::max(1, (int)std::floor(cellWidth - 1));
        for (int ux = 0; ux < underlineWidth; ux += 1) dot(baseX + ux, underlineY);
    }
    x += cellWidth;
    if (x > rightMargin + 0.001) { carriageReturn(); lineFeed(); }
}
std::vector<uint8_t>& MatrixPrinter::pageData(int index) {   // index: pageNumber() - 1
    int idx = std::max(0, std::min((int)pages.size() - 1, index - pagesDropped));
    return pages[idx];
}
std::string MatrixPrinter::toSvg(int index) {
    int pageIndex = std::max(0, std::min((int)dotPages.size() - 1, index));
    const std::vector<PrinterDot>& dots = dotPages[pageIndex];
    std::vector<std::string> lines;
    lines.push_back("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    lines.push_back("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"210mm\" height=\"297mm\" viewBox=\"0 0 " + std::to_string(width) + " " + std::to_string(height) + "\">");
    lines.push_back("<title>CPCSyntaxError Matrix Printer page " + std::to_string(pageIndex + 1) + "</title>");
    lines.push_back("<rect width=\"100%\" height=\"100%\" fill=\"#f8f6ee\"/>");
    auto numstr = [](double v) { std::string s = std::to_string(v); return s; };
    for (const PrinterDot& d : dots) {
        if (d.hasRy && d.ry != d.r) lines.push_back("<ellipse cx=\"" + escapeXml(std::to_string(d.x)) + "\" cy=\"" + escapeXml(std::to_string(d.y)) + "\" rx=\"" + numstr(d.r ? d.r : 1.15) + "\" ry=\"" + numstr(d.ry) + "\" fill=\"#161614\" fill-opacity=\"" + numstr(d.opacity ? d.opacity : 0.82) + "\"/>");
        else lines.push_back("<circle cx=\"" + escapeXml(std::to_string(d.x)) + "\" cy=\"" + escapeXml(std::to_string(d.y)) + "\" r=\"" + numstr(d.r ? d.r : 1.15) + "\" fill=\"#161614\" fill-opacity=\"" + numstr(d.opacity ? d.opacity : 0.82) + "\"/>");
    }
    lines.push_back("</svg>");
    std::string out; for (size_t i = 0; i < lines.size(); i++) { out += lines[i]; if (i + 1 < lines.size()) out += "\n"; }
    return out;
}

} // namespace cpcse
