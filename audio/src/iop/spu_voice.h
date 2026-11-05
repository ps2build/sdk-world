#ifndef AUDIO_SPU_VOICE_H
#define AUDIO_SPU_VOICE_H

#include <stdint.h>

#include "audio_channels.h"

#define SPU_VOICE_TRANSFER_CORE 1

/* Opens the output of both cores at full master volume and routes all voices to the mix. Call once after sceSdInit. */
void spu_voice_enable_output(void);

/* Scales the final mix of both cores, 0-100. */
void spu_voice_set_master_volume(int volume_percent);

/* Copies size_bytes from IOP RAM into SPU2 RAM and waits for the transfer. Returns 0 or -1. */
int spu_voice_upload(const void *iop_data, uint32_t spu_address, uint32_t size_bytes);

/* Hardware envelope level (0 when silent) and next-block address; useful for checking a voice is running. */
uint16_t spu_voice_get_envelope(int core, int voice);
uint32_t spu_voice_get_next_address(int core, int voice);

/* libsd-backed implementation of the channel layer's hardware interface. */
extern const audio_channel_hw_ops_t spu_voice_hw_ops;

#endif
