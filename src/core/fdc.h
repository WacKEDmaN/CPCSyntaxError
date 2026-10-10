// CPCSyntaxError — µPD765A floppy controller.
//
// The commands as NEC's data sheet runs them, on the CPC's wiring (Kevin Thacker, "Floppy
// disc controller"):
//   - TC (terminal count) is not connected: a read or write never stops at the last sector
//     it was asked for, it tries the next one, finds it is past EOT and ends with ST0
//     abnormal termination and ST1 EN (End of Cylinder) -- every AMSDOS read ends that way;
//   - DS1 is not connected: two drives, unit 2 is drive 0 again and unit 3 drive 1;
//   - MFM is fixed at 1: a command asking for FM finds no address marks;
//   - the drives reach a few tracks past 39 (41 for many, 43 for one) -- a protection's
//     track 41 is real; an image's missing track there reads as unformatted;
//   - INT is not connected: the CPU polls the main status register (MSR).
// A read or write goes sector by sector from R: each ID on the track compared with C, H, R
// and N (all four); a deleted data mark (CM) ends a read after its sector unless SK skips
// it; a CRC error in the data ends it (DE, DD); R = EOT ends it with EN, or with MT goes on
// to the other side. A sector not found is ND, with WC (or WC and BC for &FF) when an ID
// there had the right R and another cylinder; a track with no IDs at all is MA.
//
// TIME. The disc turns at 300 rpm, 200 ms a turn, under a 250 kbit/s MFM head: a byte every
// 32 us, 6250 bytes from index to index. Each track's sectors are laid out on it as a
// standard IBM format puts them (GAP 4a, the index mark, GAP 1, then for each sector its
// sync, ID, GAP 2, sync, data and GAP 3 -- the track's own GAP 3), squeezed to fit if they
// would not. So:
//   - a read's bytes come as its sectors' data fields pass the head, one each 32 us, and the
//     MSR says so (RQM); a byte not taken before the next one is ready is an overrun (ST1
//     OR) and ends the command -- a write's bytes are asked for the same way;
//   - a sector that is not there is ND only once the index hole has passed twice;
//   - READ ID answers when the next ID field has passed; FORMAT runs index to index;
//   - SPECIFY's times, doubled for the CPC's 4 MHz clock (the data sheet's are for 8 MHz):
//     SRT a step every (16 - SRT) x 2 ms, HLT x 4 ms to load the head, HUT x 32 ms of idle to
//     unload it; a SEEK steps a track at a time, the drive busy in the MSR meanwhile, and its
//     interrupt comes when the head arrives;
//   - READY: the drive's, once the motor has turned the disc past the index twice (as MAME's
//     floppy.cpp has it) -- before that, and with the motor off, a command is NR. The chip
//     polls READY between commands and a change is an interrupt (ST0 &C0 + the unit).
#pragma once
#include "common.h"
#include "dsk.h"
#include "machine_sounds.h"   // the drive's motor, head steps and insert

namespace cpcse {


struct FdcDrive {
    int head = 0;
    bool valid = false;
    std::shared_ptr<Track> track;
    std::shared_ptr<Disk> disk;
    int trackNum = 0;
    int driveIndex = 0;
};

class UPD765A {
public:
    // The furthest a drive's head steps (0-based): 3" drives reach 41, one 43 (see above).
    static constexpr int LAST_PHYSICAL_TRACK = 42;
    static constexpr long long REVOLUTION_US = 200000;     // 300 rpm
    static constexpr int BYTE_US = 32;                     // 250 kbit/s MFM
    static constexpr int TRACK_BYTES = (int)(REVOLUTION_US / BYTE_US);   // 6250

    int phase = 0, command = 0;
    std::vector<int> params;
    std::deque<int> result;
    Bytes transfer;
    int transferIndex = 0;
    bool motor = false;
    // The sectors a read or write moves, found again by their place on a track that is kept
    // alive -- a write's last byte comes in many instructions later, by which time the disc
    // may have been ejected or swapped, or edited (the DSK editor shares it).
    struct TransferRef { std::shared_ptr<Track> track; size_t sector = 0, copy = 0, length = 0; long long start = 0; };
    std::vector<TransferRef> transferRefs;
    bool formatting = false;
    std::shared_ptr<Track> formatTrack;
    // How the command ends, decided when it starts: its status and the ID in its result.
    struct Outcome { int st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0; };
    Outcome outcome;
    long long resultAt = -1;             // the result phase starts then (an execution without bytes)
    int scanCondition = 0;               // scans: 0x11 equal, 0x19 low or equal, 0x1d high or equal
    std::array<std::shared_ptr<Disk>, 4> drives{};
    std::array<int, 4> tracks{ 0, 0, 0, 0 };
    int sectorIndex = 0, activeDriveIndex = 0;
    // The interrupt each drive has waiting for SENSE INTERRUPT STATUS: one ST0 a drive, the
    // latest event's -- a seek ending replaces a READY change not yet sensed (-1: none).
    std::array<int, 2> interrupts{ -1, -1 };
    bool interruptPending() const { return interrupts[0] >= 0 || interrupts[1] >= 0; }
    std::function<void(std::shared_ptr<Disk>, int)> onDiskModified;

    // time (us since power on), the motor, the heads
    long long now = 0;
    long long motorSince = -1;           // when the motor last started (-1: off)
    int stepRateMs = 12, headLoadMs = 4, headUnloadMs = 32;   // SPECIFY's, at 4 MHz
    long long headLoadedUntil = -1;      // the head stays loaded until then
    struct Seek { bool busy = false; long long doneAt = 0; int target = 0, st0 = 0; };
    std::array<Seek, 2> seeks{};
    std::array<bool, 2> wasReady{ false, false };
    std::array<long long, 2> insertedAt{ -(1LL << 40), -(1LL << 40) };   // a disc turns from when it went in
    long long lastPoll = 0;

    UPD765A() { reset(); }
    void reset();
    struct SnapshotState { bool motor = false; std::array<int, 4> tracks{ 0, 0, 0, 0 }; };
    void restoreSnapshotState(const SnapshotState& state);
    std::shared_ptr<Disk> disk() { return drives[0]; }
    void setDisk(std::shared_ptr<Disk> val) { drives[0] = val; }
    int track() { return tracks[0]; }
    void setTrack(int val) { tracks[0] = val; }
    void advanceCycles(int cycles);      // microseconds
    std::shared_ptr<Disk> mount(const Bytes& input, int unit = 0);
    // A disc already in memory (the DSK editor's): the drive and the editor share it.
    void insert(std::shared_ptr<Disk> disk, int unit = 0);
    void eject(int unit = 0);
    void setMotor(int value);
    bool ready(int drive) const;         // the drive's READY line
    int status();
    int read(int port);
    void write(int port, int value);
    FdcDrive drive(int unit = 0);
    void execute();
    void writeTransfer(int value);
    void finishTransfer();

    // Where a track's fields pass the head, in bytes from the index (see above).
    struct Place { int id = 0, data = 0, end = 0; };
    static std::vector<Place> layout(const Track& track);

private:
    int paramOr(int i, int def) const { return i >= 0 && i < (int)params.size() ? params[i] : def; }
    void resultId(const Outcome& o);
    void resultLater(const Outcome& o, long long at);
    void sectorCommand(int opcode);
    void readTrack();
    void startTransfer(const std::vector<std::pair<std::shared_ptr<Track>, size_t>>& sectors, bool toFdc, long long from);
    void finishScan();
    long long byteDue(int index) const;  // when transfer byte `index` is (or is wanted) at the head
    long long nextPass(int bytesFromIndex, long long from) const;   // the next time that place passes
    long long nextIndex(long long from) const { return nextPass(0, from); }
    long long headReadyAt();             // the head loaded: now, or after HLT
    void overrun();
};

} // namespace cpcse
