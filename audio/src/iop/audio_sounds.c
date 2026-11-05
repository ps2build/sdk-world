#include "audio_sounds.h"

static uint32_t next_generation(uint32_t generation)
{
    generation = (generation + 1u) & AUDIO_SOUND_GENERATION_MASK;
    return generation == 0 ? 1u : generation;
}

static int32_t make_handle(uint32_t generation, uint32_t slot)
{
    return (int32_t)((generation << AUDIO_SOUND_HANDLE_SLOT_BITS) | (slot + 1u));
}

static audio_sound_entry_t *find_entry(const audio_sound_table_t *table, int32_t handle)
{
    uint32_t value = (uint32_t)handle;
    uint32_t slot_plus_one = value & ((1u << AUDIO_SOUND_HANDLE_SLOT_BITS) - 1u);
    uint32_t generation = value >> AUDIO_SOUND_HANDLE_SLOT_BITS;
    audio_sound_entry_t *entry;

    if (handle <= 0 || slot_plus_one == 0 || slot_plus_one > table->capacity)
        return 0;
    entry = &table->entries[slot_plus_one - 1u];
    if (!entry->in_use || entry->generation != generation)
        return 0;
    return entry;
}

void audio_sound_table_init(audio_sound_table_t *table, audio_sound_entry_t *storage, uint32_t capacity)
{
    table->entries = storage;
    table->capacity = capacity > AUDIO_SOUND_MAX_CAPACITY ? AUDIO_SOUND_MAX_CAPACITY : capacity;
    for (uint32_t i = 0; i < table->capacity; i++) {
        table->entries[i].in_use = 0;
        table->entries[i].generation = 0;
    }
    table->live_count = 0;
}

void audio_sound_table_clear(audio_sound_table_t *table)
{
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->entries[i].in_use) {
            table->entries[i].in_use = 0;
            table->entries[i].generation = next_generation(table->entries[i].generation);
        }
    }
    table->live_count = 0;
}

int32_t audio_sound_table_add(audio_sound_table_t *table, uint32_t spu_address, uint32_t size_bytes,
                              uint32_t sample_rate_hz, int channel_count, int loops)
{
    for (uint32_t slot = 0; slot < table->capacity; slot++) {
        audio_sound_entry_t *entry = &table->entries[slot];

        if (entry->in_use)
            continue;
        entry->generation = next_generation(entry->generation);
        entry->spu_address = spu_address;
        entry->size_bytes = size_bytes;
        entry->sample_rate_hz = sample_rate_hz;
        entry->channel_count = (uint8_t)channel_count;
        entry->loops = loops ? 1 : 0;
        entry->in_use = 1;
        table->live_count++;
        return make_handle(entry->generation, slot);
    }
    return -1;
}

const audio_sound_entry_t *audio_sound_table_get(const audio_sound_table_t *table, int32_t handle)
{
    return find_entry(table, handle);
}

int audio_sound_table_remove(audio_sound_table_t *table, int32_t handle)
{
    audio_sound_entry_t *entry = find_entry(table, handle);

    if (!entry)
        return -1;
    entry->in_use = 0;
    table->live_count--;
    return 0;
}
