// CPCSyntaxError — the SSA-1 and dk'tronics speech synthesisers. See the header.
#include "speech.h"

namespace cpcse {

SpeechSynth::SpeechSynth() { reset(); }

void SpeechSynth::setKind(Kind k) {
    if (k == kind) return;
    kind = k;
    // The SSA-1's SP0256 has its own 3.12 MHz resonator; the dk'tronics runs off the
    // expansion bus's 4 MHz. Either way one sample per 312 of its clocks.
    chipRate = (kind == Kind::DkTronics ? 4000000.0 : 3120000.0) / 312.0;
    reset();
}

bool SpeechSynth::loadRom(const Bytes& al2) {
    if (al2.size() != 0x800) return false;
    std::copy(al2.begin(), al2.end(), chip.rom.begin() + 0x1000);
    hasRom = true;
    reset();
    return true;
}

void SpeechSynth::reset() {
    chip.reset();
    chipPhase = 0.0; outPhase = 0.0;
    previous = current = last = 0.0f;
    queue.clear(); readIndex = 0;
    serialMode = false; escape = 0; pendingResult = -1; directMode = true;
    txBuffer.clear(); rxBuffer.clear(); rxCursor = 0;
    mp3.reset();
    ls3Reset();
}

bool SpeechSynth::handlesPort(int port) const {
    port &= 0xffff;
    if (kind == Kind::Ssa1) return port == 0xfbee || port == 0xfaee;
    if (kind == Kind::DkTronics) return port == 0xfbfe;
    if (kind == Kind::LambdaSpeak3) return port == 0xfbee || port == 0xfbfe;
    return false;
}

void SpeechSynth::tx(int value) {
    if (directMode) mp3.receive(value);
    else if (txBuffer.size() < 524) txBuffer.push_back(value);    // 256 + 268, the manual's maximum
}

void SpeechSynth::serialWrite(int value) {
    if (escape == 2) { escape = 0; tx(value); return; }
    if (escape == 3) { escape = 0; return; }                          // baud / width / parity / stop
    if (escape == 4) { escape = 5; rxCursor = (size_t)value; return; }
    if (escape == 5) { escape = 0; rxCursor |= (size_t)value << 8; return; }
    if (escape == 1) {
        escape = 0;
        const size_t avail = rxBuffer.size();
        switch (value) {
            case 0xff: tx(0xff); break;
            case 1: escape = 2; break;
            case 2: for (int b : txBuffer) mp3.receive(b); txBuffer.clear(); break;
            case 3: pendingResult = (int)(avail & 0xff); break;
            case 4: pendingResult = (int)((avail >> 8) & 0xff); break;
            case 5: pendingResult = avail >= 524 ? 1 : 0; break;
            case 6: rxBuffer.clear(); rxCursor = 0; break;
            case 7: pendingResult = rxCursor < avail ? 1 : 0; break;
            case 8: pendingResult = rxCursor < avail ? rxBuffer[rxCursor] : 0; break;
            case 9: pendingResult = rxCursor < avail ? rxBuffer[rxCursor++] : 0; break;
            case 11: escape = 4; break;
            case 12: rxCursor = 0; break;
            case 13: rxCursor = avail ? avail - 1 : 0; break;
            case 14: pendingResult = directMode ? 1 : 0; break;
            case 16: directMode = true; break;
            case 17: directMode = false; break;
            case 20: serialMode = false; txBuffer.clear(); break;      // quit and reset serial mode
            case 30: case 31: case 32: case 33: escape = 3; break;
            default: break;
        }
        return;
    }
    if (value == 0xff) { escape = 1; return; }
    tx(value);
}

void SpeechSynth::writePort(int, int value) {
    value &= 0xff;
    if (kind == Kind::LambdaSpeak3) { ls3Write(value); return; }   // speech_ls3.cpp
    if (!hasRom) return;
    chip.ald_w((uint8_t)(kind == Kind::DkTronics ? (value & 0x3f) : (value & 0xff)));
}

int SpeechSynth::readPort(int) {
    int value = 0xff;
    if (kind == Kind::LambdaSpeak3) return ls3Read();
    if (kind == Kind::Ssa1) {
        if (!chip.sby_r()) value &= ~0x80;
        if (chip.lrq_r()) value &= ~0x40;
    } else if (kind == Kind::DkTronics) {
        if (chip.lrq_r()) value &= ~0x80;
    }
    return value;
}

void SpeechSynth::advanceMicrosecond() {
    if (kind == Kind::None) return;
    // The chip, at its own rate.
    chipPhase += chipRate / 1e6;
    while (chipPhase >= 1.0) {
        chipPhase -= 1.0;
        int16_t s = 0;
        chip.render(&s, 1);
        previous = current;
        current = (float)s / 8192.0f;     // MAME's HIGH_QUALITY output is 14 bits
    }
    // LambdaSpeak 3's own work (its Epson speech), its MP3 module, and what that says back
    // over the UART.
    if (kind == Kind::LambdaSpeak3) {
        ls3Advance();
        mp3.advanceMicrosecond(outputRate);
        while (!mp3.transmit.empty()) {
            if (rxBuffer.size() < 768) rxBuffer.push_back(mp3.transmit.front());
            mp3.transmit.pop_front();
        }
    }
    // ...and the host's, interpolating between the chip's last two samples.
    outPhase += outputRate / 1e6;
    while (outPhase >= 1.0) {
        outPhase -= 1.0;
        const float t = (float)chipPhase;
        last = previous + (current - previous) * t + ttsSample * 0.8f;
        queue.push_back(last);
    }
    if (readIndex > 4096) { queue.erase(queue.begin(), queue.begin() + (long)readIndex); readIndex = 0; }
    if (queue.size() - readIndex > (size_t)(outputRate / 4)) readIndex = queue.size() - (size_t)(outputRate / 4);
}

} // namespace cpcse
