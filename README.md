<p align="center"><img src="images/logo.png" alt="CPCSyntaxError" width="600"></p>

# CPCSyntaxError

An Amstrad **CPC 464 / 6128, CPC Plus and GX4000** emulator for Windows and Linux,
built around a cycle-level model of the machine's video chips.

> **Work in progress.** The CRTC, Gate Array and monitor emulation is the mature part
> of the project. The user interface and most expansion hardware are still being
> worked on -- see [Project status](#project-status).

- **All five CRTC types** (0 HD6845S/UM6845, 1 UM6845R, 2 MC6845, 3 and 4 the
  Plus/cost-down ASICs), each with its own rule set taken from Longshot's
  *Amstrad CPC CRTC Compendium* and checked against his SHAKER test suite and
  photographs of real machines.
- **Gate Array** models 40007 / 40008 / 40010 and the ASICs 40226 / 40489,
  with per-model pixel timing (chosen in **Machine → Gate Array**), and a **monitor**
  model (CTM 640/644, CM14, GT 64/65, MM12) that locks to the composite sync the Gate
  Array really produces.
- **CPC Plus ASIC**: sprites, DMA sound, 4096-colour palette, raster interrupts,
  split screen, soft scroll.
- Disc (DSK/EDSK), tape (CDT/TZX/WAV), cartridge (CPR) and snapshot (SNA) loading,
  each with an **Eject**; joysticks and game controllers; RAM expansions up to 4 MB (in
  512K steps past 576K).
- A **tape deck** (Media window, Media > Tape, Debug > Chips > Tape): a tape counter
  with its reset, the time and block the tape is at, **rewind to the start, back a
  block, play, pause, stop, fast forward, eject**, and a list of the tape's blocks to
  jump to.
- **Expansions** (all in **Expansions** / the Settings window):
  - **M4 board**: a folder on your PC is its SD card. The M4's own ROM (`M4ROM.ROM`, by Duke,
    [spinpoint.org](http://www.spinpoint.org) -- included in `roms/` with his permission) runs unmodified;
    files, directories and long names work from BASIC (`|CD`, `CAT`, `|LS`, `|ERA`,
    `|REN`, `|COPYF`...), and programs with their own file system -- **SymbOS** -- read and
    write the card sector by sector, their changes written back to the folder.
  - **GFX9000** (Yamaha V9990 at `&FF60`): every screen mode, sprites, cursors, the
    blitter commands -- taking their real time, with the chip's **/WAIT** holding the Z80
    while it is busy -- raster timing and interrupts, and the behaviour measured on a
    real V9990 on a CPC (the Powergraph notes). Its own monitor sits **beside the
    CPC's**, in its own window, or shares one monitor -- switched, or through an emulated
    **Video9000** that superimposes it on the CPC's picture.
  - **OPL4 sound card** (Yamaha YMF278B, MoonSound-style, on the AMSDAP at
    `&FFC4`/`&FF7E`): 18 FM + 24 wavetable channels, for SymbOS's sound daemon and SymAmp.
    The General MIDI samples need Yamaha's YRW801 ROM (not included): turning the card on
    without it opens a prompt to download it from a URL or choose a copy, saved to `roms/`.
  - **PlayCity** (TotO's two YMZ294 + Z80 CTC at `&F880`-`&F988`): six more AY channels
    in stereo, the CTC's programmable clock, raster NMIs and IM2 timer interrupts.
  - **Speech synthesisers**: the Amstrad **SSA-1** (`&FBEE`) and **dk'tronics** (`&FBFE`),
    both General Instrument's SP0256-AL2 (MAME's SP0256 core), and **LambdaSpeak 3** with its
    serial **MP3 module** (a Catalex YX5300): MP3 files from a folder on your PC, laid out
    as its micro-SD card (`01/001xxx.mp3`...). The SP0256-AL2's own ROM is not included:
    choosing a speech board without it opens a prompt to download it or choose a copy.
  - Symbiface II/III mouse and clock, lightguns (Trojan Light Phazer, Gunstick,
    West Phaser), a printer or dot-matrix printer on the printer port, DigiBlaster /
    AmDrum, the Plus analogue port.
- **Disc drive and keyboard sounds** (Audio): the drive's motor, head steps, disc insert and
  eject, and key clicks, played from real recordings in `sounds/` -- replace them with your
  own WAV files (`sounds/CREDITS.txt` lists the names).
- A **debugger** (Debug menu), in a few windows grouped by purpose:
  - **Debugger**: run / step into / over / out, the Z80's registers, flags and stack
    beside the disassembly (with labels), breakpoints with conditions and memory
    watchpoints below.
  - **Chips**: live views of the CRTC, Gate Array, monitor, Plus ASIC, PSG, PPI,
    keyboard matrix, disc controller and tape, a tab each.
  - **Memory**: a hex editor, and a **memory map** of any 64K of RAM (the base 64K or an
    expansion bank), a pixel a byte, coloured by what the Z80 did there -- opcode fetch,
    operand, read, write.
  - **GFX9000**: its picture, and the V9990's state, registers, palette, command
    engine and VRAM -- as bytes, or as an image coloured as the display colours it
    (P1's layers A and B side by side).
- A **DSK editor** (Tools, or Media > Drive > Edit): make new discs in any CPC format
  (DATA, SYSTEM, IBM, 42/80-track, ParaDOS, ROMDOS, Vortex, Dobbertin or a geometry of
  your own), open standard and extended images, or work on the disc in a drive while
  the CPC uses it. Files: import (with an AMSDOS header if wanted), export, delete,
  rename, user, read-only / system flags, and a look as hex, BASIC listing, text or Z80.
  Tracks and sectors: every sector's ID, status bytes, weak copies and bytes (hex
  editor), sectors added and removed, tracks formatted. A map of who owns each block.
- **CSL scripts and SSM screenshots** (Longshot's CPC Script Language 1.5 and ScreenShot
  Management 1.1): `cpcse.exe --csl <script>` plays a script with no window and saves the
  screenshots the running program asks for -- which is how SHAKER's own scripts drive
  every test. See [CSL scripts](#csl-scripts-and-ssm-screenshots).
- A built-in **assembler**: [RASM](https://github.com/EdouardBERGE/rasm) itself,
  linked in. Assemble straight into the running machine's memory (F9) and run it
  (Ctrl+F9). The editor colours the syntax -- instructions, registers, numbers,
  strings, labels, directives, comments -- in colours you can change (**Colours...**).
- **External debugging and code loading**: a **GDB server** for VS Code's
  [DeZog](https://github.com/maziac/DeZog) (source-level breakpoints, stepping,
  watchpoints), a **command API** with the `cpcse-ctl` client for build scripts (load,
  run, type, read/write memory, breakpoints, screenshots), and **auto-reload** of your
  build every time it changes. See [External debugging](#external-debugging).
- Dockable, resizable windows (Dear ImGui docking branch); any window can be pulled
  out of the main window into its own. Only the screen, the machine and its media are
  open at first; the rest opens from the menus and docks with its group. Window >
  Interface size makes everything larger (90-200%). Button rows and text wrap to fit a
  narrow window.

## Screenshots

| | |
|---|---|
| ![Batman Forever on a CPC 6128](images/6128-BatmanForever.png) | ![Pinball Dreams on a CPC 6128](images/6128-PinballDreams.png) |
| *Batman Forever -- CPC 6128* | *Pinball Dreams -- CPC 6128* |
| ![Alcon 2020 on a GX4000](images/GX4000-ALCON.png) | ![Sonic the Hedgehog on a GX4000](images/GX4000-SONIC.png) |
| *Alcon 2020 -- GX4000* | *Sonic the Hedgehog -- GX4000* |

## Getting started

1. Download `CPCSyntaxError-win64.zip` from the
   [Releases](../../releases) page, unzip it anywhere and run `cpcse.exe` -- or build
   it yourself (below; Windows and Linux).
2. The CPC 464 and 6128 firmware ROMs and the CPC Plus / GX4000 system cartridge are
   included, in `roms/` (Amstrad have kindly given their permission for the
   redistribution of their copyrighted material but retain that copyright). The release
   zip has them beside `cpcse.exe`, and a source build copies them there. `cpcse.exe`
   looks for a `roms` folder in its working directory; another can be chosen with
   **File → ROM folder…** or `cpcse.exe --roms <folder>`. Files are found by name
   (case does not matter):

   | Machine | Files needed (name must contain) |
   |---|---|
   | CPC 464 | `464 OS`, `464 BASIC` |
   | CPC 6128 | `6128 OS`, `6128 BASIC`, `AMSDOS` |
   | 464 Plus / 6128 Plus / GX4000 | the Plus system cartridge, a `.cpr` whose name contains `burning rubber` |
| M4 board | `M4ROM` (included) |
| OPL4 card's GM samples | `yrw801` (not included -- the emulator prompts for it) |
| SSA-1 / dk'tronics / LambdaSpeak 3 speech | `sp0256-al2.bin`, 2 KB (not included -- the emulator prompts for it) |

3. The first start boots a CPC 6128. Pick another machine from **Machine → Model**, and
   load software from the **File** or **Media** menus, or drop a file onto the window.

Settings are saved to `cpcse.ini`, and the window layout to `cpcse_layout.ini`, next
to the executable. **Window → Reset layout** restores the default arrangement.

## Keys

| Key | Action |
|---|---|
| F11 | fullscreen (or double-click the picture) |
| Ctrl+R | reset |
| Ctrl+P | pause / run |
| F5 | run / pause (in a debugger window, or while paused) |
| F7 / F8 / Shift+F8 | step into / over / out |
| F9 / Ctrl+F9 | assemble / assemble and run (assembler window) |
| F12 / middle button | release the mouse (a click on the picture gives it to the CPC's Symbiface mouse) |

Keys go to the CPC unless a debugger or assembler window has the focus.

## Command line

`cpcse.exe --roms <folder>` starts with another ROM folder. `--gdb`, `--api`, `--load`,
`--watch`, `--run`, `--reset`, `--command` and `--symbols` are described under
[External debugging](#external-debugging). A headless screenshot
mode is also available for scripting:

```
cpcse.exe --shot out.bmp --model cpc6128 --frames 200 [--disk game.dsk] [--type "RUN\"GAME\n"]
          [--cart file.cpr] [--tape file.cdt] [--sna file.sna] [--crtc 0-4]
          [--gate-array 40007|40008|40010] [--beam] [--ram 64-4160]
          [--m4 <folder>] [--gfx9000] [--v9990-shot gfx.bmp] [--video9000-shot mixed.bmp]
          [--opl4] [--playcity] [--speech ssa1|dktronics|lambdaspeak3] [--mp3card <folder>]
          [--dac digiblaster|amdrum] [--wav out.wav] [--sf2]
          [--mouse "w120;j5,60;j16,2;tDIR;kEnter;s<file.bmp>"]
```

`--mouse` is a small script run after everything else: `w`ait frames, `m`ove the mouse,
`c`lick, `d`ouble-click, hold the `j`oystick (`j<bits>,<frames>`), `t`ype text, tap a
`k`ey, `s`ave a screenshot -- enough to drive a desktop such as SymbOS with no window.

Models: `cpc464`, `cpc6128`, `cpc464plus`, `cpc6128plus`, `gx4000`.

The headless modes print to the Command Prompt they were started from. `cpcse.exe` is a
windowed program, so the prompt does not wait for it: use `start /wait cpcse.exe ...` in
a batch file that needs the result.

## External debugging

Work in VS Code (or any editor) and let the emulator follow your build. All of this is
off until you turn it on: *Settings -> External debugging*, or the command line. The
servers listen on `127.0.0.1` only.

- **GDB server** (`--gdb`, port 12000): DeZog's `mame` remote attaches, for source-level
  debugging with breakpoints, stepping, watchpoints, and memory and register views.
- **Command API** (`--api`, port 6128) and `cpcse-ctl` (beside `cpcse.exe`): load code,
  type, read and write memory, set breakpoints, wait for a stop, take screenshots.
- **Auto-reload** (`--watch build/game.bin@0x4000 --run entry`): every build is written
  back into memory and started again.

```
cpcse.exe --gdb --api --watch build/game.bin@0x4000 --run entry --symbols build/game.sym
cpcse-ctl load build/game.bin 0x4000 entry
```

[docs/external-debugging.md](docs/external-debugging.md) has the DeZog `launch.json`, a
build task, every option and every command.

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
fixed time. Also machine configuration (`cpc_model`, `crtc_select`
0/1/1A/1B/2/3/4, `gate_array`, `memory_exp`, `rom_dir`, `rom_config`), `reset soft|hard`,
disc, tape and snapshot media and folders, keys (`\(...)` codes, `{groups}`, `\(KOF)`,
`key_from_file`, `keyboard_write`), the four waits, screenshots and snapshots (versions
1-3) with their names and folders, and `csl_load`. `crtc_select 3` with no model builds a
6128 Plus (the CRTC 3 is its ASIC) and takes it from its menu to BASIC before the script
starts. SSM `#0000` ends a `wait_ssm0000`, `#FFFE` saves the named screenshot, `#FFFF` a
snapshot. Anything the machine cannot do stops the script with the standard's report:
script, line, instruction, reason, the script's CSL version and the supported one.

**Erratum.** SHAKER 2.7 added a ninth sub-test to its test U ("R4 & R9 check") that the
SHAKE27A-*.CSL scripts were not updated for: the test now ends 14.2-14.7 s after its key,
the script moves on after 14 s, and the five AI screens that follow are never reached. The
player lengthens that one wait to 16 s and says so in the log; `--no-errata` turns it off.

## Building from source

### Windows

Requirements: a MinGW-w64 GCC with C++17 (GCC 10 or later), CMake 3.16+,
Ninja (optional), and the **SDL2 MinGW development package**
(`SDL2-devel-2.x.x-mingw.zip` from <https://github.com/libsdl-org/SDL/releases>).

```
cmake -S . -B build -G Ninja -DSDL2_ROOT=C:/path/to/SDL2-2.x.x/x86_64-w64-mingw32
cmake --build build
```

or unpack the SDL2 package's `x86_64-w64-mingw32` folder into `third_party/SDL2` and
run `build.bat`. The result is `build/cpcse.exe` (and `build/cpcse-ctl.exe`), with `SDL2.dll` and the `roms`
folder copied beside it; the GCC runtime is linked statically, so nothing else is
needed to run it.

### Linux

Requirements: GCC 10+ (or Clang), CMake 3.16+, Ninja (optional), pkg-config, and the
SDL2 and OpenGL development packages. On Debian / Ubuntu:

```
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev libgl-dev
cmake -S . -B build -G Ninja
cmake --build build
./build/cpcse
```

The result is `build/cpcse`, with the `roms` and `sounds` folders copied beside it.
Run it from that folder, or pass `--roms <folder>`; drive and keyboard sounds are
looked for in `sounds/` next to the executable, then in the working directory. On
Linux the GUI runs CSL scripts with `fork`/`exec`, opens folders with `xdg-open`,
and downloads ROMs with `curl` (or `wget`). Paths in Windows-authored CSL scripts
are matched case-insensitively.

## Source layout

```
src/core/      the emulation core (static library, no host dependencies)
src/gui/       the desktop front end: SDL2 + OpenGL + Dear ImGui, debugger, assembler,
               the GDB server and command API
tools/ctl/     cpcse-ctl, the command API's client
docs/          external-debugging.md
roms/          Amstrad firmware and the Plus system cartridge (see roms/README.txt)
third_party/   Dear ImGui (docking branch), RASM, ymfm, the SP0256 core, minimp3 and the
               DejaVu Sans Mono font, vendored; SDL2 is fetched separately
```

## Project status

| Area | State |
|---|---|
| CRTC types 0-4, Gate Array, monitor | Mature: rule sets from the Compendium, cross-checked by independent reference models and SHAKER |
| Z80, PSG, PPI, disc controller, tape | Working; not yet audited to the same depth as the video chips |
| CPC Plus ASIC | Working; its picture goes through the same monitor model as a CPC's |
| CSL / SSM | Complete (CSL 1.5, SSM 1.1) |
| User interface | Every option exposed, grouped by Machine / Media / Video / Audio / Input / Expansions / Tools / Debug; still being refined |
| DSK editor | Working: new discs, files and sectors of standard and extended images; its filesystem checked against AMSDOS itself (LOAD, SAVE, ERA, read-only) |
| M4 board | Working: files and raw SD sectors over a host folder, checked against the M4's own ROM source. Not yet: mounting DSK images through the M4, its ROM board, WiFi/network |
| GFX9000 (V9990) + Video9000 | Working, from Yamaha's application manual and tests of a real V9990 on a CPC; the blitter at openMSX's measured speeds (LMMC, LMCM and CMMC estimated), /WAIT on the bus |
| OPL4 (YMF278B) | Working (ymfm core); needs `yrw801*.rom` for the General MIDI samples |
| PlayCity | Working: both YMZ294s and the CTC's clock measured; its NMI / IM2 timers follow the Z80 CTC manual but are not yet tested with PlayCity software |
| SSA-1 / dk'tronics speech | Working (MAME's SP0256 core); needs `sp0256-al2.bin` |
| LambdaSpeak 3 | Its SSA-1 mode and its serial mode with the Catalex MP3 module. **Not emulated:** the DECtalk / Epson speech modes (the Epson chip's firmware) |
| Drive / keyboard sounds | Working, from recordings (an Amiga 600 drive and a PC keyboard -- no licence-clear Amstrad recordings were found; drop your own in `sounds/`) |
| **Symbiface II / III** | **Incomplete** -- mouse and RTC only; the IDE/CF interface is not emulated |
| DigiBlaster / AmDrum DACs | Working -- they were silent before 0.2.2 (never mixed in); now measured |
| **Lightguns, printers** | Working, lightly tested |

## Known issues

- **C-HSYNC width on CRTC 0 and 2.** The Gate Array's monitor sync on these chips is
  a fraction of a microsecond longer than the Compendium's table (§14.4, p.134). The
  documented width is not used yet because the monitor's recovery after a short sync
  is not modelled, and without that model it makes some pictures worse.
- **SymAmp under SymbOS**: SA2 songs written for 60/70 Hz that set their speed in the
  pattern play about 1.4x too slowly. This is SymAmp's own player, not the emulation: it
  ticks at 50 Hz and converts a song's BPM into its speed, but takes a speed set later in
  the pattern as it stands -- a real CPC plays them the same. MP3 playback is not
  available (it needs an MSX MP3 cartridge, which is not emulated).
- **GFX9000**: the chip's intermittent command faults that real-hardware tests report (a
  command reusing DY or NX/NY may go astray or hang) are not reproduced; LMMV's colour
  order with DIX=1 is approximate.
- The user interface and the expansions listed as incomplete above.

## Credits and licence

CPCSyntaxError is released under the MIT licence (see `LICENSE`). The third-party
components it contains keep their own licences -- see `THIRD_PARTY_NOTICES.md`.
Note that the built-in assembler includes the Exomizer cruncher, whose licence
permits **non-commercial use only**. The Amstrad firmware and system cartridge in
`roms/` are not covered by the MIT licence (see `roms/README.txt`).

CRTC behaviour is sourced from the **"Amstrad CPC CRTC Compendium" by Longshot**
(CC BY-NC-ND 4.0) and checked against his SHAKER test suite; the CSL and SSM formats
are his standards too. The built-in assembler
is **RASM by Edouard BERGE**. The user interface uses **Dear ImGui** by Omar Cornut
and **SDL2**.

Amstrad CPC, CPC Plus and GX4000 are trademarks of their respective owners. This
project is not affiliated with Amstrad.
