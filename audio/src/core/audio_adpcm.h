#ifndef AUDIO_ADPCM_H
#define AUDIO_ADPCM_H

#include <stdint.h>

#define AUDIO_ADPCM_BLOCK_BYTES        16u
#define AUDIO_ADPCM_FLAG_LOOP_END      0x01
#define AUDIO_ADPCM_FLAG_LOOP_REPEAT   0x02
#define AUDIO_ADPCM_FLAG_LOOP_START    0x04
#define AUDIO_APCM_HEADER_BYTES        16u
#define AUDIO_APCM_SAMPLE_RATE_HZ      44100
#define AUDIO_APCM_PAD_BLOCKS          1u

typedef struct audio_adpcm_view
{
    const uint8_t *blocks;
    uint32_t size_bytes;
    int32_t channel_count;
    int32_t loops;
    int32_t sample_rate_hz;
} audio_adpcm_view_t;

/* Returns 0 and fills view for a valid APCM file image, or -1. The view points into file_data. */
int audio_apcm_parse(const void *file_data, uint32_t file_size_bytes, audio_adpcm_view_t *view);

/*
 * Rewrites the loop flags of the 16-byte blocks in chunk, which starts chunk_offset_bytes into a clip of
 * clip_size_bytes holding channel_count equal runs. Looping clips repeat each run; other clips end each run.
 * A looping run ends its loop pad_blocks blocks before its last block, and the flags of those trailing blocks
 * are left as they are. A run that is not longer than pad_blocks is patched as if pad_blocks were 0.
 */
void audio_adpcm_patch_flags(uint8_t *chunk, uint32_t chunk_size_bytes, uint32_t chunk_offset_bytes,
                             uint32_t clip_size_bytes, int channel_count, int loops, uint32_t pad_blocks);

#endif
