<p align="center"><img src="images/logo.png" alt="CPCSyntaxError" width="600"></p>

# CPCSyntaxError

An Amstrad **CPC 464 / 6128, CPC Plus and GX4000** emulator for Windows, Linux and macOS,
built around a cycle-level model of the machine's chips -- the video chips above all.

> **Work in progress.** The video chips, the Z80 and the CPC's other chips are the mature
> part; the user interface and some expansions are still being worked on -- see
> [Project status](#project-status).

- [Screenshots](#screenshots)
- [Features](#features)
- [Getting started](#getting-started) · [Keys](#keys) · [Command line](#command-line)
- [External debugging](#external-debugging) · [CSL scripts and SSM screenshots](#csl-scripts-and-ssm-screenshots)
- [Building from source](#building-from-source) · [Source layout](#source-layout)
- [Project status](#project-status) · [Known issues](#known-issues) · [Credits and licence](#credits-and-licence)

## Screenshots

| | |
|---|---|
| ![Batman Forever on a CPC 6128](images/6128-BatmanForever.png) | ![Pinball Dreams on a CPC 6128](images/6128-PinballDreams.png) |
| *Batman Forever -- CPC 6128* | *Pinball Dreams -- CPC 6128* |
| ![Alcon 2020 on a GX4000](images/GX4000-ALCON.png) | ![Sonic the Hedgehog on a GX4000](images/GX4000-SONIC.png) |
| *Alcon 2020 -- GX4000* | *Sonic the Hedgehog -- GX4000* |
| ![The M4 board's SD card and WiFi](images/M4BoardWiFi.png) | ![The dot-matrix printer](images/dotmatrix.png) |
| *The M4 board: its SD card, and `\|NETSTAT` on your PC's network* | *The dot-matrix printer, and the expansions* |
| ![SymbOS and the memory map](images/memory%20map.png) | |
| *SymbOS on a 6128, with the memory map recording what the Z80 does* | |

## Features

### The machine, cycle by cycle

- **All five CRTC types** (0 HD6845S/UM6845, 1 UM6845R, 2 MC6845, 3 and 4 the Plus /
  cost-down ASICs), each with its own rule set taken from Longshot's *Amstrad CPC CRTC
  Compendium* and checked against his SHAKER test suite and photographs of real machines.
- **Gate Array** models 40007 / 40008 / 40010 and the ASICs 40226 / 40489, with per-model
  pixel timing (**Machine → Gate Array**), and a **monitor** model (CTM 640/644, CM14,
  GT 64/65, MM12) that locks to the composite sync the Gate Array really produces.
- **The Z80** passes ZEXDOC, ZEXALL and every test of Patrik Rak's z80test 1.2a -- the
  undocumented flags, MEMPTR, the Zilog SCF/CCF behaviour, interrupted block instructions.
- **The PSG, PPI, disc controller and tape** follow their data sheets and formats: the
  AY-3-8912, the 8255, NEC's uPD765A as Amstrad wired it, TZX 1.20.
- **CPC Plus ASIC**: sprites, DMA sound, the 4096-colour palette, raster interrupts, split
  screen, soft scroll.
- Machines: CPC 464, 6128, 464 Plus, 6128 Plus and GX4000; RAM expansions up to 4 MB (in
  512K steps past 576K); joysticks and game controllers, the Plus analogue port.

### Discs, tapes, cartridges, snapshots

- **Discs**: DSK and extended DSK, **HFE** (the Gotek's format) and **IPF**. What a program
  writes to a disc goes back into its file, in the image's own format -- an HFE keeps every
  track the CPC did not touch bit for bit (Media: "Save disc and tape writes to their
  files"). IPF originals are write-protected.
- **The disc drive in real time**: the disc turns at 300 rpm under a 250 kbit/s head, so a
  sector's bytes come as it passes (where an HFE or IPF image puts it), seeks take the step
  rate the program sets, the drive is ready once the disc is up to speed, and a program too
  slow for the bytes gets an overrun -- as on a real CPC.
- **A tape deck** (Media window, Media → Tape): a counter with its reset, the time and the
  block the tape is at, **rewind, back a block, play, record, pause, stop, fast forward,
  eject**, and the tape's blocks listed to jump to. CDT, TZX, TAP and WAV.
  - **Record**: what the CPC SAVEs goes onto the tape from where it is, or onto a **new blank
    tape** -- as standard turbo blocks (TZX &11) when it is the firmware's format, as the raw
    signal otherwise. **Save tape as** writes a .cdt; a recording on a .cdt goes back into it.
  - **Turbo**: while the tape moves the whole CPC runs faster -- up to 20x, or as fast as your
    computer goes -- so every loader, protected ones too, works as it always does.
- Cartridges (CPR) and snapshots (SNA), each with an **Eject**; drop any of them on the window.

### Cheats

- **Tools → Cheats** finds where a game keeps its lives, ammo, time or energy -- with a
  **walkthrough** that asks only what you can see: what the number on the screen says now,
  and again after it changes (or, for a bar, whether it went down, up or stayed). Then it
  holds it where you want it and asks whether it worked, trying the places one at a time
  if need be.
- It searches every way games keep such numbers at once -- as they are, one less, or BCD --
  in all of the RAM, 128K banks included. A **search** tab does the same by hand (8 or 16
  bits, with undo), and **POKEs** from a magazine can be typed in.
- A game's cheats are kept beside it as `<game>.pok`, and load (switched off) with it.

### Expansions

All in **Expansions** and the Settings window:

- **M4 board**: a folder on your PC is its SD card, and the M4's own ROM (`M4ROM.ROM`, by
  Duke, [spinpoint.org](http://www.spinpoint.org) -- included with his permission) runs
  unmodified. Files and long names from BASIC (`|CD`, `CAT`, `|LS`, `|ERA`, `|REN`,
  `|COPYF`...); **DSK images on the card open as folders** (`|CD,"game.dsk"`, then `CAT`,
  `LOAD`, `RUN`); **SymbOS** reads and writes the card sector by sector. Its **WiFi** is your
  PC's internet connection: TCP sockets, name lookups and servers (Duke's telnet and TCP
  examples work), `|HTTPGET` / `|HTTPMEM` (plain HTTP, as the real board), `|NETSTAT`.
- **Multiface II**: F10 is its STOP button; its menu, saving and reloading work. Its ROM
  (Romantic Robot's) is not included: put a file with "multiface" in its name in `roms/`.
- **RS232C serial interface** (Amstrad's and Pace's: a Z80 DART and an 8253 at
  `&FADC` / `&FBDC`): its line is a TCP port other programs connect to (`telnet localhost
  2323`), a connection out (a BBS, a telnet server), one of your PC's serial ports, or a
  loopback plug. Its ROM is not included -- fit it in a ROM slot.
- **Symbiface II / III**: mouse, clock and the **IDE/CF interface** -- its drive is a folder
  on your PC (`symide` beside the program, or one you choose), one FAT16 partition SymbOS
  reads and writes.
- **GFX9000** (Yamaha V9990 at `&FF60`): every screen mode, sprites, cursors, the blitter
  taking its real time with the chip's **/WAIT** on the bus, raster timing and interrupts.
  Its monitor sits beside the CPC's, in its own window, or shares one -- switched, or
  through an emulated **Video9000** that superimposes it on the CPC's picture.
- **OPL4 sound card** (YMF278B, MoonSound-style, on the AMSDAP at `&FFC4` / `&FF7E`): 18 FM
  and 24 wavetable channels, for SymbOS's sound daemon and SymAmp. The General MIDI samples
  need Yamaha's YRW801 ROM (not included): turning the card on without it asks for it.
- **PlayCity** (TotO's two YMZ294 and a Z80 CTC at `&F880`-`&F988`): six more AY channels
  in stereo, the CTC's clock, raster NMIs and IM2 timer interrupts.
- **Speech**: the Amstrad **SSA-1** and **dk'tronics** (the SP0256-AL2, MAME's core; its ROM
  is not included -- the emulator asks for it), and **LambdaSpeak 3**: its Epson and DECtalk
  text-to-speech spoken with your computer's own voices (Windows' voices, `say` on macOS,
  `espeak-ng` on Linux), and its serial **MP3 module** playing MP3 files from a folder.
- Lightguns (Trojan Light Phazer, Gunstick, West Phaser), a text or **dot-matrix printer**
  on the printer port (its pages saved as BMP or SVG), DigiBlaster / AmDrum.
- **Drive and keyboard sounds**: the drive's motor, head steps, insert and eject, and key
  clicks, from recordings in `sounds/` -- replace them with your own WAV files.

### Tools

- **Debugger** (Debug menu): run / step into / over / out, registers, flags and stack beside
  the disassembly (with labels), breakpoints with conditions, memory watchpoints.
  - **Chips**: live views of the CRTC, Gate Array, monitor, Plus ASIC, PSG, PPI, keyboard
    matrix, disc controller and tape.
  - **Memory**: a hex editor, and a **memory map** of any 64K of RAM, a pixel a byte,
    coloured by what the Z80 did there -- opcode fetch, operand, read, write.
  - **GFX9000**: its picture, registers, palette, command engine and VRAM.
- **DSK editor** (Tools, or Media → Drive → Edit): new discs in any CPC format (DATA,
  SYSTEM, IBM, 42/80-track, ParaDOS, ROMDOS, Vortex, Dobbertin, or a geometry of your own),
  standard and extended images, or the disc in a drive while the CPC uses it. Files: import,
  export, delete, rename, user, flags, a look as hex, BASIC, text or Z80. Tracks and
  sectors: IDs, status bytes, weak copies, bytes in a hex editor. A map of each block's owner.
- **Assembler**: [RASM](https://github.com/EdouardBERGE/rasm) itself, linked in. Assemble
  straight into the running machine (F9) and run it (Ctrl+F9), with syntax colours you can
  change.
- **External debugging and code loading**: a **GDB server** for VS Code's
  [DeZog](https://github.com/maziac/DeZog), a **command API** with the `cpcse-ctl` client,
  and **auto-reload** of your build. See [External debugging](#external-debugging).
- **CSL scripts and SSM screenshots** (Longshot's CPC Script Language 1.5 and ScreenShot
  Management 1.1): `cpcse.exe --csl <script>` plays a script with no window -- which is how
  SHAKER's own scripts drive every test. See [below](#csl-scripts-and-ssm-screenshots).

### The interface

- Dockable, resizable windows (Dear ImGui docking branch); any window can be pulled out of
  the main window into its own. Only the screen, the machine and its media are open at
  first; the rest opens from the menus and docks with its group. **Window → Interface
  size** makes everything larger (90-200%).
- The file picker switches drives (C:, D: ... on Windows) and goes to a folder typed into
  it; the Save dialog browses the same way. Files with any name open -- accented, CJK,
  emoji (Windows 10 1903 or later).

## Getting started

1. Download `CPCSyntaxError-win64-<version>.zip` from the [Releases](../../releases) page,
   unzip it anywhere and run `cpcse.exe` -- or build it yourself (below; Windows, Linux and
   macOS).
2. The CPC 464 and 6128 firmware ROMs and the CPC Plus / GX4000 system cartridge are
   included, in `roms/` (Amstrad have kindly given their permission for the redistribution
   of their copyrighted material but retain that copyright). `cpcse.exe` looks for a `roms`
   folder beside it; another can be chosen with **File → ROM folder…** or
   `cpcse.exe --roms <folder>`. Files are found by name (case does not matter):

   | For | Files needed (name must contain) |
   |---|---|
   | CPC 464 | `464 OS`, `464 BASIC` |
   | CPC 6128 | `6128 OS`, `6128 BASIC`, `AMSDOS` |
   | 464 Plus / 6128 Plus / GX4000 | the Plus system cartridge, a `.cpr` whose name contains `burning rubber` |
   | M4 board | `M4ROM` (included) |
   | Multiface II | `multiface` (not included) |
   | OPL4 card's GM samples | `yrw801` (not included -- the emulator asks for it) |
   | SSA-1 / dk'tronics / LambdaSpeak 3 speech | `sp0256-al2.bin`, 2 KB (not included -- the emulator asks for it) |

3. The first start boots a CPC 6128. Pick another machine from **Machine → Model**, and load
   software from the **File** or **Media** menus, or drop a file onto the window.

Settings are saved to `cpcse.ini`, and the window layout to `cpcse_layout.ini`, beside the
executable. **Window → Reset layout** restores the default arrangement.

## Keys

| Key | Action |
|---|---|
| F11 | fullscreen (or double-click the picture) |
| Ctrl+R | reset |
| Ctrl+P | pause / run |
| F10 | the Multiface II's STOP button |
| F5 | run / pause (in a debugger window, or while paused) |
| F7 / F8 / Shift+F8 | step into / over / out |
| F9 / Ctrl+F9 | assemble / assemble and run (assembler window) |
| F12 / middle button | release the mouse (a click on the picture gives it to the CPC's Symbiface mouse) |

Keys go to the CPC unless a debugger or assembler window has the focus.

## Command line

`cpcse.exe --roms <folder>` starts with another ROM folder. `--gdb`, `--api`, `--load`,
`--watch`, `--run`, `--reset`, `--command` and `--symbols` are described under
[External debugging](#external-debugging). A headless screenshot mode is also available for
scripting:

```
cpcse.exe --shot out.bmp --model cpc6128 --frames 200 [--disk game.dsk] [--type "RUN\"GAME\n"]
          [--cart file.cpr] [--tape file.cdt] [--sna file.sna] [--savesna out.sna]
          [--crtc 0-4] [--gate-array 40007|40008|40010] [--beam] [--ram 64-4160]
          [--disk-writeback] [--record-tape out.cdt]
          [--m4 <folder>] [--multiface] [--sf2] [--sf3] [--ide <folder>] [--xrom <slot>=<rom file>]
          [--serial tcp:<host>:<port>|listen:<port>|com:<port>|loopback]
          [--gfx9000] [--v9990-shot gfx.bmp] [--video9000-shot mixed.bmp]
          [--opl4] [--playcity] [--speech ssa1|dktronics|lambdaspeak3] [--mp3card <folder>]
          [--dac digiblaster|amdrum] [--wav out.wav]
          [--mouse "w120;j5,60;j16,2;tDIR;kEnter;s<file.bmp>"]
```

- `--type` types its text once the machine has started; `\n` is Enter.
- `--disk-writeback` saves what the program writes to the disc into its file (off in
  headless runs, on in the window). `--record-tape` puts a blank tape in with REC down and
  saves it to that file at the end.
- `--mouse` is a small script run after everything else: `w`ait frames, `m`ove the mouse,
  `c`lick, `d`ouble-click, hold the `j`oystick (`j<bits>,<frames>`), `t`ype text, tap a
  `k`ey, `s`ave a screenshot -- enough to drive a desktop such as SymbOS with no window.

Models: `cpc464`, `cpc6128`, `cpc464plus`, `cpc6128plus`, `gx4000`.

The headless modes print to the Command Prompt they were started from. `cpcse.exe` is a
windowed program, so the prompt does not wait for it: use `start /wait cpcse.exe ...` in a
batch file that needs the result.

## External debugging

Work in VS Code (or any editor) and let the emulator follow your build. All of this is off
until you turn it on: *Settings → External debugging*, or the command line. The servers
listen on `127.0.0.1` only.

- **GDB server** (`--gdb`, port 12000): DeZog's `mame` remote attaches, for source-level
  debugging with breakpoints, stepping, watchpoints, and memory and register views.
- **Command API** (`--api`, port 6128) and `cpcse-ctl` (beside `cpcse.exe`): load code,
  type, read and write memory, set breakpoints, wait for a stop, take screenshots.
- **Auto-reload** (`--watch build/game.bin@0x4000 --run entry`): every build is written back
  into memory and started again.

```
cpcse.exe --gdb --api --watch build/game.bin@0x4000 --run entry --symbols build/game.sym
cpcse-ctl load build/game.bin 0x4000 entry
```

[docs/external-debugging.md](docs/external-debugging.md) has the DeZog `launch.json`, a build
task, every option and every command.

## CSL scripts and SSM screenshots

`cpcse.exe --csl <script>` plays a CPC Script Language file with no window, writing the
screenshots the emulated program requests with SSM codes (the Z80 bytes `ED LL ED HH`) as
`CPCSE_<crtc>_<HHLL>.bmp` -- the naming the SSM standard suggests for the SHAKER portal.
Nothing of it runs unless `--csl` is given.

```
cpcse.exe --csl SHAKE27A-1.CSL --out shots --disk-dir <folder with shaker27.dsk>
```

| Option | |
|---|---|
| `--out <dir>` | screenshots, snapshots and `csl.log` (default `screenshots`) |
| `--roms <dir>` | firmware and the Plus system cartridge (default `roms`) |
| `--disk-dir`, `--tape-dir <dir>` | where to look for the media a script names |
| `--model <id>` | the machine for a script with no `cpc_model` (default `cpc6128`) |
| `--crtc <n>` | override every `crtc_select` (0-4) |
| `--emu-name <name>` | the image name prefix (default `CPCSE`) |
| `--csl-log <file>` | the log of the run (default `<out>/csl.log`) |
| `--no-chain` | do not follow `csl_load` |
| `--no-errata` | play published scripts exactly as written (see below) |

Every CSL 1.5 instruction is handled, including `wait_ssm 0xHHHH`, which holds the script
until the program sends that SSM code (and its screenshot is written) instead of waiting a
fixed time. Also machine configuration (`cpc_model`, `crtc_select` 0/1/1A/1B/2/3/4,
`gate_array`, `memory_exp`, `rom_dir`, `rom_config`), `reset soft|hard`, disc, tape and
snapshot media and folders, keys (`\(...)` codes, `{groups}`, `\(KOF)`, `key_from_file`,
`keyboard_write`), the four waits, screenshots and snapshots (versions 1-3) with their names
and folders, and `csl_load`. `crtc_select 3` with no model builds a 6128 Plus (the CRTC 3 is
its ASIC) and takes it from its menu to BASIC before the script starts. SSM `#0000` ends a
`wait_ssm0000`, `#FFFE` saves the named screenshot, `#FFFF` a snapshot. Anything the machine
cannot do stops the script with the standard's report: script, line, instruction, reason,
the script's CSL version and the supported one.

**Erratum.** SHAKER 2.7 added a ninth sub-test to its test U ("R4 & R9 check") that the
SHAKE27A-*.CSL scripts were not updated for: the test now ends 14.2-14.7 s after its key, the
script moves on after 14 s, and the five AI screens that follow are never reached. The player
lengthens that one wait to 16 s and says so in the log; `--no-errata` turns it off.

## Building from source

### Windows

Requirements: a MinGW-w64 GCC with C++17 (GCC 10 or later), CMake 3.16+, Ninja (optional),
and the **SDL2 MinGW development package** (`SDL2-devel-2.x.x-mingw.zip` from
<https://github.com/libsdl-org/SDL/releases>).

```
cmake -S . -B build -G Ninja -DSDL2_ROOT=C:/path/to/SDL2-2.x.x/x86_64-w64-mingw32
cmake --build build
```

or unpack the SDL2 package's `x86_64-w64-mingw32` folder into `third_party/SDL2` and run
`build.bat`. The result is `build/cpcse.exe` (and `build/cpcse-ctl.exe`), with `SDL2.dll`
and the `roms` folder copied beside it; the GCC runtime is linked statically, so nothing
else is needed to run it.

### Linux

Requirements: GCC 10+ (or Clang), CMake 3.16+, Ninja (optional), pkg-config, and the SDL2
and OpenGL development packages. On Debian / Ubuntu:

```
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev libgl-dev
cmake -S . -B build -G Ninja
cmake --build build
./build/cpcse
```

The result is `build/cpcse`, with the `roms` and `sounds` folders copied beside it. Run it
from that folder, or pass `--roms <folder>`; drive and keyboard sounds are looked for in
`sounds/` beside the executable, then in the working directory. On Linux the GUI runs CSL
scripts with `fork`/`exec`, opens folders with `xdg-open`, and downloads ROMs with `curl`
(or `wget`). Paths in Windows-authored CSL scripts are matched case-insensitively.

### macOS

Requirements: Xcode's command-line tools (Apple Clang), CMake 3.16+ and SDL2, for example
from Homebrew. Apple silicon and Intel Macs build the same way:

```
xcode-select --install
brew install cmake ninja sdl2
cmake -S . -B build -G Ninja
cmake --build build
./build/cpcse
```

The interface runs on OpenGL 3.2 (Core Profile), which every Mac since 2011 has. As on
Linux, the `roms` and `sounds` folders are copied beside `build/cpcse`; folders open with
`open`. The macOS build has been reported working by users but is not tested here.

## Source layout

```
src/core/      the emulation core (static library, no host dependencies)
src/gui/       the desktop front end: SDL2 + OpenGL + Dear ImGui, debugger, assembler,
               cheats, the GDB server and command API
tools/ctl/     cpcse-ctl, the command API's client
docs/          external-debugging.md
roms/          Amstrad firmware, the Plus system cartridge and the M4 ROM (see roms/README.txt)
sounds/        drive and keyboard recordings
third_party/   Dear ImGui (docking branch), RASM, ymfm, the SP0256 core, minimp3, the SPS
               decoder library (IPF; non-commercial licence) and the DejaVu Sans Mono font,
               vendored; SDL2 is fetched separately
```

## Project status

| Area | State |
|---|---|
| CRTC types 0-4, Gate Array, monitor | Mature: rule sets from the Compendium, cross-checked by independent reference models and SHAKER |
| Z80 | Mature: passes ZEXDOC, ZEXALL and all of z80test 1.2a |
| PSG, PPI, disc controller, tape | Mature: checked against their data sheets and formats; disc timing and tape SAVE checked against the CPC's own firmware |
| CPC Plus ASIC | Working; its picture goes through the same monitor model as a CPC's |
| Disc images | DSK / extended DSK read and written; HFE read and written back; IPF read (the SPS library) |
| Cheats | Working: the walkthrough and the search checked on a running CPC |
| CSL / SSM | Complete (CSL 1.5, SSM 1.1) |
| DSK editor | Working; its filesystem checked against AMSDOS itself (LOAD, SAVE, ERA, read-only) |
| User interface | Every option exposed, grouped by Machine / Media / Video / Audio / Input / Expansions / Tools / Debug; still being refined |
| M4 board | Working: files, DSK images and raw SD sectors over a host folder, checked against the M4's own ROM source; WiFi through your PC's network |
| Multiface II | Working: STOP, its menu, save and reload |
| Symbiface II / III | Working: mouse, RTC and the IDE/CF interface, checked against the ATA standard and with SymbOS |
| GFX9000 (V9990) + Video9000 | Working, from Yamaha's application manual and tests of a real V9990 on a CPC; the blitter at openMSX's measured speeds |
| OPL4 (YMF278B) | Working (ymfm core); needs `yrw801*.rom` for the General MIDI samples |
| SSA-1 / dk'tronics speech | Working (MAME's SP0256 core); needs `sp0256-al2.bin` |
| LambdaSpeak 3 | Working: Epson and DECtalk modes (with your computer's voices), the SSA-1 / dk'tronics modes, the MP3 module. Not yet: SSA-1 speech without an SP0256 ROM, the EEPROM sampler, its Amdrum mode |
| DigiBlaster / AmDrum DACs | Working |
| Drive / keyboard sounds | Working, from recordings (an Amiga 600 drive and a PC keyboard -- drop your own in `sounds/`) |
| RS232C serial interface | Working: checked against the chips' data sheets and with a CPC talking over TCP; not yet tried with the card's own ROM |
| PlayCity | Working; its NMI / IM2 timers follow the Z80 CTC manual but are not yet tested with PlayCity software |
| Lightguns, printers | Working, lightly tested |

## Known issues

- **C-HSYNC width on CRTC 0 and 2.** The Gate Array's monitor sync on these chips is a
  fraction of a microsecond longer than the Compendium's table (§14.4, p.134). The
  documented width is not used yet because the monitor's recovery after a short sync is not
  modelled, and without that model it makes some pictures worse.
- **SymAmp under SymbOS**: SA2 songs written for 60/70 Hz that set their speed in the
  pattern play about 1.4x too slowly. This is SymAmp's own player, not the emulation: a real
  CPC plays them the same. MP3 playback is not available (it needs an MSX MP3 cartridge,
  which is not emulated).
- **GFX9000**: the chip's intermittent command faults that real-hardware tests report (a
  command reusing DY or NX/NY may go astray or hang) are not reproduced; LMMV's colour order
  with DIX=1 is approximate.

## Credits and licence

CPCSyntaxError is released under the MIT licence (see `LICENSE`). The third-party components
it contains keep their own licences -- see `THIRD_PARTY_NOTICES.md`. Note that the built-in
assembler includes the Exomizer cruncher, and IPF images are read with the SPS decoder
library: both licences permit **non-commercial use only**. The Amstrad firmware and system
cartridge in `roms/` are not covered by the MIT licence (see `roms/README.txt`).

CRTC behaviour is sourced from the **"Amstrad CPC CRTC Compendium" by Longshot** (CC BY-NC-ND
4.0) and checked against his SHAKER test suite; the CSL and SSM formats are his standards
too. The built-in assembler is **RASM by Edouard BERGE**. The M4 ROM is **Duke's**. IPF
images are read with the **SPS decoder library** (KryoFlux Products & Services). The user
interface uses **Dear ImGui** by Omar Cornut and **SDL2**.

Amstrad CPC, CPC Plus and GX4000 are trademarks of their respective owners. This project is
not affiliated with Amstrad.
