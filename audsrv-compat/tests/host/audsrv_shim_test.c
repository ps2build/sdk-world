#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audsrv.h"

static int failures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++;                                                 \
        }                                                               \
    } while (0)

/* Mock of the audio library: every function records its call and returns the configured result. */
static struct
{
    int init_result, shutdown_calls, stream_start_result, stream_start_calls;
    audio_stream_format_t stream_format;
    int wait_result, wait_calls, wait_last_bytes;
    int queue_result_limit, queue_calls, queue_total, queue_error;
    int stream_volume_calls, stream_volume, master_volume_calls;
    int stop_clear_calls, free_bytes, queued_bytes;
    int free_all_calls, free_all_result;
    int volume_pan_calls, volume_pan_channel, volume_pan_volume, volume_pan_pan, volume_pan_result;
    int load_result, load_calls;
    audio_sound_t load_handle;
    int play_channel_calls, play_channel_channel, play_channel_result;
    audio_sound_t play_channel_sound;
    int play_any_calls, play_any_result;
    audio_sound_t play_any_sound;
    audio_sound_t playing_sound;
    int playing_result;
    int low_calls, low_threshold, low_result;
    audio_low_data_fn low_callback;
    void *low_user;
    int free_sound_calls, free_sound_result;
    audio_sound_t free_sound_handle;
} mock;

static void mock_reset(void)
{
    memset(&mock, 0, sizeof(mock));
    mock.queue_result_limit = 1 << 30;
}

int audio_init(void) { return mock.init_result; }
int audio_shutdown(void) { mock.shutdown_calls++; return 0; }
int audio_stream_start(const audio_stream_format_t *format)
{
    mock.stream_start_calls++;
    mock.stream_format = *format;
    return mock.stream_start_result;
}
int audio_stream_wait_until_writable(int32_t bytes, int32_t timeout_ms)
{
    (void)timeout_ms;
    mock.wait_calls++;
    mock.wait_last_bytes = bytes;
    return mock.wait_result;
}
int audio_stream_queue_pcm(const void *pcm, int32_t bytes)
{
    int accepted = bytes < mock.queue_result_limit ? bytes : mock.queue_result_limit;

    (void)pcm;
    mock.queue_calls++;
    if (mock.queue_error)
        return mock.queue_error;
    mock.queue_total += accepted;
    return accepted;
}
int audio_stream_set_volume(int volume) { mock.stream_volume_calls++; mock.stream_volume = volume; return 0; }
int audio_set_master_volume(int volume) { (void)volume; mock.master_volume_calls++; return 0; }
int audio_stream_stop_and_clear(void) { mock.stop_clear_calls++; return 0; }
int audio_stream_get_free_bytes(void) { return mock.free_bytes; }
int audio_stream_set_low_data_callback(int32_t threshold, audio_low_data_fn callback, void *user)
{
    mock.low_calls++;
    mock.low_threshold = threshold;
    mock.low_callback = callback;
    mock.low_user = user;
    return mock.low_result;
}
int audio_stream_get_queued_bytes(void) { return mock.queued_bytes; }
int audio_sound_free_all(void) { mock.free_all_calls++; return mock.free_all_result; }
int audio_channel_set_volume_and_pan(int channel, int volume, int pan)
{
    mock.volume_pan_calls++;
    mock.volume_pan_channel = channel;
    mock.volume_pan_volume = volume;
    mock.volume_pan_pan = pan;
    return mock.volume_pan_result;
}
int audio_sound_load_apcm(const void *file, int32_t size, audio_sound_t *out)
{
    (void)file;
    (void)size;
    mock.load_calls++;
    *out = mock.load_result == 0 ? mock.load_handle : AUDIO_SOUND_INVALID;
    return mock.load_result;
}
int audio_channel_play_sound(int channel, audio_sound_t sound)
{
    mock.play_channel_calls++;
    mock.play_channel_channel = channel;
    mock.play_channel_sound = sound;
    return mock.play_channel_result;
}
int audio_play_sound(audio_sound_t sound)
{
    mock.play_any_calls++;
    mock.play_any_sound = sound;
    return mock.play_any_result;
}
int audio_channel_get_playing_sound(int channel, audio_sound_t *out)
{
    (void)channel;
    *out = mock.playing_sound;
    return mock.playing_result;
}
int audio_sound_free(audio_sound_t sound)
{
    mock.free_sound_calls++;
    mock.free_sound_handle = sound;
    return mock.free_sound_result;
}

static void test_init_treats_repeat_init_as_success_and_translates_failures(void)
{
    mock_reset();
    CHECK(audsrv_init() == 0);
    mock.init_result = -AUDIO_ERR_ALREADY_INITIALIZED;
    CHECK(audsrv_init() == 0);
    mock.init_result = -AUDIO_ERR_RPC_FAILED;
    CHECK(audsrv_init() == -AUDSRV_ERR_RPC_FAILED);
    mock.init_result = -AUDIO_ERR_DRIVER_LOAD_FAILED;
    CHECK(audsrv_init() == -AUDSRV_ERR_RPC_FAILED);
    CHECK(audsrv_quit() == 0 && mock.shutdown_calls == 1);
}

static void test_error_codes_translate_per_the_table(void)
{
    static const struct { int audio; int audsrv; } rows[] = {
        {AUDIO_ERR_NOT_INITIALIZED, AUDSRV_ERR_NOT_INITIALIZED},
        {AUDIO_ERR_RPC_FAILED, AUDSRV_ERR_RPC_FAILED},
        {AUDIO_ERR_FORMAT_NOT_SUPPORTED, AUDSRV_ERR_FORMAT_NOT_SUPPORTED},
        {AUDIO_ERR_OUT_OF_MEMORY, AUDSRV_ERR_OUT_OF_MEMORY},
        {AUDIO_ERR_OUT_OF_SPU_MEMORY, AUDSRV_ERR_OUT_OF_MEMORY},
        {AUDIO_ERR_INVALID_ARGUMENT, AUDSRV_ERR_ARGS},
        {AUDIO_ERR_BAD_SOUND_DATA, AUDSRV_ERR_ARGS},
        {AUDIO_ERR_STREAM_NOT_STARTED, AUDSRV_ERR_ARGS},
        {AUDIO_ERR_NO_FREE_CHANNEL, AUDSRV_ERR_NO_MORE_CHANNELS},
        {AUDIO_ERR_TIMEOUT, AUDSRV_ERR_ARGS},
        {AUDIO_ERR_INTERRUPTED, AUDSRV_ERR_ARGS},
        {AUDIO_ERR_STREAM_PAUSED, AUDSRV_ERR_ARGS},
        {AUDIO_ERR_BUSY, AUDSRV_ERR_ARGS},
    };

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        mock_reset();
        mock.wait_result = -rows[i].audio;
        CHECK(audsrv_wait_audio(100) == -rows[i].audsrv);
        CHECK(audsrv_get_error() == rows[i].audsrv);
    }
}

static void test_error_string_follows_the_last_error(void)
{
    mock_reset();
    mock.wait_result = -AUDIO_ERR_NO_FREE_CHANNEL;
    audsrv_wait_audio(1);
    CHECK(strcmp(audsrv_get_error_string(), "no free channel") == 0);
    audsrv_play_cd(1);
    CHECK(strcmp(audsrv_get_error_string(), "no disc") == 0);
}

static void test_set_format_copies_fields_and_reports_zero_on_success(void)
{
    audsrv_fmt_t fmt = {22050, 8, 2};

    mock_reset();
    CHECK(audsrv_set_format(&fmt) == 0);
    CHECK(mock.stream_format.sample_rate_hz == 22050 && mock.stream_format.bits_per_sample == 8 && mock.stream_format.channel_count == 2);
    mock.stream_start_result = -AUDIO_ERR_FORMAT_NOT_SUPPORTED;
    CHECK(audsrv_set_format(&fmt) == -AUDSRV_ERR_FORMAT_NOT_SUPPORTED);
    CHECK(audsrv_set_format(0) == -AUDSRV_ERR_ARGS);
}

static void test_volume_is_the_stream_volume_not_the_master(void)
{
    mock_reset();
    CHECK(audsrv_set_volume(42) == 0);
    CHECK(mock.stream_volume_calls == 1 && mock.stream_volume == 42 && mock.master_volume_calls == 0);
}

static void test_play_audio_loops_in_chunks_until_everything_is_accepted(void)
{
    static char data[10000];

    mock_reset();
    CHECK(audsrv_play_audio(data, sizeof(data)) == (int)sizeof(data));
    CHECK(mock.queue_total == (int)sizeof(data));
    CHECK(mock.queue_calls == 3 && mock.wait_calls == 3);
    CHECK(mock.wait_last_bytes == (int)sizeof(data) - 2 * 4096);
    CHECK(audsrv_play_audio(data, 0) == 0);
}

static void test_play_audio_stops_when_nothing_more_is_accepted(void)
{
    static char data[9000];

    mock_reset();
    mock.queue_result_limit = 1000;
    CHECK(audsrv_play_audio(data, sizeof(data)) == (int)sizeof(data));
    CHECK(mock.queue_calls == 9);
    mock_reset();
    mock.queue_result_limit = 0;
    CHECK(audsrv_play_audio(data, 500) == 0);
}

static void test_play_audio_reports_errors_only_when_nothing_was_queued(void)
{
    static char data[8192];

    mock_reset();
    mock.wait_result = -AUDIO_ERR_STREAM_NOT_STARTED;
    CHECK(audsrv_play_audio(data, 100) == -AUDSRV_ERR_ARGS);
    CHECK(audsrv_play_audio(0, 100) == -AUDSRV_ERR_ARGS);
    CHECK(audsrv_play_audio(data, -1) == -AUDSRV_ERR_ARGS);

    mock_reset();
    mock.queue_error = -AUDIO_ERR_RPC_FAILED;
    CHECK(audsrv_play_audio(data, 100) == -AUDSRV_ERR_RPC_FAILED);
}

static void test_stop_available_queued_and_unsupported_calls(void)
{
    mock_reset();
    CHECK(audsrv_stop_audio() == 0 && mock.stop_clear_calls == 1);
    mock.free_bytes = 12345;
    mock.queued_bytes = 678;
    CHECK(audsrv_available() == 12345);
    CHECK(audsrv_queued() == 678);
    CHECK(audsrv_play_cd(1) == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_stop_cd() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_get_cdpos() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_get_trackpos() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_get_numtracks() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_get_track_offset(2) == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_pause_cd() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_resume_cd() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_play_sectors(1, 2) == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_get_cd_status() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_get_cd_type() == -AUDSRV_ERR_NO_DISC);
    CHECK(audsrv_on_cdda_stop(0, 0) == -AUDSRV_ERR_NO_DISC);
}

static void test_adpcm_init_resets_clips_and_keeps_the_stream(void)
{
    mock_reset();
    CHECK(audsrv_adpcm_init() == 0 && mock.free_all_calls == 1 && mock.stop_clear_calls == 0);
    mock.free_all_result = -AUDIO_ERR_NOT_INITIALIZED;
    CHECK(audsrv_adpcm_init() == -AUDSRV_ERR_NOT_INITIALIZED);
}

static void test_adpcm_set_volume_macro_resets_pan_to_center(void)
{
    mock_reset();
    CHECK(audsrv_adpcm_set_volume(3, 70) == 0);
    CHECK(mock.volume_pan_channel == 3 && mock.volume_pan_volume == 70 && mock.volume_pan_pan == 0);
    CHECK(audsrv_adpcm_set_volume_and_pan(4, 50, -30) == 0);
    CHECK(mock.volume_pan_channel == 4 && mock.volume_pan_volume == 50 && mock.volume_pan_pan == -30);
    mock.volume_pan_result = -AUDIO_ERR_INVALID_ARGUMENT;
    CHECK(audsrv_adpcm_set_volume_and_pan(99, 50, 0) == -AUDSRV_ERR_ARGS);
}

static void test_load_fills_the_descriptor_from_the_header(void)
{
    unsigned char file[64];
    audsrv_adpcm_t adpcm;

    memset(file, 0, sizeof(file));
    memcpy(file, "APCM", 4);
    file[4] = 1;
    file[5] = 2;
    file[6] = 1;
    file[8] = 0xB3;
    file[9] = 0x0E;

    mock_reset();
    mock.load_handle = 4321;
    CHECK(audsrv_load_adpcm(&adpcm, file, sizeof(file)) == 0);
    CHECK(adpcm.pitch == 0x0EB3 && adpcm.loop == 1 && adpcm.channels == 2 && adpcm.size == (int)sizeof(file));
    CHECK((int)(intptr_t)adpcm.buffer == 4321);

    mock.load_result = -AUDIO_ERR_BAD_SOUND_DATA;
    CHECK(audsrv_load_adpcm(&adpcm, file, sizeof(file)) == -AUDSRV_ERR_FAILED_TO_LOAD_ADPCM);
    mock.load_result = -AUDIO_ERR_OUT_OF_SPU_MEMORY;
    CHECK(audsrv_load_adpcm(&adpcm, file, sizeof(file)) == -AUDSRV_ERR_OUT_OF_MEMORY);
    CHECK(audsrv_load_adpcm(0, file, sizeof(file)) == -AUDSRV_ERR_ARGS);
    CHECK(audsrv_load_adpcm(&adpcm, 0, sizeof(file)) == -AUDSRV_ERR_ARGS);
    CHECK(audsrv_load_adpcm(&adpcm, file, 8) == -AUDSRV_ERR_ARGS);
}

static audsrv_adpcm_t sample_with_handle(int handle)
{
    audsrv_adpcm_t adpcm = {0, 0, 1, (void *)(intptr_t)handle, 100};

    return adpcm;
}

static void test_play_on_a_valid_channel_replaces_and_returns_the_channel(void)
{
    audsrv_adpcm_t adpcm = sample_with_handle(77);

    mock_reset();
    CHECK(audsrv_ch_play_adpcm(9, &adpcm) == 9);
    CHECK(mock.play_channel_calls == 1 && mock.play_channel_channel == 9 && mock.play_channel_sound == 77);
    CHECK(mock.play_any_calls == 0);
    CHECK(audsrv_ch_play_adpcm(0, &adpcm) == 0);
    CHECK(audsrv_ch_play_adpcm(23, &adpcm) == 23);
}

static void test_play_on_an_invalid_channel_uses_the_first_free_one(void)
{
    audsrv_adpcm_t adpcm = sample_with_handle(77);

    mock_reset();
    mock.play_any_result = 5;
    CHECK(audsrv_ch_play_adpcm(-1, &adpcm) == 5);
    CHECK(audsrv_ch_play_adpcm(24, &adpcm) == 5);
    CHECK(audsrv_play_adpcm(&adpcm) == 5);
    CHECK(mock.play_any_calls == 3 && mock.play_any_sound == 77 && mock.play_channel_calls == 0);

    mock.play_any_result = -AUDIO_ERR_NO_FREE_CHANNEL;
    CHECK(audsrv_play_adpcm(&adpcm) == -AUDSRV_ERR_NO_MORE_CHANNELS);
}

static void test_play_rejects_unloaded_or_bad_samples(void)
{
    audsrv_adpcm_t unloaded = sample_with_handle(0);
    audsrv_adpcm_t adpcm = sample_with_handle(5);

    mock_reset();
    CHECK(audsrv_ch_play_adpcm(1, &unloaded) == -AUDSRV_ERR_ARGS);
    CHECK(audsrv_ch_play_adpcm(1, 0) == -AUDSRV_ERR_ARGS);
    CHECK(mock.play_channel_calls == 0);
    mock.play_channel_result = -AUDIO_ERR_INVALID_ARGUMENT;
    CHECK(audsrv_ch_play_adpcm(1, &adpcm) == -AUDSRV_ERR_ARGS);
}

static void test_is_playing_compares_the_sound_on_that_channel(void)
{
    audsrv_adpcm_t adpcm = sample_with_handle(10);

    mock_reset();
    mock.playing_sound = 10;
    CHECK(audsrv_is_adpcm_playing(3, &adpcm) == 1);
    mock.playing_sound = 11;
    CHECK(audsrv_is_adpcm_playing(3, &adpcm) == 0);
    mock.playing_sound = AUDIO_SOUND_INVALID;
    CHECK(audsrv_is_adpcm_playing(3, &adpcm) == 0);
    mock.playing_sound = 10;
    CHECK(audsrv_is_adpcm_playing(24, &adpcm) == 0);
    mock.playing_result = -AUDIO_ERR_NOT_INITIALIZED;
    CHECK(audsrv_is_adpcm_playing(3, &adpcm) == 0);
}

static void test_free_releases_the_sound_and_clears_the_descriptor(void)
{
    audsrv_adpcm_t adpcm = sample_with_handle(33);

    mock_reset();
    CHECK(audsrv_free_adpcm(&adpcm) == 0);
    CHECK(mock.free_sound_calls == 1 && mock.free_sound_handle == 33 && adpcm.buffer == 0);
    CHECK(audsrv_free_adpcm(&adpcm) == -AUDSRV_ERR_ARGS);
    CHECK(mock.free_sound_calls == 1);
}

static int fillbuf_calls;
static void *fillbuf_seen_argument;

static int fillbuf_user_callback(void *arg)
{
    fillbuf_calls++;
    fillbuf_seen_argument = arg;
    return 0;
}

static void test_on_fillbuf_maps_free_space_to_a_low_data_threshold(void)
{
    int marker = 0;

    mock_reset();
    mock.free_bytes = 10000;
    mock.queued_bytes = 2000;
    CHECK(audsrv_on_fillbuf(4000, fillbuf_user_callback, &marker) == 0);
    CHECK(mock.low_calls == 1 && mock.low_threshold == 12000 - 4000 && mock.low_callback != 0);

    fillbuf_calls = 0;
    mock.low_callback(mock.low_user, 1234);
    CHECK(fillbuf_calls == 1 && fillbuf_seen_argument == &marker);

    CHECK(audsrv_on_fillbuf(13000, fillbuf_user_callback, &marker) == -AUDSRV_ERR_ARGS);
    CHECK(audsrv_on_fillbuf(-1, fillbuf_user_callback, &marker) == -AUDSRV_ERR_ARGS);

    CHECK(audsrv_on_fillbuf(0, 0, 0) == 0);
    CHECK(mock.low_callback == 0);
    fillbuf_calls = 0;
}

static void test_on_fillbuf_reports_a_stream_that_is_not_started(void)
{
    mock_reset();
    mock.free_bytes = -AUDIO_ERR_STREAM_NOT_STARTED;
    CHECK(audsrv_on_fillbuf(100, fillbuf_user_callback, 0) == -AUDSRV_ERR_ARGS);
    CHECK(mock.low_calls == 0);
    mock_reset();
    mock.free_bytes = 100;
    mock.queued_bytes = 0;
    mock.low_result = -AUDIO_ERR_STREAM_NOT_STARTED;
    CHECK(audsrv_on_fillbuf(50, fillbuf_user_callback, 0) == -AUDSRV_ERR_ARGS);
}

int main(void)
{
    test_init_treats_repeat_init_as_success_and_translates_failures();
    test_error_codes_translate_per_the_table();
    test_error_string_follows_the_last_error();
    test_set_format_copies_fields_and_reports_zero_on_success();
    test_volume_is_the_stream_volume_not_the_master();
    test_play_audio_loops_in_chunks_until_everything_is_accepted();
    test_play_audio_stops_when_nothing_more_is_accepted();
    test_play_audio_reports_errors_only_when_nothing_was_queued();
    test_stop_available_queued_and_unsupported_calls();
    test_on_fillbuf_maps_free_space_to_a_low_data_threshold();
    test_on_fillbuf_reports_a_stream_that_is_not_started();
    test_adpcm_init_resets_clips_and_keeps_the_stream();
    test_adpcm_set_volume_macro_resets_pan_to_center();
    test_load_fills_the_descriptor_from_the_header();
    test_play_on_a_valid_channel_replaces_and_returns_the_channel();
    test_play_on_an_invalid_channel_uses_the_first_free_one();
    test_play_rejects_unloaded_or_bad_samples();
    test_is_playing_compares_the_sound_on_that_channel();
    test_free_releases_the_sound_and_clears_the_descriptor();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("audsrv_shim: all tests passed\n");
    return 0;
}
