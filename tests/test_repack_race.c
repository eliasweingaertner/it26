/*
 * test_repack_race.c — "save/edit while playing" decode-cursor safety.
 *
 * While playing, the decoder caches PatternDataPos (a raw pointer into
 * Song.Patterns[CurrentPattern].PackedData) across ticks, and only
 * re-derives it (via UpdateGOTONote) when CurrentPattern/CurrentRow no
 * longer match DecodeExpectedPattern/Row. Re-packing that pattern (as
 * commit_current_pattern does on save) frees + reallocs the buffer, so
 * the cached cursor would dangle. The fix (Music_NotifyPatternRepacked,
 * called from Pattern_Pack under the engine lock) invalidates
 * DecodeExpectedPattern so the next tick re-derives the cursor.
 *
 * This checks that invariant deterministically (no ASan needed): after a
 * steady-state decode, DecodeExpectedPattern == CurrentPattern; after a
 * repack of the playing pattern it must NOT, and continued rendering must
 * proceed without faulting.
 */
#include <stdio.h>
#include <stdint.h>
#include "../src/it_music.h"
#include "../src/it_pattern.h"

extern const sounddriver_t WAVDriver;
void WAVDriver_Render(int16_t *dst, uint32_t frames);
int  Music_LoadIT(const char *path);

extern uint16_t CurrentPattern;
extern uint16_t DecodeExpectedPattern;

static editcell_t grid[MAX_PATROWS * 64];

int main(int argc, char **argv)
{
    static int16_t buf[4096 * 2];
    uint16_t rows, expect_before, expect_after, cur;
    int i, fail = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <mod>\n", argv[0]); return 2; }
    Driver = &WAVDriver;
    fprintf(stderr, "[1] loading\n"); fflush(stderr);
    if (!Music_LoadIT(argv[1])) { fprintf(stderr, "load failed\n"); return 2; }

    fprintf(stderr, "[2] init\n"); fflush(stderr);
    WAVDriver.InitSound();
    Music_ResetRNG();
    Music_InitMusic(); Music_InitStereo();
    Music_InitMixTable(); Music_InitTempo();
    Music_PlaySong(0);
    fprintf(stderr, "[3] rendering\n"); fflush(stderr);

    /* advance so PatternDataPos sits mid-buffer and the decoder is in
     * steady state (DecodeExpectedPattern == CurrentPattern) */
    for (i = 0; i < 6; i++)
        WAVDriver_Render(buf, 4096);
    fprintf(stderr, "[4] pre-repack renders done\n"); fflush(stderr);

    cur = CurrentPattern;
    expect_before = DecodeExpectedPattern;
    printf("steady state : CurrentPattern=%u DecodeExpectedPattern=%u\n",
           cur, expect_before);
    if (expect_before != cur) {
        printf("  (not decoding this pattern yet -- test inconclusive)\n");
        fail = 1;
    }

    /* commit_current_pattern(): repack the pattern being played */
    rows = (uint16_t)Pattern_Unpack(cur, grid);
    Pattern_Pack(cur, grid, rows);
    expect_after = DecodeExpectedPattern;
    printf("after repack : DecodeExpectedPattern=%u\n", expect_after);
    if (expect_after == cur) {
        printf("FAIL: cursor NOT invalidated -> next tick reads freed "
               "buffer (use-after-free)\n");
        fail = 1;
    } else {
        printf("OK: cursor invalidated -> next tick re-derives from new "
               "buffer\n");
    }

    /* keep playing across the re-derive; must not fault */
    for (i = 0; i < 6; i++)
        WAVDriver_Render(buf, 4096);
    printf("continued rendering after repack: ok\n");

    return fail ? 1 : 0;
}
