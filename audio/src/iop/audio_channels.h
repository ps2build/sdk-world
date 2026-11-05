#ifndef AUDIO_CHANNELS_H
#define AUDIO_CHANNELS_H

#include <stdint.h>

#include "audio_sounds.h"

#define AUDIO_CHANNEL_COUNT        24
#define AUDIO_VOLUME_PERCENT_MAX   100
#define AUDIO_PAN_MAX              100
#define AUDIO_PITCH_Q12_UNITY      4096u
#define AUDIO_PITCH_Q12_MIN        256u
#define AUDIO_PITCH_Q12_MAX        16384u
#define AUDIO_HW_VOLUME_MAX        0x3FFF
#define AUDIO_HW_PITCH_UNITY       0x1000u
#define AUDIO_HW_PITCH_MAX         0x3FFFu
#define AUDIO_HW_OUTPUT_RATE_HZ    48000u

#define AUDIO_CHANNEL_CORE_PRIMARY   1
#define AUDIO_CHANNEL_CORE_SECONDARY 0

/* Negative results, matching the public AUDIO_ERR_* codes. */
#define AUDIO_CHANNELS_ERR_INVALID_ARGUMENT  (-6)
#define AUDIO_CHANNELS_ERR_NO_FREE_CHANNEL   (-7)

/* What the channel layer needs from the hardware. The IOP driver supplies libsd-backed functions; tests supply a mock. */
typedef struct audio_channel_hw_ops
{
    void (*start_voice)(void *context, int core, int voice, uint32_t spu_address, uint16_t pitch, int16_t volume_left, int16_t volume_right);
    void (*stop_voice)(void *context, int core, int voice);
    void (*set_voice_volume)(void *context, int core, int voice, int16_t volume_left, int16_t volume_right);
    void (*set_voice_pitch)(void *context, int core, int voice, uint16_t pitch);
    /* Bit n set: voice n of the core has reached the end of a non-looping clip. */
    uint32_t (*get_ended_voices)(void *context, int core);
    void *context;
} audio_channel_hw_ops_t;

typedef struct audio_channel
{
    int32_t volume_percent;
    int32_t pan;
    uint32_t pitch_q12;
    int32_t sound;
    uint8_t playing;
} audio_channel_t;

typedef struct audio_channels
{
    audio_channel_t channels[AUDIO_CHANNEL_COUNT];
    const audio_channel_hw_ops_t *hw;
    audio_sound_table_t *sounds;
} audio_channels_t;

/* Pure conversions, exposed for tests. */
int16_t audio_channels_compute_hw_volume(int volume_percent, int side_attenuation_percent);
uint16_t audio_channels_compute_hw_pitch(uint32_t clip_rate_hz, uint32_t pitch_q12);

void audio_channels_init(audio_channels_t *channels, const audio_channel_hw_ops_t *hw, audio_sound_table_t *sounds);

/* Keeps the channel's volume, pan and pitch. Replaces anything already playing there. */
int audio_channels_play_sound(audio_channels_t *channels, int channel, int32_t sound);

/* Resets the first idle channel to defaults, plays there, returns its number or AUDIO_CHANNELS_ERR_NO_FREE_CHANNEL. */
int audio_channels_play_sound_on_free_channel(audio_channels_t *channels, int32_t sound);

int audio_channels_stop(audio_channels_t *channels, int channel);
void audio_channels_stop_sound_everywhere(audio_channels_t *channels, int32_t sound);
void audio_channels_stop_all(audio_channels_t *channels);

/* Refreshes the playing flags from the hardware end-of-clip state. */
void audio_channels_poll(audio_channels_t *channels);

int audio_channels_is_playing(audio_channels_t *channels, int channel);
int audio_channels_get_playing_sound(audio_channels_t *channels, int channel, int32_t *sound_out);

int audio_channels_set_volume_and_pan(audio_channels_t *channels, int channel, int volume_percent, int pan);
int audio_channels_set_volume(audio_channels_t *channels, int channel, int volume_percent);
int audio_channels_set_pitch(audio_channels_t *channels, int channel, uint32_t pitch_q12);

#endif
