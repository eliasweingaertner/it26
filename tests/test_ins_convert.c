/*
 * test_ins_convert.c — verify sample->instrument conversion
 * (Music_ClearAllInstruments + the F_SetControlInstrument fill loop).
 */
#include <stdio.h>
#include <string.h>
#include "../src/it_music.h"

int Music_LoadIT(const char *path);

int main(int argc, char **argv)
{
    int s, n, fails = 0, made = 0;

    if (argc < 2) { fprintf(stderr, "usage: %s <mod>\n", argv[0]); return 2; }
    if (!Music_LoadIT(argv[1])) { fprintf(stderr, "load failed\n"); return 2; }

    printf("before: instrument mode = %s\n",
           (Song.Header.Flags & ITF_INSTRUMENTS) ? "on" : "off");

    /* the conversion: clear all, then map each populated sample */
    Music_ClearAllInstruments();
    for (s = 0; s < MAX_SAMPLES - 1; s++) {
        sample_t     *smp = &Song.Smp[s];
        instrument_t *in  = &Song.Ins[s];
        if (!(smp->Flags & 1))
            continue;
        memcpy(in->InstrumentName, smp->SampleName, sizeof(in->InstrumentName));
        for (n = 0; n < 120; n++)
            in->NoteSampleTable[n * 2 + 1] = (uint8_t)(s + 1);
    }

    /* verify */
    for (s = 0; s < MAX_SAMPLES - 1; s++) {
        sample_t     *smp = &Song.Smp[s];
        instrument_t *in  = &Song.Ins[s];

        if (smp->Flags & 1) {
            char nm[27]; memcpy(nm, in->InstrumentName, 26); nm[26] = 0;
            made++;
            /* name copied */
            if (memcmp(in->InstrumentName, smp->SampleName, 26) != 0) {
                printf("FAIL ins %d: name mismatch\n", s + 1); fails++;
            }
            /* all 120 notes -> (note n, sample s+1) */
            for (n = 0; n < 120; n++) {
                if (in->NoteSampleTable[n*2] != (unsigned char)n ||
                    in->NoteSampleTable[n*2+1] != (unsigned char)(s+1)) {
                    printf("FAIL ins %d: note %d maps to (%d,%d)\n", s+1, n,
                           in->NoteSampleTable[n*2], in->NoteSampleTable[n*2+1]);
                    fails++; break;
                }
            }
            /* default template sanity */
            if (in->GbV != 128 || in->PPC != 60 || in->VEnvelope.Num != 2 ||
                in->VEnvelope.NodePoints[1].Tick != 100) {
                printf("FAIL ins %d: default template wrong "
                       "(GbV=%d PPC=%d VNum=%d)\n",
                       s+1, in->GbV, in->PPC, in->VEnvelope.Num);
                fails++;
            }
            printf("ins %2d <- sample \"%s\" (all notes -> smp %d)\n",
                   s+1, nm, s+1);
        } else {
            /* empty sample -> default instrument, no name */
            if (in->InstrumentName[0] != 0) {
                printf("FAIL ins %d: empty slot got a name\n", s + 1); fails++;
            }
            if (in->NoteSampleTable[1] != 0) {   /* sample byte stays 0 */
                printf("FAIL ins %d: empty slot mapped to sample %d\n",
                       s+1, in->NoteSampleTable[1]); fails++;
            }
        }
    }

    printf("%d instruments created, %d failures -> %s\n",
           made, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
