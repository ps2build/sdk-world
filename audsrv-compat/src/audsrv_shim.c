#include <stdint.h>

#include "audio.h"
#include "audsrv.h"

#define CHANNEL_COUNT          24
#define STREAM_WRITE_CHUNK     4096
#define APCM_HEADER_BYTES      16
#define APCM_CHANNELS_OFFSET   5
#define APCM_LOOP_OFFSET       6
#define APCM_PITCH_OFFSET      8

static int last_error;
static audsrv_callback_t fillbuf_callback;
static void *fillbuf_argument;

static int translate_error_code(int audio_error)
{
    switch (audio_error) {
        case AUDIO_ERR_NOT_INITIALIZED:
            return AUDSRV_ERR_NOT_INITIALIZED;
        case AUDIO_ERR_RPC_FAILED:
        case AUDIO_ERR_DRIVER_LOAD_FAILED:
            return AUDSRV_ERR_RPC_FAILED;
        case AUDIO_ERR_FORMAT_NOT_SUPPORTED:
            return AUDSRV_ERR_FORMAT_NOT_SUPPORTED;
        case AUDIO_ERR_OUT_OF_MEMORY:
        case AUDIO_ERR_OUT_OF_SPU_MEMORY:
            return AUDSRV_ERR_OUT_OF_MEMORY;
        case AUDIO_ERR_NO_FREE_CHANNEL:
            return AUDSRV_ERR_NO_MORE_CHANNELS;
        default:
            return AUDSRV_ERR_ARGS;
    }
}

static int fail(int audsrv_error)
{
    last_error = audsrv_error;
    return -audsrv_error;
}

/* Passes non-negative results through and translates failures, remembering the code. */
static int result_of(int audio_result)
{
    if (audio_result >= 0)
        return audio_result;
    return fail(translate_error_code(-audio_result));
}

static int ok_or_error(int audio_result)
{
    return result_of(audio_result) < 0 ? -last_error : 0;
}

static int is_channel(int channel)
{
    return channel >= 0 && channel < CHANNEL_COUNT;
}

static audio_sound_t sound_of(const audsrv_adpcm_t *adpcm)
{
    return adpcm ? (audio_sound_t)(intptr_t)adpcm->buffer : AUDIO_SOUND_INVALID;
}

int audsrv_init(void)
{
    int status = audio_init();

    if (status == -AUDIO_ERR_ALREADY_INITIALIZED)
        return 0;
    return ok_or_error(status);
}

int audsrv_quit(void)
{
    audio_shutdown();
    return 0;
}

int audsrv_set_format(struct audsrv_fmt_t *fmt)
{
    audio_stream_format_t format;

    if (!fmt)
        return fail(AUDSRV_ERR_ARGS);
    format.sample_rate_hz = fmt->freq;
    format.bits_per_sample = fmt->bits;
    format.channel_count = fmt->channels;
    return ok_or_error(audio_stream_start(&format));
}

int audsrv_wait_audio(int bytes)
{
    return ok_or_error(audio_stream_wait_until_writable(bytes, -1));
}

int audsrv_set_volume(int volume)
{
    return ok_or_error(audio_stream_set_volume(volume));
}

int audsrv_play_audio(const char *chunk, int bytes)
{
    int total = 0;

    if (bytes < 0 || (bytes > 0 && !chunk))
        return fail(AUDSRV_ERR_ARGS);

    while (total < bytes) {
        int size = bytes - total < STREAM_WRITE_CHUNK ? bytes - total : STREAM_WRITE_CHUNK;
        int accepted;
        int status = audio_stream_wait_until_writable(size, -1);

        if (status < 0)
            return total > 0 ? total : result_of(status);
        accepted = audio_stream_queue_pcm(chunk + total, size);
        if (accepted < 0)
            return total > 0 ? total : result_of(accepted);
        if (accepted == 0)
            break;
        total += accepted;
    }
    return total;
}

int audsrv_stop_audio(void)
{
    return ok_or_error(audio_stream_stop_and_clear());
}

int audsrv_get_error(void)
{
    return last_error;
}

const char *audsrv_get_error_string(void)
{
    switch (last_error) {
        case AUDSRV_ERR_NOERROR:
            return "no error";
        case AUDSRV_ERR_NOT_INITIALIZED:
            return "audsrv is not initialized";
        case AUDSRV_ERR_RPC_FAILED:
            return "the audio driver did not respond";
        case AUDSRV_ERR_FORMAT_NOT_SUPPORTED:
            return "format not supported";
        case AUDSRV_ERR_OUT_OF_MEMORY:
            return "out of memory";
        case AUDSRV_ERR_ARGS:
            return "invalid arguments";
        case AUDSRV_ERR_NO_DISC:
            return "no disc";
        case AUDSRV_ERR_NO_MORE_CHANNELS:
            return "no free channel";
        case AUDSRV_ERR_FAILED_TO_LOAD_ADPCM:
            return "failed to load the sample";
        case AUDSRV_ERR_FAILED_TO_CREATE_SEMA:
            return "failed to create a semaphore";
        default:
            return "unknown error";
    }
}

int audsrv_available(void)
{
    return result_of(audio_stream_get_free_bytes());
}

int audsrv_queued(void)
{
    return result_of(audio_stream_get_queued_bytes());
}

static void fillbuf_adapter(void *user, int32_t queued_bytes)
{
    audsrv_callback_t callback = fillbuf_callback;

    (void)user;
    (void)queued_bytes;
    if (callback)
        callback(fillbuf_argument);
}

int audsrv_on_fillbuf(int amount, audsrv_callback_t cb, void *arg)
{
    int free_bytes;
    int queued_bytes;
    int capacity;

    if (!cb) {
        fillbuf_callback = 0;
        return ok_or_error(audio_stream_set_low_data_callback(0, 0, 0));
    }

    free_bytes = audio_stream_get_free_bytes();
    queued_bytes = audio_stream_get_queued_bytes();
    if (free_bytes < 0)
        return result_of(free_bytes);
    if (queued_bytes < 0)
        return result_of(queued_bytes);
    capacity = free_bytes + queued_bytes;
    if (amount < 0 || amount > capacity)
        return fail(AUDSRV_ERR_ARGS);

    fillbuf_callback = cb;
    fillbuf_argument = arg;
    return ok_or_error(audio_stream_set_low_data_callback(capacity - amount, fillbuf_adapter, 0));
}

int audsrv_play_cd(int track)
{
    (void)track;
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_stop_cd(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_get_cdpos(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_get_trackpos(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_get_numtracks(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_get_track_offset(int track)
{
    (void)track;
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_pause_cd(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_resume_cd(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_play_sectors(int start, int end)
{
    (void)start;
    (void)end;
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_get_cd_status(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_get_cd_type(void)
{
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_on_cdda_stop(audsrv_callback_t cb, void *arg)
{
    (void)cb;
    (void)arg;
    return fail(AUDSRV_ERR_NO_DISC);
}

int audsrv_adpcm_init(void)
{
    return ok_or_error(audio_sound_free_all());
}

int audsrv_adpcm_set_volume_and_pan(int ch, int vol, int pan)
{
    return ok_or_error(audio_channel_set_volume_and_pan(ch, vol, pan));
}

int audsrv_load_adpcm(audsrv_adpcm_t *adpcm, void *buffer, int size)
{
    const uint8_t *bytes = buffer;
    audio_sound_t sound = AUDIO_SOUND_INVALID;
    int status;

    if (!adpcm || !buffer || size < APCM_HEADER_BYTES)
        return fail(AUDSRV_ERR_ARGS);

    status = audio_sound_load_apcm(buffer, size, &sound);
    if (status == -AUDIO_ERR_BAD_SOUND_DATA)
        return fail(AUDSRV_ERR_FAILED_TO_LOAD_ADPCM);
    if (status < 0)
        return result_of(status);

    adpcm->pitch = (int)((uint32_t)bytes[APCM_PITCH_OFFSET] | ((uint32_t)bytes[APCM_PITCH_OFFSET + 1] << 8)
                         | ((uint32_t)bytes[APCM_PITCH_OFFSET + 2] << 16) | ((uint32_t)bytes[APCM_PITCH_OFFSET + 3] << 24));
    adpcm->loop = bytes[APCM_LOOP_OFFSET];
    adpcm->channels = bytes[APCM_CHANNELS_OFFSET];
    adpcm->buffer = (void *)(intptr_t)sound;
    adpcm->size = size;
    return 0;
}

int audsrv_ch_play_adpcm(int ch, audsrv_adpcm_t *adpcm)
{
    audio_sound_t sound = sound_of(adpcm);

    if (sound == AUDIO_SOUND_INVALID)
        return fail(AUDSRV_ERR_ARGS);
    if (is_channel(ch)) {
        int status = audio_channel_play_sound(ch, sound);

        return status < 0 ? result_of(status) : ch;
    }
    return result_of(audio_play_sound(sound));
}

int audsrv_is_adpcm_playing(int ch, audsrv_adpcm_t *adpcm)
{
    audio_sound_t playing = AUDIO_SOUND_INVALID;
    audio_sound_t sound = sound_of(adpcm);

    if (!is_channel(ch) || sound == AUDIO_SOUND_INVALID)
        return 0;
    if (audio_channel_get_playing_sound(ch, &playing) < 0)
        return 0;
    return playing == sound;
}

int audsrv_free_adpcm(audsrv_adpcm_t *adpcm)
{
    audio_sound_t sound = sound_of(adpcm);
    int status;

    if (sound == AUDIO_SOUND_INVALID)
        return fail(AUDSRV_ERR_ARGS);
    status = audio_sound_free(sound);
    adpcm->buffer = 0;
    return ok_or_error(status);
}
