#include <kernel.h>
#include <kernel_util.h>
#include <malloc.h>
#include <sifrpc.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio_adpcm.h"
#include "audio_core.h"
#include "audio_pcm.h"
#include "audio_rpc.h"

#define CHANNEL_COUNT              24
#define BIND_ATTEMPTS              3000
#define BIND_RETRY_DELAY_US        1000
#define PITCH_Q12_UNITY            4096.0f
#define PITCH_RATIO_MIN            0.0625f
#define PITCH_RATIO_MAX            4.0f
#define LIBRARY_VERSION_MAJOR      1
#define LIBRARY_VERSION_MINOR      0
#define LIBRARY_VERSION_PATCH      0
#define STREAM_RING_BYTES          65536u
#define STREAM_PARTIAL_BLOCK_SLACK 7u
#define EVENT_THREAD_STACK_BYTES   8192
#define EVENT_WAIT_SLICE_MS        25
#define EVENT_EXIT_TIMEOUT_US      1000000
#define EVENT_EXIT_SETTLE_US       2000
#define DESTROY_WAIT_MS            500
#define MAX_LIVE_SOUNDS            512
#define PAN_LIMIT                  100
#define PITCH_Q12_MIN              256
#define PITCH_Q12_MAX              16384

extern void *_gp;

typedef struct audio_event_message
{
    int32_t result;
    int32_t args[4];
    uint8_t reserved[AUDIO_RPC_HEADER_BYTES - 5 * sizeof(int32_t)];
} audio_event_message_t;

struct audio_context
{
    SifRpcClientData_t client __attribute__((aligned(64)));
    audio_rpc_message_t message __attribute__((aligned(64)));
    SifRpcClientData_t event_client __attribute__((aligned(64)));
    audio_event_message_t event_message __attribute__((aligned(64)));
    audio_pcm_converter_t stream_converter;
    int32_t live_sounds[MAX_LIVE_SOUNDS];
    int live_sound_count;
    int state_semaphore;
    int active_calls;
    volatile int closing;
    int lock_semaphore;
    int callback_semaphore;
    int space_semaphore;
    int event_done_semaphore;
    int event_exit_semaphore;
    int event_thread_id;
    void *event_stack;
    audio_low_data_fn low_callback;
    void *low_user;
    int32_t low_threshold_bytes;
    int stream_started;
    int stream_paused;
};

/* The driver serves one context at a time. */
static audio_context_t *active_context;

#define REQUIRE_CONTEXT(context)                    \
    do {                                            \
        if (!(context))                             \
            return -AUDIO_ERR_NOT_INITIALIZED;      \
    } while (0)

static int audio_ctx_get_version_impl(audio_context_t *context, audio_version_t *version_out);
static int audio_ctx_set_master_volume_impl(audio_context_t *context, int volume_percent);
static int audio_ctx_get_channel_count_impl(audio_context_t *context);
static int audio_ctx_sound_load_adpcm_impl(audio_context_t *context, const audio_sound_desc_t *desc, audio_sound_t *sound_out);
static int audio_ctx_sound_load_apcm_impl(audio_context_t *context, const void *file_data, int32_t file_size_bytes, audio_sound_t *sound_out);
static int audio_ctx_sound_free_impl(audio_context_t *context, audio_sound_t sound);
static int audio_ctx_sound_free_all_impl(audio_context_t *context);
static int audio_ctx_channel_play_sound_impl(audio_context_t *context, int channel, audio_sound_t sound);
static int audio_ctx_play_sound_impl(audio_context_t *context, audio_sound_t sound);
static int audio_ctx_channel_stop_impl(audio_context_t *context, int channel);
static int audio_ctx_channel_is_playing_impl(audio_context_t *context, int channel);
static int audio_ctx_channel_get_playing_sound_impl(audio_context_t *context, int channel, audio_sound_t *sound_out);
static int audio_ctx_channel_set_volume_impl(audio_context_t *context, int channel, int volume_percent);
static int audio_ctx_channel_set_volume_and_pan_impl(audio_context_t *context, int channel, int volume_percent, int pan);
static int audio_ctx_channel_set_pitch_impl(audio_context_t *context, int channel, float ratio);
static int audio_ctx_stream_start_impl(audio_context_t *context, const audio_stream_format_t *format);
static int audio_ctx_stream_end_impl(audio_context_t *context);
static int audio_ctx_stream_queue_pcm_impl(audio_context_t *context, const void *pcm, int32_t byte_count);
static int audio_ctx_stream_get_free_bytes_impl(audio_context_t *context);
static int audio_ctx_stream_get_queued_bytes_impl(audio_context_t *context);
static int audio_ctx_stream_wait_until_writable_impl(audio_context_t *context, int32_t byte_count, int32_t timeout_ms);
static int audio_ctx_stream_stop_and_clear_impl(audio_context_t *context);
static int audio_ctx_stream_pause_impl(audio_context_t *context);
static int audio_ctx_stream_resume_impl(audio_context_t *context);
static int audio_ctx_stream_set_volume_impl(audio_context_t *context, int volume_percent);
static int audio_ctx_stream_set_low_data_callback_impl(audio_context_t *context, int32_t threshold_bytes, audio_low_data_fn callback, void *user);

/* Calls made through the public functions are counted so destroy can wait for them to leave. */
static int enter_call(audio_context_t *context)
{
    int allowed;

    WaitSema(context->state_semaphore);
    allowed = !context->closing;
    if (allowed)
        context->active_calls++;
    SignalSema(context->state_semaphore);
    return allowed;
}

static void leave_call(audio_context_t *context)
{
    WaitSema(context->state_semaphore);
    context->active_calls--;
    SignalSema(context->state_semaphore);
}

static int active_call_count(audio_context_t *context)
{
    int count;

    WaitSema(context->state_semaphore);
    count = context->active_calls;
    SignalSema(context->state_semaphore);
    return count;
}

static void lock(audio_context_t *context)
{
    WaitSema(context->lock_semaphore);
}

static void unlock(audio_context_t *context)
{
    SignalSema(context->lock_semaphore);
}

/* The caller holds the context lock. */
static int call_locked(audio_context_t *context, int function, int32_t a0, int32_t a1, int32_t a2, int32_t a3, uint32_t data_bytes)
{
    audio_rpc_message_t *message = &context->message;

    message->args[0] = a0;
    message->args[1] = a1;
    message->args[2] = a2;
    message->args[3] = a3;
    if (SifCallRpc(&context->client, function, 0, message, AUDIO_RPC_HEADER_BYTES + data_bytes, message, AUDIO_RPC_HEADER_BYTES, NULL, NULL) < 0)
        return -AUDIO_ERR_RPC_FAILED;
    return message->result;
}

static int call(audio_context_t *context, int function, int32_t a0, int32_t a1, int32_t a2, int32_t a3, uint32_t data_bytes)
{
    int result;

    lock(context);
    result = call_locked(context, function, a0, a1, a2, a3, data_bytes);
    unlock(context);
    return result;
}

static int call_simple(audio_context_t *context, int function, int32_t a0, int32_t a1, int32_t a2)
{
    REQUIRE_CONTEXT(context);
    return call(context, function, a0, a1, a2, 0, 0);
}

static int bind_client(SifRpcClientData_t *client, int server_id)
{
    for (int attempt = 0; attempt < BIND_ATTEMPTS; attempt++) {
        if (SifBindRpc(client, server_id, 0) < 0)
            return -AUDIO_ERR_RPC_FAILED;
        if (client->server != NULL)
            return 0;
        usleep(BIND_RETRY_DELAY_US);
    }
    return -AUDIO_ERR_RPC_FAILED;
}

static int create_semaphore(int initial_count, int max_count)
{
    ee_sema_t semaphore;

    memset(&semaphore, 0, sizeof(semaphore));
    semaphore.init_count = initial_count;
    semaphore.max_count = max_count;
    return CreateSema(&semaphore);
}

/* The end of an asynchronous command releases the context lock its sender kept. */
static void command_done(void *argument)
{
    audio_context_t *context = argument;

    iSignalSema(context->lock_semaphore);
}

static int has_live_sound(audio_context_t *context, audio_sound_t sound)
{
    int found = 0;

    lock(context);
    for (int i = 0; i < context->live_sound_count; i++)
        if (context->live_sounds[i] == sound)
            found = 1;
    unlock(context);
    return found;
}

static void add_live_sound(audio_context_t *context, audio_sound_t sound)
{
    lock(context);
    if (context->live_sound_count < MAX_LIVE_SOUNDS)
        context->live_sounds[context->live_sound_count++] = sound;
    unlock(context);
}

static void remove_live_sound(audio_context_t *context, audio_sound_t sound)
{
    lock(context);
    for (int i = 0; i < context->live_sound_count; i++) {
        if (context->live_sounds[i] == sound) {
            context->live_sounds[i] = context->live_sounds[--context->live_sound_count];
            break;
        }
    }
    unlock(context);
}

static void clear_live_sounds(audio_context_t *context)
{
    lock(context);
    context->live_sound_count = 0;
    unlock(context);
}

/*
 * Sends a command without waiting for the reply. The driver has one receive buffer, so only one call may be in
 * flight: the context lock stays held until the command finishes, and every later call waits for it. The sender
 * returns at once and the driver runs commands in the order they were sent.
 */
static int send_async(audio_context_t *context, int function, int32_t a0, int32_t a1, int32_t a2)
{
    audio_rpc_message_t *message = &context->message;

    lock(context);
    message->args[0] = a0;
    message->args[1] = a1;
    message->args[2] = a2;
    message->args[3] = 0;
    if (SifCallRpc(&context->client, function, SIF_RPC_M_NOWAIT, message, AUDIO_RPC_HEADER_BYTES, message, AUDIO_RPC_HEADER_BYTES, command_done, context) < 0) {
        unlock(context);
        return -AUDIO_ERR_RPC_FAILED;
    }
    return AUDIO_OK;
}

static void event_request_done(void *argument)
{
    audio_context_t *context = argument;

    iSignalSema(context->event_done_semaphore);
}

static void run_low_data_callback(audio_context_t *context)
{
    audio_low_data_fn callback;
    void *user;

    WaitSema(context->callback_semaphore);
    lock(context);
    callback = context->low_callback;
    user = context->low_user;
    unlock(context);
    if (callback) {
        int queued = audio_ctx_stream_get_queued_bytes(context);

        callback(user, queued < 0 ? 0 : queued);
    }
    SignalSema(context->callback_semaphore);
}

/* Holds the asynchronous event call, wakes waiters on space and runs the low-data callback. */
static void event_thread(void *argument)
{
    audio_context_t *context = argument;

    for (;;) {
        uint32_t events;

        context->event_message.result = 0;
        if (SifCallRpc(&context->event_client, AUDIO_RPC_EVENT_WAIT, SIF_RPC_M_NOWAIT, &context->event_message, AUDIO_RPC_HEADER_BYTES,
                       &context->event_message, AUDIO_RPC_HEADER_BYTES, event_request_done, context) < 0)
            break;
        WaitSema(context->event_done_semaphore);

        events = (uint32_t)context->event_message.result;
        if (events & AUDIO_EVENT_CANCEL)
            break;
        if (events & AUDIO_EVENT_SPACE)
            SignalSema(context->space_semaphore);
        if (events & AUDIO_EVENT_LOW_DATA)
            run_low_data_callback(context);
    }
    SignalSema(context->event_exit_semaphore);
    ExitDeleteThread();
}

static int start_event_thread(audio_context_t *context)
{
    ee_thread_status_t creator;
    ee_thread_t thread;
    int priority = 40;

    if (ReferThreadStatus(0, &creator) >= 0 && creator.current_priority > 1)
        priority = creator.current_priority - 1;

    context->event_stack = memalign(16, EVENT_THREAD_STACK_BYTES);
    if (!context->event_stack)
        return -AUDIO_ERR_OUT_OF_MEMORY;

    memset(&thread, 0, sizeof(thread));
    thread.func = event_thread;
    thread.stack = context->event_stack;
    thread.stack_size = EVENT_THREAD_STACK_BYTES;
    thread.gp_reg = &_gp;
    thread.initial_priority = priority;
    context->event_thread_id = CreateThread(&thread);
    if (context->event_thread_id < 0)
        return -AUDIO_ERR_OUT_OF_MEMORY;
    StartThread(context->event_thread_id, context);
    return AUDIO_OK;
}

static void stop_event_thread(audio_context_t *context)
{
    u64 timeout = EVENT_EXIT_TIMEOUT_US;

    if (context->event_thread_id < 0)
        return;
    call(context, AUDIO_RPC_EVENT_CANCEL, 0, 0, 0, 0, 0);
    if (WaitSemaEx(context->event_exit_semaphore, 1, &timeout) < 0) {
        TerminateThread(context->event_thread_id);
        DeleteThread(context->event_thread_id);
    } else {
        usleep(EVENT_EXIT_SETTLE_US);
    }
    context->event_thread_id = -1;
}

static void release_context(audio_context_t *context)
{
    if (context->state_semaphore >= 0)
        DeleteSema(context->state_semaphore);
    if (context->lock_semaphore >= 0)
        DeleteSema(context->lock_semaphore);
    if (context->callback_semaphore >= 0)
        DeleteSema(context->callback_semaphore);
    if (context->space_semaphore >= 0)
        DeleteSema(context->space_semaphore);
    if (context->event_done_semaphore >= 0)
        DeleteSema(context->event_done_semaphore);
    if (context->event_exit_semaphore >= 0)
        DeleteSema(context->event_exit_semaphore);
    free(context->event_stack);
    free(context);
}

int audio_context_create(const audio_context_options_t *options, audio_context_t **context_out)
{
    audio_context_t *context;
    int status;

    if (!context_out)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    *context_out = NULL;
    if (options && options->size < (int32_t)sizeof(audio_context_options_t))
        return -AUDIO_ERR_INVALID_ARGUMENT;
    if (active_context)
        return -AUDIO_ERR_ALREADY_INITIALIZED;

    context = memalign(64, sizeof(*context));
    if (!context)
        return -AUDIO_ERR_OUT_OF_MEMORY;
    memset(context, 0, sizeof(*context));
    context->state_semaphore = -1;
    context->lock_semaphore = -1;
    context->callback_semaphore = -1;
    context->space_semaphore = -1;
    context->event_done_semaphore = -1;
    context->event_exit_semaphore = -1;
    context->event_thread_id = -1;

    SifInitRpc(0);
    status = bind_client(&context->client, AUDIO_RPC_ID);
    if (status == 0)
        status = bind_client(&context->event_client, AUDIO_RPC_EVENT_ID);
    if (status < 0) {
        release_context(context);
        return status;
    }

    context->state_semaphore = create_semaphore(1, 1);
    context->lock_semaphore = create_semaphore(1, 1);
    context->callback_semaphore = create_semaphore(1, 1);
    context->space_semaphore = create_semaphore(0, 1);
    context->event_done_semaphore = create_semaphore(0, 1);
    context->event_exit_semaphore = create_semaphore(0, 1);
    if (context->state_semaphore < 0 || context->lock_semaphore < 0 || context->callback_semaphore < 0 || context->space_semaphore < 0
        || context->event_done_semaphore < 0 || context->event_exit_semaphore < 0) {
        release_context(context);
        return -AUDIO_ERR_OUT_OF_MEMORY;
    }

    status = start_event_thread(context);
    if (status < 0) {
        release_context(context);
        return status;
    }

    active_context = context;
    *context_out = context;
    return AUDIO_OK;
}

int audio_context_destroy(audio_context_t *context)
{
    if (!context)
        return AUDIO_OK;

    WaitSema(context->state_semaphore);
    context->closing = 1;
    SignalSema(context->state_semaphore);
    for (int waited_ms = 0; waited_ms < DESTROY_WAIT_MS && active_call_count(context) > 0; waited_ms++) {
        SignalSema(context->space_semaphore);
        usleep(1000);
    }
    lock(context);
    unlock(context);
    audio_ctx_stream_set_low_data_callback_impl(context, 0, NULL, NULL);
    call(context, AUDIO_RPC_STREAM_END, 0, 0, 0, 0, 0);
    call(context, AUDIO_RPC_EFFECT_RESET, 0, 0, 0, 0, 0);
    call(context, AUDIO_RPC_SOUND_FREE_ALL, 0, 0, 0, 0, 0);
    stop_event_thread(context);
    if (active_context == context)
        active_context = NULL;
    release_context(context);
    return AUDIO_OK;
}

static int audio_ctx_get_version_impl(audio_context_t *context, audio_version_t *version_out)
{
    if (!version_out)
        return -AUDIO_ERR_INVALID_ARGUMENT;

    memset(version_out, 0, sizeof(*version_out));
    version_out->library_major = LIBRARY_VERSION_MAJOR;
    version_out->library_minor = LIBRARY_VERSION_MINOR;
    version_out->library_patch = LIBRARY_VERSION_PATCH;
    if (context) {
        int packed = call(context, AUDIO_RPC_GET_VERSION, 0, 0, 0, 0, 0);

        if (packed >= 0) {
            version_out->driver_major = (uint16_t)((packed >> 16) & 0xFF);
            version_out->driver_minor = (uint16_t)((packed >> 8) & 0xFF);
            version_out->driver_patch = (uint16_t)(packed & 0xFF);
        }
    }
    return AUDIO_OK;
}

int audio_ctx_get_version(audio_context_t *context, audio_version_t *version_out)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_get_version_impl(context, version_out);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_set_master_volume_impl(audio_context_t *context, int volume_percent)
{
    if (volume_percent < 0 || volume_percent > 100)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return call_simple(context, AUDIO_RPC_SET_MASTER_VOLUME, volume_percent, 0, 0);
}

int audio_ctx_set_master_volume(audio_context_t *context, int volume_percent)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_set_master_volume_impl(context, volume_percent);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_get_channel_count_impl(audio_context_t *context)
{
    REQUIRE_CONTEXT(context);
    return CHANNEL_COUNT;
}

int audio_ctx_get_channel_count(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_get_channel_count_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int valid_desc(const audio_sound_desc_t *desc)
{
    if (!desc || !desc->adpcm_data || desc->adpcm_size_bytes <= 0)
        return 0;
    if ((uint32_t)desc->adpcm_size_bytes % AUDIO_ADPCM_BLOCK_BYTES != 0)
        return 0;
    if (desc->channel_count != 1 && desc->channel_count != 2)
        return 0;
    if (((uint32_t)desc->adpcm_size_bytes / AUDIO_ADPCM_BLOCK_BYTES) % (uint32_t)desc->channel_count != 0)
        return 0;
    return desc->sample_rate_hz >= 4000 && desc->sample_rate_hz <= 48000;
}

static int load_adpcm_blocks(audio_context_t *context, const audio_sound_desc_t *desc, uint32_t pad_blocks, audio_sound_t *sound_out)
{
    const uint8_t *source;
    uint32_t total;
    int handle;

    if (!sound_out)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    *sound_out = AUDIO_SOUND_INVALID;
    REQUIRE_CONTEXT(context);
    if (!valid_desc(desc))
        return -AUDIO_ERR_INVALID_ARGUMENT;

    source = desc->adpcm_data;
    total = (uint32_t)desc->adpcm_size_bytes;
    handle = call(context, AUDIO_RPC_SOUND_BEGIN, (int32_t)total, desc->sample_rate_hz, desc->channel_count, desc->loops ? 1 : 0, 0);
    if (handle < 0)
        return handle;

    for (uint32_t offset = 0; offset < total;) {
        uint32_t chunk = total - offset;
        uint32_t padded;
        int status;

        if (chunk > AUDIO_RPC_CHUNK_MAX_BYTES)
            chunk = AUDIO_RPC_CHUNK_MAX_BYTES;
        padded = (chunk + AUDIO_RPC_UPLOAD_ALIGNMENT - 1u) & ~(AUDIO_RPC_UPLOAD_ALIGNMENT - 1u);

        lock(context);
        memcpy(context->message.data, source + offset, chunk);
        memset(context->message.data + chunk, 0, padded - chunk);
        audio_adpcm_patch_flags(context->message.data, chunk, offset, total, desc->channel_count, desc->loops, pad_blocks);
        status = call_locked(context, AUDIO_RPC_SOUND_CHUNK, handle, (int32_t)offset, (int32_t)padded, 0, padded);
        unlock(context);

        if (status < 0) {
            call(context, AUDIO_RPC_SOUND_FREE, handle, 0, 0, 0, 0);
            return status;
        }
        offset += chunk;
    }

    add_live_sound(context, handle);
    *sound_out = handle;
    return AUDIO_OK;
}

static int audio_ctx_sound_load_adpcm_impl(audio_context_t *context, const audio_sound_desc_t *desc, audio_sound_t *sound_out)
{
    return load_adpcm_blocks(context, desc, 0, sound_out);
}

int audio_ctx_sound_load_adpcm(audio_context_t *context, const audio_sound_desc_t *desc, audio_sound_t *sound_out)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_sound_load_adpcm_impl(context, desc, sound_out);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_sound_load_apcm_impl(audio_context_t *context, const void *file_data, int32_t file_size_bytes, audio_sound_t *sound_out)
{
    audio_adpcm_view_t view;
    audio_sound_desc_t desc;

    if (!sound_out)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    *sound_out = AUDIO_SOUND_INVALID;
    REQUIRE_CONTEXT(context);
    if (file_size_bytes < 0 || audio_apcm_parse(file_data, (uint32_t)file_size_bytes, &view) != 0)
        return -AUDIO_ERR_BAD_SOUND_DATA;

    desc.adpcm_data = view.blocks;
    desc.adpcm_size_bytes = (int32_t)view.size_bytes;
    desc.sample_rate_hz = view.sample_rate_hz;
    desc.channel_count = view.channel_count;
    desc.loops = view.loops;
    return load_adpcm_blocks(context, &desc, AUDIO_APCM_PAD_BLOCKS, sound_out);
}

int audio_ctx_sound_load_apcm(audio_context_t *context, const void *file_data, int32_t file_size_bytes, audio_sound_t *sound_out)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_sound_load_apcm_impl(context, file_data, file_size_bytes, sound_out);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_sound_free_impl(audio_context_t *context, audio_sound_t sound)
{
    int status = call_simple(context, AUDIO_RPC_SOUND_FREE, sound, 0, 0);

    if (status >= 0)
        remove_live_sound(context, sound);
    return status;
}

int audio_ctx_sound_free(audio_context_t *context, audio_sound_t sound)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_sound_free_impl(context, sound);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_sound_free_all_impl(audio_context_t *context)
{
    int status = call_simple(context, AUDIO_RPC_SOUND_FREE_ALL, 0, 0, 0);

    if (status >= 0)
        clear_live_sounds(context);
    return status;
}

int audio_ctx_sound_free_all(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_sound_free_all_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int valid_channel(int channel)
{
    return channel >= 0 && channel < CHANNEL_COUNT;
}

static int audio_ctx_channel_play_sound_impl(audio_context_t *context, int channel, audio_sound_t sound)
{
    REQUIRE_CONTEXT(context);
    if (!valid_channel(channel) || !has_live_sound(context, sound))
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return send_async(context, AUDIO_RPC_CHANNEL_PLAY, channel, sound, 0);
}

int audio_ctx_channel_play_sound(audio_context_t *context, int channel, audio_sound_t sound)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_play_sound_impl(context, channel, sound);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_play_sound_impl(audio_context_t *context, audio_sound_t sound)
{
    return call_simple(context, AUDIO_RPC_CHANNEL_PLAY_ANY, sound, 0, 0);
}

int audio_ctx_play_sound(audio_context_t *context, audio_sound_t sound)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_play_sound_impl(context, sound);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_stop_impl(audio_context_t *context, int channel)
{
    REQUIRE_CONTEXT(context);
    if (!valid_channel(channel))
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return send_async(context, AUDIO_RPC_CHANNEL_STOP, channel, 0, 0);
}

int audio_ctx_channel_stop(audio_context_t *context, int channel)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_stop_impl(context, channel);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_is_playing_impl(audio_context_t *context, int channel)
{
    return call_simple(context, AUDIO_RPC_CHANNEL_IS_PLAYING, channel, 0, 0);
}

int audio_ctx_channel_is_playing(audio_context_t *context, int channel)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_is_playing_impl(context, channel);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_get_playing_sound_impl(audio_context_t *context, int channel, audio_sound_t *sound_out)
{
    int result;

    if (!sound_out)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    result = call_simple(context, AUDIO_RPC_CHANNEL_GET_PLAYING_SOUND, channel, 0, 0);
    if (result < 0)
        return result;
    *sound_out = result;
    return AUDIO_OK;
}

int audio_ctx_channel_get_playing_sound(audio_context_t *context, int channel, audio_sound_t *sound_out)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_get_playing_sound_impl(context, channel, sound_out);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_set_volume_impl(audio_context_t *context, int channel, int volume_percent)
{
    REQUIRE_CONTEXT(context);
    if (!valid_channel(channel) || volume_percent < 0 || volume_percent > 100)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return send_async(context, AUDIO_RPC_CHANNEL_SET_VOLUME, channel, volume_percent, 0);
}

int audio_ctx_channel_set_volume(audio_context_t *context, int channel, int volume_percent)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_set_volume_impl(context, channel, volume_percent);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_set_volume_and_pan_impl(audio_context_t *context, int channel, int volume_percent, int pan)
{
    REQUIRE_CONTEXT(context);
    if (!valid_channel(channel) || volume_percent < 0 || volume_percent > 100 || pan < -PAN_LIMIT || pan > PAN_LIMIT)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return send_async(context, AUDIO_RPC_CHANNEL_SET_VOLUME_PAN, channel, volume_percent, pan);
}

int audio_ctx_channel_set_volume_and_pan(audio_context_t *context, int channel, int volume_percent, int pan)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_set_volume_and_pan_impl(context, channel, volume_percent, pan);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_set_pitch_impl(audio_context_t *context, int channel, float ratio)
{
    int32_t pitch_q12;

    REQUIRE_CONTEXT(context);
    if (!valid_channel(channel) || !(ratio >= PITCH_RATIO_MIN && ratio <= PITCH_RATIO_MAX))
        return -AUDIO_ERR_INVALID_ARGUMENT;
    pitch_q12 = (int32_t)(ratio * PITCH_Q12_UNITY + 0.5f);
    if (pitch_q12 < PITCH_Q12_MIN || pitch_q12 > PITCH_Q12_MAX)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return send_async(context, AUDIO_RPC_CHANNEL_SET_PITCH, channel, pitch_q12, 0);
}

int audio_ctx_channel_set_pitch(audio_context_t *context, int channel, float ratio)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_set_pitch_impl(context, channel, ratio);
    if (context)
        leave_call(context);
    return result;
}

#define EFFECT_PRESET_COUNT 10
#define EFFECT_PARAM_MAX    255

static int audio_ctx_effect_set_preset_impl(audio_context_t *context, int preset)
{
    int status;

    REQUIRE_CONTEXT(context);
    if (preset < 0 || preset >= EFFECT_PRESET_COUNT)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    status = call_simple(context, AUDIO_RPC_EFFECT_SET_PRESET, preset, 0, 0);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_effect_set_preset(audio_context_t *context, int preset)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_effect_set_preset_impl(context, preset);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_effect_set_depth_impl(audio_context_t *context, int depth_percent)
{
    int status;

    REQUIRE_CONTEXT(context);
    if (depth_percent < 0 || depth_percent > 100)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    status = call_simple(context, AUDIO_RPC_EFFECT_SET_DEPTH, depth_percent, 0, 0);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_effect_set_depth(audio_context_t *context, int depth_percent)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_effect_set_depth_impl(context, depth_percent);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_effect_set_delay_and_feedback_impl(audio_context_t *context, int delay, int feedback)
{
    int status;

    REQUIRE_CONTEXT(context);
    if (delay < 0 || delay > EFFECT_PARAM_MAX || feedback < 0 || feedback > EFFECT_PARAM_MAX)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    status = call_simple(context, AUDIO_RPC_EFFECT_SET_DELAY_FEEDBACK, delay, feedback, 0);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_effect_set_delay_and_feedback(audio_context_t *context, int delay, int feedback)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_effect_set_delay_and_feedback_impl(context, delay, feedback);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_channel_set_effect_send_impl(audio_context_t *context, int channel, int enabled)
{
    REQUIRE_CONTEXT(context);
    if (!valid_channel(channel))
        return -AUDIO_ERR_INVALID_ARGUMENT;
    return send_async(context, AUDIO_RPC_CHANNEL_SET_EFFECT_SEND, channel, enabled ? 1 : 0, 0);
}

int audio_ctx_channel_set_effect_send(audio_context_t *context, int channel, int enabled)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_channel_set_effect_send_impl(context, channel, enabled);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_set_effect_send_impl(audio_context_t *context, int enabled)
{
    int status;

    REQUIRE_CONTEXT(context);
    status = call_simple(context, AUDIO_RPC_STREAM_SET_EFFECT_SEND, enabled ? 1 : 0, 0, 0);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_stream_set_effect_send(audio_context_t *context, int enabled)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_set_effect_send_impl(context, enabled);
    if (context)
        leave_call(context);
    return result;
}

/* Source bytes that fit in free_ring_bytes plus the room left in the converter's partial block. */
static int32_t source_bytes_for_free_space(const audio_context_t *context, uint32_t free_ring_bytes)
{
    const audio_pcm_converter_t *converter = &context->stream_converter;
    uint32_t free_blocks = free_ring_bytes / AUDIO_PCM_BLOCK_BYTES;
    uint32_t partial_room = converter->accumulated_frames + STREAM_PARTIAL_BLOCK_SLACK < AUDIO_PCM_BLOCK_FRAMES
                                ? AUDIO_PCM_BLOCK_FRAMES - STREAM_PARTIAL_BLOCK_SLACK - converter->accumulated_frames
                                : 0;
    uint64_t output_frames = (uint64_t)free_blocks * AUDIO_PCM_BLOCK_FRAMES + partial_room;
    uint64_t source_frames = output_frames * (uint64_t)converter->sample_rate_hz / AUDIO_PCM_OUTPUT_RATE_HZ;

    if (source_frames > 0)
        source_frames--;
    return (int32_t)(source_frames * (uint64_t)audio_pcm_bytes_per_source_frame(converter));
}

/* Converts the registered source-byte threshold to ring bytes and sends it. The caller holds the context lock. */
static int apply_low_threshold_locked(audio_context_t *context)
{
    const audio_pcm_converter_t *converter = &context->stream_converter;
    uint32_t frame_bytes = (uint32_t)audio_pcm_bytes_per_source_frame(converter);
    uint64_t source_frames = (uint64_t)context->low_threshold_bytes / frame_bytes;
    uint64_t ring_bytes = source_frames * AUDIO_PCM_OUTPUT_RATE_HZ / (uint64_t)converter->sample_rate_hz * 4u;

    return call_locked(context, AUDIO_RPC_STREAM_SET_LOW_THRESHOLD, (int32_t)ring_bytes, 1, 0, 0, 0);
}

static int audio_ctx_stream_start_impl(audio_context_t *context, const audio_stream_format_t *format)
{
    int status;

    if (!format)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    REQUIRE_CONTEXT(context);

    lock(context);
    if (audio_pcm_converter_init(&context->stream_converter, format->sample_rate_hz, format->bits_per_sample, format->channel_count) != 0) {
        unlock(context);
        return -AUDIO_ERR_FORMAT_NOT_SUPPORTED;
    }
    status = call_locked(context, AUDIO_RPC_STREAM_START, 0, 0, 0, 0, 0);
    context->stream_started = status >= 0;
    context->stream_paused = 0;
    if (status >= 0 && context->low_callback)
        apply_low_threshold_locked(context);
    unlock(context);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_stream_start(audio_context_t *context, const audio_stream_format_t *format)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_start_impl(context, format);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_end_impl(audio_context_t *context)
{
    int status;

    REQUIRE_CONTEXT(context);
    lock(context);
    status = call_locked(context, AUDIO_RPC_STREAM_END, 0, 0, 0, 0, 0);
    context->stream_started = 0;
    context->stream_paused = 0;
    unlock(context);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_stream_end(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_end_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_queue_pcm_impl(audio_context_t *context, const void *pcm, int32_t byte_count)
{
    const uint8_t *source = pcm;
    uint32_t used = 0;
    int32_t free_bytes;

    if (byte_count < 0 || (byte_count > 0 && !pcm))
        return -AUDIO_ERR_INVALID_ARGUMENT;
    REQUIRE_CONTEXT(context);

    lock(context);
    if (!context->stream_started) {
        unlock(context);
        return -AUDIO_ERR_STREAM_NOT_STARTED;
    }

    if (call_locked(context, AUDIO_RPC_STREAM_GET_LEVEL, 0, 0, 0, 0, 0) < 0) {
        unlock(context);
        return -AUDIO_ERR_RPC_FAILED;
    }
    free_bytes = context->message.args[0];

    while (used < (uint32_t)byte_count) {
        uint32_t blocks_fit = (uint32_t)free_bytes / AUDIO_PCM_BLOCK_BYTES;
        uint32_t blocks = 0;
        uint32_t consumed;

        if (blocks_fit > AUDIO_RPC_CHUNK_MAX_BYTES / AUDIO_PCM_BLOCK_BYTES)
            blocks_fit = AUDIO_RPC_CHUNK_MAX_BYTES / AUDIO_PCM_BLOCK_BYTES;

        consumed = audio_pcm_convert(&context->stream_converter, source + used, (uint32_t)byte_count - used, context->message.data, blocks_fit, &blocks);
        used += consumed;
        if (blocks > 0) {
            int accepted = call_locked(context, AUDIO_RPC_STREAM_WRITE, (int32_t)(blocks * AUDIO_PCM_BLOCK_BYTES), 0, 0, 0, blocks * AUDIO_PCM_BLOCK_BYTES);

            if (accepted < 0) {
                unlock(context);
                return accepted;
            }
            free_bytes -= (int32_t)(blocks * AUDIO_PCM_BLOCK_BYTES);
        }
        if (consumed == 0)
            break;
    }
    unlock(context);
    return (int)used;
}

int audio_ctx_stream_queue_pcm(audio_context_t *context, const void *pcm, int32_t byte_count)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_queue_pcm_impl(context, pcm, byte_count);
    if (context)
        leave_call(context);
    return result;
}

static int query_level(audio_context_t *context, int32_t *queued_bytes, int32_t *free_bytes)
{
    int status;

    REQUIRE_CONTEXT(context);
    lock(context);
    if (!context->stream_started) {
        unlock(context);
        return -AUDIO_ERR_STREAM_NOT_STARTED;
    }
    status = call_locked(context, AUDIO_RPC_STREAM_GET_LEVEL, 0, 0, 0, 0, 0);
    if (status >= 0) {
        *queued_bytes = status;
        *free_bytes = context->message.args[0];
    }
    unlock(context);
    return status < 0 ? status : AUDIO_OK;
}

static int audio_ctx_stream_get_free_bytes_impl(audio_context_t *context)
{
    int32_t queued, free_ring;
    int status = query_level(context, &queued, &free_ring);
    int32_t result;

    if (status < 0)
        return status;
    lock(context);
    result = source_bytes_for_free_space(context, (uint32_t)free_ring);
    unlock(context);
    return result;
}

int audio_ctx_stream_get_free_bytes(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_get_free_bytes_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_get_queued_bytes_impl(audio_context_t *context)
{
    int32_t queued, free_ring;
    int status = query_level(context, &queued, &free_ring);
    uint64_t source_frames;
    uint64_t bytes;

    if (status < 0)
        return status;
    lock(context);
    source_frames = ((uint64_t)queued / 4u * (uint64_t)context->stream_converter.sample_rate_hz + AUDIO_PCM_OUTPUT_RATE_HZ - 1u) / AUDIO_PCM_OUTPUT_RATE_HZ;
    bytes = source_frames * (uint64_t)audio_pcm_bytes_per_source_frame(&context->stream_converter);
    unlock(context);
    return (int)bytes;
}

int audio_ctx_stream_get_queued_bytes(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_get_queued_bytes_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_wait_until_writable_impl(audio_context_t *context, int32_t byte_count, int32_t timeout_ms)
{
    int32_t waited_ms = 0;

    REQUIRE_CONTEXT(context);
    if (byte_count < 1)
        return -AUDIO_ERR_INVALID_ARGUMENT;

    lock(context);
    if (!context->stream_started) {
        unlock(context);
        return -AUDIO_ERR_STREAM_NOT_STARTED;
    }
    if (byte_count > source_bytes_for_free_space(context, STREAM_RING_BYTES)) {
        unlock(context);
        return -AUDIO_ERR_INVALID_ARGUMENT;
    }
    unlock(context);

    for (;;) {
        int free_bytes;

        if (context->closing)
            return -AUDIO_ERR_INTERRUPTED;
        free_bytes = audio_ctx_stream_get_free_bytes_impl(context);

        if (free_bytes < 0)
            return free_bytes == -AUDIO_ERR_STREAM_NOT_STARTED ? -AUDIO_ERR_INTERRUPTED : free_bytes;
        if (free_bytes >= byte_count)
            return AUDIO_OK;
        if (context->stream_paused)
            return -AUDIO_ERR_STREAM_PAUSED;
        if (timeout_ms >= 0 && waited_ms >= timeout_ms)
            return -AUDIO_ERR_TIMEOUT;
        {
            int32_t slice_ms = EVENT_WAIT_SLICE_MS;
            u64 slice_us;
            long started_ticks = (long)clock();

            if (timeout_ms >= 0 && timeout_ms - waited_ms < slice_ms)
                slice_ms = timeout_ms - waited_ms;
            slice_us = (u64)slice_ms * 1000u;
            WaitSemaEx(context->space_semaphore, 1, &slice_us);
            if (context->closing)
                return -AUDIO_ERR_INTERRUPTED;
            waited_ms += (int32_t)(((long)clock() - started_ticks) / (CLOCKS_PER_SEC / 1000));
            if (waited_ms < 0)
                waited_ms = 0;
        }
    }
}

int audio_ctx_stream_wait_until_writable(audio_context_t *context, int32_t byte_count, int32_t timeout_ms)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_wait_until_writable_impl(context, byte_count, timeout_ms);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_stop_and_clear_impl(audio_context_t *context)
{
    int status;

    REQUIRE_CONTEXT(context);
    lock(context);
    if (!context->stream_started) {
        unlock(context);
        return AUDIO_OK;
    }
    status = call_locked(context, AUDIO_RPC_STREAM_CLEAR, 0, 0, 0, 0, 0);
    if (status >= 0 && context->stream_paused)
        status = call_locked(context, AUDIO_RPC_STREAM_RESUME, 0, 0, 0, 0, 0);
    audio_pcm_converter_reset(&context->stream_converter);
    context->stream_paused = 0;
    unlock(context);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_stream_stop_and_clear(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_stop_and_clear_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int stream_set_paused(audio_context_t *context, int paused)
{
    int status;

    REQUIRE_CONTEXT(context);
    lock(context);
    if (!context->stream_started) {
        unlock(context);
        return -AUDIO_ERR_STREAM_NOT_STARTED;
    }
    status = call_locked(context, paused ? AUDIO_RPC_STREAM_PAUSE : AUDIO_RPC_STREAM_RESUME, 0, 0, 0, 0, 0);
    if (status >= 0)
        context->stream_paused = paused;
    unlock(context);
    return status < 0 ? status : AUDIO_OK;
}

static int audio_ctx_stream_pause_impl(audio_context_t *context)
{
    return stream_set_paused(context, 1);
}

int audio_ctx_stream_pause(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_pause_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_resume_impl(audio_context_t *context)
{
    return stream_set_paused(context, 0);
}

int audio_ctx_stream_resume(audio_context_t *context)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_resume_impl(context);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_set_volume_impl(audio_context_t *context, int volume_percent)
{
    int status;

    if (volume_percent < 0 || volume_percent > 100)
        return -AUDIO_ERR_INVALID_ARGUMENT;
    status = call_simple(context, AUDIO_RPC_STREAM_SET_VOLUME, volume_percent, 0, 0);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_stream_set_volume(audio_context_t *context, int volume_percent)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_set_volume_impl(context, volume_percent);
    if (context)
        leave_call(context);
    return result;
}

static int audio_ctx_stream_set_low_data_callback_impl(audio_context_t *context, int32_t threshold_bytes, audio_low_data_fn callback, void *user)
{
    int status = AUDIO_OK;

    REQUIRE_CONTEXT(context);
    if (threshold_bytes < 0)
        return -AUDIO_ERR_INVALID_ARGUMENT;

    WaitSema(context->callback_semaphore);
    lock(context);
    if (!callback) {
        context->low_callback = NULL;
        context->low_user = NULL;
        if (context->stream_started)
            status = call_locked(context, AUDIO_RPC_STREAM_SET_LOW_THRESHOLD, 0, 0, 0, 0, 0);
    } else if (!context->stream_started) {
        status = -AUDIO_ERR_STREAM_NOT_STARTED;
    } else if (threshold_bytes > source_bytes_for_free_space(context, STREAM_RING_BYTES)) {
        status = -AUDIO_ERR_INVALID_ARGUMENT;
    } else {
        context->low_callback = callback;
        context->low_user = user;
        context->low_threshold_bytes = threshold_bytes;
        status = apply_low_threshold_locked(context);
    }
    unlock(context);
    SignalSema(context->callback_semaphore);
    return status < 0 ? status : AUDIO_OK;
}

int audio_ctx_stream_set_low_data_callback(audio_context_t *context, int32_t threshold_bytes, audio_low_data_fn callback, void *user)
{
    int result;

    if (context && !enter_call(context))
        return -AUDIO_ERR_NOT_INITIALIZED;
    result = audio_ctx_stream_set_low_data_callback_impl(context, threshold_bytes, callback, user);
    if (context)
        leave_call(context);
    return result;
}
