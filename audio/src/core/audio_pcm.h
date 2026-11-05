#ifndef AUDIO_PCM_H
#define AUDIO_PCM_H

#include <stdint.h>

#define AUDIO_PCM_OUTPUT_RATE_HZ    48000
#define AUDIO_PCM_BLOCK_FRAMES      256u
#define AUDIO_PCM_BLOCK_BYTES       1024u
#define AUDIO_PCM_MAX_FRAMES_PER_SOURCE_FRAME 7u

/*
 * Converts 8/16-bit mono/stereo PCM at 8000-48000 Hz to the SPU2 input layout: 48 kHz, 16-bit, in
 * 1024-byte blocks holding 256 left samples followed by 256 right samples.
 */
typedef struct audio_pcm_converter
{
    int32_t sample_rate_hz;
    int32_t bits_per_sample;
    int32_t channel_count;
    uint32_t step_q16;
    uint32_t phase_q16;
    int32_t previous_left;
    int32_t previous_right;
    int have_previous;
    uint32_t accumulated_frames;
    int16_t left[AUDIO_PCM_BLOCK_FRAMES];
    int16_t right[AUDIO_PCM_BLOCK_FRAMES];
} audio_pcm_converter_t;

/* Returns 0, or -1 for a rate, bit depth or channel count outside the supported set. */
int audio_pcm_converter_init(audio_pcm_converter_t *converter, int32_t sample_rate_hz, int32_t bits_per_sample, int32_t channel_count);

/* Drops buffered partial-block data and resampler history; keeps the format. */
void audio_pcm_converter_reset(audio_pcm_converter_t *converter);

int32_t audio_pcm_bytes_per_source_frame(const audio_pcm_converter_t *converter);

/*
 * Converts whole source frames from source until it runs out or max_blocks output blocks are full.
 * Returns the number of source bytes consumed and sets *blocks_out. A partial block stays inside the
 * converter and completes on a later call.
 */
uint32_t audio_pcm_convert(audio_pcm_converter_t *converter, const uint8_t *source, uint32_t source_bytes,
                           uint8_t *out_blocks, uint32_t max_blocks, uint32_t *blocks_out);

#endif
