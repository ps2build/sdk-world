#include <irx_imports.h>

#include "spu_voice.h"

#define ADSR1_INSTANT_ATTACK_FULL_SUSTAIN 0x00FF
#define ADSR2_SUSTAIN_FULL_FAST_RELEASE   0x1FC0
#define ALL_VOICES_MASK                   0xFFFFFFu
#define MIX_CORE_0                        0xFC0
#define MIX_CORE_1_WITH_CORE_0_INPUT      0xFFC

static uint16_t voice_entry(int core, int voice, int parameter)
{
    return (uint16_t)(SD_VOICE(core, voice) | parameter);
}

void spu_voice_enable_output(void)
{
    for (int core = 0; core < 2; core++) {
        sceSdSetParam((uint16_t)(core | SD_PARAM_MVOLL), AUDIO_HW_VOLUME_MAX);
        sceSdSetParam((uint16_t)(core | SD_PARAM_MVOLR), AUDIO_HW_VOLUME_MAX);
        sceSdSetSwitch((uint16_t)(core | SD_SWITCH_VMIXL), ALL_VOICES_MASK);
        sceSdSetSwitch((uint16_t)(core | SD_SWITCH_VMIXR), ALL_VOICES_MASK);
        sceSdSetParam((uint16_t)(core | SD_PARAM_MMIX), core == 0 ? MIX_CORE_0 : MIX_CORE_1_WITH_CORE_0_INPUT);
    }
}

void spu_voice_set_master_volume(int volume_percent)
{
    int16_t hardware = (int16_t)((volume_percent * AUDIO_HW_VOLUME_MAX) / 100);

    for (int core = 0; core < 2; core++) {
        sceSdSetParam((uint16_t)(core | SD_PARAM_MVOLL), (uint16_t)hardware);
        sceSdSetParam((uint16_t)(core | SD_PARAM_MVOLR), (uint16_t)hardware);
    }
}

int spu_voice_upload(const void *iop_data, uint32_t spu_address, uint32_t size_bytes)
{
    /* libsd takes the SPU2 address itself in the pointer argument, not a pointer to it. */
    if (sceSdVoiceTrans(SPU_VOICE_TRANSFER_CORE, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, (u8 *)iop_data, (u32 *)spu_address, size_bytes) < 0)
        return -1;
    sceSdVoiceTransStatus(SPU_VOICE_TRANSFER_CORE, 1);
    return 0;
}

uint16_t spu_voice_get_envelope(int core, int voice)
{
    return sceSdGetParam(voice_entry(core, voice, SD_VPARAM_ENVX));
}

uint32_t spu_voice_get_next_address(int core, int voice)
{
    return sceSdGetAddr(voice_entry(core, voice, SD_VADDR_NAX));
}

static void hw_start_voice(void *context, int core, int voice, uint32_t spu_address, uint16_t pitch, int16_t volume_left, int16_t volume_right)
{
    (void)context;
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_VOLL), (uint16_t)volume_left);
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_VOLR), (uint16_t)volume_right);
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_PITCH), pitch);
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_ADSR1), ADSR1_INSTANT_ATTACK_FULL_SUSTAIN);
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_ADSR2), ADSR2_SUSTAIN_FULL_FAST_RELEASE);
    sceSdSetAddr(voice_entry(core, voice, SD_VADDR_SSA), spu_address);
    sceSdSetSwitch((uint16_t)(core | SD_SWITCH_KON), 1u << voice);
}

static void hw_stop_voice(void *context, int core, int voice)
{
    (void)context;
    sceSdSetSwitch((uint16_t)(core | SD_SWITCH_KOFF), 1u << voice);
}

static void hw_set_voice_volume(void *context, int core, int voice, int16_t volume_left, int16_t volume_right)
{
    (void)context;
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_VOLL), (uint16_t)volume_left);
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_VOLR), (uint16_t)volume_right);
}

static void hw_set_voice_pitch(void *context, int core, int voice, uint16_t pitch)
{
    (void)context;
    sceSdSetParam(voice_entry(core, voice, SD_VPARAM_PITCH), pitch);
}

static uint32_t hw_get_ended_voices(void *context, int core)
{
    (void)context;
    return sceSdGetSwitch((uint16_t)(core | SD_SWITCH_ENDX)) & ALL_VOICES_MASK;
}

const audio_channel_hw_ops_t spu_voice_hw_ops = {
    hw_start_voice,
    hw_stop_voice,
    hw_set_voice_volume,
    hw_set_voice_pitch,
    hw_get_ended_voices,
    0,
};
