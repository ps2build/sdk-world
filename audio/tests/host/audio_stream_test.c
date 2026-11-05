#include <stdio.h>
#include <string.h>

#include "audio_stream.h"

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

typedef struct mock
{
    int dma_running;
    int start_calls;
    int stop_calls;
    int volume;
    int critical_depth;
    int fail_start;
    uint32_t half_bytes;
    uint32_t events;
    int event_calls;
} mock_t;

static int mock_start(void *context, uint8_t *buffer, uint32_t half_bytes)
{
    mock_t *m = context;

    (void)buffer;
    m->start_calls++;
    m->half_bytes = half_bytes;
    if (m->fail_start)
        return -1;
    m->dma_running = 1;
    return 0;
}

static void mock_stop(void *context)
{
    ((mock_t *)context)->dma_running = 0;
    ((mock_t *)context)->stop_calls++;
}

static void mock_volume(void *context, int volume)
{
    ((mock_t *)context)->volume = volume;
}

static void mock_events(void *context, uint32_t events)
{
    ((mock_t *)context)->events |= events;
    ((mock_t *)context)->event_calls++;
}

static uint32_t mock_enter(void *context)
{
    ((mock_t *)context)->critical_depth++;
    return 7;
}

static void mock_leave(void *context, uint32_t state)
{
    CHECK(state == 7);
    ((mock_t *)context)->critical_depth--;
}

typedef struct fixture
{
    mock_t mock;
    audio_stream_hw_ops_t ops;
    uint8_t ring[AUDIO_STREAM_RING_BYTES];
    uint8_t dma[AUDIO_STREAM_DMA_BYTES];
    audio_stream_t stream;
} fixture_t;

static fixture_t f;

static void fixture_init(void)
{
    memset(&f, 0, sizeof(f));
    f.ops.start_dma = mock_start;
    f.ops.stop_dma = mock_stop;
    f.ops.set_volume = mock_volume;
    f.ops.signal_events = mock_events;
    f.ops.enter_critical = mock_enter;
    f.ops.leave_critical = mock_leave;
    f.ops.context = &f.mock;
    memset(f.dma, 0xEE, sizeof(f.dma));
    audio_stream_init(&f.stream, &f.ops, f.ring, f.dma);
}

static void fill_pattern(uint8_t *bytes, uint32_t count, uint32_t seed)
{
    for (uint32_t i = 0; i < count; i++)
        bytes[i] = (uint8_t)((seed + i) * 31u + (i >> 8));
}

static int half_equals(int half, const uint8_t *expected)
{
    return memcmp(f.dma + half * AUDIO_STREAM_HALF_BYTES, expected, AUDIO_STREAM_HALF_BYTES) == 0;
}

static int half_is_silent(int half)
{
    for (uint32_t i = 0; i < AUDIO_STREAM_HALF_BYTES; i++)
        if (f.dma[half * AUDIO_STREAM_HALF_BYTES + i] != 0)
            return 0;
    return 1;
}

static void test_start_silences_the_dma_buffer_and_starts_hardware(void)
{
    fixture_init();
    CHECK(audio_stream_start(&f.stream) == 0);
    CHECK(f.mock.dma_running && f.mock.start_calls == 1);
    CHECK(f.mock.half_bytes == AUDIO_STREAM_HALF_BYTES);
    CHECK(half_is_silent(0) && half_is_silent(1));
    CHECK(f.stream.started && !f.stream.paused);
    CHECK(audio_stream_get_free_bytes(&f.stream) == AUDIO_STREAM_RING_BYTES);
}

static void test_start_failure_leaves_the_stream_stopped(void)
{
    fixture_init();
    f.mock.fail_start = 1;
    CHECK(audio_stream_start(&f.stream) == -1);
    CHECK(!f.stream.started);
    CHECK(audio_stream_write(&f.stream, f.ring, 1024) == 0);
}

static void test_restart_discards_queued_data_and_stops_first(void)
{
    static uint8_t data[4096];

    fixture_init();
    audio_stream_start(&f.stream);
    fill_pattern(data, sizeof(data), 1);
    audio_stream_write(&f.stream, data, sizeof(data));
    CHECK(audio_stream_get_queued_bytes(&f.stream) == 4096);
    CHECK(audio_stream_start(&f.stream) == 0);
    CHECK(f.mock.stop_calls == 1 && f.mock.start_calls == 2);
    CHECK(audio_stream_get_queued_bytes(&f.stream) == 0);
}

static void test_write_accepts_whole_blocks_only_and_respects_capacity(void)
{
    static uint8_t data[AUDIO_STREAM_RING_BYTES + 4096];

    fixture_init();
    CHECK(audio_stream_write(&f.stream, data, 1024) == 0);
    audio_stream_start(&f.stream);
    CHECK(audio_stream_write(&f.stream, data, 1500) == 1024);
    CHECK(audio_stream_write(&f.stream, data, 1023) == 0);
    CHECK(audio_stream_write(&f.stream, data, sizeof(data)) == AUDIO_STREAM_RING_BYTES - 1024);
    CHECK(audio_stream_get_free_bytes(&f.stream) == 0);
    CHECK(audio_stream_write(&f.stream, data, 1024) == 0);
}

static void test_data_reaches_the_dma_halves_in_order_across_ring_wrap(void)
{
    static uint8_t data[AUDIO_STREAM_HALF_BYTES * 40];
    uint32_t offset = 0;

    fixture_init();
    audio_stream_start(&f.stream);
    fill_pattern(data, sizeof(data), 5);

    for (int round = 0; round < 40; round++) {
        int half = round & 1;

        offset += audio_stream_write(&f.stream, data + offset, AUDIO_STREAM_HALF_BYTES);
        audio_stream_on_half_complete(&f.stream);
        CHECK(half_equals(half, data + (uint32_t)round * AUDIO_STREAM_HALF_BYTES));
    }
    CHECK(f.stream.underrun_count == 0);
    CHECK(audio_stream_get_queued_bytes(&f.stream) == 0);
    CHECK(f.stream.write_count > AUDIO_STREAM_RING_BYTES);
}

static void test_underrun_plays_the_partial_tail_then_silence(void)
{
    static uint8_t data[2048];

    fixture_init();
    audio_stream_start(&f.stream);
    fill_pattern(data, sizeof(data), 9);
    audio_stream_write(&f.stream, data, sizeof(data));

    audio_stream_on_half_complete(&f.stream);
    CHECK(memcmp(f.dma, data, sizeof(data)) == 0);
    for (uint32_t i = sizeof(data); i < AUDIO_STREAM_HALF_BYTES; i++)
        CHECK(f.dma[i] == 0);
    CHECK(f.stream.underrun_count == 1);

    audio_stream_on_half_complete(&f.stream);
    CHECK(half_is_silent(1));
    CHECK(f.stream.underrun_count == 2);
}

static void test_pause_outputs_silence_keeps_data_and_resumes_in_place(void)
{
    static uint8_t data[8192];

    fixture_init();
    audio_stream_start(&f.stream);
    fill_pattern(data, sizeof(data), 3);
    audio_stream_write(&f.stream, data, sizeof(data));

    audio_stream_on_half_complete(&f.stream);
    CHECK(half_equals(0, data));

    audio_stream_pause(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    CHECK(half_is_silent(1) && half_is_silent(0));
    CHECK(audio_stream_get_queued_bytes(&f.stream) == 4096);
    CHECK(f.stream.underrun_count == 0);

    audio_stream_resume(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    CHECK(half_equals(1, data + 4096));
}

static void test_clear_drops_data_and_silences_pending_halves(void)
{
    static uint8_t data[8192];

    fixture_init();
    audio_stream_start(&f.stream);
    fill_pattern(data, sizeof(data), 4);
    audio_stream_write(&f.stream, data, sizeof(data));
    audio_stream_on_half_complete(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    CHECK(!half_is_silent(0) && !half_is_silent(1));

    audio_stream_write(&f.stream, data, 4096);
    audio_stream_clear(&f.stream);
    CHECK(f.mock.critical_depth == 0);
    CHECK(audio_stream_get_queued_bytes(&f.stream) == 0);
    CHECK(half_is_silent(0) && half_is_silent(1));
    CHECK(f.stream.started);

    audio_stream_write(&f.stream, data, 4096);
    audio_stream_on_half_complete(&f.stream);
    CHECK(half_equals(0, data));
}

static void test_end_stops_hardware_and_rejects_writes(void)
{
    static uint8_t data[1024];

    fixture_init();
    audio_stream_start(&f.stream);
    audio_stream_write(&f.stream, data, sizeof(data));
    audio_stream_end(&f.stream);
    CHECK(!f.mock.dma_running && !f.stream.started);
    CHECK(audio_stream_get_queued_bytes(&f.stream) == 0);
    CHECK(audio_stream_write(&f.stream, data, sizeof(data)) == 0);
    audio_stream_end(&f.stream);
    CHECK(f.mock.stop_calls == 1);
}

static void test_volume_is_passed_to_hardware_and_kept(void)
{
    fixture_init();
    audio_stream_set_volume(&f.stream, 37);
    CHECK(f.mock.volume == 37 && f.stream.volume_percent == 37);
    audio_stream_start(&f.stream);
    CHECK(f.stream.volume_percent == 37);
}

static uint32_t take_events(void)
{
    uint32_t events = f.mock.events;

    f.mock.events = 0;
    return events;
}

static void test_every_completed_half_reports_space(void)
{
    static uint8_t data[8192];

    fixture_init();
    audio_stream_on_half_complete(&f.stream);
    CHECK(f.mock.event_calls == 0);
    audio_stream_start(&f.stream);
    audio_stream_write(&f.stream, data, sizeof(data));
    audio_stream_on_half_complete(&f.stream);
    CHECK(take_events() == AUDIO_STREAM_EVENT_SPACE);
    audio_stream_on_half_complete(&f.stream);
    CHECK(take_events() == AUDIO_STREAM_EVENT_SPACE);
    CHECK(f.mock.event_calls == 2);
}

static void test_low_data_fires_once_when_the_level_falls_to_the_threshold(void)
{
    static uint8_t data[16384];

    fixture_init();
    audio_stream_start(&f.stream);
    audio_stream_write(&f.stream, data, sizeof(data));
    audio_stream_set_low_threshold(&f.stream, 4096, 1);

    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) != 0);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
}

static void test_low_data_rearms_after_the_level_rises_above_the_threshold(void)
{
    static uint8_t data[16384];

    fixture_init();
    audio_stream_start(&f.stream);
    audio_stream_set_low_threshold(&f.stream, 2048, 1);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);

    audio_stream_write(&f.stream, data, 8192);
    audio_stream_on_half_complete(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) != 0);
}

static void test_enabling_does_not_report_a_level_that_is_already_low(void)
{
    fixture_init();
    audio_stream_start(&f.stream);
    audio_stream_set_low_threshold(&f.stream, 4096, 1);
    audio_stream_on_half_complete(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
}

static void test_disabling_stops_low_data_events(void)
{
    static uint8_t data[8192];

    fixture_init();
    audio_stream_start(&f.stream);
    audio_stream_write(&f.stream, data, sizeof(data));
    audio_stream_set_low_threshold(&f.stream, 4096, 1);
    audio_stream_set_low_threshold(&f.stream, 4096, 0);
    audio_stream_on_half_complete(&f.stream);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
}

static void test_threshold_zero_fires_only_at_empty(void)
{
    static uint8_t data[8192];

    fixture_init();
    audio_stream_start(&f.stream);
    audio_stream_write(&f.stream, data, sizeof(data));
    audio_stream_set_low_threshold(&f.stream, 0, 1);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) == 0);
    audio_stream_on_half_complete(&f.stream);
    CHECK((take_events() & AUDIO_STREAM_EVENT_LOW_DATA) != 0);
}

int main(void)
{
    test_start_silences_the_dma_buffer_and_starts_hardware();
    test_start_failure_leaves_the_stream_stopped();
    test_restart_discards_queued_data_and_stops_first();
    test_write_accepts_whole_blocks_only_and_respects_capacity();
    test_data_reaches_the_dma_halves_in_order_across_ring_wrap();
    test_underrun_plays_the_partial_tail_then_silence();
    test_pause_outputs_silence_keeps_data_and_resumes_in_place();
    test_clear_drops_data_and_silences_pending_halves();
    test_end_stops_hardware_and_rejects_writes();
    test_volume_is_passed_to_hardware_and_kept();
    test_every_completed_half_reports_space();
    test_low_data_fires_once_when_the_level_falls_to_the_threshold();
    test_low_data_rearms_after_the_level_rises_above_the_threshold();
    test_enabling_does_not_report_a_level_that_is_already_low();
    test_disabling_stops_low_data_events();
    test_threshold_zero_fires_only_at_empty();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("audio_stream: all tests passed\n");
    return 0;
}
