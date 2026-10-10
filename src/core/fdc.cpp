// CPCSyntaxError — µPD765A floppy controller. See fdc.h.
#include "fdc.h"
#include "hfe.h"
#include <algorithm>

namespace cpcse {

enum { IDLE = 0, COMMAND = 1, READ = 2, WRITE = 3, RESULT = 4, WAITING = 5 };
// The parameter bytes after each command byte (& 0x1f), as the data sheet's command table.
static int paramCount(int opcode) {
    switch (opcode) {
        case 0x02: return 8; case 0x03: return 2; case 0x04: return 1; case 0x05: return 8;
        case 0x06: return 8; case 0x07: return 1; case 0x08: return 0; case 0x09: return 8;
        case 0x0a: return 1; case 0x0c: return 8; case 0x0d: return 5; case 0x0f: return 2;
        case 0x11: case 0x19: case 0x1d: return 8;                  // the three scans
        default: return 0;
    }
}

// Status bits (the data sheet's ST0-ST2).
enum {
    ST0_AT = 0x40, ST0_IC = 0x80, ST0_SE = 0x20, ST0_NR = 0x08,
    ST1_EN = 0x80, ST1_DE = 0x20, ST1_OR = 0x10, ST1_ND = 0x04, ST1_NW = 0x02, ST1_MA = 0x01,
    ST2_CM = 0x40, ST2_DD = 0x20, ST2_WC = 0x10, ST2_SH = 0x08, ST2_SN = 0x04, ST2_BC = 0x02, ST2_MD = 0x01,
};

// A sector whose ID field had a CRC error (an extended DSK records it as ST1 DE without
// ST2 DD): the FDC cannot take that ID as a match.
static bool idCrcBad(const Sector& s) { return (s.st1 & ST1_DE) && !(s.st2 & ST2_DD); }

// The IBM layout (fdc.h): GAP 4a (80) + sync (12) + index mark (4) + GAP 1 (50), then per
// sector sync (12) + ID mark (4) + C H R N (4) + CRC (2) + GAP 2 (22) + sync (12) + data mark
// (4) + the data + CRC (2) + GAP 3.
static int fieldBytes(const Sector& s) { return s.data.empty() || s.data[0].empty() ? 128 << (s.n & 7) : (int)s.data[0].size(); }
std::vector<UPD765A::Place> UPD765A::layout(const Track& track) {
    std::vector<Place> out;
    // an HFE or IPF track: where its bitstream has them, a turn of the disc being its length
    bool known = track.lengthBytes > 0 && !track.sectors.empty();
    for (const Sector& s : track.sectors) known = known && s.idPos >= 0;
    if (known) {
        for (const Sector& s : track.sectors) {
            Place p;
            p.id = (int)((long long)s.idPos * TRACK_BYTES / track.lengthBytes);
            p.data = s.dataPos >= 0 ? (int)((long long)s.dataPos * TRACK_BYTES / track.lengthBytes) : p.id + 22 + 12 + 4;
            if (p.data < p.id) p.data += TRACK_BYTES;           // a data field past the index
            p.end = p.data + fieldBytes(s) + 2;
            out.push_back(p);
        }
        return out;
    }
    int pos = 80 + 12 + 4 + 50;
    const int gap3 = track.gap3 > 0 ? track.gap3 : 0x4e;
    for (const Sector& s : track.sectors) {
        Place p;
        p.id = pos + 22;                                 // the ID field read, CRC and all
        p.data = pos + 22 + 22 + 12 + 4;                 // the first data byte
        p.end = p.data + fieldBytes(s) + 2;
        out.push_back(p);
        pos = p.end + gap3;
    }
    if (pos > TRACK_BYTES) {                             // more than a turn: squeezed to fit
        for (Place& p : out) {
            p.id = (int)((long long)p.id * TRACK_BYTES / pos);
            p.data = (int)((long long)p.data * TRACK_BYTES / pos);
            p.end = (int)((long long)p.end * TRACK_BYTES / pos);
        }
    }
    return out;
}
long long UPD765A::nextPass(int bytesFromIndex, long long from) const {
    const long long offset = (long long)bytesFromIndex * BYTE_US % REVOLUTION_US;
    long long t = from - from % REVOLUTION_US + offset;
    if (t < from) t += REVOLUTION_US;
    return t;
}
long long UPD765A::headReadyAt() {
    return now < headLoadedUntil ? now : now + (long long)headLoadMs * 1000;
}

void UPD765A::reset() {
    std::array<std::shared_ptr<Disk>, 4> mountedDrives = drives;
    phase = IDLE; command = 0; params.clear(); result.clear();
    transfer.clear(); transferIndex = 0; motor = false; motorSince = -1;
    machineSounds.motor(false);
    transferRefs.clear(); formatting = false; formatTrack.reset();
    outcome = Outcome{}; scanCondition = 0; resultAt = -1;
    drives = mountedDrives;
    // the heads stay where they are: a reset does not move them
    sectorIndex = 0; activeDriveIndex = 0; interrupts = { -1, -1 };
    stepRateMs = 12; headLoadMs = 4; headUnloadMs = 32; headLoadedUntil = -1;
    seeks = {}; wasReady = { false, false };
}
void UPD765A::restoreSnapshotState(const SnapshotState& state) {
    auto savedOnDiskModified = onDiskModified;
    reset();
    onDiskModified = savedOnDiskModified;
    for (int drive = 0; drive < 4; drive += 1) tracks[drive] = std::clamp(state.tracks[drive], 0, LAST_PHYSICAL_TRACK);
    setMotor(state.motor ? 1 : 0);
    if (state.motor) { motorSince = now - 2 * REVOLUTION_US; insertedAt = { motorSince, motorSince }; }   // already turning
    wasReady = { ready(0), ready(1) };
}
std::shared_ptr<Disk> UPD765A::mount(const Bytes& input, int unit) {
    std::shared_ptr<Disk> disk0 = parseDiskImage(input);
    insert(disk0, unit);
    return disk0;
}
void UPD765A::insert(std::shared_ptr<Disk> disk0, int unit) {
    int driveIndex = unit & 1;
    drives[driveIndex] = std::move(disk0);
    sectorIndex = 0;
    activeDriveIndex = driveIndex;
    insertedAt[(size_t)driveIndex] = now;                    // a new disc: READY once it has turned
    machineSounds.insert();
}
void UPD765A::eject(int unit) {
    int driveIndex = unit & 1;
    if (drives[driveIndex]) machineSounds.eject();
    drives[driveIndex] = nullptr;
}
void UPD765A::setMotor(int value) {
    bool nextMotor = !!(value & 1);
    if (nextMotor != motor) machineSounds.motor(nextMotor);
    if (nextMotor && !motor) motorSince = now;
    if (!nextMotor) motorSince = -1;
    motor = nextMotor;
}
// READY: a disc, turning, the index seen twice since the motor came on (MAME's floppy.cpp).
bool UPD765A::ready(int drive) const {
    const size_t d = (size_t)(drive & 1);
    return drives[d] && motorSince >= 0 && now - std::max(motorSince, insertedAt[d]) >= 2 * REVOLUTION_US;
}

void UPD765A::advanceCycles(int cycles) {
    now += cycles;
    for (int d = 0; d < 2; d++) {
        Seek& s = seeks[(size_t)d];
        if (s.busy && now >= s.doneAt) {
            s.busy = false;
            tracks[(size_t)d] = s.target;
            interrupts[(size_t)d] = s.st0 | (ready(d) ? 0 : ST0_AT | ST0_NR);
        }
    }
    if (phase == WAITING && now >= resultAt) { resultAt = -1; resultId(outcome); }
    else if ((phase == READ || phase == WRITE) && transferIndex < (int)transfer.size() && now >= byteDue(transferIndex) + BYTE_US) overrun();
    // between commands the chip polls each drive's READY (every 2.048 ms at 4 MHz)
    if (phase == IDLE && now - lastPoll >= 2048) {
        lastPoll = now;
        for (int d = 0; d < 2; d++) {
            const bool r = ready(d);
            if (r != wasReady[(size_t)d]) { wasReady[(size_t)d] = r; interrupts[(size_t)d] = 0xc0 | d | (r ? 0 : ST0_NR); }
        }
    }
}

int UPD765A::status() {
    int busy = (seeks[0].busy ? 1 : 0) | (seeks[1].busy ? 2 : 0);   // D0, D1: a drive seeking
    if (phase == IDLE) return 0x80 | busy;
    if (phase == COMMAND) return 0x90 | busy;
    if (phase == READ) return (transferIndex < (int)transfer.size() && now >= byteDue(transferIndex) ? 0xf0 : 0x70) | busy;
    if (phase == WRITE) return (transferIndex < (int)transfer.size() && now >= byteDue(transferIndex) ? 0xb0 : 0x30) | busy;
    if (phase == WAITING) return 0x30 | busy;
    return 0xd0 | busy;
}
int UPD765A::read(int port) {
    if (!(port & 1)) return status();
    if (phase == READ) {
        // the data register: the byte at the head, once it has come (before: the last one)
        if (transferIndex >= (int)transfer.size() || now < byteDue(transferIndex))
            return transferIndex > 0 ? transfer[(size_t)transferIndex - 1] : 0xff;
        int value = transfer[(size_t)transferIndex++];
        if (transferIndex >= (int)transfer.size()) finishTransfer();
        return value;
    }
    if (phase == RESULT) {
        int value = result.empty() ? 0xff : result.front(); if (!result.empty()) result.pop_front();
        if (result.empty()) { phase = IDLE; headLoadedUntil = now + (long long)headUnloadMs * 1000; }
        return value;
    }
    return 0xff;
}
void UPD765A::write(int port, int value) {
    value &= 0xff; if (!(port & 1)) return;
    if (phase == WRITE) {
        if (transferIndex < (int)transfer.size() && now >= byteDue(transferIndex)) writeTransfer(value);
        return;                                                  // not asked for yet: not taken
    }
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
// A unit's drive: DS1 is not connected, so units 2 and 3 are drives 0 and 1 again.
FdcDrive UPD765A::drive(int unit) {
    int driveIndex = unit & 1;
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
// CPCSE_TRACE_FDC: every command and its result, with the time (us).
static bool traceFdc() { static const bool on = std::getenv("CPCSE_TRACE_FDC") != nullptr; return on; }
static void traceBytes(const char* what, long long t, int first, const std::vector<int>& bytes) {
    std::fprintf(stderr, "FDC %10lld %s %02X", t, what, first & 0xff);
    for (int b : bytes) std::fprintf(stderr, " %02X", b & 0xff);
    std::fprintf(stderr, "\n");
}
void UPD765A::resultId(const Outcome& o) {
    result = { o.st0 & 0xff, o.st1 & 0xff, o.st2 & 0xff, o.c & 0xff, o.h & 0xff, o.r & 0xff, o.n & 0xff };
    phase = RESULT;
    if (traceFdc()) traceBytes("result ", now, result[0], std::vector<int>(result.begin() + 1, result.end()));
}
void UPD765A::resultLater(const Outcome& o, long long at) {
    outcome = o;
    if (at <= now) { resultId(o); return; }
    resultAt = at;
    phase = WAITING;
}
long long UPD765A::byteDue(int index) const {
    if (formatting) {
        // FORMAT: each sector's four ID bytes as its ID field comes round after the index
        const int n = paramOr(1, 2) & 7, gap = paramOr(3, 0x4e);
        const int span = 22 + 22 + 12 + 4 + (128 << n) + 2 + gap;
        return transferRefs.empty() ? now : transferRefs[0].start + (long long)(146 + 16 + (index / 4) * span + index % 4) * BYTE_US;
    }
    int offset = 0;
    for (const TransferRef& ref : transferRefs) {
        if (index < offset + (int)ref.length) return ref.start + (long long)(index - offset) * BYTE_US;
        offset += (int)ref.length;
    }
    return transferRefs.empty() ? now : transferRefs.back().start + (long long)transferRefs.back().length * BYTE_US;
}

// A byte not taken (or given) before the next one is ready: OR, and the command ends there,
// its result naming the sector it was in.
void UPD765A::overrun() {
    Outcome o = outcome;
    o.st0 = (o.st0 & 7) | ST0_AT;
    o.st1 = (o.st1 & ~ST1_EN) | ST1_OR;
    int offset = 0;
    for (const TransferRef& ref : transferRefs) {
        if (transferIndex < offset + (int)ref.length) {
            if (ref.track && ref.sector < ref.track->sectors.size()) {
                const Sector& s = ref.track->sectors[ref.sector];
                o.c = s.c; o.h = s.h; o.r = s.r; o.n = s.n;
            }
            break;
        }
        offset += (int)ref.length;
    }
    resultAt = -1;
    if (phase == WRITE && !formatting && !scanCondition) {
        transfer.resize((size_t)transferIndex);              // what came, the rest of the field zeros
        outcome = o;
        finishTransfer();
        return;
    }
    formatting = false; scanCondition = 0; transferRefs.clear();
    resultId(o);
}

// READ DATA, READ DELETED DATA, WRITE DATA, WRITE DELETED DATA and the scans: which
// sectors move, when they pass the head, and how the command ends.
void UPD765A::sectorCommand(int opcode) {
    const bool write = opcode == 0x05 || opcode == 0x09;
    const bool scan = opcode == 0x11 || opcode == 0x19 || opcode == 0x1d;
    const int unit = paramOr(0, 0);
    const FdcDrive d = drive(unit);
    Outcome o;
    o.st0 = unit & 7;
    o.c = paramOr(1, 0); o.h = paramOr(2, 0); o.r = paramOr(3, 0); o.n = paramOr(4, 0);
    const int eot = paramOr(5, 0);
    const int step = scan ? std::max(1, paramOr(7, 1)) : 1;   // a scan's STP: every sector or every other
    if (!ready(d.driveIndex) || !d.valid) { o.st0 |= ST0_AT | ST0_NR; resultId(o); return; }
    long long t = headReadyAt();
    if (!(command & 0x40)) { o.st0 |= ST0_AT; o.st1 |= ST1_MA; resultLater(o, nextIndex(t) + REVOLUTION_US); return; }   // FM: no marks
    if (write && d.disk->writeProtected) { o.st0 |= ST0_AT; o.st1 |= ST1_NW; resultId(o); return; }
    const bool mt = command & 0x80, sk = !write && (command & 0x20);
    int head = d.head;
    std::shared_ptr<Track> track = d.track;
    std::vector<std::pair<std::shared_ptr<Track>, size_t>> moved;
    std::vector<long long> starts;
    long long resultTime = -1;                              // set when it ends without a sector
    for (int guard = 0; guard < 512; guard++) {
        if (!track || track->sectors.empty()) {
            o.st0 |= ST0_AT; o.st1 |= ST1_MA | ST1_ND;
            resultTime = nextIndex(t) + REVOLUTION_US;      // two index pulses and no ID
            break;
        }
        const std::vector<Place> places = layout(*track);
        // the first matching ID to come round
        size_t found = track->sectors.size();
        long long foundAt = 0;
        for (size_t i = 0; i < track->sectors.size(); i++) {
            const Sector& s = track->sectors[i];
            if (s.c == o.c && s.h == o.h && s.r == o.r && s.n == o.n && !idCrcBad(s)) {
                const long long at = nextPass(places[i].id, t);
                if (found == track->sectors.size() || at < foundAt) { found = i; foundAt = at; }
            }
        }
        if (found == track->sectors.size()) {
            o.st0 |= ST0_AT; o.st1 |= ST1_ND;
            for (const Sector& s : track->sectors) {
                if (s.r == o.r && s.c != o.c) o.st2 |= ST2_WC | (s.c == 0xff ? ST2_BC : 0);
                if (s.c == o.c && s.h == o.h && s.r == o.r && s.n == o.n && idCrcBad(s)) o.st1 |= ST1_DE;
            }
            resultTime = nextIndex(t) + REVOLUTION_US;
            break;
        }
        const Sector& s = track->sectors[found];
        const long long dataAt = foundAt + (long long)(places[found].data - places[found].id) * BYTE_US;
        t = foundAt + (long long)(places[found].end - places[found].id) * BYTE_US;   // past its data field
        bool skipped = false;
        if (!write) {
            const bool ddam = s.st2 & ST2_CM, wantDeleted = opcode == 0x0c;
            if (ddam != wantDeleted) {
                o.st2 |= ST2_CM;
                if (sk) skipped = true;                          // SK: past it, unread
                else { moved.push_back({ track, found }); starts.push_back(dataAt); break; }   // read, then the command ends
            }
        }
        if (!skipped) {
            moved.push_back({ track, found });
            starts.push_back(dataAt);
            if (!write && (s.st2 & ST2_DD)) { o.st0 |= ST0_AT; o.st1 |= ST1_DE; o.st2 |= ST2_DD; break; }
        }
        if (o.r == eot) {
            if (mt && head == 0 && d.disk->sides > 1) {          // MT: on to side 1 at R = 1
                head = 1; o.h ^= 1; o.r = 1;
                const int tn = d.trackNum;
                track = tn < (int)d.disk->trackData.size() && d.disk->trackData[tn].size() > 1 ? d.disk->trackData[tn][1] : nullptr;
                continue;
            }
            o.st0 |= ST0_AT; o.st1 |= ST1_EN;                    // no TC: it tries EOT + 1
            break;
        }
        o.r = (o.r + step) & 0xff;
    }
    outcome = o;
    resultAt = resultTime;
    if (moved.empty()) { resultLater(o, resultTime); return; }
    scanCondition = scan ? opcode : 0;
    startTransfer(moved, write || scan, 0);
    for (size_t i = 0; i < transferRefs.size() && i < starts.size(); i++) transferRefs[i].start = starts[i];
}

// READ TRACK: from the index, the track's sectors in the order they pass the head, EOT of
// them, whatever their IDs; ND if none of the IDs it passed was C, H, R, N.
void UPD765A::readTrack() {
    const int unit = paramOr(0, 0);
    const FdcDrive d = drive(unit);
    Outcome o;
    o.st0 = unit & 7;
    o.c = paramOr(1, 0); o.h = paramOr(2, 0); o.r = paramOr(3, 0); o.n = paramOr(4, 0);
    if (!ready(d.driveIndex) || !d.valid) { o.st0 |= ST0_AT | ST0_NR; resultId(o); return; }
    const long long index = nextIndex(headReadyAt());
    if (!(command & 0x40)) { o.st0 |= ST0_AT; o.st1 |= ST1_MA; resultLater(o, index + REVOLUTION_US); return; }
    if (!d.track || d.track->sectors.empty()) { o.st0 |= ST0_AT; o.st1 |= ST1_MA | ST1_ND; resultLater(o, index + REVOLUTION_US); return; }
    const size_t count = std::min<size_t>(std::max(1, paramOr(5, 0)), d.track->sectors.size());
    const std::vector<Place> places = layout(*d.track);
    std::vector<std::pair<std::shared_ptr<Track>, size_t>> moved;
    bool matched = false;
    for (size_t i = 0; i < count; i++) {
        const Sector& s = d.track->sectors[i];
        if (s.c == o.c && s.h == o.h && s.r == o.r && s.n == o.n) matched = true;
        if (s.st2 & ST2_DD) { o.st1 |= ST1_DE; o.st2 |= ST2_DD; }
        moved.push_back({ d.track, i });
    }
    if (!matched) o.st1 |= ST1_ND;
    o.st0 |= ST0_AT; o.st1 |= ST1_EN;                            // no TC
    outcome = o;
    resultAt = -1;
    scanCondition = 0;
    startTransfer(moved, false, 0);
    for (size_t i = 0; i < transferRefs.size(); i++) transferRefs[i].start = index + (long long)places[i].data * BYTE_US;
}

void UPD765A::startTransfer(const std::vector<std::pair<std::shared_ptr<Track>, size_t>>& sectors, bool toFdc, long long from) {
    transferRefs.clear();
    transfer.clear();
    const int n = paramOr(4, 0) & 7, dtl = paramOr(7, 0xff);
    for (const auto& [track, index] : sectors) {
        Sector& sector = track->sectors[index];
        if (sector.data.empty()) sector.data.push_back(Bytes{});
        TransferRef ref;
        ref.track = track; ref.sector = index; ref.start = from;
        // several readings of a weak sector: each in turn; one reading reads as it is
        if (sector.data.size() > 1) ref.copy = (size_t)(sector.weakIndex++ % (int)sector.data.size());
        Bytes reading = sector.data[ref.copy];
        // N 0: DTL bytes of a 128-byte field
        if (n == 0 && sector.n == 0 && dtl < (int)reading.size()) reading.resize((size_t)dtl);
        ref.length = reading.size();
        if (!toFdc) transfer.insert(transfer.end(), reading.begin(), reading.end());
        else transfer.resize(transfer.size() + reading.size(), 0);
        transferRefs.push_back(ref);
    }
    transferIndex = 0;
    phase = toFdc ? WRITE : READ;
    if (transfer.empty()) finishTransfer();
}

void UPD765A::execute() {
    int opcode = command & 0x1f;
    if (traceFdc()) {
        traceBytes("command", now, command, params);
        if (opcode == 0x08) std::fprintf(stderr, "FDC            (pending: A %02X, B %02X)\n", interrupts[0] & 0xff, interrupts[1] & 0xff);
    }
    if (opcode == 0x03) {
        // SPECIFY: SRT/HUT, HLT/ND -- the data sheet's times at 8 MHz, doubled at the CPC's 4
        const int srt = paramOr(0, 0) >> 4 & 15, hut = paramOr(0, 0) & 15, hlt = paramOr(1, 0) >> 1 & 0x7f;
        stepRateMs = (16 - srt) * 2;
        headUnloadMs = (hut ? hut : 16) * 16 * 2;
        headLoadMs = (hlt ? hlt : 128) * 2 * 2;
        phase = IDLE; return;
    }
    if (opcode == 0x08) {
        // SENSE INTERRUPT STATUS: with no interrupt pending it is an invalid command, one
        // byte of &80 (the data sheet; MAME's upd765).
        if (!interruptPending()) { result = { 0x80 }; phase = RESULT; return; }
        const size_t d = interrupts[0] >= 0 ? 0 : 1;
        const int st0 = interrupts[d];
        interrupts[d] = -1;
        result = { st0, tracks[(size_t)(st0 & 1)] };
        phase = RESULT;
        return;
    }
    if (opcode == 0x07 || opcode == 0x0f) {
        // RECALIBRATE / SEEK: the head steps a track every SRT, the drive busy meanwhile;
        // the interrupt comes when it is there. It moves whatever is in the drive, as far as
        // the drive's stop (RECALIBRATE gives up after 77 steps).
        const int unit = paramOr(0, 0), driveIndex = unit & 1;
        const int from = tracks[(size_t)driveIndex];
        int target = opcode == 0x07 ? 0 : std::clamp(paramOr(1, 0), 0, LAST_PHYSICAL_TRACK);
        int st0 = ST0_SE | (unit & 7);
        int steps = std::abs(target - from);
        if (opcode == 0x07 && steps > 77) { steps = 77; target = from - 77; st0 |= ST0_AT | 0x10; }   // EC
        Seek& s = seeks[(size_t)driveIndex];
        s.busy = true; s.target = target; s.st0 = st0;
        s.doneAt = now + (long long)steps * stepRateMs * 1000;
        phase = IDLE;
        machineSounds.steps(steps);
        return;
    }
    if (opcode == 0x04) {
        // SENSE DRIVE STATUS: ST3. Track 0 is the head's own sensor, disc or not.
        int driveIndex = paramOr(0, 0) & 1;
        int head = (unsigned)paramOr(0, 0) >> 2 & 1;
        std::shared_ptr<Disk> disk0 = drives[driveIndex];
        int st3 = (paramOr(0, 0) & 7) | (tracks[driveIndex] == 0 ? 0x10 : 0);
        if (disk0 && head < disk0->sides) {
            if (ready(driveIndex)) st3 |= 0x20;
            st3 |= (disk0->sides > 1 ? 8 : 0) | (disk0->writeProtected ? 0x40 : 0);
        }
        result = { st3 }; phase = RESULT; return;
    }
    if (opcode == 0x0a) {
        // READ ID: the next ID field to pass the head, when it has.
        const int unit = paramOr(0, 0);
        FdcDrive dd = drive(unit);
        Outcome o;
        o.st0 = unit & 7;
        if (!ready(dd.driveIndex) || !dd.valid) { o.st0 |= ST0_AT | ST0_NR; resultId(o); return; }
        const long long t = headReadyAt();
        if (!(command & 0x40) || !dd.track || dd.track->sectors.empty()) {
            o.st0 |= ST0_AT; o.st1 |= ST1_MA | ST1_ND; o.c = dd.trackNum; o.h = dd.head;
            resultLater(o, nextIndex(t) + REVOLUTION_US);
            return;
        }
        const std::vector<Place> places = layout(*dd.track);
        long long best = -1;
        const Sector* sector = nullptr;
        for (size_t i = 0; i < dd.track->sectors.size(); i++) {
            if (idCrcBad(dd.track->sectors[i])) continue;
            const long long at = nextPass(places[i].id, t);
            if (!sector || at < best) { sector = &dd.track->sectors[i]; best = at; }
        }
        if (!sector) { o.st0 |= ST0_AT; o.st1 |= ST1_MA | ST1_DE | ST1_ND; o.c = dd.trackNum; o.h = dd.head; resultLater(o, nextIndex(t) + REVOLUTION_US); return; }
        o.c = sector->c; o.h = sector->h; o.r = sector->r; o.n = sector->n;
        resultLater(o, best);
        return;
    }
    if (opcode == 0x0d) {
        // FORMAT TRACK (N, SC, GPL, D): from the index the CPU gives SC IDs, one as each
        // sector's place comes round; the track becomes those sectors filled with D -- an
        // unformatted track or one past the image's end too. It ends at the next index.
        const int unit = paramOr(0, 0);
        FdcDrive dd = drive(unit);
        Outcome o;
        o.st0 = unit & 7;
        if (!ready(dd.driveIndex) || !dd.valid) { o.st0 |= ST0_AT | ST0_NR; resultId(o); return; }
        if (dd.disk->writeProtected) { o.st0 |= ST0_AT; o.st1 |= ST1_NW; resultId(o); return; }
        const long long index = nextIndex(headReadyAt());
        if (!(command & 0x40)) { o.st0 |= ST0_AT; o.st1 |= ST1_MA; resultLater(o, index + REVOLUTION_US); return; }
        if (!dd.track) {
            Disk& disk = *dd.disk;
            if ((int)disk.trackData.size() <= dd.trackNum) disk.trackData.resize((size_t)dd.trackNum + 1);
            for (auto& cyl : disk.trackData) if ((int)cyl.size() < disk.sides) cyl.resize((size_t)disk.sides);
            disk.tracks = std::max(disk.tracks, dd.trackNum + 1);
            auto t = std::make_shared<Track>();
            t->cylinder = dd.trackNum; t->side = dd.head;
            disk.trackData[(size_t)dd.trackNum][(size_t)dd.head] = t;
            dd.track = t;
        }
        outcome = o;
        resultAt = index + REVOLUTION_US;
        formatting = true; formatTrack = dd.track;
        transferRefs.clear();
        TransferRef start; start.start = index; transferRefs.push_back(start);
        transfer.assign((size_t)paramOr(2, 0) * 4, 0); transferIndex = 0;
        phase = WRITE;
        if (transfer.empty()) finishTransfer();
        return;
    }
    if (opcode == 0x02) { readTrack(); return; }
    if (opcode == 0x05 || opcode == 0x06 || opcode == 0x09 || opcode == 0x0c || opcode == 0x11 || opcode == 0x19 || opcode == 0x1d) {
        sectorCommand(opcode);
        return;
    }
    result = { 0x80 }; phase = RESULT;                           // invalid
}

void UPD765A::writeTransfer(int value) {
    if (transferIndex < (int)transfer.size()) transfer[(size_t)transferIndex++] = (uint8_t)value;
    // a scan ends as soon as a sector meets its condition
    if (scanCondition && !formatting) {
        size_t end = 0;
        for (const TransferRef& ref : transferRefs) {
            end += ref.length;
            if ((size_t)transferIndex == end && end < transfer.size()) { finishScan(); if (phase != WRITE) return; break; }
        }
    }
    if (transferIndex >= (int)transfer.size()) finishTransfer();
}

// A scan: the CPU's bytes against each sector's (&FF from the CPU matches anything).
// Equal (&11), low or equal (&19: disc <= CPU), high or equal (&1D: disc >= CPU). The first
// sector that meets it ends the command, SH if every byte was equal; none: SN.
void UPD765A::finishScan() {
    size_t offset = 0;
    for (const TransferRef& ref : transferRefs) {
        if (offset + ref.length > (size_t)transferIndex) break;      // not all its bytes yet
        const Sector& s = ref.track->sectors[ref.sector];
        const Bytes& disc = s.data[std::min(ref.copy, s.data.size() - 1)];
        bool meets = true, equal = true;
        for (size_t k = 0; k < ref.length; k++) {
            const int cpu = transfer[offset + k], dv = k < disc.size() ? disc[k] : 0;
            if (cpu == 0xff) continue;
            if (dv != cpu) equal = false;
            if (scanCondition == 0x11 && dv != cpu) meets = false;
            if (scanCondition == 0x19 && dv > cpu) meets = false;
            if (scanCondition == 0x1d && dv < cpu) meets = false;
        }
        if (meets) {
            Outcome o = outcome;
            o.st0 = paramOr(0, 0) & 7;
            o.st1 = 0;
            o.st2 = (outcome.st2 & ST2_CM) | (equal ? ST2_SH : 0);
            o.r = s.r;
            transferRefs.clear(); scanCondition = 0;
            resultId(o);
            return;
        }
        offset += ref.length;
    }
}

void UPD765A::finishTransfer() {
    if (formatting) {
        std::vector<Sector> sectors;
        const int sizeCode = paramOr(1, 0) & 7, gap = paramOr(3, 0x4e), filler = paramOr(4, 0);
        for (int offset = 0; offset + 3 < (int)transfer.size(); offset += 4) {
            int n = transfer[offset + 3] & 7; Bytes data((size_t)128 << n, (uint8_t)filler);
            Sector s; s.c = transfer[offset]; s.h = transfer[offset + 1]; s.r = transfer[offset + 2]; s.n = transfer[offset + 3];
            s.st1 = 0; s.st2 = 0; s.data = { data }; s.weakIndex = 0;
            sectors.push_back(s);
        }
        formatTrack->sectors = sectors;
        formatTrack->sizeCode = sizeCode;
        formatTrack->gap3 = gap;
        formatTrack->filler = filler;
        formatTrack->lengthBytes = 0;                        // laid out afresh: the standard layout now
        formatting = false;
        transferRefs.clear();
        int fDrive = paramOr(0, 0) & 1;
        std::shared_ptr<Disk> fDisk = drives[fDrive];
        if (fDisk) {
            fDisk->modified = true;
            if (onDiskModified) onDiskModified(fDisk, fDrive);
        }
        Outcome o = outcome;
        if (!sectors.empty()) { o.c = sectors.back().c; o.h = sectors.back().h; o.r = sectors.back().r; o.n = sectors.back().n; }
        else { o.c = tracks[fDrive]; o.n = sizeCode; }
        resultLater(o, resultAt);                                // the next index
        return;
    }
    if (scanCondition) {
        finishScan();
        if (phase == RESULT) return;
        Outcome o = outcome;                                     // no sector met it
        o.st2 |= ST2_SN;
        scanCondition = 0; transferRefs.clear();
        resultId(o);
        return;
    }
    if (phase == WRITE) {
        // A write leaves one clean copy, its CRC good, its mark the command's: WRITE DATA a
        // data mark, WRITE DELETED DATA a deleted one. The sector is found again where it
        // was; one that has gone since takes nothing. (An overrun's short field: zeros.)
        const bool deleted = (command & 0x1f) == 0x09;
        size_t offset = 0;
        for (const TransferRef& ref : transferRefs) {
            if (offset >= transfer.size()) break;
            if (ref.track && ref.sector < ref.track->sectors.size()) {
                Sector& s = ref.track->sectors[ref.sector];
                Bytes field(ref.length, 0);
                const size_t n = std::min(ref.length, transfer.size() - offset);
                std::copy(transfer.begin() + (long)offset, transfer.begin() + (long)(offset + n), field.begin());
                s.data = { field };
                s.weakIndex = 0;
                s.st1 &= ~ST1_DE;
                s.st2 &= ~(ST2_DD | ST2_MD | ST2_CM);
                if (deleted) s.st2 |= ST2_CM;
            }
            offset += ref.length;
        }
        int driveIndex = paramOr(0, 0) & 1;
        std::shared_ptr<Disk> disk0 = drives[driveIndex];
        if (disk0) {
            disk0->modified = true;
            if (onDiskModified) onDiskModified(disk0, driveIndex);
        }
    }
    transferRefs.clear();
    resultLater(outcome, resultAt);                             // ND: once the index has passed twice
    resultAt = phase == WAITING ? resultAt : -1;
}

} // namespace cpcse
