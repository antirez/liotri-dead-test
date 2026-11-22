# c64-llm

`c64-llm` is a Commodore 64 emulator harness tailored for LLM-driven game
porting/debug workflows.

It provides:

- SDL interactive mode (default)
- `--llm` socket debug control (works in both headless and SDL-hybrid modes)

## Build

No audio:

```bash
make noaudio
```

### DRAM-test cartridge

Build a standard 8 KiB generic cartridge image with:

```bash
make dram-test-crt
```

This writes `dram-test.crt`. It autostarts on a C64 or in VICE, tests the accessible
DRAM with `$55`/`$AA` patterns, and updates `TESTING $hhhh` once per 256-byte page.
The ROM reserves zero-page `$00-$01` and `$FB-$FE` plus screen RAM `$0400-$07FF`, so
the live status display and its working pointer remain usable. As with any fixed 8 KiB
cartridge, RAM underneath its `$8000-$9FFF` ROM cannot be tested without hardware that
can disable the cartridge. A failure leaves its address on screen; a complete pass
shows `PASS - DRAM OK`.

With audio:

```bash
make macos
# or
make linux-alsa
# or
make linux-pulseaudio
```

SID keyboard sketch tool:

```bash
make sid-keys
```

`make test` runs headless regressions. See [SID emulation](docs/sid.md) for
the hardware references, current limitations and offline WAV rendering.

On macOS, the `macos` target also links a small AppKit activation shim so the
SDL window is explicitly brought to the foreground when launched from Terminal.

## SDL Mode (default)

```bash
./c64-llm
./c64-llm path/to/program.prg
./c64-llm --run path/to/program.prg
./c64-llm --bas path/to/program.bas
```

When a PRG path is provided, the emulator queues it and loads it when BASIC is
ready (to avoid early-boot memory overwrite). Wait for:

```text
Loaded <file> at $xxxx (BASIC READY)
```

then type `RUN` in the emulated C64.

With `--run` in SDL mode, startup is automated:

- emulator fast-forwards boot as fast as possible (no realtime delay),
- waits for BASIC READY,
- loads the PRG,
- injects `RUN`,
- then switches to normal paced rendering.

`--bas` loads a text listing of a C64 BASIC V2 program, tokenizes it in memory,
and runs it after boot. No separate `--run` is needed. It also works with
`--llm`, where the program starts running automatically without a window:

```bash
./c64-llm --llm --bas path/to/program.bas
```

Use numbered lines (0–63999), with LF or CRLF line endings. Lines may be out
of order; repeated numbers replace earlier lines, and a bare number deletes
the line. ASCII letters are converted to the C64's uppercase/graphics character
set, including inside strings. Blank lines and an initial UTF-8 BOM are accepted.
Each tokenized line may contain up to 250 bytes; the whole program must fit in
normal BASIC memory.

PETSCII characters can be written as decimal `{147}` or hexadecimal `{$93}`
escapes. Common names also work: `{clear}`, `{home}`, `{up}`, `{down}`, `{left}`,
`{right}`, `{return}`, `{delete}`, `{insert}`, `{rvon}`, `{rvof}`, `{pi}` and
color names such as `{white}` or `{light blue}`. Use numeric escapes for other
graphics characters or literal braces. `CHR$()` works as usual.
See [BASIC loader checks](docs/basic.md) for compatibility tests and sample programs.

SDL joystick convenience mapping (port 2):

- `Up/Down/Left/Right` -> joystick directions
- `Tab` -> joystick fire button

This mapping is for gameplay testing and does not use `Space` (kept free for
normal keyboard input).

## SID Keys Tool

`sid-keys` is a standalone SDL tool for quickly sketching 2-voice melodies
using the same SID core (`m6581.h`) used by the emulator.

Run:

```bash
./sid-keys
./sid-keys --bpm 120 --ticks 96 --silence-ms 2500
```

Keyboard mapping:

- Upper white keys (`C3..E4`): `Q W E R T Y U I O P`
- Upper black keys (`C#3..D#4`): `2 3 5 6 7 9 0`
- Lower white keys (`C2..E3`): `Z X C V B N M , . /`
- Lower black keys (`C#2..D#3`): `S D G H J L ;`

Behavior:

- Maximum 2 simultaneous notes (one per voice).
- If 3+ mapped keys are held, the oldest two stay active; when one is released,
  the next held key is promoted automatically.
- A metronome click is played continuously at `--bpm` (default `120`).
- After a few seconds of silence, phrases are printed to stdout as:
  - one output block per bar (`4/4`)
  - each block contains only:
    - `voice1: ...`
    - `voice2: ...`
- Timing is metronome-aligned:
  - phrase starts at the start of the bar where you first play
  - phrase ends at the end of the bar containing your last event
  - output is quantized in 4-tick slots on this bar grid
- Printed voices are canonicalized into stable low/high lanes, so short note
  alternations stay on the same musical hand instead of bouncing between
  `voice1` and `voice2`.
- Ticks per 4/4 bar are configurable with `--ticks` (default `96`).
- Short accidental overlaps are ignored for voice 2: if overlap with voice 1
  is shorter than 3 ticks, it is treated as a single-voice transition.
- `Space` flushes at the end of the current bar.

## Hybrid SDL + LLM Mode

Run SDL gameplay and LLM socket control at the same time:

```bash
./c64-llm --llm --sdl --llm-socket /tmp/c64-llm.sock path/to/program.prg
```

- Hybrid mode starts in `RUNNING` state so you can reproduce bugs manually.
- Manual debug-freeze hotkey: `Ctrl+Shift`.
- Attach debugger client from another shell:

```bash
nc -U /tmp/c64-llm.sock
```

## LLM Mode

Start headless server:

```bash
./c64-llm --llm --llm-socket /tmp/c64-llm.sock
```

Connect from another shell:

```bash
nc -U /tmp/c64-llm.sock
```

### Protocol Commands

- `RUN [prg_path]`
  - `RUN <file>`: reset+READY, quickload PRG, then start machine code
    (uses BASIC `SYS` stub address when available)
  - `RUN`: resume execution from current state
  - With a file path, `RUN` now performs a reset and waits for BASIC READY
    before loading, to avoid boot-race corruption.
- `RUNSYS <prg_path|addr>`
  - Deterministic machine-code start:
  - `RUNSYS <file>`: reset+READY, quickload PRG, parse BASIC `SYS` stub,
    then jump to machine-code entry
  - `RUNSYS <addr>`: jump directly to address and run
  - With a file path, `RUNSYS` also performs reset+READY wait before loading.
- `JUMP <addr>`
  - Set CPU PC to address (`$c000`, `0xc000`, or decimal)
- `JOY1 <mask>`
- `JOY2 <mask>`
  - Set joystick mask for port 1/2 (`UP=1`, `DOWN=2`, `LEFT=4`, `RIGHT=8`,
    `BTN=16`, combine with sum/bit-or)
- `KEYDOWN <key>`
- `KEYUP <key>`
- `KEY <key>`
  - Press/release one key (tap)
- `TYPE <text>`
  - Type ASCII text as key taps
- `DUMP`
  - Dump CPU registers/counters + current and predicted-next 6502 instruction
- `CPUINT`
  - Dump CPU interrupt-related internal state (`cpu_pins`, `sys_pins`,
    IRQ/NMI pipelines, `brk_flags`, current `IR`, status `P`)
- `CIA 1|2`
  - Dump internal CIA timer/interrupt state for CIA1 or CIA2
- `CIATRACE [N]`
  - Show the most recent traced CIA timer/control register accesses
    (currently focused on CIA1 timer-A fire-SFX debugging)
- `DISASM [addr] [count]`
  - Disassemble one or more instructions (default: current PC, count=1)
- `TEXT [x y w h]`
  - Dump VIC text screen content as readable ASCII approximation
  - Includes mode metadata (`text` vs `bitmap`) and screen/charset base
  - Optional window is in text cells (`0..39`, `0..24`)
- `MEMGET <addr> <len>`
  - Read memory bytes (CPU-visible memory view)
- `MEMSET <addr> <b0> [b1 ...]`
  - Write one or more bytes
- `RESET`
  - Reset machine and wait for BASIC READY in stop mode
- `STOP`
  - Enter stop mode
- `NEXT`
  - Step one instruction (stop mode)
- `FRAME`
  - Step one PAL frame (20ms emulation)
- `BREAKPOINT LIST`
- `BREAKPOINT DEL <id>`
- `BREAKPOINT PC <addr>`
- `BREAKPOINT READ <addr>`
- `BREAKPOINT WRITE <addr>`
- `PSTART`
  - Enable instruction-hit profiler (`profile[PC]++` per executed instruction)
- `PSTOP`
  - Disable profiler counting
- `PRESET`
  - Reset all profiler counters (65536 addresses)
- `PTOP [N]`
  - Show top hotspot addresses (default 16, max 1024), with disassembly
- `AUDIORESET`
  - Reset internal audio capture ring/metrics used by `AUDIOSTAT`
- `AUDIOSTAT [window_samples]`
  - Report `max_abs`, `rms`, and `mean_abs` over recent audio samples
    (default window `4096`, capped by current captured sample count)
- `SCREENSHOT <path> [x y w h]`
  - If `path` ends with `.png`: save an RGB PNG (`PNG_RGB24`)
  - Otherwise: save a text screenshot in compact 16-color charmap format
    (`C64_16COLOR_CHARMAP`)
  - Optional crop window lets you export just a sub-rectangle of the screen
    (useful to avoid large dumps)
- `QUIT`
  - Stop emulator process

Key tokens accepted by `KEY*`:

- Single ASCII char (example: `A`, `z`, `1`, `+`)
- Names: `SPACE`, `RETURN`/`ENTER`, `DEL`/`BACKSPACE`, `HOME`,
  `LEFT`, `RIGHT`, `UP`, `DOWN`, `F1..F8`, `STOP`, `RUN`, `CTRL`,
  `CBM`, `RESTORE`
- Numeric keycode (`$xx`, `0xxx`, decimal)

### Breakpoint Behavior

In headless mode, when running (`RUN`) and a breakpoint is hit, emulator
switches to stop mode and emits a line like:

```text
STOPPED BREAKPOINT id=3 type=PC addr=$0810 pc=$0810
```

In hybrid SDL+LLM mode, prefer manual freeze (`Ctrl+Shift`) then use
`NEXT`/`FRAME` for deterministic stepping.

## Notes

- In headless mode (`--llm`, `--llm --headless`, or `--llm --no-sdl`),
  emulation starts in `STOPPED` state, except `--bas`, which loads and runs its program.
- In hybrid mode (`--llm --sdl`), emulation starts in `RUNNING`
  state; use `Ctrl+Shift` or `STOP` to freeze.
- Use `RUN` to resume.
- The socket is AF_UNIX and single-client.

## Screenshot Formats

### PNG (`PNG_RGB24`)

Use a `.png` output path:

```text
SCREENSHOT /tmp/view.png 96 64 200 144
```

This is useful for image-model inspection workflows (for example:
`./describe_image.sh /tmp/view.png`).

The repository includes `describe_image.sh` at the top level for this purpose.

### Text (`C64_16COLOR_CHARMAP`)

`SCREENSHOT` now writes one character per pixel, using symbols `0..9A..F` to
represent up to 16 colors:

- Header includes `WIDTH`, `HEIGHT`, `ORIGIN_X`, `ORIGIN_Y`
- `C<symbol> <RRGGBB>` lines define symbol-to-color mapping
- Each row is `YNNN <chars...>`

Example:

```text
C64-LLM-SCREENSHOT V2
WIDTH 64
HEIGHT 32
ORIGIN_X 120
ORIGIN_Y 80
FORMAT C64_16COLOR_CHARMAP
SYMBOLS 0123456789ABCDEF
PALETTE_SIZE 3
C0 000000
C1 706DEB
C2 F7FF9F
Y000 1111111111111111...
Y001 1111111222211111...
```
