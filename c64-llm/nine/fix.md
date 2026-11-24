# Nine: fix and validation

The repair keeps the existing VIC renderer and cycle switch. There are no
demo-specific addresses, timing overrides or chip-detection patches.

## Causes and changes

- **Raster timing:** the raster counter and IRQ comparison advanced during
  cycle 63 instead of cycle 1. Correcting this lets Nine's startup synchronization
  settle on the intended timing: its `$d017` crunch write now reaches cycle 15
  instead of cycle 12.
- **Sprite crunch and lifetime:** a cycle-15 Y-expansion clear mixes the bits
  of MC and MCBASE. Cycle 16 then copies MC into MCBASE instead of approximating
  the transfer with `+2`, then `+1`. Nine's four stabilizers now survive the
  required counter wraps, keeping sprite DMA and the raster routine stable.
  DMA starts are checked at both cycles 55 and 56; display is disabled at
  cycle 58 so the final sprite row is retained.
- **Black cover sprites:** the PAL sprite X counter wraps after 503. The
  old calculation put X=480 beyond the last raster cycle. Sprite positions
  now wrap, and sequencers advance outside the host's cropped picture.
  The offscreen start of the black cover sprite therefore covers the left edge.
- **Border sampling:** the existing left/right comparators now run after the
  CPU register write, before pixel output. Nine's cycle-56 `$d016` write can
  keep the side border open. Fetches and sprite counters retain their existing
  position before the write.
- **Debugger startup:** direct machine-code launches now enable IRQs, as BASIC
  `SYS` normally does. Nine's depacker needs KERNAL interrupt service during
  startup; disabling IRQs left a CIA interrupt pending and caused an IRQ loop.

The author's [Nine Explained](https://www.linusakesson.net/scene/nine/explanation.php)
and [sprite-crunch research](https://www.linusakesson.net/scene/lunatico/misc.php)
were used to understand these requirements. VICE's
[PAL cycle model](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/viciisc/vicii-chip-model.c)
and [sprite counter behavior](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/viciisc/vicii-cycle.c)
were checked as references.

## Validation

`make test` checks raster IRQ timing, known crunch transitions, ordinary and
Y-expanded sprite rows, the crunched sprite lifetime, both DMA-start checks,
a wrapped black cover sprite and a cycle-56 border write. These tests also
pass with AddressSanitizer and UndefinedBehaviorSanitizer; unsigned sprite
masks avoid the existing signed shifts into bit 31. `make macos` builds the
frontend with audio support.
Header dependencies now cause `make noaudio` to rebuild after core edits.

The headless probe runs the same core, ROMs and loader as the frontend:

```sh
mkdir -p /tmp/nine-probe
cc -O2 -Wall -W $(sdl2-config --cflags) nine/probe.c \
    -o /tmp/nine-probe/probe $(sdl2-config --libs) -lm
/tmp/nine-probe/probe nine/lft-nine.prg /tmp/nine-probe/basic 9000
/tmp/nine-probe/probe nine/lft-nine.prg /tmp/nine-probe/direct 9000 direct
```

Each block is exactly 19,656 PAL cycles; screenshots are sampled every 500
blocks and at 3800. Both launch paths reach the ending and return to BASIC.
The samples cover the wide circle, upper-border effect, greetings and descent.

A frame in the upper-border scene was also matched against VICE `x64sc`
using sprite positions and the demo's animation state. Both emulators selected
the same chip-timing branch. After mapping palettes and accounting for a
one-row picture-origin difference, only 14 pixels differed in the compared
380-by-78 upper-border region. This is not a claim of pixel-perfect VIC
emulation; small color-edge differences remain. It supports retaining the
current renderer instead of adding a new pixel pipeline for this repair.

Interactive launch:

```sh
./c64-llm --run nine/lft-nine.prg
```
