#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_pcm.h"

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static int16_t left_sample(const uint8_t *blocks, uint32_t frame)
{
    const uint8_t *block = blocks + (frame / 256u) * AUDIO_PCM_BLOCK_BYTES;
    uint32_t i = frame % 256u;

    return (int16_t)((uint16_t)block[i * 2] | ((uint16_t)block[i * 2 + 1] << 8));
}

static int16_t right_sample(const uint8_t *blocks, uint32_t frame)
{
    const uint8_t *block = blocks + (frame / 256u) * AUDIO_PCM_BLOCK_BYTES + AUDIO_PCM_BLOCK_BYTES / 2;
    uint32_t i = frame % 256u;

    return (int16_t)((uint16_t)block[i * 2] | ((uint16_t)block[i * 2 + 1] << 8));
}

static void test_init_validates_format(void)
{
    audio_pcm_converter_t c;

    CHECK(audio_pcm_converter_init(&c, 48000, 16, 2) == 0);
    CHECK(audio_pcm_converter_init(&c, 8000, 8, 1) == 0);
    CHECK(audio_pcm_converter_init(&c, 7999, 16, 2) == -1);
    CHECK(audio_pcm_converter_init(&c, 48001, 16, 2) == -1);
    CHECK(audio_pcm_converter_init(&c, 44100, 12, 2) == -1);
    CHECK(audio_pcm_converter_init(&c, 44100, 16, 3) == -1);
    CHECK(audio_pcm_converter_init(&c, 44100, 16, 0) == -1);
    CHECK(audio_pcm_converter_init(&c, 44100, 16, 2) == 0 && audio_pcm_bytes_per_source_frame(&c) == 4);
    CHECK(audio_pcm_converter_init(&c, 44100, 8, 1) == 0 && audio_pcm_bytes_per_source_frame(&c) == 1);
}

static void test_48k_stereo_is_a_delayed_passthrough_in_planar_blocks(void)
{
    enum { FRAMES = 1025 };
    static int16_t source[FRAMES * 2];
    static uint8_t out[8 * AUDIO_PCM_BLOCK_BYTES];
    audio_pcm_converter_t c;
    uint32_t blocks = 0;
    uint32_t used;

    for (int i = 0; i < FRAMES; i++) {
        source[i * 2] = (int16_t)(i * 3);
        source[i * 2 + 1] = (int16_t)(-i * 5);
    }
    audio_pcm_converter_init(&c, 48000, 16, 2);
    used = audio_pcm_convert(&c, (const uint8_t *)source, sizeof(source), out, 8, &blocks);
    CHECK(used == sizeof(source));
    CHECK(blocks == 4);
    for (uint32_t f = 0; f < 4 * 256; f++) {
        CHECK(left_sample(out, f) == (int16_t)(f * 3));
        CHECK(right_sample(out, f) == (int16_t)(-(int32_t)f * 5));
    }
    CHECK(c.accumulated_frames == 0);
}

static void test_mono_is_duplicated_and_8bit_is_signed_and_scaled(void)
{
    static const int8_t source8[6] = {0, 10, -10, 127, -128, 5};
    static uint8_t out[AUDIO_PCM_BLOCK_BYTES];
    uint8_t padded[700];
    audio_pcm_converter_t c;
    uint32_t blocks = 0;

    memset(padded, 0, sizeof(padded));
    memcpy(padded, source8, sizeof(source8));
    audio_pcm_converter_init(&c, 48000, 8, 1);
    audio_pcm_convert(&c, padded, sizeof(padded), out, 1, &blocks);
    CHECK(blocks == 1);
    CHECK(left_sample(out, 0) == 0 && right_sample(out, 0) == 0);
    CHECK(left_sample(out, 1) == 10 * 256 && right_sample(out, 1) == 10 * 256);
    CHECK(left_sample(out, 2) == -10 * 256 && right_sample(out, 2) == -10 * 256);
    CHECK(left_sample(out, 3) == 127 * 256);
    CHECK(left_sample(out, 4) == -128 * 256);
}

static void test_output_frame_count_matches_the_rate_ratio(void)
{
    static const int rates[] = {8000, 11025, 12000, 22050, 24000, 32000, 44100, 48000};

    for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        int rate = rates[r];
        uint32_t source_frames = (uint32_t)rate;
        uint8_t *source = calloc(source_frames, 2);
        uint8_t *out = malloc(300u * AUDIO_PCM_BLOCK_BYTES);
        audio_pcm_converter_t c;
        uint32_t blocks = 0;
        uint32_t used;
        double produced;

        audio_pcm_converter_init(&c, rate, 16, 1);
        used = audio_pcm_convert(&c, source, source_frames * 2, out, 300, &blocks);
        CHECK(used == source_frames * 2);
        produced = (double)blocks * 256.0 + (double)c.accumulated_frames;
        CHECK(fabs(produced - 48000.0) < 8.0);
        free(source);
        free(out);
    }
}

static void test_upsampled_sine_keeps_its_frequency_and_shape(void)
{
    enum { RATE = 22050, SOURCE_FRAMES = 22050 };
    static int16_t source[SOURCE_FRAMES];
    static uint8_t out[300 * AUDIO_PCM_BLOCK_BYTES];
    audio_pcm_converter_t c;
    uint32_t blocks = 0;
    uint32_t crossings = 0;
    uint32_t total;

    for (int i = 0; i < SOURCE_FRAMES; i++)
        source[i] = (int16_t)(20000.0 * sin(2.0 * 3.14159265358979 * 441.0 * i / RATE));
    audio_pcm_converter_init(&c, RATE, 16, 1);
    audio_pcm_convert(&c, (const uint8_t *)source, sizeof(source), out, 300, &blocks);
    total = blocks * 256u;
    for (uint32_t f = 1; f < total; f++)
        if (left_sample(out, f - 1) < 0 && left_sample(out, f) >= 0)
            crossings++;
    CHECK(crossings >= 439 && crossings <= 442);
    for (uint32_t f = 0; f < total; f++)
        CHECK(abs(left_sample(out, f)) <= 20001);
}

static void test_chunked_conversion_equals_one_shot(void)
{
    enum { FRAMES = 6000 };
    static int16_t source[FRAMES * 2];
    static uint8_t whole[60 * AUDIO_PCM_BLOCK_BYTES];
    static uint8_t pieces[60 * AUDIO_PCM_BLOCK_BYTES];
    static const uint32_t chunk_bytes[] = {4, 40, 100, 1000, 4096};

    for (int i = 0; i < FRAMES; i++) {
        source[i * 2] = (int16_t)((i * 37) % 30000 - 15000);
        source[i * 2 + 1] = (int16_t)((i * 53) % 30000 - 15000);
    }
    for (size_t k = 0; k < sizeof(chunk_bytes) / sizeof(chunk_bytes[0]); k++) {
        audio_pcm_converter_t a, b;
        uint32_t whole_blocks = 0, piece_blocks = 0, used;
        const uint8_t *bytes = (const uint8_t *)source;
        uint32_t offset = 0;

        audio_pcm_converter_init(&a, 32000, 16, 2);
        audio_pcm_converter_init(&b, 32000, 16, 2);
        used = audio_pcm_convert(&a, bytes, sizeof(source), whole, 60, &whole_blocks);
        CHECK(used == sizeof(source));

        while (offset < sizeof(source)) {
            uint32_t size = sizeof(source) - offset < chunk_bytes[k] ? (uint32_t)sizeof(source) - offset : chunk_bytes[k];
            uint32_t got = 0;

            offset += audio_pcm_convert(&b, bytes + offset, size, pieces + (size_t)piece_blocks * AUDIO_PCM_BLOCK_BYTES, 60 - piece_blocks, &got);
            piece_blocks += got;
        }
        CHECK(whole_blocks == piece_blocks);
        CHECK(memcmp(whole, pieces, (size_t)whole_blocks * AUDIO_PCM_BLOCK_BYTES) == 0);
        CHECK(a.accumulated_frames == b.accumulated_frames);
    }
}

static void test_a_full_output_buffer_stops_without_overrun_and_resumes(void)
{
    enum { FRAMES = 4000 };
    static int16_t source[FRAMES];
    static uint8_t guarded[5 * AUDIO_PCM_BLOCK_BYTES];
    static uint8_t second[40 * AUDIO_PCM_BLOCK_BYTES];
    audio_pcm_converter_t c;
    uint32_t blocks = 0, consumed = 0, more = 0;

    for (int i = 0; i < FRAMES; i++)
        source[i] = (int16_t)i;
    memset(guarded, 0xAB, sizeof(guarded));
    audio_pcm_converter_init(&c, 24000, 16, 1);

    consumed = audio_pcm_convert(&c, (const uint8_t *)source, sizeof(source), guarded, 2, &blocks);
    CHECK(blocks == 2);
    CHECK(consumed > 0 && consumed < sizeof(source));
    for (size_t i = 2 * AUDIO_PCM_BLOCK_BYTES; i < sizeof(guarded); i++)
        CHECK(guarded[i] == 0xAB);

    consumed += audio_pcm_convert(&c, (const uint8_t *)source + consumed, sizeof(source) - consumed, second, 40, &more);
    CHECK(consumed == sizeof(source));
    CHECK(more >= 1);
}

static void test_reset_clears_partial_blocks_and_history(void)
{
    static int16_t source[100];
    static uint8_t out[AUDIO_PCM_BLOCK_BYTES];
    audio_pcm_converter_t c;
    uint32_t blocks;

    audio_pcm_converter_init(&c, 48000, 16, 1);
    audio_pcm_convert(&c, (const uint8_t *)source, sizeof(source), out, 1, &blocks);
    CHECK(c.accumulated_frames == 99 && blocks == 0);
    audio_pcm_converter_reset(&c);
    CHECK(c.accumulated_frames == 0 && !c.have_previous && c.phase_q16 == 0);
    CHECK(c.sample_rate_hz == 48000);
}

int main(void)
{
    test_init_validates_format();
    test_48k_stereo_is_a_delayed_passthrough_in_planar_blocks();
    test_mono_is_duplicated_and_8bit_is_signed_and_scaled();
    test_output_frame_count_matches_the_rate_ratio();
    test_upsampled_sine_keeps_its_frequency_and_shape();
    test_chunked_conversion_equals_one_shot();
    test_a_full_output_buffer_stops_without_overrun_and_resumes();
    test_reset_clears_partial_blocks_and_history();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio_pcm: all tests passed\n");
    return 0;
}
