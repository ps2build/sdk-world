#include <irx_imports.h>

#include "audio_channels.h"
#include "audio_selftest.h"
#include "audio_sounds.h"
#include "spu_ram.h"
#include "spu_voice.h"

#define SAMPLES_PER_BLOCK   28
#define LOOP_BLOCKS         4
#define ONE_SHOT_BLOCKS     40
#define ADPCM_FLAGS_LOOP_FIRST  0x06
#define ADPCM_FLAGS_LOOP_MIDDLE 0x02
#define ADPCM_FLAGS_LOOP_LAST   0x03
#define ADPCM_FLAGS_ONE_SHOT_LAST 0x01

static uint8_t loop_clip[LOOP_BLOCKS * 16] __attribute__((aligned(64)));
static uint8_t one_shot_clip[ONE_SHOT_BLOCKS * 16] __attribute__((aligned(64)));

static spu_ram_t selftest_ram;
static audio_sound_entry_t selftest_sound_storage[16];
static audio_sound_table_t selftest_sounds;
static audio_channels_t selftest_channels;

static int selftest_failures;

static void expect(const char *name, int condition)
{
    printf("audio selftest: [%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
        selftest_failures++;
}

/* Four-bit samples at filter 0, shift 0 decode to nibble * 4096, so a rounded sine of amplitude 7 is audible. */
static int sine_nibble(int sample, int period_samples)
{
    static const int8_t quarter[SAMPLES_PER_BLOCK + 1] = {
        0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    };
    int position = sample % period_samples;
    int quarter_length = period_samples / 4;
    int quadrant = position / quarter_length;
    int offset = position % quarter_length;
    int index = quarter_length == SAMPLES_PER_BLOCK ? offset : offset * SAMPLES_PER_BLOCK / quarter_length;
    int mirrored = quarter_length == SAMPLES_PER_BLOCK ? SAMPLES_PER_BLOCK - offset : SAMPLES_PER_BLOCK - index;

    switch (quadrant) {
        case 0: return quarter[index];
        case 1: return quarter[mirrored];
        case 2: return -quarter[index];
        default: return -quarter[mirrored];
    }
}

static void build_clip(uint8_t *clip, int blocks, int period_samples, int first_flags, int middle_flags, int last_flags)
{
    for (int block = 0; block < blocks; block++) {
        uint8_t *bytes = &clip[block * 16];

        bytes[0] = 0x00;
        bytes[1] = (uint8_t)(block == 0 ? first_flags : (block == blocks - 1 ? last_flags : middle_flags));
        for (int pair = 0; pair < 14; pair++) {
            int first = sine_nibble(block * SAMPLES_PER_BLOCK + pair * 2, period_samples) & 0xF;
            int second = sine_nibble(block * SAMPLES_PER_BLOCK + pair * 2 + 1, period_samples) & 0xF;

            bytes[2 + pair] = (uint8_t)(first | (second << 4));
        }
    }
}

static int32_t load_clip(const uint8_t *clip, uint32_t size, int loops)
{
    uint32_t address = spu_ram_allocate(&selftest_ram, size);

    if (address == SPU_RAM_NO_ADDRESS || spu_voice_upload(clip, address, size) != 0)
        return -1;
    return audio_sound_table_add(&selftest_sounds, address, size, 44100, 1, loops);
}

static void selftest_thread(void *arg)
{
    int32_t loop_sound, one_shot;
    int channel;

    (void)arg;
    spu_ram_init(&selftest_ram, SPU_RAM_CLIP_AREA_START, SPU_RAM_CLIP_AREA_END);
    audio_sound_table_init(&selftest_sounds, selftest_sound_storage, 16);
    audio_channels_init(&selftest_channels, &spu_voice_hw_ops, &selftest_sounds);
    spu_voice_enable_output();

    build_clip(loop_clip, LOOP_BLOCKS, LOOP_BLOCKS * SAMPLES_PER_BLOCK, ADPCM_FLAGS_LOOP_FIRST, ADPCM_FLAGS_LOOP_MIDDLE, ADPCM_FLAGS_LOOP_LAST);
    build_clip(one_shot_clip, ONE_SHOT_BLOCKS, SAMPLES_PER_BLOCK * 2, 0x00, 0x00, ADPCM_FLAGS_ONE_SHOT_LAST);

    loop_sound = load_clip(loop_clip, sizeof(loop_clip), 1);
    one_shot = load_clip(one_shot_clip, sizeof(one_shot_clip), 0);
    expect("clips uploaded and registered", loop_sound > 0 && one_shot > 0);

    audio_channels_set_volume(&selftest_channels, 0, 30);
    expect("play looping sound on channel 0", audio_channels_play_sound(&selftest_channels, 0, loop_sound) == 0);
    DelayThread(50 * 1000);
    expect("channel 0 is playing", audio_channels_is_playing(&selftest_channels, 0) == 1);
    expect("looping voice stays inside its clip", spu_voice_get_next_address(1, 0) < SPU_RAM_CLIP_AREA_START + sizeof(loop_clip) + 16);

    channel = audio_channels_play_sound_on_free_channel(&selftest_channels, one_shot);
    expect("one-shot goes to first free channel (1)", channel == 1);
    expect("one-shot channel resets to full volume", selftest_channels.channels[1].volume_percent == 100);
    expect("one-shot is playing right after start", audio_channels_is_playing(&selftest_channels, 1) == 1);

    DelayThread(150 * 1000);
    expect("one-shot has ended and freed its channel", audio_channels_is_playing(&selftest_channels, 1) == 0);
    expect("looping sound is still playing", audio_channels_is_playing(&selftest_channels, 0) == 1);

    channel = audio_channels_play_sound_on_free_channel(&selftest_channels, one_shot);
    expect("finished channel is reused by the next one-shot", channel == 1);
    expect("replayed one-shot is playing despite the earlier end flag", audio_channels_is_playing(&selftest_channels, 1) == 1);
    DelayThread(150 * 1000);
    expect("replayed one-shot also ends", audio_channels_is_playing(&selftest_channels, 1) == 0);

    audio_channels_set_volume_and_pan(&selftest_channels, 0, 60, -100);
    audio_channels_set_pitch(&selftest_channels, 0, AUDIO_PITCH_Q12_UNITY * 2);
    DelayThread(200 * 1000);
    expect("pitch and pan changes keep the voice playing", audio_channels_is_playing(&selftest_channels, 0) == 1);

    audio_channels_stop(&selftest_channels, 0);
    expect("stop frees the channel", audio_channels_is_playing(&selftest_channels, 0) == 0);

    printf("audio selftest: %s (%d failure(s))\n", selftest_failures ? "FAILED" : "ALL PASSED", selftest_failures);
}

void audio_selftest_tone(void)
{
    iop_thread_t thread = {0};
    int id;

    thread.attr = TH_C;
    thread.thread = selftest_thread;
    thread.priority = 0x28;
    thread.stacksize = 0x1000;
    id = CreateThread(&thread);
    if (id > 0)
        StartThread(id, NULL);
    else
        printf("audio selftest: CreateThread failed %d\n", id);
}
