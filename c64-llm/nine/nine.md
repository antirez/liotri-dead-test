# Nine: emulator-focused notes

Nine now runs through to completion. See [the fix and validation](fix.md)
for the current implementation, and [the measured investigation](investigation.md)
for failures at checkpoint `3c2a1fd`. The notes below are the earlier working
hypotheses, not a description of the repaired core.

This file is a repair-oriented synthesis of:

- Linus Akesson's `Nine Explained` page
- a local video transcript extracted from `nine/nine_video_trascript.txt` before deletion
- quick local observation of `lft-nine.prg` in this emulator

The goal is not to explain the demo in general. The goal is to capture what the emulator must get right for `Nine` to render correctly.

## What looks broken here right now

Local frame sampling in `c64-llm` suggests:

- Simple sprite-only scenes are mostly coherent.
- The raster-critical border effects are not.
- There are two apparently separate failures, not one.
- First failure: when the digits expand into the side borders, while they are still on different Y positions and not yet in the top border, the left border turns blue instead of staying black.
- Second failure: once the effect moves into the top border, the digits are still vaguely recognizable as numbers, but the whole construction falls apart as if the timing is no longer landing on the intended cycles.

Diagnostic reading of those symptoms:

- The first failure points to side-border opening and/or the black-cover logic for that scene. That is earlier than the full ghostbyte-shine-through trick.
- The second failure points to the true upper-border effect: idle graphics, ghostbyte source changes, `$d011/$d016/$d021` timing, and sprite DMA stability.
- In other words, the emulator likely has one bug in the "ordinary but still cycle-critical open border" path, and another in the much tighter top-border device path.

## The important scene changes

`Nine` is deliberately deceptive. Different parts that look like one continuous technique are actually implemented in different ways. That matters for debugging because a scene can fail for a very specific hardware reason.

### 1. Intro / ordinary-looking part

- The wizard initially really is character graphics from the ROM font.
- This trains the viewer to assume the wizard is always text. That assumption later becomes false.

### 2. Wide circle, side borders open

- This is sprite multiplexing plus open side borders.
- The main text area is actually turned off here; what looks like the wizard is rendered with sprites.
- The wizard is built so it can be drawn either with PETSCII/text or with two stacks of X-expanded sprites plus timed color changes.
- Four static sprite columns are used as timing stabilizers and black covers.
- Three freely moving hardware sprites are then enough for the moving digits while the circle is still wide and sparse.

Why this matters for the emulator:

- Side-border opening must be cycle-correct enough for real raster code.
- Sprite DMA stealing must be accurate enough that the "stabilized" timing is actually stable.

### 3. Flat circle, all nine digits on the same raster lines

- One digit is no longer a hardware sprite.
- The backmost digit is drawn using a 3x3 custom character font.
- The text area is fine-positioned for that digit, then reset lower down so the wizard renders correctly.

Why this matters:

- A failure here does not imply a sprite limit problem.
- If this scene works while the upper-border scenes fail, the issue is likely in border/idle/timed VIC behavior, not general charset rendering.

### 4. The "impossible" upper-border effect

This is the core scene to target.

What is really on screen:

- Six digits are still sprite-based.
- Of those six, two are ordinary sprites.
- Four are sprite-crunched tall sprites used mainly to stabilize DMA timing while covering more vertical area.
- The remaining three visible digits are faked by a two-sprite "device" plus idle graphics and timed color/mode changes.

This is not nine ordinary hi-res sprites.

## The specific VIC-II behavior Nine depends on

### Sprite DMA timing must be very accurate

From the article:

- A PAL raster line has 63 cycles.
- Up to 16 line cycles can be stolen for sprite fetches.
- The VIC cannot pause the CPU instantly; it warns about 3 cycles early.
- During that warning window, CPU writes can still happen, but reads stall.
- If sprite fetch groups are close enough, the VIC keeps the bus instead of releasing it and warning again.
- This means some cycles are stolen even when the middle sprite is disabled; those "bridging" cycles are essential to the timing trick.

Why it matters:

- `Nine` relies on constant-on neighbor sprites to stabilize the bus timing for moving sprites.
- The code also has a special case when CPU write cycles coincide with sprite DMA start.
- If BA/RDY/AEC handling is even slightly off, timed raster code shifts and the whole illusion collapses.

### Sprite priority is part of the illusion

- Lower-numbered sprites appear in front of higher-numbered sprites.
- The multiplexer remaps logical digits to hardware sprites to preserve the intended depth order.
- In the side-border scene, hardware sprite choice is constrained by which black cover columns would otherwise overlap it.

Why it matters:

- Wrong sprite priority or bad remapping timing will produce "almost right" but visibly wrong overlap ordering.

### Sprite crunch must behave like real hardware

The four tall stabilizer sprites in the upper-border part are not simply Y-expanded:

- They are crunched once just after appearing.
- That deliberately misaligns the internal sprite data counter.
- As the sprite advances by 3 bytes per raster line, the misalignment makes it wrap twice before the sprite ends.
- The result is effectively triple-height coverage without expensive Y-register rewriting during the tight raster section.

The code then updates sprite pointers mid-effect:

- Pointer sets are prepared in different video-memory banks.
- `$d018` is changed at specific lines to swap all relevant sprite pointers at once.
- The switch points move with the device Y position.

Why it matters:

- Emulate sprite crunch, sprite internal address counters, pointer fetch timing, and mid-frame `$d018` effects correctly.

### Idle graphics / ghostbyte behavior is central, not incidental

The upper-border illusion depends on idle graphics showing through transparent sprite pixels.

Key facts:

- Idle graphics is the repeated 8-pixel pattern sourced from the current "ghostbyte".
- In the normal case the ghostbyte comes from VIC address `$3fff`.
- The demo updates that byte to change the repeated high-resolution detail line by line.
- The digits were designed around this restriction: their hi-res curved detail has to line up with a shared ghostbyte structure.

Why it matters:

- If idle graphics, ghostbyte fetch source, or ghostbyte repeat timing is wrong, the device turns into garbage immediately.

### Mid-raster `$d011` / `$d021` / `$d016` timing must be exact

This is the most important part of the effect.

The two-sprite device uses:

- Two X-expanded multicolor sprites for coarse blocks.
- Idle graphics for the missing hi-res detail.
- Per-line background-color splits for the three digit colors and surrounding black.
- A mode switch in `$d011` to control which ghostbyte source is active.

Important details from the article and transcript:

- The explanation video simplifies this as ECM, but the article says the real code uses invalid video mode `$70` rather than plain `$50`.
- Mode `$70` still gives the needed ECM-like ghostbyte behavior, but also covers a left-edge gap that would otherwise leak the current background color.
- In the blacked-out state, the effective ghostbyte source is moved from `$3fff` to `$39ff`, where the demo keeps an all-black pattern.
- The change of idle pattern is delayed by the pixel pipeline, and that delay depends on horizontal scroll (`$d016`).
- The mode change is aligned so the new ghostbyte detail starts exactly at the left edge of the first digit.
- Then three rapid `$d021` stores perform the visible color splits.

This lets the code solve two otherwise impossible constraints:

- only three CPU registers (`A/X/Y`) are available for the four needed color states
- the splits are only 4 cycles apart

### Split timing is not uniform across all cases

The article explicitly calls out extra timing variants:

- color splits are effectively delayed by one pixel relative to scroll position 1
- scroll position 0 needs a different relative timing
- different VIC revisions have a one-cycle pipeline difference
- on newer 8565 chips, color splits create tiny grey dots/stripes that must be hidden

The demo detects the VIC version at startup using sprite collision behavior and selects timing accordingly.

Why it matters:

- If the emulator models a PAL 6569, be internally consistent with that chip.
- If collision behavior or pipeline delay is wrong, the demo may select the wrong timing path.

### Border-to-text transition is a separate implementation

When the device dips into the text area:

- the effect is no longer done with real idle graphics
- the pattern is emulated with ordinary characters
- ordinary color RAM replaces the tight border-area color splits
- the transition is triggered by landing on an `RTS` placed at a precise offset in unrolled raster code

Why it matters:

- If the text-area version looks sane but the border version does not, that narrows the bug sharply to border/idle/timed-VIC behavior.

## Practical debugging order for this emulator

1. Fix open-border timing and mid-line register effects first.
2. Fix the side-border black-cover scene: verify that the left border stays black while the sprites expand there, before the top-border trick begins.
3. Verify idle-graphics generation and ghostbyte source switching between normal and ECM-like/invalid modes.
4. Verify `$d016` horizontal-scroll-dependent pipeline delay.
5. Verify sprite DMA steal timing, including the 3-cycle warning window and "bridging" steals.
6. Verify sprite crunch and mid-frame sprite-pointer-bank changes via `$d018`.
7. Verify sprite priority and sprite-collision behavior.

## Why this matches current `c64-llm` weak spots

The known VIC-II failures already listed in [c64.h](/Users/antirez/hack/2026/c64-llm/c64.h) line up with what `Nine` stresses:

- border opening
- color splits / horizontal offsets
- raster IRQ timing
- split tests
- greydot / pixel-delay behavior
- sprite timing / scan behavior

So the likely path to making `Nine` work is not demo-specific hacking. It is improving the emulator exactly where its own TODO/failure list already says it is weakest.

## Non-regressive repair strategy

The safest conclusion from testing so far is:

- do not "fix" this with a generic "apply all VIC writes earlier" rule
- do not rely on a narrow ad hoc "apply writes before pixel decode" trick as the final answer either

Those approaches are too broad and can easily move unrelated VIC behavior by a whole cycle.

A more hardware-accurate path is to separate which internal VIC paths see the current register value immediately and which see a delayed/latching effect.

Useful reference behavior from VICE's PAL `MOS6569` model:

- horizontal border decisions use the current `$d016` CSEL bit on the border-check cycles
- vertical border decisions use the current `$d011` RSEL and DEN bits on the border-check cycles
- the graphics/idle fetch path is a different pipeline from border control
- PAL `6569` has chip-specific color/fetch latency behavior, so `$d011` mode effects are not the same thing as "all writes happen earlier"
- newer `8565` chips differ, so the behavior should be treated as chip-model-specific if that is ever emulated

What this implies for `c64-llm`:

- border logic should sample current `D011/D016` at the correct cycle
- graphics fetch / idle-graphics source selection should have its own `D011` timing model
- pixel decode / color split timing (`D011`, `D016`, `D021`, and probably `D018`) should be handled as a separate pipeline problem

That is the route most likely to fix the left-border leak first without introducing regressions in other raster effects.

## Sources

- Explanation page: <https://www.linusakesson.net/scene/nine/explanation.php>
- Demo page: <https://www.linusakesson.net/scene/nine/>
- Local transcript extracted from the deleted `nine/nine_video_trascript.txt`
