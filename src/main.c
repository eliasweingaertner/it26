/*
 * main.c
 * ------
 * itplay - native cross-platform player frontend for the Impulse
 * Tracker 2.17 engine port. Loads an .IT module and plays it through
 * the ported WAV/hiqual software driver, or renders it to a .WAV file.
 *
 * Usage:
 *   itplay <module.it> [options]
 *     -r <hz>     mix speed (8000..64000, default 44100)
 *     -w <file>   render to WAV instead of playing
 *     -o <order>  start order (default 0)
 *     -q          quiet (no status display)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "it_music.h"

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "../external/miniaudio.h"

extern const sounddriver_t WAVDriver;
void WAVDriver_Render(int16_t *dst, uint32_t frames);
void WAVDriver_SetMixSpeed(uint32_t hz);
uint32_t WAVDriver_GetMixSpeed(void);
int Music_LoadIT(const char *path);
void Music_FreeIT(void);

static volatile int g_quit = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_quit = 1;
}

static void audio_callback(ma_device *dev, void *out, const void *in,
                           ma_uint32 frames)
{
    (void)dev;
    (void)in;
    WAVDriver_Render((int16_t *)out, frames);
}

/* minimal WAV writer for -w (the original driver's reason to exist) */
static int render_to_wav(const char *path, uint32_t mixspeed)
{
    FILE *fp = fopen(path, "wb");
    static int16_t buf[4096 * 2];
    uint32_t datasize = 0;
    uint8_t hdr[44];
    int idle_ticks = 0;

    if (!fp) {
        fprintf(stderr, "cannot create %s\n", path);
        return 1;
    }
    memset(hdr, 0, 44);
    fwrite(hdr, 1, 44, fp);

    while (!StopSong && !g_quit) {
        WAVDriver_Render(buf, 4096);
        fwrite(buf, 4, 4096, fp);
        datasize += 4096 * 4;

        if (PlayMode == 0 && ++idle_ticks > 16)
            break;              /* song stopped */
        if (datasize > 0x70000000u)
            break;              /* runaway safety: ~30 min */
    }

    /* RIFF header */
    {
        uint32_t u32;
        uint16_t u16;

        memcpy(hdr + 0, "RIFF", 4);
        u32 = 36 + datasize;            memcpy(hdr + 4, &u32, 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        u32 = 16;                       memcpy(hdr + 16, &u32, 4);
        u16 = 1;                        memcpy(hdr + 20, &u16, 2);
        u16 = 2;                        memcpy(hdr + 22, &u16, 2);
        u32 = mixspeed;                 memcpy(hdr + 24, &u32, 4);
        u32 = mixspeed * 4;             memcpy(hdr + 28, &u32, 4);
        u16 = 4;                        memcpy(hdr + 32, &u16, 2);
        u16 = 16;                       memcpy(hdr + 34, &u16, 2);
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &datasize, 4);
    }
    fseek(fp, 0, SEEK_SET);
    fwrite(hdr, 1, 44, fp);
    fclose(fp);
    {
        extern uint32_t DebugFilterCalcs;
        uint32_t WAVDriver_GetClipped(void);
        printf("wrote %s (%u bytes of audio, %u clipped, "
               "%u filter coefficient updates)\n",
               path, datasize, WAVDriver_GetClipped(), DebugFilterCalcs);
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *modpath = NULL;
    const char *wavpath = NULL;
    uint32_t mixspeed = 44100;
    uint16_t startorder = 0;
    int quiet = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc)
            mixspeed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            wavpath = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            startorder = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (argv[i][0] != '-')
            modpath = argv[i];
    }

    if (!modpath) {
        printf("Impulse Tracker 2.17 engine port - .IT player\n"
               "(playback routines transliterated from the original "
               "IT_MUSIC.ASM/IT_M_EFF.INC/WAV.MIX)\n\n"
               "usage: itplay <module.it> [-r hz] [-w out.wav] "
               "[-o order] [-q]\n");
        return 1;
    }

    if (!Music_LoadIT(modpath)) {
        fprintf(stderr, "failed to load %s\n", modpath);
        return 1;
    }

    WAVDriver_SetMixSpeed(mixspeed);
    mixspeed = WAVDriver_GetMixSpeed();

    Driver = &WAVDriver;
    Driver->InitSound();
    Music_InitMusic();          /* engine init + Music_Stop             */
    Music_InitStereo();
    Music_InitMixTable();
    Music_InitTempo();

    printf("playing : %.25s\n", Song.Header.SongName);
    printf("mode    : %s / %s / %s effects, %u Hz %s\n",
           (Song.Header.Flags & ITF_INSTRUMENTS) ? "instruments"
                                                 : "samples",
           (Song.Header.Flags & ITF_LINEAR_SLIDES) ? "linear slides"
                                                   : "amiga slides",
           (Song.Header.Flags & ITF_OLD_EFFECTS) ? "old" : "IT",
           mixspeed,
           (Song.Header.Flags & ITF_STEREO) ? "stereo" : "mono");

    signal(SIGINT, on_sigint);

    Music_PlaySong(startorder);

    if (wavpath != NULL) {
        int rc = render_to_wav(wavpath, mixspeed);
        Music_FreeIT();
        return rc;
    }

    {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        ma_device dev;

        cfg.playback.format = ma_format_s16;
        cfg.playback.channels = 2;
        cfg.sampleRate = mixspeed;
        cfg.dataCallback = audio_callback;

        if (ma_device_init(NULL, &cfg, &dev) != MA_SUCCESS) {
            fprintf(stderr, "audio device init failed\n");
            Music_FreeIT();
            return 1;
        }
        ma_device_start(&dev);

        while (!g_quit) {
            if (!quiet) {
                int active = 0, c;
                for (c = 0; c < MAXSLAVECHANNELS; c++)
                    if (SChn[c].Flags & SF_CHAN_ON)
                        active++;
                printf("\rord %3u/%-3u pat %3u row %3u  "
                       "speed %2u tempo %3u  gvol %3u  chn %3d   ",
                       CurrentOrder, Song.Header.OrdNum,
                       CurrentPattern, CurrentRow,
                       CurrentSpeed, Tempo, GlobalVolume, active);
                fflush(stdout);
            }
            if (StopSong) {
                /* order list ran out: in IT the song loops; play one
                 * loop then exit the player */
                break;
            }
            ma_sleep(50);
        }

        printf("\n");
        ma_device_uninit(&dev);
    }

    Music_FreeIT();
    return 0;
}
