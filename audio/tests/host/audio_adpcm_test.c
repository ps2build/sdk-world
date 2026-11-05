#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_adpcm.h"

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint8_t *read_file(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long length;

    if (!file)
        return 0;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = 0;
    }
    fclose(file);
    *size = (uint32_t)length;
    return data;
}

static void make_header(uint8_t *file, int channels, int loop)
{
    memset(file, 0, AUDIO_APCM_HEADER_BYTES);
    memcpy(file, "APCM", 4);
    file[4] = 1;
    file[5] = (uint8_t)channels;
    file[6] = (uint8_t)loop;
}

static void test_parse_rejects_malformed_files(void)
{
    uint8_t file[16 + 64];
    audio_adpcm_view_t view;

    memset(file, 0, sizeof(file));
    make_header(file, 1, 0);
    CHECK(audio_apcm_parse(file, sizeof(file), &view) == 0);
    CHECK(view.size_bytes == 64 && view.channel_count == 1 && view.loops == 0 && view.sample_rate_hz == 44100);

    CHECK(audio_apcm_parse(0, sizeof(file), &view) == -1);
    CHECK(audio_apcm_parse(file, sizeof(file), 0) == -1);
    CHECK(audio_apcm_parse(file, 31, &view) == -1);
    CHECK(audio_apcm_parse(file, sizeof(file) - 1, &view) == -1);

    file[0] = 'X';
    CHECK(audio_apcm_parse(file, sizeof(file), &view) == -1);
    make_header(file, 1, 0);
    file[4] = 2;
    CHECK(audio_apcm_parse(file, sizeof(file), &view) == -1);
    make_header(file, 3, 0);
    CHECK(audio_apcm_parse(file, sizeof(file), &view) == -1);
    make_header(file, 0, 0);
    CHECK(audio_apcm_parse(file, sizeof(file), &view) == -1);

    make_header(file, 2, 1);
    CHECK(audio_apcm_parse(file, sizeof(file), &view) == 0 && view.loops == 1 && view.channel_count == 2);
    CHECK(audio_apcm_parse(file, 16 + 48, &view) == -1);
}

static void test_patch_one_shot_ends_each_run_only(void)
{
    uint8_t clip[16 * 8];

    memset(clip, 0, sizeof(clip));
    for (int b = 0; b < 8; b++)
        clip[b * 16 + 1] = 0x07;

    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 2, 0, 0);
    for (int b = 0; b < 8; b++) {
        int run_last = b == 3 || b == 7;

        CHECK(clip[b * 16 + 1] == (run_last ? AUDIO_ADPCM_FLAG_LOOP_END : 0));
    }
}

static void test_patch_looping_marks_start_repeat_end_per_run(void)
{
    uint8_t clip[16 * 6];

    memset(clip, 0, sizeof(clip));
    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 2, 1, 0);
    CHECK(clip[0 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_START | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[1 * 16 + 1] == AUDIO_ADPCM_FLAG_LOOP_REPEAT);
    CHECK(clip[2 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_END | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[3 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_START | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[5 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_END | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
}

static void test_patch_loop_ends_before_pad_block(void)
{
    uint8_t clip[16 * 6];

    memset(clip, 0, sizeof(clip));
    for (int b = 0; b < 6; b++)
        clip[b * 16 + 1] = 0x07;
    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 2, 1, 1);
    CHECK(clip[0 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_START | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[1 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_END | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[2 * 16 + 1] == 0x07);
    CHECK(clip[3 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_START | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[4 * 16 + 1] == (AUDIO_ADPCM_FLAG_LOOP_END | AUDIO_ADPCM_FLAG_LOOP_REPEAT));
    CHECK(clip[5 * 16 + 1] == 0x07);

    memset(clip, 0, sizeof(clip));
    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 2, 0, 1);
    CHECK(clip[2 * 16 + 1] == AUDIO_ADPCM_FLAG_LOOP_END && clip[5 * 16 + 1] == AUDIO_ADPCM_FLAG_LOOP_END);
    CHECK(clip[1 * 16 + 1] == 0 && clip[4 * 16 + 1] == 0);

    memset(clip, 0, 32);
    audio_adpcm_patch_flags(clip, 32, 0, 32, 2, 1, 1);
    CHECK(clip[1] == 0x07 && clip[17] == 0x07);
}

static void test_patch_with_pad_in_chunks_equals_whole(void)
{
    enum { BLOCKS = 40, SIZE = BLOCKS * 16 };
    static const uint32_t chunk_sizes[] = {16, 48, 160, 640};

    for (int channels = 1; channels <= 2; channels++) {
        for (size_t c = 0; c < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); c++) {
            uint8_t whole[SIZE], pieces[SIZE];

            memset(whole, 0x07, sizeof(whole));
            memset(pieces, 0x07, sizeof(pieces));
            audio_adpcm_patch_flags(whole, SIZE, 0, SIZE, channels, 1, 1);
            for (uint32_t offset = 0; offset < SIZE; offset += chunk_sizes[c]) {
                uint32_t size = SIZE - offset < chunk_sizes[c] ? SIZE - offset : chunk_sizes[c];

                audio_adpcm_patch_flags(pieces + offset, size, offset, SIZE, channels, 1, 1);
            }
            CHECK(memcmp(whole, pieces, SIZE) == 0);
        }
    }
}

static void test_parse_derives_sample_rate_from_pitch(void)
{
    static const struct { uint32_t pitch; int32_t rate; } cases[] = {
        {0x0EB3, 44100}, {0x1000, 48000}, {0x0800, 24000}, {0x05A8, 16969}, {0x0000, 44100}, {0x2000, 44100}, {0x0010, 44100},
    };
    uint8_t file[16 + 32];
    audio_adpcm_view_t view;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(file, 0, sizeof(file));
        make_header(file, 1, 0);
        file[8] = (uint8_t)cases[i].pitch;
        file[9] = (uint8_t)(cases[i].pitch >> 8);
        CHECK(audio_apcm_parse(file, sizeof(file), &view) == 0);
        CHECK(view.sample_rate_hz == cases[i].rate);
    }
}

static void test_patch_single_block_run(void)
{
    uint8_t clip[32];

    memset(clip, 0, sizeof(clip));
    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 2, 1, 0);
    CHECK(clip[1] == 0x07 && clip[17] == 0x07);
    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 2, 0, 0);
    CHECK(clip[1] == AUDIO_ADPCM_FLAG_LOOP_END && clip[17] == AUDIO_ADPCM_FLAG_LOOP_END);
}

static void test_patching_in_chunks_equals_patching_whole(void)
{
    enum { BLOCKS = 40, SIZE = BLOCKS * 16 };
    static const uint32_t chunk_sizes[] = {16, 48, 64, 160, 240, 640};

    for (int loops = 0; loops < 2; loops++) {
        for (int channels = 1; channels <= 2; channels++) {
            for (size_t c = 0; c < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); c++) {
                uint8_t whole[SIZE], pieces[SIZE];

                memset(whole, 0x55, sizeof(whole));
                memset(pieces, 0x55, sizeof(pieces));
                audio_adpcm_patch_flags(whole, SIZE, 0, SIZE, channels, loops, 0);
                for (uint32_t offset = 0; offset < SIZE; offset += chunk_sizes[c]) {
                    uint32_t size = SIZE - offset < chunk_sizes[c] ? SIZE - offset : chunk_sizes[c];

                    audio_adpcm_patch_flags(pieces + offset, size, offset, SIZE, channels, loops, 0);
                }
                CHECK(memcmp(whole, pieces, SIZE) == 0);
            }
        }
    }
}

static void test_patch_leaves_audio_bytes_alone(void)
{
    uint8_t clip[64];
    uint8_t original[64];

    for (int i = 0; i < 64; i++)
        clip[i] = original[i] = (uint8_t)(i * 7 + 3);
    audio_adpcm_patch_flags(clip, sizeof(clip), 0, sizeof(clip), 1, 1, 0);
    for (int i = 0; i < 64; i++)
        if (i % 16 != 1)
            CHECK(clip[i] == original[i]);
}

static void test_real_adp_files_parse_and_patch(void)
{
    static const char *const paths[] = {
        "../../../../Open-PS2-Loader/audio/bd_connect.adp", "../../../../Open-PS2-Loader/audio/bd_disconnect.adp",
        "../../../../Open-PS2-Loader/audio/boot.adp",       "../../../../Open-PS2-Loader/audio/cancel.adp",
        "../../../../Open-PS2-Loader/audio/confirm.adp",    "../../../../Open-PS2-Loader/audio/cursor.adp",
        "../../../../Open-PS2-Loader/audio/message.adp",    "../../../../Open-PS2-Loader/audio/transition.adp",
        "../../../../scratch/audiotest/funk.adp",           "../../../../scratch/audiotest/funk_mono.adp",
        "../../../../scratch/audiotest/funk_short.adp",     "../../../../scratch/audiotest/funk_full_mono.adp",
    };
    int parsed = 0;

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        uint32_t size;
        uint8_t *file = read_file(paths[i], &size);
        audio_adpcm_view_t view;
        uint8_t *copy;
        uint32_t run;

        if (!file) {
            printf("skip (not found): %s\n", paths[i]);
            continue;
        }
        CHECK(audio_apcm_parse(file, size, &view) == 0);
        CHECK(view.size_bytes == size - 16);
        copy = malloc(view.size_bytes);
        memcpy(copy, view.blocks, view.size_bytes);
        audio_adpcm_patch_flags(copy, view.size_bytes, 0, view.size_bytes, view.channel_count, 0, 0);
        run = view.size_bytes / (uint32_t)view.channel_count;
        for (int c = 0; c < view.channel_count; c++)
            CHECK(copy[(uint32_t)c * run + run - 15] == AUDIO_ADPCM_FLAG_LOOP_END);
        CHECK(view.sample_rate_hz == 44100);
        free(copy);
        free(file);
        parsed++;
    }
    printf("real .adp files parsed: %d of %d\n", parsed, (int)(sizeof(paths) / sizeof(paths[0])));
    CHECK(parsed >= 8);
}

int main(void)
{
    test_parse_rejects_malformed_files();
    test_patch_one_shot_ends_each_run_only();
    test_patch_looping_marks_start_repeat_end_per_run();
    test_patch_single_block_run();
    test_patch_loop_ends_before_pad_block();
    test_patch_with_pad_in_chunks_equals_whole();
    test_parse_derives_sample_rate_from_pitch();
    test_patching_in_chunks_equals_patching_whole();
    test_patch_leaves_audio_bytes_alone();
    test_real_adp_files_parse_and_patch();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio_adpcm: all tests passed\n");
    return 0;
}
