/**
 * @file
 * @ingroup audio
 * PS2 audio library: PCM streaming and preloaded ADPCM sounds on mixer channels.
 *
 * Every function here works on one default context, created by audio_init() and released by
 * audio_shutdown(). audio_core.h has the same functions on an explicit context.
 *
 * Return values: a value >= 0 is success (some functions return a count or a channel number). A failure is
 * the negated error code, for example -AUDIO_ERR_INVALID_ARGUMENT. See @ref audio_concepts for how the parts
 * fit together.
 *
 * Licensed under the Academic Free License version 2.0.
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup audio_errors Error codes
 * @ingroup audio
 * @brief Positive constants; functions return them negated.
 * @{
 */

/** Success. Functions that return a count or a channel number return it as a value >= 0. */
#define AUDIO_OK 0

#define AUDIO_ERR_NOT_INITIALIZED      1  /**< audio_init() has not been called, or audio_shutdown() has. */
#define AUDIO_ERR_ALREADY_INITIALIZED  2  /**< The audio system is already running. */
#define AUDIO_ERR_RPC_FAILED           3  /**< The driver did not answer. */
#define AUDIO_ERR_FORMAT_NOT_SUPPORTED 4  /**< Sample rate, bit depth or channel count outside the supported set. */
#define AUDIO_ERR_OUT_OF_MEMORY        5  /**< No memory, or the driver's sound table is full. */
#define AUDIO_ERR_INVALID_ARGUMENT     6  /**< A null pointer, an out-of-range value, or a handle that is not live. */
#define AUDIO_ERR_NO_FREE_CHANNEL      7  /**< audio_play_sound() found every channel busy. */
#define AUDIO_ERR_OUT_OF_SPU_MEMORY    8  /**< No free SPU2 RAM block large enough for the sound. */
#define AUDIO_ERR_BAD_SOUND_DATA       9  /**< The sound data failed validation. */
#define AUDIO_ERR_STREAM_NOT_STARTED   10 /**< The call needs a started stream. */
#define AUDIO_ERR_STREAM_PAUSED        11 /**< Waiting would never finish because the stream is paused. */
#define AUDIO_ERR_TIMEOUT              12 /**< A wait timed out. */
#define AUDIO_ERR_INTERRUPTED          13 /**< A wait ended because the stream or the audio system was shut down. */
#define AUDIO_ERR_DRIVER_LOAD_FAILED   14 /**< The driver module could not be loaded. */
#define AUDIO_ERR_BUSY                 15 /**< The resource is in use. */

/** @} */

/**
 * @defgroup audio_effect_presets Effect presets
 * @ingroup audio_effect
 * @brief Values for audio_effect_set_preset().
 * @{
 */
#define AUDIO_EFFECT_OFF           0 /**< No effect. */
#define AUDIO_EFFECT_ROOM          1 /**< Reverb: small room. */
#define AUDIO_EFFECT_STUDIO_SMALL  2 /**< Reverb: small studio. */
#define AUDIO_EFFECT_STUDIO_MEDIUM 3 /**< Reverb: medium studio. */
#define AUDIO_EFFECT_STUDIO_LARGE  4 /**< Reverb: large studio. */
#define AUDIO_EFFECT_HALL          5 /**< Reverb: hall. */
#define AUDIO_EFFECT_SPACE         6 /**< Reverb: large open space. */
#define AUDIO_EFFECT_ECHO          7 /**< Echo with feedback; uses audio_effect_set_delay_and_feedback(). */
#define AUDIO_EFFECT_DELAY         8 /**< A single delayed repeat; uses audio_effect_set_delay_and_feedback(). */
#define AUDIO_EFFECT_PIPE          9 /**< Reverb: pipe. */
/** @} */

/** Handle to a sound loaded into SPU2 memory. Valid handles are >= 1; a freed handle never becomes valid again. */
typedef int32_t audio_sound_t;

/** The value that is never a valid sound handle. */
#define AUDIO_SOUND_INVALID 0

/** Format of the PCM data given to audio_stream_queue_pcm(). */
typedef struct audio_stream_format
{
    int32_t sample_rate_hz;  /**< 8000 to 48000 */
    int32_t bits_per_sample; /**< 8 (signed) or 16 (signed little-endian) */
    int32_t channel_count;   /**< 1 or 2; stereo data is interleaved */
} audio_stream_format_t;

/** Raw PS-ADPCM data for audio_sound_load_adpcm(). */
typedef struct audio_sound_desc
{
    const void *adpcm_data;   /**< the ADPCM blocks, without a file header */
    int32_t adpcm_size_bytes; /**< a multiple of 16; stereo is all left blocks, then all right blocks */
    int32_t sample_rate_hz;   /**< native rate of the sound, 4000 to 48000 */
    int32_t channel_count;    /**< 1 or 2 */
    int32_t loops;            /**< 1 to play until stopped, 0 to play once */
} audio_sound_desc_t;

/** Library and driver versions, filled by audio_get_version(). */
typedef struct audio_version
{
    uint16_t library_major; /**< library version, major */
    uint16_t library_minor; /**< library version, minor */
    uint16_t library_patch; /**< library version, patch */
    uint16_t driver_major;  /**< driver version, major; 0 when not initialized */
    uint16_t driver_minor;  /**< driver version, minor */
    uint16_t driver_patch;  /**< driver version, patch */
} audio_version_t;

/**
 * Callback for audio_stream_set_low_data_callback().
 * @param user Pointer given at registration.
 * @param queued_source_bytes Bytes still queued when the callback runs.
 */
typedef void (*audio_low_data_fn)(void *user, int32_t queued_source_bytes);

/**
 * @defgroup audio_lifecycle Lifecycle and global settings
 * @ingroup audio
 * @{
 */

/**
 * Connects to the audio driver. audio.irx must already be loaded.
 * @return AUDIO_OK, -AUDIO_ERR_ALREADY_INITIALIZED, -AUDIO_ERR_RPC_FAILED or -AUDIO_ERR_OUT_OF_MEMORY.
 * @note Blocks until the driver answers, up to about three seconds.
 */
int audio_init(void);

/**
 * Stops everything, frees every sound and disconnects. Safe to call when not initialized.
 *
 * Threads blocked in audio_stream_wait_until_writable() return -AUDIO_ERR_INTERRUPTED. Stop the threads
 * that use the library before calling this.
 * @return AUDIO_OK.
 */
int audio_shutdown(void);

/**
 * Text for an error code.
 * @param error_code A code from @ref audio_errors, with either sign.
 * @return A static string. Never NULL. Works before audio_init().
 */
const char *audio_get_error_message(int error_code);

/**
 * Library and driver versions.
 * @param[out] version_out Receives the versions; the driver fields are 0 when not initialized.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_get_version(audio_version_t *version_out);

/**
 * Scales the final mix of the stream and all channels.
 * @param volume_percent 0 (silent) to 100 (full). Values outside the range are rejected.
 * @return AUDIO_OK, -AUDIO_ERR_NOT_INITIALIZED or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_set_master_volume(int volume_percent);

/**
 * Number of mixer channels.
 * @return 24, or -AUDIO_ERR_NOT_INITIALIZED.
 */
int audio_get_channel_count(void);

/** @} */

/**
 * @defgroup audio_stream Stream
 * @ingroup audio
 * @brief PCM playback for music and video audio.
 *
 * One stream per context. Data is queued in the format given to audio_stream_start() and the library converts
 * it to the 48 kHz stereo 16-bit layout the SPU2 plays. All byte counts are in the source format. A stream that
 * runs out of data plays silence and continues when more arrives; that is not an error.
 * @{
 */

/**
 * Starts a stream, or restarts it with a new format and discards queued data.
 * @param format Sample rate 8000-48000 Hz, 8 or 16 bits, 1 or 2 channels.
 * @return AUDIO_OK, -AUDIO_ERR_FORMAT_NOT_SUPPORTED, -AUDIO_ERR_INVALID_ARGUMENT or -AUDIO_ERR_NOT_INITIALIZED.
 * @note Keeps the stream volume. The stream starts in the playing state.
 */
int audio_stream_start(const audio_stream_format_t *format);

/**
 * Ends the stream and discards queued data. Threads waiting for space return -AUDIO_ERR_INTERRUPTED.
 * @return AUDIO_OK, also when no stream was started.
 */
int audio_stream_end(void);

/**
 * Adds PCM data to the stream. Copies the data before returning and never waits for space.
 * @param pcm Data in the stream format. No alignment needed.
 * @param byte_count Bytes to queue. A trailing partial frame is not accepted.
 * @return Bytes accepted, from 0 up to byte_count (less than byte_count when the stream is full), or
 *         -AUDIO_ERR_STREAM_NOT_STARTED, -AUDIO_ERR_INVALID_ARGUMENT, -AUDIO_ERR_NOT_INITIALIZED.
 * @note After audio_stream_wait_until_writable() returned AUDIO_OK for n bytes, queuing up to n bytes from the
 *       same thread is accepted in full.
 */
int audio_stream_queue_pcm(const void *pcm, int32_t byte_count);

/**
 * Blocks until at least byte_count bytes can be queued.
 * @param byte_count 1 up to the capacity of the stream in source bytes.
 * @param timeout_ms Milliseconds to wait: 0 polls once, -1 waits forever.
 * @return AUDIO_OK, -AUDIO_ERR_TIMEOUT, -AUDIO_ERR_STREAM_PAUSED (space is short and the stream is paused),
 *         -AUDIO_ERR_INTERRUPTED (the stream ended or the audio system shut down while waiting),
 *         -AUDIO_ERR_INVALID_ARGUMENT (byte_count larger than the stream holds), -AUDIO_ERR_STREAM_NOT_STARTED.
 * @note Other threads can keep using the library while one waits here.
 */
int audio_stream_wait_until_writable(int32_t byte_count, int32_t timeout_ms);

/**
 * Bytes queued and not yet played, rounded up.
 * @return The count, or -AUDIO_ERR_STREAM_NOT_STARTED.
 */
int audio_stream_get_queued_bytes(void);

/**
 * Bytes audio_stream_queue_pcm() accepts right now, rounded down.
 * @return The count, or -AUDIO_ERR_STREAM_NOT_STARTED.
 */
int audio_stream_get_free_bytes(void);

/**
 * Stops playback and discards queued data. The stream stays started with its format, in the playing state.
 * @return AUDIO_OK, also when no stream was started.
 */
int audio_stream_stop_and_clear(void);

/**
 * Silences the output and keeps the queued data. Queuing continues until the stream is full.
 * @return AUDIO_OK, or -AUDIO_ERR_STREAM_NOT_STARTED.
 */
int audio_stream_pause(void);

/**
 * Continues from where audio_stream_pause() stopped.
 * @return AUDIO_OK, or -AUDIO_ERR_STREAM_NOT_STARTED.
 */
int audio_stream_resume(void);

/**
 * Volume of the stream alone. It persists across restarts and works before the stream is started.
 * @param volume_percent 0 to 100.
 * @return AUDIO_OK, -AUDIO_ERR_INVALID_ARGUMENT or -AUDIO_ERR_NOT_INITIALIZED.
 */
int audio_stream_set_volume(int volume_percent);

/**
 * Registers a function called when the queued data falls to a threshold.
 *
 * The callback runs on a thread owned by the library, once each time the queue drains to the threshold. It
 * runs again only after the level has risen above the threshold. Registering does not call it for a level
 * that is already low. The callback may queue data; it must not end or restart the stream. The threshold is
 * kept when the stream is restarted.
 * @param threshold_bytes 0 up to the capacity of the stream, in source bytes; 0 calls back only when empty.
 * @param callback The function, or NULL to unregister.
 * @param user Passed to the callback unchanged.
 * @return AUDIO_OK, -AUDIO_ERR_STREAM_NOT_STARTED, -AUDIO_ERR_INVALID_ARGUMENT or -AUDIO_ERR_NOT_INITIALIZED.
 */
int audio_stream_set_low_data_callback(int32_t threshold_bytes, audio_low_data_fn callback, void *user);

/** @} */

/**
 * @defgroup audio_sound Sounds
 * @ingroup audio
 * @brief ADPCM sounds kept in SPU2 memory.
 *
 * A sound is loaded once and identified by a handle. It plays on a channel, and one sound can play on several
 * channels at once.
 * @{
 */

/**
 * Uploads raw PS-ADPCM to SPU2 memory. The data is copied; the buffer can be freed on return.
 * @param desc The data and its format.
 * @param[out] sound_out Receives the handle, or AUDIO_SOUND_INVALID on failure.
 * @return AUDIO_OK, -AUDIO_ERR_INVALID_ARGUMENT, -AUDIO_ERR_OUT_OF_SPU_MEMORY, -AUDIO_ERR_OUT_OF_MEMORY,
 *         -AUDIO_ERR_RPC_FAILED or -AUDIO_ERR_NOT_INITIALIZED.
 * @note Blocks until the upload finishes. Loop flags in the data are rewritten according to desc->loops.
 */
int audio_sound_load_adpcm(const audio_sound_desc_t *desc, audio_sound_t *sound_out);

/**
 * Uploads the image of an APCM file (the .adp files Open PS2 Loader uses). The sample rate is derived from the
 * header pitch field (44100 Hz when the field is absent or out of range); the channel count and the loop flag
 * come from the header too. A looping sound ends its loop before the last, silent block of each channel.
 * @param file_data The whole file.
 * @param file_size_bytes Its size.
 * @param[out] sound_out Receives the handle, or AUDIO_SOUND_INVALID on failure.
 * @return As audio_sound_load_adpcm(), with -AUDIO_ERR_BAD_SOUND_DATA for a file that is not a valid APCM image.
 */
int audio_sound_load_apcm(const void *file_data, int32_t file_size_bytes, audio_sound_t *sound_out);

/**
 * Stops the sound on every channel and frees its SPU2 memory. The handle is invalid afterwards.
 * @return AUDIO_OK, or -AUDIO_ERR_INVALID_ARGUMENT for an unknown or already freed handle.
 */
int audio_sound_free(audio_sound_t sound);

/**
 * Stops all channels and frees every sound. Channel volume, pan and pitch are kept.
 * @return AUDIO_OK, also with no sounds loaded.
 */
int audio_sound_free_all(void);

/** @} */

/**
 * @defgroup audio_channel Channels
 * @ingroup audio
 * @brief Mixer channels 0 to 23 that play sounds.
 *
 * A channel keeps its volume, pan and pitch until they are changed, including across sounds and before the
 * first sound plays. Commands that return AUDIO_OK without a reply from the driver (play, stop and the
 * volume, pan and pitch setters) are validated by the library, sent, and executed by the driver in the order
 * they were sent; a later call such as audio_channel_is_playing() sees their effect.
 * @{
 */

/**
 * Plays a sound on a channel, replacing whatever was playing there. The channel keeps its volume, pan and pitch.
 * @param channel 0 to 23.
 * @param sound A handle from a load function.
 * @return AUDIO_OK, or -AUDIO_ERR_INVALID_ARGUMENT for a channel out of range or a handle that is not live.
 */
int audio_channel_play_sound(int channel, audio_sound_t sound);

/**
 * Plays a sound on the first idle channel, searching from channel 0. The channel is reset to volume 100,
 * centered, normal pitch before the sound starts.
 * @param sound A handle from a load function.
 * @return The channel number 0-23, -AUDIO_ERR_NO_FREE_CHANNEL (nothing is interrupted),
 *         -AUDIO_ERR_INVALID_ARGUMENT or -AUDIO_ERR_NOT_INITIALIZED.
 * @note Waits for the driver's answer, since the channel number is the result.
 */
int audio_play_sound(audio_sound_t sound);

/**
 * Stops the sound on a channel. Volume, pan and pitch are kept.
 * @param channel 0 to 23.
 * @return AUDIO_OK, also for an idle channel, or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_channel_stop(int channel);

/**
 * Whether a channel is playing. A sound that finished counts as not playing.
 * @param channel 0 to 23.
 * @return 1 playing, 0 idle, or a negative error.
 */
int audio_channel_is_playing(int channel);

/**
 * The sound playing on a channel.
 * @param channel 0 to 23.
 * @param[out] sound_out Receives the handle, or AUDIO_SOUND_INVALID when the channel is idle.
 * @return AUDIO_OK or a negative error.
 */
int audio_channel_get_playing_sound(int channel, audio_sound_t *sound_out);

/**
 * Sets the channel volume and leaves pan and pitch unchanged. Applies at once to a playing sound and persists.
 * @param channel 0 to 23.
 * @param volume_percent 0 to 100.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_channel_set_volume(int channel, int volume_percent);

/**
 * Sets volume and pan together. Applies at once to a playing sound and persists.
 * @param channel 0 to 23.
 * @param volume_percent 0 to 100.
 * @param pan -100 (left) to 100 (right), 0 centered; it works as a balance for stereo sounds.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_channel_set_volume_and_pan(int channel, int volume_percent, int pan);

/**
 * Sets the playback speed as a ratio of the sound's own rate. Applies at once to a playing sound and persists.
 * @param channel 0 to 23.
 * @param ratio 0.0625 to 4.0; 1.0 plays at the sound's rate.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_channel_set_pitch(int channel, float ratio);

/** @} */

/**
 * @defgroup audio_effect Effects
 * @ingroup audio
 * @brief The SPU2 reverb and delay.
 *
 * One effect is active at a time. It is a mix bus that sources choose to send to: nothing is affected until
 * audio_channel_set_effect_send() or audio_stream_set_effect_send() turns a source on. A source keeps its
 * normal output and the effect is added to it, so the depth sets how much effect is heard. The send is on or
 * off; there is no per-source amount.
 *
 * The effect needs working memory in SPU2 RAM. Sounds that are loaded when a preset is selected may occupy
 * the area that the effect on the stream uses, so select the preset before loading sounds, or free sounds
 * first. audio_shutdown() turns the effect off and cuts the tail.
 * @{
 */

/**
 * Selects the effect and clears its working memory. Depth, delay and feedback keep their values.
 * @param preset One of @ref audio_effect_presets. AUDIO_EFFECT_OFF turns the effect off.
 * @return AUDIO_OK, -AUDIO_ERR_INVALID_ARGUMENT or -AUDIO_ERR_OUT_OF_SPU_MEMORY when loaded sounds occupy the
 *         working memory; the previous preset stays active in that case.
 * @note Blocks while the working memory is cleared, up to a few milliseconds.
 */
int audio_effect_set_preset(int preset);

/**
 * Sets how loud the effect is in the output. The default is 50.
 * @param depth_percent 0 to 100.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_effect_set_depth(int depth_percent);

/**
 * Sets the delay time and feedback of AUDIO_EFFECT_ECHO and AUDIO_EFFECT_DELAY. Other presets keep the values
 * for later and ignore them. The defaults are 128 and 128. The scale is not calibrated to milliseconds.
 * @param delay 0 to 255; larger is a longer delay.
 * @param feedback 0 to 255; larger repeats for longer. Only AUDIO_EFFECT_ECHO uses it; AUDIO_EFFECT_DELAY plays
 *        a single repeat.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_effect_set_delay_and_feedback(int delay, int feedback);

/**
 * Sends a channel to the effect, or stops doing so. The setting persists across sounds.
 * @param channel 0 to 23.
 * @param enabled Non-zero to send.
 * @return AUDIO_OK or -AUDIO_ERR_INVALID_ARGUMENT.
 */
int audio_channel_set_effect_send(int channel, int enabled);

/**
 * Sends the stream to the effect, or stops doing so.
 * @param enabled Non-zero to send.
 * @return AUDIO_OK.
 */
int audio_stream_set_effect_send(int enabled);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_H */
