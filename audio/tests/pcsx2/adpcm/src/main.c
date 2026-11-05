#include <kernel.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio.h"

extern unsigned char libsd_irx[];
extern unsigned int size_libsd_irx;
extern unsigned char audio_irx[];
extern unsigned int size_audio_irx;
extern int kputs(char *s);

#define POLL_MS 10

static int failures;

static void announce(const char *format, ...)
{
    char line[96];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    printf("adpcm: >>> %s\n", line);
}

static void expect(const char *name, int condition)
{
    printf("adpcm: [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        failures++;
    }
}

static void load_module(const char *name, void *buffer, unsigned int size)
{
    int ret = -1;
    int id = SifExecModuleBuffer(buffer, size, 0, NULL, &ret);

    announce("%s.irx: id=%d ret=%d%s", name, id, ret, id < 0 ? "  <-- LOAD FAILED" : "");
}

/* Builds an APCM image of silent ADPCM blocks that lasts duration_ms at pitch_hz, as the .adp files Open PS2 Loader uses are laid out. */
static audio_sound_t make_clip(int duration_ms, unsigned pitch_hz, int loop)
{
    audio_sound_t sound = AUDIO_SOUND_INVALID;
    unsigned blocks = (unsigned)((unsigned long long)duration_ms * pitch_hz / 1000 / 28);
    unsigned size = 16 + blocks * 16;
    unsigned pitch = pitch_hz * 4096 / 48000;
    unsigned char *file = calloc(1, size);
    int status;

    if (!file)
        return AUDIO_SOUND_INVALID;
    memcpy(file, "APCM", 4);
    file[4] = 1;
    file[5] = 1;
    file[6] = (unsigned char)loop;
    file[8] = (unsigned char)pitch;
    file[9] = (unsigned char)(pitch >> 8);
    status = audio_sound_load_apcm(file, (int)size, &sound);
    announce("clip %d ms at %u Hz: %u bytes, status %d", duration_ms, pitch_hz, size, status);
    free(file);
    return status == 0 ? sound : AUDIO_SOUND_INVALID;
}

/* Milliseconds until the channel stops playing, polling every POLL_MS, or -1 after max_ms. */
static int measure_play_time(int channel, int max_ms)
{
    for (int waited = 0; waited <= max_ms; waited += POLL_MS) {
        if (audio_channel_is_playing(channel) == 0)
            return waited;
        usleep(POLL_MS * 1000);
    }
    return -1;
}

int main(void)
{
    audio_version_t version;
    audio_sound_t cursor, cancel, confirm, boot, funk_mono, sounds[3];
    int channel, time_ms;

    SifInitRpc(0);
    announce("audio .adp test");
    sbv_patch_enable_lmb();
    load_module("libsd", libsd_irx, size_libsd_irx);
    load_module("audio", audio_irx, size_audio_irx);

    {
        int status = audio_init();

        announce("audio_init -> %d", status);
        expect("audio_init", status == 0);
        if (status != 0) {
            kputs("adpcm: FAIL (driver not reachable)\n");
            printf("[ps2rig] exit=1\n");
            SleepThread();
        }
    }
    expect("second audio_init is rejected", audio_init() == -AUDIO_ERR_ALREADY_INITIALIZED);
    expect("driver version reported", audio_get_version(&version) == 0 && version.driver_major == 1);
    expect("24 channels", audio_get_channel_count() == 24);

    cursor = make_clip(80, 44100, 0);
    cancel = make_clip(300, 44100, 0);
    confirm = make_clip(500, 44100, 0);
    boot = make_clip(3670, 44100, 0);
    funk_mono = make_clip(2850, 44100, 1);
    expect("five clips loaded", cursor && cancel && confirm && boot && funk_mono);

    {
        audio_sound_t bad = AUDIO_SOUND_INVALID;
        char junk[64] = "not an apcm file";

        expect("garbage is rejected as bad sound data", audio_sound_load_apcm(junk, sizeof(junk), &bad) == -AUDIO_ERR_BAD_SOUND_DATA);
        expect("rejected load leaves the handle invalid", bad == AUDIO_SOUND_INVALID);
    }

    announce("1 the short clip mono 80 ms, channel 8");
    expect("play cursor", audio_channel_play_sound(8, cursor) == 0);
    expect("cursor is playing", audio_channel_is_playing(8) == 1);
    time_ms = measure_play_time(8, 1000);
    announce("   cursor played for about %d ms (clip is 80 ms)", time_ms);
    expect("cursor ends on its own near 80 ms", time_ms >= 50 && time_ms <= 200);

    announce("2 cancel + confirm + cursor together");
    sounds[0] = cancel;
    sounds[1] = confirm;
    sounds[2] = cursor;
    {
        int used[3];

        for (int i = 0; i < 3; i++)
            used[i] = audio_play_sound(sounds[i]);
        printf("adpcm: channels %d %d %d\n", used[0], used[1], used[2]);
        expect("three distinct free channels", used[0] >= 0 && used[1] >= 0 && used[2] >= 0
                                                   && used[0] != used[1] && used[1] != used[2] && used[0] != used[2]);
        usleep(1500 * 1000);
        expect("all three finished", audio_channel_is_playing(used[0]) == 0 && audio_channel_is_playing(used[1]) == 0
                                         && audio_channel_is_playing(used[2]) == 0);
    }

    announce("3 the long clip mono 3.7 s");
    channel = audio_play_sound(boot);
    expect("boot starts", channel >= 0);
    time_ms = measure_play_time(channel, 6000);
    announce("   boot played for about %d ms (clip is 3670 ms)", time_ms);
    expect("boot ends near its real length", time_ms >= 3400 && time_ms <= 4200);

    announce("3b the short clip with the pitch field of 22050 Hz: twice as long");
    {
        audio_sound_t slow = make_clip(160, 22050, 0);

        expect("slow cursor loaded", slow != AUDIO_SOUND_INVALID);
        expect("play slow cursor", audio_channel_play_sound(8, slow) == 0);
        time_ms = measure_play_time(8, 1000);
        announce("   slow cursor played for about %d ms (expect about 160)", time_ms);
        expect("pitch field halves the rate, doubling the length", time_ms >= 110 && time_ms <= 320);
    }

    announce("4 the looping clip: pan hard left");
    expect("pan left", audio_channel_set_volume_and_pan(3, 100, -100) == 0);
    expect("play funk_mono", audio_channel_play_sound(3, funk_mono) == 0);
    usleep(1500 * 1000);
    announce("5 the looping clip: pan hard right");
    audio_channel_set_volume_and_pan(3, 100, 100);
    usleep(1500 * 1000);
    announce("6 the looping clip: centered, volume 60");
    audio_channel_set_volume_and_pan(3, 60, 0);
    usleep(1500 * 1000);
    expect("funk_mono still playing", audio_channel_is_playing(3) == 1);
    {
        audio_sound_t playing = 0;

        expect("channel reports funk_mono", audio_channel_get_playing_sound(3, &playing) == 0 && playing == funk_mono);
    }
    announce("7 the looping clip: pitch 1.5x");
    audio_channel_set_pitch(3, 1.5f);
    usleep(1500 * 1000);
    announce("8 the looping clip: pitch 0.75x");
    audio_channel_set_pitch(3, 0.75f);
    usleep(1500 * 1000);
    audio_channel_set_pitch(3, 1.0f);
    expect("pitch out of range rejected", audio_channel_set_pitch(3, 9.0f) == -AUDIO_ERR_INVALID_ARGUMENT);
    expect("volume without pan keeps pan", audio_channel_set_volume_and_pan(3, 100, 0) == 0 && audio_channel_set_volume(3, 100) == 0);

    announce("10 fire-and-forget commands");
    {
        long started;
        long async_ms;
        long sync_ms;

        started = (long)(clock() / (CLOCKS_PER_SEC / 1000));
        for (int i = 0; i < 400; i++)
            audio_channel_set_volume(5, 50 + (i % 50));
        async_ms = (long)(clock() / (CLOCKS_PER_SEC / 1000)) - started;

        started = (long)(clock() / (CLOCKS_PER_SEC / 1000));
        for (int i = 0; i < 400; i++)
            audio_channel_is_playing(5);
        sync_ms = (long)(clock() / (CLOCKS_PER_SEC / 1000)) - started;
        printf("adpcm: 400 fire-and-forget set_volume: %ld ms, 400 synchronous is_playing: %ld ms\n", async_ms, sync_ms);
        expect("400 queued commands complete without stalling", async_ms < 4000);

        expect("a synchronous call right after an async play sees it", audio_channel_play_sound(6, boot) == 0 && audio_channel_is_playing(6) == 1);
        audio_channel_stop(6);
        expect("a synchronous call right after an async stop sees it", audio_channel_is_playing(6) == 0);

        for (int i = 0; i < 50; i++) {
            audio_channel_play_sound(7, cursor);
            audio_channel_stop(7);
        }
        expect("50 play/stop pairs end stopped, in order", audio_channel_is_playing(7) == 0);

        audio_channel_play_sound(7, boot);
        audio_channel_set_volume_and_pan(7, 70, -40);
        audio_channel_set_pitch(7, 1.25f);
        expect("commands on a playing channel keep it playing", audio_channel_is_playing(7) == 1);
        audio_channel_stop(7);
    }

    announce("9 the looping clip: master volume 40 then 100");
    expect("master volume 40", audio_set_master_volume(40) == 0);
    usleep(1000 * 1000);
    expect("master volume out of range rejected", audio_set_master_volume(101) == -AUDIO_ERR_INVALID_ARGUMENT);
    audio_set_master_volume(100);
    usleep(1000 * 1000);
    expect("free a playing sound", audio_sound_free(funk_mono) == 0);
    expect("freeing stopped its channel", audio_channel_is_playing(3) == 0);
    expect("double free is rejected", audio_sound_free(funk_mono) == -AUDIO_ERR_INVALID_ARGUMENT);
    expect("stale handle cannot be played", audio_channel_play_sound(3, funk_mono) == -AUDIO_ERR_INVALID_ARGUMENT);
    expect("bad channel rejected", audio_channel_play_sound(99, cursor) == -AUDIO_ERR_INVALID_ARGUMENT);

    expect("free all", audio_sound_free_all() == 0);
    expect("handles die with free_all", audio_channel_play_sound(0, cursor) == -AUDIO_ERR_INVALID_ARGUMENT);
    expect("audio_shutdown", audio_shutdown() == 0);
    expect("calls after shutdown fail", audio_channel_stop(0) == -AUDIO_ERR_NOT_INITIALIZED);

    kputs(failures ? "adpcm: FAIL\n" : "adpcm: PASS\n");
    printf("[ps2rig] exit=%d\n", failures ? 1 : 0);
    SleepThread();
    return 0;
}
