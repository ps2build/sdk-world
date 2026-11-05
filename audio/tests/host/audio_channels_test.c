#include <stdio.h>
#include <string.h>

#include "audio_channels.h"

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

typedef struct mock_voice
{
    int active;
    uint32_t spu_address;
    uint16_t pitch;
    int16_t volume_left;
    int16_t volume_right;
    int start_count;
} mock_voice_t;

typedef struct mock_hw
{
    mock_voice_t voices[2][AUDIO_CHANNEL_COUNT];
    uint32_t ended[2];
} mock_hw_t;

static void mock_start(void *context, int core, int voice, uint32_t address, uint16_t pitch, int16_t left, int16_t right)
{
    mock_voice_t *v = &((mock_hw_t *)context)->voices[core][voice];

    v->active = 1;
    v->spu_address = address;
    v->pitch = pitch;
    v->volume_left = left;
    v->volume_right = right;
    v->start_count++;
    ((mock_hw_t *)context)->ended[core] &= ~(1u << voice);
}

static void mock_stop(void *context, int core, int voice)
{
    ((mock_hw_t *)context)->voices[core][voice].active = 0;
}

static void mock_set_volume(void *context, int core, int voice, int16_t left, int16_t right)
{
    mock_voice_t *v = &((mock_hw_t *)context)->voices[core][voice];

    v->volume_left = left;
    v->volume_right = right;
}

static void mock_set_pitch(void *context, int core, int voice, uint16_t pitch)
{
    ((mock_hw_t *)context)->voices[core][voice].pitch = pitch;
}

static uint32_t mock_get_ended(void *context, int core)
{
    return ((mock_hw_t *)context)->ended[core];
}

typedef struct fixture
{
    mock_hw_t hw;
    audio_channel_hw_ops_t ops;
    audio_sound_entry_t storage[32];
    audio_sound_table_t sounds;
    audio_channels_t channels;
} fixture_t;

static void fixture_init(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->ops.start_voice = mock_start;
    f->ops.stop_voice = mock_stop;
    f->ops.set_voice_volume = mock_set_volume;
    f->ops.set_voice_pitch = mock_set_pitch;
    f->ops.get_ended_voices = mock_get_ended;
    f->ops.context = &f->hw;
    audio_sound_table_init(&f->sounds, f->storage, 32);
    audio_channels_init(&f->channels, &f->ops, &f->sounds);
}

static int32_t add_mono(fixture_t *f, uint32_t address, uint32_t rate, int loops)
{
    return audio_sound_table_add(&f->sounds, address, 4096, rate, 1, loops);
}

static void test_handles_are_unique_and_stale_handles_fail(void)
{
    audio_sound_entry_t storage[4];
    audio_sound_table_t table;
    int32_t a, b, c;

    audio_sound_table_init(&table, storage, 4);
    a = audio_sound_table_add(&table, 0x5010, 64, 44100, 1, 0);
    CHECK(a >= 1);
    CHECK(audio_sound_table_get(&table, a) != 0);
    CHECK(audio_sound_table_remove(&table, a) == 0);
    CHECK(audio_sound_table_get(&table, a) == 0);
    CHECK(audio_sound_table_remove(&table, a) == -1);

    b = audio_sound_table_add(&table, 0x6000, 64, 44100, 1, 0);
    CHECK(b != a);
    CHECK(audio_sound_table_get(&table, a) == 0);
    CHECK(audio_sound_table_get(&table, b) != 0);

    c = audio_sound_table_add(&table, 0x7000, 64, 44100, 1, 0);
    CHECK(c > 0 && c != b);
    CHECK(audio_sound_table_get(&table, 0) == 0);
    CHECK(audio_sound_table_get(&table, -5) == 0);
    CHECK(audio_sound_table_get(&table, 0x7FFFFFFF) == 0);
}

static void test_table_full_and_clear(void)
{
    audio_sound_entry_t storage[3];
    audio_sound_table_t table;
    int32_t handles[3];

    audio_sound_table_init(&table, storage, 3);
    for (int i = 0; i < 3; i++) {
        handles[i] = audio_sound_table_add(&table, 0x5010u + 64u * (uint32_t)i, 64, 22050, 1, 0);
        CHECK(handles[i] >= 1);
    }
    CHECK(audio_sound_table_add(&table, 0x9000, 64, 22050, 1, 0) == -1);
    audio_sound_table_clear(&table);
    CHECK(table.live_count == 0);
    for (int i = 0; i < 3; i++)
        CHECK(audio_sound_table_get(&table, handles[i]) == 0);
    CHECK(audio_sound_table_add(&table, 0x9000, 64, 22050, 1, 0) >= 1);
}

static void test_volume_and_pan_math(void)
{
    CHECK(audio_channels_compute_hw_volume(100, 100) == AUDIO_HW_VOLUME_MAX);
    CHECK(audio_channels_compute_hw_volume(0, 100) == 0);
    CHECK(audio_channels_compute_hw_volume(100, 0) == 0);
    CHECK(audio_channels_compute_hw_volume(50, 100) == AUDIO_HW_VOLUME_MAX / 2);
    CHECK(audio_channels_compute_hw_volume(50, 50) == AUDIO_HW_VOLUME_MAX / 4);
}

static void test_pitch_math(void)
{
    CHECK(audio_channels_compute_hw_pitch(48000, AUDIO_PITCH_Q12_UNITY) == AUDIO_HW_PITCH_UNITY);
    CHECK(audio_channels_compute_hw_pitch(44100, AUDIO_PITCH_Q12_UNITY) == 44100u * 4096u / 48000u);
    CHECK(audio_channels_compute_hw_pitch(24000, AUDIO_PITCH_Q12_UNITY * 2) == AUDIO_HW_PITCH_UNITY);
    CHECK(audio_channels_compute_hw_pitch(48000, AUDIO_PITCH_Q12_MAX) == AUDIO_HW_PITCH_MAX);
    CHECK(audio_channels_compute_hw_pitch(4000, AUDIO_PITCH_Q12_MAX) == 1365);
    CHECK(audio_channels_compute_hw_pitch(1, 1) == 1);
}

static void test_play_uses_channel_settings_and_keeps_them(void)
{
    fixture_t f;
    int32_t sound;

    fixture_init(&f);
    sound = add_mono(&f, 0x5010, 48000, 0);

    CHECK(audio_channels_set_volume_and_pan(&f.channels, 3, 50, 100) == 0);
    CHECK(audio_channels_set_pitch(&f.channels, 3, AUDIO_PITCH_Q12_UNITY * 2) == 0);
    CHECK(audio_channels_play_sound(&f.channels, 3, sound) == 0);
    CHECK(f.hw.voices[1][3].active);
    CHECK(f.hw.voices[1][3].volume_left == 0);
    CHECK(f.hw.voices[1][3].volume_right == AUDIO_HW_VOLUME_MAX / 2);
    CHECK(f.hw.voices[1][3].pitch == AUDIO_HW_PITCH_UNITY * 2);

    CHECK(audio_channels_play_sound(&f.channels, 3, sound) == 0);
    CHECK(f.hw.voices[1][3].start_count == 2);
    CHECK(f.hw.voices[1][3].volume_right == AUDIO_HW_VOLUME_MAX / 2);
    CHECK(f.channels.channels[3].volume_percent == 50);
    CHECK(f.channels.channels[3].pan == 100);
}

static void test_volume_set_before_first_play_and_after_end_persists(void)
{
    fixture_t f;
    int32_t sound;

    fixture_init(&f);
    sound = add_mono(&f, 0x5010, 48000, 0);
    CHECK(audio_channels_set_volume(&f.channels, 5, 20) == 0);
    CHECK(audio_channels_play_sound(&f.channels, 5, sound) == 0);
    CHECK(f.hw.voices[1][5].volume_left == audio_channels_compute_hw_volume(20, 100));

    f.hw.ended[1] |= 1u << 5;
    CHECK(audio_channels_is_playing(&f.channels, 5) == 0);
    CHECK(audio_channels_play_sound(&f.channels, 5, sound) == 0);
    CHECK(f.hw.voices[1][5].volume_left == audio_channels_compute_hw_volume(20, 100));
}

static void test_play_on_free_channel_resets_settings_and_takes_lowest_idle(void)
{
    fixture_t f;
    int32_t sound;

    fixture_init(&f);
    sound = add_mono(&f, 0x5010, 48000, 1);
    audio_channels_set_volume_and_pan(&f.channels, 0, 10, -100);
    audio_channels_set_pitch(&f.channels, 0, AUDIO_PITCH_Q12_MAX);

    CHECK(audio_channels_play_sound_on_free_channel(&f.channels, sound) == 0);
    CHECK(f.channels.channels[0].volume_percent == 100);
    CHECK(f.channels.channels[0].pan == 0);
    CHECK(f.channels.channels[0].pitch_q12 == AUDIO_PITCH_Q12_UNITY);
    CHECK(f.hw.voices[1][0].volume_left == AUDIO_HW_VOLUME_MAX);
    CHECK(f.hw.voices[1][0].volume_right == AUDIO_HW_VOLUME_MAX);

    CHECK(audio_channels_play_sound_on_free_channel(&f.channels, sound) == 1);
}

static void test_play_on_free_channel_skips_busy_and_reports_none_free(void)
{
    fixture_t f;
    int32_t looping;

    fixture_init(&f);
    looping = add_mono(&f, 0x5010, 48000, 1);
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++)
        CHECK(audio_channels_play_sound_on_free_channel(&f.channels, looping) == i);
    CHECK(audio_channels_play_sound_on_free_channel(&f.channels, looping) == AUDIO_CHANNELS_ERR_NO_FREE_CHANNEL);
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++)
        CHECK(f.hw.voices[1][i].start_count == 1);

    CHECK(audio_channels_stop(&f.channels, 7) == 0);
    CHECK(audio_channels_play_sound_on_free_channel(&f.channels, looping) == 7);
}

static void test_finished_one_shot_frees_channel_but_looping_sound_does_not(void)
{
    fixture_t f;
    int32_t one_shot, looping;

    fixture_init(&f);
    one_shot = add_mono(&f, 0x5010, 48000, 0);
    looping = add_mono(&f, 0x6010, 48000, 1);
    audio_channels_play_sound(&f.channels, 0, one_shot);
    audio_channels_play_sound(&f.channels, 1, looping);

    CHECK(audio_channels_is_playing(&f.channels, 0) == 1);
    f.hw.ended[1] = 0x3;
    CHECK(audio_channels_is_playing(&f.channels, 0) == 0);
    CHECK(audio_channels_is_playing(&f.channels, 1) == 1);
    CHECK(audio_channels_play_sound_on_free_channel(&f.channels, one_shot) == 0);
}

static void test_replacing_a_playing_channel_stops_the_old_voice_first(void)
{
    fixture_t f;
    int32_t a, b;

    fixture_init(&f);
    a = add_mono(&f, 0x5010, 48000, 1);
    b = add_mono(&f, 0x6010, 48000, 1);
    audio_channels_play_sound(&f.channels, 2, a);
    audio_channels_play_sound(&f.channels, 2, b);
    CHECK(f.hw.voices[1][2].spu_address == 0x6010);
    CHECK(f.hw.voices[1][2].start_count == 2);
}

static void test_stereo_sound_uses_both_cores_with_hard_panned_sides(void)
{
    fixture_t f;
    int32_t stereo;
    int32_t playing = 0;

    fixture_init(&f);
    stereo = audio_sound_table_add(&f.sounds, 0x10000, 8192, 44100, 2, 0);
    audio_channels_set_volume_and_pan(&f.channels, 4, 100, -50);
    CHECK(audio_channels_play_sound(&f.channels, 4, stereo) == 0);

    CHECK(f.hw.voices[1][4].spu_address == 0x10000);
    CHECK(f.hw.voices[1][4].volume_left == AUDIO_HW_VOLUME_MAX);
    CHECK(f.hw.voices[1][4].volume_right == 0);
    CHECK(f.hw.voices[0][4].spu_address == 0x10000 + 4096);
    CHECK(f.hw.voices[0][4].volume_left == 0);
    CHECK(f.hw.voices[0][4].volume_right == audio_channels_compute_hw_volume(100, 50));

    audio_channels_set_volume_and_pan(&f.channels, 4, 100, 100);
    CHECK(f.hw.voices[0][4].volume_right == AUDIO_HW_VOLUME_MAX);
    CHECK(f.hw.voices[1][4].volume_left == 0);

    CHECK(audio_channels_get_playing_sound(&f.channels, 4, &playing) == 0 && playing == stereo);
    CHECK(audio_channels_stop(&f.channels, 4) == 0);
    CHECK(!f.hw.voices[1][4].active && !f.hw.voices[0][4].active);
}

static void test_stop_sound_everywhere_and_get_playing_sound(void)
{
    fixture_t f;
    int32_t a, b, playing = -1;

    fixture_init(&f);
    a = add_mono(&f, 0x5010, 48000, 1);
    b = add_mono(&f, 0x6010, 48000, 1);
    audio_channels_play_sound(&f.channels, 0, a);
    audio_channels_play_sound(&f.channels, 1, a);
    audio_channels_play_sound(&f.channels, 2, b);

    audio_channels_stop_sound_everywhere(&f.channels, a);
    CHECK(!f.hw.voices[1][0].active && !f.hw.voices[1][1].active && f.hw.voices[1][2].active);
    CHECK(audio_channels_get_playing_sound(&f.channels, 0, &playing) == 0 && playing == 0);
    CHECK(audio_channels_get_playing_sound(&f.channels, 2, &playing) == 0 && playing == b);
}

static void test_invalid_arguments_are_rejected(void)
{
    fixture_t f;
    int32_t sound;

    fixture_init(&f);
    sound = add_mono(&f, 0x5010, 48000, 0);
    CHECK(audio_channels_play_sound(&f.channels, -1, sound) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_play_sound(&f.channels, AUDIO_CHANNEL_COUNT, sound) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_play_sound(&f.channels, 0, 0) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_play_sound(&f.channels, 0, sound + 1) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_play_sound_on_free_channel(&f.channels, 12345) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_set_volume(&f.channels, 0, 101) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_set_volume(&f.channels, 0, -1) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_set_volume_and_pan(&f.channels, 0, 50, 101) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_set_pitch(&f.channels, 0, AUDIO_PITCH_Q12_MAX + 1) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_set_pitch(&f.channels, 0, AUDIO_PITCH_Q12_MIN - 1) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_is_playing(&f.channels, 99) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
    CHECK(audio_channels_stop(&f.channels, 99) == AUDIO_CHANNELS_ERR_INVALID_ARGUMENT);
}

static void test_calls_on_idle_channel_are_safe(void)
{
    fixture_t f;

    fixture_init(&f);
    CHECK(audio_channels_stop(&f.channels, 0) == 0);
    CHECK(audio_channels_set_volume(&f.channels, 0, 40) == 0);
    CHECK(audio_channels_set_pitch(&f.channels, 0, AUDIO_PITCH_Q12_UNITY) == 0);
    CHECK(audio_channels_is_playing(&f.channels, 0) == 0);
    CHECK(f.hw.voices[1][0].start_count == 0);
}

int main(void)
{
    test_handles_are_unique_and_stale_handles_fail();
    test_table_full_and_clear();
    test_volume_and_pan_math();
    test_pitch_math();
    test_play_uses_channel_settings_and_keeps_them();
    test_volume_set_before_first_play_and_after_end_persists();
    test_play_on_free_channel_resets_settings_and_takes_lowest_idle();
    test_play_on_free_channel_skips_busy_and_reports_none_free();
    test_finished_one_shot_frees_channel_but_looping_sound_does_not();
    test_replacing_a_playing_channel_stops_the_old_voice_first();
    test_stereo_sound_uses_both_cores_with_hard_panned_sides();
    test_stop_sound_everywhere_and_get_playing_sound();
    test_invalid_arguments_are_rejected();
    test_calls_on_idle_channel_are_safe();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio_channels: all tests passed\n");
    return 0;
}
