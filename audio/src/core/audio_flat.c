#include <stddef.h>

#include "audio_core.h"

/* The one context behind the audio_* functions. */
static audio_context_t *default_context;

int audio_init(void)
{
    if (default_context)
        return -AUDIO_ERR_ALREADY_INITIALIZED;
    return audio_context_create(NULL, &default_context);
}

int audio_shutdown(void)
{
    audio_context_t *context = default_context;

    default_context = NULL;
    return audio_context_destroy(context);
}

int audio_get_version(audio_version_t *version_out)
{
    return audio_ctx_get_version(default_context, version_out);
}

int audio_set_master_volume(int volume_percent)
{
    return audio_ctx_set_master_volume(default_context, volume_percent);
}

int audio_get_channel_count(void)
{
    return audio_ctx_get_channel_count(default_context);
}

int audio_stream_start(const audio_stream_format_t *format)
{
    return audio_ctx_stream_start(default_context, format);
}

int audio_stream_end(void)
{
    return audio_ctx_stream_end(default_context);
}

int audio_stream_queue_pcm(const void *pcm, int32_t byte_count)
{
    return audio_ctx_stream_queue_pcm(default_context, pcm, byte_count);
}

int audio_stream_wait_until_writable(int32_t byte_count, int32_t timeout_ms)
{
    return audio_ctx_stream_wait_until_writable(default_context, byte_count, timeout_ms);
}

int audio_stream_get_queued_bytes(void)
{
    return audio_ctx_stream_get_queued_bytes(default_context);
}

int audio_stream_get_free_bytes(void)
{
    return audio_ctx_stream_get_free_bytes(default_context);
}

int audio_stream_stop_and_clear(void)
{
    return audio_ctx_stream_stop_and_clear(default_context);
}

int audio_stream_pause(void)
{
    return audio_ctx_stream_pause(default_context);
}

int audio_stream_resume(void)
{
    return audio_ctx_stream_resume(default_context);
}

int audio_stream_set_volume(int volume_percent)
{
    return audio_ctx_stream_set_volume(default_context, volume_percent);
}

int audio_stream_set_low_data_callback(int32_t threshold_bytes, audio_low_data_fn callback, void *user)
{
    return audio_ctx_stream_set_low_data_callback(default_context, threshold_bytes, callback, user);
}

int audio_sound_load_adpcm(const audio_sound_desc_t *desc, audio_sound_t *sound_out)
{
    return audio_ctx_sound_load_adpcm(default_context, desc, sound_out);
}

int audio_sound_load_apcm(const void *file_data, int32_t file_size_bytes, audio_sound_t *sound_out)
{
    return audio_ctx_sound_load_apcm(default_context, file_data, file_size_bytes, sound_out);
}

int audio_sound_free(audio_sound_t sound)
{
    return audio_ctx_sound_free(default_context, sound);
}

int audio_sound_free_all(void)
{
    return audio_ctx_sound_free_all(default_context);
}

int audio_channel_play_sound(int channel, audio_sound_t sound)
{
    return audio_ctx_channel_play_sound(default_context, channel, sound);
}

int audio_play_sound(audio_sound_t sound)
{
    return audio_ctx_play_sound(default_context, sound);
}

int audio_channel_stop(int channel)
{
    return audio_ctx_channel_stop(default_context, channel);
}

int audio_channel_is_playing(int channel)
{
    return audio_ctx_channel_is_playing(default_context, channel);
}

int audio_channel_get_playing_sound(int channel, audio_sound_t *sound_out)
{
    return audio_ctx_channel_get_playing_sound(default_context, channel, sound_out);
}

int audio_channel_set_volume(int channel, int volume_percent)
{
    return audio_ctx_channel_set_volume(default_context, channel, volume_percent);
}

int audio_channel_set_volume_and_pan(int channel, int volume_percent, int pan)
{
    return audio_ctx_channel_set_volume_and_pan(default_context, channel, volume_percent, pan);
}

int audio_channel_set_pitch(int channel, float ratio)
{
    return audio_ctx_channel_set_pitch(default_context, channel, ratio);
}

int audio_effect_set_preset(int preset)
{
    return audio_ctx_effect_set_preset(default_context, preset);
}

int audio_effect_set_depth(int depth_percent)
{
    return audio_ctx_effect_set_depth(default_context, depth_percent);
}

int audio_effect_set_delay_and_feedback(int delay, int feedback)
{
    return audio_ctx_effect_set_delay_and_feedback(default_context, delay, feedback);
}

int audio_channel_set_effect_send(int channel, int enabled)
{
    return audio_ctx_channel_set_effect_send(default_context, channel, enabled);
}

int audio_stream_set_effect_send(int enabled)
{
    return audio_ctx_stream_set_effect_send(default_context, enabled);
}
