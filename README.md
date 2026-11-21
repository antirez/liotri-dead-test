# Liotri C64 hardware test

Liotri is an EasyFlash diagnostic cartridge for the Commodore 64. It boots
with all three system ROMs missing and checks the low 4K of RAM before its
first subroutine call. When RAM or an address multiplexer fails, the VIC can
read both the fault message and its font directly from the cartridge.

On a healthy machine the dashboard shows the quick checks first, then maps
the RAM sweep one page at a time. After a clean pass, the eight test sprites
orbit a live trace of SID voice 3 while the SID plays a three-voice tune.

![Liotri running a memory sweep and displaying SID voice 3](docs/liotri.gif)

## Run the cartridge

The ready-to-use image is [liotri.crt](liotri.crt). On a physical C64,
program the image into an EasyFlash-compatible cartridge, enable its boot
jumper, and power on. **The tests overwrite C64 RAM.** RESET starts a fresh
run and clears the recorded faults. The 586220 harness enables the optional
external port checks.

For a simulated run, open the image in an emulator that supports EasyFlash.
These examples use an EasyFlash-capable build of
[c64-hw-simulator](https://github.com/antirez/c64-hw-simulator).

```sh
c64-hw-sim liotri.crt
c64-hw-sim --harness liotri.crt
c64-hw-sim --fault U5:removed liotri.crt
```

The timings and fault reports below were verified on the simulated PAL ASSY
250407. Physical-board validation is still pending. The cartridge uses the
standard EasyFlash reset mapping. [EasyFlash Programmer’s Guide](https://skoe.de/easyflash/files/devdocs/EasyFlash-ProgRef.pdf)

## Reading a run

The left column reports measurements as they finish. Two dots mean pending.
On the simulated board, the low RAM, address lines, color RAM, CIA timers
and interrupt outputs, SID readbacks and VIC collision checks are visible by
frame 25, about half a second. The ROM checks finish around 1.3 seconds and
the first full memory result appears by 8.4 seconds. Wall-clock time
depends on the speed of the simulator.

The grid is a physical address map. The row and column are the two hex
digits of the page number. Row D, column 3 covers $D300–$D3FF, including
the RAM behind the VIC register window. The first row shows the low 4K
qualified on startup.

| Map tile | Measurement |
|---|---|
| Dot | Waiting for this pass |
| Stripes | Wrote the initial pattern |
| Inset square | Checked the pattern and wrote its inverse |
| Solid square | Final read completed |
| Red cross | A read from this page differed on this or an earlier pass |

`FIRST` holds the first failing address from a deep sweep. `BITS` accumulates
the XOR of expected and observed data with a bitwise OR over all failures.
For example, `FIRST $D34A   BITS 05` means the first mismatch occurred at
$D34A and differences have occurred on D2 and D0. Both fields and the red
page marks survive subsequent passes. After a sparse address-bus failure, the cartridge
finishes the quick checks, inhibits the deep writes, and switches to a second
ROM-backed screen. Nine flashes identify this case. The highlighted address
is the first conflicting probe, or the base if the returned tag was invalid.

On a clean pass, the memory map briefly becomes a large SID scope. Its
sixteen columns sample the actual OSC3 register. The eight hardware sprites
orbit the trace while a triangle bass, pulse arpeggio and sawtooth lead play
on the SID. The lower strip shows 32 more OSC3 samples and an ENV3 color
indicator. A physical SID drives the audio output. The test simulator
displays the visual instruments and has no host speaker output.

## When the stack or screen RAM is broken

Liotri runs with the BASIC, KERNAL and character ROM sockets all empty.
The first dashboard font comes from the cartridge. The VIC fetches it from
qualified RAM. This also supplies the sprite test with known foreground pixels
before U5 has been checked.

![Liotri after a full RAM pass with BASIC, KERNAL and character ROMs removed](docs/liotri-no-roms.png)

The cartridge takes the reset vector in Ultimax mode. The 6510 walks ones
and zeroes through a single byte at $02. It then writes address-dependent
patterns and their complements throughout $0002–$0FFF before the first JSR.
$0000 and $0001 are the 6510’s on-chip direction and port registers.

A mismatch takes a stackless reporting path. The VIC reads both the screen
matrix and its font from cartridge ROM. The highlighted rows show differing
data bits. The screen and physical cartridge LED repeat the flash count for
the highest reported bit. Counting works even when the text or colors are
unreadable. The table uses the 250407 board designators.

| Flashes | Bit | DRAM | Flashes | Bit | DRAM |
|---:|---|---|---:|---|---|
| 1 | D7 | U12 | 5 | D3 | U10 |
| 2 | D6 | U24 | 6 | D2 | U22 |
| 3 | D5 | U11 | 7 | D1 | U9 |
| 4 | D4 | U23 | 8 | D0 | U21 |

A row identifies a data path worth probing. DRAM inputs, outputs, address
multiplexers and control signals can produce similar mismatches. Later C64
boards use different DRAM organizations. A machine needs enough clock,
reset, CPU, PLA and cartridge-bus functionality to fetch instructions.
Video reporting also needs a functioning VIC output path.

## What the checks exercise

| Check | Operation |
|---|---|
| Address bus | Write 17 distinct tags at a base address and all sixteen single-bit neighbours, then read them back with cartridge, ROMs and I/O switched out. |
| Deep RAM | First sweep writes a tag derived from both address bytes and a seed across $1000–$FFFF, verifies and inverts it in descending order, then verifies the inverse in ascending order. Subsequent passes alternate address patterns with a six-element March C− on this 60K. The low 4K holds the qualified workspace and display. |
| Color RAM | Two full writes and separate verification reads of all 1,024 nibbles. The tag folds all ten address bits into four data bits. Only D0–D3 are compared. |
| ROMs | CRC-16/CCITT-FALSE over the complete CPU-visible images. Stock BASIC is A512, character ROM F542 and KERNAL revision 3 304F. DIFF means a different signature. A legitimate replacement ROM can produce it. The dashboard starts with its cartridge font. Only a matching character ROM checksum enables copying stock glyphs to the same screen font RAM. The VIC can display every result with U5 removed. |
| VIC-II | Bounded raster progress check. All eight sprites overlap each other and a solid background target. Repeat with alternating sprites enabled, check the read-to-clear latches, then separate the sprites and require the overlap to disappear. |
| CIA1 and CIA2 | Timer A underflows clock timer B in one-shot mode. Check the source bits and read-to-clear behavior, then deliver actual timer interrupts to ROM IRQ and NMI handlers. Both time-of-day clocks must move. Raster and device waits are bounded. |
| SID | Hold voice 3’s sawtooth with TEST, release it and require movement on OSC3. Gate the envelope and require ENV3 to rise and fall. The audible phrase exercises all three voices. Analog output, individual audio channels and filters require listening or probing. |
| 586220 harness | Look for mirrored user-port PB pairs in both directions. When detected, independently exercise keyboard and joystick paths using the motor-controlled switch, both paddle pairs through U28, IEC ATN-to-CLK transitions and the cassette SENSE-to-READ edge on CIA1 FLAG. |

The March C− sequence is **write 0**, **up read 0 / write 1**, **up read 1 /
write 0**, **down read 0 / write 1**, **down read 1 / write 0**, **read 0**.
The test uses $00 and $FF byte backgrounds. Graphics, music and status
updates run between pages, after the normal memory map has been restored.
The VIC collision verdict comes from its hardware latches. [Commodore 64 Programmer’s Reference Guide, graphics](https://www.commodore.ca/wp-content/uploads/2018/11/c64-programmers_reference_guide-03-programming_graphics.pdf)

`USER LOOP OPEN` is expected without a harness. A partly working set of PB
pairs produces BAD. When a loop is recognized, the strip shows individual
KEY, JOY, POT, IEC and TAPE results with + for pass and ! for fail. Loss of
all identifying pairs can look like an unplugged harness.

## Build and verify

To rebuild from source, run `make -B` in the repository root with GNU Make
and 64tass on your path. The release was assembled with 64tass
1.60.3243. You can select another installation with
`make -B TASS=/path/to/64tass`.

The entry point is [src/liotri.asm](src/liotri.asm). It includes the font,
sprite data and harness tests. The assembler emits the 64-byte CRT header,
two 8 KiB EasyFlash CHIP packets, and the CPU vectors directly into
`liotri.crt`. The cartridge contains its own repair font.

The regression suite uses Python 3 and a separate EasyFlash-capable build of
c64-hw-simulator. By default it runs `c64-hw-sim` from your path. To select
a local build and test the supplied cartridge, run the following from the
repository root.

```sh
C64_HW_SIM=/path/to/c64-hw-sim python3 tests/liotri.py
```

The suite reads the monitor output and flash reports. It exercises missing
ROMs, interrupted data and address paths, SID and CIA failures, broken harness
contacts, and a deliberately changed byte under I/O. With the character ROM
removed, it decodes screenshots using the cartridge’s own glyphs.

## License

Liotri’s source code and cartridge image are released under the [MIT License](LICENSE).
