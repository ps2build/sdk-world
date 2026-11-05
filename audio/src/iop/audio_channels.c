#include "audio_channels.h"

static int is_valid_channel(int channel)
{
    return channel >= 0 && channel < AUDIO_CHANNEL_COUNT;
}

static void reset_settings(audio_channel_t *channel)
{
    channel->volume_percent = AUDIO_VOLUME_PERCENT_MAX;
    channel->pan = 0;
    channel->pitch_q12 = AUDIO_PITCH_Q12_UNITY;
}

static int32_t max_int(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

/* side_attenuation_percent is how much of the side survives: 100 full, 0 silent. */
int16_t audio_channels_compute_hw_volume(int volume_percent, int side_attenuation_percent)
{
    return (int16_t)(((int32_t)volume_percent * side_attenuation_percent * AUDIO_HW_VOLUME_MAX) / 10000);
}

uint16_t audio_channels_compute_hw_pitch(uint32_t clip_rate_hz, uint32_t pitch_q12)
{
    uint32_t pitch = (clip_rate_hz * pitch_q12) / AUDIO_HW_OUTPUT_RATE_HZ;

    if (pitch < 1u)
        pitch = 1u;
    if (pitch > AUDIO_HW_PITCH_MAX)
        pitch = AUDIO_HW_PITCH_MAX;
    return (uint16_t)pitch;
}

static void compute_side_volumes(const audio_channel_t *channel, int16_t *left, int16_t *right)
{
    int32_t left_attenuation = 100 - max_int(0, channel->pan);
    int32_t right_attenuation = 100 - max_int(0, -channel->pan);

    *left = audio_channels_compute_hw_volume(channel->volume_percent, left_attenuation);
    *right = audio_channels_compute_hw_volume(channel->volume_percent, right_attenuation);
}

static void apply_volume(audio_channels_t *channels, int index)
{
    const audio_channel_t *channel = &channels->channels[index];
    const audio_sound_entry_t *sound = audio_sound_table_get(channels->sounds, channel->sound);
    int16_t left;
    int16_t right;

    if (!channel->playing || !sound)
        return;
    compute_side_volumes(channel, &left, &right);
    if (sound->channel_count == 2) {
        channels->hw->set_voice_volume(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY, index, left, 0);
        channels->hw->set_voice_volume(channels->hw->context, AUDIO_CHANNEL_CORE_SECONDARY, index, 0, right);
    } else {
        channels->hw->set_voice_volume(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY, index, left, right);
    }
}

static void apply_pitch(audio_channels_t *channels, int index)
{
    const audio_channel_t *channel = &channels->channels[index];
    const audio_sound_entry_t *sound = audio_sound_table_get(channels->sounds, channel->sound);
    uint16_t pitch;

    if (!channel->playing || !sound)
        return;
    pitch = audio_channels_compute_hw_pitch(sound->sample_rate_hz, channel->pitch_q12);
    channels->hw->set_voice_pitch(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY, index, pitch);
    if (sound->channel_count == 2)
        channels->hw->set_voice_pitch(channels->hw->context, AUDIO_CHANNEL_CORE_SECONDARY, index, pitch);
}

static void stop_voices(audio_channels_t *channels, int index)
{
    const audio_sound_entry_t *sound = audio_sound_table_get(channels->sounds, channels->channels[index].sound);

    channels->hw->stop_voice(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY, index);
    if (sound && sound->channel_count == 2)
        channels->hw->stop_voice(channels->hw->context, AUDIO_CHANNEL_CORE_SECONDARY, index);
    channels->channels[index].playing = 0;
}

void audio_channels_init(audio_channels_t *channels, const audio_channel_hw_ops_t *hw, audio_sound_table_t *sounds)
{
    channels->hw = hw;
    channels->sounds = sounds;
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++) {
        reset_settings(&channels->channels[i]);
        channels->channels[i].sound = 0;
        channels->channels[i].playing = 0;
    }
}

int audio_channels_play_sound(audio_channels_t *channels, int channel, int32_t sound_handle)
{
    audio_channel_t *state;
    const audio_sound_entry_t *sound;
    uint16_t pitch;
    int16_t left;
    int16_t right;

    if (!is_valid_channel(channel))
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    sound = audio_sound_table_get(channels->sounds, sound_handle);
    if (!sound)
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;

    state = &channels->channels[channel];
    if (state->playing)
        stop_voices(channels, channel);

    state->sound = sound_handle;
    state->playing = 1;
    pitch = audio_channels_compute_hw_pitch(sound->sample_rate_hz, state->pitch_q12);
    compute_side_volumes(state, &left, &right);

    if (sound->channel_count == 2) {
        uint32_t right_address = sound->spu_address + sound->size_bytes / 2u;

        channels->hw->start_voice(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY, channel, sound->spu_address, pitch, left, 0);
        channels->hw->start_voice(channels->hw->context, AUDIO_CHANNEL_CORE_SECONDARY, channel, right_address, pitch, 0, right);
    } else {
        channels->hw->start_voice(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY, channel, sound->spu_address, pitch, left, right);
    }
    return 0;
}

int audio_channels_play_sound_on_free_channel(audio_channels_t *channels, int32_t sound_handle)
{
    if (!audio_sound_table_get(channels->sounds, sound_handle))
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;

    audio_channels_poll(channels);
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++) {
        if (channels->channels[i].playing)
            continue;
        reset_settings(&channels->channels[i]);
        if (audio_channels_play_sound(channels, i, sound_handle) < 0)
            return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
        return i;
    }
    return AUDIO_CHANNELS_ERR_NO_FREE_CHANNEL;
}

int audio_channels_stop(audio_channels_t *channels, int channel)
{
    if (!is_valid_channel(channel))
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    if (channels->channels[channel].playing)
        stop_voices(channels, channel);
    return 0;
}

void audio_channels_stop_sound_everywhere(audio_channels_t *channels, int32_t sound)
{
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++)
        if (channels->channels[i].playing && channels->channels[i].sound == sound)
            stop_voices(channels, i);
}

void audio_channels_stop_all(audio_channels_t *channels)
{
    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++)
        if (channels->channels[i].playing)
            stop_voices(channels, i);
}

void audio_channels_poll(audio_channels_t *channels)
{
    uint32_t ended = channels->hw->get_ended_voices(channels->hw->context, AUDIO_CHANNEL_CORE_PRIMARY);

    for (int i = 0; i < AUDIO_CHANNEL_COUNT; i++) {
        audio_channel_t *state = &channels->channels[i];
        const audio_sound_entry_t *sound;

        if (!state->playing || !(ended & (1u << i)))
            continue;
        sound = audio_sound_table_get(channels->sounds, state->sound);
        if (!sound || !sound->loops)
            state->playing = 0;
    }
}

int audio_channels_is_playing(audio_channels_t *channels, int channel)
{
    if (!is_valid_channel(channel))
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    audio_channels_poll(channels);
    return channels->channels[channel].playing;
}

int audio_channels_get_playing_sound(audio_channels_t *channels, int channel, int32_t *sound_out)
{
    if (!is_valid_channel(channel))
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    audio_channels_poll(channels);
    *sound_out = channels->channels[channel].playing ? channels->channels[channel].sound : 0;
    return 0;
}

int audio_channels_set_volume_and_pan(audio_channels_t *channels, int channel, int volume_percent, int pan)
{
    if (!is_valid_channel(channel) || volume_percent < 0 || volume_percent > AUDIO_VOLUME_PERCENT_MAX
        || pan < -AUDIO_PAN_MAX || pan > AUDIO_PAN_MAX)
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    channels->channels[channel].volume_percent = volume_percent;
    channels->channels[channel].pan = pan;
    apply_volume(channels, channel);
    return 0;
}

int audio_channels_set_volume(audio_channels_t *channels, int channel, int volume_percent)
{
    if (!is_valid_channel(channel))
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    return audio_channels_set_volume_and_pan(channels, channel, volume_percent, channels->channels[channel].pan);
}

int audio_channels_set_pitch(audio_channels_t *channels, int channel, uint32_t pitch_q12)
{
    if (!is_valid_channel(channel) || pitch_q12 < AUDIO_PITCH_Q12_MIN || pitch_q12 > AUDIO_PITCH_Q12_MAX)
        return AUDIO_CHANNELS_ERR_INVALID_ARGUMENT;
    channels->channels[channel].pitch_q12 = pitch_q12;
    apply_pitch(channels, channel);
    return 0;
}
