#include "audio_adpcm.h"

#define APCM_PITCH_OFFSET 8
#define APCM_RATE_MIN_HZ  4000
#define APCM_RATE_MAX_HZ  48000

/* One pitch step is 48000 / 4096 = 11.7 Hz, so rates this close to a standard rate are that rate. */
#define APCM_RATE_SNAP_HZ 12

static int32_t apcm_sample_rate(const uint8_t *header)
{
    static const int32_t standard_rates[] = {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};
    uint32_t pitch = (uint32_t)header[APCM_PITCH_OFFSET] | (uint32_t)header[APCM_PITCH_OFFSET + 1] << 8 |
                     (uint32_t)header[APCM_PITCH_OFFSET + 2] << 16 | (uint32_t)header[APCM_PITCH_OFFSET + 3] << 24;
    int32_t rate;

    if (pitch == 0 || pitch > 0x1000)
        return AUDIO_APCM_SAMPLE_RATE_HZ;
    rate = (int32_t)((pitch * 48000u + 2048u) / 4096u);
    if (rate < APCM_RATE_MIN_HZ || rate > APCM_RATE_MAX_HZ)
        return AUDIO_APCM_SAMPLE_RATE_HZ;
    for (unsigned i = 0; i < sizeof(standard_rates) / sizeof(standard_rates[0]); i++) {
        int32_t difference = rate - standard_rates[i];

        if (difference >= -APCM_RATE_SNAP_HZ && difference <= APCM_RATE_SNAP_HZ)
            return standard_rates[i];
    }
    return rate;
}

int audio_apcm_parse(const void *file_data, uint32_t file_size_bytes, audio_adpcm_view_t *view)
{
    const uint8_t *bytes = file_data;
    uint32_t body_bytes;
    int32_t channel_count;

    if (!file_data || !view || file_size_bytes < AUDIO_APCM_HEADER_BYTES + AUDIO_ADPCM_BLOCK_BYTES)
        return -1;
    if (bytes[0] != 'A' || bytes[1] != 'P' || bytes[2] != 'C' || bytes[3] != 'M' || bytes[4] != 1)
        return -1;

    channel_count = bytes[5];
    if (channel_count != 1 && channel_count != 2)
        return -1;

    body_bytes = file_size_bytes - AUDIO_APCM_HEADER_BYTES;
    if (body_bytes % AUDIO_ADPCM_BLOCK_BYTES != 0 || (body_bytes / AUDIO_ADPCM_BLOCK_BYTES) % (uint32_t)channel_count != 0)
        return -1;

    view->blocks = bytes + AUDIO_APCM_HEADER_BYTES;
    view->size_bytes = body_bytes;
    view->channel_count = channel_count;
    view->loops = bytes[6] ? 1 : 0;
    view->sample_rate_hz = apcm_sample_rate(bytes);
    return 0;
}

void audio_adpcm_patch_flags(uint8_t *chunk, uint32_t chunk_size_bytes, uint32_t chunk_offset_bytes,
                             uint32_t clip_size_bytes, int channel_count, int loops, uint32_t pad_blocks)
{
    uint32_t run_bytes = clip_size_bytes / (uint32_t)channel_count;
    uint32_t loop_end_position;

    if (run_bytes <= pad_blocks * AUDIO_ADPCM_BLOCK_BYTES)
        pad_blocks = 0;
    loop_end_position = run_bytes - (pad_blocks + 1u) * AUDIO_ADPCM_BLOCK_BYTES;

    for (uint32_t offset = 0; offset + AUDIO_ADPCM_BLOCK_BYTES <= chunk_size_bytes; offset += AUDIO_ADPCM_BLOCK_BYTES) {
        uint32_t position = (chunk_offset_bytes + offset) % run_bytes;
        int is_first = position == 0;
        int is_last = position == run_bytes - AUDIO_ADPCM_BLOCK_BYTES;
        uint8_t flags = 0;

        if (loops) {
            if (position > loop_end_position)
                continue;
            is_last = position == loop_end_position;
            flags = AUDIO_ADPCM_FLAG_LOOP_REPEAT;
            if (is_first)
                flags |= AUDIO_ADPCM_FLAG_LOOP_START;
            if (is_last)
                flags |= AUDIO_ADPCM_FLAG_LOOP_END;
        } else if (is_last) {
            flags = AUDIO_ADPCM_FLAG_LOOP_END;
        }
        chunk[offset + 1] = flags;
    }
}
