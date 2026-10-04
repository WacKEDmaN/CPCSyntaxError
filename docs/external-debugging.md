# External debugging and code loading

CPCSyntaxError can be driven from outside while it runs, so you can work in VS Code or any other editor and build tool:

| | What it is for | Default port |
|---|---|---|
| **GDB server** | Source-level debugging in VS Code with [DeZog](https://github.com/maziac/DeZog), or any gdb client | 12000 |
| **Command API** | Loading code, typing, memory, registers, breakpoints and screenshots from scripts, build tasks and `cpcse-ctl` | 6128 |
| **File watching** | Reloading your build's output every time it changes, and starting it again | — |

All three are off until you turn them on. The servers listen on `127.0.0.1` only, so a program on your computer can reach them but nothing on the network can.

## Turning them on

- **In the emulator:** *Settings → External debugging*, or *Debug → External debugging*.
  - Tick **Listen** for each server. Its setting (on or off, and the port) is saved and applies the next time the emulator starts.
  - A changed port takes effect when you leave the field.
- **From the command line**, for this session only:

```
cpcse --gdb --api                                  both servers, default ports
cpcse --gdb=12001 --api=7000                       other ports
cpcse --load build/game.bin@0x4000 --run entry     load once at start, and run it
cpcse --watch build/game.bin --run entry           ...and again after every build
cpcse --watch build/game.dsk --reset --command "RUN\"GAME\n"
cpcse --symbols build/game.sym --load build/game.bin --run main
```

| Option | Meaning |
|---|---|
| `--gdb[=port]` | Start the GDB server. |
| `--api[=port]` | Start the command API. |
| `--load file[@addr]` | Load once the firmware has started (2 seconds in). A `.bin` goes at `addr`, or at the address in its AMSDOS header. `.sna`, `.dsk`, `.cpr` and tapes (`.cdt`, `.tzx`, `.wav`) load as they do from the Media menu. |
| `--watch file[@addr]` | Load it the same way, and again every time the file changes. If the file doesn't exist yet, it loads when your build first writes it. |
| `--run entry` or `--run addr` | After a `.bin` loads, start it. `entry` is the header's entry address. If the file has no header, or you gave `@addr`, `entry` means where it was loaded. Otherwise the program starts at `addr`. Without `--run`, the bytes go into memory and the PC is left alone. |
| `--reset` | Reset the machine before each load. For a `.bin`, the bytes are written once the firmware has started, so its start-up can't overwrite them. |
| `--command text` | Typed on the keyboard after loading; `\n` is Enter. For example `RUN"GAME\n` for a disc. |
| `--symbols file` | Labels for the debugger, for `--run` and for `file@label`. RASM, sjasmplus (`--sym`), pasmo, WinAPE or `name EQU value` files. With `--watch`, the file is read again after every build. |

Addresses can be written `&4000`, `#4000`, `$4000`, `0x4000`, `4000h` or `16384`, or as a label from the symbols. In a Windows command prompt, `&` has to be quoted, so `0x4000` is the easiest form there.

## VS Code with DeZog

DeZog's **MAME** remote speaks the GDB protocol, and CPCSyntaxError's GDB server answers it the way MAME's does. Start the emulator with its GDB server on, then use a launch configuration like this:

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "dezog",
            "request": "launch",
            "name": "CPCSyntaxError",
            "remoteType": "mame",
            "mame": { "port": 12000 },
            "memoryModel": "RAM",
            "sjasmplus": [ { "path": "build/game.sld" } ],
            "loadObjs": [ { "path": "build/game.bin", "start": "0x4000" } ],
            "execAddress": "0x4000",
            "topOfStack": "stack_top",
            "startAutomatically": false,
            "rootFolder": "${workspaceFolder}",
            "preLaunchTask": "build"
        }
    ]
}
```

- **Source mapping:** DeZog maps addresses to source lines using **sjasmplus** (`--sld`), **z80asm** or **z88dk** list files. It can't read RASM's output, so assemble with sjasmplus for source-level stepping. Breakpoints by address, memory views, registers and the disassembly work with any assembler.
- **Loading:** `loadObjs` writes the binary through the GDB connection and `execAddress` sets the PC. You can instead load it yourself with `cpcse-ctl load`, or let `--watch` do it (below).
- **Supported:** breakpoints (including conditional ones), step into, step over and step out, continue, pause, watchpoints (DeZog's `WPMEM`), and memory and register reads and edits.
- **Attaching and detaching:** when DeZog attaches, the machine stops, as MAME does. When it disconnects, the breakpoints and watchpoints it set are removed and the machine carries on.
- **Breakpoint list:** DeZog's breakpoints also appear in the emulator's **Debugger** window.
- **While DeZog has the machine stopped, it's DeZog's to run.** A load or a reload (from the API or `--watch`) still writes memory and sets the PC, but the machine stays stopped until you continue in DeZog. Otherwise it would run without DeZog knowing, and DeZog would miss the next stop.
- **Use DeZog's own run and step controls while it's attached.** Running from the emulator's Debugger window or the API also works, but DeZog won't learn that the machine is running until you pause it from DeZog.

### Conditions

`bpset` conditions use the emulator's breakpoint language, the same as the **Debugger** window: `A==&3F && HITS>2`, `[&BE80]==1`, `WORD(SP)==&0038`.

## The command API and `cpcse-ctl`

The API reads one request per line and answers each with one line of JSON. A request is either JSON:

```json
{"cmd":"load","path":"C:/work/game/build/game.bin","addr":"&4000","run":"entry","id":1}
```

or the same thing in short form, the way `cpcse-ctl` writes it:

```
load build/game.bin &4000 entry
```

- **Answers:** every answer has `"ok":true`, or `"ok":false` with an `"error"`. A request's `id`, if it has one, comes back in its answer.
- **Waiting requests:** `wait`, `frames`, `over`, `out`, `runto` and the `wait=1` forms answer when what they wait for happens. Requests sent after them on the same connection are answered in the meantime, so the answers can arrive out of order. Give requests an `id` to match them up.
- **The short form:**
  - The first words fill a command's fields in the order shown in the table below. Any field can also be given by name, as `name=value`.
  - Too many words is an error. The exceptions are `type`'s text and `bp`'s condition, which take the rest of the line.
  - A word in "double quotes" can contain spaces.
- **`type` in the short form:** in a line sent to the server, or a `cpcse-ctl -` script, `type` takes the rest of the line exactly as written, quotes included: `type PRINT "HI"\n`. On `cpcse-ctl`'s command line, your shell removes quotes first, so quote the whole text: `cpcse-ctl type 'PRINT "HI"\n'`.
- **Relative paths:** in a request, a relative path is relative to the **emulator's** working folder. `cpcse-ctl` turns relative paths into absolute ones first, so with it they're relative to where you run it.

`cpcse-ctl` (in the same folder as `cpcse`) sends one command and prints the answer. It exits with 0 if the command succeeded, 1 if the emulator refused it (the reason goes to stderr), and 2 if it couldn't reach the emulator.

```
cpcse-ctl load build/game.bin 0x4000 entry        write it to memory and start it
cpcse-ctl load game.dsk command='RUN"GAME\n'      insert a disc and run a file from it
cpcse-ctl load game.dsk reset=1 command='RUN"GAME\n'
cpcse-ctl bp add 0x4000                           a breakpoint
cpcse-ctl bp add main "A==3 && B==0"              one with a condition
cpcse-ctl wait stop                               wait until it stops (10 s at most)
cpcse-ctl regs                                    registers, as JSON
cpcse-ctl --field data read 0xC000 16             only the bytes, as hex
cpcse-ctl screenshot shot.png
cpcse-ctl - < script.txt                          one command per line; stops at the first failure
cpcse-ctl --port 7000 status
```

Options go before the command:
- `--port N`: another port.
- `--field NAME`: print only that field of the answer.
- `--quiet`: print nothing (the exit status still tells).

### Commands

| Command | Fields (the short form's word order) | Notes |
|---|---|---|
| `ping` | | `{"name":"CPCSyntaxError"}` |
| `status` | | model, paused, pc, frames, the last stop's reason, typing, the watched file, the GDB state |
| `pause` | | |
| `run` | `addr` | Continue, or start at `addr`. |
| `step` | `count` | Step into, `count` times, stopping early on a watchpoint. Answers with `pc` and `regs`. |
| `over`, `out` | | Step over or out. Answers when it stops (10 s at most, `timeout=` ms). |
| `runto` | `addr` | Answers when it gets there. |
| `reset` | | Leaves the machine running unless `run=0`. |
| `regs` | | `pc sp af bc de hl ix iy af2 bc2 de2 hl2 a f b c d e h l i r im iff1 iff2 halt flags` |
| `setreg` | `reg value` | Or `{"regs":{"hl":"&1234","pc":16384}}` to set several. |
| `read` | `addr len` | `data` as hex, or `bytes` as an array with `format=bytes`. Up to 64K. |
| `write` | `addr data` | `data` as hex (one word in the short form: `write &4000 3E01C9`), or a JSON array of bytes 0–255. |
| `load` | `path addr run` | `run` is `entry`, an address, or `no`. Also `unit` (drive 0 or 1), `reset=1`, `command` (typed; `\n` = Enter), `symbols`, and `wait=1` to answer when the typing is done. |
| `watch` | `path addr run` | The same fields as `load`. Reloads the file whenever it changes. |
| `unwatch` | | |
| `symbols` | `path` | Labels for the debugger, and for addresses in every command. |
| `type` | `text` | Typed as a person would, one key at a time (8 frames a key). `wait=1` answers when it's done. |
| `key` | `key` | One key code: `Space`, `Enter`, `KeyA`, `Digit1`, `F1`, `ControlLeft`. Add `shift=1` for SHIFT. |
| `screenshot` | `path` | `.png`, or `.bmp`. |
| `bp` | `action addr condition` | `add`, `remove`, `clear` or `list`. `remove` and `clear` leave a GDB client's breakpoints alone. |
| `wp` | `action start end mode` | `add`, `remove`, `clear` or `list`. `mode` is `r`, `w` or `rw` (default `w`). Use `len` instead of `end` for a length. A range stops at `&FFFF`. |
| `disasm` | `addr count` | Each line has `addr`, `bytes`, `text` and `label`. |
| `wait` | `for n` | `stop` (`n` = a timeout in ms, default 10000), `frames` (`n` frames), or `typed`. |
| `frames` | `n` | Answers after `n` frames. |
| `quit` | | Closes the emulator. |
| `help` | | Lists the commands. |

### Typing

- **Capitals:** typed with SHIFT, so `PRINT "Hi"` prints `Hi`. Keywords work in any case.
- **Typing waits while the machine is stopped,** and carries on when it runs again.
  - `wait=1`, `wait typed` and `wait frames` answer at once, with an error, if the machine stops while they wait. They don't sit out their timeout.
- **After `reset=1` or a cartridge,** the keys wait until the firmware has started.
- **Tapes:**
  - Loading a tape presses PLAY on the deck. The cassette motor still decides when the tape moves, as on a real CPC.
  - When a tape's command ends with Enter (`RUN"\n`), the "Press PLAY then any key" prompt is answered for you.
  - On a 6128 or 664, which start on disc, begin the command with `|TAPE\n`: `load game.cdt reset=1 command='|TAPE\nRUN"\n'`.

### A build task that loads and runs

`.vscode/tasks.json`:

```json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build",
            "type": "shell",
            "command": "sjasmplus --sld=build/game.sld --sym=build/game.sym src/main.asm && cpcse-ctl load build/game.bin 0x4000 entry symbols=build/game.sym",
            "group": { "kind": "build", "isDefault": true },
            "problemMatcher": []
        }
    ]
}
```

Or skip the load step: start the emulator with `cpcse --watch build/game.bin@0x4000 --run entry`, and every build is reloaded and restarted on its own.

## Auto-reload

From the command line (`--watch`), the API (`watch`), or *Settings → External debugging*:
- **The fields in Settings:** *Load at* is the address (empty: from the AMSDOS header). *Start at* is `entry` (also when empty), an address, or `-` for not at all. Tick *Reset first* to reset before each load.

What a reload does:
- **A watched `.bin`** is written back into memory each time the file changes, and started again if a run address was given. This works even while the machine is stopped at a breakpoint. If DeZog is attached and stopped, the PC is set but the machine stays stopped for DeZog to continue.
- **A `.sna` or `.cpr`** is loaded again.
- **A `.dsk`** is inserted again, and the command (for example `RUN"GAME\n`) is typed again. Use reset (`--reset`, `reset=1`, *Reset first*) so it's typed at a BASIC prompt rather than into the program that's still running.
- **Partly written files:** a file is reloaded only once it has stopped changing for a moment, so a build that is still writing it isn't loaded half-done.
- **Where to see it:** the status line and *Settings → External debugging* show the last reload and how many there have been.

## Plain gdb

The GDB server also answers a standard gdb client, using these packets:

- `?`, `g`/`G`, `p`/`P`, `m`/`M`, `c`, `s`
- `Z0`–`Z4` / `z0`–`z4`
- `qSupported` and `qXfer:features:read`
- `QStartNoAckMode`, `D` and `k`

Registers are numbered as in MAME's Z80 description: AF BC DE HL AF' BC' DE' HL' IX IY SP PC (the PC is 11).

`monitor` commands are the ones MAME's debugger provides:

- `print <expr>[,<expr>...]`:
  - Numbers are hex unless written `#decimal`.
  - `b@`, `w@` and `d@` read a byte, a word or a double word.
  - Registers can be used by name.
- `<reg>=<expr>` sets a register.
- `bpset <addr>[,<cond>]`, `bpclear [<id>]`, `bplist`
- `reset`
- `help`
