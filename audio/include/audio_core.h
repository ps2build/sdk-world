/**
 * @file
 * @ingroup audio_context
 * The audio library on an explicit context object.
 *
 * Every function in audio.h has a twin here, prefixed `audio_ctx_`, that takes the context as its first
 * argument and behaves the same. The audio.h functions are one-line wrappers over one default context. A C++
 * wrapper can own a context per object and release it in a destructor.
 *
 * Licensed under the Academic Free License version 2.0.
 */

#ifndef AUDIO_CORE_H
#define AUDIO_CORE_H

#include "audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup audio_context Contexts
 * @ingroup audio
 * @brief The audio library on an explicit context instead of the default one.
 *
 * Each `audio_ctx_*` function matches the audio.h function of the same name without the `ctx_` part, for
 * example audio_ctx_stream_start() and audio_stream_start(). Parameters, results and errors are as documented
 * there. A NULL context returns -AUDIO_ERR_NOT_INITIALIZED, except audio_ctx_get_version().
 * @{
 */

/** One connection to the audio driver, with its stream state and lock. */
typedef struct audio_context audio_context_t;

/** Options for audio_context_create(). */
typedef struct audio_context_options
{
    int32_t size; /**< sizeof(audio_context_options_t), so the struct can grow */
} audio_context_options_t;

/**
 * Connects to the audio driver. audio.irx must already be loaded.
 * @param options Options, or NULL for the defaults.
 * @param[out] context_out Receives the context, or NULL on failure.
 * @return AUDIO_OK, -AUDIO_ERR_ALREADY_INITIALIZED, -AUDIO_ERR_RPC_FAILED, -AUDIO_ERR_OUT_OF_MEMORY or
 *         -AUDIO_ERR_INVALID_ARGUMENT.
 * @note The driver serves one context at a time, so a second create fails while one exists.
 */
int audio_context_create(const audio_context_options_t *options, audio_context_t **context_out);

/**
 * Stops everything, frees every sound and releases the context.
 *
 * Calls in flight on other threads finish first (up to half a second) and waiters return
 * -AUDIO_ERR_INTERRUPTED. New calls on the context fail. Stop the threads that use the context before
 * destroying it.
 * @param context The context. NULL is accepted.
 * @return AUDIO_OK.
 */
int audio_context_destroy(audio_context_t *context);

/** As audio_get_version(); a NULL context reports the library version only. */
int audio_ctx_get_version(audio_context_t *context, audio_version_t *version_out);
/** As audio_set_master_volume(). */
int audio_ctx_set_master_volume(audio_context_t *context, int volume_percent);
/** As audio_get_channel_count(). */
int audio_ctx_get_channel_count(audio_context_t *context);

/** As audio_stream_start(). */
int audio_ctx_stream_start(audio_context_t *context, const audio_stream_format_t *format);
/** As audio_stream_end(). */
int audio_ctx_stream_end(audio_context_t *context);
/** As audio_stream_queue_pcm(). */
int audio_ctx_stream_queue_pcm(audio_context_t *context, const void *pcm, int32_t byte_count);
/** As audio_stream_wait_until_writable(). */
int audio_ctx_stream_wait_until_writable(audio_context_t *context, int32_t byte_count, int32_t timeout_ms);
/** As audio_stream_get_queued_bytes(). */
int audio_ctx_stream_get_queued_bytes(audio_context_t *context);
/** As audio_stream_get_free_bytes(). */
int audio_ctx_stream_get_free_bytes(audio_context_t *context);
/** As audio_stream_stop_and_clear(). */
int audio_ctx_stream_stop_and_clear(audio_context_t *context);
/** As audio_stream_pause(). */
int audio_ctx_stream_pause(audio_context_t *context);
/** As audio_stream_resume(). */
int audio_ctx_stream_resume(audio_context_t *context);
/** As audio_stream_set_volume(). */
int audio_ctx_stream_set_volume(audio_context_t *context, int volume_percent);
/** As audio_stream_set_low_data_callback(). */
int audio_ctx_stream_set_low_data_callback(audio_context_t *context, int32_t threshold_bytes, audio_low_data_fn callback, void *user);

/** As audio_sound_load_adpcm(). */
int audio_ctx_sound_load_adpcm(audio_context_t *context, const audio_sound_desc_t *desc, audio_sound_t *sound_out);
/** As audio_sound_load_apcm(). */
int audio_ctx_sound_load_apcm(audio_context_t *context, const void *file_data, int32_t file_size_bytes, audio_sound_t *sound_out);
/** As audio_sound_free(). */
int audio_ctx_sound_free(audio_context_t *context, audio_sound_t sound);
/** As audio_sound_free_all(). */
int audio_ctx_sound_free_all(audio_context_t *context);

/** As audio_channel_play_sound(). */
int audio_ctx_channel_play_sound(audio_context_t *context, int channel, audio_sound_t sound);
/** As audio_play_sound(). */
int audio_ctx_play_sound(audio_context_t *context, audio_sound_t sound);
/** As audio_channel_stop(). */
int audio_ctx_channel_stop(audio_context_t *context, int channel);
/** As audio_channel_is_playing(). */
int audio_ctx_channel_is_playing(audio_context_t *context, int channel);
/** As audio_channel_get_playing_sound(). */
int audio_ctx_channel_get_playing_sound(audio_context_t *context, int channel, audio_sound_t *sound_out);
/** As audio_channel_set_volume(). */
int audio_ctx_channel_set_volume(audio_context_t *context, int channel, int volume_percent);
/** As audio_channel_set_volume_and_pan(). */
int audio_ctx_channel_set_volume_and_pan(audio_context_t *context, int channel, int volume_percent, int pan);
/** As audio_channel_set_pitch(). */
int audio_ctx_channel_set_pitch(audio_context_t *context, int channel, float ratio);

/** As audio_effect_set_preset(). */
int audio_ctx_effect_set_preset(audio_context_t *context, int preset);
/** As audio_effect_set_depth(). */
int audio_ctx_effect_set_depth(audio_context_t *context, int depth_percent);
/** As audio_effect_set_delay_and_feedback(). */
int audio_ctx_effect_set_delay_and_feedback(audio_context_t *context, int delay, int feedback);
/** As audio_channel_set_effect_send(). */
int audio_ctx_channel_set_effect_send(audio_context_t *context, int channel, int enabled);
/** As audio_stream_set_effect_send(). */
int audio_ctx_stream_set_effect_send(audio_context_t *context, int enabled);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CORE_H */
