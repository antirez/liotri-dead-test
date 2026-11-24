#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFERS_COUNT 2
#define C64_RING_CAPACITY (1u << 14) /* 16384 samples */
#define AUDIO_BUFFER_SAMPLES 512
/* Allow two 20 ms emulator frames plus callback scheduling jitter. Trimming
 * to just 1024 samples discards normal frame bursts and causes underruns. */
#define MAX_QUEUED_SAMPLES (AUDIO_BUFFER_SAMPLES * 4)

/* macOS audio backend notes:
 * - AudioQueue callback runs on a realtime thread: no blocking locks/realloc.
 * - Emulator callback is single producer, AudioQueue callback is single consumer.
 * - Only the consumer advances read_pos, trimming old backlog to bound latency.
 * - The producer drops incoming samples if the ring is full; it must never
 *   overwrite samples the consumer may still be reading.
 */
typedef struct {
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[BUFFERS_COUNT];
    UInt32 bufferSize;

    short *ring;
    uint32_t ring_mask;
    _Atomic uint32_t read_pos;
    _Atomic uint32_t write_pos;

    short last_sample;
} AudioState;

static inline float clampf_pm1(float x) {
    if (x > 1.0f) return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
}

static void BufferCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    (void)inAQ;
    AudioState *state = (AudioState *)inUserData;
    short *out = (short *)inBuffer->mAudioData;
    const uint32_t req = inBuffer->mAudioDataBytesCapacity / (UInt32)sizeof(short);

    uint32_t read = atomic_load_explicit(&state->read_pos, memory_order_acquire);
    const uint32_t write = atomic_load_explicit(&state->write_pos, memory_order_acquire);
    uint32_t avail = write - read;
    if (avail > C64_RING_CAPACITY) {
        /* Shouldn't happen in normal flow, but recover conservatively. */
        avail = C64_RING_CAPACITY;
    }
    if (avail > MAX_QUEUED_SAMPLES) {
        read += avail - MAX_QUEUED_SAMPLES;
        avail = MAX_QUEUED_SAMPLES;
    }

    uint32_t copy = req;
    if (copy > avail) copy = avail;

    for (uint32_t i = 0; i < copy; i++) {
        const short s = state->ring[(read + i) & state->ring_mask];
        out[i] = s;
        state->last_sample = s;
    }

    /* Fill underrun tail with last sample to avoid hard zero-edge clicks. */
    for (uint32_t i = copy; i < req; i++) {
        out[i] = state->last_sample;
    }

    read += copy;
    atomic_store_explicit(&state->read_pos, read, memory_order_release);

    inBuffer->mAudioDataByteSize = req * (UInt32)sizeof(short);
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

void audio_from_emulator(const float *samples, int num_samples, void *user_data) {
    AudioState *state = (AudioState *)user_data;
    if (!state || !samples || num_samples <= 0) {
        return;
    }

    /* If producer burst is bigger than ring, keep newest tail only. */
    uint32_t n = (uint32_t)num_samples;
    const float *src = samples;
    if (n > C64_RING_CAPACITY) {
        src += (n - C64_RING_CAPACITY);
        n = C64_RING_CAPACITY;
    }

    uint32_t read = atomic_load_explicit(&state->read_pos, memory_order_acquire);
    uint32_t write = atomic_load_explicit(&state->write_pos, memory_order_relaxed);

    uint32_t used = write - read;
    if (used > C64_RING_CAPACITY) {
        used = C64_RING_CAPACITY;
    }
    uint32_t free_space = C64_RING_CAPACITY - used;

    if (n > free_space) {
        n = free_space;
    }

    for (uint32_t i = 0; i < n; i++) {
        const float s = clampf_pm1(src[i]);
        state->ring[(write + i) & state->ring_mask] = (short)(s * 32767.0f);
    }
    write += n;
    atomic_store_explicit(&state->write_pos, write, memory_order_release);
}

void *audio_init(void) {
    AudioState *state = (AudioState *)calloc(1, sizeof(*state));
    if (!state) {
        return NULL;
    }

    state->ring = (short *)calloc(C64_RING_CAPACITY, sizeof(short));
    if (!state->ring) {
        free(state);
        return NULL;
    }
    state->ring_mask = C64_RING_CAPACITY - 1;
    state->last_sample = 0;

    AudioStreamBasicDescription format = {0};
    format.mSampleRate = 44100;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = 1;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2;
    format.mBytesPerPacket = 2;

    OSStatus status = AudioQueueNewOutput(&format, BufferCallback, state, NULL, NULL, 0, &state->queue);
    if (status) {
        fprintf(stderr, "Error creating audio queue: %d\n", (int)status);
        free(state->ring);
        free(state);
        return NULL;
    }

    state->bufferSize = (UInt32)(AUDIO_BUFFER_SAMPLES * sizeof(short));

    for (int i = 0; i < BUFFERS_COUNT; i++) {
        status = AudioQueueAllocateBuffer(state->queue, state->bufferSize, &state->buffers[i]);
        if (status) {
            fprintf(stderr, "Error allocating buffer %d: %d\n", i, (int)status);
            AudioQueueDispose(state->queue, true);
            free(state->ring);
            free(state);
            return NULL;
        }
        memset(state->buffers[i]->mAudioData, 0, state->bufferSize);
        state->buffers[i]->mAudioDataByteSize = state->bufferSize;
        AudioQueueEnqueueBuffer(state->queue, state->buffers[i], 0, NULL);
    }

    status = AudioQueueStart(state->queue, NULL);
    if (status) {
        fprintf(stderr, "Error starting audio queue: %d\n", (int)status);
        AudioQueueDispose(state->queue, true);
        free(state->ring);
        free(state);
        return NULL;
    }

    return (void *)state;
}

void audio_cleanup(void *user_data) {
    AudioState *state = (AudioState *)user_data;
    if (!state) return;

    AudioQueueStop(state->queue, true);
    AudioQueueDispose(state->queue, true);

    free(state->ring);
    free(state);
}
