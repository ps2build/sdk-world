#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio.h"
#include "libsd_irx.h"
#include "audio_irx.h"

static void *read_file(const char *path, int *size_out)
{
    FILE *file = fopen(path, "rb");
    long length;
    void *data;

    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    length = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(file);
    *size_out = (int)length;
    return data;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "host:sound.adp";
    audio_sound_t sound;
    void *file_data;
    int file_size = 0;
    int channel;
    int rc;

    SifInitRpc(0);
    sbv_patch_enable_lmb();
    SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, NULL);
    SifExecModuleBuffer(audio_irx, size_audio_irx, 0, NULL, NULL);

    rc = audio_init();
    if (rc != AUDIO_OK) {
        printf("audio_init: %s\n", audio_get_error_message(rc));
        SleepThread();
    }

    file_data = read_file(path, &file_size);
    if (!file_data) {
        printf("cannot read %s\n", path);
        SleepThread();
    }

    rc = audio_sound_load_apcm(file_data, file_size, &sound);
    free(file_data);
    if (rc != AUDIO_OK) {
        printf("load: %s\n", audio_get_error_message(rc));
        SleepThread();
    }

    channel = audio_play_sound(sound);
    if (channel < 0) {
        printf("play: %s\n", audio_get_error_message(-channel));
        SleepThread();
    }
    printf("playing on channel %d\n", channel);

    audio_channel_set_volume_and_pan(channel, 80, -50);
    while (audio_channel_is_playing(channel) == 1)
        usleep(10000);

    audio_sound_free(sound);
    audio_shutdown();
    printf("done\n");
    SleepThread();
    return 0;
}
