// CPCSyntaxError — Cassette image and pulse engine.
#include "tape.h"
#include "ay.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <regex>

namespace cpcse {

static const uint8_t TZX_SIGNATURE[8] = { 0x5a, 0x58, 0x54, 0x61, 0x70, 0x65, 0x21, 0x1a };
static const int TAPE_RELAY_DELAY_CYCLES = 1500000;
static const int PULSE_REFILL_THRESHOLD = 2048;
static const int MAX_CONTROL_BLOCK_STEPS = 100000;
static const size_t MAX_CALL_DEPTH = 65536;           // TZX 0x26 return addresses outstanding
static const long long MAX_BLOCK_PULSES = 30000000;   // one block's pulses (an hour of WAV is ~15M)

static void requireBytes(const Bytes& data, int offset, int length, const std::string& context = "tape image") {
    // Never offset + length: a block length read from the file can be near 2^31 and wrap.
    if (offset < 0 || length < 0 || offset > (int)data.size() || length > (int)data.size() - offset) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "0x%X", std::max(0, offset));
        throw std::runtime_error("Truncated " + context + " at offset " + buf);
    }
}
static unsigned get2(const Bytes& data, int p) { requireBytes(data, p, 2, "16-bit tape field"); return (unsigned)(data[p] | (data[p + 1] << 8)); }
static unsigned get3(const Bytes& data, int p) { requireBytes(data, p, 3, "24-bit tape field"); return (unsigned)(data[p] | (data[p + 1] << 8) | (data[p + 2] << 16)); }
static unsigned get4(const Bytes& data, int p) { requireBytes(data, p, 4, "32-bit tape field"); return (unsigned)(data[p] | (data[p + 1] << 8) | (data[p + 2] << 16) | (data[p + 3] << 24)); }
static int signed16(int value) { return value & 0x8000 ? value - 0x10000 : value; }
static bool hasTzxSignature(const Bytes& data) {
    if (data.size() < 10) return false;
    for (int i = 0; i < 8; i++) if (data[i] != TZX_SIGNATURE[i]) return false;
    return true;
}
static bool isStructurallyValidTap(const Bytes& data) {
    if (data.empty()) return false;
    int p = 0;
    while (p < (int)data.size()) {
        if (p + 2 > (int)data.size()) return false;
        int length = (unsigned)(data[p] | (data[p + 1] << 8));
        p += 2;
        if (p + length > (int)data.size()) return false;
        p += length;
    }
    return p == (int)data.size();
}
static bool endsWithCi(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); i++)
        if (std::tolower((unsigned char)s[s.size() - suffix.size() + i]) != std::tolower((unsigned char)suffix[i])) return false;
    return true;
}

// A sampled recording (.wav: RIFF PCM, 8 or 16 bits, any channels -- the first is used)
// as a TZX holding one CSW block (0x18), so the one pulse engine plays it. The signal is
// squared with a Schmitt trigger at a tenth of its peak either side of its middle, so hiss
// near zero makes no edges; each run of samples between two edges is one pulse.
static Bytes wavToTzx(const Bytes& wav) {
    auto u16 = [&](size_t o) { return (unsigned)(wav[o] | wav[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (uint32_t)(wav[o] | wav[o + 1] << 8 | wav[o + 2] << 16 | (uint32_t)wav[o + 3] << 24); };
    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAV file");
    unsigned format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t dataAt = 0, dataSize = 0;
    for (size_t p = 12; p + 8 <= wav.size();) {
        const uint32_t size = u32(p + 4);
        if (size > wav.size() - (p + 8)) { if (std::memcmp(wav.data() + p, "data", 4) == 0) { dataAt = p + 8; dataSize = wav.size() - (p + 8); } break; }
        if (std::memcmp(wav.data() + p, "fmt ", 4) == 0 && size >= 16) {
            format = u16(p + 8); channels = u16(p + 10); rate = u32(p + 12); bits = u16(p + 22);
        } else if (std::memcmp(wav.data() + p, "data", 4) == 0) {
            dataAt = p + 8; dataSize = size;
        }
        p += 8 + size + (size & 1);   // chunks are padded to an even size
    }
    if (format != 1 || (bits != 8 && bits != 16) || !channels || channels > 8 || rate < 1000 || rate > 0xffffff || !dataAt)
        throw std::runtime_error("Unsupported WAV (PCM, 8 or 16 bits, is what a tape is)");
    const size_t frame = channels * (bits / 8), samples = dataSize / frame;
    auto sample = [&](size_t i) -> int {
        const size_t o = dataAt + i * frame;
        return bits == 8 ? ((int)wav[o] - 128) * 256 : (int16_t)u16(o);
    };
    long long sum = 0;
    int peak = 0;
    for (size_t i = 0; i < samples; i++) sum += sample(i);
    const int middle = samples ? (int)(sum / (long long)samples) : 0;
    for (size_t i = 0; i < samples; i++) peak = std::max(peak, std::abs(sample(i) - middle));
    const int hysteresis = std::max(64, peak / 10);
    Bytes rle;
    uint32_t pulses = 0;
    int level = -1;               // not yet known
    uint32_t run = 0;
    auto putRun = [&](uint32_t n) {
        if (n == 0) return;
        if (n < 256) rle.push_back((uint8_t)n);
        else { rle.push_back(0); for (int k = 0; k < 4; k++) rle.push_back((uint8_t)(n >> (8 * k))); }
        pulses += 1;
    };
    for (size_t i = 0; i < samples; i++) {
        const int v = sample(i) - middle;
        int now = level;
        if (v > hysteresis) now = 1;
        else if (v < -hysteresis) now = 0;
        if (level >= 0 && now != level) { putRun(run); run = 0; }
        level = now;
        run += 1;
    }
    putRun(run);
    if (!pulses) throw std::runtime_error("The WAV holds no signal");
    Bytes t = { 'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20, 0x18 };
    const uint32_t body = 10 + (uint32_t)rle.size();
    for (int k = 0; k < 4; k++) t.push_back((uint8_t)(body >> (8 * k)));
    t.push_back(0); t.push_back(0);                                           // no pause after
    for (int k = 0; k < 3; k++) t.push_back((uint8_t)(rate >> (8 * k)));
    t.push_back(1);                                                           // RLE
    for (int k = 0; k < 4; k++) t.push_back((uint8_t)(pulses >> (8 * k)));
    t.insert(t.end(), rle.begin(), rle.end());
    return t;
}

CPCTapeDrive::CPCTapeDrive(AY38912* ay, bool requireMotor, double tstateFrequency)
    : ay(ay), requireMotor(requireMotor), tstateRatio(tstateFrequency / 3500000.0) {
    reset();
}

bool CPCTapeDrive::isMotorActive() { return requireMotor ? motorOn : true; }
bool CPCTapeDrive::isActive() { return isMotorActive() && loaded && playing && !paused && !tapeEnded; }

void CPCTapeDrive::reset() {
    motorOn = false;
    tapeCounter = 0;
    tapeCounterCycles = 0;
    startupDelayCycles = 0;
    playing = false;
    rewind();
    setTapeNoise(0);
}
void CPCTapeDrive::eject() {
    loaded = false;
    recording = false; modified = false; blockOpen = false; recorded.clear(); recordPulses.clear();
    paused = false;
    playedCycles = counterZeroCycles = totalCycles = 0;
    blockStartCycles.clear();
    name = "";
    format = "";
    data.clear();
    blocks.clear();
    playing = false;
    currentBlock = 0;
    tapeEnded = false;
    pulseQueue.clear();
    pulseQueueIndex = 0;
    pulseCyclesLeft = 0;
    tapeCounter = 0;
    tapeCounterCycles = 0;
    startupDelayCycles = 0;
    setTapeNoise(0);
}

void CPCTapeDrive::parseTzx(const Bytes& data) {
    if (!hasTzxSignature(data)) throw std::runtime_error("Not a valid TZX/CDT file");
    int p = 10;
    while (p < (int)data.size()) {
        int fileOffset = p;
        int id = data[p++];
        int start = p;
        int length;
        switch (id) {
            case 0x10: length = get2(data, p + 2) + 4; break;
            case 0x11: length = get3(data, p + 15) + 18; break;
            case 0x12: length = 4; break;
            case 0x13: requireBytes(data, p, 1, "TZX pulse-sequence block"); length = (data[p] & 0xff) * 2 + 1; break;
            case 0x14: length = get3(data, p + 7) + 10; break;
            case 0x15: length = get3(data, p + 5) + 8; break;
            // the C64 blocks start with their own length, as the extension rule's blocks do
            case 0x16: length = get4(data, p) + 4; break;
            case 0x17: length = get4(data, p) + 4; break;
            case 0x18:
            case 0x19: length = get4(data, p) + 4; break;
            case 0x20: length = 2; break;
            case 0x21: requireBytes(data, p, 1, "TZX group-start block"); length = (data[p] & 0xff) + 1; break;
            case 0x22: length = 0; break;
            case 0x23: length = 2; break;
            case 0x24: length = 2; break;
            case 0x25: length = 0; break;
            case 0x26: length = get2(data, p) * 2 + 2; break;
            case 0x27: length = 0; break;
            case 0x28: length = get2(data, p) + 2; break;
            case 0x2a: length = 4; break;
            case 0x2b: length = get4(data, p) + 4; break;
            case 0x30: requireBytes(data, p, 1, "TZX text-description block"); length = (data[p] & 0xff) + 1; break;
            case 0x31: requireBytes(data, p, 2, "TZX message block"); length = (data[p + 1] & 0xff) + 2; break;
            case 0x32: length = get2(data, p) + 2; break;
            case 0x33: requireBytes(data, p, 1, "TZX hardware-info block"); length = (data[p] & 0xff) * 3 + 1; break;
            case 0x34: length = 8; break;
            case 0x35: length = get4(data, p + 10) + 14; break;
            case 0x40: length = get3(data, p + 1) + 4; break;
            case 0x5a: length = 9; break;
            default: length = get4(data, p) + 4; break;
        }
        char idbuf[8]; std::snprintf(idbuf, sizeof(idbuf), "0x%02X", id);
        requireBytes(data, start, length, std::string("TZX block ") + idbuf);
        TapeBlock b; b.id = id; b.start = start; b.length = length; b.fileOffset = fileOffset;
        blocks.push_back(b);
        p += length;
    }
}
void CPCTapeDrive::parseTap(const Bytes& data) {
    int p = 0;
    while (p < (int)data.size()) {
        requireBytes(data, p, 2, "TAP block length");
        int length = get2(data, p);
        int fileOffset = p;
        p += 2;
        requireBytes(data, p, length, "TAP data block");
        TapeBlock b; b.id = 0xfe; b.start = p; b.length = length; b.fileOffset = fileOffset;
        blocks.push_back(b);
        p += length;
    }
}
bool CPCTapeDrive::load(const Bytes& input, const std::string& fileName) {
    const bool riff = input.size() >= 12 && std::memcmp(input.data(), "RIFF", 4) == 0;
    try { data = riff ? wavToTzx(input) : input; }
    catch (...) { eject(); throw; }
    bool tapName = endsWithCi(fileName, ".tap");
    bool tzx = hasTzxSignature(data);
    playing = false;
    motorOn = false;
    loaded = false;
    recording = false; modified = false; blockOpen = false;
    name = fileName;
    format = "";
    blocks.clear();
    lastError = "";
    try {
        if (tzx) {
            format = riff ? "WAV" : endsWithCi(fileName, ".cdt") ? "CDT" : "TZX";
            parseTzx(data);
        } else if (tapName || isStructurallyValidTap(data)) {
            format = "TAP";
            parseTap(data);
        } else {
            throw std::runtime_error("Unsupported tape image (expected TZX/CDT or TAP)");
        }
        if (blocks.empty()) throw std::runtime_error("Tape image contains no blocks");
    } catch (...) {
        eject();
        throw;
    }
    loaded = true;
    buildTimeline();
    rewind();
    return true;
}
void CPCTapeDrive::rewind() {
    playedCycles = 0;
    pulseCarry = 0;
    counterZeroCycles = 0;
    currentBlock = 0;
    tapeEnded = false;
    pulseQueue.clear();
    pulseQueueIndex = 0;
    pulseCyclesLeft = 0;
    tapeCounter = 0;
    tapeCounterCycles = 0;
    startupDelayCycles = tapeRelayDelay && loaded ? TAPE_RELAY_DELAY_CYCLES : 0;
    currentPulseLevel = initialSignalLevel ? 1 : 0;
    portBBit = currentPulseLevel ? 0x80 : 0x00;
    ampHigh = false;
    loopStack.clear();
    callStack.clear();
    lastError = "";
}
void CPCTapeDrive::setMotor(bool on) {
    bool oldMotor = motorOn;
    motorOn = on;
    if (recording) { if (oldMotor && !on) endRecordedBlock(); if (!isActive()) setTapeNoise(0); return; }
    if (motorOn && !oldMotor && loaded && !tapeEnded) {
        startupDelayCycles = tapeRelayDelay ? TAPE_RELAY_DELAY_CYCLES : 0;
        playing = true;
    }
    if (!isActive()) setTapeNoise(0);
}
// PPI port B bit 7, the cassette read input. It carries the head's signal only while a
// tape is actually playing under it; with the motor off, no tape, or the tape ended there
// is no signal and the input reads 0. The real CPC photographed for SHAKER reads PPI.B as
// #5E/#5F on AI/E and C3/A with nothing in the deck -- bit 7 clear. We reported the level a
// tape STARTS at (initialSignalLevel) whenever the deck was idle, and printed #DE/#DF.
int CPCTapeDrive::getPortBBit() { return isActive() && !recording ? portBBit : 0x00; }
void CPCTapeDrive::setTapeNoise(double level) { if (ay) ay->tapeNoise = level; }
void CPCTapeDrive::addPulse(double cycles, int level) {
    // Whole cycles, with what each rounding leaves carried into the next pulse: 2168 T is
    // 619.43 us, and rounding every one alone ran a pilot tone 0.07% fast.
    const double exact = cycles + pulseCarry;
    int c = std::max(1, (int)std::lround(exact));
    pulseCarry = exact - c;
    PulseEvent e; e.type = PulseEvent::PULSE; e.cycles = c; e.level = level ? 1 : 0; pulseQueue.push_back(e);
}
void CPCTapeDrive::addLevel(int level) { PulseEvent e; e.type = PulseEvent::LEVEL; e.level = level ? 1 : 0; pulseQueue.push_back(e); }
void CPCTapeDrive::addStop(const char* reason) { PulseEvent e; e.type = PulseEvent::STOP; e.reason = reason; pulseQueue.push_back(e); }
double CPCTapeDrive::tstatesToCycles(double tstates) { return tstates * tstateRatio; }
double CPCTapeDrive::msToCycles(double ms) { return ms * (tstateRatio * 3500); }
void CPCTapeDrive::emitPilot(double tstates, int count) {
    double c = tstatesToCycles(tstates);
    for (int i = 0; i < count; i += 1) { ampHigh = !ampHigh; addPulse(c, ampHigh); }
}
void CPCTapeDrive::emitSync(double t1, double t2) {
    if (t1 > 0) { ampHigh = !ampHigh; addPulse(tstatesToCycles(t1), ampHigh); }
    if (t2 > 0) { ampHigh = !ampHigh; addPulse(tstatesToCycles(t2), ampHigh); }
}
// A block's pulses are queued whole: one may not ask for more than MAX_BLOCK_PULSES (a
// crafted turbo block's 16 MB of data would be 268 million of them, gigabytes).
static void requirePulses(long long pulses, const char* what) {
    if (pulses > MAX_BLOCK_PULSES) throw std::runtime_error(std::string(what) + " is too long");
}
void CPCTapeDrive::emitData(int p, int length, double t0, double t1, int lastBits) {
    requirePulses((long long)std::max(0, length) * 16, "Data block");
    double c0 = tstatesToCycles(t0);
    double c1 = tstatesToCycles(t1);
    int finalBits = lastBits >= 1 && lastBits <= 8 ? lastBits : 8;
    for (int i = 0; i < length; i += 1) {
        int byteVal = data[p + i] & 0xff;
        int bits = i == length - 1 ? finalBits : 8;
        for (int bit = 7; bit >= 8 - bits; bit -= 1) {
            double c = ((byteVal >> bit) & 1) ? c1 : c0;
            ampHigh = !ampHigh; addPulse(c, ampHigh);
            ampHigh = !ampHigh; addPulse(c, ampHigh);
        }
    }
}
void CPCTapeDrive::emitDirect(int p, int length, double tstatesPerSample, int bitsUsedInLastByte) {
    requirePulses((long long)std::max(0, length) * 8, "Direct recording");
    double c = tstatesToCycles(tstatesPerSample);
    int finalBits = bitsUsedInLastByte >= 1 && bitsUsedInLastByte <= 8 ? bitsUsedInLastByte : 8;
    for (int i = 0; i < length; i += 1) {
        int byteVal = data[p + i] & 0xff;
        int bits = i == length - 1 ? finalBits : 8;
        for (int bit = 7; bit >= 8 - bits; bit -= 1) {
            int level = (byteVal >> bit) & 1;
            addPulse(c, level);
            ampHigh = level == 1;
        }
    }
}
void CPCTapeDrive::emitCsw(TapeBlock& block) {
    int p = block.start;
    // the 10-byte header after the length, before any field of it is read (the compression
    // byte at +9 was read unchecked)
    if (block.length < 14) throw std::runtime_error("Truncated TZX CSW recording block");
    requireBytes(data, p, 14, "TZX CSW recording block");
    int pause = get2(data, p + 4);
    int sampleRate = get3(data, p + 6);
    int compression = data[p + 9] & 0xff;
    int expectedPulses = get4(data, p + 10);
    if (!sampleRate) throw std::runtime_error("Invalid zero sample rate in TZX CSW block");
    if (compression != 1) throw std::runtime_error("Only RLE CSW tape blocks are supported (Z-RLE is not)");
    const Bytes rleStorage(data.begin() + (p + 14), data.begin() + (p + block.length));
    const Bytes* rle = &rleStorage;
    int offset = 0;
    int pulseCount = 0;
    while (offset < (int)rle->size() && (!expectedPulses || pulseCount < expectedPulses)) {
        int samples = (*rle)[offset++];
        if (samples == 0) {
            if (offset + 4 > (int)rle->size()) throw std::runtime_error("Truncated CSW extended pulse");
            samples = get4(*rle, offset);
            offset += 4;
        }
        if (!samples) throw std::runtime_error("Invalid zero-length CSW pulse");
        if (pulseCount >= MAX_BLOCK_PULSES) throw std::runtime_error("CSW recording is too long");
        ampHigh = !ampHigh;
        addPulse(tstatesToCycles(samples * 3500000.0 / sampleRate), ampHigh);
        pulseCount += 1;
    }
    if (expectedPulses && pulseCount != expectedPulses) {
        throw std::runtime_error("CSW pulse count mismatch (" + std::to_string(pulseCount) + "/" + std::to_string(expectedPulses) + ")");
    }
    emitPause(pause);
}
CPCTapeDrive::GenParsed CPCTapeDrive::readGeneralizedDefinitions(int offset, int alphabetSize, int maximumPulses, int limit) {
    std::vector<GeneralizedDefinition> definitions;
    int definitionSize = 1 + maximumPulses * 2;
    requireBytes(data, offset, alphabetSize * definitionSize, "TZX generalized symbol table");
    if (offset + alphabetSize * definitionSize > limit) throw std::runtime_error("Generalized symbol table exceeds its TZX block");
    for (int symbol = 0; symbol < alphabetSize; symbol += 1) {
        int start = offset + symbol * definitionSize;
        std::vector<int> pulses;
        for (int i = 0; i < maximumPulses; i += 1) {
            int length = get2(data, start + 1 + i * 2);
            if (!length) break;
            pulses.push_back(length);
        }
        definitions.push_back({ data[start] & 3, pulses });
    }
    return { definitions, offset + alphabetSize * definitionSize };
}
void CPCTapeDrive::emitGeneralizedSymbol(const GeneralizedDefinition& definition) {
    switch (definition.flags & 3) {
        case 0: ampHigh = !ampHigh; break;
        case 1: break;
        case 2: ampHigh = false; break;
        case 3: ampHigh = true; break;
        default: break;
    }
    for (int i = 0; i < (int)definition.pulses.size(); i += 1) {
        if (i) ampHigh = !ampHigh;
        addPulse(tstatesToCycles(definition.pulses[i]), ampHigh);
    }
}
void CPCTapeDrive::emitGeneralized(TapeBlock& block) {
    int p = block.start;
    int limit = p + block.length;
    if (block.length < 18) throw std::runtime_error("Truncated TZX generalized data block");
    int pause = get2(data, p + 4);
    int pilotRecords = get4(data, p + 6);
    if (pilotRecords < 0 || (long long)pilotRecords * 3 > block.length) throw std::runtime_error("Generalized pilot stream exceeds its TZX block");
    int pilotMaximumPulses = data[p + 10] & 0xff;
    int pilotAlphabetSize = (data[p + 11] & 0xff) ? (data[p + 11] & 0xff) : 256;
    int dataSymbols = get4(data, p + 12);
    int dataMaximumPulses = data[p + 16] & 0xff;
    int dataAlphabetSize = (data[p + 17] & 0xff) ? (data[p + 17] & 0xff) : 256;
    int offset = p + 18;
    if (pilotRecords) {
        GenParsed parsed = readGeneralizedDefinitions(offset, pilotAlphabetSize, pilotMaximumPulses, limit);
        offset = parsed.next;
        requireBytes(data, offset, pilotRecords * 3, "TZX generalized pilot stream");
        if (offset + pilotRecords * 3 > limit) throw std::runtime_error("Generalized pilot stream exceeds its TZX block");
        long long pulses = 0;
        for (int record = 0; record < pilotRecords; record += 1) {
            int symbol = data[offset] & 0xff;
            int repetitions = get2(data, offset + 1);
            offset += 3;
            if (symbol >= (int)parsed.definitions.size()) throw std::runtime_error("Invalid generalized pilot symbol " + std::to_string(symbol));
            pulses += (long long)repetitions * (long long)(parsed.definitions[symbol].pulses.size() + 1);
            if (pulses > MAX_BLOCK_PULSES) throw std::runtime_error("Generalized pilot stream is too long");
            for (int repeat = 0; repeat < repetitions; repeat += 1) emitGeneralizedSymbol(parsed.definitions[symbol]);
        }
    }
    if (dataSymbols) {
        GenParsed parsed = readGeneralizedDefinitions(offset, dataAlphabetSize, dataMaximumPulses, limit);
        offset = parsed.next;
        int bitsPerSymbol = std::max(1, (int)std::ceil(std::log2((double)dataAlphabetSize)));
        if (dataSymbols < 0 || (double)dataSymbols * bitsPerSymbol / 8 > block.length) throw std::runtime_error("Generalized data stream exceeds its TZX block");
        int streamBytes = (int)std::ceil(bitsPerSymbol * (double)dataSymbols / 8);
        size_t longest = 1;
        for (const auto& d : parsed.definitions) longest = std::max(longest, d.pulses.size() + 1);
        if ((long long)dataSymbols * (long long)longest > MAX_BLOCK_PULSES) throw std::runtime_error("Generalized data stream is too long");
        requireBytes(data, offset, streamBytes, "TZX generalized data stream");
        if (offset + streamBytes > limit) throw std::runtime_error("Generalized data stream exceeds its TZX block");
        int bitOffset = 0;
        for (int record = 0; record < dataSymbols; record += 1) {
            int symbol = 0;
            for (int bit = 0; bit < bitsPerSymbol; bit += 1) {
                int absoluteBit = bitOffset++;
                symbol = symbol << 1 | ((data[offset + ((unsigned)absoluteBit >> 3)] >> (7 - (absoluteBit & 7))) & 1);
            }
            if (symbol >= (int)parsed.definitions.size()) throw std::runtime_error("Invalid generalized data symbol " + std::to_string(symbol));
            emitGeneralizedSymbol(parsed.definitions[symbol]);
        }
    }
    emitPause(pause);
}
int CPCTapeDrive::getSelectTarget(const TapeBlock& block, int index) {
    int p = block.start;
    if (block.length < 3) return index + 1;
    int choiceCount = data[p + 2] & 0xff;
    int offset = p + 3;
    struct Choice { int relative; std::string text; };
    std::vector<Choice> choices;
    for (int choice = 0; choice < choiceCount; choice += 1) {
        if (offset + 3 > p + block.length) break;
        int relative = signed16(get2(data, offset));
        int textLength = data[offset + 2] & 0xff;
        if (offset + 3 + textLength > p + block.length) break;
        std::string text;
        for (int k = 0; k < textLength; k++) text.push_back((char)data[offset + 3 + k]);
        choices.push_back({ relative, text });
        offset += 3 + textLength;
    }
    if (choices.empty()) return index + 1;
    // built once: a tape can pass through a select block thousands of times a second
    static const std::regex spectrum48("(^|[^0-9])48(k|[^0-9]|$)", std::regex::icase);
    static const std::regex spectrum128("128|\\+2|\\+3", std::regex::icase);
    const std::regex& modelPattern = machineModel == "zx48" ? spectrum48 : spectrum128;
    const Choice* selected = nullptr;
    for (auto& choice : choices) if (std::regex_search(choice.text, modelPattern)) { selected = &choice; break; }
    if (!selected) selected = &choices[0];
    return index + selected->relative;
}
void CPCTapeDrive::emitPause(double ms) {
    if (ms <= 0) return;
    ampHigh = !ampHigh;
    addPulse(msToCycles(std::min(1.0, ms)), ampHigh);
    ampHigh = false;
    if (ms > 1) addPulse(msToCycles(ms - 1), 0);
    else addLevel(0);
}
bool CPCTapeDrive::shouldStopOn48KBlock() {
    if (stopTapeMode == "spectrum") return machineModel == "zx48";
    return stopTapeMode == "always";
}
std::string CPCTapeDrive::decodeBlock(int index) {
    if (index < 0 || index >= (int)blocks.size()) return "end";
    TapeBlock& block = blocks[index];
    int p = block.start;
    int id = block.id;
    switch (id) {
        case 0xfe: {
            bool isHeader = block.length > 0 && (data[p] & 0xff) < 0x80;
            emitPilot(2168, isHeader ? headerPilotPulses : dataPilotPulses);
            emitSync(667, 735);
            emitData(p, block.length, 855, 1710, 8);
            emitPause(1000);
            break;
        }
        case 0x10: {
            int pause = get2(data, p);
            int dataLength = get2(data, p + 2);
            bool isHeader = dataLength > 0 && (data[p + 4] & 0xff) < 0x80;
            emitPilot(2168, isHeader ? headerPilotPulses : dataPilotPulses);
            emitSync(667, 735);
            emitData(p + 4, dataLength, 855, 1710, 8);
            emitPause(pause);
            break;
        }
        case 0x11: {
            int pilotLen = get2(data, p);
            int sync1 = get2(data, p + 2);
            int sync2 = get2(data, p + 4);
            int bit0 = get2(data, p + 6);
            int bit1 = get2(data, p + 8);
            int pilotNum = get2(data, p + 10);
            int lastBits = data[p + 12] & 0xff;
            int pause = get2(data, p + 13);
            int dataLength = get3(data, p + 15);
            emitPilot(pilotLen, pilotNum);
            emitSync(sync1, sync2);
            emitData(p + 18, dataLength, bit0, bit1, lastBits);
            emitPause(pause);
            break;
        }
        case 0x12: {
            int length = get2(data, p);
            int pulseCount = get2(data, p + 2);
            double c = tstatesToCycles(length);
            for (int i = 0; i < pulseCount; i += 1) { ampHigh = !ampHigh; addPulse(c, ampHigh); }
            break;
        }
        case 0x13: {
            int pulseCount = data[p] & 0xff;
            for (int i = 0; i < pulseCount; i += 1) {
                int length = get2(data, p + 1 + i * 2);
                ampHigh = !ampHigh; addPulse(tstatesToCycles(length), ampHigh);
            }
            break;
        }
        case 0x14: {
            int bit0 = get2(data, p);
            int bit1 = get2(data, p + 2);
            int lastBits = data[p + 4] & 0xff;
            int pause = get2(data, p + 5);
            int dataLength = get3(data, p + 7);
            emitData(p + 10, dataLength, bit0, bit1, lastBits);
            emitPause(pause);
            break;
        }
        case 0x15: {
            int tstatesPerSample = get2(data, p);
            int pause = get2(data, p + 2);
            int bitsInLastByte = data[p + 4] & 0xff;
            int dataLength = get3(data, p + 5);
            emitDirect(p + 8, dataLength, tstatesPerSample, bitsInLastByte);
            emitPause(pause);
            break;
        }
        case 0x18: emitCsw(block); break;
        case 0x19: emitGeneralized(block); break;
        case 0x20: {
            int ms = get2(data, p);
            if (ms == 0) { addStop("pause"); return "stop"; }
            emitPause(ms);
            break;
        }
        case 0x23: { currentBlock = index + signed16(get2(data, p)); return "jump"; }
        case 0x24: { loopStack.push_back({ index + 1, (int)get2(data, p) }); break; }
        case 0x25: {
            if (!loopStack.empty()) {
                LoopFrame& loop = loopStack.back();
                loop.remaining -= 1;
                if (loop.remaining > 0) { currentBlock = loop.start; return "jump"; }
                loopStack.pop_back();
            }
            break;
        }
        case 0x26: {
            int callCount = get2(data, p);
            std::vector<int> destinations;
            for (int i = 0; i < callCount; i += 1) destinations.push_back(index + signed16(get2(data, p + 2 + i * 2)));
            // unshift(...destinations.slice(1), index+1)
            std::vector<int> toInsert;
            for (int i = 1; i < (int)destinations.size(); i++) toInsert.push_back(destinations[i]);
            toInsert.push_back(index + 1);
            callStack.insert(callStack.begin(), toInsert.begin(), toInsert.end());
            if (!destinations.empty()) { currentBlock = destinations[0]; return "jump"; }
            break;
        }
        case 0x27: {
            if (!callStack.empty()) { currentBlock = callStack.front(); callStack.pop_front(); return "jump"; }
            break;
        }
        case 0x28: currentBlock = getSelectTarget(block, index); return "jump";
        case 0x2a: {
            if (shouldStopOn48KBlock()) { addStop("48k"); return "stop"; }
            break;
        }
        case 0x2b: {
            if (block.length >= 5) {
                int level = data[p + 4] & 1;
                ampHigh = level == 1;
                addLevel(level);
            }
            break;
        }
        default: break;
    }
    return "next";
}
void CPCTapeDrive::ensurePulses() {
    if (pulseQueueIndex > 4096) {
        pulseQueue.erase(pulseQueue.begin(), pulseQueue.begin() + pulseQueueIndex);
        pulseQueueIndex = 0;
    }
    int controlSteps = 0;
    while ((int)pulseQueue.size() - pulseQueueIndex < PULSE_REFILL_THRESHOLD
        && currentBlock >= 0 && currentBlock < (int)blocks.size()) {
        int index = currentBlock;
        std::string result;
        // A block the image gets wrong stops the tape, as a deck stops on a bad block; the
        // error must not leave the emulation loop (it would end the program).
        try { result = decodeBlock(index); }
        catch (const std::exception& ex) {
            lastError = ex.what();
            addStop("block-error");
            currentBlock = index + 1;
            break;
        }
        if (callStack.size() > MAX_CALL_DEPTH) {
            lastError = "TZX call sequences nest too deeply";
            addStop("control-flow-error");
            break;
        }
        if (result != "jump") currentBlock = index + 1;
        controlSteps += 1;
        if (result == "stop") break;
        if (controlSteps > MAX_CONTROL_BLOCK_STEPS) {
            lastError = "TZX control flow did not make progress";
            addStop("control-flow-error");
            break;
        }
    }
}
bool CPCTapeDrive::processQueueEvent(const PulseEvent& event) {
    if (event.type == PulseEvent::STOP) {
        playing = false;
        pulseCyclesLeft = 0;
        currentPulseLevel = 0;
        portBBit = 0x00;
        ampHigh = false;
        setTapeNoise(0);
        return false;
    }
    currentPulseLevel = event.level ? 1 : 0;
    portBBit = currentPulseLevel ? 0x80 : 0x00;
    if (event.type == PulseEvent::PULSE) {
        pulseCyclesLeft = event.cycles;
    }
    return true;
}
void CPCTapeDrive::advanceCycles(int cycles) {
    int elapsedCycles = std::max(0, cycles);
    if (recording) {
        if (isMotorActive() && playing && !paused) {
            recordClock += elapsedCycles;
            playedCycles = recordFrom + recordClock;
            if (blockOpen && recordClock - lastEdge > RECORD_GAP_US) endRecordedBlock();   // a silence: the block ends
        }
        return;
    }
    if (!isActive()) { setTapeNoise(0); return; }
    if (startupDelayCycles > 0) {
        ensurePulses();
        startupDelayCycles = std::max(0, startupDelayCycles - elapsedCycles);
        setTapeNoise(0);
        return;
    }
    playedCycles += elapsedCycles;
    tapeCounterCycles += elapsedCycles;
    while (tapeCounterCycles >= 1000000) {   // once a second of tape, not every step
        tapeCounterCycles -= 1000000;
        tapeCounter = counter();
    }
    int remaining = elapsedCycles;
    ensurePulses();
    while (remaining > 0 && isActive()) {
        if (pulseCyclesLeft > 0) {
            int step = std::min(remaining, pulseCyclesLeft);
            pulseCyclesLeft -= step;
            remaining -= step;
            continue;
        }
        if (pulseQueueIndex >= (int)pulseQueue.size()) {
            ensurePulses();
            if (pulseQueueIndex >= (int)pulseQueue.size()) {
                if (currentBlock < 0 || currentBlock >= (int)blocks.size()) {
                    tapeEnded = true;
                    playing = false;
                    currentPulseLevel = 0;
                    portBBit = 0x00;
                }
                break;
            }
        }
        const PulseEvent& event = pulseQueue[pulseQueueIndex++];
        if (!processQueueEvent(event)) break;
    }
    setTapeNoise(isActive() ? (portBBit == 0x80 ? 0.4 : -0.4) : 0);
}
void CPCTapeDrive::jumpToBlock(int index) {
    if (!loaded || index < 0 || index >= (int)blocks.size()) return;
    if (index < (int)blockStartCycles.size()) playedCycles = blockStartCycles[(size_t)index];
    pulseCarry = 0;
    currentBlock = index;
    pulseQueue.clear();
    pulseQueueIndex = 0;
    pulseCyclesLeft = 0;
    currentPulseLevel = initialSignalLevel ? 1 : 0;
    portBBit = currentPulseLevel ? 0x80 : 0x00;
    ampHigh = false;
    loopStack.clear();
    callStack.clear();
    tapeEnded = false;
}
std::string CPCTapeDrive::getBlockDescription(int index) {
    if (index < 0 || index >= (int)blocks.size()) return "Invalid Block";
    TapeBlock& block = blocks[index];
    int id = block.id;
    auto n = [](int v) { return std::to_string(std::max(0, v)); };
    std::string i = std::to_string(index);
    switch (id) {
        case 0xfe: {
            int flag = block.length ? data[block.start] & 0xff : -1;
            int length = std::max(0, block.length - 2);
            return flag == 0x00 ? "[" + i + "] TAP Header (" + std::to_string(length) + " B)" : "[" + i + "] TAP Data (" + std::to_string(length) + " B)";
        }
        case 0x10: return "[" + i + "] Standard Speed Data (" + n(block.length - 4) + " B)";
        case 0x11: return "[" + i + "] Turbo Loading Data (" + n(block.length - 18) + " B)";
        case 0x12: return "[" + i + "] Pure Tone";
        case 0x13: return "[" + i + "] Pulse Sequence";
        case 0x14: return "[" + i + "] Pure Data (" + n(block.length - 10) + " B)";
        case 0x15: return "[" + i + "] Direct Recording";
        case 0x16: return "[" + i + "] C64 ROM Loader (" + n(block.length - 4) + " B)";
        case 0x17: return "[" + i + "] C64 Turbo Loader (" + n(block.length - 4) + " B)";
        case 0x18: return "[" + i + "] CSW Recording";
        case 0x19: return "[" + i + "] Generalized Data Block";
        case 0x20: return "[" + i + "] Pause";
        case 0x21: return "[" + i + "] Group Start";
        case 0x22: return "[" + i + "] Group End";
        case 0x23: return "[" + i + "] Jump to Block";
        case 0x24: return "[" + i + "] Loop Start";
        case 0x25: return "[" + i + "] Loop End";
        case 0x26: return "[" + i + "] Call Sequence";
        case 0x27: return "[" + i + "] Return";
        case 0x28: return "[" + i + "] Select Block";
        case 0x2a: return "[" + i + "] Stop Tape in 48K Mode";
        case 0x2b: return "[" + i + "] Set Signal Level";
        case 0x30: return "[" + i + "] Text Description";
        case 0x31: return "[" + i + "] Message Block";
        case 0x32: return "[" + i + "] Archive Info";
        case 0x33: return "[" + i + "] Hardware Type";
        case 0x35: return "[" + i + "] Custom Info";
        case 0x5a: return "[" + i + "] Glue Block";
        default: { char b[16]; std::snprintf(b, sizeof(b), "0x%X", id); return "[" + i + "] Block " + b + " (" + std::to_string(block.length) + " B)"; }
    }
}
// Each block's start on the tape: every block decoded once, in order, and its pulses
// added up. Jumps and loops are not followed -- the tape is a length of tape, whatever a
// loader then does with it.
void CPCTapeDrive::buildTimeline() {
    blockStartCycles.assign(blocks.size(), 0);
    pulseCarry = 0;
    long long at = 0, pulses = 0;
    for (size_t i = 0; i < blocks.size(); i++) {
        blockStartCycles[i] = at;
        // A real tape is a few million pulses; a crafted one of tiny blocks each asking for
        // 65535 could take minutes to measure. Past this the blocks after are not measured
        // (they all start where the measuring stopped).
        if (pulses > 2 * MAX_BLOCK_PULSES) continue;   // 60 million: some eight hours of CPC tape
        pulseQueue.clear();
        try { decodeBlock((int)i); } catch (const std::exception&) {}   // a bad block: no length
        for (const PulseEvent& e : pulseQueue) if (e.type == PulseEvent::PULSE) at += e.cycles;
        pulses += (long long)pulseQueue.size();
    }
    totalCycles = at;
    pulseQueue.clear();
    pulseQueueIndex = 0;
    loopStack.clear();
    callStack.clear();
    currentBlock = 0;
    ampHigh = false;
}

int CPCTapeDrive::blockAtPosition() const {
    if (!loaded || blockStartCycles.empty()) return -1;
    // the last block starting at or before the head (the starts never go back): a binary
    // search, as a tape can have a million blocks and this is asked every frame
    auto after = std::upper_bound(blockStartCycles.begin() + 1, blockStartCycles.end(), playedCycles);
    return (int)(after - blockStartCycles.begin()) - 1;
}

int CPCTapeDrive::counter() const {
    const long long since = playedCycles - counterZeroCycles;
    const long long seconds = (long long)(since / cyclesPerSecond());
    return (int)(((seconds % 1000) + 1000) % 1000);   // below its 000 it counts back from 999
}

void CPCTapeDrive::play() {
    if (!loaded || tapeEnded) return;
    playing = true;
    paused = false;
}

void CPCTapeDrive::setPaused(bool on) {
    paused = on && playing;
    if (!isActive()) setTapeNoise(0);
}

void CPCTapeDrive::stop() {
    playing = false;
    paused = false;
    setTapeNoise(0);
}

void CPCTapeDrive::seekBlock(int index) {
    if (!loaded || blocks.empty()) return;
    jumpToBlock(std::clamp(index, 0, (int)blocks.size() - 1));
}

void CPCTapeDrive::fastForward() {
    const int b = blockAtPosition();
    if (b < 0) return;
    if (b + 1 < (int)blocks.size()) seekBlock(b + 1);
    else { playedCycles = totalCycles; currentBlock = (int)blocks.size(); tapeEnded = true; pulseQueue.clear(); pulseQueueIndex = 0; }
}

void CPCTapeDrive::rewindBlock() {
    int b = blockAtPosition();
    if (b < 0) return;
    if (b >= (int)blocks.size()) b = (int)blocks.size() - 1;
    // Within two seconds of a block's start, back to the one before; else to this one's start.
    const long long into = playedCycles - blockStartCycles[(size_t)b];
    if (into < (long long)(2 * cyclesPerSecond()) && b > 0) b -= 1;
    seekBlock(b);
}

// ---------------------------------------------------------------- recording

static void put16(Bytes& b, uint32_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
static void put24(Bytes& b, uint32_t v) { put16(b, v); b.push_back((uint8_t)(v >> 16)); }
static void put32(Bytes& b, uint32_t v) { put16(b, v); put16(b, v >> 16); }

void CPCTapeDrive::newBlank(const std::string& fileName) {
    eject();
    data = { 'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20 };
    name = fileName;
    format = "CDT";
    loaded = true;
    buildTimeline();
    rewind();
}

bool CPCTapeDrive::startRecording() {
    if (!loaded || !hasTzxSignature(data)) return false;            // a .TAP: not this deck's format
    if (recording) return true;
    // overwritten from the head on: every block that starts before it stays (one the head is
    // part way through is kept whole), every one from there is recorded over
    size_t first = 0;
    while (first < blocks.size() && first < blockStartCycles.size() && blockStartCycles[first] < playedCycles) first++;
    recordCut = first < blocks.size() ? (size_t)blocks[first].fileOffset : data.size();
    recordFrom = std::min(playedCycles, totalCycles);
    recording = true;
    playing = true;
    paused = false;
    tapeEnded = false;
    recorded.clear();
    recordPulses.clear();
    recordClock = 0;
    lastEdge = -1;
    lastBlockPause = -1;
    blockOpen = false;
    pulseQueue.clear(); pulseQueueIndex = 0; pulseCyclesLeft = 0;
    setTapeNoise(0);
    return true;
}

void CPCTapeDrive::setWriteLevel(int level) {
    level = level ? 1 : 0;
    if (level == writeLevel) return;
    writeLevel = level;
    if (!recording || !isMotorActive() || !playing || paused) return;
    if (!blockOpen) {
        // a block's first edge; the silence since the last one is that block's pause
        if (lastBlockPause >= 0 && lastEdge >= 0) {
            const long long ms = std::clamp<long long>((recordClock - lastEdge) / 1000, 1, 65535);
            recorded[(size_t)lastBlockPause] = (uint8_t)ms;
            recorded[(size_t)lastBlockPause + 1] = (uint8_t)(ms >> 8);
        }
        blockOpen = true;
        recordPulses.clear();
    } else {
        recordPulses.push_back((uint32_t)(recordClock - lastEdge));  // the pulse this edge ends
    }
    lastEdge = recordClock;
}

// A recorded block as a TZX turbo block (&11), when it is the shape the CPC firmware writes
// (and most loaders read): a pilot of equal pulses, two sync pulses, then bits of two equal
// pulses each -- a 0 short, a 1 twice as long. The pulses are sorted into the two lengths by
// the gap between them; the block is taken only if that sorting is beyond doubt (each bit's
// two pulses the same kind, the kinds TURBO_SEPARATION apart, the pilot within
// TURBO_TOLERANCE of even), else the recording stays as it was (CSW). Lengths are T-states
// of 3.5 MHz, as the format has them.
static constexpr double TURBO_TOLERANCE = 0.12;
static constexpr double TURBO_SEPARATION = 1.4;
static bool turboBlock(const std::vector<uint32_t>& p, int pauseMs, Bytes& out) {
    static const bool trace = std::getenv("CPCSE_TRACE_TAPEREC") != nullptr;
    auto refuse = [&](const char* why, size_t at) {
        if (trace) {
            std::fprintf(stderr, "TAPEREC no turbo block (%s at pulse %zu of %zu):", why, at, p.size());
            for (size_t i = at > 6 ? at - 6 : 0; i < p.size() && i < at + 10; i++) std::fprintf(stderr, " %u", p[i]);
            std::fprintf(stderr, "  first:");
            for (size_t i = 0; i < p.size() && i < 12; i++) std::fprintf(stderr, " %u", p[i]);
            std::fprintf(stderr, "\n");
        }
        return false;
    };
    if (p.size() < 64) return refuse("short", 0);
    // the pilot: the run of pulses like the first
    size_t pilot = 0;
    const double first = p[0];
    while (pilot < p.size() && std::abs(p[pilot] - first) <= first * TURBO_TOLERANCE) pilot++;
    if (pilot < 16 || pilot + 2 >= p.size()) return refuse("no pilot", pilot);
    double pilotLen = 0;
    for (size_t i = 0; i < pilot; i++) pilotLen += p[i];
    pilotLen /= (double)pilot;
    const double sync1 = p[pilot], sync2 = p[pilot + 1];
    // the data pulses after it, two to a bit
    const size_t start = pilot + 2;
    size_t count = p.size() - start;
    if (count & 1) count--;                                       // a lone last pulse: the line's last edge
    if (count < 16) return refuse("no data", start);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (size_t i = start; i < start + count; i++) { lo = std::min(lo, p[i]); hi = std::max(hi, p[i]); }
    const double split = (lo + hi) / 2.0;
    double sum0 = 0, sum1 = 0;
    size_t n0 = 0, n1 = 0;
    std::vector<uint8_t> bits;
    bits.reserve(count / 2);
    for (size_t i = start; i < start + count; i += 2) {
        const bool a = p[i] > split, b = p[i + 1] > split;
        if (a != b) return refuse("unequal pair", i);                // not two equal pulses: not this format
        (a ? sum1 : sum0) += p[i] + p[i + 1];
        (a ? n1 : n0) += 2;
        bits.push_back(a ? 1 : 0);
    }
    const double zero = n0 ? sum0 / (double)n0 : (n1 ? sum1 / (double)n1 / 2 : 0);
    const double one = n1 ? sum1 / (double)n1 : zero * 2;
    if (zero <= 0 || one <= zero * 1.3) return refuse("one not longer", start);
    // Unambiguous, as a reader that compares each pulse with a threshold between the two
    // lengths (the firmware's) sees it: the longest 0 pulse well short of the shortest 1 --
    // the writer's own unevenness (the firmware's pulses stretch where it fetches the next
    // byte, some 55 us at 2000 baud) then plays back as the even lengths it meant.
    uint32_t longest0 = 0, shortest1 = UINT32_MAX;
    for (size_t i = 0; i < bits.size(); i++)
        for (int k = 0; k < 2; k++) {
            const uint32_t v = p[start + 2 * i + (size_t)k];
            if (bits[i]) shortest1 = std::min(shortest1, v); else longest0 = std::max(longest0, v);
        }
    if (n0 && n1 && longest0 * TURBO_SEPARATION >= shortest1) return refuse("lengths overlap", start);
    for (size_t i = 0; i < pilot; i++) if (std::abs(p[i] - pilotLen) > pilotLen * TURBO_TOLERANCE) return refuse("pilot uneven", i);
    auto t = [](double us) { return (uint32_t)std::clamp<long long>(std::llround(us * 3.5), 1, 65535); };
    Bytes data((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) if (bits[i]) data[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    const int lastBits = bits.size() % 8 ? (int)(bits.size() % 8) : 8;
    out.push_back(0x11);
    put16(out, t(pilotLen)); put16(out, t(sync1)); put16(out, t(sync2));
    put16(out, t(zero)); put16(out, t(one));
    put16(out, (uint32_t)std::min<size_t>(pilot, 65535));
    out.push_back((uint8_t)lastBits);
    put16(out, (uint32_t)pauseMs);
    put24(out, (uint32_t)data.size());
    out.insert(out.end(), data.begin(), data.end());
    return true;
}

// The pulses so far as one block: a turbo block (&11) when they are the firmware's shape,
// else a CSW block (TZX &18: its length, pause, sample rate, RLE, pulse count, then each
// pulse in samples -- a byte, or 0 and four bytes for a long one).
void CPCTapeDrive::endRecordedBlock() {
    const bool had = blockOpen;
    blockOpen = false;
    if (!had || recordPulses.size() < 2) { recordPulses.clear(); return; }   // a stray edge is not a block
    {
        Bytes turbo;
        if (turboBlock(recordPulses, 1000, turbo)) {
            lastBlockPause = (int)(recorded.size() + 14);            // &11's pause, after its fourteen bytes
            recorded.insert(recorded.end(), turbo.begin(), turbo.end());
            recordPulses.clear();
            return;
        }
    }
    Bytes rle;
    uint32_t count = 0;
    for (size_t i = 0; i < recordPulses.size(); i++) {
        uint32_t samples = (uint32_t)((recordPulses[i] * (uint64_t)RECORD_RATE + 500000) / 1000000);
        if (samples == 0) samples = 1;
        if (samples < 256) rle.push_back((uint8_t)samples);
        else { rle.push_back(0); put32(rle, samples); }
        count++;
    }
    recorded.push_back(0x18);
    put32(recorded, (uint32_t)(10 + rle.size()));
    lastBlockPause = (int)recorded.size();
    put16(recorded, 1000);                                           // until the next block says otherwise
    put24(recorded, RECORD_RATE);
    recorded.push_back(1);                                           // RLE
    put32(recorded, count);
    recorded.insert(recorded.end(), rle.begin(), rle.end());
    recordPulses.clear();
}

void CPCTapeDrive::stopRecording() {
    if (!recording) return;
    endRecordedBlock();
    recording = false;
    playing = false;
    if (recorded.empty()) return;                                    // nothing was written
    Bytes image(data.begin(), data.begin() + (long)std::min(recordCut, data.size()));
    image.insert(image.end(), recorded.begin(), recorded.end());
    data = image;
    blocks.clear();
    parseTzx(data);
    buildTimeline();
    // the head where the recording ended: after the last block written
    playedCycles = totalCycles;
    currentBlock = (int)blocks.size();
    tapeEnded = true;
    pulseQueue.clear(); pulseQueueIndex = 0; pulseCyclesLeft = 0;
    recorded.clear();
    modified = true;
}

bool CPCTapeDrive::togglePlay() {
    if (!loaded || tapeEnded) return false;
    playing = !playing;
    paused = false;
    if (!playing) setTapeNoise(0);
    return playing;
}

} // namespace cpcse
