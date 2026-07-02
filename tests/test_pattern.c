/*
 * test_pattern.c
 * --------------
 * Deterministic regression test for the editor's pattern pack/unpack.
 *
 *  1. Idempotency: unpack(P) -> grid; pack(grid) -> P'; unpack(P') must
 *     yield the identical grid. Catches data loss in either direction.
 *  2. Audio equivalence: rendering the module, then round-tripping every
 *     pattern through unpack/pack (which replaces the packed stream the
 *     player reads), then rendering again from an identical engine state,
 *     must produce byte-identical audio. This proves the repacked stream
 *     is semantically identical to the original as far as the player is
 *     concerned.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/it_music.h"
#include "../src/it_pattern.h"
#include "../src/it_save.h"

extern const sounddriver_t WAVDriver;
void WAVDriver_Render(int16_t *dst, uint32_t frames);
int  Music_LoadIT(const char *path);
void Music_FreeIT(void);

static editcell_t gridA[MAX_PATROWS * 64];
static editcell_t gridB[MAX_PATROWS * 64];

/* FNV-1a over a render of `seconds` seconds */
static uint64_t render_hash(int seconds)
{
    static int16_t buf[4096 * 2];
    uint64_t h = 1469598103934665603ULL;
    int blocks = seconds * 44100 / 4096;
    int b;

    WAVDriver.InitSound();
    Music_ResetRNG();
    Music_InitMusic();
    Music_InitStereo();
    Music_InitMixTable();
    Music_InitTempo();
    Music_PlaySong(0);

    for (b = 0; b < blocks; b++) {
        int i;
        WAVDriver_Render(buf, 4096);
        for (i = 0; i < 4096 * 2; i++) {
            h ^= (uint16_t)buf[i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

int main(int argc, char **argv)
{
    int i, p;
    int idem_fail = 0;
    uint64_t h_before, h_after;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <module.it>\n", argv[0]);
        return 2;
    }

    Driver = &WAVDriver;

    if (!Music_LoadIT(argv[1])) {
        fprintf(stderr, "load failed: %s\n", argv[1]);
        return 2;
    }

    /* render the pristine module twice to confirm the harness resets
     * all engine state deterministically between runs */
    h_before = render_hash(20);
    {
        uint64_t h_control = render_hash(20);
        printf("control     : render twice (no repack) -> %s\n",
               h_before == h_control ? "deterministic"
                                     : "NON-DETERMINISTIC (harness bug)");
    }

    /* (1) idempotency check per pattern */
    for (p = 0; p < Song.Header.PatNum; p++) {
        uint16_t rowsA = Pattern_Unpack((uint16_t)p, gridA);
        Pattern_Pack((uint16_t)p, gridA, rowsA);
        {
            uint16_t rowsB = Pattern_Unpack((uint16_t)p, gridB);
            if (rowsB != rowsA ||
                memcmp(gridA, gridB,
                       sizeof(editcell_t) * rowsA * 64) != 0) {
                if (idem_fail < 5)
                    printf("  idempotency MISMATCH at pattern %d "
                           "(rows %u vs %u)\n", p, rowsA, rowsB);
                idem_fail++;
            }
        }
    }

    /* (2) audio equivalence after round-tripping all patterns */
    h_after = render_hash(20);

    printf("module      : %s\n", argv[1]);
    printf("patterns    : %d\n", Song.Header.PatNum);
    printf("idempotency : %s (%d mismatched)\n",
           idem_fail ? "FAIL" : "pass", idem_fail);
    printf("audio hash  : before=%016llx after=%016llx -> %s\n",
           (unsigned long long)h_before, (unsigned long long)h_after,
           h_before == h_after ? "IDENTICAL" : "DIFFERENT");

    /* (3) optional save -> reload round-trip (feature 004): save with
     * each SaveFormat, reload the file, re-render; every hash must
     * equal the original's. */
    if (argc > 2 && strcmp(argv[2], "--roundtrip") == 0) {
        static const uint8_t formats[3] = { 3, 0, 2 };
        const char *tmp = "rt_tmp.it";
        char msg0[64];
        int f, rt_fail = 0;

        snprintf(msg0, sizeof(msg0), "roundtrip check %s", argv[1]);
        strncpy(IT_MessageData, msg0, sizeof(IT_MessageData) - 1);

        for (f = 0; f < 3; f++) {
            uint64_t h_rt = 0;
            int ok;

            SaveFormat = formats[f];
            ok = Save_ITModule(tmp);
            if (ok) {
                Music_FreeIT();
                ok = Music_LoadIT(tmp);
            }
            if (ok) {
                h_rt = render_hash(20);
                if (strcmp(IT_MessageData, msg0) != 0)
                    ok = 0;
            }
            printf("roundtrip   : fmt %d hash=%016llx msg=%s -> %s\n",
                   formats[f], (unsigned long long)h_rt,
                   ok ? "ok" : "BAD",
                   (ok && h_rt == h_before) ? "IDENTICAL" : "DIFFERENT");
            if (!ok || h_rt != h_before)
                rt_fail++;
        }
        remove(tmp);
        Music_FreeIT();
        return (idem_fail == 0 && h_before == h_after && rt_fail == 0)
               ? 0 : 1;
    }

    Music_FreeIT();

    (void)i;
    return (idem_fail == 0 && h_before == h_after) ? 0 : 1;
}
