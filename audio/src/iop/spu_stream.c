#include <irx_imports.h>

#include "audio_channels.h"
#include "audio_server.h"
#include "spu_stream.h"

#define STREAM_CORE 0

static uint8_t ring_storage[AUDIO_STREAM_RING_BYTES] __attribute__((aligned(64)));
static uint8_t dma_storage[AUDIO_STREAM_DMA_BYTES] __attribute__((aligned(1024)));
static audio_stream_t stream;

static int transfer_done_handler(int core, void *argument)
{
    (void)core;
    (void)argument;
    audio_stream_on_half_complete(&stream);
    return 0;
}

static void apply_volume(int volume_percent)
{
    uint16_t hardware = (uint16_t)((volume_percent * AUDIO_HW_VOLUME_MAX) / 100);

    sceSdSetParam(STREAM_CORE | SD_PARAM_BVOLL, hardware);
    sceSdSetParam(STREAM_CORE | SD_PARAM_BVOLR, hardware);
}

static int hw_start_dma(void *context, uint8_t *dma_buffer, uint32_t half_bytes)
{
    (void)context;
    sceSdSetTransIntrHandler(STREAM_CORE, transfer_done_handler, NULL);
    apply_volume(stream.volume_percent);
    return sceSdBlockTrans(STREAM_CORE, SD_BLOCK_TRANS_WRITE | SD_BLOCK_TRANS_LOOP, dma_buffer, half_bytes * 2u);
}

static void hw_stop_dma(void *context)
{
    (void)context;
    sceSdBlockTrans(STREAM_CORE, SD_BLOCK_TRANS_STOP, NULL, 0);
    sceSdSetTransIntrHandler(STREAM_CORE, NULL, NULL);
    apply_volume(0);
}

static void hw_set_volume(void *context, int volume_percent)
{
    (void)context;
    if (stream.started)
        apply_volume(volume_percent);
}

static void hw_signal_events(void *context, uint32_t events)
{
    (void)context;
    audio_server_signal_events(events);
}

static uint32_t hw_enter_critical(void *context)
{
    int state;

    (void)context;
    CpuSuspendIntr(&state);
    return (uint32_t)state;
}

static void hw_leave_critical(void *context, uint32_t state)
{
    (void)context;
    CpuResumeIntr((int)state);
}

static const audio_stream_hw_ops_t hw_ops = {
    hw_start_dma,
    hw_stop_dma,
    hw_set_volume,
    hw_signal_events,
    hw_enter_critical,
    hw_leave_critical,
    NULL,
};

audio_stream_t *spu_stream_get(void)
{
    return &stream;
}

void spu_stream_init(void)
{
    audio_stream_init(&stream, &hw_ops, ring_storage, dma_storage);
}
