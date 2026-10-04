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
    if (start < 0) start = 0;
    if (end > (int)bytes.size()) end = (int)bytes.size();
    if (end < start) end = start;
    return Bytes(bytes.begin() + start, bytes.begin() + end);
}
static int nominalSize(int n) { return 128 << std::min(7, n & 7); }

std::shared_ptr<Disk> parseDsk(const Bytes& input) {
    const Bytes& bytes = input;
    std::string signature = text(bytes, 0, std::min(34, (int)bytes.size()));
    bool extended = startsWith(signature, "EXTENDED CPC DSK File") || startsWith(signature, "EXTENDED Disk-File");
    bool standard = startsWith(signature, "MV - CPCEMU") || startsWith(signature, "MV - CPC Disk-File");
    if (!extended && !standard) throw std::runtime_error("Invalid CPC DSK image.");
    if (bytes.size() < 0x100) throw std::runtime_error("Truncated DSK image.");
    int tracks = bytes[0x30], sides = bytes[0x31];
    if (!tracks || !sides || sides > 2) throw std::runtime_error("Invalid DSK geometry.");
    auto disk = std::make_shared<Disk>();
    disk->extended = extended;
    disk->creator = text(bytes, 0x22, 14);
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
            // The track header and its sector table must lie in the file: a short or crafted
            // image must not be read past its end.
            if (offset + 0x18 > (int)bytes.size())
                throw std::runtime_error("Truncated DSK track " + std::to_string(cylinder) + "/" + std::to_string(side) + ".");
            int sectorCount = bytes[offset + 0x15];
            const int tableEnd = std::min(offset + (sectorCount > 29 ? 0x200 : 0x100), (int)bytes.size());
            sectorCount = std::min(sectorCount, std::max(0, (tableEnd - (offset + 0x18)) / 8));
            auto track = std::make_shared<Track>();
            track->cylinder = bytes[offset + 0x10]; track->side = bytes[offset + 0x11];
            track->dataRate = bytes[offset + 0x12]; track->recordingMode = bytes[offset + 0x13];
            track->sizeCode = bytes[offset + 0x14];
            track->gap3 = bytes[offset + 0x16]; track->filler = bytes[offset + 0x17];
            int dataOffset = offset + (sectorCount > 29 ? 0x200 : 0x100);
            for (int index = 0; index < sectorCount; index += 1) {
                int info = offset + 0x18 + index * 8;
                int n = bytes[info + 3] & 0xff, nominal = nominalSize(n);
                int stored = extended ? word(bytes, info + 6) : 0;
                int maxTrackBytes = std::max(0, offset + trackSize - dataOffset);
                int length = std::min(stored ? stored : nominal, maxTrackBytes);
                int copies = stored && stored > nominal && stored % nominal == 0 ? stored / nominal : 1;
                std::vector<Bytes> data;
                for (int copy = 0; copy < copies; copy += 1)
                    data.push_back(sliceBytes(bytes, dataOffset + copy * (copies > 1 ? nominal : length),
                                              dataOffset + (copies > 1 ? (copy + 1) * nominal : length)));
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

static std::shared_ptr<Track> trackAt(const Disk& disk, int cylinder, int side) {
    return cylinder < (int)disk.trackData.size() && side < (int)disk.trackData[cylinder].size() ? disk.trackData[cylinder][side] : nullptr;
}
// What a sector's data takes in the image: every copy, one after the other.
static int storedSize(const Sector& s) {
    int n = 0;
    for (const Bytes& b : s.data) n += (int)b.size();
    return n;
}

bool dskFitsStandard(const Disk& disk, std::string* why) {
    int size = -1;
    for (int cylinder = 0; cylinder < disk.tracks; cylinder++)
        for (int side = 0; side < disk.sides; side++) {
            auto track = trackAt(disk, cylinder, side);
            if (!track) { if (why) *why = "track " + std::to_string(cylinder) + " side " + std::to_string(side) + " is unformatted"; return false; }
            if (track->sectors.size() > 29) { if (why) *why = "a track holds more than 29 sectors"; return false; }
            int bytes = 0x100;
            for (const Sector& s : track->sectors) {
                if (s.data.size() != 1 || (int)s.data[0].size() != nominalSize(s.n)) {
                    if (why) *why = "a sector is weak or not its full size (cylinder " + std::to_string(cylinder) + ")";
                    return false;
                }
                bytes += nominalSize(s.n);
            }
            if (size >= 0 && bytes != size) { if (why) *why = "the tracks are not all one size"; return false; }
            size = bytes;
        }
    return true;
}

Bytes serializeDsk(const Disk& disk, bool standard) {
    const int tracks = disk.tracks, sides = disk.sides;
    if (standard && !dskFitsStandard(disk)) standard = false;
    std::vector<int> trackSizes;
    for (int cylinder = 0; cylinder < tracks; cylinder += 1)
        for (int side = 0; side < sides; side += 1) {
            auto track = trackAt(disk, cylinder, side);
            if (!track) { trackSizes.push_back(0); continue; }
            int dataBytes = 0;
            for (const auto& sector : track->sectors) dataBytes += storedSize(sector);
            const int info = track->sectors.size() > 29 ? 0x200 : 0x100;
            trackSizes.push_back(standard ? info + dataBytes : (info + dataBytes + 255) / 256 * 256);
        }
    // What the format cannot hold is refused, not written as a broken image: an extended
    // image keeps each track's size in one byte of 256s (0xFF00 at most) and has room
    // for 204 of them; a standard one keeps one 16-bit size for all.
    if (!standard && tracks * sides > 0x100 - 0x34)
        throw std::runtime_error("The disc has " + std::to_string(tracks * sides) + " tracks; an extended DSK holds 204 at most.");
    for (size_t i = 0; i < trackSizes.size(); i++)
        if (trackSizes[i] > (standard ? 0xffff : 0xff00))
            throw std::runtime_error("Track " + std::to_string((int)i / sides) + " side " + std::to_string((int)i % sides) + " holds " +
                                     std::to_string(trackSizes[i]) + " bytes, more than a DSK track can.");
    int total = 0x100; for (int x : trackSizes) total += x;
    Bytes out(total, 0);
    auto putText = [&](int offset, const std::string& value) { for (int i = 0; i < (int)value.size(); i += 1) out[offset + i] = (uint8_t)value[i]; };
    putText(0x00, standard ? "MV - CPCEMU Disk-File\r\nDisk-Info\r\n" : "EXTENDED CPC DSK File\r\nDisk-Info\r\n");
    putText(0x22, (disk.creator.empty() ? std::string("CPCSE") : disk.creator).substr(0, 14));
    out[0x30] = (uint8_t)(tracks & 0xff);
    out[0x31] = (uint8_t)(sides & 0xff);
    if (standard) {
        const int size = trackSizes.empty() ? 0 : trackSizes[0];
        out[0x32] = (uint8_t)(size & 0xff); out[0x33] = (uint8_t)(size >> 8);
    } else {
        for (int i = 0; i < (int)trackSizes.size() && 0x34 + i < 0x100; i += 1) out[0x34 + i] = (uint8_t)((trackSizes[i] / 256) & 0xff);
    }
    int offset = 0x100;
    for (int cylinder = 0; cylinder < tracks; cylinder += 1)
        for (int side = 0; side < sides; side += 1) {
            auto track = trackAt(disk, cylinder, side);
            const int trackSize = trackSizes[cylinder * sides + side];
            if (!track || !trackSize) continue;
            const int sectorCount = (int)track->sectors.size();
            const int info = sectorCount > 29 ? 0x200 : 0x100;
            putText(offset, "Track-Info\r\n");
            out[offset + 0x10] = (uint8_t)(track->cylinder & 0xff);
            out[offset + 0x11] = (uint8_t)(track->side & 0xff);
            out[offset + 0x12] = (uint8_t)(track->dataRate & 0xff);
            out[offset + 0x13] = (uint8_t)(track->recordingMode & 0xff);
            out[offset + 0x14] = (uint8_t)(track->sizeCode & 0xff);
            out[offset + 0x15] = (uint8_t)(sectorCount & 0xff);
            out[offset + 0x16] = (uint8_t)(track->gap3 & 0xff);
            out[offset + 0x17] = (uint8_t)(track->filler & 0xff);
            for (int index = 0; index < sectorCount; index += 1) {
                const Sector& sector = track->sectors[index];
                const int infoOffset = offset + 0x18 + index * 8;
                const int stored = standard ? 0 : storedSize(sector);
                out[infoOffset] = (uint8_t)(sector.c & 0xff);
                out[infoOffset + 1] = (uint8_t)(sector.h & 0xff);
                out[infoOffset + 2] = (uint8_t)(sector.r & 0xff);
                out[infoOffset + 3] = (uint8_t)(sector.n & 0xff);
                out[infoOffset + 4] = (uint8_t)(sector.st1 & 0xff);
                out[infoOffset + 5] = (uint8_t)(sector.st2 & 0xff);
                out[infoOffset + 6] = (uint8_t)(stored & 0xff);
                out[infoOffset + 7] = (uint8_t)((stored >> 8) & 0xff);
            }
            int dataOffset = offset + info;
            for (const auto& sector : track->sectors)
                for (const Bytes& copy : sector.data) {
                    const int n = std::min((int)copy.size(), offset + trackSize - dataOffset);
                    for (int k = 0; k < n; k++) out[dataOffset + k] = copy[k];
                    dataOffset += (int)copy.size();
                }
            for (int a = dataOffset; a < offset + trackSize; a++) out[a] = (uint8_t)track->filler;
            offset += trackSize;
        }
    return out;
}

} // namespace cpcse
