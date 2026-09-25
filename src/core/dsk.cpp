// CPCSyntaxError — CPC disk-image codec.
#include "dsk.h"
#include <stdexcept>

namespace cpcse {

static std::string text(const Bytes& bytes, int offset, int length) {
    std::string s;
    for (int k = 0; k < length && offset + k < (int)bytes.size(); k++) s.push_back((char)bytes[offset + k]);
    return s;
}
static int word(const Bytes& bytes, int offset) { return bytes[offset] | bytes[offset + 1] << 8; }
static bool startsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }
static Bytes sliceBytes(const Bytes& bytes, int start, int end) {
    if (start < 0) start = 0; if (end > (int)bytes.size()) end = (int)bytes.size();
    if (end < start) end = start;
    return Bytes(bytes.begin() + start, bytes.begin() + end);
}

std::shared_ptr<Disk> parseDsk(const Bytes& input) {
    const Bytes& bytes = input;
    std::string signature = text(bytes, 0, std::min(34, (int)bytes.size()));
    bool extended = startsWith(signature, "EXTENDED CPC DSK File") || startsWith(signature, "EXTENDED Disk-File");
    bool standard = startsWith(signature, "MV - CPCEMU") || startsWith(signature, "MV - CPC Disk-File");
    if (!extended && !standard) throw std::runtime_error("Invalid CPC DSK image.");
    int tracks = bytes[0x30], sides = bytes[0x31];
    if (!tracks || !sides || sides > 2) throw std::runtime_error("Invalid DSK geometry.");
    auto disk = std::make_shared<Disk>();
    disk->creator = text(bytes, 0x22, 14);
    // .replace(/\0+$/, '').trim()
    while (!disk->creator.empty() && disk->creator.back() == '\0') disk->creator.pop_back();
    { size_t a = 0, b = disk->creator.size();
      while (a < b && std::isspace((unsigned char)disk->creator[a])) a++;
      while (b > a && std::isspace((unsigned char)disk->creator[b - 1])) b--;
      disk->creator = disk->creator.substr(a, b - a); }
    disk->tracks = tracks; disk->sides = sides;
    disk->trackData.assign(tracks, {});
    int standardTrackSize = word(bytes, 0x32); int offset = 0x100;
    for (int cylinder = 0; cylinder < tracks; cylinder += 1) {
        disk->trackData[cylinder].assign(sides, nullptr);
        for (int side = 0; side < sides; side += 1) {
            int tableIndex = cylinder * sides + side;
            int trackSize = extended ? ((0x34 + tableIndex < (int)bytes.size() ? bytes[0x34 + tableIndex] : 0) << 8) : standardTrackSize;
            if (!trackSize) { disk->trackData[cylinder][side] = nullptr; continue; }
            if (offset + trackSize > (int)bytes.size() || text(bytes, offset, 12) != "Track-Info\r\n")
                throw std::runtime_error("Invalid DSK track " + std::to_string(cylinder) + "/" + std::to_string(side) + ".");
            int sectorCount = bytes[offset + 0x15];
            auto track = std::make_shared<Track>();
            track->cylinder = bytes[offset + 0x12]; track->side = bytes[offset + 0x13];
            int dataOffset = offset + (sectorCount > 29 ? 0x200 : 0x100);
            for (int index = 0; index < sectorCount; index += 1) {
                int info = offset + 0x18 + index * 8;
                int n = bytes[info + 3] & 0xff, sizeCode = std::min(7, n & 7), nominal = 128 << sizeCode;
                int stored = extended ? word(bytes, info + 6) : 0;
                int maxTrackBytes = std::max(0, offset + trackSize - dataOffset);
                int length = std::min(stored ? stored : nominal, maxTrackBytes);
                int copies = stored && stored % nominal == 0 ? std::max(1, stored / nominal) : 1;
                std::vector<Bytes> data;
                for (int copy = 0; copy < copies; copy += 1)
                    data.push_back(sliceBytes(bytes, dataOffset + copy * nominal, dataOffset + std::min(length, (copy + 1) * nominal)));
                Sector sector;
                sector.c = bytes[info]; sector.h = bytes[info + 1]; sector.r = bytes[info + 2]; sector.n = n;
                sector.st1 = bytes[info + 4]; sector.st2 = bytes[info + 5]; sector.data = data; sector.weakIndex = 0;
                track->sectors.push_back(sector);
                dataOffset += length;
            }
            disk->trackData[cylinder][side] = track; offset += trackSize;
        }
    }
    return disk;
}

Bytes serializeDsk(const Disk& disk) {
    int tracks = disk.tracks, sides = disk.sides;
    std::vector<int> trackSizes;
    for (int cylinder = 0; cylinder < tracks; cylinder += 1) {
        for (int side = 0; side < sides; side += 1) {
            std::shared_ptr<Track> track = (cylinder < (int)disk.trackData.size() && side < (int)disk.trackData[cylinder].size()) ? disk.trackData[cylinder][side] : nullptr;
            if (!track) { trackSizes.push_back(0); continue; }
            int sectorCount = (int)track->sectors.size();
            int dataBytes = 0;
            for (const auto& sector : track->sectors) dataBytes += sector.data.empty() ? 0 : (int)sector.data[0].size();
            int info = sectorCount > 29 ? 0x200 : 0x100;
            trackSizes.push_back((int)std::ceil((info + dataBytes) / 256.0) * 256);
        }
    }
    int total = 0x100; for (int x : trackSizes) total += x;
    Bytes out(total, 0);
    auto putText = [&](int offset, const std::string& value) { for (int i = 0; i < (int)value.size(); i += 1) out[offset + i] = (uint8_t)value[i]; };
    putText(0x00, "EXTENDED CPC DSK File\r\n");
    std::string creator = (disk.creator.empty() ? std::string("CPCSE") : disk.creator).substr(0, 13);
    putText(0x22, creator);
    out[0x30] = (uint8_t)(tracks & 0xff);
    out[0x31] = (uint8_t)(sides & 0xff);
    for (int i = 0; i < (int)trackSizes.size(); i += 1) out[0x34 + i] = (uint8_t)((trackSizes[i] / 256) & 0xff);

    int offset = 0x100;
    for (int cylinder = 0; cylinder < tracks; cylinder += 1) {
        for (int side = 0; side < sides; side += 1) {
            std::shared_ptr<Track> track = (cylinder < (int)disk.trackData.size() && side < (int)disk.trackData[cylinder].size()) ? disk.trackData[cylinder][side] : nullptr;
            int trackSize = trackSizes[cylinder * sides + side];
            if (!track || !trackSize) continue;
            int sectorCount = (int)track->sectors.size();
            int info = sectorCount > 29 ? 0x200 : 0x100;
            putText(offset, "Track-Info\r\n");
            out[offset + 0x0C] = 0x4a; out[offset + 0x0D] = 0x41; out[offset + 0x0E] = 0x4d; out[offset + 0x0F] = 0x53;
            out[offset + 0x10] = (uint8_t)(cylinder & 0xff);
            out[offset + 0x11] = 0;
            out[offset + 0x12] = (uint8_t)(track->cylinder & 0xff);
            out[offset + 0x13] = (uint8_t)(track->side & 0xff);
            out[offset + 0x14] = (uint8_t)((track->sectors.empty() ? 2 : track->sectors[0].n) & 7);
            out[offset + 0x15] = (uint8_t)(sectorCount & 0xff);
            out[offset + 0x16] = 0x4e;
            out[offset + 0x17] = 0xe5;
            for (int index = 0; index < sectorCount; index += 1) {
                const Sector& sector = track->sectors[index];
                int infoOffset = offset + 0x18 + index * 8;
                int nominal = 128 << std::min(7, (sector.n & 7));
                int stored = sector.data.empty() ? nominal : (int)sector.data[0].size();
                out[infoOffset] = (uint8_t)(sector.c & 0xff);
                out[infoOffset + 1] = (uint8_t)(sector.h & 0xff);
                out[infoOffset + 2] = (uint8_t)(sector.r & 0xff);
                out[infoOffset + 3] = (uint8_t)(sector.n & 0xff);
                out[infoOffset + 4] = (uint8_t)(sector.st1 & 0xff);
                out[infoOffset + 5] = (uint8_t)(sector.st2 & 0xff);
                out[infoOffset + 6] = (uint8_t)(stored & 0xff);
                out[infoOffset + 7] = (uint8_t)(((unsigned)stored >> 8) & 0xff);
            }
            int dataOffset = offset + info;
            for (const auto& sector : track->sectors) {
                const Bytes emptyBuf;
                const Bytes& data = sector.data.empty() ? emptyBuf : sector.data[0];
                int copyLen = std::min((int)data.size(), offset + trackSize - dataOffset);
                for (int k = 0; k < copyLen; k++) out[dataOffset + k] = data[k];
                dataOffset += (int)data.size();
            }
            for (int a = dataOffset; a < offset + trackSize; a++) if (a >= 0 && a < (int)out.size()) out[a] = 0xe5;
            offset += trackSize;
        }
    }
    return out;
}

} // namespace cpcse
