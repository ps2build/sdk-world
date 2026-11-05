#include "spu_ram.h"

static uint32_t round_up_to_granularity(uint32_t size_bytes)
{
    return (size_bytes + SPU_RAM_GRANULARITY_BYTES - 1u) & ~(SPU_RAM_GRANULARITY_BYTES - 1u);
}

static void remove_range(spu_ram_t *ram, uint32_t index)
{
    for (uint32_t i = index + 1; i < ram->free_range_count; i++)
        ram->free_ranges[i - 1] = ram->free_ranges[i];
    ram->free_range_count--;
}

static void insert_range(spu_ram_t *ram, uint32_t index, uint32_t address, uint32_t size_bytes)
{
    for (uint32_t i = ram->free_range_count; i > index; i--)
        ram->free_ranges[i] = ram->free_ranges[i - 1];
    ram->free_ranges[index].address = address;
    ram->free_ranges[index].size_bytes = size_bytes;
    ram->free_range_count++;
}

void spu_ram_init(spu_ram_t *ram, uint32_t area_start, uint32_t area_end)
{
    ram->area_start = area_start;
    ram->area_end = area_end;
    spu_ram_reset(ram);
}

void spu_ram_reset(spu_ram_t *ram)
{
    ram->free_range_count = 0;
    if (ram->area_end > ram->area_start)
        insert_range(ram, 0, ram->area_start, ram->area_end - ram->area_start);
}

uint32_t spu_ram_allocate(spu_ram_t *ram, uint32_t size_bytes)
{
    if (size_bytes == 0 || size_bytes > ram->area_end - ram->area_start)
        return SPU_RAM_NO_ADDRESS;

    size_bytes = round_up_to_granularity(size_bytes);

    for (uint32_t i = 0; i < ram->free_range_count; i++) {
        spu_ram_range_t *range = &ram->free_ranges[i];
        uint32_t address;

        if (range->size_bytes < size_bytes)
            continue;

        address = range->address;
        if (range->size_bytes == size_bytes) {
            remove_range(ram, i);
        } else {
            range->address += size_bytes;
            range->size_bytes -= size_bytes;
        }
        return address;
    }
    return SPU_RAM_NO_ADDRESS;
}

int spu_ram_allocate_at(spu_ram_t *ram, uint32_t address, uint32_t size_bytes)
{
    uint32_t end;

    if (size_bytes == 0 || (address % SPU_RAM_GRANULARITY_BYTES) != 0)
        return -1;
    size_bytes = round_up_to_granularity(size_bytes);
    end = address + size_bytes;
    if (end < address)
        return -1;

    for (uint32_t i = 0; i < ram->free_range_count; i++) {
        spu_ram_range_t *range = &ram->free_ranges[i];
        uint32_t range_end = range->address + range->size_bytes;

        if (address < range->address || end > range_end)
            continue;
        if (address == range->address && end == range_end) {
            remove_range(ram, i);
        } else if (address == range->address) {
            range->address = end;
            range->size_bytes -= size_bytes;
        } else if (end == range_end) {
            range->size_bytes -= size_bytes;
        } else {
            if (ram->free_range_count >= SPU_RAM_MAX_FREE_RANGES)
                return -1;
            insert_range(ram, i + 1, end, range_end - end);
            range->size_bytes = address - range->address;
        }
        return 0;
    }
    return -1;
}

int spu_ram_release(spu_ram_t *ram, uint32_t address, uint32_t size_bytes)
{
    uint32_t index = 0;
    uint32_t end;
    int merges_previous = 0;
    int merges_next = 0;

    if (size_bytes == 0 || (address % SPU_RAM_GRANULARITY_BYTES) != 0)
        return -1;

    size_bytes = round_up_to_granularity(size_bytes);
    end = address + size_bytes;
    if (address < ram->area_start || end > ram->area_end || end < address)
        return -1;

    while (index < ram->free_range_count && ram->free_ranges[index].address < address)
        index++;

    if (index > 0) {
        const spu_ram_range_t *previous = &ram->free_ranges[index - 1];

        if (previous->address + previous->size_bytes > address)
            return -1;
        merges_previous = (previous->address + previous->size_bytes == address);
    }
    if (index < ram->free_range_count) {
        const spu_ram_range_t *next = &ram->free_ranges[index];

        if (next->address < end)
            return -1;
        merges_next = (next->address == end);
    }

    if (merges_previous && merges_next) {
        ram->free_ranges[index - 1].size_bytes += size_bytes + ram->free_ranges[index].size_bytes;
        remove_range(ram, index);
    } else if (merges_previous) {
        ram->free_ranges[index - 1].size_bytes += size_bytes;
    } else if (merges_next) {
        ram->free_ranges[index].address = address;
        ram->free_ranges[index].size_bytes += size_bytes;
    } else {
        if (ram->free_range_count >= SPU_RAM_MAX_FREE_RANGES)
            return -1;
        insert_range(ram, index, address, size_bytes);
    }
    return 0;
}

uint32_t spu_ram_get_free_bytes(const spu_ram_t *ram)
{
    uint32_t total = 0;

    for (uint32_t i = 0; i < ram->free_range_count; i++)
        total += ram->free_ranges[i].size_bytes;
    return total;
}

uint32_t spu_ram_get_largest_free_block(const spu_ram_t *ram)
{
    uint32_t largest = 0;

    for (uint32_t i = 0; i < ram->free_range_count; i++)
        if (ram->free_ranges[i].size_bytes > largest)
            largest = ram->free_ranges[i].size_bytes;
    return largest;
}
