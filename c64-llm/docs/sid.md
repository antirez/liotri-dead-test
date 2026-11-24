# SID emulation

The SID remains a single header, with the same initialization and tick API.
It uses the existing oscillators, envelope rate LFSR and state-variable filter.
The changes below were derived from hardware documentation and measurements,
without adding another emulator core.

## Digital behavior

- Oscillators keep running when no waveform is selected. Disconnecting the
  waveform DAC no longer shifts the oscillator phase.
- Sync resets the correct destination voice. All three oscillators advance
  before sync and waveform output, so ring modulation is independent of the
  order in which voices are processed.
- Triangle has a grounded low bit. Its inversion follows the ring-modulation
  truth table, and selecting sawtooth disables the shared XOR gates.
- Pulse comparison is delayed by one cycle. Noise shifting takes two cycles
  after the oscillator's bit-19 edge. TEST holds the oscillator at zero,
  eventually resets noise, and completes a noise shift when released.
- Combined noise can feed zero bits back into the shift register. The
  triangle/saw/pulse combination no longer collapses to a boolean value.
- The envelope's exponential divider remembers the last threshold crossed,
  including during attack. Zero hold is separate from the envelope state,
  permitting wraparound after a short gate pulse. Gate changes preserve the
  rate counter and its ADSR delay behavior.
- Readable registers update the bus latch. Unconnected paddle inputs read
  as `$ff`.

## Audio output

An integer phase accumulator schedules output at the requested sample rate.
The old fixed-point period produced about 44,157 samples per PAL second when
44,100 were requested. Exact scheduling avoids that pitch and queue drift.

The signal is processed at the SID clock rate. A 1023-tap Blackman-windowed
sinc removes frequencies above the host's audio band before downsampling.
Adjacent chip cycles are interpolated at the output sample boundary. The
filter adds about 0.52 ms of delay at the PAL clock and needs no allocations.
The regression tests cover 44.1, 48 and 96 kHz output, with PAL and NTSC clocks.

The internal filter keeps its original cutoff curve and resonance mapping,
but its coefficients now use the actual chip clock. Floating-point integration
avoids integer truncation and overflow; double-precision state preserves small
changes around a DC offset. A 30 Hz minimum cutoff lets the filter settle at
register value zero.

Voice and mixer DC offsets are represented before volume control. This makes
`$d418` sample playback depend on voice routing, including the cancellation
between a direct voice and an inverted low-pass voice. Nominal values are
0.26 V per silent voice, a -0.04 V mixer offset, and a waveform swing of
-0.40 to +0.65 V at full envelope. These approximate the measurements in
*The C64 Digi*; they are not a calibration of every SID's analog output.

After the SID mixer, a 16 kHz low-pass models the motherboard output circuit.
A 16 Hz high-pass removes DC. The latter is a nominal load: the real cutoff
depends on the coupling capacitor and the attached amplifier.

## Headless checks

```sh
make test
make tests/sid
./tests/sid /tmp/sid.wav
```

The WAV renderer opens neither a window nor an audio device. It produces one
second each of triangle, sawtooth, pulse, combined waves, sync, ring modulation,
noise, a resonant low-pass sweep and volume samples.

Tests cover sample timing, waveform logic, envelope periods and quirks, bus
reads, filter stability, DC cancellation, reset and copying SID state. At each
audio rate, sixteen input tones from output Nyquist to near chip Nyquist check
alias rejection. The largest residual among those tones was -104.8 dB at
44.1 kHz, -84.7 dB at 48 kHz and -105.7 dB at 96 kHz, relative to the 1 kHz
reference. These are sampled checks, not a worst-case bound over all frequencies.

The SID tests and WAV renderer pass AddressSanitizer and UndefinedBehaviorSanitizer.
The macOS frontend, SID keyboard tool and a standalone C++ header check build.
Nine also completes 9000 PAL frames in the headless probe and returns to BASIC.
That run rendered 179.55 seconds of audio in 30.29 seconds on the development
machine, with a peak magnitude of 0.557 and no clipping. No window or audio
device was opened during these checks.

The SID state has changed size, so the C64 snapshot version is now 2. Older
snapshots are rejected rather than loaded with the wrong layout. Standalone
builds using the header must link the math library where needed (`-lm`).

## Remaining approximations

This is still a compact 6581 model. Combined waveforms retain the existing
neighbor-bit approximation; their analog coupling and oscillator feedback
are not fully modeled. The filter is linear and uses a generic cutoff curve,
without the chip's nonlinear DACs, distortion or unit-to-unit variation.
Disconnected waveform output is held without analog decay. Noise TEST reset
uses a nominal 32768-cycle delay, and bus decay uses a fixed 8192 cycles.
Envelope gate pipelines and interacting sync edges still need cycle-level
hardware validation. There is no separate 8580 model.

## Hardware references

- [MOS 6581 datasheet](https://www.waitingforfriday.com/?p=661): registers,
  routing, envelope operation and electrical specifications.
- [SID die schematics](https://github.com/libsidplayfp/SID_schematics/wiki):
  especially [oscillator](https://github.com/libsidplayfp/SID_schematics/wiki/Oscillator),
  [triangle](https://github.com/libsidplayfp/SID_schematics/wiki/Triangle-Waveform),
  [pulse](https://github.com/libsidplayfp/SID_schematics/wiki/Pulse-Waveform),
  [noise](https://github.com/libsidplayfp/SID_schematics/wiki/Noise-Generator),
  [exponential divider](https://github.com/libsidplayfp/SID_schematics/wiki/Exponential-Divider),
  [counter logic](https://github.com/libsidplayfp/SID_schematics/wiki/Counter-Logic),
  [data bus](https://github.com/libsidplayfp/SID_schematics/wiki/Data-Bus) and
  [external filter](https://github.com/libsidplayfp/SID_schematics/wiki/External-Filter).
- Asger Alstrup's hardware recordings of
  [triangle](http://www.sidmusic.org/sid/sidtech4.html) and
  [noise](http://www.sidmusic.org/sid/sidtech5.html), including the observed
  variation in TEST reset time.
- [The C64 Digi](https://raw.githubusercontent.com/Jeff-Birt/C64_DIGI/master/the_c64_digi.txt),
  C=Hacking 20, by Robin Harbron, Levente Harsfalvi and Stephen Judd:
  measured DC offsets, filter inversion and volume samples.
