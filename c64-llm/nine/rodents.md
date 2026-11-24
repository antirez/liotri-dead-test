# Rodents in the Attic

The [demo page](https://www.linusakesson.net/scene/rodents-in-the-attic/)
links the author's [technical explanation](https://www.youtube.com/watch?v=uZ1atMUOUMU).
The moving characters combine sprites with black stencils. On the right,
sprites also restore the room colors, and timed `$d016` writes open the border.

The flashing black rectangle was the right border closing over the last
character column. Sprite DMA held the CPU on an opcode fetch when the raster
IRQ arrived. The CPU advanced its IRQ pipeline while stalled and accepted
the interrupt before executing that opcode. This entered the demo's raster
handler too early for its CIA timer correction, moving subsequent border
writes from cycle 56 to cycles 52 or 53.

The interrupt decision is already latched at an opcode fetch. Keeping the
IRQ pipeline in place during that stalled fetch lets the instruction finish
before servicing a newly arrived IRQ. Other stalled reads keep sampling IRQ.
Only this condition changes in `m6502.h`.

The affected instruction sequence was compared cycle by cycle with the
[perfect6502 transistor simulation](https://github.com/mist64/perfect6502).
`tests/cpu_timing.c` covers IRQ arriving during a held opcode fetch, IRQ
already pending before the fetch, and IRQ arriving during a held data read.
The first case fails with the previous CPU implementation.

The existing probe can reproduce the scene without opening a window:

```sh
cc -O2 -Wall -W $(sdl2-config --cflags) nine/probe.c \
    -o /tmp/rodents-probe $(sdl2-config --libs) -lm
/tmp/rodents-probe nine/lft-rodents-in-the-attic.prg /tmp/rodents 7491
```

A fresh-boot run through 9,000 PAL frame-length blocks was checked, including
every screenshot from blocks 7490 through 7520. The reported 8-by-8 rectangle
at `(344,81)` contained 64 or 16 black pixels in failing captures before the
fix, and none in these 31 captures after it. Border-opening writes on raster
lines 94 through 97 consistently moved to cycle 56. The CPU test also passes
under AddressSanitizer and UndefinedBehaviorSanitizer.
`make test` and `make macos` pass. A 9,000-block headless run of Nine still
completes and returns to BASIC.
