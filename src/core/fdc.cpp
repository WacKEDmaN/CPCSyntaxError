// CPCSyntaxError — µPD765A floppy controller.
#include "fdc.h"
#include <algorithm>
#include <random>

namespace cpcse {

enum { IDLE = 0, COMMAND = 1, READ = 2, WRITE = 3, RESULT = 4 };
// PARAMS lookup: parameter count per command opcode (& 0x1f).
static int paramCount(int opcode) {
    switch (opcode) {
        case 0x02: return 8; case 0x03: return 2; case 0x04: return 1; case 0x05: return 8;
        case 0x06: return 8; case 0x07: return 1; case 0x08: return 0; case 0x09: return 8;
        case 0x0a: return 1; case 0x0c: return 8; case 0x0d: return 5; case 0x0f: return 2;
        default: return 0;
    }
}


static double jsRandom() {
    static std::mt19937 engine(0x1234abcd);
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(engine);
}
static int randByte() { return (int)std::floor(jsRandom() * 256); }

static Bytes mutateWeakSector(const Sector& sector) {
    const Bytes& base = sector.data[0];
    Bytes copy(base.size());
    for (size_t i = 0; i < base.size(); i++) copy[i] = base[i];
    if (sector.c == 0 && (sector.r == 72 || sector.r == 193 || sector.r == 65)) {
        for (int i = 256; i < std::min((int)288, (int)copy.size()); i += 1) {
            if (jsRandom() < 0.6) copy[i] = (uint8_t)randByte();
        }
        for (int i = 336; i < (int)copy.size(); i += 1) {
            if (jsRandom() < 0.7) copy[i] = (uint8_t)randByte();
        }
    } else if (sector.c == 40 && sector.r == 198) {
        for (int i = 100; i < (int)copy.size(); i += 1) {
            double prob = (i == 105 || i == 106) ? 0.9 : 0.5;
            if (jsRandom() < prob) copy[i] = (uint8_t)randByte();
        }
    } else if (sector.c == 39 && sector.r == 202) {
        if (copy.size() > 5) {
            int oldVal = copy[5];
            do { copy[5] = (uint8_t)randByte(); } while (copy[5] == oldVal && jsRandom() < 0.8);
        }
        for (int i = 6; i < std::min((int)14, (int)copy.size()); i += 1) {
            if (jsRandom() < 0.4) copy[i] = (uint8_t)randByte();
        }
        if (copy.size() >= 16) {
            int matchByte = randByte();
            copy[14] = (uint8_t)matchByte; copy[15] = (uint8_t)matchByte;
        }
    } else {
        int numChanges = 4 + (int)std::floor(jsRandom() * 4);
        for (int i = 0; i < numChanges && copy.size() > 4; i += 1) {
            int pos = 4 + (int)std::floor(jsRandom() * (copy.size() - 4));
            copy[pos] = (uint8_t)randByte();
        }
    }
    return copy;
}

void UPD765A::reset() {
    std::array<std::shared_ptr<Disk>, 4> mountedDrives = drives;
    phase = IDLE; command = 0; params.clear(); result.clear();
    transfer.clear(); transferIndex = 0; motor = false;
    machineSounds.motor(false);
    transferTargets.clear(); ownedTargets.clear(); transferRefs.clear(); formatting = false; activeSector.reset();
    drives = mountedDrives;
    // tracks preserved (this.tracks ? slice : [0,0,0,0]) — already a member.
    sectorIndex = 0; interruptState = 0x80; activeDriveIndex = 0;
    // onDiskModified preserved (member).
    sectorReadCounts.clear();
    currentSectorHasError = false;
    toggleSpeedlock = false;
    storedSpeedlockByte = 0;
}
void UPD765A::restoreSnapshotState(const SnapshotState& state) {
    auto savedOnDiskModified = onDiskModified;
    reset();
    onDiskModified = savedOnDiskModified;
    for (int drive = 0; drive < 4; drive += 1) {
        std::shared_ptr<Disk> disk0 = drives[drive];
        int requested = state.tracks[drive];
        tracks[drive] = disk0 ? std::max(0, std::min(requested, disk0->tracks - 1)) : 0;
    }
    setMotor(state.motor ? 1 : 0);
}
std::shared_ptr<Disk> UPD765A::mount(const Bytes& input, int unit) {
    std::shared_ptr<Disk> disk0 = parseDsk(input);
    insert(disk0, unit);
    return disk0;
}
void UPD765A::insert(std::shared_ptr<Disk> disk0, int unit) {
    int driveIndex = unit & 3;
    drives[driveIndex] = std::move(disk0);
    tracks[driveIndex] = 0;
    sectorIndex = 0;
    activeDriveIndex = driveIndex;
    interruptState = 0xc0 | driveIndex;
    machineSounds.insert();
}
void UPD765A::eject(int unit) {
    int driveIndex = unit & 3;
    if (drives[driveIndex]) machineSounds.eject();
    drives[driveIndex] = nullptr;
    interruptState = 0xc8 | driveIndex;
}
void UPD765A::setMotor(int value) {
    bool nextMotor = !!(value & 1);
    if (nextMotor != motor) machineSounds.motor(nextMotor);
    motor = nextMotor;
}
int UPD765A::status() {
    if (phase == IDLE) return 0x80;
    if (phase == COMMAND) return 0x90;
    if (phase == READ) return 0xf0;
    if (phase == WRITE) return 0xb0;
    return 0xd0;
}
int UPD765A::read(int port) {
    if (!(port & 1)) return status();
    if (phase == READ) {
        int value = transferIndex < (int)transfer.size() ? transfer[transferIndex] : 0xff; transferIndex++;
        if (transferIndex >= (int)transfer.size()) finishTransfer();
        return value;
    }
    if (phase == RESULT) {
        int value = result.empty() ? 0xff : result.front(); if (!result.empty()) result.pop_front();
        if (result.empty()) phase = IDLE;
        return value;
    }
    return 0xff;
}
void UPD765A::write(int port, int value) {
    value &= 0xff; if (!(port & 1)) return;
    if (phase == WRITE) { writeTransfer(value); return; }
    if (phase == IDLE) {
        command = value; params.clear(); phase = COMMAND;
        if (paramCount(value & 0x1f) == 0) execute();
        return;
    }
    if (phase == COMMAND) {
        params.push_back(value);
        if ((int)params.size() >= paramCount(command & 0x1f)) execute();
    }
}
FdcDrive UPD765A::drive(int unit) {
    int driveIndex = unit & 3;
    activeDriveIndex = driveIndex;
    int head = (unsigned)unit >> 2 & 1;
    std::shared_ptr<Disk> disk0 = drives[driveIndex];
    int trackNum = tracks[driveIndex];
    bool valid = disk0 && (head < disk0->sides);
    std::shared_ptr<Track> track;
    if (valid && trackNum >= 0 && trackNum < (int)disk0->trackData.size() && head < (int)disk0->trackData[trackNum].size())
        track = disk0->trackData[trackNum][head];
    return { head, valid, track, disk0, trackNum, driveIndex };
}
bool UPD765A::isDriveActive(int index) {
    return motor && activeDriveIndex == (index & 3);
}
Sector* UPD765A::sectorFor() { return sectorFor(params); }
Sector* UPD765A::sectorFor(const std::vector<int>& p) {
    FdcDrive d = drive(p.empty() ? 0 : p[0]);
    if (!d.valid || !d.track) return nullptr;
    auto get = [&](int i) { return i < (int)p.size() ? p[i] : 0; };
    for (auto& sector : d.track->sectors) if (sector.c == get(1) && sector.r == get(3) && sector.n == (get(4) & 7)) return &sector;
    for (auto& sector : d.track->sectors) if (sector.c == get(1) && sector.r == get(3)) return &sector;
    return nullptr;
}
std::vector<Sector*> UPD765A::sectorsForTransfer(bool readTrack) {
    FdcDrive d = drive(paramOr(0, 0));
    std::vector<Sector*> out;
    if (!d.valid || !d.track || d.track->sectors.empty()) return out;
    if (readTrack) { for (auto& s : d.track->sectors) out.push_back(&s); return out; }
    int c = paramOr(1, 0), startR = paramOr(3, 0), n = paramOr(4, 0) & 7;
    int eot = paramOr(5, 0) >= startR ? paramOr(5, 0) : startR;
    for (int r = startR; r <= eot; r += 1) {
        Sector* found = nullptr;
        for (auto& sector : d.track->sectors) if (sector.c == c && sector.r == (r & 0xff) && sector.n == n) { found = &sector; break; }
        if (!found) for (auto& sector : d.track->sectors) if (sector.c == c && sector.r == (r & 0xff)) { found = &sector; break; }
        if (!found) break;
        out.push_back(found);
    }
    return out;
}
void UPD765A::beginSectorTransfer(const std::vector<Sector*>& sectors, bool write) {
    transferTargets.clear(); ownedTargets.clear(); transferRefs.clear(); int length = 0;
    FdcDrive d = drive(paramOr(0, 0));
    for (Sector* sector : sectors) {
        TransferRef ref;
        if (d.track && !d.track->sectors.empty() && sector >= &d.track->sectors.front() && sector <= &d.track->sectors.back()) {
            ref.track = d.track;
            ref.sector = (size_t)(sector - &d.track->sectors.front());
        }
        Bytes* target = nullptr;
        bool isWeak = !!((sector->st1 & 0x20) || (sector->st2 & 0x20));
        bool isLegacySpeedlock = (!isWeak && sector->c == 0
            && (sector->r == 72 || sector->r == 193 || sector->r == 65));
        std::string sectorKey = std::to_string(sector->c) + ":" + std::to_string(sector->h) + ":" + std::to_string(sector->r) + ":" + std::to_string(sector->n);
        int readCount = (sectorReadCounts.count(sectorKey) ? sectorReadCounts[sectorKey] : 0) + 1;
        sectorReadCounts[sectorKey] = readCount;
        if (isWeak) currentSectorHasError = (readCount > 1);
        else currentSectorHasError = false;
        if (sector->data.empty()) sector->data.push_back(Bytes{});   // an empty sector transfers nothing
        if (sector->data.size() > 1) {
            int copyIndex = sector->weakIndex++ % (int)sector->data.size();
            target = &sector->data[copyIndex];
            ref.copy = (size_t)copyIndex;
        } else if (!write && isWeak) {
            ownedTargets.push_back(std::make_shared<Bytes>(mutateWeakSector(*sector)));
            target = ownedTargets.back().get();
        } else if (!write && isLegacySpeedlock) {
            toggleSpeedlock = !toggleSpeedlock;
            const Bytes& base = sector->data[0];
            auto buf = std::make_shared<Bytes>(base);
            if (!toggleSpeedlock) {
                if ((*buf)[0] != 0) storedSpeedlockByte = (*buf)[0];
                (*buf)[0] = ((*buf)[0] != 0) ? 0 : (uint8_t)(storedSpeedlockByte ? storedSpeedlockByte : 0);
            }
            ownedTargets.push_back(buf);
            target = ownedTargets.back().get();
        } else {
            toggleSpeedlock = false;
            target = &sector->data[0];
        }
        transferTargets.push_back(target);
        ref.length = target->size();
        transferRefs.push_back(ref);
        length += (int)target->size();
    }
    transfer.assign(length, 0); int offset = 0;
    if (!write) {
        for (Bytes* target : transferTargets) {
            for (size_t k = 0; k < target->size(); k++) transfer[offset + k] = (*target)[k];
            offset += (int)target->size();
        }
    }
    transferIndex = 0;
    if (!sectors.empty()) { Sector* last = sectors.back(); activeSector = ActiveSector{ last->c, last->h, last->r, last->n, last->st1, last->st2 }; }
    else activeSector.reset();
    phase = write ? WRITE : READ;
}
void UPD765A::result7(int st0, int st1, int st2) {
    int driveIndex = paramOr(0, 0) & 3;
    result = { st0, st1, st2, paramOr(1, tracks[driveIndex]), paramOr(2, 0), paramOr(3, 0), paramOr(4, 0) };
    phase = RESULT;
}
void UPD765A::execute() {
    int opcode = command & 0x1f;
    if (opcode == 0x03) { phase = IDLE; return; } // SPECIFY
    if (opcode == 0x08) {
        int driveIndex = interruptState & 3;
        result = { interruptState, tracks[driveIndex] };
        interruptState = 0x80;
        phase = RESULT;
        return;
    }
    if (opcode == 0x07) {
        int driveIndex = paramOr(0, 0) & 3;
        int oldTrack = tracks[driveIndex];
        tracks[driveIndex] = 0;
        interruptState = 0x20 | (paramOr(0, 0) & 7);
        phase = IDLE;
        machineSounds.steps(oldTrack);   // RECALIBRATE steps the head back to track 0
        return;
    }
    if (opcode == 0x0f) {
        int driveIndex = paramOr(0, 0) & 3;
        FdcDrive dd = drive(paramOr(0, 0));
        std::shared_ptr<Disk> disk0 = dd.disk;
        int oldTrack = tracks[driveIndex];
        int newTrack = disk0 ? std::min(paramOr(1, 0), disk0->tracks - 1) : 0;
        tracks[driveIndex] = newTrack;
        interruptState = (disk0 ? 0x20 : 0x48) | (paramOr(0, 0) & 7);
        phase = IDLE;
        machineSounds.steps(std::abs(newTrack - oldTrack));   // one step a track
        return;
    }
    if (opcode == 0x04) {
        int driveIndex = paramOr(0, 0) & 3;
        int head = (unsigned)paramOr(0, 0) >> 2 & 1;
        std::shared_ptr<Disk> disk0 = drives[driveIndex];
        int st3 = paramOr(0, 0) & 7;
        if (disk0 && head < disk0->sides) {
            st3 |= 0x20 | (tracks[driveIndex] == 0 ? 0x10 : 0) | (disk0->sides > 1 ? 8 : 0);
        }
        result = { st3 }; phase = RESULT; return;
    }
    if (opcode == 0x0a) {
        FdcDrive dd = drive(paramOr(0, 0));
        Sector* sector = nullptr;
        if (dd.track && !dd.track->sectors.empty()) sector = &dd.track->sectors[sectorIndex % (int)dd.track->sectors.size()];
        if (!dd.valid || !sector) {
            // params.splice(1,4, trackNum, head, 0, 0)
            while ((int)params.size() < 5) params.push_back(0);
            params[1] = dd.trackNum; params[2] = dd.head; params[3] = 0; params[4] = 0;
            int st0 = (paramOr(0, 0) & 7) | (dd.valid ? 0x40 : 0x48);
            result7(st0, dd.valid ? 0x05 : 0x04, 0x00);
            return;
        }
        sectorIndex += 1;
        while ((int)params.size() < 5) params.push_back(0);
        params[1] = sector->c; params[2] = sector->h; params[3] = sector->r; params[4] = sector->n;
        result7(paramOr(0, 0) & 7);
        return;
    }
    if (opcode == 0x02 || opcode == 0x05 || opcode == 0x06 || opcode == 0x09 || opcode == 0x0c) {
        std::vector<Sector*> sectors = sectorsForTransfer(opcode == 0x02);
        if (sectors.empty()) {
            FdcDrive dd = drive(paramOr(0, 0));
            int st0 = (paramOr(0, 0) & 7) | (dd.valid ? 0x40 : 0x48);
            result7(st0, dd.valid ? 0x05 : 0x04, 0x00);
            return;
        }
        beginSectorTransfer(sectors, opcode == 0x05 || opcode == 0x09);
        return;
    }
    if (opcode == 0x0d) {
        FdcDrive dd = drive(paramOr(0, 0));
        if (!dd.valid || !dd.track) {
            int st0 = (paramOr(0, 0) & 7) | (dd.valid ? 0x40 : 0x48);
            result7(st0, dd.valid ? 0x05 : 0x04, 0x00);
            return;
        }
        formatting = true; formatTrack = dd.track;
        transfer.assign((paramOr(2, 0)) * 4, 0); transferIndex = 0; phase = WRITE; return;
    }
    result = { 0x80 }; phase = RESULT;
}
void UPD765A::writeTransfer(int value) {
    if (transferIndex < (int)transfer.size()) transfer[transferIndex++] = (uint8_t)value;
    if (transferIndex >= (int)transfer.size()) finishTransfer();
}
void UPD765A::finishTransfer() {
    if (formatting) {
        std::vector<Sector> sectors; int sizeCode = paramOr(1, 0) & 7, filler = paramOr(4, 0);
        for (int offset = 0; offset + 3 < (int)transfer.size(); offset += 4) {
            int n = transfer[offset + 3] & 7; Bytes data(128 << n, (uint8_t)filler);
            Sector s; s.c = transfer[offset]; s.h = transfer[offset + 1]; s.r = transfer[offset + 2]; s.n = n;
            s.st1 = 0; s.st2 = 0; s.data = { data }; s.weakIndex = 0;
            sectors.push_back(s);
        }
        formatTrack->sectors = sectors; formatting = false;
        int fDrive = paramOr(0, 0) & 3;
        std::shared_ptr<Disk> fDisk = drives[fDrive];
        if (fDisk) {
            fDisk->modified = true;
            if (onDiskModified) onDiskModified(fDisk, fDrive);
        }
        if (!sectors.empty()) { Sector& last = sectors.back(); activeSector = ActiveSector{ last.c, last.h, last.r, last.n, last.st1, last.st2 }; }
        else activeSector = ActiveSector{ tracks[paramOr(0, 0) & 3], 0, 0, sizeCode, 0, 0 };
    } else if (phase == WRITE) {
        // Found again, not written through the pointers taken at the start: the sector may
        // have moved or gone since. One that is gone takes nothing.
        size_t offset = 0;
        for (const TransferRef& ref : transferRefs) {
            if (ref.track && ref.sector < ref.track->sectors.size()) {
                Sector& s = ref.track->sectors[ref.sector];
                if (ref.copy < s.data.size()) {
                    Bytes& dest = s.data[ref.copy];
                    const size_t n = std::min({ dest.size(), ref.length, transfer.size() - std::min(offset, transfer.size()) });
                    for (size_t k = 0; k < n; k++) dest[k] = transfer[offset + k];
                }
            }
            offset += ref.length;
        }
        transferTargets.clear();
        transferRefs.clear();
        int driveIndex = paramOr(0, 0) & 3;
        std::shared_ptr<Disk> disk0 = drives[driveIndex];
        if (disk0) {
            disk0->modified = true;
            if (onDiskModified) onDiskModified(disk0, driveIndex);
        }
    }
    if (activeSector) {
        while ((int)params.size() < 5) params.push_back(0);
        params[1] = activeSector->c; params[2] = activeSector->h; params[3] = activeSector->r; params[4] = activeSector->n;
    }
    int st1 = activeSector ? (activeSector->st1 & 0xff) : 0;
    int st2 = activeSector ? (activeSector->st2 & 0xff) : 0;
    if (currentSectorHasError) {
        st1 |= 0x20;
        st2 |= 0x20;
    }
    if (phase == READ && ((command & 0x1f) == 0x06 || (command & 0x1f) == 0x0c)) {
        bool deleted = !!(st2 & 0x40);
        bool wantsDeleted = (command & 0x1f) == 0x0c;
        if (deleted != wantsDeleted) {
            st2 |= 0x40;
        }
    }
    int st0 = (paramOr(0, 0) & 7) | (((st1 & 0x24) || (st2 & 0x20)) ? 0x40 : 0);
    result7(st0, st1, st2);
}

} // namespace cpcse
