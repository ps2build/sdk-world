#include "audio_effects.h"

/* Work area sizes in 16-byte units, indexed by preset. */
static const uint16_t work_area_units[AUDIO_EFFECT_PRESET_COUNT] = {0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0};

uint32_t audio_effects_get_work_area_bytes(int preset)
{
    if (preset <= AUDIO_EFFECT_PRESET_OFF || preset >= AUDIO_EFFECT_PRESET_COUNT)
        return 0;
    return (uint32_t)work_area_units[preset] * 16u;
}

int16_t audio_effects_compute_hw_depth(int depth_percent)
{
    return (int16_t)(((int32_t)depth_percent * AUDIO_EFFECT_HW_DEPTH_MAX) / AUDIO_EFFECT_DEPTH_MAX);
}

int audio_effects_compute_hw_feedback(int preset, int feedback)
{
    if (preset != AUDIO_EFFECT_PRESET_ECHO)
        return 0;
    return (feedback * AUDIO_EFFECT_HW_FEEDBACK_MAX) / AUDIO_EFFECT_PARAM_MAX;
}

static uint32_t core0_area_address(int preset)
{
    return AUDIO_EFFECT_CORE0_AREA_END - audio_effects_get_work_area_bytes(preset);
}

static void apply_sends(audio_effects_t *fx)
{
    for (int core = 0; core < AUDIO_EFFECT_CORE_COUNT; core++)
        fx->hw->set_voice_sends(fx->hw->context, core, fx->channel_sends);
    fx->hw->set_input_send(fx->hw->context, 0, fx->stream_send);
}

static void restore_defaults(audio_effects_t *fx)
{
    fx->preset = AUDIO_EFFECT_PRESET_OFF;
    fx->depth_percent = AUDIO_EFFECT_DEFAULT_DEPTH;
    fx->delay = AUDIO_EFFECT_DEFAULT_DELAY;
    fx->feedback = AUDIO_EFFECT_DEFAULT_FEEDBACK;
    fx->channel_sends = 0;
    fx->stream_send = 0;
}

void audio_effects_init(audio_effects_t *fx, const audio_effect_hw_ops_t *hw, spu_ram_t *ram)
{
    fx->hw = hw;
    fx->ram = ram;
    fx->core0_area_held = 0;
    restore_defaults(fx);
}

static void apply_mode(audio_effects_t *fx, int old_mode, int new_mode)
{
    int16_t depth = audio_effects_compute_hw_depth(fx->depth_percent);

    for (int core = 0; core < AUDIO_EFFECT_CORE_COUNT; core++)
        fx->hw->set_mode(fx->hw->context, core, old_mode, new_mode, depth, depth, fx->delay, audio_effects_compute_hw_feedback(new_mode, fx->feedback));
}

static void release_core0_area(audio_effects_t *fx)
{
    int preset = (int)fx->preset;

    if (fx->core0_area_held)
        spu_ram_release(fx->ram, core0_area_address(preset), audio_effects_get_work_area_bytes(preset));
    fx->core0_area_held = 0;
}

int audio_effects_set_preset(audio_effects_t *fx, int preset)
{
    int old_preset = (int)fx->preset;
    uint32_t new_bytes = audio_effects_get_work_area_bytes(preset);

    if (preset < 0 || preset >= AUDIO_EFFECT_PRESET_COUNT)
        return AUDIO_EFFECTS_ERR_INVALID_ARGUMENT;

    release_core0_area(fx);
    if (new_bytes > 0) {
        if (spu_ram_allocate_at(fx->ram, core0_area_address(preset), new_bytes) != 0) {
            audio_effects_reserve_after_ram_reset(fx);
            return AUDIO_EFFECTS_ERR_OUT_OF_SPU_MEMORY;
        }
        fx->core0_area_held = 1;
    }

    fx->preset = preset;
    apply_mode(fx, old_preset, preset);
    apply_sends(fx);
    return 0;
}

int audio_effects_set_depth(audio_effects_t *fx, int depth_percent)
{
    int16_t depth;

    if (depth_percent < 0 || depth_percent > AUDIO_EFFECT_DEPTH_MAX)
        return AUDIO_EFFECTS_ERR_INVALID_ARGUMENT;
    fx->depth_percent = depth_percent;
    depth = audio_effects_compute_hw_depth(depth_percent);
    if (fx->preset != AUDIO_EFFECT_PRESET_OFF)
        for (int core = 0; core < AUDIO_EFFECT_CORE_COUNT; core++)
            fx->hw->set_depth(fx->hw->context, core, depth, depth);
    return 0;
}

int audio_effects_set_delay_and_feedback(audio_effects_t *fx, int delay, int feedback)
{
    if (delay < 0 || delay > AUDIO_EFFECT_PARAM_MAX || feedback < 0 || feedback > AUDIO_EFFECT_PARAM_MAX)
        return AUDIO_EFFECTS_ERR_INVALID_ARGUMENT;
    fx->delay = delay;
    fx->feedback = feedback;
    if (fx->preset == AUDIO_EFFECT_PRESET_ECHO || fx->preset == AUDIO_EFFECT_PRESET_DELAY)
        apply_mode(fx, (int)fx->preset, (int)fx->preset);
    return 0;
}

int audio_effects_set_channel_send(audio_effects_t *fx, int channel, int enabled)
{
    if (channel < 0 || channel >= AUDIO_CHANNEL_COUNT)
        return AUDIO_EFFECTS_ERR_INVALID_ARGUMENT;
    if (enabled)
        fx->channel_sends |= 1u << channel;
    else
        fx->channel_sends &= ~(1u << channel);
    for (int core = 0; core < AUDIO_EFFECT_CORE_COUNT; core++)
        fx->hw->set_voice_sends(fx->hw->context, core, fx->channel_sends);
    return 0;
}

int audio_effects_set_stream_send(audio_effects_t *fx, int enabled)
{
    fx->stream_send = enabled ? 1 : 0;
    fx->hw->set_input_send(fx->hw->context, 0, fx->stream_send);
    return 0;
}

void audio_effects_reset(audio_effects_t *fx)
{
    int old_preset = (int)fx->preset;

    release_core0_area(fx);
    restore_defaults(fx);
    if (old_preset != AUDIO_EFFECT_PRESET_OFF)
        apply_mode(fx, old_preset, AUDIO_EFFECT_PRESET_OFF);
    apply_sends(fx);
}

void audio_effects_reserve_after_ram_reset(audio_effects_t *fx)
{
    uint32_t bytes = audio_effects_get_work_area_bytes((int)fx->preset);

    fx->core0_area_held = 0;
    if (bytes > 0 && spu_ram_allocate_at(fx->ram, core0_area_address((int)fx->preset), bytes) == 0)
        fx->core0_area_held = 1;
}
