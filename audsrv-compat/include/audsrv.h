/**
 * @file
 * @ingroup audsrv_compat
 * audsrv source compatibility for the audio library.
 *
 * Code written against the audsrv API builds and runs on top of audio without changes. Streaming maps onto the
 * audio stream, samples onto audio sounds and channels. CD audio is not supported and reports
 * AUDSRV_ERR_NO_DISC. @ref audsrv_migration lists each call with its audio equivalent and the differences.
 *
 * Licensed under the Academic Free License version 2.0.
 */

#ifndef AUDSRV_H
#define AUDSRV_H

/** Id of the audsrv IOP module in the original API. */
#define AUDSRV_IRX 0x870884e

#define MIN_VOLUME 0   /**< Lowest volume in percent. */
#define MAX_VOLUME 100 /**< Highest volume in percent. */

/**
 * @defgroup audsrv_errors Error codes
 * @ingroup audsrv_compat
 * @brief Functions return them negated; audsrv_get_error() returns the last one as a positive value.
 * @{
 */
#define AUDSRV_ERR_NOERROR               0x0000 /**< No error. */
#define AUDSRV_ERR_NOT_INITIALIZED       0x0001 /**< audsrv_init() has not been called, or audsrv_quit() has. */
#define AUDSRV_ERR_RPC_FAILED            0x0002 /**< The audio driver did not answer. */
#define AUDSRV_ERR_FORMAT_NOT_SUPPORTED  0x0003 /**< The stream format is not supported. */
#define AUDSRV_ERR_OUT_OF_MEMORY         0x0004 /**< Out of memory or SPU2 memory. */
#define AUDSRV_ERR_ARGS                  0x0005 /**< Invalid arguments, or a call the stream state does not allow. */
#define AUDSRV_ERR_NO_DISC               0x0006 /**< Returned by every CD call; CD audio is not supported. */
#define AUDSRV_ERR_NO_MORE_CHANNELS      0x0007 /**< No free channel for audsrv_ch_play_adpcm(). */
#define AUDSRV_ERR_FAILED_TO_LOAD_ADPCM  0x0010 /**< The sample could not be loaded. */
#define AUDSRV_ERR_FAILED_TO_CREATE_SEMA 0x0011 /**< Reserved; never returned. */
/** @} */

/** Stream format: output frequency in Hz, bits per sample (8 or 16), channels (1 or 2). */
typedef struct audsrv_fmt_t
{
    int freq;     /**< 8000 to 48000 */
    int bits;     /**< 8 or 16 */
    int channels; /**< 1 or 2 */
} audsrv_fmt_t;

/** A loaded ADPCM sample. Treat it as opaque: audsrv_load_adpcm() fills it and the other calls read it. */
typedef struct audsrv_adpcm_t
{
    int pitch;     /**< hardware pitch value from the file header */
    int loop;      /**< 1 if the sample loops */
    int channels;  /**< 1 or 2 */
    void *buffer;  /**< the audio library sound handle, 0 when not loaded */
    int size;      /**< size of the file image that was loaded */
} audsrv_adpcm_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Callback type for audsrv_on_fillbuf() and audsrv_on_cdda_stop(). */
typedef int (*audsrv_callback_t)(void *arg);

/**
 * @defgroup audsrv_stream Stream
 * @ingroup audsrv_compat
 * @{
 */

/**
 * Starts the audio system. Calling it again is not an error.
 * @return 0 on success, or a negative error.
 */
int audsrv_init(void);

/**
 * Stops the audio system.
 * @return 0.
 */
int audsrv_quit(void);

/**
 * Sets the stream format and restarts the stream. The volume is kept.
 * @param fmt The format.
 * @return 0 on success, or a negative error.
 */
int audsrv_set_format(struct audsrv_fmt_t *fmt);

/**
 * Blocks until bytes can be queued.
 * @param bytes Size of the next chunk.
 * @return 0, or a negative error (also when bytes is larger than the stream buffer).
 */
int audsrv_wait_audio(int bytes);

/**
 * Sets the stream volume. The volume of sample channels is separate.
 * @param volume Percent, 0 to 100.
 * @return 0 on success, or a negative error.
 */
int audsrv_set_volume(int volume);

/**
 * Queues audio in the stream format, waiting for space as needed so that all of it is accepted.
 * @param chunk The data.
 * @param bytes Its size.
 * @return The bytes queued, or a negative error when none were.
 */
int audsrv_play_audio(const char *chunk, int bytes);

/**
 * Stops the stream and discards queued data. The stream can be used again without audsrv_set_format().
 * @return 0 on success, or a negative error.
 */
int audsrv_stop_audio(void);

/** @return The last error code raised by any call, as a positive value; 0 if none. */
int audsrv_get_error(void);

/** @return Text for audsrv_get_error(). */
const char *audsrv_get_error_string(void);

/** @return Bytes of stream space free, or a negative error. */
int audsrv_available(void);

/** @return Bytes queued in the stream, or a negative error. */
int audsrv_queued(void);

/**
 * Calls cb(arg) when at least amount bytes of stream space are free, once each time the queue drains
 * to that point. A NULL cb unregisters. The stream must be started. Returns 0, or -AUDSRV_ERR_ARGS if
 * amount is larger than the stream buffer.
 * @param amount Free space that triggers the call.
 * @param cb The function, or NULL.
 * @param arg Passed to cb.
 * @return 0 or a negative error.
 */
int audsrv_on_fillbuf(int amount, audsrv_callback_t cb, void *arg);

/** @} */

/**
 * @defgroup audsrv_cd CD audio
 * @ingroup audsrv_compat
 * @brief Not supported. Every function returns -AUDSRV_ERR_NO_DISC.
 * @{
 */
/** Plays a CD audio track. */
int audsrv_play_cd(int track);
/** Stops CD audio. */
int audsrv_stop_cd(void);
/** Position on the disc. */
int audsrv_get_cdpos(void);
/** Position within the current track. */
int audsrv_get_trackpos(void);
/** Number of tracks. */
int audsrv_get_numtracks(void);
/** Start of a track. */
int audsrv_get_track_offset(int track);
/** Pauses CD audio. */
int audsrv_pause_cd(void);
/** Resumes CD audio. */
int audsrv_resume_cd(void);
/** Plays a range of sectors. */
int audsrv_play_sectors(int start, int end);
/** Drive status. */
int audsrv_get_cd_status(void);
/** Disc type. */
int audsrv_get_cd_type(void);
/** Registers a function called when CD audio stops. */
int audsrv_on_cdda_stop(audsrv_callback_t cb, void *arg);
/** @} */

/**
 * @defgroup audsrv_samples ADPCM samples
 * @ingroup audsrv_compat
 * @brief Sounds played on channels 0 to 23.
 * @{
 */

/**
 * Frees all loaded samples and stops every voice. The stream is not touched. Can be called repeatedly.
 * @return 0 or a negative error.
 */
int audsrv_adpcm_init(void);

/**
 * Sets volume and pan of a channel. Both persist across plays and can be set before the first play.
 * @param ch Channel 0 to 23.
 * @param vol Volume in percent, 0 to 100.
 * @param pan -100 (left) to 100 (right), 0 centered.
 * @return 0 on success, or a negative error.
 */
int audsrv_adpcm_set_volume_and_pan(int ch, int vol, int pan);

/** Sets the volume of a channel and the pan to center (pan 0). */
#define audsrv_adpcm_set_volume(ch, vol) audsrv_adpcm_set_volume_and_pan(ch, vol, 0)

/**
 * Loads an APCM sample image. The buffer can be freed on return.
 * @param adpcm Receives the sample descriptor.
 * @param buffer The whole file.
 * @param size Its size.
 * @return 0, or a negative error (-AUDSRV_ERR_FAILED_TO_LOAD_ADPCM for data that is not a valid image).
 */
int audsrv_load_adpcm(audsrv_adpcm_t *adpcm, void *buffer, int size);

/**
 * Plays a loaded sample on channel ch (0-23), replacing what is playing there. Any other value of ch plays it
 * on the first free channel.
 * @param ch The channel, or -1 for any free channel.
 * @param adpcm The sample.
 * @return The channel number, or a negative error (-AUDSRV_ERR_NO_MORE_CHANNELS when none is free).
 */
int audsrv_ch_play_adpcm(int ch, audsrv_adpcm_t *adpcm);

/** Plays a sample on the first free channel. */
#define audsrv_play_adpcm(adpcm) audsrv_ch_play_adpcm(-1, adpcm)

/**
 * Whether this sample is playing on channel ch.
 * @return 1 if it is, else 0.
 */
int audsrv_is_adpcm_playing(int ch, audsrv_adpcm_t *adpcm);

/**
 * Frees a sample and clears its descriptor.
 * @return 0 or a negative error.
 */
int audsrv_free_adpcm(audsrv_adpcm_t *adpcm);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
