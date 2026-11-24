#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHIPS_IMPL
#include "m6581.h"

#define SID_CLOCK_HZ 985248
#define SAMPLE_RATE 44100
#define WINDOW_W 960
#define WINDOW_H 520
#define MAX_SEGMENTS 2048
#define NUM_VOICES 2
#define TICK_QUANTUM 4
#define MIN_SECOND_VOICE_OVERLAP_TICKS 3
#define DEFAULT_BPM 120.0
#define DEFAULT_TICKS_PER_BAR 96
#define DEFAULT_SILENCE_MS 2500

#define WHITE_W 72
#define WHITE_H 160
#define BLACK_W 40
#define BLACK_H 96

typedef struct {
    SDL_Scancode scancode;
    int note_idx;
    int is_black;
    SDL_Rect rect;
    const char *key_label;
} keydef_t;

typedef struct {
    int note_idx; /* -1 means rest */
    uint64_t duration_ms;
} segment_t;

typedef struct {
    segment_t seg[MAX_SEGMENTS];
    int seg_len;
    int current_note; /* -1 means rest */
    uint64_t segment_start_ms;
} voice_track_t;

typedef struct {
    m6581_t sid;
    uint64_t sid_pins;
    SDL_AudioDeviceID audio_dev;
    int scancode_for_voice[NUM_VOICES]; /* 0 when free */
    int note_for_voice[NUM_VOICES];
    double bpm;
    double beat_samples;
    double sample_counter;
    double next_beat_sample;
    double click_phase;
    int click_samples_left;
    int click_samples_total;
    double click_amp;
    double click_freq;
    uint64_t beat_index;
} sid_state_t;

typedef struct {
    voice_track_t voices[NUM_VOICES];
    int phrase_active;
    uint64_t phrase_start_ms;
    uint64_t last_event_ms;
    uint64_t silence_ms;
    double bpm;
    int ticks_per_bar;
    uint64_t transport_start_ms;
} recorder_t;

static keydef_t g_keys[34];
static int g_num_keys = 0;
static uint8_t g_key_down[SDL_NUM_SCANCODES];
static uint64_t g_key_press_order[SDL_NUM_SCANCODES];
static uint64_t g_press_counter = 1;

static inline uint64_t pins_set_addr(uint64_t pins, uint8_t addr) {
    return (pins & ~0xFFFFULL) | ((uint64_t)addr & 0x1FULL);
}

static inline uint64_t pins_set_data(uint64_t pins, uint8_t data) {
    return (pins & ~0xFF0000ULL) | (((uint64_t)data << 16) & 0xFF0000ULL);
}

static void sid_write_reg(sid_state_t *s, uint8_t reg, uint8_t value) {
    uint64_t pins = s->sid_pins;
    pins = pins_set_addr(pins, reg);
    pins = pins_set_data(pins, value);
    pins |= M6581_CS;
    pins &= ~M6581_RW;
    s->sid_pins = m6581_tick(&s->sid, pins);
    s->sid_pins &= ~M6581_CS;
}

static uint16_t note_to_sid_freq(int note_idx) {
    const double c0_hz = 16.351597831287414;
    double hz = c0_hz * pow(2.0, (double)note_idx / 12.0);
    int reg = (int)llround(hz * (double)(1 << 24) / (double)SID_CLOCK_HZ);
    if (reg < 0) reg = 0;
    if (reg > 0xFFFF) reg = 0xFFFF;
    return (uint16_t)reg;
}

static void sid_voice_init(sid_state_t *sid, int voice) {
    uint8_t base = (uint8_t)(voice * 7);
    sid_write_reg(sid, (uint8_t)(base + 2), 0x00); /* pulse lo */
    sid_write_reg(sid, (uint8_t)(base + 3), 0x08); /* pulse hi */
    sid_write_reg(sid, (uint8_t)(base + 5), 0x24); /* attack/decay */
    sid_write_reg(sid, (uint8_t)(base + 6), 0xC4); /* sustain/release */
    sid_write_reg(sid, (uint8_t)(base + 4), M6581_CTRL_PULSE); /* gate off */
}

static void sid_voice_note_on(sid_state_t *sid, int voice, int note_idx) {
    uint8_t base = (uint8_t)(voice * 7);
    uint16_t freq = note_to_sid_freq(note_idx);
    sid_write_reg(sid, (uint8_t)(base + 0), (uint8_t)(freq & 0xFF));
    sid_write_reg(sid, (uint8_t)(base + 1), (uint8_t)(freq >> 8));
    sid_write_reg(sid, (uint8_t)(base + 4), M6581_CTRL_PULSE | M6581_CTRL_GATE);
}

static void sid_voice_note_off(sid_state_t *sid, int voice) {
    uint8_t base = (uint8_t)(voice * 7);
    sid_write_reg(sid, (uint8_t)(base + 4), M6581_CTRL_PULSE);
}

static void sid_init(sid_state_t *sid, double bpm) {
    memset(sid, 0, sizeof(*sid));
    for (int i = 0; i < NUM_VOICES; i++) {
        sid->scancode_for_voice[i] = 0;
        sid->note_for_voice[i] = -1;
    }
    m6581_init(&sid->sid, &(m6581_desc_t){
        .tick_hz = SID_CLOCK_HZ,
        .sound_hz = SAMPLE_RATE,
        .magnitude = 0.6f,
    });
    m6581_reset(&sid->sid);
    sid->sid_pins = 0;

    sid_write_reg(sid, M6581_MODE_VOL, 0x0F); /* master vol */
    sid_voice_init(sid, 0);
    sid_voice_init(sid, 1);

    sid_write_reg(sid, M6581_V3_CTRL, 0x00);
    sid->bpm = bpm;
    sid->beat_samples = (60.0 * (double)SAMPLE_RATE) / sid->bpm;
    sid->sample_counter = 0.0;
    sid->next_beat_sample = 0.0;
    sid->click_phase = 0.0;
    sid->click_samples_left = 0;
    sid->click_samples_total = 0;
    sid->click_amp = 0.0;
    sid->click_freq = 0.0;
    sid->beat_index = 0;
}

static void note_index_to_name(int note_idx, char out[16]) {
    static const char *names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = note_idx / 12;
    int pitch = note_idx % 12;
    snprintf(out, 16, "%s%d", names[pitch], octave);
}

static double recorder_bar_ms(const recorder_t *r) {
    return 240000.0 / r->bpm; /* 4 beats per bar */
}

static uint64_t recorder_bar_index_at(const recorder_t *r, uint64_t now_ms) {
    if (now_ms <= r->transport_start_ms) return 0;
    double delta = (double)(now_ms - r->transport_start_ms);
    return (uint64_t)floor(delta / recorder_bar_ms(r));
}

static uint64_t recorder_bar_start_ms_at(const recorder_t *r, uint64_t now_ms) {
    double bar_ms = recorder_bar_ms(r);
    uint64_t idx = recorder_bar_index_at(r, now_ms);
    return r->transport_start_ms + (uint64_t)llround((double)idx * bar_ms);
}

static uint64_t recorder_bar_end_ms_containing(const recorder_t *r, uint64_t now_ms) {
    double bar_ms = recorder_bar_ms(r);
    uint64_t idx = recorder_bar_index_at(r, now_ms);
    return r->transport_start_ms + (uint64_t)llround((double)(idx + 1) * bar_ms);
}

static void recorder_reset(recorder_t *r, uint64_t now_ms) {
    memset(r, 0, sizeof(*r));
    r->bpm = DEFAULT_BPM;
    r->ticks_per_bar = DEFAULT_TICKS_PER_BAR;
    r->silence_ms = DEFAULT_SILENCE_MS;
    r->transport_start_ms = now_ms;
    r->phrase_start_ms = now_ms;
    r->last_event_ms = now_ms;
    for (int i = 0; i < NUM_VOICES; i++) {
        r->voices[i].current_note = -1;
        r->voices[i].segment_start_ms = now_ms;
    }
}

static void recorder_start_phrase_if_needed(recorder_t *r, uint64_t now_ms) {
    if (r->phrase_active) return;
    uint64_t bar_start = recorder_bar_start_ms_at(r, now_ms);
    r->phrase_active = 1;
    r->phrase_start_ms = bar_start;
    r->last_event_ms = now_ms;
    for (int i = 0; i < NUM_VOICES; i++) {
        r->voices[i].seg_len = 0;
        r->voices[i].current_note = -1;
        r->voices[i].segment_start_ms = bar_start;
    }
}

static void recorder_append_segment(voice_track_t *v, int note_idx, uint64_t duration_ms) {
    if (duration_ms == 0) return;
    if (v->seg_len > 0 && v->seg[v->seg_len - 1].note_idx == note_idx) {
        v->seg[v->seg_len - 1].duration_ms += duration_ms;
        return;
    }
    if (v->seg_len >= MAX_SEGMENTS) return;
    v->seg[v->seg_len].note_idx = note_idx;
    v->seg[v->seg_len].duration_ms = duration_ms;
    v->seg_len++;
}

static void recorder_close_segment(recorder_t *r, int voice, uint64_t now_ms) {
    voice_track_t *v = &r->voices[voice];
    if (now_ms < v->segment_start_ms) {
        v->segment_start_ms = now_ms;
        return;
    }
    uint64_t elapsed = now_ms - v->segment_start_ms;
    recorder_append_segment(v, v->current_note, elapsed);
    v->segment_start_ms = now_ms;
}

static void recorder_note_on(recorder_t *r, int voice, int note_idx, uint64_t now_ms) {
    recorder_start_phrase_if_needed(r, now_ms);
    recorder_close_segment(r, voice, now_ms);
    r->voices[voice].current_note = note_idx;
    r->voices[voice].segment_start_ms = now_ms;
    r->last_event_ms = now_ms;
}

static void recorder_note_off(recorder_t *r, int voice, uint64_t now_ms) {
    if (!r->phrase_active) return;
    recorder_close_segment(r, voice, now_ms);
    r->voices[voice].current_note = -1;
    r->voices[voice].segment_start_ms = now_ms;
    r->last_event_ms = now_ms;
}

typedef struct {
    int note_idx; /* -1 means rest */
    int ticks;
} quant_segment_t;

static void build_tick_timeline(
    const voice_track_t *track,
    int total_ticks,
    uint64_t phrase_duration_ms,
    int *out_ticks)
{
    if (phrase_duration_ms == 0) phrase_duration_ms = 1;
    if (total_ticks <= 0) return;

    for (int t = 0; t < total_ticks; t++) {
        double w_start = ((double)t * (double)phrase_duration_ms) / (double)total_ticks;
        double w_end = ((double)(t + 1) * (double)phrase_duration_ms) / (double)total_ticks;
        int best_note = -1;
        double best_overlap = -1.0;

        double seg_cursor = 0.0;
        for (int i = 0; i < track->seg_len; i++) {
            double seg_start = seg_cursor;
            double seg_end = seg_start + (double)track->seg[i].duration_ms;
            seg_cursor = seg_end;

            if (seg_end <= w_start) continue;
            if (seg_start >= w_end) break;

            double ov_start = seg_start > w_start ? seg_start : w_start;
            double ov_end = seg_end < w_end ? seg_end : w_end;
            double ov = ov_end - ov_start;
            if (ov <= 0.0) continue;

            if (ov > best_overlap) {
                best_overlap = ov;
                best_note = track->seg[i].note_idx;
            }
        }
        out_ticks[t] = best_note;
    }
}

static void suppress_short_second_voice_overlap(
    int *voice0_ticks,
    int *voice1_ticks,
    int total_ticks,
    int min_ticks)
{
    int t = 0;
    while (t < total_ticks) {
        if (voice0_ticks[t] == -1 || voice1_ticks[t] == -1) {
            t++;
            continue;
        }
        int start = t;
        while (t < total_ticks && voice0_ticks[t] != -1 && voice1_ticks[t] != -1) {
            t++;
        }
        int len = t - start;
        if (len < min_ticks) {
            for (int i = start; i < t; i++) {
                voice1_ticks[i] = -1;
            }
            /* This was a legato-like transition, not real polyphony:
             * move any immediate voice2-only continuation into voice1. */
            int i = t;
            while (i < total_ticks && voice0_ticks[i] == -1 && voice1_ticks[i] != -1) {
                voice0_ticks[i] = voice1_ticks[i];
                voice1_ticks[i] = -1;
                i++;
            }
        }
    }
}

static int collect_unique_tick_notes(int note0, int note1, int out[2]) {
    int count = 0;
    if (note0 != -1) out[count++] = note0;
    if (note1 != -1 && note1 != note0) out[count++] = note1;
    if (count == 2 && out[0] > out[1]) {
        int tmp = out[0];
        out[0] = out[1];
        out[1] = tmp;
    }
    return count;
}

static int lane_single_note_cost(int last_note, int last_tick, int note_idx, int tick, int lane) {
    if (last_note == -1) {
        return 10000 + (lane * 32);
    }
    return abs(note_idx - last_note) * 64 + (tick - last_tick);
}

static void canonicalize_voice_lanes(int *voice0_ticks, int *voice1_ticks, int total_ticks) {
    int lane_last_note[NUM_VOICES] = {-1, -1};
    int lane_last_tick[NUM_VOICES] = {0, 0};

    for (int t = 0; t < total_ticks; t++) {
        int notes[2];
        int count = collect_unique_tick_notes(voice0_ticks[t], voice1_ticks[t], notes);
        int out0 = -1;
        int out1 = -1;

        if (count == 2) {
            out0 = notes[0];
            out1 = notes[1];
            lane_last_note[0] = out0;
            lane_last_tick[0] = t;
            lane_last_note[1] = out1;
            lane_last_tick[1] = t;
        } else if (count == 1) {
            int note = notes[0];
            int cost0 = lane_single_note_cost(lane_last_note[0], lane_last_tick[0], note, t, 0);
            int cost1 = lane_single_note_cost(lane_last_note[1], lane_last_tick[1], note, t, 1);

            if (cost0 <= cost1) {
                out0 = note;
                lane_last_note[0] = note;
                lane_last_tick[0] = t;
            } else {
                out1 = note;
                lane_last_note[1] = note;
                lane_last_tick[1] = t;
            }
        }

        voice0_ticks[t] = out0;
        voice1_ticks[t] = out1;
    }
}

static int timeline_to_quant_segments(
    const int *ticks,
    int total_ticks,
    quant_segment_t *out,
    int out_cap)
{
    if (out_cap <= 0) return 0;
    const int windows = total_ticks / TICK_QUANTUM;
    int out_len = 0;
    int prev_state = -1;

    for (int w = 0; w < windows; w++) {
        int start = w * TICK_QUANTUM;
        int best_note = ticks[start];
        int best_count = -1;

        for (int i = 0; i < TICK_QUANTUM; i++) {
            int n = ticks[start + i];
            int cnt = 1;
            for (int j = i + 1; j < TICK_QUANTUM; j++) {
                if (ticks[start + j] == n) cnt++;
            }
            if (cnt > best_count || (cnt == best_count && n == prev_state)) {
                best_count = cnt;
                best_note = n;
            }
        }

        if (out_len > 0 && out[out_len - 1].note_idx == best_note) {
            out[out_len - 1].ticks += TICK_QUANTUM;
        } else if (out_len < out_cap) {
            out[out_len].note_idx = best_note;
            out[out_len].ticks = TICK_QUANTUM;
            out_len++;
        }
        prev_state = best_note;
    }
    return out_len;
}

static void recorder_print_voice(const quant_segment_t *v, int vlen, int voice_index) {
    printf("voice%d: ", voice_index + 1);
    if (vlen == 0) {
        printf("(empty)\n");
        return;
    }
    int first = 1;
    for (int i = 0; i < vlen; i++) {
        int rem = v[i].ticks;
        while (rem > 0) {
            int chunk = rem > 248 ? 248 : rem;
            if (!first) printf(" ");
            if (v[i].note_idx == -1) {
                printf("R:%d", chunk);
            } else {
                char nn[16];
                note_index_to_name(v[i].note_idx, nn);
                printf("%s:%d", nn, chunk);
            }
            first = 0;
            rem -= chunk;
        }
    }
    printf("\n");
}

static void recorder_flush_phrase(recorder_t *r, uint64_t now_ms) {
    if (!r->phrase_active) return;

    if (now_ms < r->phrase_start_ms) now_ms = r->phrase_start_ms;

    for (int i = 0; i < NUM_VOICES; i++) {
        voice_track_t *v = &r->voices[i];
        recorder_close_segment(r, i, now_ms);
        v->current_note = -1;
        v->segment_start_ms = now_ms;
    }

    uint64_t phrase_duration_ms = now_ms - r->phrase_start_ms;
    double bar_ms = recorder_bar_ms(r);
    int bars = (int)llround((double)phrase_duration_ms / bar_ms);
    if (bars < 1) bars = 1;
    int total_ticks = bars * r->ticks_per_bar;
    if (total_ticks < TICK_QUANTUM) total_ticks = TICK_QUANTUM;
    total_ticks -= (total_ticks % TICK_QUANTUM);
    if (total_ticks < TICK_QUANTUM) total_ticks = TICK_QUANTUM;

    int *v0_ticks = (int *)malloc((size_t)total_ticks * sizeof(int));
    int *v1_ticks = (int *)malloc((size_t)total_ticks * sizeof(int));
    if (!v0_ticks || !v1_ticks) {
        free(v0_ticks);
        free(v1_ticks);
        return;
    }
    build_tick_timeline(&r->voices[0], total_ticks, phrase_duration_ms, v0_ticks);
    build_tick_timeline(&r->voices[1], total_ticks, phrase_duration_ms, v1_ticks);
    suppress_short_second_voice_overlap(v0_ticks, v1_ticks, total_ticks, MIN_SECOND_VOICE_OVERLAP_TICKS);
    canonicalize_voice_lanes(v0_ticks, v1_ticks, total_ticks);

    int any_nonempty = 0;
    for (int i = 0; i < total_ticks; i++) {
        if (v0_ticks[i] != -1 || v1_ticks[i] != -1) {
            any_nonempty = 1;
            break;
        }
    }
    if (!any_nonempty) {
        free(v0_ticks);
        free(v1_ticks);
        r->phrase_active = 0;
        r->phrase_start_ms = now_ms;
        r->last_event_ms = now_ms;
        for (int i = 0; i < NUM_VOICES; i++) {
            r->voices[i].seg_len = 0;
            r->voices[i].current_note = -1;
            r->voices[i].segment_start_ms = now_ms;
        }
        return;
    }

    printf("\n---- phrase (bpm=%.1f, bars=%d, ticks=%d, quantum=%d, metronome-aligned) ----\n",
           r->bpm, bars, total_ticks, TICK_QUANTUM);
    for (int bar = 0; bar < bars; bar++) {
        quant_segment_t q0[MAX_SEGMENTS];
        quant_segment_t q1[MAX_SEGMENTS];
        const int *bar_v0 = v0_ticks + (bar * r->ticks_per_bar);
        const int *bar_v1 = v1_ticks + (bar * r->ticks_per_bar);
        int q0_len = timeline_to_quant_segments(bar_v0, r->ticks_per_bar, q0, MAX_SEGMENTS);
        int q1_len = timeline_to_quant_segments(bar_v1, r->ticks_per_bar, q1, MAX_SEGMENTS);
        printf("bar %d/%d\n", bar + 1, bars);
        recorder_print_voice(q0, q0_len, 0);
        recorder_print_voice(q1, q1_len, 1);
    }
    printf("---------------------------------------\n\n");
    fflush(stdout);
    free(v0_ticks);
    free(v1_ticks);

    r->phrase_active = 0;
    r->phrase_start_ms = now_ms;
    r->last_event_ms = now_ms;
    for (int i = 0; i < NUM_VOICES; i++) {
        r->voices[i].seg_len = 0;
        r->voices[i].current_note = -1;
        r->voices[i].segment_start_ms = now_ms;
    }
}

static int key_to_note(SDL_Scancode scancode, int *out_note) {
    for (int i = 0; i < g_num_keys; i++) {
        if (g_keys[i].scancode == scancode) {
            *out_note = g_keys[i].note_idx;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    SDL_Scancode scancode;
    int note_idx;
    uint64_t order;
} pressed_key_t;

static int collect_pressed_keys(pressed_key_t *out, int max_out) {
    int count = 0;
    for (int i = 0; i < g_num_keys; i++) {
        SDL_Scancode sc = g_keys[i].scancode;
        if (!g_key_down[sc]) continue;
        if (count >= max_out) break;
        out[count].scancode = sc;
        out[count].note_idx = g_keys[i].note_idx;
        out[count].order = g_key_press_order[sc];
        count++;
    }
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (out[j].order < out[i].order) {
                pressed_key_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return count;
}

static void sync_voices_from_pressed(sid_state_t *sid, recorder_t *rec, uint64_t now_ms) {
    pressed_key_t pressed[64];
    int pressed_count = collect_pressed_keys(pressed, 64);
    int target_count = pressed_count < NUM_VOICES ? pressed_count : NUM_VOICES;

    SDL_Scancode target_sc[NUM_VOICES] = {0};
    int target_note[NUM_VOICES] = {-1, -1};
    int target_used[NUM_VOICES] = {0};

    for (int i = 0; i < target_count; i++) {
        target_sc[i] = pressed[i].scancode;
        target_note[i] = pressed[i].note_idx;
    }

    SDL_LockAudioDevice(sid->audio_dev);

    for (int v = 0; v < NUM_VOICES; v++) {
        SDL_Scancode cur = (SDL_Scancode)sid->scancode_for_voice[v];
        if (cur == 0) continue;
        int keep_slot = -1;
        for (int t = 0; t < target_count; t++) {
            if (!target_used[t] && target_sc[t] == cur) {
                keep_slot = t;
                break;
            }
        }
        if (keep_slot != -1) {
            target_used[keep_slot] = 1;
            continue;
        }
        sid_voice_note_off(sid, v);
        sid->scancode_for_voice[v] = 0;
        sid->note_for_voice[v] = -1;
        recorder_note_off(rec, v, now_ms);
    }

    for (int t = 0; t < target_count; t++) {
        if (target_used[t]) continue;
        int free_voice = -1;
        for (int v = 0; v < NUM_VOICES; v++) {
            if (sid->scancode_for_voice[v] == 0) {
                free_voice = v;
                break;
            }
        }
        if (free_voice == -1) break;
        sid->scancode_for_voice[free_voice] = (int)target_sc[t];
        sid->note_for_voice[free_voice] = target_note[t];
        sid_voice_note_on(sid, free_voice, target_note[t]);
        recorder_note_on(rec, free_voice, target_note[t], now_ms);
        target_used[t] = 1;
    }

    SDL_UnlockAudioDevice(sid->audio_dev);
}

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    sid_state_t *sid = (sid_state_t *)userdata;
    float *out = (float *)stream;
    int n = len / (int)sizeof(float);

    for (int i = 0; i < n; i++) {
        while (sid->sample_counter >= sid->next_beat_sample) {
            int accent = ((sid->beat_index % 4ULL) == 0ULL);
            sid->click_samples_total = accent ? (int)(0.050 * SAMPLE_RATE) : (int)(0.035 * SAMPLE_RATE);
            sid->click_samples_left = sid->click_samples_total;
            sid->click_amp = accent ? 0.22 : 0.16;
            sid->click_freq = accent ? 1800.0 : 1200.0;
            sid->click_phase = 0.0;
            sid->next_beat_sample += sid->beat_samples;
            sid->beat_index++;
        }

        float sid_sample = 0.0f;
        for (;;) {
            sid->sid_pins = m6581_tick(&sid->sid, sid->sid_pins);
            if (sid->sid_pins & M6581_SAMPLE) {
                sid_sample = sid->sid.sample;
                break;
            }
        }

        float met_sample = 0.0f;
        if (sid->click_samples_left > 0 && sid->click_samples_total > 0) {
            double env = (double)sid->click_samples_left / (double)sid->click_samples_total;
            met_sample = (float)(sin(sid->click_phase) * sid->click_amp * env);
            sid->click_phase += (6.283185307179586 * sid->click_freq) / (double)SAMPLE_RATE;
            if (sid->click_phase > 6.283185307179586) sid->click_phase -= 6.283185307179586;
            sid->click_samples_left--;
        }

        float mixed = sid_sample + met_sample;
        if (mixed > 1.0f) mixed = 1.0f;
        if (mixed < -1.0f) mixed = -1.0f;
        out[i] = mixed;
        sid->sample_counter += 1.0;
    }
}

static void add_white_keys(int base_x, int base_y, SDL_Scancode *sc, int *notes, const char **labels, int count) {
    for (int i = 0; i < count; i++) {
        g_keys[g_num_keys].scancode = sc[i];
        g_keys[g_num_keys].note_idx = notes[i];
        g_keys[g_num_keys].is_black = 0;
        g_keys[g_num_keys].rect.x = base_x + i * WHITE_W;
        g_keys[g_num_keys].rect.y = base_y;
        g_keys[g_num_keys].rect.w = WHITE_W - 1;
        g_keys[g_num_keys].rect.h = WHITE_H;
        g_keys[g_num_keys].key_label = labels[i];
        g_num_keys++;
    }
}

static void add_black_keys(int base_x, int base_y, SDL_Scancode *sc, int *notes, const char **labels, int count) {
    const int white_after_index[7] = {1, 2, 4, 5, 6, 8, 9};
    for (int i = 0; i < count; i++) {
        int after = white_after_index[i];
        g_keys[g_num_keys].scancode = sc[i];
        g_keys[g_num_keys].note_idx = notes[i];
        g_keys[g_num_keys].is_black = 1;
        g_keys[g_num_keys].rect.x = base_x + after * WHITE_W - (BLACK_W / 2);
        g_keys[g_num_keys].rect.y = base_y;
        g_keys[g_num_keys].rect.w = BLACK_W;
        g_keys[g_num_keys].rect.h = BLACK_H;
        g_keys[g_num_keys].key_label = labels[i];
        g_num_keys++;
    }
}

static void init_key_layout(void) {
    static SDL_Scancode high_white_sc[10] = {
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T,
        SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P
    };
    static int high_white_notes[10] = {36, 38, 40, 41, 43, 45, 47, 48, 50, 52};
    static const char *high_white_lbl[10] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};

    static SDL_Scancode high_black_sc[7] = {
        SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_5, SDL_SCANCODE_6,
        SDL_SCANCODE_7, SDL_SCANCODE_9, SDL_SCANCODE_0
    };
    static int high_black_notes[7] = {37, 39, 42, 44, 46, 49, 51};
    static const char *high_black_lbl[7] = {"2", "3", "5", "6", "7", "9", "0"};

    static SDL_Scancode low_white_sc[10] = {
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B,
        SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH
    };
    static int low_white_notes[10] = {24, 26, 28, 29, 31, 33, 35, 36, 38, 40};
    static const char *low_white_lbl[10] = {"Z", "X", "C", "V", "B", "N", "M", ",", ".", "/"};
    static SDL_Scancode low_black_sc[7] = {
        SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_G, SDL_SCANCODE_H,
        SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON
    };
    static int low_black_notes[7] = {25, 27, 30, 32, 34, 37, 39};
    static const char *low_black_lbl[7] = {"S", "D", "G", "H", "J", "L", ";"};

    g_num_keys = 0;
    add_white_keys(40, 40, high_white_sc, high_white_notes, high_white_lbl, 10);
    add_black_keys(40, 40, high_black_sc, high_black_notes, high_black_lbl, 7);
    add_white_keys(40, 260, low_white_sc, low_white_notes, low_white_lbl, 10);
    add_black_keys(40, 260, low_black_sc, low_black_notes, low_black_lbl, 7);
}

static void draw_keyboard(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 20, 24, 32, 255);
    SDL_RenderClear(renderer);

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_num_keys; i++) {
            keydef_t *k = &g_keys[i];
            if ((pass == 0 && k->is_black) || (pass == 1 && !k->is_black)) continue;

            int pressed = g_key_down[k->scancode] != 0;
            if (k->is_black) {
                if (pressed) SDL_SetRenderDrawColor(renderer, 210, 170, 70, 255);
                else SDL_SetRenderDrawColor(renderer, 24, 24, 26, 255);
            } else {
                if (pressed) SDL_SetRenderDrawColor(renderer, 244, 214, 124, 255);
                else SDL_SetRenderDrawColor(renderer, 238, 238, 232, 255);
            }
            SDL_RenderFillRect(renderer, &k->rect);
            SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
            SDL_RenderDrawRect(renderer, &k->rect);
        }
    }

    SDL_SetRenderDrawColor(renderer, 70, 78, 94, 255);
    SDL_Rect sep = {.x = 30, .y = 230, .w = WINDOW_W - 60, .h = 2};
    SDL_RenderFillRect(renderer, &sep);

    SDL_RenderPresent(renderer);
}

static void print_mapping(void) {
    printf("sid-keys: 2-voice SID sketch keyboard\n");
    printf("Upper white  (C3..E4): Q W E R T Y U I O P\n");
    printf("Upper black  (C#3..D#4): 2 3 5 6 7 9 0\n");
    printf("Lower white  (C2..E3): Z X C V B N M , . /\n");
    printf("Lower black  (C#2..D#3): S D G H J L ;\n");
    printf("Controls: ESC=quit, SPACE=flush phrase at current bar end\n");
    printf("Output: voice1/voice2 tokens with durations quantized to %d ticks\n", TICK_QUANTUM);
    fflush(stdout);
}

int main(int argc, char **argv) {
    double bpm = DEFAULT_BPM;
    int ticks_per_bar = DEFAULT_TICKS_PER_BAR;
    uint64_t silence_ms = DEFAULT_SILENCE_MS;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bpm") && i + 1 < argc) {
            bpm = atof(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc) {
            ticks_per_bar = (int)strtol(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--silence-ms") && i + 1 < argc) {
            silence_ms = (uint64_t)strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--bpm N] [--ticks N] [--silence-ms N]\n", argv[0]);
            printf("  --bpm N         Metronome BPM (default %.1f)\n", DEFAULT_BPM);
            printf("  --ticks N       Ticks per 4/4 bar (default %d)\n", DEFAULT_TICKS_PER_BAR);
            printf("  --silence-ms N  Flush phrase after N ms silence (default %d)\n", DEFAULT_SILENCE_MS);
            return 0;
        }
        fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        fprintf(stderr, "Try: %s --help\n", argv[0]);
        return 1;
    }

    if (bpm <= 0.0) bpm = DEFAULT_BPM;
    if (ticks_per_bar < TICK_QUANTUM) ticks_per_bar = DEFAULT_TICKS_PER_BAR;
    if (ticks_per_bar % TICK_QUANTUM != 0) {
        int rounded = ((ticks_per_bar + (TICK_QUANTUM / 2)) / TICK_QUANTUM) * TICK_QUANTUM;
        if (rounded < TICK_QUANTUM) rounded = TICK_QUANTUM;
        fprintf(stderr, "Note: --ticks adjusted to %d (must be multiple of %d)\n", rounded, TICK_QUANTUM);
        ticks_per_bar = rounded;
    }
    if (silence_ms == 0) silence_ms = DEFAULT_SILENCE_MS;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("sid-keys",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    sid_state_t sid;
    sid_init(&sid, bpm);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_callback;
    want.userdata = &sid;

    sid.audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!sid.audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    init_key_layout();
    print_mapping();
    printf("Timing: bpm=%.1f, ticks/bar=%d, silence=%llums, quant=%d ticks\n\n",
           bpm, ticks_per_bar, (unsigned long long)silence_ms, TICK_QUANTUM);
    fflush(stdout);

    uint64_t start_ms = SDL_GetTicks64();
    recorder_t rec;
    recorder_reset(&rec, start_ms);
    rec.bpm = bpm;
    rec.ticks_per_bar = ticks_per_bar;
    rec.transport_start_ms = start_ms;
    rec.silence_ms = silence_ms;

    SDL_PauseAudioDevice(sid.audio_dev, 0);

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Scancode sc = ev.key.keysym.scancode;
                if (ev.key.repeat) continue;

                if (sc == SDL_SCANCODE_ESCAPE) {
                    running = 0;
                    continue;
                }
                if (sc == SDL_SCANCODE_SPACE) {
                    if (rec.phrase_active) {
                        uint64_t end_ms = recorder_bar_end_ms_containing(&rec, SDL_GetTicks64());
                        recorder_flush_phrase(&rec, end_ms);
                    }
                    continue;
                }

                g_key_down[sc] = 1;
                int note_idx = -1;
                if (!key_to_note(sc, &note_idx)) continue;
                g_key_press_order[sc] = g_press_counter++;
                sync_voices_from_pressed(&sid, &rec, SDL_GetTicks64());
            } else if (ev.type == SDL_KEYUP) {
                SDL_Scancode sc = ev.key.keysym.scancode;
                g_key_down[sc] = 0;
                int note_idx = -1;
                if (!key_to_note(sc, &note_idx)) continue;
                g_key_press_order[sc] = 0;
                sync_voices_from_pressed(&sid, &rec, SDL_GetTicks64());
            }
        }

        int all_idle = 1;
        for (int i = 0; i < NUM_VOICES; i++) {
            if (sid.scancode_for_voice[i] != 0) {
                all_idle = 0;
                break;
            }
        }

        uint64_t now = SDL_GetTicks64();
        if (rec.phrase_active && all_idle && now - rec.last_event_ms >= rec.silence_ms) {
            uint64_t end_ms = recorder_bar_end_ms_containing(&rec, rec.last_event_ms);
            recorder_flush_phrase(&rec, end_ms);
        }

        draw_keyboard(renderer);
    }

    SDL_CloseAudioDevice(sid.audio_dev);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
