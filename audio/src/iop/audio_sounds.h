#ifndef AUDIO_SOUNDS_H
#define AUDIO_SOUNDS_H

#include <stdint.h>

#define AUDIO_SOUND_HANDLE_SLOT_BITS 12u
#define AUDIO_SOUND_MAX_CAPACITY     ((1u << AUDIO_SOUND_HANDLE_SLOT_BITS) - 1u)
#define AUDIO_SOUND_GENERATION_MASK  0x7FFFFu

typedef struct audio_sound_entry
{
    uint32_t spu_address;
    uint32_t size_bytes;
    uint32_t sample_rate_hz;
    uint32_t generation;
    uint8_t channel_count;
    uint8_t loops;
    uint8_t in_use;
} audio_sound_entry_t;

/* Handles are (generation << 12) | (slot + 1): >= 1, never reused for a different clip until the generation wraps. */
typedef struct audio_sound_table
{
    audio_sound_entry_t *entries;
    uint32_t capacity;
    uint32_t live_count;
} audio_sound_table_t;

void audio_sound_table_init(audio_sound_table_t *table, audio_sound_entry_t *storage, uint32_t capacity);
void audio_sound_table_clear(audio_sound_table_t *table);

/* Returns a handle >= 1, or -1 if the table is full. */
int32_t audio_sound_table_add(audio_sound_table_t *table, uint32_t spu_address, uint32_t size_bytes,
                              uint32_t sample_rate_hz, int channel_count, int loops);

/* Returns NULL for an unknown, freed or stale handle. */
const audio_sound_entry_t *audio_sound_table_get(const audio_sound_table_t *table, int32_t handle);

/* Returns 0, or -1 for an unknown, freed or stale handle. */
int audio_sound_table_remove(audio_sound_table_t *table, int32_t handle);

#endif
