#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_ram.h"

#define AREA_START SPU_RAM_CLIP_AREA_START
#define AREA_END   SPU_RAM_CLIP_AREA_END
#define AREA_BYTES (AREA_END - AREA_START)

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void test_fresh_area_is_one_free_range(void)
{
    spu_ram_t ram;

    spu_ram_init(&ram, AREA_START, AREA_END);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES);
    CHECK(spu_ram_get_largest_free_block(&ram) == AREA_BYTES);
    CHECK(AREA_BYTES >= 1536u * 1024u);
}

static void test_allocations_are_first_fit_and_rounded(void)
{
    spu_ram_t ram;
    uint32_t a, b;

    spu_ram_init(&ram, AREA_START, AREA_END);
    a = spu_ram_allocate(&ram, 1);
    b = spu_ram_allocate(&ram, 17);
    CHECK(a == AREA_START);
    CHECK(b == AREA_START + 16);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES - 16 - 32);
}

static void test_zero_and_oversize_fail(void)
{
    spu_ram_t ram;

    spu_ram_init(&ram, AREA_START, AREA_END);
    CHECK(spu_ram_allocate(&ram, 0) == SPU_RAM_NO_ADDRESS);
    CHECK(spu_ram_allocate(&ram, AREA_BYTES + 16) == SPU_RAM_NO_ADDRESS);
}

static void test_exhaustion_then_release_makes_room(void)
{
    spu_ram_t ram;
    uint32_t whole;

    spu_ram_init(&ram, AREA_START, AREA_END);
    whole = spu_ram_allocate(&ram, AREA_BYTES);
    CHECK(whole == AREA_START);
    CHECK(spu_ram_allocate(&ram, 16) == SPU_RAM_NO_ADDRESS);
    CHECK(spu_ram_get_free_bytes(&ram) == 0);
    CHECK(spu_ram_release(&ram, whole, AREA_BYTES) == 0);
    CHECK(spu_ram_get_largest_free_block(&ram) == AREA_BYTES);
}

static void test_release_coalesces_in_every_order(void)
{
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

    for (int o = 0; o < 6; o++) {
        spu_ram_t ram;
        uint32_t block[3];

        spu_ram_init(&ram, AREA_START, AREA_END);
        for (int i = 0; i < 3; i++)
            block[i] = spu_ram_allocate(&ram, 4096);
        for (int i = 0; i < 3; i++)
            CHECK(spu_ram_release(&ram, block[orders[o][i]], 4096) == 0);
        CHECK(ram.free_range_count == 1);
        CHECK(spu_ram_get_largest_free_block(&ram) == AREA_BYTES);
    }
}

static void test_freed_hole_is_reused_first_fit(void)
{
    spu_ram_t ram;
    uint32_t a, b;

    spu_ram_init(&ram, AREA_START, AREA_END);
    a = spu_ram_allocate(&ram, 1024);
    b = spu_ram_allocate(&ram, 1024);
    spu_ram_allocate(&ram, 1024);
    CHECK(spu_ram_release(&ram, a, 1024) == 0);
    CHECK(spu_ram_allocate(&ram, 1024) == a);
    CHECK(spu_ram_release(&ram, b, 1024) == 0);
    CHECK(spu_ram_allocate(&ram, 2048) != b);
}

static void test_invalid_release_is_rejected(void)
{
    spu_ram_t ram;
    uint32_t a;

    spu_ram_init(&ram, AREA_START, AREA_END);
    a = spu_ram_allocate(&ram, 256);
    CHECK(spu_ram_release(&ram, a + 8, 16) == -1);
    CHECK(spu_ram_release(&ram, AREA_START - 16, 16) == -1);
    CHECK(spu_ram_release(&ram, AREA_END, 16) == -1);
    CHECK(spu_ram_release(&ram, a, 0) == -1);
    CHECK(spu_ram_release(&ram, a, 256) == 0);
    CHECK(spu_ram_release(&ram, a, 256) == -1);
    CHECK(spu_ram_release(&ram, a + 16, 16) == -1);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES);
}

static void test_free_list_capacity_is_enforced(void)
{
    enum { COUNT = SPU_RAM_MAX_FREE_RANGES * 2 + 4 };
    static uint32_t block[COUNT];
    spu_ram_t ram;
    int released = 0;
    int refused = 0;

    spu_ram_init(&ram, AREA_START, AREA_END);
    for (int i = 0; i < COUNT; i++)
        block[i] = spu_ram_allocate(&ram, 16);
    for (int i = 0; i < COUNT; i += 2) {
        if (spu_ram_release(&ram, block[i], 16) == 0)
            released++;
        else
            refused++;
    }
    CHECK(released == SPU_RAM_MAX_FREE_RANGES - 1);
    CHECK(refused > 0);
    CHECK(ram.free_range_count <= SPU_RAM_MAX_FREE_RANGES);
}

static void test_reset_restores_everything(void)
{
    spu_ram_t ram;

    spu_ram_init(&ram, AREA_START, AREA_END);
    for (int i = 0; i < 10; i++)
        spu_ram_allocate(&ram, 1000);
    spu_ram_reset(&ram);
    CHECK(ram.free_range_count == 1);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES);
}

static uint32_t rng_state = 12345u;

static uint32_t next_random(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void test_random_stress_against_shadow_bitmap(void)
{
    enum { MAX_LIVE = 200, UNITS = AREA_BYTES / 16 };
    static uint8_t used[UNITS];
    struct
    {
        uint32_t address, size;
    } live[MAX_LIVE];
    int live_count = 0;
    spu_ram_t ram;

    spu_ram_init(&ram, AREA_START, AREA_END);
    memset(used, 0, sizeof(used));

    for (int step = 0; step < 20000; step++) {
        int do_allocate = live_count == 0 || (live_count < MAX_LIVE && (next_random() & 1));

        if (do_allocate) {
            uint32_t size = 1 + next_random() % 40000;
            uint32_t address = spu_ram_allocate(&ram, size);

            if (address != SPU_RAM_NO_ADDRESS) {
                uint32_t rounded = (size + 15u) & ~15u;

                CHECK(address % 16 == 0 && address >= AREA_START && address + rounded <= AREA_END);
                for (uint32_t u = (address - AREA_START) / 16; u < (address - AREA_START + rounded) / 16; u++) {
                    if (used[u]) {
                        printf("FAIL: allocated memory overlaps a live block\n");
                        failures++;
                        return;
                    }
                    used[u] = 1;
                }
                live[live_count].address = address;
                live[live_count].size = size;
                live_count++;
            }
        } else {
            int pick = (int)(next_random() % (uint32_t)live_count);
            uint32_t rounded = (live[pick].size + 15u) & ~15u;

            CHECK(spu_ram_release(&ram, live[pick].address, live[pick].size) == 0);
            for (uint32_t u = (live[pick].address - AREA_START) / 16; u < (live[pick].address - AREA_START + rounded) / 16; u++)
                used[u] = 0;
            live[pick] = live[live_count - 1];
            live_count--;
        }

        if (step % 500 == 0) {
            uint32_t used_bytes = 0;

            for (uint32_t u = 0; u < UNITS; u++)
                used_bytes += used[u] * 16u;
            CHECK(used_bytes + spu_ram_get_free_bytes(&ram) == AREA_BYTES);
        }
    }

    while (live_count > 0) {
        live_count--;
        CHECK(spu_ram_release(&ram, live[live_count].address, live[live_count].size) == 0);
    }
    CHECK(ram.free_range_count == 1);
    CHECK(spu_ram_get_largest_free_block(&ram) == AREA_BYTES);
}

static void test_allocate_at_takes_an_exact_range(void)
{
    spu_ram_t ram;
    uint32_t middle = AREA_START + 0x1000u;

    spu_ram_init(&ram, AREA_START, AREA_END);
    CHECK(spu_ram_allocate_at(&ram, AREA_END - 0x100u, 0x100u) == 0);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES - 0x100u);
    CHECK(ram.free_range_count == 1);

    CHECK(spu_ram_allocate_at(&ram, AREA_START, 0x40u) == 0);
    CHECK(spu_ram_allocate(&ram, 16) == AREA_START + 0x40u);

    CHECK(spu_ram_allocate_at(&ram, middle, 0x200u) == 0);
    CHECK(ram.free_range_count == 2);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES - 0x100u - 0x40u - 16u - 0x200u);

    CHECK(spu_ram_release(&ram, middle, 0x200u) == 0);
    CHECK(spu_ram_release(&ram, AREA_END - 0x100u, 0x100u) == 0);
    CHECK(spu_ram_release(&ram, AREA_START, 0x40u) == 0);
    CHECK(spu_ram_release(&ram, AREA_START + 0x40u, 16u) == 0);
    CHECK(ram.free_range_count == 1);
    CHECK(spu_ram_get_largest_free_block(&ram) == AREA_BYTES);
}

static void test_allocate_at_rejects_taken_or_invalid_ranges(void)
{
    spu_ram_t ram;
    uint32_t a;

    spu_ram_init(&ram, AREA_START, AREA_END);
    a = spu_ram_allocate(&ram, 0x100u);
    CHECK(spu_ram_allocate_at(&ram, a + 0x80u, 0x40u) == -1);
    CHECK(spu_ram_allocate_at(&ram, a - 0x10u, 0x40u) == -1);
    CHECK(spu_ram_allocate_at(&ram, a + 0xF0u, 0x20u) == -1);
    CHECK(spu_ram_allocate_at(&ram, AREA_END - 0x10u, 0x20u) == -1);
    CHECK(spu_ram_allocate_at(&ram, AREA_START + 0x1001u, 0x20u) == -1);
    CHECK(spu_ram_allocate_at(&ram, AREA_START + 0x1000u, 0) == -1);
    CHECK(spu_ram_allocate_at(&ram, 0xFFFFFFF0u, 0x20u) == -1);
    CHECK(spu_ram_get_free_bytes(&ram) == AREA_BYTES - 0x100u);
}

int main(void)
{
    test_allocate_at_takes_an_exact_range();
    test_allocate_at_rejects_taken_or_invalid_ranges();
    test_fresh_area_is_one_free_range();
    test_allocations_are_first_fit_and_rounded();
    test_zero_and_oversize_fail();
    test_exhaustion_then_release_makes_room();
    test_release_coalesces_in_every_order();
    test_freed_hole_is_reused_first_fit();
    test_invalid_release_is_rejected();
    test_free_list_capacity_is_enforced();
    test_reset_restores_everything();
    test_random_stress_against_shadow_bitmap();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("spu_ram: all tests passed\n");
    return 0;
}
