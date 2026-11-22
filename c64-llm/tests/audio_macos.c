/* Exercise the real ring/callback without starting an audio device. */
#include <AudioToolbox/AudioToolbox.h>
#include <assert.h>

static OSStatus enqueue(AudioQueueRef queue, AudioQueueBufferRef buffer,
                        UInt32 count, const AudioStreamPacketDescription *packets) {
    (void)queue;
    (void)buffer;
    (void)count;
    (void)packets;
    return noErr;
}

#define AudioQueueEnqueueBuffer enqueue
#include "../audio_macos.c"
#undef AudioQueueEnqueueBuffer

static short ring[C64_RING_CAPACITY];
static short output[AUDIO_BUFFER_SAMPLES];
static AudioQueueBuffer buffer = {
    .mAudioDataBytesCapacity = sizeof(output),
    .mAudioData = output,
};

static void init(AudioState *state, uint32_t position) {
    memset(state, 0, sizeof(*state));
    state->ring = ring;
    state->ring_mask = C64_RING_CAPACITY - 1;
    atomic_init(&state->read_pos, position);
    atomic_init(&state->write_pos, position);
}

static void produce(AudioState *state, unsigned *sequence) {
    float samples[256];
    for (unsigned i = 0; i < 256; i++) {
        /* Conversion yields the integer sequence 0..29999, then repeats. */
        samples[i] = (float)(1 + (*sequence)++ % 30000) / 32768.0f;
    }
    audio_from_emulator(samples, 256, state);
}

static void test_frame_bursts(uint32_t position) {
    AudioState state;
    init(&state, position);
    unsigned produced = 0, played = 0;
    for (int i = 0; i < 4; i++) produce(&state, &produced);

    /* 20 ms frames at 44100 Hz, delivered in the frontend's 256-sample
     * chunks. AudioQueue may request both 512-sample buffers together.
     * Run ten seconds, including all relative phases of the two cadences.
     */
    unsigned next_frame = 882, next_output = 1024, pending = 0;
    while (next_output <= 441000) {
        if (next_frame <= next_output) {
            pending += 882;
            while (pending >= 256) {
                produce(&state, &produced);
                pending -= 256;
            }
            next_frame += 882;
        } else {
            for (int i = 0; i < 2; i++) {
                uint32_t read = atomic_load(&state.read_pos);
                BufferCallback(&state, NULL, &buffer);
                assert(atomic_load(&state.read_pos) - read == AUDIO_BUFFER_SAMPLES);
                for (unsigned j = 0; j < AUDIO_BUFFER_SAMPLES; j++) {
                    assert(output[j] == (short)(played++ % 30000));
                }
            }
            next_output += 1024;
        }
    }
}

static void test_full_ring(void) {
    AudioState state;
    init(&state, 0);
    float samples[C64_RING_CAPACITY];
    for (unsigned i = 0; i < C64_RING_CAPACITY; i++) samples[i] = 0.25f;
    audio_from_emulator(samples, C64_RING_CAPACITY, &state);
    assert(atomic_load(&state.write_pos) == C64_RING_CAPACITY);
    samples[0] = -0.5f;
    audio_from_emulator(samples, 1, &state);
    /* The producer cannot move the read position or overwrite unread data. */
    assert(atomic_load(&state.read_pos) == 0);
    assert(atomic_load(&state.write_pos) == C64_RING_CAPACITY);
    assert(ring[0] == (short)(0.25f * 32767));

    BufferCallback(&state, NULL, &buffer);
    assert(atomic_load(&state.write_pos) - atomic_load(&state.read_pos) ==
           MAX_QUEUED_SAMPLES - AUDIO_BUFFER_SAMPLES);
    for (unsigned i = 0; i < AUDIO_BUFFER_SAMPLES; i++) {
        assert(output[i] == (short)(0.25f * 32767));
    }
    audio_from_emulator(samples, 1, &state);
    assert(atomic_load(&state.write_pos) == C64_RING_CAPACITY + 1);
    assert(ring[0] == (short)(-0.5f * 32767));
}

int main(void) {
    test_frame_bursts(0);
    test_frame_bursts(UINT32_MAX - 511); // cursor wrap must preserve occupancy
    test_full_ring();
    puts("macOS audio buffering tests passed");
    return 0;
}
