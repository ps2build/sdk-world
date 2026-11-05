#ifndef AUDIO_SPU_STREAM_H
#define AUDIO_SPU_STREAM_H

#include "audio_stream.h"

/* The single stream instance, wired to the SPU2 core 0 input DMA. */
audio_stream_t *spu_stream_get(void);

void spu_stream_init(void);

#endif
