#ifndef AUDIO_SPU_RAM_H
#define AUDIO_SPU_RAM_H

#include <stdint.h>

#define SPU_RAM_GRANULARITY_BYTES 16u
#define SPU_RAM_MAX_FREE_RANGES   512
#define SPU_RAM_NO_ADDRESS        0xFFFFFFFFu

/* Clip area: from just after the reserved silent block up to the reverb reserve at the top of the 2 MiB. */
#define SPU_RAM_CLIP_AREA_START 0x005010u
#define SPU_RAM_CLIP_AREA_END   0x1E0000u

typedef struct spu_ram_range
{
    uint32_t address;
    uint32_t size_bytes;
} spu_ram_range_t;

/* First-fit allocator over SPU2 RAM. Tracks only free ranges; callers keep each block's size. */
typedef struct spu_ram
{
    uint32_t area_start;
    uint32_t area_end;
    uint32_t free_range_count;
    spu_ram_range_t free_ranges[SPU_RAM_MAX_FREE_RANGES];
} spu_ram_t;

void spu_ram_init(spu_ram_t *ram, uint32_t area_start, uint32_t area_end);
void spu_ram_reset(spu_ram_t *ram);

/* Returns the address of a block of at least size_bytes (rounded up to 16), or SPU_RAM_NO_ADDRESS. */
uint32_t spu_ram_allocate(spu_ram_t *ram, uint32_t size_bytes);

/* Takes exactly [address, address + size_bytes). Returns 0, or -1 if any part is outside the area, misaligned or already taken. */
int spu_ram_allocate_at(spu_ram_t *ram, uint32_t address, uint32_t size_bytes);

/* Returns 0, or -1 if the block is outside the area, misaligned, overlaps free space, or the free list is full. */
int spu_ram_release(spu_ram_t *ram, uint32_t address, uint32_t size_bytes);

uint32_t spu_ram_get_free_bytes(const spu_ram_t *ram);
uint32_t spu_ram_get_largest_free_block(const spu_ram_t *ram);

#endif
