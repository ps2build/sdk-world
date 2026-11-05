#ifndef AUDIO_RPC_H
#define AUDIO_RPC_H

#include <stdint.h>

/* Private protocol between the EE library and audio.irx. Not a public interface. */

#define AUDIO_RPC_ID              0x41554431u
#define AUDIO_RPC_EVENT_ID        0x41554432u
#define AUDIO_RPC_HEADER_BYTES    64u
#define AUDIO_RPC_CHUNK_MAX_BYTES 0x4000u
#define AUDIO_RPC_BUFFER_BYTES    (AUDIO_RPC_HEADER_BYTES + AUDIO_RPC_CHUNK_MAX_BYTES)
#define AUDIO_RPC_UPLOAD_ALIGNMENT 64u

#define AUDIO_DRIVER_VERSION_MAJOR 1
#define AUDIO_DRIVER_VERSION_MINOR 1
#define AUDIO_DRIVER_VERSION_PATCH 0

enum audio_rpc_function
{
    AUDIO_RPC_GET_VERSION = 1,       /* result: (major << 16) | (minor << 8) | patch */
    AUDIO_RPC_SET_MASTER_VOLUME,     /* args: percent */
    AUDIO_RPC_SOUND_BEGIN,           /* args: size_bytes, sample_rate_hz, channel_count, loops; result: handle or error */
    AUDIO_RPC_SOUND_CHUNK,           /* args: handle, offset_bytes, size_bytes; data follows the header */
    AUDIO_RPC_SOUND_FREE,            /* args: handle */
    AUDIO_RPC_SOUND_FREE_ALL,
    AUDIO_RPC_CHANNEL_PLAY,          /* args: channel, handle */
    AUDIO_RPC_CHANNEL_PLAY_ANY,      /* args: handle; result: channel or error */
    AUDIO_RPC_CHANNEL_STOP,          /* args: channel */
    AUDIO_RPC_CHANNEL_SET_VOLUME_PAN,/* args: channel, volume_percent, pan */
    AUDIO_RPC_CHANNEL_SET_PITCH,     /* args: channel, pitch_q12 */
    AUDIO_RPC_CHANNEL_IS_PLAYING,    /* args: channel; result: 1 or 0 */
    AUDIO_RPC_CHANNEL_GET_PLAYING_SOUND, /* args: channel; result: handle or 0 */
    AUDIO_RPC_CHANNEL_SET_VOLUME,    /* args: channel, volume_percent; pan is unchanged */
    AUDIO_RPC_STREAM_START,          /* starts, or restarts and discards queued blocks; the stream always plays 48 kHz stereo */
    AUDIO_RPC_STREAM_END,
    AUDIO_RPC_STREAM_WRITE,          /* args: byte_count (whole 1024-byte blocks); data follows; result: bytes accepted */
    AUDIO_RPC_STREAM_CLEAR,
    AUDIO_RPC_STREAM_PAUSE,
    AUDIO_RPC_STREAM_RESUME,
    AUDIO_RPC_STREAM_SET_VOLUME,     /* args: percent */
    AUDIO_RPC_STREAM_GET_LEVEL,      /* result: queued bytes; args[0]: free bytes, args[1]: underruns so far */
    AUDIO_RPC_STREAM_SET_LOW_THRESHOLD, /* args: ring bytes, enabled */
    AUDIO_RPC_EVENT_CANCEL,          /* makes the pending AUDIO_RPC_EVENT_WAIT return with AUDIO_EVENT_CANCEL */
    AUDIO_RPC_STREAM_DEBUG,          /* result: halves completed; args: start result, DMA status, bytes written, bytes read */
    AUDIO_RPC_EFFECT_SET_PRESET,     /* args: preset 0-9; the work area of core 0 comes from the clip memory, so it can fail with out of SPU memory */
    AUDIO_RPC_EFFECT_SET_DEPTH,      /* args: percent */
    AUDIO_RPC_EFFECT_SET_DELAY_FEEDBACK, /* args: delay 0-255, feedback 0-255 */
    AUDIO_RPC_EFFECT_RESET,          /* preset off, no sends, defaults */
    AUDIO_RPC_CHANNEL_SET_EFFECT_SEND, /* args: channel, enabled */
    AUDIO_RPC_STREAM_SET_EFFECT_SEND   /* args: enabled */
};

/* Function of the event server (AUDIO_RPC_EVENT_ID): blocks until the driver has events; result holds the bits. */
#define AUDIO_RPC_EVENT_WAIT 1

#define AUDIO_EVENT_SPACE    0x00000001u
#define AUDIO_EVENT_LOW_DATA 0x00000002u
#define AUDIO_EVENT_CANCEL   0x80000000u

/* Request and reply share one buffer: the EE sends header + data, the IOP overwrites result and returns the header. */
typedef struct audio_rpc_message
{
    int32_t result;
    int32_t args[4];
    uint8_t reserved[AUDIO_RPC_HEADER_BYTES - 5 * sizeof(int32_t)];
    uint8_t data[AUDIO_RPC_CHUNK_MAX_BYTES];
} audio_rpc_message_t;

#endif
