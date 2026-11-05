#include "audio_stream.h"

static void copy_bytes(uint8_t *to, const uint8_t *from, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        to[i] = from[i];
}

static void zero_bytes(uint8_t *to, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        to[i] = 0;
}

static uint32_t enter(const audio_stream_t *stream)
{
    return stream->hw->enter_critical ? stream->hw->enter_critical(stream->hw->context) : 0;
}

static void leave(const audio_stream_t *stream, uint32_t state)
{
    if (stream->hw->leave_critical)
        stream->hw->leave_critical(stream->hw->context, state);
}

void audio_stream_init(audio_stream_t *stream, const audio_stream_hw_ops_t *hw, uint8_t *ring, uint8_t *dma)
{
    stream->hw = hw;
    stream->ring = ring;
    stream->dma = dma;
    stream->write_count = 0;
    stream->read_count = 0;
    stream->underrun_count = 0;
    stream->half_count = 0;
    stream->last_start_result = 0;
    stream->next_half = 0;
    stream->low_threshold_bytes = 0;
    stream->low_armed = 0;
    stream->low_enabled = 0;
    stream->volume_percent = 100;
    stream->started = 0;
    stream->paused = 0;
}

int audio_stream_start(audio_stream_t *stream)
{
    if (stream->started)
        audio_stream_end(stream);

    zero_bytes(stream->dma, AUDIO_STREAM_DMA_BYTES);
    stream->write_count = 0;
    stream->read_count = 0;
    stream->underrun_count = 0;
    stream->next_half = 0;
    stream->paused = 0;

    stream->half_count = 0;
    stream->last_start_result = stream->hw->start_dma(stream->hw->context, stream->dma, AUDIO_STREAM_HALF_BYTES);
    if (stream->last_start_result < 0)
        return -1;
    stream->started = 1;
    return 0;
}

void audio_stream_end(audio_stream_t *stream)
{
    if (!stream->started)
        return;
    stream->hw->stop_dma(stream->hw->context);
    stream->started = 0;
    stream->paused = 0;
    stream->write_count = 0;
    stream->read_count = 0;
}

uint32_t audio_stream_get_queued_bytes(const audio_stream_t *stream)
{
    return stream->write_count - stream->read_count;
}

uint32_t audio_stream_get_free_bytes(const audio_stream_t *stream)
{
    return AUDIO_STREAM_RING_BYTES - audio_stream_get_queued_bytes(stream);
}

uint32_t audio_stream_write(audio_stream_t *stream, const uint8_t *blocks, uint32_t byte_count)
{
    uint32_t accepted = byte_count & ~(AUDIO_STREAM_BLOCK_BYTES - 1u);
    uint32_t free_bytes = audio_stream_get_free_bytes(stream);
    uint32_t position;
    uint32_t first;

    if (!stream->started)
        return 0;
    if (accepted > free_bytes)
        accepted = free_bytes;
    if (accepted == 0)
        return 0;

    position = stream->write_count & (AUDIO_STREAM_RING_BYTES - 1u);
    first = AUDIO_STREAM_RING_BYTES - position;
    if (first > accepted)
        first = accepted;
    copy_bytes(stream->ring + position, blocks, first);
    copy_bytes(stream->ring, blocks + first, accepted - first);
    stream->write_count += accepted;
    if (stream->low_enabled && audio_stream_get_queued_bytes(stream) > stream->low_threshold_bytes)
        stream->low_armed = 1;
    return accepted;
}

void audio_stream_clear(audio_stream_t *stream)
{
    uint32_t state = enter(stream);

    stream->read_count = stream->write_count;
    zero_bytes(stream->dma, AUDIO_STREAM_DMA_BYTES);
    leave(stream, state);
}

void audio_stream_pause(audio_stream_t *stream)
{
    stream->paused = 1;
}

void audio_stream_resume(audio_stream_t *stream)
{
    stream->paused = 0;
}

void audio_stream_set_volume(audio_stream_t *stream, int volume_percent)
{
    stream->volume_percent = volume_percent;
    stream->hw->set_volume(stream->hw->context, volume_percent);
}

void audio_stream_set_low_threshold(audio_stream_t *stream, uint32_t threshold_bytes, int enabled)
{
    uint32_t state = enter(stream);

    stream->low_threshold_bytes = threshold_bytes;
    stream->low_enabled = enabled ? 1 : 0;
    stream->low_armed = stream->low_enabled && audio_stream_get_queued_bytes(stream) > threshold_bytes;
    leave(stream, state);
}

static void report_events(audio_stream_t *stream)
{
    uint32_t events = AUDIO_STREAM_EVENT_SPACE;

    if (stream->low_enabled) {
        if (audio_stream_get_queued_bytes(stream) <= stream->low_threshold_bytes) {
            if (stream->low_armed) {
                events |= AUDIO_STREAM_EVENT_LOW_DATA;
                stream->low_armed = 0;
            }
        } else {
            stream->low_armed = 1;
        }
    }
    if (stream->hw->signal_events)
        stream->hw->signal_events(stream->hw->context, events);
}

void audio_stream_on_half_complete(audio_stream_t *stream)
{
    uint8_t *half = stream->dma + stream->next_half * AUDIO_STREAM_HALF_BYTES;
    uint32_t queued = stream->write_count - stream->read_count;

    if (stream->started && !stream->paused && queued > 0) {
        uint32_t take = queued < AUDIO_STREAM_HALF_BYTES ? queued : AUDIO_STREAM_HALF_BYTES;
        uint32_t position = stream->read_count & (AUDIO_STREAM_RING_BYTES - 1u);
        uint32_t first = AUDIO_STREAM_RING_BYTES - position;

        if (first > take)
            first = take;
        copy_bytes(half, stream->ring + position, first);
        copy_bytes(half + first, stream->ring, take - first);
        zero_bytes(half + take, AUDIO_STREAM_HALF_BYTES - take);
        stream->read_count += take;
        if (take < AUDIO_STREAM_HALF_BYTES)
            stream->underrun_count++;
    } else {
        zero_bytes(half, AUDIO_STREAM_HALF_BYTES);
        if (stream->started && !stream->paused)
            stream->underrun_count++;
    }
    stream->next_half ^= 1u;
    stream->half_count++;
    if (stream->started)
        report_events(stream);
}
