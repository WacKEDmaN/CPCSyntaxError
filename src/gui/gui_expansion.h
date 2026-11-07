// CPCSyntaxError: the Expansion port window: the 50 pins of the CPC's expansion connector
// lit while active, the boards on the bus with the ports they decode, the ports two boards
// both answer, and a log of the I/O cycles (core/expansion_bus.h records them).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cpcse {

class EmuHost;

class ExpansionPortWindow {
public:
    explicit ExpansionPortWindow(EmuHost& host);
    void draw(bool* open);
    bool focused = false;
    // the ports each fitted board decodes and the ports two of them share, from the decoders
    // themselves (public for gui-shell-check)
    struct Clash { int first, last; bool write; uint32_t boards; };
    std::vector<Clash> clashes;
    std::array<std::string, 32> boardPorts;     // per BusDevice: "&FBEE-&FBEF, &FAEF (in)"
    uint32_t fittedBoards = 0;                  // a bit per BusDevice
    void scanDecoders();
private:
    EmuHost& host;
    std::array<float, 50> glow{};               // each pin's lamp, 0..1, fading
    std::array<float, 32> boardGlow{};          // each board's lamp
    std::array<uint64_t, 32> boardSeen{};       // the access counts at the last frame
    // a second's worth, for the tooltips
    std::array<uint64_t, 50> secActive{}, secEdges{};
    uint64_t secSamples = 0;
    std::array<double, 50> dutyShown{}, edgesShown{};
    double secStarted = 0;
    double lastScan = -10;
    uint32_t scannedFor = 0xffffffff;
    bool breakNoted = false;
};

} // namespace cpcse
