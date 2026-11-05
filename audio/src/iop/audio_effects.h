#ifndef AUDIO_EFFECTS_H
#define AUDIO_EFFECTS_H

#include <stdint.h>

#include "audio_channels.h"
#include "spu_ram.h"

#define AUDIO_EFFECT_PRESET_COUNT     10
#define AUDIO_EFFECT_PRESET_OFF       0
#define AUDIO_EFFECT_PRESET_ECHO      7
#define AUDIO_EFFECT_PRESET_DELAY     8
#define AUDIO_EFFECT_DEPTH_MAX        100
#define AUDIO_EFFECT_PARAM_MAX        255
#define AUDIO_EFFECT_HW_DEPTH_MAX     0x7FFF
#define AUDIO_EFFECT_DEFAULT_DEPTH    50
#define AUDIO_EFFECT_DEFAULT_DELAY    128
#define AUDIO_EFFECT_DEFAULT_FEEDBACK 128

/* libsd turns the feedback value into a signed 16-bit gain of about value * 258 / 32768, so values above 127 wrap to a
   negative gain and a value near 127 feeds back almost the whole signal, which rings. The 0-255 scale of the API is
   mapped onto 0 to this limit. */
#define AUDIO_EFFECT_HW_FEEDBACK_MAX 120

/* The effect processors: core 1 serves the channels, core 0 serves the stream and the right side of stereo sounds. */
#define AUDIO_EFFECT_CORE_COUNT 2

/* Core 0's work area ends here (libsd sets its end address to 0x1DFFFF), inside the top of the clip memory.
   Core 1's ends at the top of SPU2 RAM, inside the reserve above the clip memory. */
#define AUDIO_EFFECT_CORE0_AREA_END 0x1E0000u

#define AUDIO_EFFECTS_ERR_INVALID_ARGUMENT  (-6)
#define AUDIO_EFFECTS_ERR_OUT_OF_SPU_MEMORY (-8)

/* What the effects layer needs from the hardware. The IOP driver supplies libsd-backed functions; tests supply a mock. */
typedef struct audio_effect_hw_ops
{
    /* Switches a core from old_mode to new_mode: clears the old work area when the mode changes, loads the new
       parameters and enables the processor unless new_mode is off. Returns 0 or a negative value. */
    int (*set_mode)(void *context, int core, int old_mode, int new_mode, int16_t depth_left, int16_t depth_right, int delay, int feedback);
    void (*set_depth)(void *context, int core, int16_t depth_left, int16_t depth_right);
    /* Bit n set: voice n of the core also feeds the effect. */
    void (*set_voice_sends)(void *context, int core, uint32_t voice_mask);
    /* Whether the core's input (the stream) feeds the effect. */
    void (*set_input_send)(void *context, int core, int enabled);
    void *context;
} audio_effect_hw_ops_t;

typedef struct audio_effects
{
    const audio_effect_hw_ops_t *hw;
    spu_ram_t *ram;
    int32_t preset;
    int32_t depth_percent;
    int32_t delay;
    int32_t feedback;
    uint32_t channel_sends;
    uint8_t stream_send;
    uint8_t core0_area_held;
} audio_effects_t;

/* Work area size in bytes for a preset, 0 for off or an unknown preset. */
uint32_t audio_effects_get_work_area_bytes(int preset);

int16_t audio_effects_compute_hw_depth(int depth_percent);

/* The value passed to libsd for a preset: 0 for everything but echo, since only echo repeats more than once. */
int audio_effects_compute_hw_feedback(int preset, int feedback);

/* Does not touch the hardware. The effects start off with no sends and depth, delay and feedback at their defaults. */
void audio_effects_init(audio_effects_t *fx, const audio_effect_hw_ops_t *hw, spu_ram_t *ram);

/* Selects a preset on both cores and clears its work area. Core 0's area is taken from the clip memory; the call
   fails with AUDIO_EFFECTS_ERR_OUT_OF_SPU_MEMORY, changing nothing, when sounds occupy it. */
int audio_effects_set_preset(audio_effects_t *fx, int preset);
int audio_effects_set_depth(audio_effects_t *fx, int depth_percent);
/* Stored for every preset; only echo and delay use them. */
int audio_effects_set_delay_and_feedback(audio_effects_t *fx, int delay, int feedback);
int audio_effects_set_channel_send(audio_effects_t *fx, int channel, int enabled);
int audio_effects_set_stream_send(audio_effects_t *fx, int enabled);

/* Preset off, no sends, defaults restored and core 0's work area returned to the clip memory. */
void audio_effects_reset(audio_effects_t *fx);

/* Call after the clip memory allocator was reset so core 0's work area is reserved again. */
void audio_effects_reserve_after_ram_reset(audio_effects_t *fx);

#endif
