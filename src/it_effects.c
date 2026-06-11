/*
 * it_effects.c
 * ------------
 * 1:1 C transliteration of IT_M_EFF.INC (Impulse Tracker 2.17 effect
 * handlers). Label names and control flow follow the original assembly;
 * gotos are used deliberately where the original jumps between effect
 * handlers, because that *is* the structure of the engine.
 *
 * Register conventions of the original mapped to locals:
 *   CL = hc->Msk working copy, CH = low byte of hc->Flags working copy
 *   (written back with "Mov [DI], CH" - reproduced explicitly).
 */

#include <string.h>
#include "it_music.h"

/* SlideTable (IT_M_EFF.INC line 3) */
static const uint8_t SlideTable[9] = { 1, 4, 8, 16, 32, 64, 96, 128, 255 };

/* Misc effect data accessors ([DI+40h]..[DI+4Fh]) */
static inline uint16_t GetMiscW(const hostchn_t *hc, int off)
{
    uint16_t v;
    memcpy(&v, &hc->MiscEfctData[off - 0x40], 2);
    return v;
}
static inline void SetMiscW(hostchn_t *hc, int off, uint16_t v)
{
    memcpy(&hc->MiscEfctData[off - 0x40], &v, 2);
}
#define MiscB(hc, off) ((hc)->MiscEfctData[(off) - 0x40])

static void InitCommandX2(hostchn_t *hc, uint8_t al);
static void InitCommandG11(hostchn_t *hc);
static void InitCommandM2(hostchn_t *hc, uint8_t al);
static void InitVibrato(hostchn_t *hc);
static void InitTremelo(hostchn_t *hc);

/* =====================================================================
 * CommandD2 - shared volume store tail (CommandD/volume column slides)
 * ===================================================================== */
static void CommandD2v(hostchn_t *hc, slavechn_t *sc, uint8_t al)
{
    sc->Vol = al;
    sc->VS = al;
    hc->VSe = al;
    sc->Flags |= SF_RECALC_VOL;
}

/* =====================================================================
 * InitVolumeEffect - volume column effects (A..H), IT_M_EFF.INC line 61
 * ===================================================================== */
void InitVolumeEffect(hostchn_t *hc)
{
    uint8_t al, ah, efx, param;
    slavechn_t *sc;

    if (!(hc->Msk & 0x44))
        return;

    ah = hc->Vol;
    if ((uint8_t)(ah & 0x7F) < 65)
        return;
    al = (uint8_t)((ah & 0x7F) - 65);
    if (ah & 0x80)
        al += 60;

    efx = al / 10;              /* effect number  */
    param = al % 10;            /* effect parameter */
    hc->VCm = efx;

    /* effect memory */
    if (param != 0) {
        if (efx < 4) {
            hc->VCmVal = param;             /* InitNormalMemory   */
        } else if (efx < 6) {
            hc->EFG = (uint8_t)(param << 2);/* InitSlideMemory    */
        } else if (efx == 6) {
            uint8_t dl = SlideTable[param - 1];
            if (Song.Header.Flags & ITF_LINK_G_TO_EF)
                hc->GOE = dl;
            else
                hc->EFG = dl;
        }
        /* efx == 7: no memory here (handled in the H path below) */
    }

    /* InitVolumeEffectNoMemory */
    if (!(hc->Flags & HF_CHAN_ON)) {
        /* InitVolumeEffect3 */
        if (efx != 7)
            return;
        goto EffectH;
    }
    sc = SLAVE(hc);

    if (efx <= 1) {
        /* A: fine volume slide up / B: fine volume slide down */
        al = hc->VCmVal;
        if (efx == 1) {
            /* B: AL = VS - param, floor 0 */
            int16_t v = (int16_t)sc->VS - al;
            al = (v < 0) ? 0 : (uint8_t)v;
        } else {
            uint16_t v = (uint16_t)(al + sc->VS);
            al = (v > 64) ? 64 : (uint8_t)v;
        }
        CommandD2v(hc, sc, al);
        return;
    }

    hc->Flags |= HF_UPDATE_VOLEFCT_IF_CHAN_ON;

    if (efx < 6)                /* C, D, E, F: tick updates only */
        return;
    if (efx == 6) {             /* G: tone portamento */
        InitCommandG11(hc);
        return;
    }

EffectH:
    {
        uint8_t depth = (uint8_t)(param << 2);
        if (depth != 0)
            hc->VDp = depth;
    }
    if (hc->Flags & HF_CHAN_ON)
        InitVibrato(hc);
}

/* =====================================================================
 * Volume column tick updates
 * ===================================================================== */
void VolumeCommandC(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    uint16_t v = (uint16_t)(hc->VCmVal + sc->VS);

    if (v > 64) {
        hc->Flags &= (uint16_t)~HF_UPDATE_VOLEFCT_IF_CHAN_ON;
        v = 64;
    }
    CommandD2v(hc, sc, (uint8_t)v);
}

void VolumeCommandD(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    int16_t v = (int16_t)sc->VS - hc->VCmVal;

    if (v < 0) {
        hc->Flags &= (uint16_t)~HF_UPDATE_VOLEFCT_IF_CHAN_ON;
        v = 0;
    }
    CommandD2v(hc, sc, (uint8_t)v);
}

void VolumeCommandE(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);

    PitchSlideDown(hc, sc, (int16_t)(hc->EFG << 2));
    sc->FrequencySet = sc->Frequency;
}

void VolumeCommandF(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);

    PitchSlideUp(hc, sc, (int16_t)(hc->EFG << 2));
    sc->FrequencySet = sc->Frequency;
}

void VolumeCommandG(hostchn_t *hc)
{
    slavechn_t *sc;
    uint16_t bx;

    if (!(hc->Flags & HF_PITCH_SLIDE_ONGOING))
        return;

    bx = (Song.Header.Flags & ITF_LINK_G_TO_EF) ? hc->GOE : hc->EFG;
    if (bx == 0)
        return;
    bx <<= 2;

    sc = SLAVE(hc);

    if (MiscB(hc, 0x42) == 1) {
        /* slide up */
        PitchSlideUp(hc, sc, (int16_t)bx);
        if (!(sc->Flags & SF_NOTE_STOP)) {
            if ((uint32_t)sc->Frequency < (uint32_t)hc->PortaFreq) {
                sc->FrequencySet = sc->Frequency;
                return;
            }
        }
        sc->Flags &= (uint16_t)~SF_NOTE_STOP;
        hc->Flags |= HF_CHAN_ON;
        hc->Flags &= (uint16_t)~0x110;  /* turn off calling */
        sc->Frequency = sc->FrequencySet = hc->PortaFreq;
    } else {
        /* slide down */
        PitchSlideDown(hc, sc, (int16_t)bx);
        if ((uint32_t)sc->Frequency > (uint32_t)hc->PortaFreq) {
            sc->FrequencySet = sc->Frequency;
            return;
        }
        hc->Flags &= (uint16_t)~0x110;
        sc->Frequency = sc->FrequencySet = hc->PortaFreq;
    }
}

/* =====================================================================
 * InitNoCommand - note handling without (or before) an effect.
 * IT_M_EFF.INC line 315/355.
 * ===================================================================== */
void InitNoCommand(hostchn_t *hc)
{
    uint8_t cl = hc->Msk;
    uint8_t ch = (uint8_t)hc->Flags;
    slavechn_t *sc = NULL;
    uint8_t al;

    if (!(cl & 0x33))
        goto NoOldEffect;

    /* note (or instrument) present - check for noteoff/cut/fade */
    al = hc->Nt2;
    if (al >= 120) {
        /* InitNoCommand4 */
        if (!(ch & 4))
            goto NoOldEffect;
        sc = SLAVE(hc);

        if (al > 0xFE) {                    /* 255: note off */
            sc->Flags |= SF_NOTE_OFF;
            goto InitNoCommand11;
        }
        if (al != 0xFE) {                   /* 120..253: note fade */
            sc->Flags |= SF_FADEOUT;
            goto NoOldEffect;
        }
        /* 254: note cut */
        ch &= (uint8_t)~4;
        if (sc->Smp == 100 || (DriverFlags & 2))
            sc->Flags |= SF_NOTE_STOP;      /* cut with ramp-down */
        else
            sc->Flags = 0x200;              /* instant cut */
        goto NoOldEffect;
    }

    if ((ch & 4) && !(cl & 0x11)) {
        /* instrument only, channel on: same note+ins = retrigger only */
        sc = SLAVE(hc);
        if (sc->Nte == hc->Nte && sc->Ins == hc->Ins)
            goto NoOldEffect;
    }

    /* InitNoCommand9: volume column portamento bypasses allocation */
    if ((cl & 0x44) && hc->Vol >= 193 && hc->Vol <= 202 &&
        (hc->Flags & HF_CHAN_ON)) {
        InitVolumeEffect(hc);
        return;
    }

    /* NoVolumePorta */
    sc = AllocateChannelPtr(hc, &ch);
    if (!sc)
        goto NoOldEffect;

    /* channel allocated - put volume */
    sc->Vol = sc->VS = hc->VSe;

    if (!(Song.Header.Flags & ITF_INSTRUMENTS)) {
        /* sample mode: default sample pan */
        sample_t *s = SAMPLEHDR(sc->SmpOffs);
        if (s->DfP & 0x80) {
            uint8_t p = s->DfP & 0x7F;
            hc->CP = p;
            sc->Pan = p;
            sc->PS = p;
        }
    }

    {
        sample_t *s = SAMPLEHDR(sc->SmpOffs);

        sc->OldSampleOffset = 0;
        sc->SmpErr = 0;
        sc->SampleOffset = 0;

        sc->Frequency = sc->FrequencySet =
            (int32_t)(((uint64_t)s->C5Speed * PitchTable[hc->Nt2]) >> 16);
    }

    ch |= 4;
    ch &= (uint8_t)~16;

InitNoCommand11:
    GetLoopInformation(sc);

    /* InitNoCommand1: old effects = instrument-present retriggers env */
    if (cl & 0x66) {
        if ((Song.Header.Flags & (ITF_INSTRUMENTS | ITF_OLD_EFFECTS)) ==
                (ITF_INSTRUMENTS | ITF_OLD_EFFECTS) &&
            (cl & 0x22) && hc->Ins != 0xFF && hc->Ins != 0) {
            sc->FadeOut = 0x400;
            InitPlayInstrument(hc, sc, hc->Ins);
        }
    }

NoOldEffect:
    if (cl & 0x44) {
        al = hc->Vol;
        if (al <= 64)
            goto InitNoCommand8;
        if ((uint8_t)((al & 0x7F)) < 65) {
            /* InitNoCommandPanning: 128..192 */
            hc->Flags = (uint16_t)((hc->Flags & 0xFF00) | ch);
            InitCommandX2(hc, (uint8_t)(al - 128));
        }
    }

    /* InitNoCommand7 */
    if (cl & 0x22) {
        if (hc->Smp != 0) {
            sample_t *s = SAMPLEHDR(hc->Smp - 1);
            al = s->Vol;        /* default volume */
            goto InitNoCommand8;
        }
    }
    goto InitNoCommand3;

InitNoCommand8:
    hc->VSe = al;
    if (ch & 4) {
        sc = SLAVE(hc);
        sc->Vol = al;
        sc->VS = al;
        sc->Flags |= SF_RECALC_VOL;
    }

InitNoCommand3:
    {
        int bit80 = (hc->Flags & HF_APPLY_RANDOM_VOL) != 0;
        hc->Flags = (uint16_t)((hc->Flags & 0xFF00) | ch);
        if (bit80)
            ApplyRandomValues(hc);
    }

    InitVolumeEffect(hc);
}

/* =====================================================================
 * Command init handlers A..Z
 * ===================================================================== */
void InitCommandA(hostchn_t *hc)        /* set speed */
{
    uint16_t ax = hc->CmdVal;

    if (ax != 0) {
        CurrentTick = (uint16_t)(CurrentTick - CurrentSpeed + ax);
        ProcessTick = (uint16_t)(ProcessTick - CurrentSpeed + ax);
        CurrentSpeed = ax;
    }
    InitNoCommand(hc);
}

void InitCommandB(hostchn_t *hc)        /* jump to order */
{
    uint16_t ax = hc->CmdVal;

    if (ax <= CurrentOrder)
        StopSong = 1;

    ProcessOrder = (uint16_t)(ax - 1);
    ProcessRow = 0xFFFE;
    InitNoCommand(hc);
}

void InitCommandC(hostchn_t *hc)        /* break to row */
{
    if (PatternLooping == 0) {
        BreakRow = hc->CmdVal;
        ProcessRow = 0xFFFE;
    }
    InitNoCommand(hc);
}

/* InitCommandD7: shared with Kxx/Lxx */
static void InitCommandD7(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    uint8_t al = hc->DKL;
    uint8_t hi, lo;

    sc->Flags |= SF_RECALC_VOL;

    lo = al & 0x0F;
    hi = al & 0xF0;

    if (lo == 0) {
        /* Dx0: slide up */
        al >>= 4;
        hc->VCh = al;
        hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
        if (al == 0x0F)
            CommandD(hc);
        return;
    }
    if (hi == 0) {
        /* D0x: slide down */
        hc->VCh = (uint8_t)-al;
        hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
        if ((int8_t)hc->VCh == -15)
            CommandD(hc);
        return;
    }
    if (lo == 0x0F) {
        /* DxF: fine slide up */
        uint16_t v;
        hc->VCh = 0;
        v = (uint16_t)((al >> 4) + sc->VS);
        if (v > 64)
            v = 64;
        sc->Vol = (uint8_t)v;
        sc->VS = (uint8_t)v;
        hc->VSe = (uint8_t)v;
        return;
    }
    if (hi == 0xF0) {
        /* DFx: fine slide down */
        int16_t v;
        hc->VCh = 0;
        v = (int16_t)sc->VS - lo;
        if (v < 0)
            v = 0;
        sc->Vol = (uint8_t)v;
        sc->VS = (uint8_t)v;
        hc->VSe = (uint8_t)v;
        return;
    }
    /* invalid combined nibbles: no action (InitCommandD6) */
}

void InitCommandD(hostchn_t *hc)        /* volume slide */
{
    InitNoCommand(hc);

    if (hc->CmdVal != 0)
        hc->DKL = hc->CmdVal;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    InitCommandD7(hc);
}

void InitCommandE(hostchn_t *hc)        /* pitch slide down */
{
    uint8_t al, ah;
    slavechn_t *sc;

    InitNoCommand(hc);

    if (hc->CmdVal != 0)
        hc->EFG = hc->CmdVal;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    sc = SLAVE(hc);
    al = hc->EFG;
    if (al == 0)
        return;

    ah = al & 0xF0;
    if (ah >= 0xE0) {
        /* EEx extra fine / EFx fine: immediate */
        al &= 0x0F;
        if (al == 0)
            return;
        if (ah != 0xE0)
            al <<= 2;
        PitchSlideDown(hc, sc, al);
        sc->FrequencySet = sc->Frequency;
        return;
    }

    SetMiscW(hc, 0x40, (uint16_t)(al << 2));
    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
}

void InitCommandF(hostchn_t *hc)        /* pitch slide up */
{
    uint8_t al, ah;
    slavechn_t *sc;

    InitNoCommand(hc);

    if (hc->CmdVal != 0)
        hc->EFG = hc->CmdVal;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    sc = SLAVE(hc);
    al = hc->EFG;
    if (al == 0)
        return;

    ah = al & 0xF0;
    if (ah >= 0xE0) {
        al &= 0x0F;
        if (al == 0)
            return;
        if (ah != 0xE0)
            al <<= 2;
        PitchSlideUp(hc, sc, al);
        sc->FrequencySet = sc->Frequency;
        return;
    }

    SetMiscW(hc, 0x40, (uint16_t)(al << 2));
    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
}

/* InitCommandG11: the Gxx setup, also entered from Lxx and the volume
 * column G effect. */
static void InitCommandG11(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    uint8_t cl = hc->Msk;
    uint8_t al;
    sample_t *s;

    if ((cl & 0x22) && hc->Smp != 0) {
        uint8_t smp = (uint8_t)(hc->Smp - 1);

        if (Song.Header.Flags & ITF_LINK_G_TO_EF) {
            /* InitGXXCompat1: keep playing sample, retake its volume */
            hc->Smp = (uint8_t)(sc->Smp + 1);
            s = SAMPLEHDR(sc->Smp);
            sc->SVl = (uint8_t)(s->GvL * 2);
            goto InitCommandG18;
        }

        if (hc->Smp == 101)
            goto InitCommandG13;        /* don't overwrite note if MIDI */

        {
            int ins_same = (sc->Ins == hc->Ins);
            sc->Nte = hc->Nte;
            sc->Ins = hc->Ins;

            if (ins_same) {
                if (smp == sc->Smp)
                    goto InitCommandG13;        /* G17 */
                goto InitCommandG16;
            }
            /* InitCommandG19 */
            if (smp == sc->Smp)
                goto InitCommandG18;
        }

InitCommandG16:
        /* sample changed: swap sample data under the playing note */
        s = SAMPLEHDR(smp);
        sc->Flags = (uint16_t)((sc->Flags & 0x00FF) | 0x0100);

        sc->SmpOffs = smp;
        sc->Smp = smp;

        sc->ViDepth = 0;
        sc->LpD = 0;
        sc->OldSampleOffset = 0;
        sc->SmpErr = 0;
        sc->SampleOffset = 0;

        sc->SVl = (uint8_t)(s->GvL * 2);

        if (!(s->Flags & 1)) {
            /* InitCommandGNoSample */
            sc->Flags = 0x200;
            hc->Flags &= (uint16_t)~HF_CHAN_ON;
            return;
        }

        sc->Bit = s->Flags & 2;
        sc->ViP = 0;

        GetLoopInformation(sc);
        cl = hc->Msk;

InitCommandG18:
        if (Song.Header.Flags & ITF_INSTRUMENTS) {
            uint16_t was_on;
            instrument_t *in;

            sc->FadeOut = 0x400;

            was_on = sc->Flags & 1;
            InitPlayInstrument(hc, sc, hc->Ins);
            if (was_on & 1)
                sc->Flags &= (uint16_t)~SF_NEW_NOTE;

            in = INSTRUMENT(hc->Ins);
            sc->SVl = (uint8_t)(((uint16_t)in->GbV * sc->SVl) >> 7);
        }
        goto InitCommandG14;
    }

InitCommandG13:
    if (!(cl & 0x11))
        goto InitCommandG1;

InitCommandG14:
    al = hc->Nt2;
    if (al > 119) {
        /* InitCommandG12: noteoff/cut/fade in a Gxx row */
        if (!(hc->Flags & HF_CHAN_ON))
            goto InitCommandG1;

        if (al > 0xFE) {
            sc->Flags |= SF_NOTE_OFF;
            GetLoopInformation(sc);
            goto InitCommandG1;
        }
        if (al == 0xFE) {
            hc->Flags &= (uint16_t)~HF_CHAN_ON;
            sc->Flags = 0x200;
            goto InitCommandG1;
        }
        sc->Flags |= SF_FADEOUT;
        goto InitCommandG1;
    }

    /* InitCommandG5 */
    if (hc->Smp != 101)
        sc->Nte = al;

    s = SAMPLEHDR(sc->SmpOffs);
    hc->PortaFreq =
        (int32_t)(((uint64_t)s->C5Speed * PitchTable[al]) >> 16);
    hc->Flags |= HF_PITCH_SLIDE_ONGOING;

InitCommandG1:
    if (cl & 0x44) {
        al = hc->Vol;
        if (al <= 64)
            goto InitCommandG4;
        if ((uint8_t)(al & 0x7F) < 65)
            InitCommandX2(hc, (uint8_t)(al - 128));     /* panning */
        goto InitCommandG2;
    }

InitCommandG2:
    if (cl & 0x22) {
        s = SAMPLEHDR(sc->SmpOffs);
        al = s->Vol;
        goto InitCommandG4;
    }
    goto InitCommandG3;

InitCommandG4:
    sc->Flags |= SF_RECALC_VOL;
    sc->Vol = al;
    sc->VS = al;
    hc->VSe = al;

InitCommandG3:
    if (hc->Flags & HF_PITCH_SLIDE_ONGOING) {
        uint16_t ax = (Song.Header.Flags & ITF_LINK_G_TO_EF) ? hc->GOE
                                                             : hc->EFG;
        ax <<= 2;
        if (ax != 0) {
            uint32_t pf = (uint32_t)hc->PortaFreq;
            uint32_t fs = (uint32_t)sc->FrequencySet;

            SetMiscW(hc, 0x40, ax);

            if (pf != fs) {
                MiscB(hc, 0x42) = (pf > fs) ? 1 : 0;

                if (!(hc->Flags & HF_UPDATE_VOLEFCT_IF_CHAN_ON))
                    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
            }
        }
    }

    /* InitCommandGEnd: don't call volume effects if it has a Gxx */
    if (!(hc->Flags & HF_UPDATE_VOLEFCT_IF_CHAN_ON))
        InitVolumeEffect(hc);
}

void InitCommandG(hostchn_t *hc)        /* tone portamento */
{
    if (hc->CmdVal != 0) {
        if (Song.Header.Flags & ITF_LINK_G_TO_EF)
            hc->GOE = hc->CmdVal;
        else
            hc->EFG = hc->CmdVal;
    }

    if (!(hc->Flags & HF_CHAN_ON)) {
        InitNoCommand(hc);
        return;
    }
    InitCommandG11(hc);
}

void InitCommandH(hostchn_t *hc)        /* vibrato */
{
    uint8_t al, lo, hi;

    if ((hc->Msk & 0x11) && hc->Nte <= 119) {
        hc->VPo = 0;
        hc->LVi = 0;
    }

    al = hc->CmdVal;
    lo = al & 0x0F;             /* depth nibble  */
    hi = al & 0xF0;             /* speed nibble  */

    if ((lo | hi) != 0) {
        if (hi != 0)
            hc->VSp = (uint8_t)(hi >> 2);   /* speed*4 */

        if (lo != 0) {
            uint8_t depth = (uint8_t)(lo << 2);
            if (Song.Header.Flags & ITF_OLD_EFFECTS)
                depth = (uint8_t)(depth << 1);
            hc->VDp = depth;
        }
    }

    InitNoCommand(hc);

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
    InitVibrato(hc);
}

void InitCommandI(hostchn_t *hc)        /* tremor */
{
    InitNoCommand(hc);

    if (hc->CmdVal != 0)
        hc->I00 = hc->CmdVal;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;

    {
        uint8_t off = hc->I00 & 0x0F;
        uint8_t on  = (uint8_t)(hc->I00 >> 4);

        if (Song.Header.Flags & ITF_OLD_EFFECTS) {
            off++;
            on++;
        }
        MiscB(hc, 0x40) = off;
        MiscB(hc, 0x41) = on;
    }

    CommandI(hc);
}

void InitCommandJ(hostchn_t *hc)        /* arpeggio */
{
    uint8_t al;

    InitNoCommand(hc);

    SetMiscW(hc, 0x40, 0);

    al = hc->CmdVal;
    if (al == 0)
        al = hc->J00;
    else
        hc->J00 = al;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;

    /* the original stores pointers into PitchTable+240 (= entry 60);
     * we store the table indices instead. */
    SetMiscW(hc, 0x44, (uint16_t)(60 + (al & 0x0F)));
    SetMiscW(hc, 0x42, (uint16_t)(60 + (al >> 4)));
}

void InitCommandK(hostchn_t *hc)        /* vibrato + volume slide */
{
    if (hc->CmdVal != 0)
        hc->DKL = hc->CmdVal;

    InitNoCommand(hc);

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    InitVibrato(hc);
    InitCommandD7(hc);
    hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
}

void InitCommandL(hostchn_t *hc)        /* porta + volume slide */
{
    if (hc->CmdVal != 0)
        hc->DKL = hc->CmdVal;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    InitCommandG11(hc);
    InitCommandD7(hc);
    hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
}

static void InitCommandM2(hostchn_t *hc, uint8_t al)
{
    if (hc->Flags & HF_CHAN_ON) {
        slavechn_t *sc = SLAVE(hc);
        sc->CVl = al;
        sc->Flags |= SF_RECALC_VOL;
    }
    hc->CV = al;
}

void InitCommandM(hostchn_t *hc)        /* set channel volume */
{
    InitNoCommand(hc);

    if (hc->CmdVal <= 0x40)
        InitCommandM2(hc, hc->CmdVal);
}

void InitCommandN(hostchn_t *hc)        /* channel volume slide */
{
    uint8_t al, lo, hi;

    if (hc->CmdVal != 0)
        hc->N00 = hc->CmdVal;

    InitNoCommand(hc);

    al = hc->N00;
    lo = al & 0x0F;
    hi = al & 0xF0;

    if (lo == 0) {
        MiscB(hc, 0x40) = (uint8_t)(al >> 4);
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;
    }
    if (hi == 0) {
        MiscB(hc, 0x40) = (uint8_t)-al;
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;
    }
    if (lo == 0x0F) {
        uint16_t v = (uint16_t)((al >> 4) + hc->CV);
        InitCommandM2(hc, (v > 64) ? 64 : (uint8_t)v);
        return;
    }
    if (hi == 0xF0) {
        int16_t v = (int16_t)hc->CV - lo;
        InitCommandM2(hc, (v < 0) ? 0 : (uint8_t)v);
        return;
    }
}

void InitCommandO(hostchn_t *hc)        /* set sample offset */
{
    if (hc->CmdVal != 0)
        hc->O00 = hc->CmdVal;

    InitNoCommand(hc);

    if (!(hc->Msk & 0x33))
        return;
    if (hc->Nt2 >= 120)
        return;
    if (!(hc->Flags & HF_CHAN_ON))
        return;

    {
        slavechn_t *sc = SLAVE(hc);
        uint32_t eax = ((uint32_t)hc->OxH << 16) | ((uint32_t)hc->O00 << 8);

        if (eax >= (uint32_t)sc->LoopEnd) {
            if (!(Song.Header.Flags & ITF_OLD_EFFECTS))
                return;
            eax = (uint32_t)sc->LoopEnd - 1;
        }
        sc->SampleOffset = (int32_t)eax;
        sc->OldSampleOffset = eax;
        sc->SmpErr = 0;
    }
}

void InitCommandP(hostchn_t *hc)        /* pan slide */
{
    uint8_t al, lo, hi, dl;

    if (hc->CmdVal != 0)
        hc->P00 = hc->CmdVal;

    InitNoCommand(hc);

    dl = hc->CP;
    if (hc->Flags & HF_CHAN_ON)
        dl = SLAVE(hc)->PS;

    if (dl == 100)              /* surround */
        return;

    al = hc->P00;
    lo = al & 0x0F;
    hi = al & 0xF0;

    if (lo == 0) {
        /* Px0: slide towards 0 */
        MiscB(hc, 0x40) = (uint8_t)-(al >> 4);
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;
    }
    if (hi == 0) {
        /* P0x */
        MiscB(hc, 0x40) = lo;
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;
    }
    if (lo == 0x0F) {
        /* PxF: fine, towards 0 */
        int16_t v = (int16_t)dl - (hi >> 4);
        InitCommandX2(hc, (v < 0) ? 0 : (uint8_t)v);
        return;
    }
    if (hi == 0xF0) {
        /* PFx: fine, towards 64 */
        uint16_t v = (uint16_t)(lo + dl);
        InitCommandX2(hc, (v > 64) ? 64 : (uint8_t)v);
        return;
    }
}

void InitCommandQ(hostchn_t *hc)        /* retrigger */
{
    InitNoCommand(hc);

    if (hc->CmdVal != 0)
        hc->Q00 = hc->CmdVal;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;

    if (hc->Msk & 0x11) {
        hc->RTC = hc->Q00 & 0x0F;
        return;
    }
    CommandQ(hc);
}

void InitCommandR(hostchn_t *hc)        /* tremolo */
{
    uint8_t al = hc->CmdVal;
    uint8_t lo = al & 0x0F, hi = al & 0xF0;

    if ((lo | hi) != 0) {
        if (hi != 0)
            hc->TSp = (uint8_t)(hi >> 2);
        if (lo != 0)
            hc->TDp = (uint8_t)(lo << 1);
    }

    InitNoCommand(hc);

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
    InitTremelo(hc);
}

/* ---- Sxx ---- */
void InitCommandS(hostchn_t *hc)
{
    uint8_t al = hc->CmdVal;
    uint8_t cmd, val;

    if (al == 0)
        al = hc->S00;
    hc->S00 = al;

    cmd = al & 0xF0;
    val = al & 0x0F;

    MiscB(hc, 0x40) = val;
    MiscB(hc, 0x41) = cmd;

    switch (cmd >> 4) {
    default:
    case 0x0: case 0x1: case 0x2:
        InitNoCommand(hc);
        return;

    case 0x3:                   /* set vibrato waveform */
        if (val <= 3)
            hc->VWF = val;
        InitNoCommand(hc);
        return;

    case 0x4:                   /* set tremolo waveform */
        if (val <= 3)
            hc->TWF = val;
        InitNoCommand(hc);
        return;

    case 0x5:                   /* set panbrello waveform */
        if (val <= 3) {
            hc->PWF = val;
            hc->PPo = 0;
        }
        InitNoCommand(hc);
        return;

    case 0x6:                   /* extra delay of x frames */
        CurrentTick = (uint16_t)(CurrentTick + val);
        ProcessTick = (uint16_t)(ProcessTick + val);
        InitNoCommand(hc);
        return;

    case 0x7:                   /* instrument functions */
        switch (val) {
        case 0x0: {             /* past note cut */
            slavechn_t *sc = SChn;
            uint16_t cx;
            uint8_t target = hc->HCN | 0x80;

            InitNoCommand(hc);
            for (cx = NumChannels; cx != 0; cx--, sc++) {
                if (sc->HCN != target)
                    continue;
                if (DriverFlags & 2)
                    sc->Flags |= SF_NOTE_STOP;
                else
                    sc->Flags = 0x200;
            }
            return;
        }
        case 0x1:               /* past note off */
        case 0x2: {             /* past note fade */
            slavechn_t *sc = SChn;
            uint16_t cx;
            uint8_t target = hc->HCN | 0x80;
            uint16_t flag = (val == 1) ? SF_NOTE_OFF : SF_FADEOUT;

            InitNoCommand(hc);
            for (cx = NumChannels; cx != 0; cx--, sc++) {
                if (sc->HCN != target)
                    continue;
                sc->Flags |= flag;
                GetLoopInformation(sc);
            }
            return;
        }
        case 0x3: case 0x4: case 0x5: case 0x6: /* set NNA */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->NNA = (uint8_t)(val - 3);
            return;
        case 0x7:               /* volume envelope on */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->Flags &= (uint16_t)~SF_VOLENV_ON;
            return;
        case 0x8:               /* volume envelope off */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->Flags |= SF_VOLENV_ON;
            return;
        case 0x9:               /* pan envelope on */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->Flags &= (uint16_t)~SF_PANENV_ON;
            return;
        case 0xA:               /* pan envelope off */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->Flags |= SF_PANENV_ON;
            return;
        case 0xB:               /* pitch envelope on */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->Flags &= (uint16_t)~SF_PITCHENV_ON;
            return;
        case 0xC:               /* pitch envelope off */
            InitNoCommand(hc);
            if (hc->Flags & HF_CHAN_ON)
                SLAVE(hc)->Flags |= SF_PITCHENV_ON;
            return;
        default:
            InitNoCommand(hc);
            return;
        }

    case 0x8: {                 /* set pan */
        uint16_t ax = (uint16_t)((uint8_t)(val | (val << 4)));
        ax = (uint16_t)((ax + 2) >> 2);
        InitNoCommand(hc);
        InitCommandX2(hc, (uint8_t)ax);
        return;
    }

    case 0x9:                   /* set surround */
        if (val == 1) {
            InitNoCommand(hc);
            InitCommandX2(hc, 100);
        } else {
            InitNoCommand(hc);
        }
        return;

    case 0xA:                   /* set high order offset */
        hc->OxH = val;
        InitNoCommand(hc);
        return;

    case 0xB:                   /* pattern loop */
        InitNoCommand(hc);
        if (val != 0) {
            if (hc->PLC == 0) {
                hc->PLC = val;
            } else {
                hc->PLC--;
                if (hc->PLC == 0) {
                    hc->PLR = (uint8_t)(CurrentRow + 1);
                    return;
                }
            }
            ProcessRow = (uint16_t)(hc->PLR - 1);
            PatternLooping = 1;
        } else {
            hc->PLR = (uint8_t)CurrentRow;
        }
        return;

    case 0xC:                   /* note cut */
        hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
        InitNoCommand(hc);
        return;

    case 0xD:                   /* note delay */
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;

    case 0xE:                   /* pattern delay */
        if (RowDelayOn == 0) {
            RowDelay = (uint8_t)(val + 1);
            RowDelayOn = 1;
        }
        InitNoCommand(hc);
        return;

    case 0xF:                   /* MIDI macro select */
        hc->SFx = val;
        InitNoCommand(hc);
        return;
    }
}

void InitCommandT(hostchn_t *hc)        /* tempo */
{
    uint8_t al = hc->CmdVal;

    if (al != 0)
        hc->T00 = al;
    al = hc->T00;

    if (al >= 0x20) {
        Tempo = al;
        Music_InitTempo();
        InitNoCommand(hc);
        return;
    }

    /* tempo slide */
    InitNoCommand(hc);
    hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
}

void InitCommandU(hostchn_t *hc)        /* fine vibrato */
{
    uint8_t al, lo, hi;

    if ((hc->Msk & 0x11) != 0) {
        hc->VPo = 0;
        hc->LVi = 0;
    }

    al = hc->CmdVal;
    lo = al & 0x0F;
    hi = al & 0xF0;

    if (hi != 0)
        hc->VSp = (uint8_t)(hi >> 2);

    if (lo != 0) {
        uint8_t depth = lo;
        if (Song.Header.Flags & ITF_OLD_EFFECTS)
            depth = (uint8_t)(depth << 1);
        hc->VDp = depth;
    }

    InitNoCommand(hc);

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
    InitVibrato(hc);
}

void InitCommandV(hostchn_t *hc)        /* set global volume */
{
    if (hc->CmdVal <= 0x80) {
        GlobalVolume = hc->CmdVal;
        RecalculateAllVolumes();
    }
    InitNoCommand(hc);
}

void InitCommandW(hostchn_t *hc)        /* global volume slide */
{
    uint8_t al, lo, hi;

    InitNoCommand(hc);

    al = hc->CmdVal;
    if (al == 0)
        al = hc->W00;
    else
        hc->W00 = al;

    lo = al & 0x0F;
    hi = al & 0xF0;

    if ((lo | hi) == 0)
        return;

    if (hi == 0) {
        /* W0x: slide down */
        MiscB(hc, 0x40) = (uint8_t)-lo;
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;
    }
    if (lo == 0) {
        /* Wx0: slide up */
        MiscB(hc, 0x40) = (uint8_t)(hi >> 4);
        hc->Flags |= HF_ALWAYS_UPDATE_EFCT;
        return;
    }
    if (hi == 0xF0) {
        /* WFx: fine slide down */
        int16_t v = (int16_t)GlobalVolume - lo;
        GlobalVolume = (v < 0) ? 0 : (uint8_t)v;
        RecalculateAllVolumes();
        return;
    }
    if (lo == 0x0F) {
        /* WxF: fine slide up */
        int16_t v = (int16_t)GlobalVolume + (hi >> 4);
        GlobalVolume = (v > 128) ? 128 : (uint8_t)v;
        RecalculateAllVolumes();
        return;
    }
}

static void InitCommandX2(hostchn_t *hc, uint8_t al)
{
    if (hc->Flags & HF_CHAN_ON) {
        slavechn_t *sc = SLAVE(hc);
        sc->Pan = al;
        sc->PS = al;
        sc->Flags |= SF_RECALC_FINALVOL | SF_RECALC_PAN;
    }
    hc->CP = al;
}

void InitCommandX(hostchn_t *hc)        /* set pan */
{
    uint16_t ax;

    InitNoCommand(hc);

    ax = (uint16_t)((hc->CmdVal + 2) >> 2);
    InitCommandX2(hc, (uint8_t)ax);
}

void InitCommandY(hostchn_t *hc)        /* panbrello */
{
    uint8_t al = hc->CmdVal;
    uint8_t lo = al & 0x0F, hi = al & 0xF0;

    if ((lo | hi) != 0) {
        if (hi != 0)
            hc->PSp = (uint8_t)(hi >> 4);
        if (lo != 0)
            hc->PDp = (uint8_t)(lo << 1);
    }

    InitNoCommand(hc);

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
    CommandY(hc);
}

void InitCommandZ(hostchn_t *hc)        /* MIDI macro */
{
    uint8_t bl;
    slavechn_t *sc;

    InitNoCommand(hc);

    bl = hc->CmdVal;
    sc = (hc->SCOffst < MAXSLAVECHANNELS) ? SLAVE(hc) : NULL;

    if (!(bl & 0x80)) {
        /* parameterised macro SFx (macros start at 120h) */
        uint16_t bx = (uint16_t)(0x120 + ((hc->SFx & 0x0F) << 5));
        MIDITranslate(hc, sc, bx);
    } else {
        /* fixed macros Z80..ZFF (start at 320h) */
        uint16_t bx = (uint16_t)(0x320 + ((bl & 0x7F) << 5));
        MIDITranslate(hc, sc, bx);
    }
}

/* =====================================================================
 * Tick update handlers
 * ===================================================================== */
void NoCommand(hostchn_t *hc)
{
    (void)hc;
}

void CommandD(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    int16_t v = (int16_t)(int8_t)hc->VCh + sc->VS;

    if (v < 0) {
        hc->Flags &= (uint16_t)~HF_UPDATE_EFCT_IF_CHAN_ON;
        v = 0;
    } else if (v > 64) {
        hc->Flags &= (uint16_t)~HF_UPDATE_EFCT_IF_CHAN_ON;
        v = 64;
    }
    CommandD2v(hc, sc, (uint8_t)v);
}

void CommandE(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);

    PitchSlideDown(hc, sc, (int16_t)GetMiscW(hc, 0x40));
    sc->FrequencySet = sc->Frequency;
}

void CommandF(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);

    PitchSlideUp(hc, sc, (int16_t)GetMiscW(hc, 0x40));
    sc->FrequencySet = sc->Frequency;
}

void CommandG(hostchn_t *hc)
{
    slavechn_t *sc;
    int16_t bx;

    if (!(hc->Flags & HF_PITCH_SLIDE_ONGOING))
        return;

    bx = (int16_t)GetMiscW(hc, 0x40);
    sc = SLAVE(hc);

    if (MiscB(hc, 0x42) == 1) {
        /* slide up */
        PitchSlideUp(hc, sc, bx);

        if (!(sc->Flags & SF_NOTE_STOP)) {
            if ((uint32_t)sc->Frequency < (uint32_t)hc->PortaFreq) {
                sc->FrequencySet = sc->Frequency;
                return;
            }
        }
        sc->Flags &= (uint16_t)~SF_NOTE_STOP;
        hc->Flags |= HF_CHAN_ON;
        hc->Flags &= (uint16_t)~(3 | 16);       /* turn off calling */
        sc->Frequency = sc->FrequencySet = hc->PortaFreq;
    } else {
        /* slide down */
        PitchSlideDown(hc, sc, bx);

        if ((uint32_t)sc->Frequency > (uint32_t)hc->PortaFreq) {
            sc->FrequencySet = sc->Frequency;
            return;
        }
        hc->Flags &= (uint16_t)~(3 | 16);
        sc->Frequency = sc->FrequencySet = hc->PortaFreq;
    }
}

/* CommandH5: shared vibrato application tail. Note that a resulting
 * slide of 0 still goes through PitchSlideUp (sets the freq-change
 * flag), exactly like the original's JNS into CommandH3. */
static void CommandH5(hostchn_t *hc, slavechn_t *sc, int8_t al)
{
    int16_t ax;
    uint8_t ah;

    ax = (int16_t)(al * (int8_t)hc->VDp);   /* IMul Byte Ptr [DI+3Ah] */
    ax = (int16_t)(ax << 2);                /* SAL AX, 2              */
    ax = (int16_t)(ax + 0x80);              /* Add AX, 80h            */

    ah = (uint8_t)((uint16_t)ax >> 8);
    if (Song.Header.Flags & ITF_OLD_EFFECTS)
        ah = (uint8_t)-ah;                  /* Neg AH                 */

    if ((int8_t)ah < 0) {
        /* CommandH4: Neg BL then PitchSlideDown */
        PitchSlideDown(hc, sc, (uint8_t)-(int8_t)ah);
    } else {
        /* CommandH3 (also taken when ah == 0) */
        PitchSlideUp(hc, sc, ah);
    }
}

/* InitVibrato: entry that respects old-effects mode */
static void InitVibrato(hostchn_t *hc)
{
    slavechn_t *sc;

    if (!(Song.Header.Flags & ITF_OLD_EFFECTS)) {
        CommandH(hc);
        return;
    }

    sc = SLAVE(hc);
    sc->Flags |= SF_FREQ_CHANGE;
    CommandH5(hc, sc, (int8_t)hc->LVi);     /* use last vibrato value */
}

void CommandH(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    int8_t al;

    sc->Flags |= SF_FREQ_CHANGE;

    {
        uint8_t bl = (uint8_t)(hc->VPo + hc->VSp);
        hc->VPo = bl;

        if (hc->VWF == 3)
            al = (int8_t)((Random() & 127) - 64);
        else
            al = FineWaveData[((hc->VWF & 3) << 8) + bl];
    }

    hc->LVi = (uint8_t)al;
    CommandH5(hc, sc, al);
}

void CommandI(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);

    sc->Flags |= SF_RECALC_VOL;

    hc->TCD--;
    if ((int8_t)hc->TCD <= 0) {
        hc->Too ^= 1;
        hc->TCD = MiscB(hc, 0x40 + (hc->Too & 1));
    }

    if (hc->Too != 1)
        sc->Vol = 0;
}

void CommandJ(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    uint16_t bx = GetMiscW(hc, 0x40);
    uint64_t prod;

    sc->Flags |= SF_FREQ_CHANGE;

    bx += 2;
    if (bx >= 6) {
        SetMiscW(hc, 0x40, 0);
        return;
    }
    SetMiscW(hc, 0x40, bx);

    prod = (uint64_t)(uint32_t)sc->Frequency *
           PitchTable[GetMiscW(hc, (int)(0x40 + bx))];

    if ((prod >> 48) != 0) {            /* Test EDX, 0FFFF0000h */
        sc->Frequency = 0;
        return;
    }
    sc->Frequency = (int32_t)(prod >> 16);
}

void CommandK(hostchn_t *hc)
{
    CommandH(hc);
    CommandD(hc);
}

void CommandL(hostchn_t *hc)
{
    if (hc->Flags & HF_PITCH_SLIDE_ONGOING) {
        CommandG(hc);
        hc->Flags |= HF_UPDATE_EFCT_IF_CHAN_ON;
    }
    CommandD(hc);
}

void CommandN(hostchn_t *hc)
{
    int16_t v = (int16_t)hc->CV + (int8_t)MiscB(hc, 0x40);

    if (v < 0)
        v = 0;
    else if (v > 64)
        v = 64;
    InitCommandM2(hc, (uint8_t)v);
}

void CommandP(hostchn_t *hc)
{
    uint8_t al = hc->CP;
    int16_t v;

    if (hc->Flags & HF_CHAN_ON)
        al = SLAVE(hc)->PS;

    v = (int16_t)al + (int8_t)MiscB(hc, 0x40);
    if (v < 0)
        v = 0;
    else if (v > 64)
        v = 64;
    InitCommandX2(hc, (uint8_t)v);
}

void CommandQ(hostchn_t *hc)
{
    slavechn_t *sc;
    uint8_t bl, type;
    int16_t al;

    hc->RTC--;
    if ((int8_t)hc->RTC > 0)
        return;

    /* reset counter */
    bl = hc->Q00 & 0x0F;
    type = (uint8_t)(hc->Q00 >> 4);
    hc->RTC = bl;

    sc = SLAVE(hc);

    if (DriverFlags & 2) {
        /* hiqual: keep the old note ringing while retriggering */
        if (Song.Header.Flags & ITF_INSTRUMENTS) {
            /* find a free channel for the copy */
            slavechn_t *di = SChn;
            uint16_t cx;
            int found = 0;

            for (cx = NumChannels; cx != 0; cx--, di++) {
                if (!(di->Flags & SF_CHAN_ON)) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                memcpy(di, sc, sizeof(slavechn_t));
                sc->Flags |= SF_NOTE_STOP;      /* cut original */
                sc->HCN |= 0x80;                /* disown original */
                sc = di;
                hc->SCOffst = (uint16_t)(sc - SChn);
            }
        } else {
            /* sample mode: ghost channel at +64 */
            uint16_t idx = (uint16_t)(sc - SChn);
            memcpy(&SChn[idx + 64], sc, sizeof(slavechn_t));
            SChn[idx + 64].Flags |= SF_NOTE_STOP;
            SChn[idx + 64].HCN |= 0x80;
        }
    }

    /* CommandQNoHiQual */
    sc->OldSampleOffset = 0;
    sc->SmpErr = 0;
    sc->SampleOffset = 0;
    sc->Flags |= 0x540;     /* loop changed + new note + recalc finalvol */

    al = sc->VS;
    switch (type) {
    case 0x0: return;
    case 0x1: al -= 1;  goto CheckLow;
    case 0x2: al -= 2;  goto CheckLow;
    case 0x3: al -= 4;  goto CheckLow;
    case 0x4: al -= 8;  goto CheckLow;
    case 0x5: al -= 16; goto CheckLow;
    case 0x6: al = (int16_t)((uint8_t)(al << 1) / 3); goto End;
    case 0x7: al = (uint8_t)al >> 1; goto End;
    case 0x8: return;
    case 0x9: al += 1;  goto CheckHigh;
    case 0xA: al += 2;  goto CheckHigh;
    case 0xB: al += 4;  goto CheckHigh;
    case 0xC: al += 8;  goto CheckHigh;
    case 0xD: al += 16; goto CheckHigh;
    case 0xE: al = (int16_t)((uint8_t)((uint8_t)(al + al) + al) >> 1);
              goto CheckHigh;
    case 0xF: al = (uint8_t)(al << 1); goto CheckHigh;
    default:  return;
    }

CheckLow:
    if ((int8_t)al < 0)
        al = 0;
    goto End;

CheckHigh:
    if ((uint8_t)al > 64)
        al = 64;

End:
    sc->Vol = (uint8_t)al;
    sc->VS = (uint8_t)al;
    hc->VSe = (uint8_t)al;
    sc->Flags |= SF_RECALC_VOL;

    if (hc->Smp == 101)
        MIDITranslate(hc, sc, MIDICOMMAND_STOPNOTE);
}

/* InitTremelo: entry that respects old-effects mode */
static void InitTremelo(hostchn_t *hc)
{
    slavechn_t *sc;
    int16_t ax;
    int16_t v;

    if (!(Song.Header.Flags & ITF_OLD_EFFECTS)) {
        CommandR(hc);
        return;
    }

    sc = SLAVE(hc);
    sc->Flags |= SF_RECALC_FINALVOL;

    /* CommandR2 with AL = last tremolo */
    ax = (int16_t)((int8_t)hc->LTr * (int8_t)hc->TDp);
    ax = (int16_t)((ax << 2) + 0x80);

    v = (int16_t)sc->Vol + (int8_t)(ax >> 8);
    if (v < 0)
        v = 0;
    else if (v > 64)
        v = 64;
    sc->Vol = (uint8_t)v;
}

void CommandR(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    int8_t al;
    int16_t ax, v;

    sc->Flags |= SF_RECALC_VOL;

    {
        uint8_t bl = (uint8_t)(hc->TPo + hc->TSp);
        hc->TPo = bl;

        if (hc->TWF == 3)
            al = (int8_t)((Random() & 127) - 64);
        else
            al = FineWaveData[((hc->TWF & 3) << 8) + bl];
    }

    hc->LTr = (uint8_t)al;

    /* CommandR2 */
    ax = (int16_t)(al * (int8_t)hc->TDp);
    ax = (int16_t)((ax << 2) + 0x80);

    v = (int16_t)sc->Vol + (int8_t)(ax >> 8);
    if (v < 0)
        v = 0;
    else if (v > 64)
        v = 64;
    sc->Vol = (uint8_t)v;
}

void CommandS(hostchn_t *hc)
{
    uint8_t ah = MiscB(hc, 0x41);

    if (ah == 0xD0) {
        /* note delay */
        MiscB(hc, 0x40)--;
        if ((int8_t)MiscB(hc, 0x40) > 0)
            return;

        hc->Flags &= (uint16_t)~3;
        InitNoCommand(hc);
        hc->Flags |= HF_ROW_UPDATED;

        if ((Song.Header.ChnlPan[hc->HCN] & 0x80) &&
            !(hc->Flags & HF_FREEPLAY_NOTE) && (hc->Flags & HF_CHAN_ON)) {
            SLAVE(hc)->Flags |= SF_CHN_MUTED;
        }
        return;
    }

    if (ah == 0xC0) {
        /* note cut */
        slavechn_t *sc;

        if (!(hc->Flags & HF_CHAN_ON))
            return;

        MiscB(hc, 0x40)--;
        if ((int8_t)MiscB(hc, 0x40) > 0)
            return;

        sc = SLAVE(hc);
        hc->Flags &= (uint16_t)~HF_CHAN_ON;

        if (sc->Smp == 100 || (DriverFlags & 2))
            sc->Flags |= SF_NOTE_STOP;
        else
            sc->Flags = 0x200;
        return;
    }
}

void CommandT(hostchn_t *hc)
{
    uint8_t al = hc->T00;
    int16_t bx = Tempo;

    if (al & 0xF0) {
        /* slide up */
        bx = (int16_t)(bx + al - 0x10);
        if (bx > 0xFF)
            bx = 0xFF;
    } else {
        /* slide down */
        bx = (int16_t)(bx - al);
        if (bx < 0x20)
            bx = 0x20;
    }
    Tempo = (uint8_t)bx;
    if (Driver && Driver->SetTempo)
        Driver->SetTempo(Tempo);
}

void CommandW(hostchn_t *hc)
{
    int16_t v = (int16_t)GlobalVolume + (int8_t)MiscB(hc, 0x40);

    if (v < 0)
        v = 0;
    else if (v > 128)
        v = 128;
    GlobalVolume = (uint8_t)v;
    RecalculateAllVolumes();
}

void CommandY(hostchn_t *hc)
{
    slavechn_t *sc;
    int8_t al;
    int16_t ax, v;

    if (!(hc->Flags & HF_CHAN_ON))
        return;

    sc = SLAVE(hc);

    if (hc->PWF < 3) {
        uint8_t bl = (uint8_t)(hc->PPo + hc->PSp);
        hc->PPo = bl;
        al = FineWaveData[((hc->PWF & 3) << 8) + bl];
    } else {
        /* random: speed = delay time */
        hc->PPo--;
        if ((int8_t)hc->PPo > 0) {
            al = (int8_t)hc->LPn;
        } else {
            hc->PPo = hc->PSp;
            al = (int8_t)((Random() & 127) - 64);
            hc->LPn = (uint8_t)al;
        }
    }

    ax = (int16_t)(al * (int8_t)hc->PDp);
    ax = (int16_t)((ax << 2) + 0x80);

    if (sc->PS == 100)          /* surround */
        return;

    v = (int16_t)sc->PS + (int8_t)(ax >> 8);
    if (v < 0)
        v = 0;
    else if (v > 64)
        v = 64;

    sc->Flags |= SF_RECALC_PAN;
    sc->Pan = (uint8_t)v;
}

/* =====================================================================
 * Dispatch tables (IT_MUSIC.ASM line 599)
 * ===================================================================== */
const effect_fn InitCommandTable[32] = {
    InitNoCommand, InitCommandA, InitCommandB, InitCommandC,
    InitCommandD,  InitCommandE, InitCommandF, InitCommandG,
    InitCommandH,  InitCommandI, InitCommandJ, InitCommandK,
    InitCommandL,  InitCommandM, InitCommandN, InitCommandO,
    InitCommandP,  InitCommandQ, InitCommandR, InitCommandS,
    InitCommandT,  InitCommandU, InitCommandV, InitCommandW,
    InitCommandX,  InitCommandY, InitCommandZ, InitNoCommand,
    InitNoCommand, InitNoCommand, InitNoCommand, InitNoCommand,
};

const effect_fn CommandTable[32] = {
    NoCommand, NoCommand, NoCommand, NoCommand,
    CommandD,  CommandE,  CommandF,  CommandG,
    CommandH,  CommandI,  CommandJ,  CommandK,
    CommandL,  NoCommand, CommandN,  NoCommand,
    CommandP,  CommandQ,  CommandR,  CommandS,
    CommandT,  CommandH,  NoCommand, CommandW,
    NoCommand, CommandY,  NoCommand, NoCommand,
    NoCommand, NoCommand, NoCommand, NoCommand,
};

const effect_fn VolumeEffectTable[8] = {
    NoCommand, NoCommand,
    VolumeCommandC, VolumeCommandD,
    VolumeCommandE, VolumeCommandF,
    VolumeCommandG, CommandH,
};
