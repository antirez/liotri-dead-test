# Nine: measured failures at checkpoint 3c2a1fd

Investigation date: 2026-09-23. The checkpoint preserves the previously pending
VIC changes, macOS activation shim, demo and notes. `make macos` passed.
This is a historical record of the unrepaired checkpoint. See
[the implemented fix and validation](fix.md) for the current behavior.
In particular, later comparison with VICE did not justify the proposed
graphics-pipeline rewrite below; correcting the timing and sprite logic
was sufficient to make Nine run well.

## Reproduce

From the repository root, build the headless probe using the same core, ROMs,
loader and PNG writer as the frontend:

```sh
mkdir -p /tmp/nine-probe
cc -O2 -Wall -W $(sdl2-config --cflags) nine/probe.c \
    -o /tmp/nine-probe/probe $(sdl2-config --libs) -lm
/tmp/nine-probe/probe nine/lft-nine.prg /tmp/nine-probe/basic 6000
```

This boots to READY, loads the PRG and injects BASIC `RUN`. It executes 19,656
cycles per numbered block, saving screenshots every 500 blocks and at 3800.
It logs all `$d017` writes, plus VIC register writes in the sampled blocks.
The CSV calls the block number `frame`; these are exact PAL-length intervals
at the initial boot phase, not captures synchronized to vertical blanking.
Screenshots can therefore contain portions of consecutive video frames.
`line` and `cycle` are the core's post-tick `v_count` and `h_count`, not an
independently calibrated hardware clock. `pc` is the CPU's live PC after the
tick, which is usually past the opcode of a store.

Interactive reproduction uses:

```sh
./c64-llm --run nine/lft-nine.prg
```

## 1. Lost sprite crunch breaks the upper-border timing

The strongest finding is an actual loss of the DMA that keeps the raster
routine synchronized. In block 6000 of the ordinary BASIC launch:

| Raster line | Cycle | Write | Sprite DMA mask | Sprite 0 MCBASE |
| --- | --- | --- | --- | --- |
| 11 | 36 | `$d017 = $35` | `$35` | 6 |
| 12 | 12 | `$d017 = $00` | `$35` | 6 |
| 26 | 19 | `$d011 = $17` | `$ff` | 51 |
| 29 | 19 | `$d011 = $17` | `$ff` | 60 |
| 30 | 19 | `$d011 = $17` | `$ca` | 63 |
| 31 | 16 | `$d011 = $17` | `$ca` | 1 |
| 32 | 13 | `$d011 = $17` | `$ca` | 2 |

`$ff -> $ca` removes exactly `$35`: sprites 0, 2, 4 and 5, the four
stabilizers. The counters reach the ordinary termination value instead of
wrapping through a long crunched sprite. The repeated mode writes then move
from cycle 19 to 16 to 13. This is execution timing drift, not just misplaced
pixels. At block 5500, a stretch of the effect has no sprite DMA at all;
successive mode changes spill into different positions and even different
numbers of raster lines.

The corresponding shortcomings in [m6569.h](../m6569.h) are concrete:

- `_m6569_write`, case `$17`, only updates the expansion flip-flops. It never
  applies the special mixed-bit MC/MCBASE crunch operation.
- `_m6569_sunit_update_mcbase` and `_m6569_sunit_dma_disp_disable` implement
  the older simplified `+2`, then `+1` recipe. Their comments specify cycles
  15 and 16, but `_m6569_tick` calls them in cases 16 and 17.
- The actual clear in this run arrives at internal cycle 12. Adding a
  cycle-15 special case alone would consequently miss Nine's write. The
  earlier timing discrepancy must also be resolved. Its upstream cause
  (including the demo's CIA timer synchronization) is not isolated yet.

The author's [sprite-crunch research](https://www.linusakesson.net/scene/lunatico/misc.php)
and VICE's [D017 implementation](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/viciisc/vicii-mem.c)
describe the mixed-bit operation. VICE's
[counter update](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/viciisc/vicii-cycle.c)
transfers MC into MCBASE at cycle 16. These references establish why simply
moving the existing `+2/+1` calls is insufficient.

### Controlled experiment

This experimental mode exists in commit `1ce7889` only. It was removed from
the current probe once real sprite crunch was implemented.

```sh
/tmp/nine-probe/probe nine/lft-nine.prg /tmp/nine-probe/forced 6000 force-crunch
```

This diagnostic mode accepts the mistimed expansion clear after block 5000
and injects the intended counter change, compensating for this core's later
`+3`. It is deliberately **not** a valid hardware implementation or a proposed
production patch.

The stabilizers now remain active through the relevant rows. At block 6000,
lines 24 through 30 keep DMA mask `$ff`, and the `$d011` pairs stay at cycles
21/39 instead of drifting. Much of the horizontal streaking disappears and
the sprite digits remain recognizable. Fine detail and alignment remain
incorrect. This isolates the missing stabilizer DMA as a major contributor;
it does not validate all remaining timing. The intervention also changes
subsequent execution, so screenshots at equal block counts need not have
identical animation positions.

## 2. The graphics and color pipelines are insufficient

I fetched and read the author's
[Nine Explained technical writeup](https://www.linusakesson.net/scene/nine/explanation.php),
as well as the [demo page](https://www.linusakesson.net/scene/nine/).
The writeup specifically requires chip-dependent pixel latency, a one-pixel
background-color split offset, and timed invalid-mode switching. Those
requirements explain why stabilizing the sprite DMA is only part of the job.

In the current core:

- `_m6569_g_i_access` returns a byte which `_m6569_decode_pixels` uses in the
  same tick. There are no intervening graphics fetch pipeline stages.
- `_m6569_write` changes the graphics mode and background colors just before
  the eight pixels of that tick are decoded. A color store affects the whole
  group; there is no pixel-specific register latency.
- Horizontal scroll initializes the shifter at cycle 16. It does not supply
  the independent fetch/mode/color timing needed for this effect.

VICE's [cycle renderer](https://github.com/VICE-Team/svn-mirror/blob/main/vice/src/viciisc/vicii-draw-cycle.c)
maintains separate graphics-data, mode, scroll and color state. It is useful
as a reference for the missing distinctions, not as evidence that any one
constant delay transplanted into this core would be correct.

The `$3fff`/`$39ff` address selection itself **already exists** in
`_m6569_io_update_memory_unit`. Invalid modes 5–7 also already produce black
in this checkpoint. Neither feature can be described as wholly missing.
The remaining task concerns when fetched data, mode selection and colors
reach the output. Collision-based chip detection must be checked against
the resulting timing model, rather than forcing the demo to a preferred path.

## 3. The earlier left-border leak is separately reproduced

Block 3800 shows blue reaching the left edge while the wide sprite circle
is still below the top border. At this point `$d011 = $4b`, the VIC is idle,
and the vertical border flag is clear. For example, line 71 writes
`$d016 = $00` at internal cycle 55; line 72 restores `$08` at cycle 15.
The main border is open across the intervening line boundary.

Thus side-border opening is happening. The symptom is not adequately
explained by saying that the emulator never opens the border. The black
cover sprites, sprite/background alignment and border sampling need a
separate comparison. I have not isolated which one causes this leak.
The `last_bg` path for a *set* vertical border flag is not an explanation for
these particular traced rows, where that flag is clear.

The sprite BA schedule also already combines neighboring sprite requests;
it is not wholly missing the bridging described by the author. Its exact
phasing and the sprite start checks still need validation.

## 4. Debugger launch is a separate failure

```sh
/tmp/nine-probe/probe nine/lft-nine.prg /tmp/nine-probe/direct 6000 direct
```

This uses `llm_start_machine_code`, the same shortcut as debugger `RUN file`
and `RUNSYS file`. It produces a blank blue display instead of the normal
intro. BASIC `RUN` reaches the intro and all the later scenes above.

The shortcut resets A/X/Y, status and the stack and jumps directly to the
parsed SYS address. It is not equivalent to BASIC's SYS calling context.
The exact dependency responsible for Nine's blank display is not yet
isolated; the different outcome is established. Use the BASIC launch for
further VIC investigation so this does not confound the results.

## Repair order supported by the evidence

1. Calibrate the CPU/CIA/VIC cycle relationship at the crunch write and
   implement the real MC/MCBASE behavior. Verify that `$35` DMA persists
   throughout the upper-border device and the raster routine stops drifting.
2. Model graphics fetch, mode selection and color output timing separately.
   Validate startup chip detection and the ghostbyte effect under that model.
3. Diagnose the earlier side-border black-cover failure independently.
4. Address the debugger's launch-context discrepancy separately.

Use general VIC conformance tests as well as Nine before accepting a core
change. Also force a rebuild after header edits: the current `noaudio` binary
target depends on `c64-llm.c`, not on `m6569.h` or the other chip headers.
