#include "audio.h"

static const char *const error_messages[] = {
    "success",
    "audio is not initialized",
    "audio is already initialized",
    "the audio driver did not respond",
    "unsupported stream format",
    "out of memory",
    "invalid argument",
    "no free channel",
    "out of SPU2 memory",
    "invalid sound data",
    "no stream has been started",
    "the stream is paused",
    "timed out",
    "interrupted",
    "failed to load the audio driver",
    "resource busy",
};

const char *audio_get_error_message(int error_code)
{
    int code = error_code < 0 ? -error_code : error_code;

    if (code >= (int)(sizeof(error_messages) / sizeof(error_messages[0])))
        return "unknown audio error";
    return error_messages[code];
}
