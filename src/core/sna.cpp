// CPCSyntaxError — CPC SNA snapshot codec.
#include "sna.h"
#include "emulator.h"
#include "memory.h"
#include "asic.h"
#include "z80.h"
#include "crtc.h"
#include "gate_array.h"
#include "ppi.h"
#include "ay.h"
#include "fdc.h"
#include <stdexcept>
#include <regex>

namespace cpcse {

static void putText(Bytes& bytes, int offset, const std::string& value) { for (int i = 0; i < (int)value.size(); i++) bytes[offset + i] = (uint8_t)value[i]; }
static const std::string SNA_EXT_MAGIC = "CPCS";
static const int SNA_EXT_OFFSET = 0xd0;
static const int SNA_EXT_OLD_OFFSET = 0xa0;
static int u16(const Bytes& b, int o) { return b[o] | b[o + 1] << 8; }
static void put16(Bytes& b, int o, int v) { b[o] = (uint8_t)(v & 255); b[o + 1] = (uint8_t)((unsigned)v >> 8 & 255); }
static unsigned read32(const Bytes& b, int o) { return (unsigned)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24); }
static void put32(Bytes& b, int o, unsigned v) { for (int i = 0; i < 4; i++) b[o + i] = (uint8_t)(v >> (i * 8) & 255); }
static const int MAX_SNA_RAM_KIB = 4160;
static std::string ascii(const Bytes& b, int off, int len) { std::string s; for (int i = 0; i < len && off + i < (int)b.size(); i++) s += (char)b[off + i]; return s; }
static bool hasExtAt(const Bytes& header, int offset) { return ascii(header, offset, 4) == SNA_EXT_MAGIC; }
static int extensionOffset(const Bytes& header) { return hasExtAt(header, SNA_EXT_OFFSET) ? SNA_EXT_OFFSET : (hasExtAt(header, SNA_EXT_OLD_OFFSET) ? SNA_EXT_OLD_OFFSET : -1); }

static int snaCpcType(GX4000* emulator) {
    bool plus = emulator->plusHardware;
    bool computer = emulator->plusComputer;
    int ramKiB = emulator->ramKiB ? emulator->ramKiB : (int)(emulator->memory->ram.size() / 1024);
    if (!ramKiB) ramKiB = 64;
    if (plus && computer) return ramKiB > 64 ? 4 : 5;
    if (plus && !computer) return 6;
    if (emulator->model == "CPC 664") return 1;
    if (emulator->model == "CPC 6128" || ramKiB > 64) return 2;
    return 0;
}

static Bytes sliceB(const Bytes& b, int a, int c) { a = std::max(0, a); c = std::min((int)b.size(), c); if (c < a) c = a; return Bytes(b.begin() + a, b.begin() + c); }

static Bytes buildPlusChunk(GX4000* emulator) {
    Bytes p(0x8f8, 0);
    for (int i = 0; i < 0x800; i++) p[i] = (uint8_t)((emulator->memory->asicRam[i * 2] & 15) | ((emulator->memory->asicRam[i * 2 + 1] & 15) << 4));
    { Bytes s = sliceB(emulator->memory->asicRam, 0x2000, 0x2080); for (int i = 0; i < (int)s.size(); i++) p[0x800 + i] = s[i]; }
    for (int i = 0; i < 32; i++) {
        int colour = emulator->asic->palette[i] & 0x0fff;
        p[0x880 + i * 2] = (uint8_t)(colour & 0xff);
        p[0x881 + i * 2] = (uint8_t)((unsigned)colour >> 8 & 0x0f);
    }
    { Bytes s = sliceB(emulator->memory->asicRam, 0x2800, 0x2806); for (int i = 0; i < (int)s.size(); i++) p[0x8c0 + i] = s[i]; }
    p[0x8c6] = (uint8_t)(emulator->gateArray->romConfig & 0xff);
    for (int i = 0; i < 3; i++) {
        DmaChannel& d = emulator->asic->dma[i]; int o = 0x8d0 + i * 4;
        put16(p, o, d.pointer); p[o + 2] = (uint8_t)(d.prescale & 0xff); p[o + 3] = 0;
    }
    p[0x8df] = (uint8_t)(emulator->asic->dmaStatus & 0xff);
    for (int i = 0; i < 3; i++) {
        DmaChannel& d = emulator->asic->dma[i]; int o = 0x8e0 + i * 7;
        put16(p, o, d.loopCount); put16(p, o + 2, d.loopStart); put16(p, o + 4, d.pause);
        p[o + 6] = (uint8_t)d.prescaleCurrent;
    }
    p[0x8f5] = (uint8_t)(emulator->gateArray->romConfig & 0xff);
    p[0x8f6] = (uint8_t)(emulator->asic->locked ? 0 : 1);
    p[0x8f7] = (uint8_t)(emulator->asic->unlockPosition & 0xff);
    Bytes chunk(8 + p.size());
    putText(chunk, 0, "CPC+"); put32(chunk, 4, (unsigned)p.size()); for (int i = 0; i < (int)p.size(); i++) chunk[8 + i] = p[i];
    return chunk;
}

static Bytes unrle(const Bytes& data, int size) {
    Bytes out(size, 0); int p = 0;
    for (int i = 0; i < (int)data.size() && p < size;) {
        if (data[i] == 0xe5 && (i + 1 < (int)data.size() ? data[i + 1] : 0) != 0) {
            if (i + 2 >= (int)data.size()) break;   // a run cut off at the end
            int n = data[i + 1], v = data[i + 2];
            for (int k = p; k < std::min(size, p + n); k++) out[k] = (uint8_t)v;
            p += n; i += 3;
        } else {
            out[p++] = data[i++];
            if (out[p - 1] == 0xe5 && i < (int)data.size() && data[i] == 0) i++;
        }
    }
    return out;
}
static void scanSnaChunks(const Bytes& bytes, int pos, const std::function<void(const std::string&, const Bytes&, int)>& onChunk) {
    while (pos + 8 <= (int)bytes.size()) {
        std::string id = ascii(bytes, pos, 4);
        const unsigned raw = read32(bytes, pos + 4);
        // Unsigned: a size of 2 GB or more must not turn negative and walk backwards.
        if (raw > (unsigned)((int)bytes.size() - pos - 8)) break;
        const int size = (int)raw;
        Bytes payload = sliceB(bytes, pos + 8, pos + 8 + size);
        onChunk(id, payload, size);
        pos += 8 + size;
    }
}
static int memoryChunkIndex(const std::string& id) {
    if (std::regex_match(id, std::regex("^MEM[0-9A-F]$", std::regex::icase))) { try { return std::stoi(id.substr(3, 1), nullptr, 16); } catch (...) { return -1; } }
    if (std::regex_match(id, std::regex("^MX[0-9A-F]{2}$", std::regex::icase))) { try { return std::stoi(id.substr(2), nullptr, 16); } catch (...) { return -1; } }
    if (id.size() >= 4 && id.substr(0, 3) == "MEM" && (unsigned char)id[3] >= 65) return (unsigned char)id[3] - 55;
    return -1;
}
static int romChunkBank(const std::string& id) {
    if (id == "LOWR") return 256;
    return std::regex_match(id, std::regex("^RM[0-9A-F]{2}$", std::regex::icase)) ? (int)std::stol(id.substr(2), nullptr, 16) : -1;
}
static Bytes decodeRomPayload(const Bytes& payload) { return (int)payload.size() == 0x4000 ? payload : unrle(payload, 0x4000); }
static Bytes buildChunk(const std::string& id, const Bytes& data) {
    Bytes chunk(8 + data.size());
    std::string tag = (id.empty() ? "????" : id).substr(0, 4); while (tag.size() < 4) tag += ' ';
    putText(chunk, 0, tag); put32(chunk, 4, (unsigned)data.size());
    for (int i = 0; i < (int)data.size(); i++) chunk[8 + i] = data[i];
    return chunk;
}
static DebugMetadata makeDebugMetadata() { DebugMetadata m; m.remu = parseRemu(""); return m; }
static void collectDebugChunk(DebugMetadata& metadata, const std::string& id, const Bytes& payload) {
    Bytes bytes = payload;
    int romBank = romChunkBank(id);
    if (romBank >= 0) {
        Bytes decoded = decodeRomPayload(bytes);
        if (romBank == 256) { metadata.roms.lower = decoded; metadata.roms.hasLower = true; }
        else metadata.roms.upper[romBank] = decoded;
        metadata.preservedChunks.push_back({ id, bytes });
        return;
    }
    if (id == "REMU") {
        std::string decoded(bytes.begin(), bytes.end());
        while (!decoded.empty() && decoded.back() == '\0') decoded.pop_back();
        metadata.remu = parseRemu(decoded);
        return;
    }
    if (id != "CPC+" && memoryChunkIndex(id) < 0) metadata.preservedChunks.push_back({ id, bytes });
}
static int targetRamSizeKiB(int kib) {
    int value = std::max(64, kib);
    if (value <= 64) return 64;
    if (value <= 128) return 128;
    if (value <= 256) return 256;
    if (value <= 320) return 320;
    if (value <= 512) return 512;
    if (value <= 576) return 576;
    // a segmented board: 64K + whole 512K segments, up to eight
    return std::min(4160, 64 + (value - 64 + 511) / 512 * 512);
}

Snapshot parseSna(const Bytes& bytes) {
    if (bytes.size() < 256 || ascii(bytes, 0, 8) != "MV - SNA") throw std::runtime_error("Not an Amstrad CPC SNA file");
    int version = bytes[0x10];
    if (version != 1 && version != 2 && version != 3) throw std::runtime_error("Unsupported SNA version " + std::to_string(version));
    Bytes header = sliceB(bytes, 0, 256);
    int declaredKiB = u16(header, 0x6b);
    Bytes ram;
    int pos = 256;
    Bytes plus; bool hasPlus = false;
    DebugMetadata debugMetadata = makeDebugMetadata();
    auto handleChunk = [&](const std::string& id, const Bytes& payload, int) {
        // The chunk is 0x8f8 bytes; a shorter one is not one (applySna reads up to 0x8f7).
        if (id == "CPC+" && payload.size() >= 0x8f8 && payload.size() < 0x1000) { plus = payload; hasPlus = true; }
        collectDebugChunk(debugMetadata, id, payload);
    };
    if (declaredKiB) {
        int rawKiB = declaredKiB < 64 ? 64 : declaredKiB;
        if (rawKiB > MAX_SNA_RAM_KIB || (rawKiB & 15)) throw std::runtime_error("Invalid SNA RAM size " + std::to_string(declaredKiB) + " KiB");
        ram.assign((size_t)rawKiB * 1024, 0);
        int size = std::min((int)ram.size(), (int)bytes.size() - pos);
        for (int i = 0; i < size; i++) ram[i] = bytes[pos + i];
        pos += rawKiB * 1024;
        scanSnaChunks(bytes, pos, handleChunk);
    } else {
        ram.clear();
        scanSnaChunks(bytes, pos, [&](const std::string& id, const Bytes& payload, int size) {
            int block = memoryChunkIndex(id);
            if (block >= 0) {
                Bytes decoded = size == 65536 ? payload : unrle(payload, 65536);
                if ((block + 1) * 65536 > MAX_SNA_RAM_KIB * 1024) return;
                if ((block + 1) * 65536 > (int)ram.size()) ram.resize((size_t)(block + 1) * 65536, 0);
                for (int i = 0; i < (int)decoded.size() && block * 65536 + i < (int)ram.size(); i++) ram[block * 65536 + i] = decoded[i];
            } else handleChunk(id, payload, size);
        });
        if (ram.empty()) ram.assign(64 * 1024, 0);
    }
    Snapshot s; s.header = header; s.ram = ram; s.plus = plus; s.hasPlus = hasPlus; s.debugMetadata = debugMetadata; s.version = version; s.cpcType = header[0x6d];
    return s;
}

static Bytes rle(const Bytes& data) {
    Bytes out;
    for (int i = 0; i < (int)data.size();) {
        int value = data[i]; int n = 1;
        while (i + n < (int)data.size() && data[i + n] == value && n < 255) n++;
        if (n > (value == 0xe5 ? 1 : 2)) { out.push_back(0xe5); out.push_back((uint8_t)n); out.push_back((uint8_t)value); }
        else for (int j = 0; j < n; j++) { out.push_back((uint8_t)value); if (value == 0xe5) out.push_back(0); }
        i += n;
    }
    return out;
}
static Bytes buildMemChunk(int block, const Bytes& data, bool compressed) {
    Bytes packed = compressed ? rle(data) : data;
    const Bytes& payload = packed.size() < 65536 ? packed : data;
    char idbuf[8];
    std::string id;
    if (block < 16) { std::snprintf(idbuf, sizeof(idbuf), "MEM%X", block); id = idbuf; }
    else { std::snprintf(idbuf, sizeof(idbuf), "MX%02X", block); id = idbuf; }
    return buildChunk(id, payload);
}

static std::vector<RemuBreakpoint> snapshotBreakpointRecords(GX4000* emulator) {
    std::vector<RemuBreakpoint> records;
    if (emulator->breakpoints) {
        for (int address : *emulator->breakpoints) {
            std::string source = "user";
            auto it = emulator->breakpointSources.find(address); if (it != emulator->breakpointSources.end()) source = it->second;
            MemoryIdentity identity = emulator->memory->debugIdentity(address);
            RemuBreakpoint b; b.address = address; b.source = source; b.kind = identity.kind.empty() ? "logical" : identity.kind; b.bank = identity.bank;
            records.push_back(b);
        }
    }
    return records;
}
static std::vector<Bytes> buildDebugChunks(GX4000* emulator) {
    DebugMetadata metadata = emulator->snapshotDebugMetadata ? *emulator->snapshotDebugMetadata : makeDebugMetadata();
    std::vector<Bytes> chunks;
    std::unordered_set<std::string> ids;
    for (const PreservedChunk& item : metadata.preservedChunks) {
        if (item.id.empty() || item.id == "REMU" || item.id == "CPC+" || memoryChunkIndex(item.id) >= 0) continue;
        chunks.push_back(buildChunk(item.id, item.payload)); ids.insert(item.id);
    }
    if (!emulator->memory->snapshotLowerRom.empty() && !ids.count("LOWR")) chunks.push_back(buildChunk("LOWR", emulator->memory->snapshotLowerRom));
    for (int slot = 0; slot < (int)emulator->memory->snapshotUpperRoms.size(); slot++) {
        const Bytes& rom = emulator->memory->snapshotUpperRoms[slot];
        char idbuf[8]; std::snprintf(idbuf, sizeof(idbuf), "RM%02X", slot); std::string id = idbuf;
        if (!rom.empty() && !ids.count(id)) chunks.push_back(buildChunk(id, rom));
    }
    BuildRemuOptions opts;
    if (emulator->debugSymbols) opts.symbols = emulator->debugSymbols->entries;
    opts.breakpoints = snapshotBreakpointRecords(emulator);
    opts.watchpoints = emulator->watchpoints;
    std::string remuText = buildRemuText(metadata, opts);
    if (!remuText.empty()) chunks.push_back(buildChunk("REMU", Bytes(remuText.begin(), remuText.end())));
    return chunks;
}

Bytes createSna(GX4000* emulator, bool compressed) {
    Bytes h(256, 0);
    putText(h, 0, "MV - SNA");
    h[0x10] = 3;
    Z80* cpu = emulator->cpu;
    h[0x11] = (uint8_t)cpu->f; h[0x12] = (uint8_t)cpu->a;
    h[0x13] = (uint8_t)cpu->c; h[0x14] = (uint8_t)cpu->b;
    h[0x15] = (uint8_t)cpu->e; h[0x16] = (uint8_t)cpu->d;
    h[0x17] = (uint8_t)cpu->l; h[0x18] = (uint8_t)cpu->h;
    h[0x19] = (uint8_t)cpu->r; h[0x1a] = (uint8_t)cpu->i;
    h[0x1b] = (uint8_t)(cpu->iff1 ? 1 : 0); h[0x1c] = (uint8_t)(cpu->iff2 ? 1 : 0);
    put16(h, 0x1d, cpu->ix); put16(h, 0x1f, cpu->iy);
    put16(h, 0x21, cpu->sp); put16(h, 0x23, cpu->pc);
    h[0x25] = (uint8_t)(cpu->im & 3);
    h[0x26] = (uint8_t)cpu->fp; h[0x27] = (uint8_t)cpu->ap;
    h[0x28] = (uint8_t)cpu->cp; h[0x29] = (uint8_t)cpu->bp;
    h[0x2a] = (uint8_t)cpu->ep; h[0x2b] = (uint8_t)cpu->dp;
    h[0x2c] = (uint8_t)cpu->lp; h[0x2d] = (uint8_t)cpu->hp;
    h[0x2e] = (uint8_t)(emulator->gateArray->paletteIndex & 0x1f);
    for (int i = 0; i < 17; i++) h[0x2f + i] = emulator->gateArray->gaPalette[i];
    h[0x40] = (uint8_t)((emulator->gateArray->romConfig & 0x1f) | 0x80);
    h[0x41] = (uint8_t)(emulator->memory->ramConfig);
    h[0x42] = (uint8_t)(emulator->crtc->selected & 0x1f);
    for (int i = 0; i < 18; i++) h[0x43 + i] = emulator->crtc->registers[i];
    h[0x55] = (uint8_t)emulator->memory->upperRom;
    h[0x56] = (uint8_t)emulator->ppi->portA; h[0x57] = (uint8_t)emulator->ppi->portB;
    h[0x58] = (uint8_t)emulator->ppi->portC; h[0x59] = (uint8_t)(emulator->ppi->control | 0x80);
    h[0x5a] = (uint8_t)emulator->ay->selected;
    for (int i = 0; i < 16; i++) h[0x5b + i] = emulator->ay->registers[i];
    h[0x6d] = (uint8_t)snaCpcType(emulator);
    h[0x9c] = (uint8_t)(emulator->fdc->motor ? 1 : 0);
    h[0x9d] = (uint8_t)emulator->fdc->tracks[0];
    h[0x9e] = (uint8_t)emulator->fdc->tracks[1];
    CRTC6845* crtc = emulator->crtc;
    // CRTC 1-B (our type 5) is a UM6845R, so it saves as a 1 -- the snapshot format
    // has no room for the ACCC's 1-A/1-B distinction (ACCC §11.6).
    h[0xa4] = (uint8_t)(crtc->type == 5 ? 1 : (crtc->type >= 0 && crtc->type <= 4 ? crtc->type : 0));
    h[0xa9] = (uint8_t)(crtc->horizontal & 0xff);
    h[0xaa] = 0;
    h[0xab] = (uint8_t)(crtc->vertical & 0x7f);
    h[0xac] = (uint8_t)(crtc->raster & 0x1f);
    h[0xad] = (uint8_t)(crtc->verticalAdjust & 0x1f);
    h[0xae] = (uint8_t)(crtc->hsyncCounter & 0x0f);
    h[0xaf] = (uint8_t)(crtc->vsyncCounter & 0x0f);
    h[0xb0] = (uint8_t)((crtc->vsync ? 1 : 0) | (crtc->hsync ? 2 : 0) | (crtc->hDisplay ? 4 : 0) | (crtc->vDisplay ? 8 : 0)
        | (crtc->horizontalTotalMatch ? 16 : 0)
        | ((crtc->type == 1 ? crtc->r4Match : crtc->vertical >= crtc->registers[4]) ? 32 : 0)
        | ((crtc->type == 1 ? crtc->r9Match : crtc->raster >= crtc->registers[9]) ? 64 : 0)
        | (crtc->verticalAdjustActive ? 128 : 0));
    h[0xb1] = (uint8_t)(crtc->verticalAdjustActive ? 1 : 0);
    h[0xb2] = (uint8_t)(emulator->gateArray->interruptSyncCount & 0xff);
    h[0xb3] = (uint8_t)(emulator->gateArray->interruptCounter & 0xff);
    h[0xb4] = (uint8_t)((cpu->pendingInterrupt != -1 || (emulator->asic->irqStatus & 0xf0)) ? 1 : 0);
    h[0xb5] = (uint8_t)(emulator->asic->irqStatus & 0xf0);
    h[0xb6] = (uint8_t)(emulator->plusHardware ? 0 : 1);
    h[0xb7] = (uint8_t)(emulator->plusHardware ? 1 : 0);
    putText(h, SNA_EXT_OFFSET, SNA_EXT_MAGIC);
    put16(h, 0xd4, crtc->scanlineInFrame);
    h[0xd6] = (uint8_t)(crtc->interlaceField & 1);
    h[0xd7] = (uint8_t)(crtc->charClockRemainder & 3);
    h[0xd8] = (uint8_t)((crtc->r7Match ? 1 : 0) | (crtc->rasterMatchForced ? 2 : 0) | (crtc->vblank ? 4 : 0)
        | (crtc->hDisplay ? 0 : 8) | (crtc->vDisplay ? 0 : 16)
        | (crtc->r4Match ? 32 : 0) | (crtc->r9Match ? 64 : 0)
        | (crtc->verticalAdjustActive ? 128 : 0));
    put16(h, 0xd9, crtc->rowAddress);
    put16(h, 0xdb, crtc->nextRowAddress);
    put16(h, 0xdd, crtc->maRow);
    put16(h, 0xdf, crtc->requestedAddress ? crtc->requestedAddress : crtc->screenAddress());
    put16(h, 0xe1, crtc->frameAddress ? crtc->frameAddress : crtc->screenAddress());
    h[0xe3] = (uint8_t)(crtc->vlc & 7);
    h[0xe4] = (uint8_t)(emulator->memory->ram4MbSegment & 7);
    int ramKiB = (int)(emulator->memory->ram.size() / 1024);
    put16(h, 0x6b, compressed ? 0 : ramKiB);

    std::vector<Bytes> parts;
    if (compressed) {
        parts.push_back(h);
        const Bytes& ram = emulator->memory->ram;
        for (int offset = 0, block = 0; offset < (int)ram.size(); offset += 65536, block += 1)
            parts.push_back(buildMemChunk(block, sliceB(ram, offset, offset + 65536), true));
        if (emulator->plusHardware) parts.push_back(buildPlusChunk(emulator));
        for (auto& c : buildDebugChunks(emulator)) parts.push_back(c);
    } else {
        parts.push_back(h);
        parts.push_back(emulator->memory->ram);
        if (emulator->plusHardware) parts.push_back(buildPlusChunk(emulator));
        for (auto& c : buildDebugChunks(emulator)) parts.push_back(c);
    }
    int total = 0; for (auto& p : parts) total += (int)p.size();
    Bytes result(total); int at = 0;
    for (auto& p : parts) { for (int i = 0; i < (int)p.size(); i++) result[at + i] = p[i]; at += (int)p.size(); }
    return result;
}

void applySna(GX4000* emulator, const Snapshot& snapshot) {
    const Bytes& h = snapshot.header;
    const Bytes& ram = snapshot.ram;
    const Bytes& plus = snapshot.plus; bool hasPlusChunk = snapshot.hasPlus;
    int extOffset = extensionOffset(h);
    bool hasExt = extOffset >= 0;
    bool legacyExt = extOffset == SNA_EXT_OLD_OFFSET;
    if (ram.size() != emulator->memory->ram.size()) {
        int ramKiB = std::max(64, (int)(ram.size() / 1024));
        emulator->memory->setRamSize(targetRamSizeKiB(ramKiB));
    }
    for (int i = 0; i < (int)emulator->memory->ram.size() && i < (int)ram.size(); i++) emulator->memory->ram[i] = ram[i];

    emulator->memory->clearSnapshotRoms();
    DebugMetadata debugMetadata = snapshot.debugMetadata;
    if (debugMetadata.roms.hasLower) emulator->memory->setSnapshotLowerRom(debugMetadata.roms.lower);
    for (auto& kv : debugMetadata.roms.upper) emulator->memory->setSnapshotUpperRom(kv.first, kv.second);
    emulator->snapshotDebugMetadata = std::make_shared<DebugMetadata>(debugMetadata);

    Z80* cpu = emulator->cpu;
    cpu->f = h[0x11]; cpu->a = h[0x12];
    cpu->c = h[0x13]; cpu->b = h[0x14];
    cpu->e = h[0x15]; cpu->d = h[0x16];
    cpu->l = h[0x17]; cpu->h = h[0x18];
    cpu->r = h[0x19]; cpu->i = h[0x1a];
    cpu->iff1 = h[0x1b] != 0; cpu->iff2 = h[0x1c] != 0;
    cpu->ix = u16(h, 0x1d); cpu->iy = u16(h, 0x1f);
    cpu->sp = u16(h, 0x21); cpu->pc = u16(h, 0x23);
    cpu->im = h[0x25] & 3;
    cpu->fp = h[0x26]; cpu->ap = h[0x27];
    cpu->cp = h[0x28]; cpu->bp = h[0x29];
    cpu->ep = h[0x2a]; cpu->dp = h[0x2b];
    cpu->lp = h[0x2c]; cpu->hp = h[0x2d];

    int cpcType = h[0x6d];
    emulator->plusHardware = cpcType >= 4 || hasPlusChunk;
    emulator->gateArray->setPlusHardware(emulator->plusHardware);
    emulator->plusComputer = cpcType == 4 || cpcType == 5;
    emulator->ramKiB = (int)(emulator->memory->ram.size() / 1024);
    emulator->crtc->setType(emulator->plusHardware ? 3 : (h[0xa4] == 1 ? 1 : 0));
    emulator->ppi->setPlusMode(emulator->plusHardware);
    for (int i = 0; i < 18; i++) emulator->crtc->registers[i] = h[0x43 + i];
    emulator->crtc->selected = h[0x42] & 0x1f;
    for (int i = 0; i < 17; i++) emulator->gateArray->gaPalette[i] = h[0x2f + i];
    emulator->gateArray->paletteIndex = h[0x2e] & 0x1f;
    emulator->gateArray->romConfig = h[0x40] & 0xff;
    emulator->gateArray->newMode = emulator->gateArray->mode = emulator->gateArray->romConfig & 3;
    emulator->memory->setGateArrayConfig(h[0x40] & 0x1f);
    int ram4MbSegment = hasExt ? (legacyExt ? (h[0xbf] & 7) : (h[0xe4] & 7)) : 0;
    emulator->memory->setRamConfig(h[0x41] & 0x3f, hasExt ? ((0x7f - ram4MbSegment) << 8) : 0x7f00);
    if (!emulator->plusHardware) emulator->memory->setUpperRomSelect(h[0x55]);
    emulator->ppi->portA = h[0x56]; emulator->ppi->portB = h[0x57];
    emulator->ppi->portC = h[0x58]; emulator->ppi->setMode(h[0x59]);
    emulator->ay->selected = h[0x5a] & 15;
    for (int i = 0; i < 16; i++) emulator->ay->registers[i] = h[0x5b + i];
    if (snapshot.version >= 3) {
        UPD765A::SnapshotState st; st.motor = h[0x9c] != 0; st.tracks = { h[0x9d], h[0x9e], 0, 0 };
        emulator->fdc->restoreSnapshotState(st);
    }

    if (hasPlusChunk && emulator->plusHardware) {
        for (int i = 0; i < 0x800 && i < (int)plus.size(); i++) {
            int value = plus[i];
            emulator->asic->writeAsicRam(i * 2, value & 15);
            emulator->asic->writeAsicRam(i * 2 + 1, (unsigned)value >> 4 & 15);
        }
        for (int i = 0; i < 0x80 && 0x800 + i < (int)plus.size(); i++) emulator->asic->writeAsicRam(0x2000 + i, plus[0x800 + i]);
        for (int i = 0; i < 32 && 0x881 + i * 2 < (int)plus.size(); i++) emulator->asic->setPlusPalette(i, plus[0x880 + i * 2] | (plus[0x881 + i * 2] & 15) << 8);
        emulator->asic->locked = plus[0x8f6] != 1;
        for (int i = 0; i < 6 && 0x8c0 + i < (int)plus.size(); i++) emulator->asic->writeAsicRam(0x2800 + i, plus[0x8c0 + i]);
        for (int i = 0; i < 3 && 0x8d2 + i * 4 < (int)plus.size(); i++) {
            int o = 0x8d0 + i * 4; DmaChannel& d = emulator->asic->dma[i];
            d.pointer = u16(plus, o); d.prescale = plus[o + 2] & 0xff; d.prescaleCurrent = 0;
        }
        int dcsr = 0x8df < (int)plus.size() ? plus[0x8df] : emulator->asic->dmaStatus;
        emulator->asic->dmaStatus = dcsr & 0xff;
        emulator->asic->irqStatus = (snapshot.version >= 3 ? h[0xb5] : 0) & 0xf0;
        if (!emulator->asic->irqStatus && h[0xb4]) emulator->asic->irqStatus = (dcsr & 0x70) ? (dcsr & 0x70) : ((dcsr & 0x80) ? 0x80 : 0);
        for (int index = 0; index < 3; index++) {
            DmaChannel& d = emulator->asic->dma[index];
            d.active = !!(emulator->asic->dmaStatus & (1 << index));
            d.irq = !!(emulator->asic->irqStatus & (0x40 >> index));
        }
        emulator->asic->rasterInterruptPending = !!(emulator->asic->irqStatus & 0x80);
        emulator->asic->updateDmaStatusRam();
        for (int i = 0; i < 3 && 0x8e6 + i * 7 < (int)plus.size(); i++) {
            int o = 0x8e0 + i * 7; DmaChannel& d = emulator->asic->dma[i];
            d.loopCount = u16(plus, o) & 0x0fff;
            d.loopStart = u16(plus, o + 2);
            d.pause = u16(plus, o + 4) & 0x0fff;
            d.prescaleCurrent = plus[o + 6] & 0xff;
            d.instruction = 0; d.instructionFetched = false; d.irqRequest = false;
        }
        int gaRegister = ((0x8f5 < (int)plus.size() ? plus[0x8f5] : 0) ? plus[0x8f5] : (hasExt ? plus[0x8c6] : 0)) & 0xff;
        if (!gaRegister) gaRegister = h[0x40] & 0xff;
        emulator->gateArray->romConfig = gaRegister;
        emulator->gateArray->newMode = emulator->gateArray->mode = gaRegister & 3;
        emulator->asic->locked = plus[0x8f6] == 0;
        emulator->asic->unlockPosition = 0x8f7 < (int)plus.size() ? plus[0x8f7] : emulator->asic->unlockPosition;
    }

    if (!hasExt && snapshot.version >= 3) {
        emulator->gateArray->interruptSyncCount = h[0xb2] & 0xff;
        emulator->gateArray->interruptCounter = h[0xb3] & 0xff;
        emulator->crtc->hDisplay = !!(h[0xb0] & 4);
        emulator->crtc->hDisplayInternal = emulator->crtc->hDisplay; emulator->crtc->skewHistory = 0;
        emulator->crtc->vDisplay = !!(h[0xb0] & 8);
        if (h[0xb4]) emulator->cpu->pendingInterrupt = 0xff;
    }
    if (hasExt) {
        CRTC6845* crtc = emulator->crtc;
        crtc->setType(h[0xa4] <= 4 ? h[0xa4] : crtc->type);
        crtc->horizontal = h[0xa9] & 0xff;
        crtc->vertical = h[0xab] & 0x7f;
        crtc->raster = h[0xac] & 0x1f;
        crtc->verticalAdjust = h[0xad] & 0x1f;
        crtc->hsyncCounter = h[0xae] & 0x0f;
        crtc->vsyncCounter = h[0xaf] & 0x0f;
        crtc->vsync = !!(h[0xb0] & 1);
        crtc->hsync = !!(h[0xb0] & 2);
        crtc->horizontalTotalMatch = !!(h[0xb0] & 16);
        crtc->r4Match = !!(h[0xb0] & 32);
        crtc->r9Match = !!(h[0xb0] & 64);
        crtc->verticalAdjustActive = !!(h[0xb1] & 1) || !!(h[0xb0] & 128);
        emulator->gateArray->interruptSyncCount = h[0xb2];
        emulator->gateArray->interruptCounter = h[0xb3];
        if (legacyExt) {
            crtc->scanlineInFrame = u16(h, 0xa5) & 0x3ff;
            crtc->interlaceField = h[0xa7] & 1;
            crtc->charClockRemainder = h[0xa8] & 3;
            crtc->r7Match = !!(h[0xaa] & 1);
            crtc->rasterMatchForced = !!(h[0xaa] & 2);
            crtc->vblank = !!(h[0xaa] & 4);
            crtc->hDisplay = !(h[0xb0] & 4);
            crtc->hDisplayInternal = crtc->hDisplay; crtc->skewHistory = 0;
            crtc->vDisplay = !(h[0xb0] & 8);
            crtc->rowAddress = u16(h, 0xb4) & 0x3fff;
            crtc->nextRowAddress = u16(h, 0xb6) & 0x3fff;
            crtc->maRow = u16(h, 0xb8) & 0x3fff;
            crtc->requestedAddress = u16(h, 0xba) & 0x3fff;
            crtc->frameAddress = u16(h, 0xbc) & 0x3fff;
            crtc->vlc = h[0xbe] & 7;
        } else {
            crtc->scanlineInFrame = u16(h, 0xd4) & 0x3ff;
            crtc->interlaceField = h[0xd6] & 1;
            crtc->charClockRemainder = h[0xd7] & 3;
            crtc->r7Match = !!(h[0xd8] & 1);
            crtc->rasterMatchForced = !!(h[0xd8] & 2);
            crtc->vblank = !!(h[0xd8] & 4);
            crtc->hDisplay = !(h[0xd8] & 8);
            crtc->hDisplayInternal = crtc->hDisplay; crtc->skewHistory = 0;
            crtc->vDisplay = !(h[0xd8] & 16);
            crtc->r4Match = !!(h[0xd8] & 32);
            crtc->r9Match = !!(h[0xd8] & 64);
            crtc->verticalAdjustActive = !!(h[0xd8] & 128) || !!(h[0xb1] & 1) || !!(h[0xb0] & 128);
            crtc->rowAddress = u16(h, 0xd9) & 0x3fff;
            crtc->nextRowAddress = u16(h, 0xdb) & 0x3fff;
            crtc->maRow = u16(h, 0xdd) & 0x3fff;
            crtc->requestedAddress = u16(h, 0xdf) & 0x3fff;
            crtc->frameAddress = u16(h, 0xe1) & 0x3fff;
            crtc->vlc = h[0xe3] & 7;
        }
        emulator->cpu->pendingInterrupt = h[0xb4] ? 0xff : -1;
    }

    emulator->memory->remap();
    if (!hasExt) {
        emulator->crtc->horizontalTotalMatch = snapshot.version >= 3 ? emulator->crtc->horizontal >= emulator->crtc->registers[0] : false;
        emulator->crtc->requestedAddress = emulator->crtc->screenAddress();
        emulator->crtc->frameAddress = emulator->crtc->requestedAddress;
        emulator->crtc->rowAddress = emulator->crtc->frameAddress;
        emulator->crtc->nextRowAddress = emulator->crtc->rowAddress;
        emulator->crtc->maRow = emulator->crtc->rowAddress;
        emulator->crtc->vlc = emulator->crtc->videoRaster();
    }
    emulator->rasterCapture.clear();
    emulator->rasterFrame.clear();
    emulator->timingInstructionActive = false;
}

} // namespace cpcse
