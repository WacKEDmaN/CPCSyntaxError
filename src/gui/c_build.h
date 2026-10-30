// CPCSyntaxError GUI — C programs for the CPC: SDCC, with CPCtelera's library, run from the
// assembler window.
//
// SDCC is GPL, so it is not linked in: it stays its own programs (sdcc, sdasz80, sdar) in a
// folder the user names, and this file runs them. CPCtelera (its "next" branch,
// github.com/Arnaud6128/cpctelera/tree/cpctelera_next: SDCC 4.6.0, __sdcccall(1), LGPL v3)
// builds with bash, make and, on Windows, Cygwin; here the same commands are run directly,
// so a CPCtelera folder needs no setup at all:
//
//   library  every src/**/*.s assembled from the CPCtelera folder (the sources include
//            "src/macros/..."): sdasz80 -I<cpct>/src/ -l -o -s <obj>.rel <src>.s, then
//            sdar rc cpctelera.lib (its Makefile and cfg/build_config.mk). Built once into a
//            cache, again when SDCC or the sources change.
//   project  every .c and .s under the project's source folder (CPCtelera: SRCDIR of
//            cfg/build_config.mk, else the main file's folder):
//              sdcc -I<cpct>/src -I<srcdir> -mz80 <Z80CCFLAGS> -c x.c -o obj/x.rel
//              sdasz80 -l -o -s -I<cpct>/src obj/x.rel x.s
//            linked as the project template does:
//              sdcc -mz80 --no-std-crt0 -Wl-u --code-loc <Z80CODELOC> --data-loc 0
//                   -l<cpctelera.lib> <objects> -o obj/<project>.ihx
//   result   the Intel HEX's bytes from its lowest address (hex2bin -p 00: gaps are 0); the
//            program starts at _main, read from the link map (CPCtelera's GETRUNADDRESS).
//
// Without a CPCtelera folder the same steps build plain SDCC C, with no library.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cpcse {

struct CToolchain {
    // "sdcc": SDCC, with CPCtelera when its folder is set. "z88dk": z88dk's zcc +cpc (its
    // classic library; the firmware prints), every .c / .asm in the main file's folder, one
    // command: zcc +cpc [-compiler=sdcc] -lndos -m --list --c-code-in-asm -create-app
    // -zorg=<code address> -o obj/<name>.bin <files>. The program starts at the code address
    // (z88dk's crt0, which calls main). zsdcc writes ";file:line:" in its listings, so its
    // lines can be stepped; sccz80's markers come a line late, so it has no line table.
    std::string compiler = "sdcc";
    std::string z88dkFolder;         // z88dk's folder; "" = the usual places, then the PATH
    bool z88dkSccz80 = false;        // z88dk's own sccz80 instead of zsdcc
    std::string sdccFolder;          // SDCC's folder (or its bin); "" = look in the usual places
    std::string cpcteleraFolder;     // CPCtelera (the repository or its cpctelera folder); "" = none
    int codeLoc = 0x4000;            // when the project has no cfg/build_config.mk (CPCtelera's default)
    std::string extraFlags;          // added to every sdcc -c
    // A CPCtelera project's asset steps (cpct_assets.h) run before the compile, each when its
    // outputs are older than its inputs. forceAssets: all of them; assetsOnly: stop there.
    bool forceAssets = false, assetsOnly = false;
    // A CPCtelera project's TARGET may ask for a snapshot (.sna): a machine makes it. The
    // GUI passes one (a second machine of the kind it runs, so the shown one is not
    // touched); without it no .sna is written. Called on the build's thread.
    std::function<bool(const std::vector<uint8_t>& bytes, int load, int run, std::vector<uint8_t>& sna, std::string& why)> makeSnapshot;
};

struct CBuildMessage {
    std::string file;                // "" = not about a line of a file (the linker, a tool)
    int line = 0;
    std::string text;
    bool error = true;
};

// One run of addresses compiled from one C line (from the linker's .rst listings).
struct CSourceLine {
    int start = 0, end = 0;          // [start, end)
    std::string file;                // as SDCC named it (the path it was given)
    int line = 0;
};

struct CBuildResult {
    bool ok = false;
    std::vector<CBuildMessage> messages;
    std::vector<uint8_t> bytes;
    int loadAddress = -1, runAddress = -1;
    std::vector<std::pair<std::string, int>> symbols;   // from the link map
    std::string log;                 // every command and what it printed
    std::string binPath;             // the binary written beside the objects
    std::string project;             // the project's name
    // the images its TARGET asked for (cpct_assets.h, makeProjectMedia); "" = not made
    std::string dskPath, cdtPath, snaPath;
    bool discNeeded = false;         // the disc holds the program's data too: run with it in drive A
    std::vector<CSourceLine> lines;  // the C lines' addresses, for stepping by line
    // the bytes after each label in the listings (.ds N, or the bytes of .db / .dw lines):
    // a variable's size, for the editor's mouse-over; missing = not known
    std::map<std::string, int> symbolSizes;
};

// SDCC's bin folder: the folder given or its bin, else the usual installs, else the PATH.
// "" when no sdcc is found.
std::string findSdccBin(const std::string& folder);
// CPCtelera's own folder (the one holding src/cpctelera.h), from the repository or itself.
std::string findCpctelera(const std::string& folder);
// z88dk's folder (holding bin/zcc and lib/config): the folder given, else C:/z88dk, the
// folder beside this program's, else the zcc on the PATH. "" when none.
std::string findZ88dk(const std::string& folder);

// Build the project the main file belongs to. Runs the tools on threads; calls progress
// with a line now and then (from those threads). Safe to run off the UI thread.
CBuildResult buildC(const CToolchain& tools, const std::string& mainPath, const std::string& cacheFolder,
                    const std::function<void(const std::string&)>& progress);

// Runs a program (no window); its standard output and error together into `output`. The
// exit code, or -1 when it could not be started.
int runProgram(const std::vector<std::string>& args, const std::string& cwd, std::string& output);

// Exposed for the check: one tool line of output read as a message (false: not one).
bool parseToolMessage(const std::string& line, CBuildMessage& out);
// The bytes of an Intel HEX file from its lowest address; false when it is not one.
bool readIntelHex(const std::string& text, std::vector<uint8_t>& bytes, int& lowest);
// The C lines of one .rst listing (the linker's -u copy of SDCC's .lst, addresses final):
// SDCC puts ";<file>:<line>: <the C text>" before each line's code; each code line has its
// address in columns 4-11 and its bytes in columns 13-29. Runs are added to `out`.
void parseRstLines(const std::string& text, std::vector<CSourceLine>& out);
// The C lines of one z88dk listing (zcc --list --c-code-in-asm with zsdcc: file.c.lis):
// ";<file>:<line>: <C>" before each line's code; each code line "<n>  <offset>  <bytes>";
// offsets count from the start of SECTION code_compiler, at `base` once linked (the map
// gives a function's address). Other sections' lines are not code.
void parseLisLines(const std::string& text, int base, std::vector<CSourceLine>& out);
// The size of each labelled thing in an .rst (SDCC) or .lis (z88dk) listing: "label:" or
// "label::", then ".ds N" / "defs N" (N bytes), or lines of bytes up to the next label.
void parseListingSizes(const std::string& text, std::map<std::string, int>& out);

} // namespace cpcse
