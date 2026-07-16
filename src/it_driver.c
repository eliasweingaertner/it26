/*
 * it_driver.c
 * -----------
 * Port of Impulse Tracker's high-quality software driver - the "WAV
 * writer" driver (WAVDRV.ASM + MIXWAV.INC + WAV.MIX), which is the pure
 * software path: 256 channels, cubic spline interpolation, per-channel
 * resonant filters, volume ramping and click removal, 32-bit mixing with
 * error-feedback dither on output. DriverFlags = 3 (MIDI out + hiqual),
 * exactly like ITWAV.DRV.
 *
 * Differences from the DOS original, all mechanical or documented:
 *  - renders into a caller-supplied buffer instead of writing a .WAV
 *    via INT 21h; the per-tick mixing pipeline is unchanged.
 *  - no EMS/64KB segmentation: samples live in flat memory, so the
 *    MFS/MBS "separate into 64k chunks" plumbing reduces to a single
 *    inner-loop call per chunk. The chunk math (ceil(amount/step)) is
 *    kept identical.
 *  - FPU code (single-precision control word 7Fh in the original) is
 *    done in C floats; FIST rounding reproduced with llrintf.
 *  - The WAV driver's 4-band output equalizer is implemented but
 *    bypassed by default: its parameters lived in the user's saved
 *    driver configuration, and the source-default volumes would mute
 *    the output entirely.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "it_music.h"
#include "it_pattern.h"     /* Engine_Lock / Engine_Unlock hooks */

/* WAVSWITC.INC */
#define VOLUMERAMP      1
#define DOUBLEVOLUME    0
#define DITHEROUTPUT    1
#define RAMPSPEED       8
#define RAMPCOMPENSATE  255

#define MAXMIXFRAMES    8192    /* >= 2.5*64000/32 + 1 */

/* ---- Driver state (WAVDRV.ASM data area) ---------------------------- */

static uint16_t MixSpeed = 44100;
static uint16_t MixVolume = 0;
static uint8_t  Stereo = 0, StereoSet = 0;
static uint16_t WBytesToMix = 0;
static uint32_t BytesToMixFractional = 0;
static uint32_t CurrentFractional = 0;
static uint16_t RealBytesToMix = 0;
static uint16_t StartNoRamp = 0;
static uint32_t NumClipped = 0;

/* 32-bit mix buffer: L,R pairs (the DOS version keeps an 80-byte filter
 * scratch area in front; here the filter scratch is explicit). */
static int32_t MixBuffer[MAXMIXFRAMES * 2];

static int32_t LastClickRemovalLeft = 0;
static int32_t LastClickRemovalRight = 0;
static int32_t LastLeftValue = 0;
static int32_t LastRightValue = 0;

/* Per-host-channel filter parameters set via the F0 F0 macro protocol:
 * [0..63] = cutoff (default 7Fh), [64..127] = resonance (default 0). */
static uint8_t FilterParameters[128];

/* Per-slave-channel filter state (FilterValues, 4 dwords each). */
typedef struct chnfilter_t {
    float y1, y2;     /* last two filter outputs   */
    float fa, fb;     /* coefficients a and b      */
} chnfilter_t;
static chnfilter_t FilterValues[MAXSLAVECHANNELS];

/* current chunk's filter c coefficient (1 - a - b, computed per tick) */
static float FilterC;

static float FreqMultiplier;    /* MixSpeed / (2*PI*110*2^0.25)          */
#define FREQMULTIPLIER_BASE_BITS 0x3A9F7867u /* 1/(2*PI*110*2^0.25)      */
#define FREQPARAMMUL_BITS        0xB92AAAAAu /* -1/(24*256)              */

/* QualityFactorTable (Q.INC), 128 single-precision bit patterns. */
static const uint32_t QualityFactorTable[128] = {
    0x3F800000, 0x3F7A8874, 0x3F752ECB, 0x3F6FF262,
    0x3F6AD298, 0x3F65CED3, 0x3F60E678, 0x3F5C18F1,
    0x3F5765AC, 0x3F52CC19, 0x3F4E4BAC, 0x3F49E3DC,
    0x3F459421, 0x3F415BF8, 0x3F3D3AE1, 0x3F39305C,
    0x3F353BEF, 0x3F315D21, 0x3F2D937C, 0x3F29DE8C,
    0x3F263DE0, 0x3F22B109, 0x3F1F379A, 0x3F1BD12A,
    0x3F187D50, 0x3F153BA8, 0x3F120BCD, 0x3F0EED5F,
    0x3F0BDFFD, 0x3F08E34B, 0x3F05F6EE, 0x3F031A8C,
    0x3F004DCE, 0x3EFB20BE, 0x3EF5C3D4, 0x3EF0843C,
    0x3EEB6156, 0x3EE65A84, 0x3EE16F2D, 0x3EDC9EBB,
    0x3ED7E89B, 0x3ED34C3C, 0x3ECEC913, 0x3ECA5E95,
    0x3EC60C3B, 0x3EC1D181, 0x3EBDADE7, 0x3EB9A0EE,
    0x3EB5AA1A, 0x3EB1C8F2, 0x3EADFCFF, 0x3EAA45CE,
    0x3EA6A2ED, 0x3EA313EE, 0x3E9F9862, 0x3E9C2FE1,
    0x3E98DA02, 0x3E95965F, 0x3E926494, 0x3E8F4440,
    0x3E8C3504, 0x3E893681, 0x3E86485D, 0x3E836A3E,
    0x3E809BCC, 0x3E7BB965, 0x3E765939, 0x3E711670,
    0x3E6BF06A, 0x3E66E68A, 0x3E61F836, 0x3E5D24D6,
    0x3E586BD9, 0x3E53CCAD, 0x3E4F46C5, 0x3E4AD998,
    0x3E46849E, 0x3E424752, 0x3E3E2134, 0x3E3A11C4,
    0x3E361887, 0x3E323503, 0x3E2E66C2, 0x3E2AAD4F,
    0x3E270838, 0x3E23770F, 0x3E1FF965, 0x3E1C8ED2,
    0x3E1936EC, 0x3E15F14C, 0x3E12BD91, 0x3E0F9B56,
    0x3E0C8A3E, 0x3E0989E9, 0x3E0699FD, 0x3E03BA20,
    0x3E00E9F9, 0x3DFC5268, 0x3DF6EEF8, 0x3DF1A8FC,
    0x3DEC7FD5, 0x3DE772E5, 0x3DE28191, 0x3DDDAB43,
    0x3DD8EF67, 0x3DD44D6C, 0x3DCFC4C4, 0x3DCB54E6,
    0x3DC6FD4A, 0x3DC2BD6A, 0x3DBE94C7, 0x3DBA82DF,
    0x3DB68738, 0x3DB2A157, 0x3DAED0C5, 0x3DAB150E,
    0x3DA76DC0, 0x3DA3DA6C, 0x3DA05AA3, 0x3D9CEDFC,
    0x3D99940E, 0x3D964C71, 0x3D9316C3, 0x3D8FF2A1,
    0x3D8CDFAB, 0x3D89DD84, 0x3D86EBCF, 0x3D840A32,
};

static inline float f32_from_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* dither accumulators (DITHEROUTPUT) */
static int32_t MonoDitherValue, LeftDitherValue, RightDitherValue;

/* output 4-band EQ (disabled by default, see header comment) */
static int OutputEQEnabled = 0;
static float EQBand[16];        /* LastFilter: 4 bands x stereo x (cur,old) */
static float EQCoeff[8];        /* FilterCoefficients */
static float EQVolume[4];       /* FilterVolumes */
static IT_MAYBE_UNUSED uint8_t VolumeTable[8] = { 0, 16, 96, 127, 0, 0, 0, 0 };

/* MIDI interpreter (SendUARTOut) state */
static uint8_t InterpretState = 0;
static uint8_t InterpretType = 0;

/* per-chunk mixing state (MixBufferOffset / MixBlockSize etc.) */
static int32_t *MixBufPos;
static uint16_t MixBlockSize;

/* volume ramp state (self-modifying code locations in the original) */
static int32_t RVC, LVC;        /* current right/left volume             */
static int32_t RVSet, LVSet;    /* destination right/left volume         */

/* =====================================================================
 * SendUARTOut - MIDIOut handler; interprets F0 F0 <type> <val> locally
 * ===================================================================== */
static void ResetFilters(void)
{
    memset(FilterParameters, 0x7F, 64);
    memset(FilterParameters + 64, 0x00, 64);
}

static void SendUARTOut(hostchn_t *hc, slavechn_t *sc, uint8_t al)
{
    uint8_t ah = InterpretState;

    if (ah >= 2) {
        if (ah == 2) {
            /* expect interpret type (0 = cutoff, 1 = resonance) */
            if (al < 2) {
                InterpretType = al;
                InterpretState++;
                return;
            }
            InterpretState = 0;
            return;
        }

        /* have type; get parameter, then return to normal */
        if (al <= 0x7F) {
            uint16_t bx = (uint16_t)(((InterpretType << 6) +
                                      (hc ? hc->HCN : 0)) & 127);
            FilterParameters[bx] = al;
            if (sc)
                sc->Flags |= SF_RECALC_FINALVOL;
        }
        InterpretState = 0;
        return;
    }

    if (al == 0xF0) {
        InterpretState++;
        return;
    }

    if (ah != 0) {
        InterpretState = 0;
        return;
    }

    /* normal state: MIDI reset messages clear the filters */
    if (al == 0xFC || al == 0xFA || al == 0xFF)
        ResetFilters();
}

/* =====================================================================
 * Cubic interpolation + filter (Get8BitWaveform / Get16BitWaveform)
 * ===================================================================== */
static inline int32_t GetWaveform(const void *smpdata, int is16,
                                  int32_t pos, uint16_t frac,
                                  chnfilter_t *flt)
{
    float t = (float)frac * f32_from_bits(0x37800000u); /* 1/65536 */
    float u, v, w, x;
    float a, b, c, val, y;

    if (is16) {
        const int16_t *s = (const int16_t *)smpdata;
        int32_t p0 = pos;
        if (p0 != 0)            /* Sub BX,1; AdC BX,0 */
            p0--;
        u = (float)s[p0];
        v = (float)s[pos];
        w = (float)s[pos + 1];
        x = (float)s[pos + 2];
    } else {
        const int8_t *s = (const int8_t *)smpdata;
        int32_t p0 = pos;
        if (p0 != 0)
            p0--;
        u = (float)s[p0];
        v = (float)s[pos];
        w = (float)s[pos + 1];
        x = (float)s[pos + 2];
    }

    /* a = x+3v-3w-u; b = 3w+3u-6v; c = 6w-2u-x-3v; d = 6v */
    a = x + 3.0f * v - 3.0f * w - u;
    b = 3.0f * w + 3.0f * u - 6.0f * v;
    c = 6.0f * w - 2.0f * u - x - 3.0f * v;

    val = ((a * t + b) * t + c) * t + 6.0f * v;
    val *= is16 ? f32_from_bits(0x3E2AAAAAu)    /* 1/6   */
                : f32_from_bits(0x422AAAAAu);   /* 256/6 */

    /* resonant filter: y = a.x + b.y1 + c.y2 */
    y = val * flt->fa + flt->y1 * flt->fb + flt->y2 * FilterC;
    flt->y2 = flt->y1;
    flt->y1 = y;

    /* filter clipping (FILTERCLIPPINGTYPECOMPRESSOR) */
    if (fabsf(y) > 32768.0f) {
        int32_t out = (int32_t)llrintf(
            22713.0f * log2f(fabsf(y) * f32_from_bits(0x38000000u)) +
            32768.0f);
        return (y < 0.0f) ? -out : out;
    }

    return (int32_t)llrintf(y);
}

/* =====================================================================
 * Inner mixing loops (Mix32Stereo8/16Bit, Mix32Surround8/16Bit) and the
 * zero-volume position-only variants.
 * pos64 = (position << 16) | fraction, advanced by the 16.16 step.
 * ===================================================================== */
static int64_t MixInner(slavechn_t *sc, const void *smpdata, int is16,
                        int surround, int backwards,
                        int64_t pos64, uint32_t frames)
{
    chnfilter_t *flt = &FilterValues[sc - SChn];
    int32_t *si = MixBufPos;
    uint32_t step = sc->MixStep;
    uint32_t n;

    for (n = frames; n != 0; n--) {
        int32_t pos = (int32_t)(pos64 >> 16);
        uint16_t frac = (uint16_t)pos64;
        int32_t smp = GetWaveform(smpdata, is16, pos, frac, flt);
        int32_t eax;

        if (surround) {
            eax = (int32_t)((uint32_t)smp * (uint32_t)LVC);
            si[0] = (int32_t)((uint32_t)si[0] - (uint32_t)eax);
            LastLeftValue = eax;
            si[1] = (int32_t)((uint32_t)si[1] + (uint32_t)eax);

            LVC += (LVSet - LVC) >> RAMPSPEED;
        } else {
            eax = (int32_t)((uint32_t)smp * (uint32_t)LVC);
            si[0] = (int32_t)((uint32_t)si[0] - (uint32_t)eax);
            LastLeftValue = eax;

            eax = (int32_t)((uint32_t)smp * (uint32_t)RVC);
            si[1] = (int32_t)((uint32_t)si[1] - (uint32_t)eax);
            LastRightValue = eax;

            LVC += (LVSet - LVC) >> RAMPSPEED;
            RVC += (RVSet - RVC) >> RAMPSPEED;
        }

        si += 2;
        if (backwards)
            pos64 -= step;
        else
            pos64 += step;
    }

    if (surround)
        LastRightValue = -LastLeftValue;

    return pos64;
}

/* PreM32SMix8/16Bit / PreM32UMix8/16Bit: set up ramp state and store the
 * end-of-tick volume into the "old volume" words. */
static void PreMix(slavechn_t *sc, int surround)
{
    /* new volumes live in the LeftVolume dword ([SI+0Ch] right,
     * [SI+0Eh] left); old (ramp) volumes in RightVolume ([SI+1Ch]/[1Eh]) */
    uint16_t newR = (uint16_t)(sc->LeftVolume & 0xFFFF);
    uint16_t newL = (uint16_t)((uint32_t)sc->LeftVolume >> 16);
    uint16_t oldR = (uint16_t)(sc->RightVolume & 0xFFFF);
    uint16_t oldL = (uint16_t)((uint32_t)sc->RightVolume >> 16);
    int32_t ax, bx, dx, bp;
    uint16_t cx;

    if (surround) {
        ax = oldL;
        bx = newL;
        LVC = ax;
        if (bx >= ax)
            bx += RAMPCOMPENSATE;
        LVSet = bx;

        for (cx = RealBytesToMix; cx != 0; cx--)
            ax += (bx - ax) >> RAMPSPEED;

        sc->RightVolume = (int32_t)((sc->RightVolume & 0xFFFF) |
                                    ((uint32_t)(uint16_t)ax << 16));
        return;
    }

    ax = newR;
    bx = newL;
    dx = oldR;
    bp = oldL;

    RVC = dx;
    LVC = bp;

    if (ax >= dx)
        ax += RAMPCOMPENSATE;
    if (bx >= bp)
        bx += RAMPCOMPENSATE;

    RVSet = ax;
    LVSet = bx;

    for (cx = RealBytesToMix; cx != 0; cx--) {
        dx += (ax - dx) >> RAMPSPEED;
        bp += (bx - bp) >> RAMPSPEED;
    }

    sc->RightVolume = (int32_t)((uint16_t)dx |
                                ((uint32_t)(uint16_t)bp << 16));
}

/* =====================================================================
 * MFS/MBS: mix "amount" (32.16 fixed) worth of sample advance through
 * the inner loop. Returns the updated position.
 * ===================================================================== */
static int64_t MixAmount(slavechn_t *sc, const void *smpdata, int is16,
                         int surround, int backwards,
                         int64_t pos64, int64_t amount)
{
    uint32_t step = sc->MixStep;
    uint32_t frames;

    if (step == 0 || amount <= 0)
        return pos64;

    /* frames = ceil(amount / step) (Div; Add EDX,-1; AdC EAX,0) */
    frames = (uint32_t)((uint64_t)amount / step);
    if ((uint64_t)amount % step)
        frames++;
    if (frames == 0)
        return pos64;
    if (frames > MixBlockSize)
        frames = MixBlockSize;  /* safety; amounts are derived from
                                   MixBlockSize so this only clips
                                   rounding artefacts */

    pos64 = MixInner(sc, smpdata, is16, surround, backwards, pos64, frames);

    MixBlockSize = (uint16_t)(MixBlockSize - frames);
    MixBufPos += (size_t)frames * 2;

    return pos64;
}

/* Click removal when a non-looping sample runs out (MixNoLoop3/4). */
static void EndOfSampleClickRemoval(void)
{
    int32_t edx = -LastLeftValue;
    int32_t edi = -LastRightValue;
    int32_t *si = MixBufPos;
    uint16_t cx = MixBlockSize;

    for (; cx != 0; cx--) {
        int32_t eax, ebx;

        si[0] = (int32_t)((uint32_t)si[0] + (uint32_t)edx);
        si[1] = (int32_t)((uint32_t)si[1] + (uint32_t)edi);

        eax = edx >> 12;
        ebx = edi >> 12;
        if (eax == 0)
            eax = 1;
        if (ebx == 0)
            ebx = 1;
        edx -= eax;
        edi -= ebx;

        si += 2;
    }

    LastClickRemovalLeft += edx;
    LastClickRemovalRight += edi;
}

static void TurnOffChannel(slavechn_t *sc)
{
    sc->Flags = 0x200;
    if (!(sc->HCN & 0x80))
        HChn[sc->HCOffst & 63].Flags &= (uint16_t)~HF_CHAN_ON;
}

/* =====================================================================
 * Mix*Loop / Update*Loop (MIXWAV.INC)
 * ===================================================================== */
static int64_t GetPos64(slavechn_t *sc)
{
    return ((int64_t)sc->SampleOffset << 16) | sc->SmpErr;
}
static void SetPos64(slavechn_t *sc, int64_t p)
{
    sc->SampleOffset = (int32_t)(p >> 16);
    sc->SmpErr = (uint16_t)p;
}

static void MixNoLoop(slavechn_t *sc, const void *smp, int is16, int sur)
{
    int64_t pos, avail, advance;

    PreMix(sc, sur);

    pos = GetPos64(sc);
    advance = (int64_t)(uint64_t)sc->MixStep * MixBlockSize;
    avail = ((int64_t)sc->LoopEnd << 16) - pos;
    if (avail < 0)
        return;

    if (advance < avail) {
        SetPos64(sc, MixAmount(sc, smp, is16, sur, 0, pos, advance));
        return;
    }

    pos = MixAmount(sc, smp, is16, sur, 0, pos, avail);
    SetPos64(sc, pos);

    /* turn off + click removal */
    TurnOffChannel(sc);
    EndOfSampleClickRemoval();
}

static void MixForwardsLoop(slavechn_t *sc, const void *smp, int is16,
                            int sur)
{
    int64_t pos, avail, advance;

    PreMix(sc, sur);
    pos = GetPos64(sc);

    for (;;) {
        advance = (int64_t)(uint64_t)sc->MixStep * MixBlockSize;
        avail = ((int64_t)sc->LoopEnd << 16) - pos;

        if (avail >= 0) {
            if (advance < avail) {
                pos = MixAmount(sc, smp, is16, sur, 0, pos, advance);
                break;
            }
            pos = MixAmount(sc, smp, is16, sur, 0, pos, avail);
        }

        /* wrap (integer part only, like the original) */
        {
            uint32_t over = (uint32_t)((pos >> 16) - sc->LoopEnd);
            uint32_t len = (uint32_t)(sc->LoopEnd - sc->LoopBeg);

            if ((int32_t)len <= 0)
                break;
            pos = ((int64_t)(sc->LoopBeg + (over % len)) << 16) |
                  (pos & 0xFFFF);
        }

        if ((int16_t)MixBlockSize <= 0)
            break;
    }
    SetPos64(sc, pos);
}

static void MixPingPongLoop(slavechn_t *sc, const void *smp, int is16,
                            int sur)
{
    int64_t pos, avail, advance;

    PreMix(sc, sur);
    pos = GetPos64(sc);

    for (;;) {
        advance = (int64_t)(uint64_t)sc->MixStep * MixBlockSize;

        if (sc->LpD == 0) {
            /* forwards */
            avail = ((int64_t)sc->LoopEnd << 16) - pos;
            if (avail >= 0) {
                if (advance < avail) {
                    pos = MixAmount(sc, smp, is16, sur, 0, pos, advance);
                    break;
                }
                pos = MixAmount(sc, smp, is16, sur, 0, pos, avail);
            }
            /* wrap forwards */
            {
                uint32_t over = (uint32_t)((pos >> 16) - sc->LoopEnd);
                uint32_t len = (uint32_t)(sc->LoopEnd - sc->LoopBeg);
                uint32_t r;

                if ((int32_t)len <= 0)
                    break;
                r = over % (len * 2);
                if (r < len) {
                    sc->LpD = 1;
                    pos = ((int64_t)(sc->LoopEnd - r - 1) << 16) |
                          ((0x10000 - (pos & 0xFFFF)) & 0xFFFF);
                } else {
                    pos = ((int64_t)(sc->LoopBeg + (r - len)) << 16) |
                          (pos & 0xFFFF);
                }
            }
        } else {
            /* backwards */
            avail = pos - ((int64_t)sc->LoopBeg << 16) - 0xFFFF;
            if (avail >= 0) {
                if (advance < avail) {
                    pos = MixAmount(sc, smp, is16, sur, 1, pos, advance);
                    break;
                }
                pos = MixAmount(sc, smp, is16, sur, 1, pos, avail);
            }
            /* wrap backwards */
            {
                uint32_t under = (uint32_t)(sc->LoopBeg - (pos >> 16));
                uint32_t len = (uint32_t)(sc->LoopEnd - sc->LoopBeg);
                uint32_t r;

                if ((int32_t)len <= 0)
                    break;
                r = under % (len * 2);
                if (r < len) {
                    sc->LpD = 0;
                    pos = ((int64_t)(sc->LoopBeg + r) << 16) |
                          ((0x10000 - (pos & 0xFFFF)) & 0xFFFF);
                } else {
                    pos = ((int64_t)(sc->LoopEnd - (r - len) - 1) << 16) |
                          (pos & 0xFFFF);
                }
            }
        }

        if ((int16_t)MixBlockSize <= 0)
            break;
    }
    SetPos64(sc, pos);
}

/* zero-volume position-only updates */
static void UpdateNoLoop(slavechn_t *sc)
{
    int64_t pos = GetPos64(sc) +
                  (int64_t)(uint64_t)sc->MixStep * MixBlockSize;

    if ((int32_t)(pos >> 16) >= sc->LoopEnd) {
        TurnOffChannel(sc);
        if (sc->HCN & 0x80)
            SetPos64(sc, pos);  /* UpdateNoLoop2 falls through when
                                   disowned in the original */
        return;
    }
    SetPos64(sc, pos);
}

static void UpdateForwardsLoop(slavechn_t *sc)
{
    int64_t pos = GetPos64(sc) +
                  (int64_t)(uint64_t)sc->MixStep * MixBlockSize;

    if ((int32_t)(pos >> 16) >= sc->LoopEnd) {
        uint32_t over = (uint32_t)((pos >> 16) - sc->LoopEnd);
        uint32_t len = (uint32_t)(sc->LoopEnd - sc->LoopBeg);

        if (len != 0)
            pos = ((int64_t)(sc->LoopBeg + (over % len)) << 16) |
                  (pos & 0xFFFF);
    }
    SetPos64(sc, pos);
}

static void UpdatePingPongLoop(slavechn_t *sc)
{
    int64_t adv = (int64_t)(uint64_t)sc->MixStep * MixBlockSize;
    int64_t pos = GetPos64(sc);

    if (sc->LpD == 0) {
        pos += adv;
        if ((uint32_t)(pos >> 16) > (uint32_t)sc->LoopEnd) {
            uint32_t over = (uint32_t)((pos >> 16) - sc->LoopEnd);
            uint32_t len = (uint32_t)(sc->LoopEnd - sc->LoopBeg);
            uint32_t r;

            if (len != 0) {
                r = over % (len * 2);
                if (r < len) {
                    sc->LpD = 1;
                    pos = ((int64_t)(sc->LoopEnd - r - 1) << 16) |
                          ((0x10000 - (pos & 0xFFFF)) & 0xFFFF);
                } else {
                    pos = ((int64_t)(sc->LoopBeg + (r - len)) << 16) |
                          (pos & 0xFFFF);
                }
            }
        }
    } else {
        pos -= adv;
        if ((int32_t)(pos >> 16) <= sc->LoopBeg) {
            uint32_t under = (uint32_t)(sc->LoopBeg - (pos >> 16));
            uint32_t len = (uint32_t)(sc->LoopEnd - sc->LoopBeg);
            uint32_t r;

            if (len != 0) {
                r = under % (len * 2);
                if (r < len) {
                    sc->LpD = 0;
                    pos = ((int64_t)(sc->LoopBeg + r) << 16) |
                          ((0x10000 - (pos & 0xFFFF)) & 0xFFFF);
                } else {
                    pos = ((int64_t)(sc->LoopEnd - (r - len) - 1) << 16) |
                          (pos & 0xFFFF);
                }
            }
        }
    }
    SetPos64(sc, pos);
}

/* =====================================================================
 * M32MixHandler - per-tick channel preparation and mixing
 * ===================================================================== */
static void M32MixHandler(void)
{
    int ch;

    /* accumulate the fractional tick length */
    {
        uint64_t nf = (uint64_t)CurrentFractional + BytesToMixFractional;
        uint16_t cx = WBytesToMix;

        if (nf >> 32)
            cx++;
        CurrentFractional = (uint32_t)nf;
        RealBytesToMix = cx;
        if (RealBytesToMix > MAXMIXFRAMES)
            RealBytesToMix = MAXMIXFRAMES;
    }

    /* pre-fill the buffer with the decaying click-removal residue */
    {
        int32_t edx = LastClickRemovalLeft;
        int32_t ebp = LastClickRemovalRight;
        int32_t *si = MixBuffer;
        uint16_t cx;

        for (cx = RealBytesToMix; cx != 0; cx--) {
            int32_t eax = edx >> 12;
            int32_t ebx = ebp >> 12;

            si[0] = edx;
            si[1] = ebp;

            if (eax == 0)
                eax = 1;
            if (ebx == 0)
                ebx = 1;
            edx -= eax;
            ebp -= ebx;

            si += 2;
        }
        LastClickRemovalLeft = edx;
        LastClickRemovalRight = ebp;
    }

    /* channels, last to first */
    for (ch = MAXSLAVECHANNELS - 1; ch >= 0; ch--) {
        slavechn_t *sc = &SChn[ch];
        chnfilter_t *flt = &FilterValues[ch];
        uint16_t cx;
        int row;

        if (!(sc->Flags & SF_CHAN_ON))
            continue;
        if (sc->Smp == 100)     /* MIDI */
            continue;

        cx = sc->Flags;

        if (cx & SF_NOTE_STOP) {
            /* note cut: ramp to zero this tick */
            sc->Flags &= (uint16_t)~SF_CHAN_ON;
            cx &= (uint16_t)~SF_CHAN_ON;
            sc->Vol16b = 0;
            cx |= SF_RECALC_FINALVOL;
        }

        if (cx & SF_FREQ_CHANGE) {
            uint32_t freq = (uint32_t)sc->Frequency;

            if ((freq >> 16) >= MixSpeed) {
                /* M32MixHandlerError */
                TurnOffChannel(sc);
                continue;
            }
            sc->MixStep = (uint32_t)(((uint64_t)freq << 16) / MixSpeed);
        }

        if (cx & SF_NEW_NOTE) {
            sc->RightVolume = 0;        /* old volumes = 0 (ramp in) */
            flt->y1 = 0.0f;
            flt->y2 = 0.0f;
            flt->fa = 1.0f;
            flt->fb = 0.0f;
        }

        if (cx & (SF_RECALC_FINALVOL | SF_LOOP_CHANGED | SF_PAN_CHANGED)) {
            /* fetch filter parameters */
            uint8_t cutoff, reso;

            if (sc->HCN & 0x80) {
                cutoff = (uint8_t)(sc->VEnv.CurNode >> 8);
                reso = (uint8_t)(sc->MBank >> 8);
            } else {
                cutoff = FilterParameters[sc->HCN & 63];
                reso = FilterParameters[(sc->HCN & 63) + 64];
                sc->VEnv.CurNode =
                    (uint16_t)((sc->VEnv.CurNode & 0xFF) | (cutoff << 8));
                sc->MBank = (uint16_t)((sc->MBank & 0xFF) | (reso << 8));
            }

            {
                uint16_t freqvalue =
                    (uint16_t)((uint8_t)sc->MBank * cutoff);
                uint16_t ax = (uint16_t)((freqvalue >> 8) +
                                         ((freqvalue & 0x80) ? 1 : 0));
                ax |= (uint16_t)(reso << 8);

                if (ax != 0x7F) {
                    /* compute filter coefficients (single precision) */
                    extern uint32_t DebugFilterCalcs;
                    float r;

                    DebugFilterCalcs++;
                    r = exp2f((float)freqvalue *
                              f32_from_bits(FREQPARAMMUL_BITS)) *
                        FreqMultiplier;
                    float q = f32_from_bits(QualityFactorTable[reso & 127]);
                    float d = q * r + q - 1.0f;
                    float e = r * r;
                    float fa = 1.0f / (1.0f + d + e);

                    flt->fa = fa;
                    flt->fb = (d + 2.0f * e) * fa;
                }
            }

            /* compute volumes + select mixing row */
            if (cx & SF_CHN_MUTED)
                goto ZeroVolume;

            if (Stereo && sc->FPP != 100) {
                uint32_t t;
                uint16_t volR, volL;

                t = (uint32_t)sc->FPP * (uint8_t)MixVolume;
                t = (t & 0xFFFF) * sc->Vol16b;
                volR = (uint16_t)(t >> 14);

                t = (uint32_t)(64 - sc->FPP) * (uint8_t)MixVolume;
                t = (t & 0xFFFF) * sc->Vol16b;
                volL = (uint16_t)(t >> 14);

                sc->LeftVolume = (int32_t)(volR | ((uint32_t)volL << 16));

                if (sc->LeftVolume != 0) {
                    row = 30;
                    goto HaveRow;
                }
ZeroVolume:
                sc->LeftVolume = 0;
                if (sc->RightVolume == 0) {
                    row = 0;
                    goto HaveRow;
                }
                row = 30;
                goto HaveRow;
            } else {
                /* surround/mono */
                uint32_t t = (uint32_t)sc->Vol16b * MixVolume;
                uint16_t vol = (uint16_t)(t >> 8);

                if (Stereo)
                    vol >>= 1;

                sc->LeftVolume = (int32_t)(vol | ((uint32_t)vol << 16));

                if (vol == 0) {
                    if (sc->RightVolume == 0)
                        goto ZeroVolume; /* -> row 0 via LeftVolume==0 */
                }
                row = 60;
                goto HaveRow;
            }

HaveRow:
            if (sc->Bit & 2)
                row += 90;
            if (sc->LpM >= 8) {
                row += 10;
                if (sc->LpM > 8)
                    row += 10;
            }
            sc->MixMode = (uint16_t)row;
        }

        /* call the mixing routine */
        row = sc->MixMode;
        MixBlockSize = RealBytesToMix;
        MixBufPos = MixBuffer;
        sc->OldSampleOffset = (uint32_t)sc->SampleOffset;

        if (StartNoRamp && (cx & SF_NEW_NOTE) && sc->SampleOffset == 0)
            sc->RightVolume = sc->LeftVolume;

        /* load filter c for this chunk: c = 1 - a - b */
        FilterC = 1.0f - flt->fa - flt->fb;

        {
            sample_t *s = SAMPLEHDR(sc->SmpOffs);
            const void *smp = s->Data;
            int is16 = (row >= 90);
            int volpart = (is16 ? row - 90 : row);
            int looppart = volpart % 30;
            int sur;

            volpart -= looppart;
            sur = (volpart == 60);

            if (smp == NULL) {
                TurnOffChannel(sc);
                continue;
            }

            if (volpart == 0) {
                if (looppart == 0)
                    UpdateNoLoop(sc);
                else if (looppart == 10)
                    UpdateForwardsLoop(sc);
                else
                    UpdatePingPongLoop(sc);
            } else {
                if (looppart == 0)
                    MixNoLoop(sc, smp, is16, sur);
                else if (looppart == 10)
                    MixForwardsLoop(sc, smp, is16, sur);
                else
                    MixPingPongLoop(sc, smp, is16, sur);
            }
        }

        sc->Flags &= 0x788D;

        /* sample mode: mirror filter state into the ghost channel */
        if (!(Song.Header.Flags & ITF_INSTRUMENTS) && ch < 64)
            FilterValues[ch + 64] = *flt;
    }

    /* output EQ (4-band crossover) - bypassed unless configured */
    if (OutputEQEnabled) {
        int32_t *si = MixBuffer;
        uint16_t cx;

        for (cx = RealBytesToMix; cx != 0; cx--, si += 2) {
            float l = (float)si[0];
            float r = (float)si[1];
            float outl = 0.0f, outr = 0.0f;
            float prevl = 0.0f, prevr = 0.0f;
            int band;

            for (band = 0; band < 4; band++) {
                float a = EQCoeff[band * 2];
                float b = EQCoeff[band * 2 + 1];
                float nl = l * a + EQBand[band * 4 + 0] * b;
                float nr = r * a + EQBand[band * 4 + 1] * b;

                EQBand[band * 4 + 0] = nl;
                EQBand[band * 4 + 1] = nr;

                outl += (nl - prevl) * EQVolume[band];
                outr += (nr - prevr) * EQVolume[band];
                prevl = nl;
                prevr = nr;
            }
            si[0] = (int32_t)llrintf(outl);
            si[1] = (int32_t)llrintf(outr);
        }
    }
}

/* =====================================================================
 * Driver entry points
 * ===================================================================== */
static void WAV_SetTempo(uint8_t TempoVal)
{
    uint32_t eax = (uint32_t)MixSpeed * 2 + (MixSpeed >> 1);
    uint32_t ebx = TempoVal;

    if (ebx == 0)
        ebx = 1;

    WBytesToMix = (uint16_t)(eax / ebx);
    BytesToMixFractional = eax % ebx;
}

static void WAV_SetMixVolume(uint8_t Vol)
{
    MixVolume = Vol;
}

static void WAV_SetStereo(uint8_t s)
{
    StereoSet = s;
}

/* declared below; reset here so a fresh InitSound starts the output FIFO
 * clean (matters for deterministic offline renders / re-inits). */
static uint32_t OutFilled, OutPos;

static int WAV_InitSound(void)
{
    LastClickRemovalLeft = 0;
    LastClickRemovalRight = 0;
    NumClipped = 0;
    CurrentFractional = 0;
    MonoDitherValue = 0;
    LeftDitherValue = 0;
    RightDitherValue = 0;
    InterpretState = 0;
    OutFilled = 0;
    OutPos = 0;

    memset(FilterValues, 0, sizeof(FilterValues));
    memset(EQBand, 0, sizeof(EQBand));
    ResetFilters();

    FreqMultiplier = (float)MixSpeed * f32_from_bits(FREQMULTIPLIER_BASE_BITS);
    return 1;
}

static void WAV_UninitSound(void)
{
}

const sounddriver_t WAVDriver = {
    "WAV writer (hiqual) driver",
    256,                /* DefaultChannels / MaxChannels */
    3,                  /* DriverFlags: MIDI out + hiqual */
    WAV_InitSound,
    WAV_UninitSound,
    WAV_SetTempo,
    WAV_SetMixVolume,
    WAV_SetStereo,
    SendUARTOut,
};

/* ---- render API ------------------------------------------------------ */

static int16_t OutBuffer[MAXMIXFRAMES * 2];
/* OutFilled / OutPos are declared and reset up in WAV_InitSound. */

void WAVDriver_SetMixSpeed(uint32_t hz)
{
    if (hz < 8000)
        hz = 8000;
    if (hz > 64000)
        hz = 64000;
    MixSpeed = (uint16_t)hz;
}

uint32_t WAVDriver_GetMixSpeed(void)
{
    return MixSpeed;
}

uint32_t WAVDriver_GetClipped(void)
{
    return NumClipped;
}

uint32_t DebugFilterCalcs = 0;

/* one tick: Update() + mix + convert to 16-bit (Poll equivalent) */
static void DoTick(void)
{
    uint16_t cx;
    int32_t *si;
    int16_t *di;

    if (PlayMode == 0)
        Stereo = StereoSet;     /* stereo switch applies when idle */

    /* Serialise the sequencer tick (which reads packed pattern data and
     * channel tables) against editor pattern edits on the main thread. */
    if (Engine_Lock)
        Engine_Lock();
    Update();
    M32MixHandler();
    if (Engine_Unlock)
        Engine_Unlock();

    /* convert (DITHEROUTPUT path: error-feedback) */
    si = MixBuffer;
    di = OutBuffer;

    if (Stereo) {
        int32_t edx = LeftDitherValue;
        int32_t ebp = RightDitherValue;

        for (cx = RealBytesToMix; cx != 0; cx--) {
            int32_t eax, ebx;

            edx += si[0];
            ebp += si[1];

            eax = edx >> 14;
            edx &= 0x3FFF;
            ebx = ebp >> 14;
            ebp &= 0x3FFF;

            if (eax < -0x8000) { eax = -0x8000; NumClipped++; }
            else if (eax > 0x7FFF) { eax = 0x7FFF; NumClipped++; }
            if (ebx < -0x8000) { ebx = -0x8000; NumClipped++; }
            else if (ebx > 0x7FFF) { ebx = 0x7FFF; NumClipped++; }

            di[0] = (int16_t)eax;
            di[1] = (int16_t)ebx;
            si += 2;
            di += 2;
        }
        LeftDitherValue = edx;
        RightDitherValue = ebp;
    } else {
        int32_t ebx = MonoDitherValue;

        for (cx = RealBytesToMix; cx != 0; cx--) {
            int32_t eax;

            ebx += si[0];
            eax = ebx >> 14;
            ebx &= 0x3FFF;

            if (eax < -0x8000) { eax = -0x8000; NumClipped++; }
            else if (eax > 0x7FFF) { eax = 0x7FFF; NumClipped++; }

            di[0] = (int16_t)eax;   /* duplicate mono to both outputs */
            di[1] = (int16_t)eax;
            si += 2;
            di += 2;
        }
        MonoDitherValue = ebx;
    }

    OutFilled = RealBytesToMix;
    OutPos = 0;
}

/* passive output tap for the Alt-F12 spectrum analyser (feature 013):
 * the last 2048 rendered frames, mixed down to mono (the original's
 * GetWaveform driver hook, DriverFlags bit 2 semantics). Written only
 * from the render path; the analyser copies it unlocked -- a torn
 * frame just smears one FFT column, as on real hardware. */
static int16_t WaveTap[2048];
static uint32_t WaveTapPos;

void WAVDriver_GetWaveForm(int16_t *out2048)
{
    uint32_t i, p = WaveTapPos;

    for (i = 0; i < 2048; i++)
        out2048[i] = WaveTap[(p + i) & 2047];
}

void WAVDriver_Render(int16_t *dst, uint32_t frames)
{
    while (frames != 0) {
        uint32_t n, i;

        if (OutPos >= OutFilled)
            DoTick();

        n = OutFilled - OutPos;
        if (n > frames)
            n = frames;

        memcpy(dst, &OutBuffer[OutPos * 2], n * 4);
        for (i = 0; i < n; i++) {       /* mono tap: (L+R)/2 */
            WaveTap[WaveTapPos] =
                (int16_t)((dst[i * 2] + dst[i * 2 + 1]) >> 1);
            WaveTapPos = (WaveTapPos + 1) & 2047;
        }
        dst += n * 2;
        OutPos += n;
        frames -= n;
    }
}
