#include <stdio.h>
#include <string.h>

#include "audio_effects.h"

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

typedef struct mock_core
{
    int mode;
    int16_t depth_left;
    int16_t depth_right;
    int delay;
    int feedback;
    uint32_t voice_sends;
    int input_send;
    int mode_calls;
    int last_old_mode;
} mock_core_t;

typedef struct mock_hw
{
    mock_core_t cores[AUDIO_EFFECT_CORE_COUNT];
} mock_hw_t;

static int mock_set_mode(void *context, int core, int old_mode, int new_mode, int16_t left, int16_t right, int delay, int feedback)
{
    mock_core_t *c = &((mock_hw_t *)context)->cores[core];

    c->mode = new_mode;
    c->depth_left = left;
    c->depth_right = right;
    c->delay = delay;
    c->feedback = feedback;
    c->mode_calls++;
    c->last_old_mode = old_mode;
    return 0;
}

static void mock_set_depth(void *context, int core, int16_t left, int16_t right)
{
    mock_core_t *c = &((mock_hw_t *)context)->cores[core];

    c->depth_left = left;
    c->depth_right = right;
}

static void mock_set_voice_sends(void *context, int core, uint32_t mask)
{
    ((mock_hw_t *)context)->cores[core].voice_sends = mask;
}

static void mock_set_input_send(void *context, int core, int enabled)
{
    ((mock_hw_t *)context)->cores[core].input_send = enabled;
}

typedef struct fixture
{
    mock_hw_t hw;
    audio_effect_hw_ops_t ops;
    spu_ram_t ram;
    audio_effects_t fx;
} fixture_t;

static void fixture_init(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->ops.set_mode = mock_set_mode;
    f->ops.set_depth = mock_set_depth;
    f->ops.set_voice_sends = mock_set_voice_sends;
    f->ops.set_input_send = mock_set_input_send;
    f->ops.context = &f->hw;
    spu_ram_init(&f->ram, SPU_RAM_CLIP_AREA_START, SPU_RAM_CLIP_AREA_END);
    audio_effects_init(&f->fx, &f->ops, &f->ram);
}

static void test_work_area_sizes_fit_the_reserves(void)
{
    uint32_t largest = 0;

    CHECK(audio_effects_get_work_area_bytes(0) == 0);
    CHECK(audio_effects_get_work_area_bytes(-1) == 0);
    CHECK(audio_effects_get_work_area_bytes(AUDIO_EFFECT_PRESET_COUNT) == 0);
    CHECK(audio_effects_get_work_area_bytes(1) == 0x26Cu * 16u);
    for (int preset = 1; preset < AUDIO_EFFECT_PRESET_COUNT; preset++)
        if (audio_effects_get_work_area_bytes(preset) > largest)
            largest = audio_effects_get_work_area_bytes(preset);
    CHECK(largest == 98368u);
    CHECK(largest < 0x20000u);
}

static void test_depth_conversion(void)
{
    CHECK(audio_effects_compute_hw_depth(0) == 0);
    CHECK(audio_effects_compute_hw_depth(100) == 0x7FFF);
    CHECK(audio_effects_compute_hw_depth(50) == 0x3FFF);
}

static void test_preset_applies_to_both_cores_and_keeps_depth(void)
{
    fixture_t f;

    fixture_init(&f);
    CHECK(audio_effects_set_depth(&f.fx, 80) == 0);
    CHECK(f.hw.cores[0].mode_calls == 0);
    CHECK(audio_effects_set_preset(&f.fx, 5) == 0);
    for (int core = 0; core < 2; core++) {
        CHECK(f.hw.cores[core].mode == 5);
        CHECK(f.hw.cores[core].last_old_mode == 0);
        CHECK(f.hw.cores[core].depth_left == audio_effects_compute_hw_depth(80));
        CHECK(f.hw.cores[core].depth_right == audio_effects_compute_hw_depth(80));
    }
    CHECK(audio_effects_set_preset(&f.fx, 2) == 0);
    CHECK(f.hw.cores[1].last_old_mode == 5);
    CHECK(f.hw.cores[1].mode == 2);
}

static void test_depth_change_while_on_reaches_hardware(void)
{
    fixture_t f;

    fixture_init(&f);
    audio_effects_set_preset(&f.fx, 1);
    CHECK(audio_effects_set_depth(&f.fx, 20) == 0);
    CHECK(f.hw.cores[0].depth_left == audio_effects_compute_hw_depth(20));
    CHECK(f.hw.cores[1].depth_right == audio_effects_compute_hw_depth(20));
}

static void test_invalid_arguments_are_rejected(void)
{
    fixture_t f;

    fixture_init(&f);
    CHECK(audio_effects_set_preset(&f.fx, -1) < 0);
    CHECK(audio_effects_set_preset(&f.fx, AUDIO_EFFECT_PRESET_COUNT) < 0);
    CHECK(audio_effects_set_depth(&f.fx, -1) < 0);
    CHECK(audio_effects_set_depth(&f.fx, 101) < 0);
    CHECK(audio_effects_set_delay_and_feedback(&f.fx, -1, 0) < 0);
    CHECK(audio_effects_set_delay_and_feedback(&f.fx, 256, 0) < 0);
    CHECK(audio_effects_set_delay_and_feedback(&f.fx, 0, 256) < 0);
    CHECK(audio_effects_set_channel_send(&f.fx, -1, 1) < 0);
    CHECK(audio_effects_set_channel_send(&f.fx, AUDIO_CHANNEL_COUNT, 1) < 0);
    CHECK(f.fx.preset == 0);
    CHECK(f.hw.cores[0].mode_calls == 0);
}

static void test_delay_and_feedback_reapply_only_for_echo_and_delay(void)
{
    fixture_t f;

    fixture_init(&f);
    audio_effects_set_preset(&f.fx, 5);
    f.hw.cores[0].mode_calls = 0;
    CHECK(audio_effects_set_delay_and_feedback(&f.fx, 10, 20) == 0);
    CHECK(f.hw.cores[0].mode_calls == 0);

    audio_effects_set_preset(&f.fx, AUDIO_EFFECT_PRESET_ECHO);
    f.hw.cores[0].mode_calls = 0;
    CHECK(audio_effects_set_delay_and_feedback(&f.fx, 30, 40) == 0);
    CHECK(f.hw.cores[0].mode_calls == 1);
    CHECK(f.hw.cores[0].last_old_mode == AUDIO_EFFECT_PRESET_ECHO);
    CHECK(f.hw.cores[0].mode == AUDIO_EFFECT_PRESET_ECHO);
    CHECK(f.hw.cores[1].delay == 30);
    CHECK(f.hw.cores[1].feedback == audio_effects_compute_hw_feedback(AUDIO_EFFECT_PRESET_ECHO, 40));
    CHECK(f.hw.cores[1].feedback == 40 * AUDIO_EFFECT_HW_FEEDBACK_MAX / 255);
}

static void test_feedback_is_scaled_into_the_stable_range_and_only_echo_repeats(void)
{
    fixture_t f;

    CHECK(audio_effects_compute_hw_feedback(AUDIO_EFFECT_PRESET_ECHO, 0) == 0);
    CHECK(audio_effects_compute_hw_feedback(AUDIO_EFFECT_PRESET_ECHO, 255) == AUDIO_EFFECT_HW_FEEDBACK_MAX);
    CHECK(audio_effects_compute_hw_feedback(AUDIO_EFFECT_PRESET_ECHO, 128) < 127);
    CHECK(audio_effects_compute_hw_feedback(AUDIO_EFFECT_PRESET_DELAY, 255) == 0);
    CHECK(audio_effects_compute_hw_feedback(5, 255) == 0);
    /* libsd's gain is value * 258 in 16 bits; it must stay below the sign bit. */
    CHECK(AUDIO_EFFECT_HW_FEEDBACK_MAX * 258 < 32768);

    fixture_init(&f);
    audio_effects_set_preset(&f.fx, AUDIO_EFFECT_PRESET_DELAY);
    CHECK(f.hw.cores[0].feedback == 0);
}

static void test_channel_sends_set_voice_bits_on_both_cores(void)
{
    fixture_t f;

    fixture_init(&f);
    CHECK(audio_effects_set_channel_send(&f.fx, 3, 1) == 0);
    CHECK(audio_effects_set_channel_send(&f.fx, 23, 1) == 0);
    CHECK(f.hw.cores[0].voice_sends == ((1u << 3) | (1u << 23)));
    CHECK(f.hw.cores[1].voice_sends == ((1u << 3) | (1u << 23)));
    CHECK(audio_effects_set_channel_send(&f.fx, 3, 0) == 0);
    CHECK(f.hw.cores[1].voice_sends == (1u << 23));
}

static void test_stream_send_goes_to_core_zero(void)
{
    fixture_t f;

    fixture_init(&f);
    CHECK(audio_effects_set_stream_send(&f.fx, 1) == 0);
    CHECK(f.hw.cores[0].input_send == 1);
    CHECK(audio_effects_set_stream_send(&f.fx, 0) == 0);
    CHECK(f.hw.cores[0].input_send == 0);
}

static void test_core0_work_area_is_taken_from_and_returned_to_the_clip_memory(void)
{
    fixture_t f;
    uint32_t free_before;
    uint32_t bytes;

    fixture_init(&f);
    free_before = spu_ram_get_free_bytes(&f.ram);
    bytes = audio_effects_get_work_area_bytes(7);

    CHECK(audio_effects_set_preset(&f.fx, 7) == 0);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before - bytes);
    CHECK(audio_effects_set_preset(&f.fx, 2) == 0);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before - audio_effects_get_work_area_bytes(2));
    CHECK(audio_effects_set_preset(&f.fx, 0) == 0);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before);
    CHECK(spu_ram_get_largest_free_block(&f.ram) == free_before);
}

static void test_preset_fails_when_a_sound_sits_on_the_work_area(void)
{
    fixture_t f;
    uint32_t area_start = AUDIO_EFFECT_CORE0_AREA_END - audio_effects_get_work_area_bytes(5);
    uint32_t free_before;

    fixture_init(&f);
    CHECK(spu_ram_allocate_at(&f.ram, area_start + 0x100u, 0x40u) == 0);
    free_before = spu_ram_get_free_bytes(&f.ram);

    CHECK(audio_effects_set_preset(&f.fx, 5) == AUDIO_EFFECTS_ERR_OUT_OF_SPU_MEMORY);
    CHECK(f.fx.preset == 0);
    CHECK(f.hw.cores[0].mode_calls == 0);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before);

    CHECK(audio_effects_set_preset(&f.fx, 1) == 0);
    CHECK(audio_effects_set_preset(&f.fx, 5) == AUDIO_EFFECTS_ERR_OUT_OF_SPU_MEMORY);
    CHECK(f.fx.preset == 1);
    CHECK(f.fx.core0_area_held == 1);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before - audio_effects_get_work_area_bytes(1));
}

static void test_reset_turns_everything_off_and_frees_the_area(void)
{
    fixture_t f;
    uint32_t free_before;

    fixture_init(&f);
    free_before = spu_ram_get_free_bytes(&f.ram);
    audio_effects_set_preset(&f.fx, 6);
    audio_effects_set_depth(&f.fx, 90);
    audio_effects_set_channel_send(&f.fx, 4, 1);
    audio_effects_set_stream_send(&f.fx, 1);

    audio_effects_reset(&f.fx);
    CHECK(f.fx.preset == 0);
    CHECK(f.fx.depth_percent == AUDIO_EFFECT_DEFAULT_DEPTH);
    CHECK(f.hw.cores[0].mode == 0);
    CHECK(f.hw.cores[1].last_old_mode == 6);
    CHECK(f.hw.cores[1].voice_sends == 0);
    CHECK(f.hw.cores[0].input_send == 0);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before);
}

static void test_reserve_after_ram_reset_takes_the_area_again(void)
{
    fixture_t f;
    uint32_t free_before;

    fixture_init(&f);
    free_before = spu_ram_get_free_bytes(&f.ram);
    audio_effects_set_preset(&f.fx, 3);
    spu_ram_reset(&f.ram);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before);
    audio_effects_reserve_after_ram_reset(&f.fx);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before - audio_effects_get_work_area_bytes(3));
    CHECK(f.fx.core0_area_held == 1);
    audio_effects_reset(&f.fx);
    CHECK(spu_ram_get_free_bytes(&f.ram) == free_before);
}

int main(void)
{
    test_work_area_sizes_fit_the_reserves();
    test_depth_conversion();
    test_preset_applies_to_both_cores_and_keeps_depth();
    test_depth_change_while_on_reaches_hardware();
    test_invalid_arguments_are_rejected();
    test_delay_and_feedback_reapply_only_for_echo_and_delay();
    test_feedback_is_scaled_into_the_stable_range_and_only_echo_repeats();
    test_channel_sends_set_voice_bits_on_both_cores();
    test_stream_send_goes_to_core_zero();
    test_core0_work_area_is_taken_from_and_returned_to_the_clip_memory();
    test_preset_fails_when_a_sound_sits_on_the_work_area();
    test_reset_turns_everything_off_and_frees_the_area();
    test_reserve_after_ram_reset_takes_the_area_again();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio_effects: all tests passed\n");
    return 0;
}
