/* Headless SID checks derived from the hardware references in docs/sid.md. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#define CHIPS_IMPL
#include "../m6581.h"

static void init(m6581_t* sid) {
    m6581_init(sid, &(m6581_desc_t){985248, 44100, 1.0f});
}

static void write_reg(m6581_t* sid, uint8_t reg, uint8_t data) {
    uint64_t pins = M6581_CS | reg;
    M6581_SET_DATA(pins, data);
    _m6581_write(sid, pins);
}

static uint8_t read_reg(m6581_t* sid, uint8_t reg) {
    return M6581_GET_DATA(_m6581_read(sid, M6581_CS | M6581_RW | reg));
}

static void ticks(m6581_t* sid, int count) {
    while (count--) m6581_tick(sid, 0);
}

static void test_sample_clock(void) {
    const int clocks[] = {985248, 1022727};
    const int rates[] = {44100, 48000, 96000};
    for (unsigned c = 0; c < 2; c++) {
        for (unsigned r = 0; r < 3; r++) {
            m6581_t sid;
            m6581_init(&sid, &(m6581_desc_t){clocks[c], rates[r], 1.0f});
            unsigned samples = 0;
            for (int i = 1; i <= clocks[c]; i++) {
                samples += !!(m6581_tick(&sid, 0) & M6581_SAMPLE);
                assert(samples == (uint64_t)i * rates[r] / clocks[c]);
            }
            assert(samples == (unsigned)rates[r]);
            m6581_reset(&sid);
            assert(sid.sample_phase == 0 && sid.sample == 0);
        }
    }
}

static void test_oscillator(void) {
    m6581_t sid;
    init(&sid);
    write_reg(&sid, M6581_V3_FREQ_HI, 0x40);
    ticks(&sid, 1000);
    assert(sid.voice[2].wav_accum == ((1000 * 0x4000) & 0xFFFFFF));
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_SAWTOOTH);
    ticks(&sid, 1);
    assert(read_reg(&sid, M6581_OSC3RAND) == ((1001 * 0x4000) >> 16));
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_TEST);
    ticks(&sid, 100);
    assert(sid.voice[2].wav_accum == 0);

    /* The triangle DAC's low bit is grounded, including at the peak. */
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_TRIANGLE);
    write_reg(&sid, M6581_V3_FREQ_HI, 0);
    for (unsigned i = 0; i < 8192; i++) {
        sid.voice[2].wav_accum = i * 2048;
        ticks(&sid, 1);
        unsigned expected = i < 4096 ? i : 8191 - i;
        assert(sid.voice[2].wav_output == (expected & 0xFFE));
    }
}

static void test_sync_and_ring(void) {
    for (int i = 0; i < 3; i++) {
        m6581_t sid;
        init(&sid);
        int source = (i+2)%3;
        int other = (i+1)%3;
        sid.voice[source].wav_accum = 0x7FFFFF;
        sid.voice[source].freq = 1;
        sid.voice[i].wav_accum = 0x123456;
        sid.voice[i].ctrl = M6581_CTRL_SYNC | M6581_CTRL_SAWTOOTH;
        sid.voice[other].wav_accum = 0x654321;
        ticks(&sid, 1);
        assert(sid.voice[i].wav_accum == 0);
        assert(sid.voice[i].wav_output == 0);
        assert(sid.voice[other].wav_accum == 0x654321);

        /* Every voice must see the source's current cycle, regardless of index. */
        init(&sid);
        sid.voice[source].wav_accum = 0x7FFFFF;
        sid.voice[source].freq = 1;
        sid.voice[i].ctrl = M6581_CTRL_RINGMOD | M6581_CTRL_TRIANGLE;
        ticks(&sid, 1);
        assert(sid.voice[i].wav_output == 0);
    }

    /* Truth table of the triangle XOR selector in the die schematics. */
    m6581_t sid;
    init(&sid);
    for (int saw = 0; saw < 2; saw++) {
        for (int ring = 0; ring < 2; ring++) {
            for (int phase = 0; phase < 2; phase++) {
                for (int source = 0; source < 2; source++) {
                    sid.voice[0].ctrl = M6581_CTRL_TRIANGLE | (saw << 5) | (ring << 2);
                    sid.voice[0].wav_accum = (phase << 23) | 0x123456;
                    sid.voice[2].wav_accum = source << 23;
                    bool invert = !saw && (ring ? phase == source : phase != 0);
                    assert(_m6581_triangle(&sid.voice[0], &sid.voice[2]) ==
                           (invert ? 0xDB8 : 0x246));
                }
            }
        }
    }
}

static void test_pulse_and_noise(void) {
    m6581_t sid;
    init(&sid);
    m6581_voice_t* v = &sid.voice[2];
    v->ctrl = M6581_CTRL_PULSE;
    v->pulse_width = 0x800;
    v->freq = 1;
    v->wav_accum = 0x7FFFFF;
    ticks(&sid, 1);
    assert(v->wav_output == 0);
    ticks(&sid, 1);
    assert(v->wav_output == 0xFFF);

    v->ctrl = M6581_CTRL_NOISE;
    v->wav_accum = 0x07FFFF;
    v->noise_shift = 0x500001;
    ticks(&sid, 1);
    assert(v->noise_shift == 0x500001);
    ticks(&sid, 1);
    assert(v->noise_shift == 0x500001);
    ticks(&sid, 1);
    assert(v->noise_shift == 0x200003);

    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_NOISE | M6581_CTRL_TEST);
    ticks(&sid, 0x8000);
    assert(v->noise_shift == 0x7FFFFF);
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_NOISE);
    assert(v->noise_shift == 0x7FFFFE);

    /* Pulse low pulls the connected noise bits down; pure noise then locks. */
    v->freq = 0xFFFF;
    v->pulse_width = 0xFFF;
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_PULSE | M6581_CTRL_NOISE);
    ticks(&sid, 2000);
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_NOISE);
    ticks(&sid, 100);
    assert(v->noise_shift == 0 && v->wav_output == 0);

    init(&sid);
    v->ctrl = M6581_CTRL_TRIANGLE | M6581_CTRL_SAWTOOTH | M6581_CTRL_PULSE;
    v->freq = 0x7000;
    unsigned peak = 0;
    for (int i = 0; i < 1000; i++) {
        ticks(&sid, 1);
        if (v->wav_output > peak) peak = v->wav_output;
    }
    assert(peak > 255); // the DAC must not collapse to a boolean
}

static void test_envelope(void) {
    /* Cycle counts independently enumerated from the documented rate LFSR. */
    const int periods[] = {9,32,63,95,149,220,267,313,392,977,1954,3126,3907,11720,19532,31251};
    for (int rate = 0; rate < 16; rate++) {
        m6581_t sid;
        init(&sid);
        write_reg(&sid, M6581_V3_ATKDEC, rate << 4);
        write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_GATE);
        ticks(&sid, periods[rate] - 1);
        assert(read_reg(&sid, M6581_ENV3) == 0);
        ticks(&sid, 1);
        assert(read_reg(&sid, M6581_ENV3) == 1);
    }

    m6581_t sid;
    init(&sid);
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_GATE);
    ticks(&sid, 5 * 9);
    write_reg(&sid, M6581_V3_CTRL, 0);
    ticks(&sid, 9);
    assert(read_reg(&sid, M6581_ENV3) == 4); // period 30 has not been latched yet
    ticks(&sid, 4 * 9);
    assert(read_reg(&sid, M6581_ENV3) == 0);
    ticks(&sid, 100);
    assert(read_reg(&sid, M6581_ENV3) == 0);

    init(&sid);
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_GATE);
    ticks(&sid, 7 * 9);
    write_reg(&sid, M6581_V3_CTRL, 0);
    ticks(&sid, 30 * 9 - 1);
    assert(read_reg(&sid, M6581_ENV3) == 7);
    ticks(&sid, 1);
    assert(read_reg(&sid, M6581_ENV3) == 6);

    /* Changing GATE must not restart the rate counter (the ADSR delay bug). */
    init(&sid);
    write_reg(&sid, M6581_V3_SUSREL, 0x0F);
    ticks(&sid, 100);
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_GATE);
    ticks(&sid, 32767 - 100 + 9 - 1);
    assert(read_reg(&sid, M6581_ENV3) == 0);
    ticks(&sid, 1);
    assert(read_reg(&sid, M6581_ENV3) == 1);

    /* A quick gate pulse unlocks zero, permitting release to wrap to $ff. */
    init(&sid);
    write_reg(&sid, M6581_V3_CTRL, M6581_CTRL_GATE);
    write_reg(&sid, M6581_V3_CTRL, 0);
    ticks(&sid, 9);
    assert(read_reg(&sid, M6581_ENV3) == 0xFF);
}

static void test_bus(void) {
    m6581_t sid;
    init(&sid);
    write_reg(&sid, 31, 0xAB);
    assert(read_reg(&sid, 0) == 0xAB);
    sid.voice[2].env_cur_level = 0x53;
    assert(read_reg(&sid, M6581_ENV3) == 0x53);
    assert(read_reg(&sid, 31) == 0x53);
    ticks(&sid, 0x2000);
    assert(read_reg(&sid, 31) == 0);
    assert(read_reg(&sid, M6581_POT_X) == 0xFF);
}

static double sample_tone(double freq, int rate) {
    m6581_t sid;
    m6581_init(&sid, &(m6581_desc_t){985248, rate, 1.0f});
    double sum = 0.0;
    unsigned count = 0;
    for (int i = 0; i < 985248 / 2; i++) {
        float input = (float)sin(6.283185307179586 * freq * i / 985248);
        if (_m6581_sample(&sid, input) && i > 985248 / 10) {
            sum += sid.sample * sid.sample;
            count++;
        }
    }
    return sqrt(sum / count);
}

static void test_resampling(void) {
    const int rates[] = {44100, 48000, 96000};
    for (unsigned i = 0; i < 3; i++) {
        double pass = sample_tone(1000, rates[i]);
        double stop = 0;
        for (int j = 0; j < 16; j++) {
            double freq = rates[i] * 0.5 + (985248 * 0.49 - rates[i] * 0.5) * j / 15;
            double rms = sample_tone(freq, rates[i]);
            if (rms > stop) stop = rms;
        }
        printf("SID %d Hz: 1 kHz RMS %.6f, worst of 16 stopband tones %.1f dB\n",
               rates[i], pass, 20 * log10(stop / pass));
        assert(pass > 0.69 && pass < 0.71);
        assert(stop / pass < 0.001);
    }
}

static void write_le(FILE* fp, unsigned value, int bytes) {
    while (bytes--) {
        fputc(value & 255, fp);
        value >>= 8;
    }
}

/* One second each: triangle, saw, pulse, combined, sync, ring, noise,
   resonant low-pass sweep, volume digi. No audio device or window needed. */
static int render(const char* path) {
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return 1;
    }
    const unsigned rate = 44100;
    fwrite("RIFF", 1, 4, fp);
    write_le(fp, 36 + 9 * rate * 2, 4);
    fwrite("WAVEfmt ", 1, 8, fp);
    write_le(fp, 16, 4);
    write_le(fp, 1, 2);
    write_le(fp, 1, 2);
    write_le(fp, rate, 4);
    write_le(fp, rate * 2, 4);
    write_le(fp, 2, 2);
    write_le(fp, 16, 2);
    fwrite("data", 1, 4, fp);
    write_le(fp, 9 * rate * 2, 4);
    const uint8_t control[] = {0x11,0x21,0x41,0x71,0x23,0x15,0x81,0x21,0};
    for (int part = 0; part < 9; part++) {
        m6581_t sid;
        init(&sid);
        write_reg(&sid, M6581_V1_SUSREL, 0xF0);
        write_reg(&sid, M6581_V1_PW_HI, 8);
        write_reg(&sid, M6581_V1_CTRL, control[part]);
        write_reg(&sid, M6581_V3_FREQ_HI, 0x20);
        write_reg(&sid, M6581_MODE_VOL, part == 7 ? 0x1F : 15);
        if (part == 7) write_reg(&sid, M6581_RES_FILT, 0xF1);
        for (unsigned i = 0; i < 985248; i++) {
            if (i % 256 == 0) {
                unsigned freq = 0x800 + (uint64_t)i * 0xF000 / 985248;
                write_reg(&sid, M6581_V1_FREQ_LO, freq & 255);
                write_reg(&sid, M6581_V1_FREQ_HI, freq >> 8);
                write_reg(&sid, M6581_FC_HI, (uint64_t)i * 255 / 985248);
            }
            if (part == 8) {
                write_reg(&sid, M6581_MODE_VOL, ((uint64_t)i * 880 / 985248) & 1 ? 15 : 0);
            }
            if (m6581_tick(&sid, 0) & M6581_SAMPLE) {
                assert(isfinite(sid.sample) && fabsf(sid.sample) < 1.0f);
                write_le(fp, (uint16_t)(int16_t)(sid.sample * 32767), 2);
            }
        }
    }
    int failed = ferror(fp);
    if (fclose(fp)) failed = 1;
    return failed;
}

static double digi_tone(uint8_t route, uint8_t mode) {
    m6581_t sid;
    init(&sid);
    write_reg(&sid, M6581_RES_FILT, route);
    double sum = 0.0, mean = 0.0;
    unsigned count = 0;
    for (int i = 0; i < 985248 / 2; i++) {
        write_reg(&sid, M6581_MODE_VOL, mode | (((uint64_t)i * 2000 / 985248) & 1 ? 15 : 0));
        if ((m6581_tick(&sid, 0) & M6581_SAMPLE) && i > 985248 / 10) {
            sum += sid.sample * sid.sample;
            mean += sid.sample;
            count++;
        }
    }
    assert(fabs(mean / count) < 0.0001);
    return sqrt(sum / count);
}

static void test_digis(void) {
    double normal = digi_tone(0, 0);
    double disconnected = digi_tone(7, 0);
    double cancelled = digi_tone(1, 0x90);
    printf("SID volume digis: normal %.6f, disconnected %.6f, DC cancellation %.6f RMS\n",
           normal, disconnected, cancelled);
    assert(normal > 0.05);
    assert(disconnected < normal * 0.1);
    assert(cancelled < normal * 0.1);

    /* Constant volume with closed envelopes settles to silence. */
    m6581_t sid;
    init(&sid);
    write_reg(&sid, M6581_MODE_VOL, 15);
    ticks(&sid, 985248 / 2);
    assert(fabsf(sid.sample) < 0.0000001f);
}

static void test_filter(void) {
    m6581_t sid;
    init(&sid);
    for (int cutoff = 0; cutoff < 2048; cutoff += 31) {
        for (int resonance = 0; resonance < 16; resonance++) {
            _m6581_init_filter(&sid.filter, sid.tick_hz);
            write_reg(&sid, M6581_FC_LO, cutoff & 7);
            write_reg(&sid, M6581_FC_HI, cutoff >> 3);
            write_reg(&sid, M6581_RES_FILT, resonance << 4);
            write_reg(&sid, M6581_MODE_VOL, 0x7F);
            for (int i = 0; i < 2000; i++) {
                float out = _m6581_filter_output(&sid.filter, (i & 64) ? 0.7f : -0.7f);
                assert(isfinite(out) && fabsf(out) < 10.0f);
            }
        }
    }
    /* Low-pass inverts DC, while band-pass and high-pass reject it. */
    _m6581_init_filter(&sid.filter, sid.tick_hz);
    write_reg(&sid, M6581_FC_HI, 128);
    for (int i = 0; i < 100000; i++) _m6581_filter_output(&sid.filter, 0.26f);
    assert(fabs(sid.filter.v_lp + 0.26f) < 0.0001);
    assert(fabs(sid.filter.v_bp) < 0.0001);
    assert(fabs(sid.filter.v_hp) < 0.0001);
}

static void test_reset_and_copy(void) {
    m6581_t sid;
    init(&sid);
    write_reg(&sid, M6581_V1_FREQ_HI, 0x40);
    write_reg(&sid, M6581_V1_SUSREL, 0xF0);
    write_reg(&sid, M6581_V1_CTRL, 0x21);
    write_reg(&sid, M6581_MODE_VOL, 15);
    ticks(&sid, 100000);
    m6581_t saved = sid;
    for (int i = 0; i < 10000; i++) {
        assert(m6581_tick(&sid, 0) == m6581_tick(&saved, 0));
        assert(sid.sample == saved.sample);
    }
    m6581_reset(&sid);
    m6581_reset(&saved);
    ticks(&sid, 10000);
    assert(sid.sample == 0.0f);
    assert(saved.sample_pos == 0 && saved.sample_phase == 0);
}

int main(int argc, char** argv) {
    if (argc == 2) return render(argv[1]);
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [OUTPUT.wav]\n", argv[0]);
        return 1;
    }
    test_sample_clock();
    test_oscillator();
    test_sync_and_ring();
    test_pulse_and_noise();
    test_envelope();
    test_bus();
    test_resampling();
    test_digis();
    test_filter();
    test_reset_and_copy();
    puts("SID tests passed");
    return 0;
}
