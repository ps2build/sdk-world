#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include <stdint.h>

#define AUDIO_STREAM_RING_BYTES  65536u
#define AUDIO_STREAM_HALF_BYTES  4096u
#define AUDIO_STREAM_BLOCK_BYTES 1024u
#define AUDIO_STREAM_DMA_BYTES   (AUDIO_STREAM_HALF_BYTES * 2u)

/* Event bits reported to the EE: the ring has more room, or the queued level fell to the low-data threshold. */
#define AUDIO_STREAM_EVENT_SPACE    0x1u
#define AUDIO_STREAM_EVENT_LOW_DATA 0x2u

/* What the stream needs from the hardware. The IOP driver supplies libsd-backed functions; tests supply a mock. */
typedef struct audio_stream_hw_ops
{
    /* Starts looping DMA over two halves of half_bytes each; on_half_complete must run after each half. */
    int (*start_dma)(void *context, uint8_t *dma_buffer, uint32_t half_bytes);
    void (*stop_dma)(void *context);
    void (*set_volume)(void *context, int volume_percent);
    /* Called from interrupt context with AUDIO_STREAM_EVENT_* bits; may be NULL. */
    void (*signal_events)(void *context, uint32_t events);
    uint32_t (*enter_critical)(void *context);
    void (*leave_critical)(void *context, uint32_t state);
    void *context;
} audio_stream_hw_ops_t;

/*
 * A ring of ready-to-play input blocks feeding a two-half DMA buffer. The producer is the RPC thread, the
 * consumer is the DMA-complete interrupt. The counters run freely; the ring size is a power of two.
 */
typedef struct audio_stream
{
    const audio_stream_hw_ops_t *hw;
    uint8_t *ring;
    uint8_t *dma;
    volatile uint32_t write_count;
    volatile uint32_t read_count;
    volatile uint32_t underrun_count;
    volatile uint32_t half_count;
    int last_start_result;
    uint32_t next_half;
    uint32_t low_threshold_bytes;
    volatile uint8_t low_armed;
    uint8_t low_enabled;
    int volume_percent;
    uint8_t started;
    uint8_t paused;
} audio_stream_t;

void audio_stream_init(audio_stream_t *stream, const audio_stream_hw_ops_t *hw, uint8_t *ring, uint8_t *dma);

/* Starts, or restarts and discards queued data. Returns 0 or -1. */
int audio_stream_start(audio_stream_t *stream);
void audio_stream_end(audio_stream_t *stream);

/* Appends whole 1024-byte blocks and returns the number of bytes accepted. */
uint32_t audio_stream_write(audio_stream_t *stream, const uint8_t *blocks, uint32_t byte_count);

uint32_t audio_stream_get_queued_bytes(const audio_stream_t *stream);
uint32_t audio_stream_get_free_bytes(const audio_stream_t *stream);

/* Drops queued data and silences the pending DMA halves. The stream stays started. */
void audio_stream_clear(audio_stream_t *stream);
void audio_stream_pause(audio_stream_t *stream);
void audio_stream_resume(audio_stream_t *stream);
void audio_stream_set_volume(audio_stream_t *stream, int volume_percent);

/*
 * Reports AUDIO_STREAM_EVENT_LOW_DATA once when the queued bytes fall to threshold_bytes or less, and again
 * after the level has risen above it. Enabling does not report a level that is already low.
 */
void audio_stream_set_low_threshold(audio_stream_t *stream, uint32_t threshold_bytes, int enabled);

/* Refills the half that just finished playing. Runs in interrupt context. */
void audio_stream_on_half_complete(audio_stream_t *stream);

#endif
