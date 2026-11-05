#include <kernel.h>
#include <loadfile.h>
#include <math.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "audio.h"
#include "libsd_irx.h"
#include "audio_irx.h"

#define SAMPLE_RATE    44100
#define FREQUENCY_HZ   440.0f
#define CHUNK_FRAMES   1024
#define SECONDS        3

int main(void)
{
    audio_stream_format_t format = { SAMPLE_RATE, 16, 1 };
    int16_t chunk[CHUNK_FRAMES];
    float phase = 0.0f;
    int chunks = SAMPLE_RATE * SECONDS / CHUNK_FRAMES;
    int rc;

    SifInitRpc(0);
    sbv_patch_enable_lmb();
    SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, NULL);
    SifExecModuleBuffer(audio_irx, size_audio_irx, 0, NULL, NULL);

    rc = audio_init();
    if (rc == AUDIO_OK)
        rc = audio_stream_start(&format);
    if (rc != AUDIO_OK) {
        printf("audio: %s\n", audio_get_error_message(rc));
        SleepThread();
    }

    for (int i = 0; i < chunks; i++) {
        for (int n = 0; n < CHUNK_FRAMES; n++) {
            chunk[n] = (int16_t)(sinf(phase) * 12000.0f);
            phase += 2.0f * 3.14159265f * FREQUENCY_HZ / SAMPLE_RATE;
            if (phase > 2.0f * 3.14159265f)
                phase -= 2.0f * 3.14159265f;
        }
        audio_stream_wait_until_writable(sizeof(chunk), 1000);
        audio_stream_queue_pcm(chunk, sizeof(chunk));
    }

    while (audio_stream_get_queued_bytes() > 0)
        usleep(10000);
    audio_stream_end();
    audio_shutdown();
    printf("done\n");
    SleepThread();
    return 0;
}
