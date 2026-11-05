#include <irx_imports.h>

#include "audio_server.h"
#include "spu_voice.h"
#ifdef AUDIO_SELFTEST_TONE
#include "audio_selftest.h"
#endif

#define MODNAME "audio"
IRX_ID(MODNAME, 1, 1);

extern struct irx_export_table _exp_audio;

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (RegisterLibraryEntries(&_exp_audio) != 0)
        return MODULE_NO_RESIDENT_END;

    sceSdInit(0);
    spu_voice_enable_output();
#ifdef AUDIO_SELFTEST_TONE
    audio_selftest_tone();
#else
    if (audio_server_start() != 0)
        return MODULE_NO_RESIDENT_END;
#endif
    return MODULE_RESIDENT_END;
}

int _shutdown(void)
{
    return MODULE_NO_RESIDENT_END;
}

void _retonly(void)
{
}
