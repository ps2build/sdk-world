#include "audio_pcm.h"

#include <string.h>

int audio_pcm_converter_init(audio_pcm_converter_t *converter, int32_t sample_rate_hz, int32_t bits_per_sample, int32_t channel_count)
{
    if (sample_rate_hz < 8000 || sample_rate_hz > AUDIO_PCM_OUTPUT_RATE_HZ)
        return -1;
    if (bits_per_sample != 8 && bits_per_sample != 16)
        return -1;
    if (channel_count != 1 && channel_count != 2)
        return -1;

    converter->sample_rate_hz = sample_rate_hz;
    converter->bits_per_sample = bits_per_sample;
    converter->channel_count = channel_count;
    converter->step_q16 = ((uint32_t)sample_rate_hz << 16) / AUDIO_PCM_OUTPUT_RATE_HZ;
    audio_pcm_converter_reset(converter);
    return 0;
}

void audio_pcm_converter_reset(audio_pcm_converter_t *converter)
{
    converter->phase_q16 = 0;
    converter->previous_left = 0;
    converter->previous_right = 0;
    converter->have_previous = 0;
    converter->accumulated_frames = 0;
}

int32_t audio_pcm_bytes_per_source_frame(const audio_pcm_converter_t *converter)
{
    return (converter->bits_per_sample / 8) * converter->channel_count;
}

static void read_frame(const audio_pcm_converter_t *converter, const uint8_t *frame, int32_t *left, int32_t *right)
{
    if (converter->bits_per_sample == 8) {
        *left = (int32_t)(int8_t)frame[0] * 256;
        *right = converter->channel_count == 2 ? (int32_t)(int8_t)frame[1] * 256 : *left;
    } else {
        *left = (int16_t)((uint16_t)frame[0] | ((uint16_t)frame[1] << 8));
        *right = converter->channel_count == 2 ? (int16_t)((uint16_t)frame[2] | ((uint16_t)frame[3] << 8)) : *left;
    }
}

static int32_t interpolate(int32_t from, int32_t to, uint32_t phase_q16)
{
    return from + (int32_t)(((int64_t)(to - from) * (int64_t)phase_q16) >> 16);
}

static void write_block(const audio_pcm_converter_t *converter, uint8_t *block)
{
    for (uint32_t i = 0; i < AUDIO_PCM_BLOCK_FRAMES; i++) {
        uint16_t left = (uint16_t)converter->left[i];
        uint16_t right = (uint16_t)converter->right[i];

        block[i * 2] = (uint8_t)(left & 0xFF);
        block[i * 2 + 1] = (uint8_t)(left >> 8);
        block[AUDIO_PCM_BLOCK_BYTES / 2 + i * 2] = (uint8_t)(right & 0xFF);
        block[AUDIO_PCM_BLOCK_BYTES / 2 + i * 2 + 1] = (uint8_t)(right >> 8);
    }
}

uint32_t audio_pcm_convert(audio_pcm_converter_t *converter, const uint8_t *source, uint32_t source_bytes,
                           uint8_t *out_blocks, uint32_t max_blocks, uint32_t *blocks_out)
{
    uint32_t frame_bytes = (uint32_t)audio_pcm_bytes_per_source_frame(converter);
    uint32_t consumed = 0;
    uint32_t blocks = 0;

    while (consumed + frame_bytes <= source_bytes) {
        int32_t left, right;

        if (blocks >= max_blocks && converter->accumulated_frames + AUDIO_PCM_MAX_FRAMES_PER_SOURCE_FRAME >= AUDIO_PCM_BLOCK_FRAMES)
            break;

        read_frame(converter, source + consumed, &left, &right);
        consumed += frame_bytes;

        if (!converter->have_previous) {
            converter->previous_left = left;
            converter->previous_right = right;
            converter->have_previous = 1;
            continue;
        }

        while (converter->phase_q16 < 65536u) {
            converter->left[converter->accumulated_frames] = (int16_t)interpolate(converter->previous_left, left, converter->phase_q16);
            converter->right[converter->accumulated_frames] = (int16_t)interpolate(converter->previous_right, right, converter->phase_q16);
            converter->accumulated_frames++;
            converter->phase_q16 += converter->step_q16;

            if (converter->accumulated_frames == AUDIO_PCM_BLOCK_FRAMES) {
                write_block(converter, out_blocks + (size_t)blocks * AUDIO_PCM_BLOCK_BYTES);
                blocks++;
                converter->accumulated_frames = 0;
            }
        }
        converter->phase_q16 -= 65536u;
        converter->previous_left = left;
        converter->previous_right = right;
    }

    *blocks_out = blocks;
    return consumed;
}
