// CPCSyntaxError — external development: the emulator driven from an editor, a debugger
// or a build script on the same computer. Three parts, all off until asked for:
//
//   GDB remote server (default port 12000)
//     The GDB remote serial protocol as MAME's gdbstub speaks it for a Z80, so DeZog in
//     VS Code attaches with its "mame" remote type: registers, memory, breakpoints
//     (bpset/bpclear and Z0/Z1), watchpoints (Z2 write, Z3 read, Z4 both), continue,
//     pause (Ctrl-C) and stop reports naming the PC or the watched address. A plain gdb
//     client works too (g/G/p/P/m/M/s/c/?).
//
//   Command API (default port 6128)
//     One JSON object per line in, one per line out: load a program (.bin at an address
//     or by its AMSDOS header, .sna, .dsk, .cpr, tapes), type on the keyboard, read and
//     write memory and registers, breakpoints, stepping, screenshots, waiting for a stop.
//     tools/ctl (cpcse-ctl) is its command-line client; docs/external-debugging.md lists
//     every command.
//
//   Watching a file
//     A build's output reloaded whenever it changes: a .bin poked back in and started, a
//     .sna or a cartridge reloaded, a .dsk re-inserted (and a command typed to run it).
//
// While a GDB client is attached and stopped, the machine is the client's to run: a load
// or a reload writes memory and sets the PC but leaves the start to the client's continue.
//
// Both servers listen on 127.0.0.1 only. Everything runs on the UI thread: poll() is
// called once per main-loop iteration and never blocks.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace cpcse {

class EmuHost;
class Debugger;

// What to load and what to do with it.
struct LoadRequest {
    std::string path;
    int address = -1;        // .bin: where it goes (-1: its AMSDOS header's load address)
    // .bin: start it afterwards at this address. RUN_ENTRY: the header's entry address
    // (or where it was loaded); NO_RUN: leave the PC alone.
    int run = NO_RUN;
    static constexpr int NO_RUN = -1, RUN_ENTRY = -2;
    int unit = 0;            // .dsk: drive A (0) or B (1)
    // Reset the machine first. The command, and a .bin's bytes, wait for the firmware to
    // start (100 frames), which would otherwise write over them.
    bool reset = false;
    std::string command;     // typed once it is in, "\n" for Enter: RUN"GAME\n
    std::string symbols;     // a symbol file to load with it (RASM, sjasmplus, pasmo, ...)
};

class DevServer {
public:
    DevServer(EmuHost& host, Debugger& debugger);
    ~DevServer();

    // Once per main-loop iteration, before the emulation runs its frames.
    void poll();

    // The servers. start*() returns false with the reason in *Error (a port in use).
    bool startGdb(int port);
    void stopGdb();
    bool gdbListening() const;
    bool gdbConnected() const;
    int gdbPort() const;
    std::string gdbError;

    bool startApi(int port);
    void stopApi();
    bool apiListening() const;
    int apiClients() const;
    int apiPort() const;
    std::string apiError;

    void loadWhenReady(const LoadRequest& request);
    // Reloads the request's file whenever it changes on disc (an empty path stops).
    void watch(const LoadRequest& request);
    const LoadRequest* watched() const;
    int reloads() const;
    // A symbol file into the debugger's labels; how many were read, -1 on failure.
    int loadSymbols(const std::string& path, std::string& error);
    // "&4000", "0x4000", "16384" or a label, as the debugger reads them; -1 if none.
    int address(const std::string& text) const;


    // The last thing that happened ("Reloaded game.bin", "DeZog connected"), for the UI.
    std::string lastEvent;

    // Settings (cpcse.ini): whether each server starts with the emulator, and its port.
    // The command line's --gdb/--api start them for one session without changing these.
    bool gdbAtStart = false, apiAtStart = false;
    int gdbPortSetting = 12000, apiPortSetting = 6128;
    void loadSettings(const std::map<std::string, std::string>& ini);
    void saveSettings(std::ostream& out) const;
    void startFromSettings();

    std::function<void()> onQuit;   // the API's "quit"

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// The command-line options main() hands over: --gdb[=port] --api[=port]
// --load file[@addr] --watch file[@addr] --run entry|addr --reset --command text
// --symbols file (addresses may be labels from the symbols). Returns false (with a
// message) for a malformed one; unrelated arguments are skipped.
bool applyDevArguments(DevServer& dev, int argc, char** argv, std::string& error);

} // namespace cpcse
