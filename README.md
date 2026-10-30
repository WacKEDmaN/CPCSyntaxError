<p align="center"><img src="images/logo.png" alt="CPCSyntaxError" width="600"></p>

# CPCSyntaxError

CPCSyntaxError is an emulator for the Amstrad **CPC 464, CPC 664, CPC 6128, CPC Plus and GX4000**.
It runs on Windows, Linux and macOS. It has a cycle-level model of the chips of the machine.

> **This is work in progress.** The video chips, the Z80 and the other chips of the CPC are
> complete. Work continues on the user interface and on some expansions. Refer to
> [Project status](#project-status).

- [Screenshots](#screenshots)
- [Features](#features)
- [Getting started](#getting-started), [Keys](#keys), [Command line](#command-line)
- [External debugging](#external-debugging), [CSL scripts and SSM screenshots](#csl-scripts-and-ssm-screenshots)
- [Building from source](#building-from-source), [Source layout](#source-layout)
- [Project status](#project-status), [Known issues](#known-issues), [Credits and licence](#credits-and-licence)

## Screenshots

| | |
|---|---|
| ![Batman Forever on a CPC 6128](images/6128-BatmanForever.png) | ![Pinball Dreams on a CPC 6128](images/6128-PinballDreams.png) |
| *Batman Forever on a CPC 6128* | *Pinball Dreams on a CPC 6128* |
| ![Alcon 2020 on a GX4000](images/GX4000-ALCON.png) | ![Sonic the Hedgehog on a GX4000](images/GX4000-SONIC.png) |
| *Alcon 2020 on a GX4000* | *Sonic the Hedgehog on a GX4000* |
| ![The M4 board and its WiFi](images/M4BoardWiFi.png) | ![The dot-matrix printer](images/dotmatrix.png) |
| *The M4 board: the SD card, and `\|NETSTAT` on the network of the PC* | *The dot-matrix printer and the expansion settings* |
| ![SymbOS and the memory map](images/memory%20map.png) | ![The BASIC window](images/basic-editor.png) |
| *SymbOS on a CPC 6128, with the memory map* | *The BASIC window, next to the assembler* |

## Features

### The machine

- **All five CRTC types** are available:
  - Type 0: HD6845S / UM6845
  - Type 1: UM6845R
  - Type 2: MC6845
  - Types 3 and 4: the ASICs of the Plus and the cost-down CPC

  Each type has its own rule set. The rules come from the *Amstrad CPC CRTC Compendium* by
  Longshot. The SHAKER test suite by Longshot and photographs of real machines check them.
- **The Gate Array** models are 40007, 40008, 40010 and the ASICs 40226 and 40489. Each model
  has its own pixel timing. Select the model in **Machine → Gate Array**.
- **The monitor** models are the CTM 640/644, CM14, GT 64/65 and MM12. The monitor locks to
  the composite sync that the Gate Array makes. It has **H. Hold** and **V. Hold** controls,
  as the pots on a real monitor (**Video → Monitor**). Turn V. Hold too far and the picture
  rolls. Turn H. Hold too far and the picture breaks into diagonal bars. **Centre both** sets
  the default again.
- **The CRT effect** (**Video → CRT effect**) shows the tube's own pattern: the slot mask of
  the colour sets (the CTM's tube is a slot-mask tube), with each red, green or blue phosphor
  slot lit as a whole, and scanlines on the green and grey sets. You can choose the pattern,
  its strength and size, and turn the mask 90 degrees. The **soft slot mask** keeps each
  pixel's colour whole under fine dark lines, as ACE-DL draws a CTM. A bloom adds a glow. A
  blur softens the edges of the pixels, as the round spot of the beam does.
- **The Advanced CRT Filter** (**Video → Advanced CRT Filter**) draws the picture on the
  graphics card in two stages, each of which you can turn off. The signal stage decodes it as
  a PAL signal: the colour spreads over the pixels round it and alternate lines' colour
  shifts, then the scanlines, brightness, contrast and gamma. The screen stage is the tube's
  glass: a bulge, rounded corners, bleed and glow, dark lines every few screen lines, a
  phosphor mask (stripes, slots or dots), a tint and an optional rolling bar. While it is on,
  the CRT effect above is not drawn.
- **The Z80** passes ZEXDOC, ZEXALL and all the tests of z80test 1.2a by Patrik Rak. This
  includes the undocumented flags, MEMPTR, SCF/CCF on a Zilog chip and interrupted block
  instructions.
- **The PSG, the PPI, the disc controller and the tape** agree with their data sheets and
  formats: the AY-3-8912, the 8255, the NEC uPD765A as Amstrad connected it, and TZX 1.20.
- **The CPC Plus ASIC** has sprites, DMA sound, the 4096-colour palette, raster interrupts,
  split screen and soft scroll.
- The machines are the CPC 464, CPC 664, CPC 6128, 464 Plus, 6128 Plus and GX4000. RAM
  expansions go up to 4 MB. On a Plus, a classic RAM board on the expansion port behaves as on
  the real machine (**Machine → RAM**). Joysticks, game controllers and the analogue port of the Plus are available.

### Discs and tapes

- **Disc images**: DSK, extended DSK, **HFE** (the format of the Gotek) and **IPF**.
- **Writes to the image**: the emulator writes the changes from a program into the image
  file, in the format of that image. An HFE file keeps all unchanged tracks bit for bit. IPF
  images are write-protected. Use Media → "Save disc and tape writes to their files".
- **The disc drive in real time**:
  - The disc turns at 300 rpm under a 250 kbit/s head.
  - The bytes of a sector come when the sector passes the head. HFE and IPF images give the
    real position of each sector.
  - A seek takes the step rate that the program sets.
  - The drive is ready when the disc turns at full speed.
  - A program that is too slow for the bytes gets an overrun, as on a real CPC.
- **The tape deck** is in the Tape window and in Media → Tape. It has a counter with a reset,
  the time and the block of the tape, and a list of the blocks. Its buttons are rewind, back a
  block, play, record, pause, stop, fast forward and eject. It reads CDT, TZX, TAP and WAV.
  - **Record**: the deck records the output of SAVE. It records from the current position,
    or onto a **new blank tape**. The firmware format becomes standard turbo blocks (TZX
    &11). Other signals stay as the raw signal. **Save tape as** writes a CDT file. A
    recording on a CDT file goes back into that file.
  - **Fast loading** (on by default): the blocks that the CPC firmware reads play at a speed
    that you set, from 1000 to 3500 baud (default 3000). Each tape has its own speed, so each
    block gets its own factor. A block plays fast only when it is in the firmware's format and
    the firmware reads it. A block that a custom or protected loader reads plays at its own
    speed. The list of blocks shows the decision for each block.
  - **Instant loading** (off by default): when the firmware reads a block, its bytes go into
    memory at once. There is no wait, and no border stripes or tape sound. Other blocks play
    as they are. This works with the OS ROMs of the 464, 664, 6128 and the Plus.
  - **Turbo**: while the tape moves, the full CPC runs faster. The speed is up to 20 times, or
    the maximum speed of your computer. All loaders work, protected loaders too.
- **Cartridges (CPR and CRO) and snapshots (SNA)** are available. Each type of media has an
  **Eject** command. You can drop a file on the window. A **CRO** file holds groups of ROMs:
  a group of cartridge ROMs is a Plus cartridge, and a group of CPC ROMs starts a classic CPC
  with them fitted (Cartridge → CRO group selects the group).

### Cheats

- Use **Tools → Cheats** to find where a game keeps a value: lives, ammunition, time or energy.
- **The walkthrough** asks only for what you see on the screen:
  1. Type the number that the game shows now.
  2. Play until the number changes. Type the new number.
  3. Do step 2 again until one or two places stay.
  4. Select the value that you want. The emulator keeps the value there.
  5. Tell the emulator if the cheat works. If it does not, the emulator tries each place
     one at a time.
- For a bar with no number, tell the emulator if the bar went down, went up or did not
  change.
- The search finds all the usual formats at the same time: the value, the value minus one,
  and BCD. It searches all the RAM, the 128K banks too.
- **The Search tab** gives the same search by hand, for 8-bit and 16-bit values, with undo.
- You can type **POKEs** from a magazine.
- The emulator keeps the cheats for a game in a file next to the game, `<game>.pok`. The
  cheats load with the game, switched off.

### Expansions

Select the expansions in the **Expansions** menu or in the Settings window.

- **M4 board**:
  - A folder on your PC is its SD card.
  - The ROM of the M4 (`M4ROM.ROM` by Duke, [spinpoint.org](http://www.spinpoint.org)) is
    included with his permission. It runs without changes.
  - Files and long names work from BASIC: `|CD`, `CAT`, `|LS`, `|ERA`, `|REN`, `|COPYF`.
  - **A DSK image on the card opens as a folder**: type `|CD,"game.dsk"`, then use `CAT`,
    `LOAD` and `RUN`.
  - **SymbOS** reads and writes the card sector by sector.
  - **The WiFi** uses the internet connection of your PC. CPC programs open TCP sockets, find
    names and run servers. The telnet and TCP examples by Duke work. `|HTTPGET` and `|HTTPMEM`
    download plain HTTP files, as on the real board. `|NETSTAT` shows the network.
- **Multiface II**: F10 is its STOP button. The menu, save and reload work. The ROM is from
  Romantic Robot and is not included. Put a file with "multiface" in its name in `roms/`.
- **RS232C serial interface** (from Amstrad and Pace): this card has a Z80 DART and an 8253 at
  `&FADC` and `&FBDC`. Its line can be one of these:
  - a TCP port for other programs (for example `telnet localhost 2323`)
  - a connection to a host (a BBS or a telnet server)
  - a serial port of your PC
  - a loopback plug

  The ROM of the card is not included. Put it in a ROM slot.
- **Symbiface II and III**: the mouse, the clock and the **IDE/CF interface**. The IDE drive
  is a folder on your PC: `symide` next to the program, or a folder that you select. SymbOS
  reads and writes it as one FAT16 partition. The **X-MASS** is the same IDE/CF interface
  alone, at `&FD08`.
- **SYMBiFACE III**, more:
  - **Its sound card** (the SE-ONE at `&FF20`): SymAmp plays MP3 files through it, with
    volume, balance, bass, treble and the spectrum. TMTLOGIC's SE-ONE player, samples, speech
    sounds and MIDI programs work. The **USB stick** and the **SD card** are folders on your PC
    (Expansions → Symbiface).
  - **Its WiFi** uses the network of your PC. **Web radio works**: TMTLOGIC's `WEB-DK.BAS`
    plays its station.
  - **Its other functions**: `|VER`, the clock, the buzzer, the measurements and the **OLED**
    display (Expansions → Symbiface → SF3 OLED display).
  - MP3 and WAV are decoded in the emulator. AAC, AAC+, Ogg, WMA, M4A/MP4, FLAC and Opus need
    [ffmpeg](https://ffmpeg.org) on your PC: on the PATH, or named with `ffmpeg=` in
    `cpcse.ini`.
- **Mouse interfaces** (Input → Mouse interface): the **AMX mouse** on the joystick port, the
  **Kempston mouse** and TotO's **MultiPlay**. They are off while a Symbiface II or III is
  fitted, because the Symbiface has its own mouse.
- **GFX9000** (Yamaha V9990 at `&FF60`): all the screen modes, sprites, cursors and blitter
  commands. The blitter takes its real time, and the **/WAIT** signal stops the Z80 while the
  chip is busy. The picture of the GFX9000 can be next to the CPC picture or in its own
  window. It can also share one monitor, with a switch or through a **Video9000**.
- **OPL4 sound card** (YMF278B, on the AMSDAP at `&FFC4` and `&FF7E`): 18 FM channels and 24
  wavetable channels, for the sound daemon of SymbOS and for SymAmp. The General MIDI samples
  need the YRW801 ROM from Yamaha. It is not included. The emulator asks for it.
- **PlayCity** (two YMZ294 and a Z80 CTC at `&F880` to `&F988`): six more AY channels in
  stereo, the clock of the CTC, raster NMIs and IM2 timer interrupts.
- **Speech**:
  - The **SSA-1** from Amstrad and the **dk'tronics** speech unit use the SP0256-AL2 (the MAME
    core). Its ROM is not included. The emulator asks for it.
  - **LambdaSpeak 3**: its Epson and DECtalk speech uses the voices of your computer. These are
    the Windows voices, `say` on macOS and `espeak-ng` on Linux. Its serial **MP3 module**
    plays MP3 files from a folder.
- Lightguns (Trojan Light Phazer, Gunstick, West Phaser), a text or **dot-matrix printer** on
  the printer port, and the DigiBlaster and AmDrum. You can save printer pages as BMP or SVG.
- **Drive and keyboard sounds**: the drive motor, the head steps, disc insert and eject, and
  key clicks. They are recordings in `sounds/`. You can replace them with your own WAV files.
- **A volume for each sound device** (Audio → Sound sources): the CPC, the DACs, speech, the
  LambdaSpeak 3 MP3 module, the SYMBiFACE III, the PlayCity and the OPL4, each from 0 to 200%.

### Tools

- **The debugger** (Debug menu):
  - Run, step into, step over and step out.
  - The registers, flags and stack of the Z80, next to the disassembly with labels.
  - Breakpoints with conditions, and memory watchpoints.
  - **Chips**: live views of the CRTC, the Gate Array, the monitor, the Plus ASIC, the PSG,
    the PPI, the keyboard matrix, the disc controller and the tape.
  - **Memory**: a hex editor and a **memory map**. The map shows one pixel for each byte of a
    64K bank. The colour shows what the Z80 did there: opcode fetch, operand, read or write.
  - **GFX9000**: its picture, registers, palette, command engine and VRAM.
  - **Logic analyser**: the machine's signals as traces, one sample a microsecond: HSYNC,
    VSYNC, C-SYNC, DISPEN, INT, HALT, the cassette motor and input, the disc controller's
    interrupt, the Gate Array mode, the CRTC counters C0, C4, C9, MA and R52, the Z80's PC and
    the three AY channels. Zoom, two markers that measure time, and a trigger that stops the
    CPC on a VSYNC, HSYNC or INT edge, a mode change, or a PC. It records only while open.
  - **Profiler**: press Record, let the program run, then press Stop. It shows the time of
    each routine (entered by a CALL, an RST or an interrupt), its own and with what it calls,
    the busiest instructions, and a map of the 64K. The disassembly shows the time of each
    line as a bar. It records only while open.
  - **Raster timing overlay** (Debug menu): a tick on the picture where each OUT to the
    CRTC, the Gate Array, the PPI or the Plus palette was made, as the beam swept it.
  - **Back in time**: **Back** (Shift+F7) one instruction, **Frame back**, **Run back**
    (Shift+F5) to the last breakpoint hit, and **Last write to** an address: the instruction
    that last wrote it. Running on from there replays the inputs as they came.
- **Quick save slots**: nine slots for each program. **Ctrl+1** to **Ctrl+9** load a slot,
  **Ctrl+Shift+1** to **Ctrl+Shift+9** save it. Each slot is a `.sna` file and a picture in
  `states/<program>/`, so the slots stay from one session to the next. The Snapshot window
  shows them as pictures. **File → Undo the last load** gives back the machine as it was.
- **Recording** (File → Record): video and sound into an AVI as **MJPEG** (most players,
  Windows' own player too), into an MP4 as **H.264** with [ffmpeg](https://ffmpeg.org) (small
  files: 90 seconds is about 3.5 MB), or the sound only into a WAV. The picture can be the one
  shown, the CPC, the GFX9000, both side by side, or the screen with its CRT effects. Every
  frame is recorded, also at unlimited speed.
- **The timeline** (Machine window, on by default): a snapshot of the machine every second
  and every input since. **Ctrl+Left** goes a second back (hold it to go on). The snapshots
  are exact: from one, with the same inputs, the machine runs the same, instruction for
  instruction. A reset, a change of the machine or its media, or an edit in the debugger
  starts it again. The M4, OPL4, IDE, serial and speech cards are not rewound, nor what is
  written to a disc.
- **Input movies** (Machine → Movie): record from power-on, then every key, joystick, mouse
  and tape button. Played back with the same machine and media, it is the same run; a check
  every second shows that it stays in step, or where it went out of step.
- **The graphics finder** (Tools): memory as CPC graphics in mode 0, 1 or 2, or as Plus
  sprite data, to find sprites, fonts and screens. Set the width and the height until the
  graphics line up; select a linear layout or the CPC screen layout. Save a tile as a PNG,
  or copy it as `db` lines.
- **The DSK editor** (Tools, or Media → Drive → Edit):
  - Make new discs in all the CPC formats: DATA, SYSTEM, IBM, 42 or 80 tracks, ParaDOS,
    ROMDOS, Vortex, Dobbertin, or your own geometry.
  - Open standard and extended images, or edit the disc in a drive while the CPC uses it.
  - Files: import, export, delete, rename, change the user and the flags. View a file as hex,
    BASIC, text or Z80 code.
  - Tracks and sectors: the IDs, the status bytes, the weak copies, and the bytes in a hex
    editor. A map shows the owner of each block.
- **The assembler** is [RASM](https://github.com/EdouardBERGE/rasm), included in the program.
  Press F9 to assemble into the memory of the running machine. Press Ctrl+F9 to assemble and
  run. You can change the colours of the syntax.
- **C programs** in the same window. Open a `.c` file: F9 builds it with
  [SDCC](https://sdcc.sourceforge.net) and, if you name a copy of
  [CPCtelera](https://github.com/Arnaud6128/cpctelera/tree/cpctelera_next) in **C settings...**,
  with CPCtelera's library. Ctrl+F9 builds it and runs it from BASIC, as a real CPC runs a
  binary. You do not need Cygwin, make or bash.
  - The project's `cfg/*.mk` asset steps run first: images, Tiled maps, Arkos Tracker 3 music,
    packed data (zx0, zx1, zx7b) and `.bin` files. **Convert assets** runs all of them again.
  - All 58 example projects of CPCtelera's "next" branch build and run.
  - SDCC is not included: install it separately. [z88dk](https://z88dk.org) can be the
    compiler in its place (sccz80 or zsdcc), in **C settings...**.
  - A CPCtelera project's disc and tape images and an SNA snapshot are made too.
  - Step a C line (F10, F11, Shift+F11) and click the gutter to set a breakpoint.
- **Run to cursor** (Ctrl+F10, or the right-click menu) runs to the line under the cursor and
  goes past every breakpoint on the way, in assembly and in C. Put the mouse on a variable or
  a label to see its address and its value.
- **The BASIC window** (next to the assembler) lists the BASIC program in the machine, edits
  it, and puts it back into memory at once, as if it had been typed. It can type RUN for you.
  Its tokenizer is checked byte for byte against Locomotive BASIC itself.
- **External debugging and code loading**: a **GDB server** for
  [DeZog](https://github.com/maziac/DeZog) in VS Code, a **command API** with the `cpcse-ctl`
  client, and an **auto-reload** of your build. Refer to [External debugging](#external-debugging).
- **CSL scripts and SSM screenshots**: these are the CPC Script Language 1.5 and ScreenShot
  Management 1.1 standards by Longshot. `cpcse.exe --csl <script>` runs a script with no
  window. The SHAKER scripts run in this mode. Refer to
  [CSL scripts and SSM screenshots](#csl-scripts-and-ssm-screenshots).

### The interface

- The windows dock and change size (Dear ImGui, docking branch). You can move each window out
  of the main window.
- At the start, only the screen, the Machine and ROMs windows and the media windows (Discs,
  Tape, Cartridge, Snapshot, as tabs) are open. Open the other windows from the menus. They
  dock with their group.
- **The on-screen CPC keyboard** (Input menu): the keyboard of the 464, 664, 6128 or Plus,
  drawn from photographs of the real machines, with UK, French or Spanish legends. A key
  lights while the CPC sees it pressed. Click a key to press it; right-click to hold it down.
- **Joysticks and joypads**: select the controller for each CPC joystick, and redefine each
  button and direction (Input → Joysticks and buttons...). On a Plus or GX4000, a joystick
  can be digital or analogue (the Plus analogue port).
- **Keys**: the CPC layout (each key by its position) or the PC layout (each key types its
  character on the CPC), for a UK, French or Spanish CPC keyboard.
- **File → Recent files**: the last 12 discs, tapes, cartridges and snapshots.
- Use **Window → Interface size** to make the interface larger (90% to 200%).
- The file picker shows all the drives (C:, D: and more on Windows). You can type a folder
  name into it. The Save dialog works the same way. Files with all types of names open,
  for example accented, CJK and emoji names (Windows 10 1903 or later).

## Getting started

1. Download the package for your system from the [Releases](../../releases) page:

   | System | Package |
   |---|---|
   | Windows (64-bit) | `CPCSyntaxError-win64-<version>.zip` |
   | Linux (x86_64) | `CPCSyntaxError-linux-x86_64-<version>.tar.gz` |
   | Linux (x86_64), Flatpak | `CPCSyntaxError-linux-x86_64-<version>.flatpak` |
   | macOS 10.15 or later (Apple silicon and Intel) | `CPCSyntaxError-macos-universal-<version>.zip` |

2. Unzip it into a folder.
3. Run the program:
   - Windows: run `cpcse.exe`.
   - Linux: install SDL2 from your distribution (for example `sudo apt install libsdl2-2.0-0`),
     then run `./cpcse`.
   - Linux, Flatpak: you do not unzip it. Install it, then start CPCSyntaxError from your
     applications menu. The first install gets the freedesktop runtime from Flathub:
     ```
     flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
     flatpak install --user CPCSyntaxError-linux-x86_64-<version>.flatpak
     ```
     The Flatpak keeps its settings, screenshots and ROM folder in
     `~/.var/app/io.github.wackedman.CPCSyntaxError/data/cpcsyntaxerror`. It can open files
     in your home folder.
   - macOS: the app does not have an Apple Developer ID signature. The first time, right-click
     `CPCSyntaxError.app` and select **Open**, then select **Open** again. Keep the app in a
     folder that you can write to, because it keeps its settings inside the app.

You can also build the program yourself (refer to [Building from source](#building-from-source)).
GitHub Actions builds the packages (`.github/workflows/build.yml`; the Flatpak from `flatpak/`).

The firmware ROMs of the CPC 464, CPC 664, CPC 6128 and CPC Plus, and the system cartridge
of the GX4000, are included in `roms/`. Amstrad gave permission to distribute their copyrighted
material. Amstrad keeps that copyright.

`cpcse.exe` looks for the `roms` folder next to it. To use another folder, use **File → ROM
folder…** or `cpcse.exe --roms <folder>`. The emulator finds each file by its name. Upper and
lower case are the same.

| For | The file name must contain |
|---|---|
| CPC 464 | `464 OS`, `464 BASIC` |
| CPC 664 | `664 OS`, `664 BASIC`, `AMSDOS` |
| CPC 6128 | `6128 OS`, `6128 BASIC`, `AMSDOS` |
| 464 Plus, 6128 Plus | `6128+ OS`, `6128+ BASIC`; the 6128 Plus also `Plus A2` (its AMSDOS 0.7). Without them, the system cartridge. |
| GX4000 | `burning rubber` (the system cartridge, a `.cpr` file) |
| M4 board | `M4ROM` (included) |
| Multiface II | `multiface` (not included) |
| OPL4 General MIDI samples | `yrw801` (not included, the emulator asks for it) |
| SSA-1, dk'tronics, LambdaSpeak 3 speech | `sp0256-al2.bin`, 2 KB (not included, the emulator asks for it) |

The first start uses a CPC 6128. To use another machine, use **Machine → Model**. To load
software, use the **File** or **Media** menu, or drop a file on the window.

The emulator saves the settings in `cpcse.ini` and the window layout in `cpcse_layout.ini`.
These files are next to the program. **Window → Reset layout** sets the default layout again.

## Keys

| Key | Action |
|---|---|
| F11 | Fullscreen (or double-click the picture) |
| Ctrl+R | Reset |
| Ctrl+P | Pause or run |
| F10 | The STOP button of the Multiface II |
| F5 | Run or pause (in a debugger window, or when paused) |
| F7, F8, Shift+F8 | Step into, step over, step out |
| Shift+F7, Shift+F5 | One instruction back, run back to the last breakpoint hit |
| Ctrl+Left | A second back (the timeline) |
| Ctrl+1 to Ctrl+9 | Load quick save slot 1 to 9 |
| Ctrl+Shift+1 to Ctrl+Shift+9 | Save quick save slot 1 to 9 |
| F9, Ctrl+F9 | Assemble, assemble and run (in the assembler window); build, build and run for a `.c` file |
| Ctrl+F10 | Run to the cursor's line, past the breakpoints (in the assembler window) |
| F10, F11, Shift+F11 | Step a C line over, into, out (in the assembler window, for a `.c` file) |
| F12, middle button | Release the mouse (click the picture to give the mouse to the Symbiface or the mouse interface) |

The keys go to the CPC. The exception is when a debugger or assembler window has the focus.

In the CPC layout, the keys without a PC key in the same place are: CLR (Delete), DEL
(Backspace), COPY (Left Alt), the CPC's `[` key (the key under Esc, left of 1), the CPC's `\` key
(`\`, and on a UK keyboard also the key left of Z), and f0 to f9, f. and the small ENTER (the numeric
keypad, or F1 to F10). With joystick emulation on, the cursor keys, Ctrl and Alt are the
joystick: switch it off to use them as the CPC's cursor keys, CONTROL and COPY.

## Command line

`cpcse.exe --roms <folder>` starts the emulator with another ROM folder.
[External debugging](#external-debugging) shows the options `--gdb`, `--api`, `--load`,
`--watch`, `--run`, `--reset`, `--command` and `--symbols`. It also takes the `-i`, `-o`, `-s`
and `-O` options TRSE passes, so TRSE can run CPC programs in it
([docs/external-debugging.md](docs/external-debugging.md#trse)).

A headless screenshot mode is available for scripts:

```
cpcse.exe --shot out.bmp --model cpc6128 --frames 200 [--disk game.dsk] [--type "RUN\"GAME\n"]
          [--cart file.cpr] [--tape file.cdt] [--sna file.sna] [--savesna out.sna]
          [--crtc 0-4] [--gate-array 40007|40008|40010] [--beam] [--ram 64-4160]
          [--disk-writeback] [--record-tape out.cdt] [--tape-fast <baud>|instant|off] [--raster-overlay]
          [--hhold -79..79] [--vhold -50..50]
          [--m4 <folder>] [--multiface] [--sf2] [--sf3] [--xmass] [--ide <folder>] [--xrom <slot>=<rom file>]
          [--mouse-interface amx|kempston|multiplay-a|multiplay-b] [--cro-group <n>]
          [--serial tcp:<host>:<port>|listen:<port>|com:<port>|loopback]
          [--gfx9000] [--v9990-shot gfx.bmp] [--video9000-shot mixed.bmp]
          [--opl4] [--playcity] [--speech ssa1|dktronics|lambdaspeak3] [--mp3card <folder>]
          [--dac digiblaster|amdrum] [--wav out.wav] [--record out.avi|out.mp4|out.wav]
          [--record-format mjpeg|h264] [--record-source shown|cpc|gfx|both]
          [--sf3-usb <folder>] [--sf3-sd <folder>] [--z88dk <folder>] [--sccz80]
          [--gfx9000-monitor beside|window|switch|video9000]
          [--mouse "w120;j5,60;j16,2;tDIR;kEnter;s<file.bmp>"]
```

- `--cbuild <main.c>` builds a C project with SDCC and runs it (add `--cpctelera <folder>`,
  `--sdcc <folder>`, `--codeloc <address>` or `--force-assets` as necessary).
- `--type` types its text after the machine starts. `\n` is the Enter key.
- `--disk-writeback` writes the changes from the program into the disc file. This is off in
  headless mode and on in the window.
- `--record-tape` puts a blank tape in the deck and starts to record. At the end, the
  emulator saves the tape in that file.
- `--raster-overlay` draws the raster timing overlay on the picture.
- `--record` records the whole run: an `.avi` with picture and sound (`--record-format`
  `mjpeg`, the default), an `.mp4` (`h264`, with ffmpeg), or a `.wav` with the sound only.
  `--record-source` picks the picture.
- `--sf3-usb` and `--sf3-sd` fit a SYMBiFACE III with that folder as its USB stick or SD card.
- `--tape-fast` sets the fast loading of the tape: a speed in baud, `instant`, or `off`. At
  the end, the emulator prints the decision for each block.
- `--mouse` runs a small script after all the other steps. The commands are: `w` (wait
  frames), `m` (move the mouse), `c` (click), `d` (double-click), `j` (hold the joystick,
  `j<bits>,<frames>`), `t` (type text), `k` (tap a key) and `s` (save a screenshot). This is
  sufficient to control a desktop such as SymbOS with no window.

The models are `cpc464`, `cpc664`, `cpc6128`, `cpc464plus`, `cpc6128plus` and `gx4000`.

The headless modes write to the Command Prompt that started them. `cpcse.exe` is a windowed
program, thus the prompt does not wait for it. In a batch file that needs the result, use
`start /wait cpcse.exe ...`.

## External debugging

Write your code in VS Code (or another editor), and let the emulator follow your build. These
functions are off until you switch them on. Use *Settings → External debugging* or the
command line. The servers listen on `127.0.0.1` only.

- **GDB server** (`--gdb`, port 12000): the `mame` remote of DeZog connects to it. It gives
  source-level debugging with breakpoints, steps, watchpoints, and views of the memory and
  the registers.
- **Command API** (`--api`, port 6128) and `cpcse-ctl` (next to `cpcse.exe`): load code,
  type, read and write memory, set breakpoints, wait for a stop and take screenshots.
- **Auto-reload** (`--watch build/game.bin@0x4000 --run entry`): the emulator writes each new
  build into memory and starts it again.

```
cpcse.exe --gdb --api --watch build/game.bin@0x4000 --run entry --symbols build/game.sym
cpcse-ctl load build/game.bin 0x4000 entry
```

[docs/external-debugging.md](docs/external-debugging.md) gives the DeZog `launch.json`, a
build task, all the options and all the commands.

## CSL scripts and SSM screenshots

`cpcse.exe --csl <script>` runs a CPC Script Language file with no window. The emulated program
asks for screenshots with SSM codes (the Z80 bytes `ED LL ED HH`). The emulator saves each one
as `CPCSE_<crtc>_<HHLL>.bmp`. This is the name format that the SSM standard gives for the
SHAKER portal. This mode runs only when you give `--csl`.

```
cpcse.exe --csl SHAKE27A-1.CSL --out shots --disk-dir <folder with shaker27.dsk>
```

| Option | Function |
|---|---|
| `--out <dir>` | The folder for screenshots, snapshots and `csl.log` (default `screenshots`) |
| `--roms <dir>` | The firmware and the Plus system cartridge (default `roms`) |
| `--disk-dir`, `--tape-dir <dir>` | The folders for the media that a script names |
| `--model <id>` | The machine for a script with no `cpc_model` (default `cpc6128`) |
| `--crtc <n>` | Use this CRTC type (0 to 4) for each `crtc_select` |
| `--emu-name <name>` | The prefix of the image names (default `CPCSE`) |
| `--csl-log <file>` | The log of the run (default `<out>/csl.log`) |
| `--no-chain` | Do not follow `csl_load` |
| `--no-errata` | Run published scripts exactly as written (refer to the erratum below) |

The player knows all the CSL 1.5 instructions:

- `wait_ssm 0xHHHH`: the script waits until the program sends that SSM code and its
  screenshot is saved.
- Machine configuration: `cpc_model`, `crtc_select` (0, 1, 1A, 1B, 2, 3, 4), `gate_array`,
  `memory_exp`, `rom_dir`, `rom_config`.
- `reset soft` and `reset hard`.
- Disc, tape and snapshot media, and their folders.
- Keys: `\(...)` codes, `{groups}`, `\(KOF)`, `key_from_file`, `keyboard_write`.
- The four wait instructions.
- Screenshots and snapshots (versions 1 to 3), with their names and folders.
- `csl_load`.

`crtc_select 3` with no model makes a 6128 Plus, because CRTC 3 is its ASIC. The player
goes from its menu to BASIC before the script starts. SSM `#0000` ends a `wait_ssm0000`.
`#FFFE` saves the named screenshot and `#FFFF` saves a snapshot. If the machine cannot do an
instruction, the script stops with the report that the standard gives. The report contains the
script, the line, the instruction, the reason, the CSL version of the script and the supported
version.

**Erratum.** SHAKER 2.7 added a ninth sub-test ("R4 & R9 check") to its test U. The
SHAKE27A-*.CSL scripts do not include this change. The test now ends 14.2 s to 14.7 s after
its key, but the script continues after 14 s. Thus the script does not get to the next five
AI screens. The player makes that one wait 16 s long and writes this in the log. `--no-errata`
switches this change off.

## Building from source

### Windows

You need:

- a MinGW-w64 GCC with C++17 (GCC 10 or later)
- CMake 3.16 or later
- Ninja (optional)
- the **SDL2 MinGW development package** (`SDL2-devel-2.x.x-mingw.zip` from
  <https://github.com/libsdl-org/SDL/releases>)

```
cmake -S . -B build -G Ninja -DSDL2_ROOT=C:/path/to/SDL2-2.x.x/x86_64-w64-mingw32
cmake --build build
```

Alternatively, put the `x86_64-w64-mingw32` folder of the SDL2 package into
`third_party/SDL2` and run `build.bat`.

The result is `build/cpcse.exe` and `build/cpcse-ctl.exe`. The build copies `SDL2.dll` and
the `roms` folder next to them. The GCC runtime is linked statically. You do not need more
files to run the program.

### Linux

You need GCC 10 or later (or Clang), CMake 3.16 or later, Ninja (optional), pkg-config, and
the SDL2 and OpenGL development packages. On Debian and Ubuntu:

```
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev libgl-dev
cmake -S . -B build -G Ninja
cmake --build build
./build/cpcse
```

The result is `build/cpcse`. The build copies the `roms` and `sounds` folders next to it. Run
the program from that folder, or give `--roms <folder>`. The program looks for the drive and
keyboard sounds in `sounds/` next to the program, then in the working folder.

On Linux, the GUI runs CSL scripts with `fork` and `exec`. It opens folders with `xdg-open`
and downloads ROMs with `curl` (or `wget`). The paths in CSL scripts from Windows are not case
sensitive.

### macOS

You need the command-line tools of Xcode (Apple Clang), CMake 3.16 or later and SDL2, for
example from Homebrew. Macs with Apple silicon and Intel Macs use the same procedure:

```
xcode-select --install
brew install cmake ninja sdl2
cmake -S . -B build -G Ninja
cmake --build build
./build/cpcse
```

The interface uses OpenGL 3.2 (Core Profile). All Macs since 2011 have it. As on Linux, the
build copies the `roms` and `sounds` folders next to `build/cpcse`. Folders open with `open`.
Users report that the macOS build works. We did not test it here.

## Source layout

```
src/core/      The emulation core (a static library with no host dependencies)
src/gui/       The desktop program: SDL2, OpenGL and Dear ImGui, the debugger, the assembler,
               the cheats, the GDB server and the command API
tools/ctl/     cpcse-ctl, the client for the command API
docs/          external-debugging.md
roms/          The Amstrad firmware, the Plus system cartridge and the M4 ROM
               (refer to roms/README.txt)
sounds/        The drive and keyboard recordings
third_party/   Dear ImGui (docking branch), RASM, ymfm, the SP0256 core, minimp3, the SPS
               decoder library (for IPF, non-commercial licence) and the DejaVu Sans Mono
               font. SDL2 is not included.
```

## Project status

| Area | Status |
|---|---|
| CRTC types 0 to 4, Gate Array, monitor | Complete. The rule sets come from the Compendium. Independent reference models and SHAKER check them. |
| Z80 | Complete. It passes ZEXDOC, ZEXALL and all of z80test 1.2a. |
| PSG, PPI, disc controller, tape | Complete. They agree with their data sheets and formats. The CPC firmware checks the disc timing and the tape SAVE. |
| CPC Plus ASIC | Working. Its picture uses the same monitor model as the picture of a CPC. |
| Disc images | DSK and extended DSK: read and write. HFE: read and write. IPF: read (with the SPS library). |
| Cheats | Working. The walkthrough and the search are tested on a running CPC. |
| CSL and SSM | Complete (CSL 1.5, SSM 1.1). |
| DSK editor | Working. AMSDOS checks its file system (LOAD, SAVE, ERA, read-only). |
| User interface | All options are available, in the menus Machine, Media, Video, Audio, Input, Expansions, Tools and Debug. Work continues. |
| M4 board | Working. Files, DSK images and raw SD sectors use a folder on the PC. The ROM source of the M4 checks them. The WiFi uses the network of your PC. |
| Multiface II | Working: STOP, the menu, save and reload. |
| Symbiface II and III, X-MASS | Working: the mouse, the RTC and the IDE/CF interface. The ATA standard and SymbOS check them. |
| SYMBiFACE III sound card, WiFi and functions | Working: SymAmp's MP3 driver, TMTLOGIC's programs (SEPLAYER, SAMPLER, SE-TALK, SE-SYNTH, EMR, WEB-DK) and ROM, and SF3Monitor. Not emulated: the ROM banks, the file functions, DFU, MIDI files, the FM radio's sound. |
| Mouse interfaces (AMX, Kempston, MultiPlay) | Working. Their CPCWiki pages check them. They are not yet tested with AMX Art, OCP Art Studio or Carrier Command. |
| CRO ROM containers | Working. The two example files of the format start on the Plus. |
| GFX9000 (V9990) and Video9000 | Working. The data comes from the application manual from Yamaha and from tests of a real V9990 on a CPC. The blitter speeds are from the measurements of openMSX. |
| OPL4 (YMF278B) | Working (the ymfm core). It needs `yrw801*.rom` for the General MIDI samples. |
| SSA-1 and dk'tronics speech | Working (the SP0256 core from MAME). It needs `sp0256-al2.bin`. |
| LambdaSpeak 3 | Working: the Epson and DECtalk modes (with the voices of your computer), the SSA-1 and dk'tronics modes, the EEPROM, the Amdrum mode, MIDI out (Windows) and the MP3 module. |
| DigiBlaster and AmDrum DACs | Working. |
| Drive and keyboard sounds | Working. They are recordings of an Amiga 600 drive and a PC keyboard. You can put your own recordings in `sounds/`. |
| RS232C serial interface | Working. The data sheets of the chips check it, and a CPC talks through it over TCP. It is not yet tested with the ROM of the card. |
| PlayCity | Working. Its NMI and IM2 timers follow the Z80 CTC manual. They are not yet tested with PlayCity software. |
| Lightguns, printers | Working. Only some tests are done. |

## Known issues

- **SymAmp under SymbOS.** SA2 songs for 60 Hz or 70 Hz that set their speed in the pattern
  play approximately 1.4 times too slowly. The cause is the player of SymAmp, not the
  emulator. A real CPC plays them at the same speed. For MP3 playback, fit a SYMBiFACE III.
- **LambdaSpeak 3 in its Amdrum mode** takes all `&FFxx` ports. In that mode, the sound card
  of the SYMBiFACE III does not answer.
- **GFX9000.** Tests on real hardware show that some commands sometimes fail (a command that
  uses DY or NX/NY again can go wrong or stop). The emulator does not do this. The colour order
  of LMMV with DIX=1 is approximate.

## Credits and licence

CPCSyntaxError is by **WacKEDmaN** (author and prompter). The code is written with Claude
(Anthropic).

CPCSyntaxError has the MIT licence (refer to `LICENSE`). The third-party parts keep their own
licences. Refer to `THIRD_PARTY_NOTICES.md`. Two licences permit **non-commercial use only**:
the Exomizer cruncher in the assembler, and the SPS decoder library for IPF images. The MIT
licence does not cover the Amstrad firmware and the system cartridge in `roms/` (refer to
`roms/README.txt`).

The CRTC data comes from the **"Amstrad CPC CRTC Compendium" by Longshot** (CC BY-NC-ND 4.0).
His SHAKER test suite checks it. The CSL and SSM formats are also his standards.

- The assembler is **RASM by Edouard BERGE**.
- The M4 ROM is by **Duke**.
- The **SPS decoder library** (KryoFlux Products & Services) reads IPF images.
- The user interface uses **Dear ImGui** by Omar Cornut, and **SDL2**.

Amstrad CPC, CPC Plus and GX4000 are trademarks of their owners. This project has no
connection with Amstrad.
