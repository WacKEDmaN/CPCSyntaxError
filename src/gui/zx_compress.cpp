// CPCSyntaxError GUI — ZX0, ZX1 and ZX7B compression. See zx_compress.h.
//
// The algorithms are those of the programs below, changed only to work on memory and to keep
// their state in objects (the programs use globals and never free their blocks).
//
// ZX0 v2.2 and ZX1 v1.5:
// (c) Copyright 2021 by Einar Saukas. All rights reserved.
//
// ZX7b v1.01:
// ZX7b (c) Copyright 2013 by Antonio Villena. All rights reserved.
// Based on ZX7 <http://www.worldofspectrum.org/infoseekid.cgi?id=0027996>
// (c) Copyright 2012 by Einar Saukas. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * The name of its author may not be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "zx_compress.h"

#include <algorithm>
#include <memory>

namespace cpcse {

namespace {

// ---------------------------------------------------------------- ZX0 / ZX1 (one optimizer)

constexpr int INITIAL_OFFSET = 1;
constexpr int MAX_OFFSET_ZX0 = 32640, MAX_OFFSET_ZX1 = 32512;

struct Block {
    Block* chain = nullptr;
    Block* ghostChain = nullptr;
    int bits = 0, index = 0, offset = 0, references = 0;
};

// memory.c: blocks taken from arrays of 10000, used again when no chain refers to them
struct BlockPool {
    std::vector<std::unique_ptr<Block[]>> arrays;
    Block* ghostRoot = nullptr;
    int left = 0;

    Block* allocate(int bits, int index, int offset, Block* chain) {
        Block* ptr;
        if (ghostRoot) {
            ptr = ghostRoot;
            ghostRoot = ptr->ghostChain;
            if (ptr->chain && !--ptr->chain->references) {
                ptr->chain->ghostChain = ghostRoot;
                ghostRoot = ptr->chain;
            }
        } else {
            if (!left) { arrays.emplace_back(new Block[10000]); left = 10000; }
            ptr = &arrays.back()[--left];
        }
        ptr->bits = bits;
        ptr->index = index;
        ptr->offset = offset;
        if (chain) chain->references++;
        ptr->chain = chain;
        ptr->references = 0;
        return ptr;
    }
    void assign(Block** ptr, Block* chain) {
        chain->references++;
        if (*ptr && !--(*ptr)->references) {
            (*ptr)->ghostChain = ghostRoot;
            ghostRoot = *ptr;
        }
        *ptr = chain;
    }
};

int offsetCeiling(int index, int offsetLimit) {
    return index > offsetLimit ? offsetLimit : index < INITIAL_OFFSET ? INITIAL_OFFSET : index;
}

int eliasGammaBits(int value) {
    int bits = 1;
    while (value >>= 1) bits += 2;
    return bits;
}

// optimize.c (skip = 0). zx1: the cost of a match from a new offset is ZX1's.
Block* optimize(BlockPool& pool, const uint8_t* input, int inputSize, int offsetLimit, bool zx1) {
    int maxOffset = offsetCeiling(inputSize - 1, offsetLimit);
    std::vector<Block*> lastLiteral((size_t)maxOffset + 1), lastMatch((size_t)maxOffset + 1), optimal((size_t)inputSize);
    std::vector<int> matchLength((size_t)maxOffset + 1), bestLength((size_t)std::max(inputSize, 3));
    if (inputSize > 2) bestLength[2] = 2;

    // start with fake block
    pool.assign(&lastMatch[INITIAL_OFFSET], pool.allocate(-1, -1, INITIAL_OFFSET, nullptr));

    for (int index = 0; index < inputSize; index++) {
        int bestLengthSize = 2;
        maxOffset = offsetCeiling(index, offsetLimit);
        for (int offset = 1; offset <= maxOffset; offset++) {
            if (index != 0 && index >= offset && input[index] == input[index - offset]) {
                // copy from last offset
                if (lastLiteral[offset]) {
                    const int length = index - lastLiteral[offset]->index;
                    const int bits = lastLiteral[offset]->bits + 1 + eliasGammaBits(length);
                    pool.assign(&lastMatch[offset], pool.allocate(bits, index, offset, lastLiteral[offset]));
                    if (!optimal[index] || optimal[index]->bits > bits) pool.assign(&optimal[index], lastMatch[offset]);
                }
                // copy from new offset
                if (++matchLength[offset] > 1) {
                    if (bestLengthSize < matchLength[offset]) {
                        int bits = optimal[index - bestLength[bestLengthSize]]->bits + eliasGammaBits(bestLength[bestLengthSize] - 1);
                        do {
                            bestLengthSize++;
                            const int bits2 = optimal[index - bestLengthSize]->bits + eliasGammaBits(bestLengthSize - 1);
                            if (bits2 <= bits) {
                                bestLength[bestLengthSize] = bestLengthSize;
                                bits = bits2;
                            } else {
                                bestLength[bestLengthSize] = bestLength[bestLengthSize - 1];
                            }
                        } while (bestLengthSize < matchLength[offset]);
                    }
                    const int length = bestLength[matchLength[offset]];
                    const int bits = zx1 ? optimal[index - length]->bits + 1 + (offset > 128 ? 16 : 8) + eliasGammaBits(length - 1)
                                         : optimal[index - length]->bits + 8 + eliasGammaBits((offset - 1) / 128 + 1) + eliasGammaBits(length - 1);
                    if (!lastMatch[offset] || lastMatch[offset]->index != index || lastMatch[offset]->bits > bits) {
                        pool.assign(&lastMatch[offset], pool.allocate(bits, index, offset, optimal[index - length]));
                        if (!optimal[index] || optimal[index]->bits > bits) pool.assign(&optimal[index], lastMatch[offset]);
                    }
                }
            } else {
                // copy literals
                matchLength[offset] = 0;
                if (lastMatch[offset]) {
                    const int length = index - lastMatch[offset]->index;
                    const int bits = lastMatch[offset]->bits + 1 + eliasGammaBits(length) + length * 8;
                    pool.assign(&lastLiteral[offset], pool.allocate(bits, index, 0, lastMatch[offset]));
                    if (!optimal[index] || optimal[index]->bits > bits) pool.assign(&optimal[index], lastLiteral[offset]);
                }
            }
        }
    }
    return optimal[(size_t)inputSize - 1];
}

// compress.c of ZX0 and of ZX1: the bit writer and its state
struct BitWriter {
    std::vector<uint8_t> out;
    size_t outIndex = 0, bitIndex = 0;
    int bitMask = 0;
    bool backtrack = false;          // ZX0 only

    void writeByte(int value) {
        if (outIndex < out.size()) out[outIndex] = (uint8_t)value;
        outIndex++;
    }
    void orAt(size_t at, int value) { if (at < out.size()) out[at] |= (uint8_t)value; }
    void writeBit(int value) {
        if (backtrack) {
            if (value) orAt(outIndex - 1, 1);
            backtrack = false;
        } else {
            if (!bitMask) {
                bitMask = 128;
                bitIndex = outIndex;
                writeByte(0);
            }
            if (value) orAt(bitIndex, bitMask);
            bitMask >>= 1;
        }
    }
};

Block* unreverse(Block* optimal) {
    Block* prev = nullptr;
    while (optimal) {
        Block* next = optimal->chain;
        optimal->chain = prev;
        prev = optimal;
        optimal = next;
    }
    return prev;
}

std::vector<uint8_t> compressZx0(Block* optimal, const uint8_t* input, bool backwards, bool invert) {
    BitWriter w;
    w.out.assign((size_t)(optimal->bits + 25) / 8, 0);
    auto gamma = [&](int value, bool backwardsMode, bool invertMode) {
        int i;
        for (i = 2; i <= value; i <<= 1) {}
        i >>= 1;
        while (i >>= 1) {
            w.writeBit(backwardsMode);
            w.writeBit(invertMode ? !(value & i) : (value & i));
        }
        w.writeBit(!backwardsMode);
    };
    Block* prev = unreverse(optimal);
    int lastOffset = INITIAL_OFFSET;
    size_t inputIndex = 0;
    w.backtrack = true;
    for (optimal = prev->chain; optimal; prev = optimal, optimal = optimal->chain) {
        const int length = optimal->index - prev->index;
        if (!optimal->offset) {
            w.writeBit(0);                                       // copy literals indicator
            gamma(length, backwards, false);
            for (int i = 0; i < length; i++) w.writeByte(input[inputIndex++]);
        } else if (optimal->offset == lastOffset) {
            w.writeBit(0);                                       // copy from last offset
            gamma(length, backwards, false);
            inputIndex += (size_t)length;
        } else {
            w.writeBit(1);                                       // copy from new offset
            gamma((optimal->offset - 1) / 128 + 1, backwards, invert);
            if (backwards) w.writeByte(((optimal->offset - 1) % 128) << 1);
            else w.writeByte((127 - (optimal->offset - 1) % 128) << 1);
            w.backtrack = true;
            gamma(length - 1, backwards, false);
            inputIndex += (size_t)length;
            lastOffset = optimal->offset;
        }
    }
    w.writeBit(1);                                               // end marker
    gamma(256, backwards, invert);
    return w.out;
}

std::vector<uint8_t> compressZx1(Block* optimal, const uint8_t* input, bool backwards) {
    BitWriter w;
    w.out.assign((size_t)(optimal->bits + 24) / 8, 0);
    auto gamma = [&](int value) {
        int i;
        for (i = 2; i <= value; i <<= 1) {}
        i >>= 1;
        while (i >>= 1) {
            w.writeBit(1);
            w.writeBit(value & i);
        }
        w.writeBit(0);
    };
    Block* prev = unreverse(optimal);
    int lastOffset = INITIAL_OFFSET;
    size_t inputIndex = 0;
    bool first = true;
    for (optimal = prev->chain; optimal; prev = optimal, optimal = optimal->chain) {
        const int length = optimal->index - prev->index;
        if (!optimal->offset) {
            if (first) first = false;                            // copy literals indicator
            else w.writeBit(0);
            gamma(length);
            for (int i = 0; i < length; i++) w.writeByte(input[inputIndex++]);
        } else if (optimal->offset == lastOffset) {
            w.writeBit(0);
            gamma(length);
            inputIndex += (size_t)length;
        } else {
            w.writeBit(1);
            const int o = optimal->offset;
            if (backwards) {
                if (o > 128) {
                    w.writeByte(((o - 1) & 254) + 1);
                    w.writeByte(o / 256 * 2 + (o - 1) % 2 + 2);
                } else {
                    w.writeByte((o - 1) * 2);
                }
            } else {
                if (o > 128) {
                    w.writeByte(255 - ((o - 1) & 254));
                    w.writeByte(252 - (o - 1) / 256 * 2 + o % 2);
                } else {
                    w.writeByte(256 - o * 2);
                }
            }
            gamma(length - 1);
            inputIndex += (size_t)length;
            lastOffset = o;
        }
    }
    w.writeBit(1);                                               // end marker
    if (backwards) { w.writeByte(1); w.writeByte(0); }
    else { w.writeByte(255); w.writeByte(255); }
    return w.out;
}

// ---------------------------------------------------------------- ZX7B

constexpr size_t ZX7B_MAX_OFFSET = 2176, ZX7B_MAX_LEN = 65536;

struct Match { size_t index = 0; Match* next = nullptr; };
struct Optimal { size_t bits = 0; int offset = 0; int len = 0; };

int countBits7(int offset, int len) {
    int bits = 1, value = len - 1;
    while (value > 1) { bits += 2; value >>= 1; }
    return 1 + (offset > 128 ? 12 : 8) + bits;
}

std::vector<Optimal> optimize7(const uint8_t* input, size_t inputSize) {
    std::vector<size_t> min(ZX7B_MAX_OFFSET + 1), max(ZX7B_MAX_OFFSET + 1);
    std::vector<Match> matches(256 * 256), slots(inputSize);
    std::vector<Optimal> optimal(inputSize);
    optimal[0].bits = 8;                                         // first byte is always literal
    for (size_t i = 1; i < inputSize; i++) {
        optimal[i].bits = optimal[i - 1].bits + 9;
        const int matchIndex = input[i - 1] << 8 | input[i];
        size_t bestLen = 1;
        for (Match* match = &matches[(size_t)matchIndex]; match->next != nullptr && bestLen < ZX7B_MAX_LEN; match = match->next) {
            const size_t offset = i - match->next->index;
            if (offset > ZX7B_MAX_OFFSET) {
                match->next = nullptr;
                break;
            }
            size_t len;
            for (len = 2; len <= ZX7B_MAX_LEN; len++) {
                if (len > bestLen && (len & 0xff)) {
                    bestLen = len;
                    const size_t bits = optimal[i - len].bits + (size_t)countBits7((int)offset, (int)len);
                    if (optimal[i].bits > bits) {
                        optimal[i].bits = bits;
                        optimal[i].offset = (int)offset;
                        optimal[i].len = (int)len;
                    }
                } else if (i + 1 == max[offset] + len && max[offset] != 0) {
                    len = i - min[offset];
                    if (len > bestLen) len = bestLen;
                }
                if (i < offset + len || input[i - len] != input[i - len - offset]) break;
            }
            min[offset] = i + 1 - len;
            max[offset] = i;
        }
        slots[i].index = i;
        slots[i].next = matches[(size_t)matchIndex].next;
        matches[(size_t)matchIndex].next = &slots[i];
    }
    return optimal;
}

std::vector<uint8_t> compress7(std::vector<Optimal>& optimal, const uint8_t* input, size_t inputSize) {
    BitWriter w;
    size_t inputIndex = inputSize - 1;
    w.out.assign((optimal[inputIndex].bits + 16 + 7) / 8, 0);
    auto gamma = [&](int value) {
        int bits = 0, rvalue = 0;
        while (value > 1) {
            ++bits;
            rvalue <<= 1;
            rvalue |= value & 1;
            value >>= 1;
        }
        while (bits--) {
            w.writeBit(0);
            w.writeBit(rvalue & 1);
            rvalue >>= 1;
        }
        w.writeBit(1);
    };
    // un-reverse optimal sequence
    optimal[inputIndex].bits = 0;
    while (inputIndex > 0) {
        const size_t inputPrev = inputIndex - (optimal[inputIndex].len > 0 ? (size_t)optimal[inputIndex].len : 1);
        optimal[inputPrev].bits = inputIndex;
        inputIndex = inputPrev;
    }
    w.writeByte(input[0]);                                       // first byte is always literal
    while ((inputIndex = optimal[inputIndex].bits) > 0) {
        if (optimal[inputIndex].len == 0) {
            w.writeBit(0);
            w.writeByte(input[inputIndex]);
        } else {
            w.writeBit(1);                                       // sequence indicator
            gamma(optimal[inputIndex].len - 1);                  // sequence length
            int offset1 = optimal[inputIndex].offset - 1;        // sequence offset
            if (offset1 < 128) {
                w.writeByte(offset1);
            } else {
                offset1 -= 128;
                w.writeByte((offset1 & 127) | 128);
                for (int mask = 1024; mask > 127; mask >>= 1) w.writeBit((offset1 & mask) > 0);
            }
        }
    }
    w.writeBit(1);                                               // end mark
    gamma(0xff);
    return w.out;
}

} // namespace

bool zxCompress(const std::string& method, const std::vector<uint8_t>& data, std::vector<uint8_t>& out, std::string& why) {
    if (data.empty()) { why = "empty input"; return false; }
    if (data.size() > 0x7fffffff / 8) { why = "input too large"; return false; }
    const bool backwards = method == "zx0b" || method == "zx1b" || method == "zx7b";
    std::vector<uint8_t> input = data;
    if (backwards) std::reverse(input.begin(), input.end());
    if (method == "zx0" || method == "zx0b") {
        BlockPool pool;
        Block* best = optimize(pool, input.data(), (int)input.size(), MAX_OFFSET_ZX0, false);
        out = compressZx0(best, input.data(), backwards, !backwards);
    } else if (method == "zx1" || method == "zx1b") {
        BlockPool pool;
        Block* best = optimize(pool, input.data(), (int)input.size(), MAX_OFFSET_ZX1 - (backwards ? 1 : 0), true);
        out = compressZx1(best, input.data(), backwards);
    } else if (method == "zx7b") {
        std::vector<Optimal> optimal = optimize7(input.data(), input.size());
        out = compress7(optimal, input.data(), input.size());
    } else if (method == "rlewb") {
        // RLE-WB (format by Eric Boez, 2019, for FUSION-C; cpctelera_next 1.04a's
        // tools/rlewb, binary mode "0 3"). Written here from the format, not from that
        // program's source: 2 to 251 equal bytes -> &80 count byte; a lone &80 -> &80 &00;
        // any other byte as it is; the end -> &80 &FF &80 &FF. The original reads one byte
        // past the input's end from memory it never sets, so its last run can vary; here a
        // run stops at the end of the data.
        const uint8_t CODE = 0x80;
        for (size_t r = 0; r < input.size();) {
            size_t e = 1;
            while (r + e < input.size() && input[r + e] == input[r] && e < 251) e++;
            if (e >= 2) { out.push_back(CODE); out.push_back((uint8_t)e); out.push_back(input[r]); r += e; }
            else if (input[r] == CODE) { out.push_back(CODE); out.push_back(0); r++; }
            else { out.push_back(input[r]); r++; }
        }
        for (int k = 0; k < 2; k++) { out.push_back(CODE); out.push_back(0xFF); }
    } else {
        why = "unknown compressor '" + method + "' (zx7b, zx1, zx1b, zx0, zx0b or rlewb)";
        return false;
    }
    if (backwards) std::reverse(out.begin(), out.end());
    return true;
}

} // namespace cpcse
