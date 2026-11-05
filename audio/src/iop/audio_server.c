#include <irx_imports.h>

#include "audio_channels.h"
#include "audio_effects.h"
#include "audio_rpc.h"
#include "audio_server.h"
#include "audio_sounds.h"
#include "spu_effect.h"
#include "spu_ram.h"
#include "spu_stream.h"
#include "spu_voice.h"

#define SOUND_CAPACITY         512
#define SERVER_THREAD_PRIORITY 0x28
#define SERVER_THREAD_STACK    0x2000
#define EVENT_THREAD_STACK     0x800

#define ERR_OUT_OF_MEMORY      (-5)
#define ERR_INVALID_ARGUMENT   (-6)
#define ERR_OUT_OF_SPU_MEMORY  (-8)
#define ERR_BAD_SOUND_DATA     (-9)

static spu_ram_t spu_ram;
static audio_sound_entry_t sound_storage[SOUND_CAPACITY];
static audio_sound_table_t sounds;
static audio_channels_t channels;
static audio_effects_t effects;
static audio_rpc_message_t rpc_buffer __attribute__((aligned(64)));

static SifRpcDataQueue_t rpc_queue;
static SifRpcServerData_t rpc_server;

static SifRpcDataQueue_t event_queue;
static SifRpcServerData_t event_server;
static uint8_t event_buffer[AUDIO_RPC_HEADER_BYTES] __attribute__((aligned(64)));
static int event_flag = -1;

static uint32_t align_upload(uint32_t size_bytes)
{
    return (size_bytes + AUDIO_RPC_UPLOAD_ALIGNMENT - 1u) & ~(AUDIO_RPC_UPLOAD_ALIGNMENT - 1u);
}

static int32_t sound_begin(const int32_t *args)
{
    uint32_t size_bytes = (uint32_t)args[0];
    int32_t rate = args[1];
    int32_t channel_count = args[2];
    int32_t loops = args[3];
    uint32_t address;
    int32_t handle;

    if (size_bytes == 0 || size_bytes % 16u != 0)
        return ERR_BAD_SOUND_DATA;
    if (channel_count != 1 && channel_count != 2)
        return ERR_INVALID_ARGUMENT;
    if ((size_bytes / 16u) % (uint32_t)channel_count != 0 || rate < 4000 || rate > 48000)
        return ERR_BAD_SOUND_DATA;

    address = spu_ram_allocate(&spu_ram, align_upload(size_bytes));
    if (address == SPU_RAM_NO_ADDRESS)
        return ERR_OUT_OF_SPU_MEMORY;

    handle = audio_sound_table_add(&sounds, address, size_bytes, (uint32_t)rate, channel_count, loops);
    if (handle < 0) {
        spu_ram_release(&spu_ram, address, align_upload(size_bytes));
        return ERR_OUT_OF_MEMORY;
    }
    return handle;
}

static int32_t sound_chunk(const audio_rpc_message_t *message, int length)
{
    const audio_sound_entry_t *sound = audio_sound_table_get(&sounds, message->args[0]);
    uint32_t offset = (uint32_t)message->args[1];
    uint32_t size_bytes = (uint32_t)message->args[2];

    if (!sound || size_bytes == 0 || size_bytes > AUDIO_RPC_CHUNK_MAX_BYTES || size_bytes % AUDIO_RPC_UPLOAD_ALIGNMENT != 0)
        return ERR_INVALID_ARGUMENT;
    if (offset > align_upload(sound->size_bytes) || size_bytes > align_upload(sound->size_bytes) - offset)
        return ERR_INVALID_ARGUMENT;
    if ((uint32_t)length < AUDIO_RPC_HEADER_BYTES + size_bytes)
        return ERR_INVALID_ARGUMENT;
    return spu_voice_upload(message->data, sound->spu_address + offset, size_bytes) == 0 ? 0 : ERR_INVALID_ARGUMENT;
}

static int32_t sound_free(int32_t handle)
{
    const audio_sound_entry_t *sound = audio_sound_table_get(&sounds, handle);
    uint32_t address;
    uint32_t size_bytes;

    if (!sound)
        return ERR_INVALID_ARGUMENT;
    address = sound->spu_address;
    size_bytes = align_upload(sound->size_bytes);
    audio_channels_stop_sound_everywhere(&channels, handle);
    audio_sound_table_remove(&sounds, handle);
    return spu_ram_release(&spu_ram, address, size_bytes) == 0 ? 0 : ERR_INVALID_ARGUMENT;
}

static void *rpc_handler(int function, void *buffer, int length)
{
    audio_rpc_message_t *message = buffer;
    int32_t *args = message->args;

    switch (function) {
        case AUDIO_RPC_GET_VERSION:
            message->result = (AUDIO_DRIVER_VERSION_MAJOR << 16) | (AUDIO_DRIVER_VERSION_MINOR << 8) | AUDIO_DRIVER_VERSION_PATCH;
            break;
        case AUDIO_RPC_SET_MASTER_VOLUME:
            if (args[0] < 0 || args[0] > AUDIO_VOLUME_PERCENT_MAX) {
                message->result = ERR_INVALID_ARGUMENT;
            } else {
                spu_voice_set_master_volume(args[0]);
                message->result = 0;
            }
            break;
        case AUDIO_RPC_SOUND_BEGIN:
            message->result = sound_begin(args);
            break;
        case AUDIO_RPC_SOUND_CHUNK:
            message->result = sound_chunk(message, length);
            break;
        case AUDIO_RPC_SOUND_FREE:
            message->result = sound_free(args[0]);
            break;
        case AUDIO_RPC_SOUND_FREE_ALL:
            audio_channels_stop_all(&channels);
            audio_sound_table_clear(&sounds);
            spu_ram_reset(&spu_ram);
            audio_effects_reserve_after_ram_reset(&effects);
            message->result = 0;
            break;
        case AUDIO_RPC_CHANNEL_PLAY:
            message->result = audio_channels_play_sound(&channels, args[0], args[1]);
            break;
        case AUDIO_RPC_CHANNEL_PLAY_ANY:
            message->result = audio_channels_play_sound_on_free_channel(&channels, args[0]);
            break;
        case AUDIO_RPC_CHANNEL_STOP:
            message->result = audio_channels_stop(&channels, args[0]);
            break;
        case AUDIO_RPC_CHANNEL_SET_VOLUME_PAN:
            message->result = audio_channels_set_volume_and_pan(&channels, args[0], args[1], args[2]);
            break;
        case AUDIO_RPC_CHANNEL_SET_VOLUME:
            message->result = audio_channels_set_volume(&channels, args[0], args[1]);
            break;
        case AUDIO_RPC_CHANNEL_SET_PITCH:
            message->result = audio_channels_set_pitch(&channels, args[0], (uint32_t)args[1]);
            break;
        case AUDIO_RPC_CHANNEL_IS_PLAYING:
            message->result = audio_channels_is_playing(&channels, args[0]);
            break;
        case AUDIO_RPC_CHANNEL_GET_PLAYING_SOUND: {
            int32_t playing = 0;
            int status = audio_channels_get_playing_sound(&channels, args[0], &playing);

            message->result = status < 0 ? status : playing;
            break;
        }
        case AUDIO_RPC_STREAM_START:
            message->result = audio_stream_start(spu_stream_get()) == 0 ? 0 : ERR_INVALID_ARGUMENT;
            break;
        case AUDIO_RPC_STREAM_END:
            audio_stream_end(spu_stream_get());
            message->result = 0;
            break;
        case AUDIO_RPC_STREAM_WRITE:
            if (args[0] <= 0 || (uint32_t)args[0] > AUDIO_RPC_CHUNK_MAX_BYTES || (uint32_t)length < AUDIO_RPC_HEADER_BYTES + (uint32_t)args[0])
                message->result = ERR_INVALID_ARGUMENT;
            else
                message->result = (int32_t)audio_stream_write(spu_stream_get(), message->data, (uint32_t)args[0]);
            break;
        case AUDIO_RPC_STREAM_CLEAR:
            audio_stream_clear(spu_stream_get());
            message->result = 0;
            break;
        case AUDIO_RPC_STREAM_PAUSE:
            audio_stream_pause(spu_stream_get());
            message->result = 0;
            break;
        case AUDIO_RPC_STREAM_RESUME:
            audio_stream_resume(spu_stream_get());
            message->result = 0;
            break;
        case AUDIO_RPC_STREAM_SET_VOLUME:
            if (args[0] < 0 || args[0] > AUDIO_VOLUME_PERCENT_MAX) {
                message->result = ERR_INVALID_ARGUMENT;
            } else {
                audio_stream_set_volume(spu_stream_get(), args[0]);
                message->result = 0;
            }
            break;
        case AUDIO_RPC_STREAM_DEBUG:
            message->result = (int32_t)spu_stream_get()->half_count;
            args[0] = spu_stream_get()->last_start_result;
            args[1] = (int32_t)sceSdBlockTransStatus(0, 0);
            args[2] = (int32_t)spu_stream_get()->write_count;
            args[3] = (int32_t)spu_stream_get()->read_count;
            break;
        case AUDIO_RPC_STREAM_SET_LOW_THRESHOLD:
            audio_stream_set_low_threshold(spu_stream_get(), (uint32_t)args[0], args[1]);
            message->result = 0;
            break;
        case AUDIO_RPC_EFFECT_SET_PRESET:
            message->result = audio_effects_set_preset(&effects, args[0]);
            break;
        case AUDIO_RPC_EFFECT_SET_DEPTH:
            message->result = audio_effects_set_depth(&effects, args[0]);
            break;
        case AUDIO_RPC_EFFECT_SET_DELAY_FEEDBACK:
            message->result = audio_effects_set_delay_and_feedback(&effects, args[0], args[1]);
            break;
        case AUDIO_RPC_EFFECT_RESET:
            audio_effects_reset(&effects);
            message->result = 0;
            break;
        case AUDIO_RPC_CHANNEL_SET_EFFECT_SEND:
            message->result = audio_effects_set_channel_send(&effects, args[0], args[1]);
            break;
        case AUDIO_RPC_STREAM_SET_EFFECT_SEND:
            message->result = audio_effects_set_stream_send(&effects, args[0]);
            break;
        case AUDIO_RPC_EVENT_CANCEL:
            SetEventFlag(event_flag, AUDIO_EVENT_CANCEL);
            message->result = 0;
            break;
        case AUDIO_RPC_STREAM_GET_LEVEL:
            message->result = (int32_t)audio_stream_get_queued_bytes(spu_stream_get());
            message->args[0] = (int32_t)audio_stream_get_free_bytes(spu_stream_get());
            message->args[1] = (int32_t)spu_stream_get()->underrun_count;
            break;
        default:
            message->result = ERR_INVALID_ARGUMENT;
            break;
    }
#ifdef AUDIO_TRACE
    if (function != AUDIO_RPC_STREAM_GET_LEVEL && function != AUDIO_RPC_STREAM_WRITE)
        printf("audio: fn=%d args=%d,%d,%d,%d -> %d\n", function, (int)args[0], (int)args[1], (int)args[2], (int)args[3], (int)message->result);
#endif
    return buffer;
}

void audio_server_signal_events(uint32_t events)
{
    if (event_flag >= 0)
        iSetEventFlag(event_flag, events);
}

static void *event_handler(int function, void *buffer, int length)
{
    audio_rpc_message_t *message = buffer;
    u32 bits = 0;

    (void)length;
    if (function == AUDIO_RPC_EVENT_WAIT)
        WaitEventFlag(event_flag, AUDIO_EVENT_SPACE | AUDIO_EVENT_LOW_DATA | AUDIO_EVENT_CANCEL, WEF_OR | WEF_CLEAR, &bits);
    message->result = (int32_t)bits;
    return buffer;
}

static void event_thread(void *argument)
{
    (void)argument;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&event_queue, GetThreadId());
    sceSifRegisterRpc(&event_server, AUDIO_RPC_EVENT_ID, event_handler, event_buffer, NULL, NULL, &event_queue);
    sceSifRpcLoop(&event_queue);
}

static void rpc_thread(void *argument)
{
    (void)argument;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&rpc_queue, GetThreadId());
    sceSifRegisterRpc(&rpc_server, AUDIO_RPC_ID, rpc_handler, &rpc_buffer, NULL, NULL, &rpc_queue);
    sceSifRpcLoop(&rpc_queue);
}

int audio_server_start(void)
{
    iop_thread_t thread = {0};
    int id;

    spu_ram_init(&spu_ram, SPU_RAM_CLIP_AREA_START, SPU_RAM_CLIP_AREA_END);
    audio_sound_table_init(&sounds, sound_storage, SOUND_CAPACITY);
    audio_channels_init(&channels, &spu_voice_hw_ops, &sounds);
    audio_effects_init(&effects, &spu_effect_hw_ops, &spu_ram);
    spu_stream_init();

    {
        iop_event_t flag = {0};

        flag.attr = EA_SINGLE;
        event_flag = CreateEventFlag(&flag);
        if (event_flag < 0)
            return -1;
    }

    thread.attr = TH_C;
    thread.thread = rpc_thread;
    thread.priority = SERVER_THREAD_PRIORITY;
    thread.stacksize = SERVER_THREAD_STACK;
    id = CreateThread(&thread);
    if (id <= 0)
        return -1;
    StartThread(id, NULL);

    thread.thread = event_thread;
    thread.stacksize = EVENT_THREAD_STACK;
    id = CreateThread(&thread);
    if (id <= 0)
        return -1;
    StartThread(id, NULL);
    return 0;
}
