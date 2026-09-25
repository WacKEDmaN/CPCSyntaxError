// CPCSyntaxError — Cassette image and pulse engine.
#include "tape.h"
#include "ay.h"
#include <stdexcept>
#include <regex>

namespace cpcse {

static const uint8_t TZX_SIGNATURE[8] = { 0x5a, 0x58, 0x54, 0x61, 0x70, 0x65, 0x21, 0x1a };
static const int TAPE_RELAY_DELAY_CYCLES = 1500000;
static const int PULSE_REFILL_THRESHOLD = 2048;
static const int MAX_CONTROL_BLOCK_STEPS = 100000;

static void requireBytes(const Bytes& data, int offset, int length, const std::string& context = "tape image") {
    if (offset < 0 || length < 0 || offset + length > (int)data.size()) {
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

CPCTapeDrive::CPCTapeDrive(AY38912* ay, bool requireMotor, double tstateFrequency)
    : ay(ay), requireMotor(requireMotor), tstateRatio(tstateFrequency / 3500000.0) {
    reset();
}

bool CPCTapeDrive::isMotorActive() { return requireMotor ? motorOn : true; }
bool CPCTapeDrive::isActive() { return isMotorActive() && loaded && playing && !tapeEnded; }
bool CPCTapeDrive::isPulseActive() { return pulseLedCycles > 0; }

void CPCTapeDrive::reset() {
    motorOn = false;
    pulseLedCycles = 0;
    tapeCounter = 0;
    tapeCounterCycles = 0;
    startupDelayCycles = 0;
    playing = false;
    rewind();
    setTapeNoise(0);
}
void CPCTapeDrive::eject() {
    loaded = false;
    name = "";
    format = "";
    data.clear();
    blocks.clear();
    playing = false;
    currentBlock = 0;
    fastBlockIndex = 0;
    tapeEnded = false;
    pulseQueue.clear();
    pulseQueueIndex = 0;
    pulseCyclesLeft = 0;
    tapeCounter = 0;
    tapeCounterCycles = 0;
    startupDelayCycles = 0;
    setTapeNoise(0);
}
void CPCTapeDrive::setOutputSampleRate(double sampleRate) {
    outputSampleRate = std::max(0.0, std::isfinite(sampleRate) ? sampleRate : 0.0);
    samplePhase = 0;
    sampleQueue.clear();
    sampleReadIndex = 0;
}
double CPCTapeDrive::readSample() {
    if (sampleReadIndex < (int)sampleQueue.size()) return sampleQueue[sampleReadIndex++];
    return lastOutputSample;
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
            case 0x16: length = get4(data, p + 13) + 17; break;
            case 0x17: length = get4(data, p + 15) + 19; break;
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
    data = input;
    bool tapName = endsWithCi(fileName, ".tap");
    bool tzx = hasTzxSignature(data);
    playing = false;
    motorOn = false;
    loaded = false;
    name = fileName;
    format = "";
    blocks.clear();
    lastError = "";
    try {
        if (tzx) {
            format = endsWithCi(fileName, ".cdt") ? "CDT" : "TZX";
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
    rewind();
    return true;
}
bool CPCTapeDrive::loadAsync(const Bytes& input, const std::string& fileName) {
    load(input, fileName);
    try {
        prepareCompressedBlocks();
    } catch (...) {
        eject();
        throw;
    }
    return true;
}
void CPCTapeDrive::prepareCompressedBlocks() {
    for (auto& block : blocks) {
        if (block.id != 0x18) continue;
        int p = block.start;
        if (block.length < 14) throw std::runtime_error("Truncated TZX CSW recording block");
        int bodyLength = get4(data, p);
        if (bodyLength < 10) throw std::runtime_error("Invalid TZX CSW recording length");
        int compression = data[p + 9] & 0xff;
        Bytes encoded(data.begin() + (p + 14), data.begin() + (p + 4 + bodyLength));
        if (compression == 1) { block.cswRle = encoded; block.hasCswRle = true; }
        else if (compression == 2) throw std::runtime_error("Z-RLE compressed CSW tape blocks are not supported");
        else throw std::runtime_error("Unsupported TZX CSW compression type " + std::to_string(compression));
        block.cswPulseCount = get4(data, p + 10);
    }
}
void CPCTapeDrive::rewind() {
    currentBlock = 0;
    fastBlockIndex = 0;
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
    fastLoopStack.clear();
    fastCallStack.clear();
    fastFinished = false;
    lastError = "";
}
void CPCTapeDrive::setMotor(bool on) {
    bool oldMotor = motorOn;
    motorOn = on;
    if (motorOn && !oldMotor && loaded && !tapeEnded) {
        startupDelayCycles = tapeRelayDelay ? TAPE_RELAY_DELAY_CYCLES : 0;
        playing = true;
    }
    if (!isActive()) setTapeNoise(0);
}
bool CPCTapeDrive::isMotorOn() { return motorOn; }
// PPI port B bit 7, the cassette read input. It carries the head's signal only while a
// tape is actually playing under it; with the motor off, no tape, or the tape ended there
// is no signal and the input reads 0. The real CPC photographed for SHAKER reads PPI.B as
// #5E/#5F on AI/E and C3/A with nothing in the deck -- bit 7 clear. We reported the level a
// tape STARTS at (initialSignalLevel) whenever the deck was idle, and printed #DE/#DF.
int CPCTapeDrive::getPortBBit() { return isActive() ? portBBit : 0x00; }
int CPCTapeDrive::getEarLevel() { return portBBit == 0x80 ? 1 : 0; }
void CPCTapeDrive::setTapeNoise(double level) { if (ay) ay->tapeNoise = level; }
void CPCTapeDrive::addPulse(double cycles, int level) {
    int c = std::max(1, (int)std::lround(cycles));
    PulseEvent e; e.type = PulseEvent::PULSE; e.cycles = c; e.level = level ? 1 : 0; pulseQueue.push_back(e);
}
void CPCTapeDrive::addLevel(int level) { PulseEvent e; e.type = PulseEvent::LEVEL; e.level = level ? 1 : 0; pulseQueue.push_back(e); }
void CPCTapeDrive::addStop(const std::string& reason) { PulseEvent e; e.type = PulseEvent::STOP; e.reason = reason; pulseQueue.push_back(e); }
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
void CPCTapeDrive::emitData(int p, int length, double t0, double t1, int lastBits) {
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
    int pause = get2(data, p + 4);
    int sampleRate = get3(data, p + 6);
    int compression = data[p + 9] & 0xff;
    int expectedPulses = get4(data, p + 10);
    if (!sampleRate) throw std::runtime_error("Invalid zero sample rate in TZX CSW block");
    Bytes rleStorage;
    const Bytes* rle = block.hasCswRle ? &block.cswRle : nullptr;
    if (!rle && compression == 1) { rleStorage = Bytes(data.begin() + (p + 14), data.begin() + (p + block.length)); rle = &rleStorage; }
    if (!rle) throw std::runtime_error("Z-RLE CSW block was not prepared; use loadAsync()");
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
    int pause = get2(data, p + 4);
    int pilotRecords = get4(data, p + 6);
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
        for (int record = 0; record < pilotRecords; record += 1) {
            int symbol = data[offset] & 0xff;
            int repetitions = get2(data, offset + 1);
            offset += 3;
            if (symbol >= (int)parsed.definitions.size()) throw std::runtime_error("Invalid generalized pilot symbol " + std::to_string(symbol));
            for (int repeat = 0; repeat < repetitions; repeat += 1) emitGeneralizedSymbol(parsed.definitions[symbol]);
        }
    }
    if (dataSymbols) {
        GenParsed parsed = readGeneralizedDefinitions(offset, dataAlphabetSize, dataMaximumPulses, limit);
        offset = parsed.next;
        int bitsPerSymbol = (int)std::ceil(std::log2((double)dataAlphabetSize));
        int streamBytes = (int)std::ceil(bitsPerSymbol * (double)dataSymbols / 8);
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
    std::regex modelPattern = machineModel == "zx48"
        ? std::regex("(^|[^0-9])48(k|[^0-9]|$)", std::regex::icase)
        : std::regex("128|\\+2|\\+3", std::regex::icase);
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
        std::string result = decodeBlock(index);
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
        pulseLedCycles = std::max(pulseLedCycles, 1200);
    }
    return true;
}
void CPCTapeDrive::advanceCycles(int cycles) {
    int elapsedCycles = std::max(0, cycles);
    pulseLedCycles = std::max(0, pulseLedCycles - elapsedCycles);
    if (!isActive()) { setTapeNoise(0); return; }
    if (startupDelayCycles > 0) {
        ensurePulses();
        startupDelayCycles = std::max(0, startupDelayCycles - elapsedCycles);
        setTapeNoise(0);
        return;
    }
    tapeCounterCycles += elapsedCycles;
    while (tapeCounterCycles >= 1000000) {
        tapeCounterCycles -= 1000000;
        tapeCounter = (tapeCounter + 1) % 1000;
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
    currentBlock = index;
    fastBlockIndex = index;
    pulseQueue.clear();
    pulseQueueIndex = 0;
    pulseCyclesLeft = 0;
    currentPulseLevel = initialSignalLevel ? 1 : 0;
    portBBit = currentPulseLevel ? 0x80 : 0x00;
    ampHigh = false;
    loopStack.clear();
    callStack.clear();
    fastLoopStack.clear();
    fastCallStack.clear();
    fastFinished = false;
    tapeEnded = false;
}
std::optional<Bytes> CPCTapeDrive::getBlockData(int index) {
    if (index < 0 || index >= (int)blocks.size()) return std::nullopt;
    TapeBlock& block = blocks[index];
    int p = block.start;
    auto sub = [&](int a, int b) { return Bytes(data.begin() + a, data.begin() + b); };
    switch (block.id) {
        case 0xfe: return sub(p, p + block.length);
        case 0x10: { int dataLength = get2(data, p + 2); return sub(p + 4, p + 4 + dataLength); }
        case 0x11: { int dataLength = get3(data, p + 15); return sub(p + 18, p + 18 + dataLength); }
        case 0x14: { int dataLength = get3(data, p + 7); return sub(p + 10, p + 10 + dataLength); }
        case 0x15: { int dataLength = get3(data, p + 5); return sub(p + 8, p + 8 + dataLength); }
        default: return std::nullopt;
    }
}
std::optional<Bytes> CPCTapeDrive::getNextLoadableBlock() {
    if (!loaded || fastFinished) return std::nullopt;
    int controlSteps = 0;
    while (fastBlockIndex >= 0 && fastBlockIndex < (int)blocks.size()) {
        int index = fastBlockIndex;
        TapeBlock& block = blocks[index];
        int p = block.start;
        if (block.id == 0xfe || block.id == 0x10 || block.id == 0x11) {
            fastBlockIndex = index + 1;
            std::optional<Bytes> bytes = getBlockData(index);
            currentBlock = fastBlockIndex;
            pulseQueue.clear();
            pulseQueueIndex = 0;
            pulseCyclesLeft = 0;
            currentPulseLevel = initialSignalLevel ? 1 : 0;
            portBBit = currentPulseLevel ? 0x80 : 0x00;
            ampHigh = false;
            tapeEnded = false;
            return bytes;
        }
        switch (block.id) {
            case 0x23: fastBlockIndex = index + signed16(get2(data, p)); break;
            case 0x24: fastLoopStack.push_back({ index + 1, (int)get2(data, p) }); fastBlockIndex = index + 1; break;
            case 0x25: {
                if (!fastLoopStack.empty()) {
                    LoopFrame& loop = fastLoopStack.back();
                    loop.remaining -= 1;
                    if (loop.remaining > 0) fastBlockIndex = loop.start;
                    else { fastLoopStack.pop_back(); fastBlockIndex = index + 1; }
                } else fastBlockIndex = index + 1;
                break;
            }
            case 0x26: {
                int callCount = get2(data, p);
                std::vector<int> destinations;
                for (int i = 0; i < callCount; i += 1) destinations.push_back(index + signed16(get2(data, p + 2 + i * 2)));
                std::vector<int> toInsert;
                for (int i = 1; i < (int)destinations.size(); i++) toInsert.push_back(destinations[i]);
                toInsert.push_back(index + 1);
                fastCallStack.insert(fastCallStack.begin(), toInsert.begin(), toInsert.end());
                fastBlockIndex = !destinations.empty() ? destinations[0] : index + 1;
                break;
            }
            case 0x27: fastBlockIndex = !fastCallStack.empty() ? (fastCallStack.front()) : index + 1; if (!fastCallStack.empty()) fastCallStack.pop_front(); break;
            case 0x28: fastBlockIndex = getSelectTarget(block, index); break;
            default: fastBlockIndex = index + 1; break;
        }
        controlSteps += 1;
        if (controlSteps > MAX_CONTROL_BLOCK_STEPS) {
            lastError = "TZX fast-load control flow did not make progress";
            fastFinished = true;
            return std::nullopt;
        }
    }
    fastFinished = true;
    return std::nullopt;
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
        case 0x16: return "[" + i + "] C64 ROM Loader (" + n(block.length - 17) + " B)";
        case 0x17: return "[" + i + "] C64 Turbo Loader (" + n(block.length - 19) + " B)";
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
bool CPCTapeDrive::togglePlay() {
    if (!loaded || tapeEnded) return false;
    playing = !playing;
    if (!playing) setTapeNoise(0);
    return playing;
}

SpectrumTapeDrive::SpectrumTapeDrive(AY38912* ay) : CPCTapeDrive(ay, false, 3500000) {
    tapeRelayDelay = false;
    headerPilotPulses = 8063;
    dataPilotPulses = 3223;
    initialSignalLevel = 0;
    stopTapeMode = "spectrum";
    machineModel = "zx48";
    rewind();
}
void SpectrumTapeDrive::setModel(const std::string& model) {
    machineModel = model;
    tstateRatio = model == "zx48" ? 1 : 3546900.0 / 3500000.0;
}

} // namespace cpcse
