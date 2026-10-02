// CPCSyntaxError — µPD765A floppy controller.
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

struct ActiveSector { int c = 0, h = 0, r = 0, n = 0, st1 = 0, st2 = 0; };

class UPD765A {
public:
    int phase = 0, command = 0;
    std::vector<int> params;
    std::deque<int> result;
    Bytes transfer;
    int transferIndex = 0;
    bool motor = false;
    std::vector<Bytes*> transferTargets;
    std::vector<std::shared_ptr<Bytes>> ownedTargets;
    bool formatting = false;
    std::optional<ActiveSector> activeSector;
    std::array<std::shared_ptr<Disk>, 4> drives{};
    std::array<int, 4> tracks{ 0, 0, 0, 0 };
    int sectorIndex = 0, interruptState = 0x80, activeDriveIndex = 0;
    std::function<void(std::shared_ptr<Disk>, int)> onDiskModified;
    std::unordered_map<std::string, int> sectorReadCounts;
    bool currentSectorHasError = false;
    bool toggleSpeedlock = false;
    int storedSpeedlockByte = 0;
    std::shared_ptr<Track> formatTrack;

    UPD765A() { reset(); }
    void reset();
    struct SnapshotState { bool motor = false; std::array<int, 4> tracks{ 0, 0, 0, 0 }; };
    void restoreSnapshotState(const SnapshotState& state);
    std::shared_ptr<Disk> disk() { return drives[0]; }
    void setDisk(std::shared_ptr<Disk> val) { drives[0] = val; }
    int track() { return tracks[0]; }
    void setTrack(int val) { tracks[0] = val; }
    void advanceCycles(int cycles) { (void)cycles; }
    std::shared_ptr<Disk> mount(const Bytes& input, int unit = 0);
    void eject(int unit = 0);
    void setMotor(int value);
    int status();
    int read(int port);
    void write(int port, int value);
    FdcDrive drive(int unit = 0);
    bool isDriveActive(int index = 0);
    Sector* sectorFor();
    Sector* sectorFor(const std::vector<int>& params);
    std::vector<Sector*> sectorsForTransfer(bool readTrack = false);
    void beginSectorTransfer(const std::vector<Sector*>& sectors, bool write);
    void result7(int st0 = 0, int st1 = 0, int st2 = 0);
    void execute();
    void writeTransfer(int value);
    void finishTransfer();

private:
    int paramOr(int i, int def) const { return i >= 0 && i < (int)params.size() ? params[i] : def; }
};

} // namespace cpcse
