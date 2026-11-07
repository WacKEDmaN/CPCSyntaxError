// CPCSyntaxError: the CPC's 50-way expansion connector, as a debugger sees it.
//
// THE PINS (Amstrad Computer User, April 1985, Figure 4, the 464/664/6128 connector):
//   1 SOUND   2 GND     3-18 A15..A0   19-26 D7..D0   27 +5V
//   28 /MREQ  29 /M1    30 /RFSH  31 /IORQ  32 /RD  33 /WR  34 /HALT  35 /INT  36 /NMI
//   37 /BUSRQ 38 /BUSAK 39 READY  40 /BUS RESET  41 /RESET  42 /ROMEN  43 ROMDIS
//   44 /RAMRD 45 RAMDIS 46 CURSOR 47 LPEN  48 /EXP  49 GND  50 CLOCK (4 MHz)
//
// The probe only reads the machine. It samples the pins once a microsecond (the rate the
// logic analyser samples at) and counts every I/O cycle by the devices that decode its
// address, while `enabled` (its window is open); otherwise it costs nothing.
//
// It lives inside the machine object, which a rewind copies byte for byte, so it holds
// fixed-size arrays only; the timeline keeps the live probe across a restore (it is the
// debugger's, not the machine's).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace cpcse {

inline constexpr int EXPANSION_PIN_COUNT = 50;

struct ExpansionPin {
    int number;             // 1..50, as on the connector
    const char* name;
    bool activeLow;         // the name carries the bar: the pin is "active" when low
    const char* what;       // one line for the tooltip
};
// Index = pin number - 1.
const std::array<ExpansionPin, EXPANSION_PIN_COUNT>& expansionPins();

// Pin numbers by name, for the code that drives them.
namespace pin {
inline constexpr int SOUND = 1, GND1 = 2, A15 = 3, A0 = 18, D7 = 19, D0 = 26, VCC = 27;
inline constexpr int MREQ = 28, M1 = 29, RFSH = 30, IORQ = 31, RD = 32, WR = 33, HALT = 34;
inline constexpr int INT = 35, NMI = 36, BUSRQ = 37, BUSAK = 38, READY = 39, BUSRESET = 40;
inline constexpr int RESET = 41, ROMEN = 42, ROMDIS = 43, RAMRD = 44, RAMDIS = 45;
inline constexpr int CURSOR = 46, LPEN = 47, EXP = 48, GND2 = 49, CLOCK = 50;
}

// Everything that can decode an I/O address on the bus: the machine's own chips first
// (ACCC §5.1's partial decoding), then the boards on the connector.
enum class BusDevice : uint8_t {
    GateArray, Crtc, RomSelect, Ppi, FdcMotor, Fdc, Printer,      // the CPC's own
    M4, Multiface, Rs232, Opl4, PlayCity, Speech, Sf2Mouse, Sf2Rtc, Sf3, Ide,
    Albireo1, Albireo2, AlbireoUart, Hd20, MouseInterface, Gfx9000, AmdrumDac,
    Count
};
inline constexpr int BUS_DEVICE_COUNT = (int)BusDevice::Count;
inline constexpr int FIRST_BOARD = (int)BusDevice::M4;
const char* busDeviceName(BusDevice d);
inline bool isBoard(int d) { return d >= FIRST_BOARD && d < BUS_DEVICE_COUNT; }
inline constexpr uint32_t BOARD_MASK = ((1u << BUS_DEVICE_COUNT) - 1) & ~((1u << FIRST_BOARD) - 1);

struct ExpansionBusProbe {
    bool enabled = false;

    // the pins, since the window last took them (take())
    uint32_t samples = 0;                                  // microseconds sampled
    std::array<uint32_t, EXPANSION_PIN_COUNT> activeUs{};  // microseconds each pin was active
    std::array<uint32_t, EXPANSION_PIN_COUNT> edges{};     // times each pin went active
    std::array<bool, EXPANSION_PIN_COUNT> level{};         // its level at the last sample
    // a pulse shorter than a microsecond (an I/O cycle's /IORQ, an /NMI), seen at the next sample
    std::array<bool, EXPANSION_PIN_COUNT> pulse{};
    // the chips' own counts at the last sample (an NMI, a light-pen strobe, a reset, a write)
    uint32_t seenNmi = 0, seenLpen = 0, seenReset = 0, seenMemoryWrites = 0;

    // the last I/O cycle on the bus: A15..A0 and D7..D0 hold it
    int lastPort = -1, lastValue = 0, lastPc = 0;
    bool lastWrite = false;
    uint32_t lastDecoders = 0;                             // a bit per BusDevice

    // per device
    struct DeviceStats {
        uint32_t reads = 0, writes = 0;                    // since take()
        uint64_t totalReads = 0, totalWrites = 0;
        int lastPort = -1, lastValue = 0;
        bool lastWrite = false;
        uint64_t lastAtUs = 0;                             // the machine's microsecond
    };
    std::array<DeviceStats, BUS_DEVICE_COUNT> devices{};

    // the I/O log: the newest LOG_SIZE cycles
    static constexpr int LOG_SIZE = 256;
    struct Access {
        uint64_t atUs = 0;
        uint32_t decoders = 0;
        uint16_t port = 0, pc = 0;
        uint8_t value = 0;
        bool write = false;
    };
    std::array<Access, LOG_SIZE> log{};
    uint64_t logCount = 0;
    bool logExpansionOnly = true;                          // only &F8xx-&FFxx (the boards' space)

    // stop the CPC when one of these devices is accessed (a bit per BusDevice)
    uint32_t breakOn = 0;

    // the window took the counts: start again
    void take() {
        samples = 0;
        activeUs.fill(0);
        edges.fill(0);
        for (DeviceStats& d : devices) d.reads = d.writes = 0;
    }
    void clearLog() { logCount = 0; }
    // a pin's state for this microsecond
    void set(int pinNumber, bool active) {
        const int i = pinNumber - 1;
        const bool on = active || pulse[(std::size_t)i];
        pulse[(std::size_t)i] = false;
        if (on) activeUs[(std::size_t)i] += 1;
        if (on && !level[(std::size_t)i]) edges[(std::size_t)i] += 1;
        level[(std::size_t)i] = on;
    }
    void strobe(int pinNumber) { pulse[(std::size_t)(pinNumber - 1)] = true; }
};

} // namespace cpcse
