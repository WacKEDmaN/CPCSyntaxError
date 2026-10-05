// CPCSyntaxError — the GATE ARRAY's pixel pipeline.
//
// ACCC §9.1 (p.47): "The GATE ARRAY/ASIC reads the data pointed to by the CRTC from
// memory in order to convert and display it as pixels. It performs this operation by
// considering both parameters: the graphics mode and the colour associated with the
// pixel to be displayed."
//
// The CRTC never touches the picture. It addresses RAM and it drives this chip's
// DISPTMG, HSYNC and VSYNC pins; RAM answers onto this chip's data bus; and this chip
// puts out R, G and B. All of the decoding below therefore belongs here -- it used to
// live in the renderer, which is a consumer of RGB and has no business knowing how a
// VRAM byte becomes a pen.
//
// §9.1's table, which is what pixelPen() and penFromRotation() implement:
//
//   mode 0   VRAM A0 B0 A2 B2 A1 B1 A3 B3  ->  2 pixels of 16 colours
//   mode 1   VRAM A0 B0 C0 D0 A1 B1 C1 D1  ->  4 pixels of 4 colours
//   mode 2   VRAM A0 B0 C0 D0 E0 F0 G0 H0  ->  8 pixels of 2 colours
//   mode 3   VRAM A0 B0  x  x A1 B1  x  x  ->  2 pixels of 4 colours
#include "gate_array.h"
#include "asic.h"
#include "memory.h"

namespace cpcse {


// ACCC §9.3.4: the GATE ARRAY decodes a byte through a shift register that is rotated
// left one bit as each pixel OF THE CURRENT MODE is clocked out — so mode 2 rotates
// every Pixel-M2, mode 1 every 2, modes 0/3 every 4. The pen is always read from the
// same four bit positions of the rotated register (b7,b3,b5,b1, b7 lowest).
// Expressing it this way is what lets a mid-byte mode change be modelled honestly:
// "the calculated PENs are distorted for the rest of the pixels to be displayed in
// the byte. They are partly composed of the rotations performed for the previous
// mode."
// ACCC §9.3.4.3.1 (p.58): "The GATE ARRAY considers that its bit counter is not at 0
// but at 5. From the 6th bit of the byte read in VRAM, it 'starts' processing the
// calculation of the colour number as if they were the 1st bits of a byte." — a plain
// left SHIFT by the bit counter. The bit shifted IN is this chip's own: §9.3.4.3 has
// the consumed bits "considered to be 0 (GA 40010) or 1 (GA 40008)", which is why the
// fill comes from the model rather than from the caller.
// Within a single mode the shifted-in bits never reach a pen bit position, so this is
// byte-for-byte identical to the rotate/table form for every unsplit byte.
int GateArray::penFromRotation(int mode, int byte, int rotation) const {
    int fill = model ? model->modeSwitchFillBit() : 0;
    unsigned b = (unsigned)(byte & 0xff);
    unsigned r = (unsigned)(rotation < 0 ? 0 : rotation > 8 ? 8 : rotation);
    unsigned v = ((b << r) | (fill ? ((1u << r) - 1u) : 0u)) & 0xff;
    if (mode == 2) return (int)(v >> 7 & 1);
    if (mode == 1) return (int)((v >> 7 & 1) | (v >> 2 & 2));
    int pen = (int)((v >> 7 & 1) | (v >> 2 & 2) | (v >> 3 & 4) | ((v << 2) & 8));
    return mode == 3 ? (pen & 3) : pen;
}

// ACCC §9.3.4 MODE SPLITTING. One VRAM byte into its 8 Pixel-M2 pens, honouring a
// graphic-mode change that lands mid-byte. switchPixel < 0 (or >= 8) means no change,
// and the result is then identical to pixelPen() for every pixel.
void GateArray::bytePens(int byte, int modeBefore, int modeAfter, int switchPixel, int pens[8]) const {
    if (switchPixel < 0 || switchPixel > 7) { switchPixel = 8; modeBefore = modeAfter; }
    // ACCC §9.3.4.3 (p.46, p.59-60): "The 40007/40008 seems to be in advance of 1/16
    // MHz on the 40010 when it processes the bits of the byte fetched from VRAM", and
    // the two per-GA tables measure exactly what that costs. For MODE 2 -> MODE 0 the
    // 40010 shows (A3 A2 A1 A0) = 0, b0, 0, b2 and the 40007/8 shows 1, b1, 1, b3:
    // the same decode one bit earlier in the byte, padded with 1s instead of 0s. Same
    // one-bit step in the 2->1 and 2->3 rows. The unchanged-mode row (2->2) is
    // identical on both parts, so the offset belongs to the NEW mode's decode only.
    // Only from a MODE 2 source: 1/16 MHz is one whole bit of the decoder only when it
    // is consuming a bit per Pixel-M2. p.61's MODE 0 table is printed once "for the
    // GATE ARRAY 40007, 40008 and 40010" — from the wider modes the two parts agree.
    int lead = model ? model->modeSwitchDecodeLead() : 0;
    int newModeRotationBias = (switchPixel < 8 && modeAfter != modeBefore
        && modeBefore == 2) ? -lead : 0;
    int rotation = 0;
    int pixel = 0;
    while (pixel < 8) {
        bool afterSwitch = pixel >= switchPixel;
        int mode = afterSwitch ? modeAfter : modeBefore;
        int width = (mode == 2) ? 1 : (mode == 1) ? 2 : 4;
        int pen = penFromRotation(mode, byte, rotation + (afterSwitch ? newModeRotationBias : 0));
        // The group is cut short if the mode changes inside it. A cut group does NOT
        // advance the bit counter: the GA hands over part-way through decoding that
        // pixel, so the new mode starts from the rotation the old one had reached.
        // ACCC p.61's MODE 0 -> MODE 0 row is the proof — the resumed pixel is
        // (b0,b4,b2,b6), rotation 1, even though the switch falls inside the second
        // mode-0 group; advancing on the cut would give rotation 2 and (0,b3,b1,b5).
        // p.70 says the same of the 40226 from the other end: "if the new required mode
        // is 1 or 2, then the PRE-ASIC 'reuses' bits already used to calculate the
        // colour number of the previous pixel", and its MODE 0 rows start at rotation 0.
        int end = pixel + width;
        bool cut = pixel < switchPixel && end > switchPixel;
        if (cut) end = switchPixel;
        if (end > 8) end = 8;
        for (int p = pixel; p < end; p += 1) pens[p] = pen;
        if (!cut) rotation += 1;   // the bit counter never wraps within a byte
        pixel = end;
    }
}

// ACCC §9.1's table read straight out: mode 0 takes A3 A2 A1 A0 from b7 b3 b5 b1 for
// the first pixel and b6 b2 b4 b0 for the second, mode 1 takes two bits per pixel,
// mode 2 one, and mode 3 is mode 0 with the top two pen bits dropped.
std::array<int, 2> GateArray::mode0Pens(int byte) const {
    const unsigned b = (unsigned)byte;
    return { (int)(((b >> 7) & 1) | ((b >> 2) & 2) | ((b >> 3) & 4) | ((b << 2) & 8)),
             (int)(((b >> 6) & 1) | ((b >> 1) & 2) | ((b >> 2) & 4) | ((b << 3) & 8)) };
}
std::array<int, 4> GateArray::mode1Pens(int byte) const {
    const unsigned b = (unsigned)byte;
    return { (int)(((b >> 7) & 1) | ((b >> 2) & 2)),
             (int)(((b >> 6) & 1) | ((b >> 1) & 2)),
             (int)(((b >> 5) & 1) | (b & 2)),
             (int)(((b >> 4) & 1) | ((b & 1) << 1)) };
}
int GateArray::pixelPen(int mode, int byte, int pixel) const {
    pixel &= 7;
    if (mode == 0 || mode == 3) {
        int pen = mode0Pens(byte)[pixel < 4 ? 0 : 1];
        return mode == 3 ? pen & 3 : pen;
    }
    if (mode == 1) return mode1Pens(byte)[(unsigned)pixel >> 1];
    return (unsigned)byte >> (7 - pixel) & 1;
}


// ACCC §9.1: "This pixel coding represents an index in a table containing 5-bit
// colour-coded colours." The 32 colours this chip can drive onto R, G and B. It lived
// in asic.cpp, which is the CPC+ extension -- a 464 drives these without one.
//
// What is NOT here, deliberately: the tube's tint. A green-screen or monochrome CTM
// turns these same RGB values into its own, and that belongs to the monitor, which is
// where monitorTransformRgb() lives.
static const int GA_RGB[32] = {
    0x636363, 0x636363, 0x00ff63, 0xffff63, 0x000063, 0xff0063, 0x006363, 0xff6363,
    0xff0063, 0xffff63, 0xffff00, 0xfefefe, 0xff0000, 0xff00ff, 0xff6300, 0xff63ff,
    0x000063, 0x00ff63, 0x00ff00, 0x00ffff, 0x000000, 0x0000ff, 0x006300, 0x0063ff,
    0x630063, 0x63ff63, 0x63ff00, 0x63ffff, 0x630000, 0x6300ff, 0x636300, 0x6363ff
};

int GateArray::rgbForHardwareColour(int hardwareColour) {
    return GA_RGB[hardwareColour & 0x1f];
}



} // namespace cpcse
