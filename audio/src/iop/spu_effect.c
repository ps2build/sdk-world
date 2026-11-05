#include <irx_imports.h>

#include "spu_effect.h"
#include "spu_voice.h"

#define MIX_INPUT_WET_BITS 0x030
#define ALL_VOICES_MASK    0xFFFFFFu

static int hw_set_mode(void *context, int core, int old_mode, int new_mode, int16_t depth_left, int16_t depth_right, int delay, int feedback)
{
    sceSdEffectAttr attribute;
    int status;

    (void)context;
    attribute.core = core;
    attribute.mode = new_mode;
    attribute.depth_L = depth_left;
    attribute.depth_R = depth_right;
    attribute.delay = delay;
    attribute.feedback = feedback;

    /* With the processor off, sceSdSetEffectAttr only loads the parameters and the work area address. */
    sceSdSetCoreAttr((uint16_t)(core | SD_CORE_EFFECT_ENABLE), 0);
    if (old_mode != new_mode && old_mode != SD_EFFECT_MODE_OFF)
        sceSdClearEffectWorkArea(core, SPU_VOICE_TRANSFER_CORE, old_mode);
    status = sceSdSetEffectAttr(core, &attribute);
    if (old_mode != new_mode && new_mode != SD_EFFECT_MODE_OFF)
        sceSdClearEffectWorkArea(core, SPU_VOICE_TRANSFER_CORE, new_mode);
    if (new_mode != SD_EFFECT_MODE_OFF)
        sceSdSetCoreAttr((uint16_t)(core | SD_CORE_EFFECT_ENABLE), 1);
#ifdef AUDIO_TRACE
    printf("audio: effect core=%d mode=%d->%d status=%d enabled=%d evol=%04x/%04x esa=%06x eea=%06x\n", core, old_mode, new_mode, status,
           (int)sceSdGetCoreAttr((uint16_t)(core | SD_CORE_EFFECT_ENABLE)), sceSdGetParam((uint16_t)(core | SD_PARAM_EVOLL)),
           sceSdGetParam((uint16_t)(core | SD_PARAM_EVOLR)), (unsigned)sceSdGetAddr((uint16_t)(core | SD_ADDR_ESA)),
           (unsigned)sceSdGetAddr((uint16_t)(core | SD_ADDR_EEA)));
#endif
    return status;
}

static void hw_set_depth(void *context, int core, int16_t depth_left, int16_t depth_right)
{
    (void)context;
    sceSdSetParam((uint16_t)(core | SD_PARAM_EVOLL), (uint16_t)depth_left);
    sceSdSetParam((uint16_t)(core | SD_PARAM_EVOLR), (uint16_t)depth_right);
}

static void hw_set_voice_sends(void *context, int core, uint32_t voice_mask)
{
    (void)context;
    sceSdSetSwitch((uint16_t)(core | SD_SWITCH_VMIXEL), voice_mask & ALL_VOICES_MASK);
    sceSdSetSwitch((uint16_t)(core | SD_SWITCH_VMIXER), voice_mask & ALL_VOICES_MASK);
#ifdef AUDIO_TRACE
    printf("audio: voice sends core=%d mask=%06x vmixel=%06x vmixer=%06x\n", core, (unsigned)voice_mask,
           (unsigned)sceSdGetSwitch((uint16_t)(core | SD_SWITCH_VMIXEL)), (unsigned)sceSdGetSwitch((uint16_t)(core | SD_SWITCH_VMIXER)));
#endif
}

static void hw_set_input_send(void *context, int core, int enabled)
{
    uint16_t mix = sceSdGetParam((uint16_t)(core | SD_PARAM_MMIX));

    (void)context;
    mix = enabled ? (uint16_t)(mix | MIX_INPUT_WET_BITS) : (uint16_t)(mix & ~MIX_INPUT_WET_BITS);
    sceSdSetParam((uint16_t)(core | SD_PARAM_MMIX), mix);
#ifdef AUDIO_TRACE
    printf("audio: input send core=%d enabled=%d mmix=%03x\n", core, enabled, sceSdGetParam((uint16_t)(core | SD_PARAM_MMIX)));
#endif
}

const audio_effect_hw_ops_t spu_effect_hw_ops = {
    hw_set_mode,
    hw_set_depth,
    hw_set_voice_sends,
    hw_set_input_send,
    0,
};
