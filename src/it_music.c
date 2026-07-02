/*
 * it_music.c
 * ----------
 * 1:1 C transliteration of the Impulse Tracker 2.17 playback engine
 * (IT_MUSIC.ASM by Jeffrey Lim). Function and label names follow the
 * original so the port can be audited side by side with the assembly.
 *
 * Differences from the DOS original, all mechanical:
 *  - segment:offset addressing replaced by pointers/indices
 *    (instrument offsets -> 1-based instrument numbers in InsOffs,
 *     sample header offsets -> 0-based sample indices in SmpOffs,
 *     host/slave channel offsets -> table indices in HCOffst/SCOffst,
 *     0xFFFF in HCOffst = "no host channel", where the original used 0).
 *  - FPU code (USEFPUCODE=1, as in the 2.17 release) is emulated with
 *    doubles; FIST rounding (round-to-nearest-even) and the 0x80000000
 *    out-of-range result are reproduced explicitly.
 *  - EMS pattern storage, driver .DRV loading, screens and the editor
 *    APIs are not part of the playback core and live elsewhere/not yet.
 */

#include <string.h>
#include <math.h>
#include "it_music.h"

/* ---- Globals (IT_MUSIC.ASM data area) ------------------------------- */

song_t   Song;
char     MIDIDataArea[MIDIDATAAREA_SIZE];

hostchn_t  HChn[64];
slavechn_t SChn[MAXSLAVECHANNELS];
uint8_t    MuteChannelTable[64];

uint16_t LastSample    = 0;
uint16_t PlayMode      = 0;
uint16_t CurrentOrder  = 0;
uint16_t CurrentPattern= 0;
uint16_t CurrentRow    = 0;
uint16_t ProcessOrder  = 0;
uint16_t ProcessRow    = 0;
uint16_t BreakRow      = 0;
uint8_t  RowDelay      = 0;
uint8_t  RowDelayOn    = 0;

uint16_t NumberOfRows  = 64;
uint16_t CurrentTick   = 6;
uint16_t CurrentSpeed  = 6;
uint16_t ProcessTick   = 0;
uint8_t  Tempo         = 125;
uint8_t  GlobalVolume  = 128;
uint16_t NumChannels   = 256;
uint8_t  SoloSample    = 0xFF;
uint8_t  SoloInstrument= 0xFF;
uint8_t  StopSong      = 0;
uint8_t  PatternLooping= 0;
uint8_t  ReverseChannels = 0;
uint8_t  OrderLockFlag = 0;

uint16_t DecodeExpectedPattern = 0xFFFE;
uint16_t DecodeExpectedRow     = 0xFFFE;

slavechn_t *LastSlaveChannel = NULL;

const sounddriver_t *Driver = NULL;
uint16_t DriverFlags = 0;
uint16_t StopEndOfPlaySection = 0;

/* Pattern decode position (PatternOffset/PatternSegment in the original;
 * the editor's "arrayed" pattern path does not exist in this port). */
static const uint8_t *PatternDataPos = NULL;

/* EmptyPattern: 64 rows of nothing (IT_MUSIC.ASM line 255). */
static const uint8_t EmptyPatternData[64] = {0};

/* MIDI state */
static uint8_t  LastMIDIByte = 0xFF;
static uint16_t MIDIPitchDepthSent = 0;
static uint8_t  DoMIDICycle = 0;
static uint8_t  MIDIPrograms[16];
static uint16_t MIDIBanks[16];
static uint8_t  MIDIPan[16];
static uint16_t MIDIPitch[16];

/* AllocateChannel working variables */
static uint16_t AllocateNumChannels;
static uint16_t AllocateSlaveOffset;     /* slave channel start index */

#if USEFPUCODE
/* Const1_On_768 is stored as the single-precision bit pattern 3AAAAAABh
 * in the original; reproduce that exact constant. */
static double Const1_On_768_value(void)
{
    static double v = 0.0;
    if (v == 0.0) {
        union { uint32_t u; float f; } c;
        c.u = 0x3AAAAAABu;
        v = (double)c.f;
    }
    return v;
}
#define Const_14317456 14317456.0 /* 1712*8363 */
#endif

/* =====================================================================
 * RecalculateAllVolumes
 * ===================================================================== */
void RecalculateAllVolumes(void)
{
    slavechn_t *sc = SChn;
    for (uint16_t cx = NumChannels; cx != 0; cx--, sc++)
        sc->Flags |= 18; /* recalc pan + recalc vol */
}

/* =====================================================================
 * Random - bit-exact port of the 16-bit generator (Seed1/Seed2).
 * Returns AL.
 * ===================================================================== */
static uint16_t Seed1 = 0x1234;
static uint16_t Seed2 = 0x5678;

static inline uint16_t rol16(uint16_t x, unsigned n)
{
    n &= 15;
    return n ? (uint16_t)((x << n) | (x >> (16 - n))) : x;
}

void Music_ResetRNG(void)
{
    Seed1 = 0x1234;
    Seed2 = 0x5678;
}

uint8_t Random(void)
{
    uint16_t ax = Seed1, bx = Seed2, cx = bx, dx = bx;

    ax = (uint16_t)(ax + bx);
    ax = rol16(ax, cx & 31);        /* Rol AX, CL (count masked mod 32)  */
    ax ^= dx;
    cx = (uint16_t)((cx >> 8) | (cx << 8)); /* XChg CL, CH               */
    bx = (uint16_t)(bx + cx);
    dx = (uint16_t)(dx + bx);
    cx = (uint16_t)(cx + ax);
    {
        uint16_t cf = bx & 1;       /* Ror BX, 1 -> CF = old bit 0       */
        bx = (uint16_t)((bx >> 1) | (bx << 15));
        (void)bx;
        ax = (uint16_t)(ax - dx - cf); /* SBB AX, DX                     */
    }
    Seed2 = dx;
    Seed1 = ax;
    return (uint8_t)ax;
}

/* =====================================================================
 * MIDISendFilter / SetFilterCutoff / SetFilterResonance
 * ===================================================================== */
static void MIDISendFilter(hostchn_t *hc, slavechn_t *sc, uint8_t al)
{
    if (!(DriverFlags & 1))
        return;

    if (al >= 0x80 && al < 0xF0) {  /* running status filter */
        if (al == LastMIDIByte)
            return;
        LastMIDIByte = al;
    }
    if (Driver && Driver->MIDIOut)
        Driver->MIDIOut(hc, sc, al);
}

void SetFilterCutoff(slavechn_t *sc, uint8_t Val)
{
    hostchn_t *hc = &HChn[sc->HCOffst & 63]; /* Mov DI, [SI+38h] */

    MIDISendFilter(hc, sc, 0xF0);
    MIDISendFilter(hc, sc, 0xF0);
    MIDISendFilter(hc, sc, 0x00);
    MIDISendFilter(hc, sc, Val);
}

void SetFilterResonance(slavechn_t *sc, uint8_t Val)
{
    hostchn_t *hc = &HChn[sc->HCOffst & 63];

    MIDISendFilter(hc, sc, 0xF0);
    MIDISendFilter(hc, sc, 0xF0);
    MIDISendFilter(hc, sc, 0x01);
    MIDISendFilter(hc, sc, Val);
}

/* =====================================================================
 * MIDITranslate - interprets a MIDI macro from MIDIDataArea, or the
 * internal pitch-change command.
 * ===================================================================== */
static const uint8_t MidiPitchSendString[5] = { 0x65, 0x00, 0x64, 0x00, 0x06 };

void MIDITranslate(hostchn_t *hc, slavechn_t *sc, uint16_t MacroOffset)
{
    if (!(DriverFlags & 1))
        return;

    if (MacroOffset >= 0xF000) {
        /* Internal MIDI command: pitch wheel.
         * Depth = (98304/PWD) * log2(Freq/OldFreq), FPU in the original. */
        int32_t depth;
        uint8_t cl;

        if (!(Song.Header.Flags & ITF_MIDI_PITCH))
            return;
        if (Song.Header.PWD == 0)
            return;
        if (!sc || sc->RightVolume == 0 || sc->Frequency == 0)
            return; /* FIDiv by 0: keep deterministic instead of NaN */

        depth = (int32_t)llrint((98304.0 / (double)Song.Header.PWD) *
                                log2((double)sc->Frequency /
                                     (double)sc->RightVolume));

        cl = sc->MCh;
        if (cl == 0)
            return;
        cl--;

        if (depth != 0) {
            uint16_t bit = (uint16_t)(1u << cl);
            if (!(MIDIPitchDepthSent & bit)) {
                MIDIPitchDepthSent |= bit;
                MIDISendFilter(hc, sc, (uint8_t)(0xB0 | cl));
                for (int i = 0; i < 5; i++)
                    MIDISendFilter(hc, sc, MidiPitchSendString[i]);
                MIDISendFilter(hc, sc, Song.Header.PWD);
            }
        }

        {
            int32_t eax = depth + 0x2000;
            if (eax < 0)
                eax = 0;
            if (eax >= 0x4000)
                eax = 0x3FFF;
            if (MIDIPitch[cl] == (uint16_t)eax)
                return;
            MIDIPitch[cl] = (uint16_t)eax;

            MIDISendFilter(hc, sc, (uint8_t)(0xE0 | cl));
            MIDISendFilter(hc, sc, (uint8_t)(MIDIPitch[cl] & 0x7F));
            MIDISendFilter(hc, sc, (uint8_t)((MIDIPitch[cl] >> 7) & 0x7F));
        }
        return;
    }

    /* Parameterised macro string */
    {
        const char *fs = MIDIDataArea;
        uint16_t bx = MacroOffset;
        uint8_t  al = 0;
        uint16_t cx = 0;

        for (;;) {
            uint8_t ah = (uint8_t)fs[bx];
            bx++;

            if (ah == 0) {
                if (cx != 0)
                    MIDISendFilter(hc, sc, al);
                return;
            }

            if (ah == ' ') {
                if (cx == 0)
                    continue;
                goto send;
            }

            /* hex digit? */
            if (ah >= '0' && ah <= '9') {
                al = (uint8_t)((al << 4) | (ah - '0'));
                cx++;
                goto value_end;
            }
            if (ah >= 'A' && ah <= 'F') {
                al = (uint8_t)((al << 4) | (ah - 'A' + 10));
                cx++;
                goto value_end;
            }
            if (ah < 'a' || ah > 'z')
                continue;

            if (ah == 'c') {
                if (!sc)
                    continue;
                al = (uint8_t)((al << 4) | ((sc->MCh - 1) & 0x0F));
                cx++;
                goto value_end;
            }

            /* parameter letters flush any pending nibble pair first */
            if (cx != 0) {
                MIDISendFilter(hc, sc, al);
                cx = 0;
            }

            if (ah == 'z') { al = hc ? hc->CmdVal : 0; goto send; }
            if (ah == 'o') { al = hc ? hc->O00 : 0; goto send; }

            if (!sc)
                continue;

            if (ah == 'n') { al = sc->Nte; goto send; }
            if (ah == 'm') { al = sc->LpD; goto send; }

            if (ah == 'v') {
                /* velocity from volume chain */
                if (sc->Flags & 0x800) {
                    al = 0;
                    goto send;
                }
                {
                    uint32_t t = (uint32_t)sc->VS * GlobalVolume;   /* 0->2^13 */
                    t = (uint32_t)t * sc->CVl;                       /* 0->2^19 */
                    t >>= 4;                                         /* 0->2^15 */
                    t = (t & 0xFFFF) * sc->SVl;                      /* 0->2^22 */
                    t >>= 15;                                        /* 0->2^7  */
                    al = (uint8_t)t;
                    /* Sub AL,1; AdC AL,1 -> max(t,1) on the byte */
                    if (al == 0)
                        al = 1;
                    if (al > 0x7F)
                        al = 0x7F;
                }
                goto send;
            }

            if (ah == 'u') {
                if (sc->Flags & 0x800) {
                    al = 0;
                    goto send;
                }
                al = sc->FV;
                if (al == 0)
                    al = 1;
                if (al > 0x7F)
                    al = 0x7F;
                goto send;
            }

            if (ah == 'h') { al = sc->HCN & 0x7F; goto send; }

            if (ah == 'x' || ah == 'y') {
                al = (ah == 'x') ? sc->Pan : sc->FP;
                {
                    uint16_t v = (uint16_t)al * 2;
                    if (v > 0x7F) {
                        v--;
                        if (v > 0x7F)
                            v = 0x40;
                    }
                    al = (uint8_t)v;
                }
                goto send;
            }

            if (ah == 'p') { al = sc->MPr; goto send; }

            /* 'b'/'a': bank low/high; the Add/AdC/Dec dance maps a byte
             * of 0FFh to 0, anything else passes through. */
            if (ah == 'b') {
                al = (uint8_t)(sc->MBank & 0xFF);
                if (al == 0xFF)
                    al = 0;
                goto send;
            }
            if (ah == 'a') {
                al = (uint8_t)(sc->MBank >> 8);
                if (al == 0xFF)
                    al = 0;
                goto send;
            }

            al = 0;
            continue;

value_end:
            if (cx < 2)
                continue;
send:
            MIDISendFilter(hc, sc, al);
            al = 0;
            cx = 0;
        }
    }
}

/* =====================================================================
 * InitPlayInstrument - IT_MUSIC.ASM line 776
 * ===================================================================== */
void InitPlayInstrument(hostchn_t *hc, slavechn_t *sc, uint16_t InsNum)
{
    instrument_t *in = INSTRUMENT(InsNum);
    uint8_t dl, dh, al;
    int16_t pan;

    sc->InsOffs = InsNum;

    sc->NNA = in->NNA;
    sc->DCT = in->DCT;
    sc->DCA = in->DCA;

    if (hc->MCh != 0) {
        sc->MCh = hc->MCh;
        sc->MPr = hc->MPr;
        sc->MBank = in->MIDIBnk;
        sc->LpD = hc->Nte;     /* [SI+0Bh] = pattern note for MIDI */
    }

    dl = hc->CP;               /* DX = [DI+2Eh]: DL=CP, DH=CV */
    dh = hc->CV;
    al = in->DfP;
    sc->CVl = dh;
    if (!(al & 0x80))
        dl = al;

    /* check for sample pan */
    if (hc->Smp != 0) {
        sample_t *s = SAMPLEHDR(hc->Smp - 1);
        if (s->DfP & 0x80)
            dl = s->DfP & 0x7F;
    }

    if (dl == 100) {
        pan = 100;
    } else {
        /* pitch-pan separation */
        int16_t ax = (int16_t)((int8_t)((uint8_t)(hc->Nte - in->PPC)) *
                               (int8_t)in->PPS);
        ax >>= 3;              /* SAR AX, 3 */
        ax += dl;
        if (ax < 0)
            ax = 0;
        else if (ax > 64)
            ax = 64;
        pan = ax;
    }

    sc->Pan = (uint8_t)pan;
    sc->PS  = (uint8_t)pan;

    /* envelope init */
    sc->VEnv.Pos = 0; sc->VEnv.CurNode = 0; sc->VEnv.NextTick = 0;
    sc->PEnv.Value = 0;
    sc->PEnv.Pos = 0; sc->PEnv.CurNode = 0; sc->PEnv.NextTick = 0;
    sc->PtEnv.Value = 0;
    sc->PtEnv.Pos = 0; sc->PtEnv.CurNode = 0; sc->PtEnv.NextTick = 0;
    sc->VEnv.Value = 64 << 16; /* 400000h */

    {
        uint16_t ax = (uint16_t)((((in->PtEnvelope.Flags & 1) << 2) |
                                  ((in->PEnvelope.Flags  & 1) << 1) |
                                  ( in->VEnvelope.Flags  & 1)) << 12);
        sc->Flags = (uint16_t)(ax | 0x133);
    }

    /* envelope carry */
    if (LastSlaveChannel != NULL) {
        slavechn_t *di = LastSlaveChannel;

        if ((in->VEnvelope.Flags & 9) == 9) {
            sc->VEnv.Value    = di->VEnv.Value;
            sc->VEnv.Delta    = di->VEnv.Delta;
            sc->VEnv.Pos      = di->VEnv.Pos;
            sc->VEnv.CurNode  = di->VEnv.CurNode;
            sc->VEnv.NextTick = di->VEnv.NextTick;
        }
        if ((in->PEnvelope.Flags & 9) == 9) {
            sc->PEnv.Value    = di->PEnv.Value;
            sc->PEnv.Delta    = di->PEnv.Delta;
            sc->PEnv.Pos      = di->PEnv.Pos;
            sc->PEnv.CurNode  = di->PEnv.CurNode;
            sc->PEnv.NextTick = di->PEnv.NextTick;
        }
        if ((in->PtEnvelope.Flags & 9) == 9) {
            sc->PtEnv.Value    = di->PtEnv.Value;
            sc->PtEnv.Delta    = di->PtEnv.Delta;
            sc->PtEnv.Pos      = di->PtEnv.Pos;
            sc->PtEnv.CurNode  = di->PtEnv.CurNode;
            sc->PtEnv.NextTick = di->PtEnv.NextTick;
        }
    }

    hc->Flags |= HF_APPLY_RANDOM_VOL;

    if (hc->MCh == 0) {
        uint16_t bx = (uint16_t)(in->IFC | (in->IFR << 8));

        sc->MBank = 0x00FF;    /* Mov Word Ptr [SI+3Eh], 0FFh */

        if (bx & 0x0080)
            SetFilterCutoff(sc, (uint8_t)(bx & 0x7F));

        if (bx & 0x8000) {
            uint8_t bl = (uint8_t)((bx >> 8) & 0x7F);
            sc->MBank = (uint16_t)((sc->MBank & 0x00FF) | (bl << 8));
            SetFilterResonance(sc, bl);
        }
    }
}

/* =====================================================================
 * ApplyRandomValues - IT_MUSIC.ASM line 992
 * ===================================================================== */
void ApplyRandomValues(hostchn_t *hc)
{
    slavechn_t *sc = SLAVE(hc);
    instrument_t *in = INSTRUMENT(sc->InsOffs);
    int8_t al;
    uint8_t ah;

    hc->Flags &= ~HF_APPLY_RANDOM_VOL;

    al = (int8_t)Random();
    ah = in->RV;               /* random volume, 0->100 */
    if (ah != 0) {
        int16_t ax = (int16_t)(al * (int8_t)ah); /* IMul: -12800..12700 */
        int16_t cx;
        ax >>= 6;              /* SAR: -200..198 */
        ax++;                  /* -199..199 */

        cx = sc->SVl;          /* sample volume set */
        ax = (int16_t)((int32_t)ax * cx / 199);  /* IMul DX; IDiv 199 */
        ax += cx;

        if (ax < 0)
            sc->SVl = 0;
        else if (ax > 128)
            sc->SVl = 128;
        else
            sc->SVl = (uint8_t)ax;
    }

    al = (int8_t)Random();
    ah = in->RP;               /* random pan, 0->64 */
    if (ah != 0) {
        int16_t ax = (int16_t)(al * (int8_t)ah);
        uint8_t dl = sc->Pan;
        int16_t dx;
        ax >>= 7;              /* SAR AX, 7 */
        if (dl == 100)
            return;
        dx = (int16_t)(dl + ax);
        if (dx < 0) {
            sc->Pan = 0; sc->PS = 0;
        } else if (dx > 64) {
            sc->Pan = 64; sc->PS = 64;
        } else {
            sc->Pan = (uint8_t)dx; sc->PS = (uint8_t)dx;
        }
    }
}

/* =====================================================================
 * GetLoopInformation - IT_MUSIC.ASM line 2233
 * ===================================================================== */
void GetLoopInformation(slavechn_t *sc)
{
    sample_t *s = SAMPLEHDR(sc->SmpOffs);
    uint8_t al = s->Flags, ah;
    int32_t ecx, edx;

    if (al & 0x30) {           /* any loop? */
        ecx = (int32_t)s->LoopBeg;
        edx = (int32_t)s->LoopEnd;
        ah = al;

        if (al & 0x20) {       /* susloop? */
            if (!(sc->Flags & SF_NOTE_OFF)) {
                ecx = (int32_t)s->SusLoopBeg;
                edx = (int32_t)s->SusLoopEnd;
                ah >>= 1;
            } else if (!(al & 0x10)) {
                goto NoLoop;
            }
        }
        ah = (ah & 0x40) ? 24 : 8;
    } else {
NoLoop:
        ecx = 0;
        edx = (int32_t)s->Length;
        ah = 0;
    }

    if (sc->LpM == ah && sc->LoopBeg == ecx && sc->LoopEnd == edx)
        return;

    sc->LpM = ah;
    sc->LoopBeg = ecx;
    sc->LoopEnd = edx;
    sc->Flags |= SF_LOOP_CHANGED;
}

/* =====================================================================
 * Pitch slides. With USEFPUCODE (the 2.17 release configuration) the
 * lookup tables are not used; the FPU computes the slides directly.
 * ===================================================================== */
#if USEFPUCODE

static void PitchSlideFPUFreqCheck(hostchn_t *hc, slavechn_t *sc, double r)
{
    int32_t f;

    /* FIStP DWord: round-to-nearest-even; out of range -> 80000000h */
    if (r >= 2147483647.5 || r < -2147483648.0 || r != r)
        f = INT32_MIN;
    else
        f = (int32_t)llrint(r);

    sc->Frequency = f;
    sc->Flags |= SF_FREQ_CHANGE;    /* Or Byte Ptr [SI], 32 */

    if ((uint32_t)f >= 0x7FFFFFFFu) {
        /* PitchSlideUpLinear1: turn off channel */
        sc->Flags |= SF_NOTE_STOP;
        hc->Flags &= (uint16_t)~HF_CHAN_ON;
    }
}

void PitchSlideUpLinear(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    double r = exp2((double)Val * Const1_On_768_value()) *
               (double)sc->Frequency;
    PitchSlideFPUFreqCheck(hc, sc, r);
}

void PitchSlideUpAmiga(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    double freq = (double)sc->Frequency;
    double r = (Const_14317456 * freq) /
               (Const_14317456 - (double)Val * freq);
    PitchSlideFPUFreqCheck(hc, sc, r);
}

void PitchSlideUp(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    if (Song.Header.Flags & ITF_LINEAR_SLIDES)
        PitchSlideUpLinear(hc, sc, Val);
    else
        PitchSlideUpAmiga(hc, sc, Val);
}

void PitchSlideDown(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    PitchSlideUp(hc, sc, (int16_t)-Val); /* Neg BX; fall into PitchSlideUp */
}

#else /* !USEFPUCODE: table-based slides (IT 2.14 behaviour) */

void PitchSlideDownAmiga(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    uint64_t prod;
    uint32_t ebx;
    int cx = 0;

    (void)hc;
    sc->Flags |= SF_FREQ_CHANGE;

    prod = (uint64_t)(uint32_t)sc->Frequency * (uint16_t)Val + 1712u*8363u;
    while (prod >> 32) {
        prod >>= 1;
        cx++;
    }
    ebx = (uint32_t)prod;

    prod = (uint64_t)8363u * 1712u * (uint32_t)sc->Frequency;
    prod >>= cx;

    if (ebx > (uint32_t)(prod >> 32)) {
        sc->Frequency = (int32_t)(prod / ebx);
    }
}

void PitchSlideDownLinear(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    uint16_t mul;

    (void)hc;
    sc->Flags |= SF_FREQ_CHANGE;

    /* Slide values arrive pre-multiplied by 4 from the effect handlers;
     * the original's byte-offset arithmetic selects entry Val>>2 of the
     * coarse table (ShR BX,1; And BX,Not 1 into a word table). */
    if ((uint16_t)Val <= 0x0F)
        mul = FineLinearSlideDownTable[(uint16_t)Val];
    else
        mul = LinearSlideDownTable[(uint16_t)Val >> 2];

    sc->Frequency = (int32_t)(((uint64_t)(uint32_t)sc->Frequency * mul) >> 16);
}

void PitchSlideDown(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    if (Song.Header.Flags & ITF_LINEAR_SLIDES)
        PitchSlideDownLinear(hc, sc, Val);
    else
        PitchSlideDownAmiga(hc, sc, Val);
}

void PitchSlideUpLinear(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    uint32_t mul;
    uint64_t prod;

    sc->Flags |= SF_FREQ_CHANGE;

    /* coarse table: And BX,Not 3 into a dword table -> entry Val>>2 */
    if ((uint16_t)Val <= 0x0F)
        mul = FineLinearSlideUpTable[(uint16_t)Val];
    else
        mul = LinearSlideUpTable[(uint16_t)Val >> 2];

    prod = (uint64_t)(uint32_t)sc->Frequency * mul;
    if ((prod >> 48) != 0) {        /* Test EDX, 0FFFF0000h */
        sc->Flags |= SF_NOTE_STOP;
        hc->Flags &= (uint16_t)~HF_CHAN_ON;
        return;
    }
    sc->Frequency = (int32_t)(prod >> 16);
}

void PitchSlideUpAmiga(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    uint64_t prod = (uint64_t)(uint32_t)sc->Frequency * (uint16_t)Val;

    sc->Flags |= SF_FREQ_CHANGE;

    if (prod >> 32)
        goto TurnOff;
    {
        uint32_t ecx = 1712u * 8363u;
        uint32_t edx = ecx;
        if ((uint32_t)prod >= ecx)
            goto TurnOff;
        ecx -= (uint32_t)prod;
        prod = (uint64_t)(uint32_t)sc->Frequency * edx;
        if (ecx <= (uint32_t)(prod >> 32))
            goto TurnOff;
        sc->Frequency = (int32_t)(prod / ecx);
        return;
    }
TurnOff:
    sc->Flags |= SF_NOTE_STOP;
    hc->Flags &= (uint16_t)~HF_CHAN_ON;
}

void PitchSlideUp(hostchn_t *hc, slavechn_t *sc, int16_t Val)
{
    if (Song.Header.Flags & ITF_LINEAR_SLIDES)
        PitchSlideUpLinear(hc, sc, Val);
    else
        PitchSlideUpAmiga(hc, sc, Val);
}

#endif /* USEFPUCODE */

/* =====================================================================
 * AllocateChannel - IT_MUSIC.ASM line 1492.
 * hflags points at the caller's working copy of the host channel flags
 * low byte (the original keeps it in CH and clears bit 4 on failure).
 * Returns the allocated slave channel or NULL.
 * ===================================================================== */
static slavechn_t *AllocChannelFail(slavechn_t *sc, uint8_t *hflags)
{
    if (sc)
        sc->Flags = 0x200;
    *hflags &= (uint8_t)~4;
    return NULL;
}

slavechn_t *AllocateChannelPtr(hostchn_t *hc, uint8_t *hflags)
{
    slavechn_t *sc;
    instrument_t *insp = NULL;
    uint16_t ins;

    LastSlaveChannel = NULL;

    if (!(Song.Header.Flags & ITF_INSTRUMENTS))
        goto SampleHandler;

    /* --- instrument handler (AllocateChannel1) --- */
    AllocateSlaveOffset = 0;
    AllocateNumChannels = NumChannels;

    if (hc->Smp == 101 && NumChannels != MAXSLAVECHANNELS) {
        AllocateSlaveOffset = NumChannels;
        AllocateNumChannels = (uint16_t)(MAXSLAVECHANNELS - NumChannels);
    }

    ins = hc->Ins;
    if (ins == 0xFF)
        goto SampleHandler;
    if (ins == 0)
        return NULL;            /* AllocateChannel5 */

    insp = INSTRUMENT(ins);

    if (*hflags & 4) {          /* host channel on -> NNA handling */
        uint8_t nna;

        sc = SLAVE(hc);
        if (sc->InsOffs == ins)
            LastSlaveChannel = sc;

        nna = sc->NNA;
        if (nna == 0)
            goto Allocate20;    /* note cut */

        sc->HCN |= 0x80;        /* disown channel */

        /* AllocateHandleNNA */
        if (sc->VS == 0 || sc->CVl == 0 || sc->SVl == 0)
            goto Allocate20;

        if (nna < 2)
            goto Allocate8;     /* note continue */
        if (nna == 2) {
            sc->Flags |= SF_NOTE_OFF;
            GetLoopInformation(sc);
            goto Allocate8;
        }
        sc->Flags |= SF_FADEOUT; /* nna = 3 -> fade */
        goto Allocate8;

Allocate20:
        if (sc->Smp == 100) {   /* MIDI? */
            sc->Flags |= SF_NOTE_STOP;
            sc->HCN |= 0x80;
            if (hc->Smp == 101)
                goto MIDIDC;
            goto Allocate4;
        }
        /* Allocate20Samples */
        if (DriverFlags & 2) {  /* hiqual: keep for ramp-down */
            sc->Flags |= SF_NOTE_STOP;
            sc->HCN |= 0x80;
            goto Allocate4;
        }
        {
            uint8_t dct = insp->DCT;
            sc->Flags = 0x200;
            if (dct == 0)
                goto FoundInstrument;
            goto Allocate11;
        }
    }

Allocate8:
    if (hc->Smp == 101)
        goto MIDIDC;
    if (insp->DCT == 0)
        goto Allocate4;

Allocate11:
    /* duplicate check */
    {
        uint8_t dl, dh, ah, chDCA;
        uint16_t bp;
        uint16_t cx;
        slavechn_t *s2;

        dl = hc->Nte;           /* Mov DX, [DI+3] */
        dh = hc->Ins;
        bp = offsetof(slavechn_t, Nte);
        if (insp->DCT != 1) {
            bp = offsetof(slavechn_t, Ins);
            dl = dh;
            if (insp->DCT != 3) {
                bp = offsetof(slavechn_t, Smp);
                if (hc->Smp == 0)
                    goto Allocate4;
                dl = (uint8_t)(hc->Smp - 1);
            }
        }
        ah = hc->HCN | 0x80;
        chDCA = insp->DCA;
        goto Allocate6;

MIDIDC:
        dl = hc->Nt2;
        dh = hc->Ins;
        bp = offsetof(slavechn_t, Nte);
        ah = hc->MCh;
        chDCA = insp->DCA;

Allocate6:
        s2 = &SChn[AllocateSlaveOffset];
        for (cx = AllocateNumChannels & 0xFF ? (AllocateNumChannels & 0xFF)
                                             : 256; ; ) {
            /* note: the original decrements CL (8 bits); with 256
             * channels CL=0 -> 256 iterations. Reproduced above. */
            if (s2->Flags & SF_CHAN_ON) {
                if (hc->Smp == 101)
                    goto MIDIDCT;
                if (s2->HCN != ah)
                    goto Allocate7;
MIDIDCT:
                if (s2->Ins != dh)
                    goto Allocate7;
                if (*(const uint8_t *)((const char *)s2 + bp) != dl)
                    goto Allocate7;

                if (hc->Smp == 101) {
                    /* MIDI handling */
                    if (s2->Smp != 100)
                        goto Allocate7;
                    if (ah != s2->MCh)
                        goto Allocate7;
                    s2->Flags |= SF_NOTE_STOP;
                    if (!(s2->HCN & 0x80)) {
                        hostchn_t *bphost = &HChn[s2->HCOffst & 63];
                        s2->HCN |= 0x80;
                        bphost->Flags &= (uint16_t)~HF_CHAN_ON;
                    }
                    goto Allocate7;
                }

                if (chDCA != s2->DCA)
                    goto Allocate7;

                if (chDCA == 0) {
                    /* duplicate check action: cut */
                    if (DriverFlags & 2) {
                        s2->Flags |= SF_NOTE_STOP;
                        s2->HCN |= 0x80;
                        goto Allocate4;
                    }
                    {
                        uint8_t dct2 = insp->DCT;
                        s2->Flags = 0x200;
                        sc = s2;
                        if (dct2 == 0)
                            goto FoundInstrument;
                        goto Allocate11;
                    }
                }

                s2->DCT = 0;
                s2->DCA = 0;
                nnaFromDCA:
                {
                    uint8_t nna = (uint8_t)(chDCA + 1);
                    sc = s2;
                    if (sc->VS == 0 || sc->CVl == 0 || sc->SVl == 0)
                        goto Allocate20;
                    if (nna < 2)
                        goto Allocate8;
                    if (nna == 2) {
                        sc->Flags |= SF_NOTE_OFF;
                        GetLoopInformation(sc);
                        goto Allocate8;
                    }
                    sc->Flags |= SF_FADEOUT;
                    goto Allocate8;
                }
                goto nnaFromDCA; /* unreachable; silences fallthrough */
            }
Allocate7:
            s2++;
            cx--;
            if (cx == 0)
                break;
        }
    }

Allocate4:
    /* search for a free channel */
    {
        uint16_t cx = AllocateNumChannels;
        sc = &SChn[AllocateSlaveOffset];

        if (hc->Smp == 101) {
            /* MIDI slave channels maintained while still referenced */
            for (; cx != 0; cx--, sc++) {
                if (sc->Flags & SF_CHAN_ON)
                    continue;
                if (sc->HCOffst == 0xFFFF)
                    goto FoundInstrument;
                if (HChn[sc->HCOffst & 63].SCOffst !=
                    (uint16_t)(sc - SChn))
                    goto FoundInstrument;
            }
            goto Allocate17;
        }

        for (; cx != 0; cx--, sc++) {
            if (!(sc->Flags & SF_CHAN_ON))
                goto FoundInstrument;
        }
    }

Allocate17:
    /* common sample search: find sample used by > 2 disowned channels,
     * steal the quietest one */
    {
        uint8_t  count[100];
        slavechn_t *loc[100];
        uint8_t  vols[100];
        uint16_t cx;
        slavechn_t *di;
        int i;
        uint8_t ah;
        slavechn_t *found;

        memset(count, 0, sizeof(count));
        memset(loc, 0, sizeof(loc));
        memset(vols, 0xFF, sizeof(vols));

        di = &SChn[AllocateSlaveOffset];
        for (cx = AllocateNumChannels; cx != 0; cx--, di++) {
            uint8_t bl = di->Smp;
            if (bl > 99)
                continue;
            count[bl]++;
            if (!(di->HCN & 0x80))
                continue;
            if (vols[bl] <= di->FV)
                continue;
            loc[bl] = di;
            vols[bl] = di->FV;
        }

        ah = 2;
        found = NULL;
        for (i = 0; i < 100; i++) {
            if (count[i] > ah) {
                ah = count[i];
                found = loc[i];
            }
        }
        if (found) {
            sc = found;
            goto FoundInstrument;
        }

        /* Find the host channel with the most (disowned) slave channels,
         * then the softest non-single sample in that channel. */
        {
            uint8_t chnCount[64];

            memset(chnCount, 0, sizeof(chnCount));
            di = &SChn[AllocateSlaveOffset];
            for (cx = AllocateNumChannels; cx != 0; cx--, di++)
                chnCount[di->HCN & 0x3F]++;

CountEnd:
            {
                uint8_t maxcount = 1, chn = 0;
                for (i = 0; i < 64; i++) {
                    if (chnCount[i] > maxcount) {
                        maxcount = chnCount[i];
                        chn = (uint8_t)i;
                    }
                }

                if (maxcount > 1) {
                    uint8_t al = chn | 0x80;
                    uint8_t bh = (uint8_t)(hc->Smp - 1);
                    uint8_t bestvol = 0xFF;
                    slavechn_t *si = NULL;

                    di = &SChn[AllocateSlaveOffset];
                    for (cx = AllocateNumChannels; cx != 0; cx--, di++) {
                        if (di->HCN != al)
                            continue;
                        if (bestvol <= di->FV)
                            continue;
                        if (di->Smp != bh) {
                            /* check if any other channel has di's sample */
                            uint8_t bl = di->Smp;
                            slavechn_t *s3;
                            uint16_t c3;
                            int second = 0;

                            di->Smp = 0xFF;
                            s3 = &SChn[AllocateSlaveOffset];
                            for (c3 = AllocateNumChannels; c3 != 0;
                                 c3--, s3++) {
                                if (s3->Smp == bh || s3->Smp == bl) {
                                    second = 1;
                                    break;
                                }
                            }
                            di->Smp = bl;
                            if (!second)
                                continue;
                        }
                        si = di;
                        bestvol = di->FV;
                    }

                    if (si) {
                        /* sample search: quietest disowned with the same
                         * sample as si */
                        uint8_t al2 = si->Smp;
                        uint8_t best2 = 0xFF;

                        di = &SChn[AllocateSlaveOffset];
                        for (cx = AllocateNumChannels; cx != 0;
                             cx--, di++) {
                            if (di->Smp != al2)
                                continue;
                            if (!(di->HCN & 0x80))
                                continue;
                            if (best2 <= di->FV)
                                continue;
                            si = di;
                            best2 = di->FV;
                        }
                        sc = si;
                        goto FoundInstrument;
                    }

                    chnCount[chn] = 0;
                    goto CountEnd;
                }
            }

            /* softest search: quietest disowned channel overall */
            {
                uint8_t best = 0xFF;
                slavechn_t *si = NULL;

                di = &SChn[AllocateSlaveOffset];
                for (cx = AllocateNumChannels; cx != 0; cx--, di++) {
                    if (!(di->HCN & 0x80))
                        continue;
                    if (best < di->FV)
                        continue;
                    si = di;
                    best = di->FV;
                }
                if (si) {
                    sc = si;
                    goto FoundInstrument;
                }
                return AllocChannelFail(NULL, hflags);
            }
        }
    }

FoundInstrument:
    hc->SCOffst = (uint16_t)(sc - SChn);
    sc->HCN = hc->HCN;
    sc->HCOffst = (uint16_t)(hc - HChn);

    sc->Bit = 0; sc->ViP = 0; sc->ViDepth = 0;  /* [SI+18h] = 0 */
    sc->LpD = 0;                                /* reset loop dirn */

    InitPlayInstrument(hc, sc, ins);

    sc->SVl = insp->GbV;
    sc->FadeOut = 0x400;

    {
        uint8_t note = (hc->Smp == 101) ? hc->Nt2 : hc->Nte;
        sc->Nte = note;
        sc->Ins = hc->Ins;
    }

    if (hc->Smp == 0)
        return AllocChannelFail(sc, hflags);
    {
        uint8_t smp = (uint8_t)(hc->Smp - 1);
        sample_t *s;

        sc->Smp = smp;
        sc->SmpOffs = smp;

        s = SAMPLEHDR(smp);
        if (s->Length == 0)
            return AllocChannelFail(sc, hflags);
        if (!(s->Flags & 1))
            return AllocChannelFail(sc, hflags);

        sc->Bit = s->Flags & 2;
        sc->SVl = (uint8_t)(((uint16_t)s->GvL * sc->SVl) >> 6);
        return sc;
    }

SampleHandler:
    /* --- AllocateChannel15: sample mode, slave = host channel number --- */
    {
        uint8_t hcn = hc->HCN;
        sc = &SChn[hcn];

        if ((DriverFlags & 2) && (sc->Flags & SF_CHAN_ON)) {
            /* copy out channel for ramp-down (slave + 64) */
            sc->Flags |= SF_NOTE_STOP;
            sc->HCN |= 0x80;
            memcpy(&SChn[hcn + 64], sc, sizeof(slavechn_t));
        }

        hc->SCOffst = (uint16_t)(sc - SChn);
        sc->HCOffst = (uint16_t)(hc - HChn);
        sc->HCN = hcn;
        sc->Flags = 0x133;

        sc->CVl = hc->CV;
        sc->Pan = hc->CP;
        sc->PS  = hc->CP;

        /* AllocateChannel3 */
        sc->FadeOut = 0x400;
        /* Mov Word Ptr [SI+52h], 64: high word of VEnv.Value */
        sc->VEnv.Value = (int32_t)((sc->VEnv.Value & 0xFFFF) | (64u << 16));
        sc->MBank = 0x00FF;

        sc->Nte = hc->Nte;
        sc->Ins = hc->Ins;

        if (hc->Smp == 0)
            return AllocChannelFail(sc, hflags);
        {
            uint8_t smp = (uint8_t)(hc->Smp - 1);
            sample_t *s;

            sc->Smp = smp;
            sc->SmpOffs = smp;

            sc->Bit = 0; sc->ViP = 0; sc->ViDepth = 0;
            sc->PEnv.Value &= 0xFFFF;   /* no pan deviation  */
            sc->PtEnv.Value &= 0xFFFF;  /* no pitch deviation */
            sc->LpD = 0;

            s = SAMPLEHDR(smp);
            if (s->Length == 0)
                return AllocChannelFail(sc, hflags);
            if (!(s->Flags & 1))
                return AllocChannelFail(sc, hflags);

            sc->Bit = s->Flags & 2;
            sc->SVl = (uint8_t)(s->GvL * 2);
            return sc;
        }
    }
}

/* =====================================================================
 * GetChannels / tempo helpers
 * ===================================================================== */
void GetChannels(void)
{
    uint16_t ax = Driver ? Driver->MaxChannels : 32;
    if (ax >= MAXSLAVECHANNELS)
        ax = MAXSLAVECHANNELS;
    NumChannels = ax;
}

void Music_InitTempo(void)
{
    if (Driver && Driver->SetTempo)
        Driver->SetTempo(Tempo);
}

void Music_InitMixTable(void)
{
    if (Driver && Driver->SetMixVolume)
        Driver->SetMixVolume(Song.Header.MV);
}

void Music_InitStereo(void)
{
    if (Driver && Driver->SetStereo)
        Driver->SetStereo(Song.Header.Flags & 1);
    RecalculateAllVolumes();
}

void Music_SetGlobalVolume(uint8_t Vol)
{
    GlobalVolume = Vol;
    RecalculateAllVolumes();
}

uint16_t Music_IncreaseSpeed(void)
{
    if (CurrentSpeed != 1) {
        CurrentSpeed--;
        Song.Header.IS = (uint8_t)CurrentSpeed;
    }
    return CurrentSpeed;
}

uint16_t Music_DecreaseSpeed(void)
{
    if (CurrentSpeed < 0xFF) {
        CurrentSpeed++;
        Song.Header.IS = (uint8_t)CurrentSpeed;
    }
    return CurrentSpeed;
}

/* =====================================================================
 * UpdateGOTONote / UpdateNoteData / PreInitCommand - pattern decoding
 * ===================================================================== */
static void GetPatternInfo(uint16_t num, const uint8_t **data,
                           uint16_t *rows)
{
    pattern_t *p = (num < MAX_PATTERNS) ? &Song.Patterns[num] : NULL;

    if (p && p->PackedData) {
        *data = p->PackedData;
        *rows = p->Rows;
    } else {
        *data = EmptyPatternData;
        *rows = 64;
    }
}

static void UpdateGOTONote(void)
{
    const uint8_t *si;
    uint16_t rows, cx;

    DecodeExpectedPattern = CurrentPattern;

    GetPatternInfo(CurrentPattern, &si, &rows);

    cx = ProcessRow;
    if (rows <= cx)
        cx = 0;

    CurrentRow = cx;
    ProcessRow = cx;
    DecodeExpectedRow = cx;
    NumberOfRows = rows;

    /* skip cx rows, updating the channel "last value" caches */
    cx++;
    while (--cx != 0) {
        for (;;) {
            uint8_t al = *si++;
            hostchn_t *hc;
            uint8_t dh;

            if (al == 0)
                break;

            hc = &HChn[(al & 0x7F) - 1];
            dh = hc->Msk;
            if (al & 0x80) {
                dh = *si++;
                hc->Msk = dh;
            }
            if (dh & 1)
                hc->Nte = *si++;
            if (dh & 2)
                hc->Ins = *si++;
            if (dh & 4)
                hc->Vol = *si++;
            if (dh & 8) {
                hc->OCm = si[0];
                hc->OCmVal = si[1];
                si += 2;
            }
        }
    }

    PatternDataPos = si;
}

static void PreInitCommand(hostchn_t *hc)
{
    if (hc->Msk & 0x33) {
        if (!(Song.Header.Flags & ITF_INSTRUMENTS))
            goto NoXlat;
        if (hc->Nte >= 120)
            goto NoXlat;
        if (hc->Ins == 0)
            goto NoXlat;

        {
            instrument_t *in = INSTRUMENT(hc->Ins);
            uint8_t mch = in->MCh;
            uint8_t nt2, smp;

            if (mch == 0) {
                nt2 = in->NoteSampleTable[hc->Nte * 2];
                smp = in->NoteSampleTable[hc->Nte * 2 + 1];
            } else {
                if (mch == 17)
                    mch = (uint8_t)((hc->HCN & 0x0F) + 1);
                hc->MCh = mch;
                hc->MPr = in->MPr;
                smp = 101;
                nt2 = in->NoteSampleTable[hc->Nte * 2];
            }
            hc->Nt2 = nt2;
            hc->Smp = smp;
            if (smp == 0)
                goto End4;      /* no sample -> don't init the command */
        }
        goto InitCmd;
NoXlat:
        hc->Nt2 = hc->Nte;
        hc->Smp = hc->Ins;
    }

InitCmd:
    InitCommandTable[hc->Cmd & 31](hc);

    hc->Flags |= HF_ROW_UPDATED;

    if (Song.Header.ChnlPan[hc->HCN] & 0x80) {     /* channel muted? */
        if (!(hc->Flags & HF_FREEPLAY_NOTE) && (hc->Flags & HF_CHAN_ON))
            SLAVE(hc)->Flags |= SF_CHN_MUTED;       /* Or [SI+1], 8 */
    }

End4:;
}

static void UpdateNoteData(void)
{
    const uint8_t *si;
    int i;

    PatternLooping = 0;

    if (CurrentPattern != DecodeExpectedPattern) {
        UpdateGOTONote();
    } else {
        DecodeExpectedRow++;
        if (CurrentRow != DecodeExpectedRow)
            UpdateGOTONote();
    }

    /* clear all old command flags */
    for (i = 0; i < 64; i++)
        HChn[i].Flags &= (uint16_t)~(3 + 32 + 64 + 256);

    si = PatternDataPos;
    for (;;) {
        uint8_t al = *si++;
        hostchn_t *hc;
        uint8_t dh;
        uint16_t ax;

        if (al == 0)
            break;

        hc = &HChn[(al & 0x7F) - 1];
        dh = hc->Msk;
        if (al & 0x80) {
            dh = *si++;
            hc->Msk = dh;
        }
        if (dh & 1)
            hc->Nte = *si++;
        if (dh & 2)
            hc->Ins = *si++;
        if (dh & 4)
            hc->Vol = *si++;
        if (dh & 8) {
            hc->OCm = si[0];
            hc->OCmVal = si[1];
            si += 2;
            ax = (uint16_t)(hc->OCm | (hc->OCmVal << 8));
        } else if (dh & 0x80) {
            ax = (uint16_t)(hc->OCm | (hc->OCmVal << 8));
        } else {
            ax = 0;
        }
        hc->Cmd = (uint8_t)ax;
        hc->CmdVal = (uint8_t)(ax >> 8);

        PreInitCommand(hc);
    }
    PatternDataPos = si;
}

/* =====================================================================
 * UpdateVibrato (auto/sample vibrato) - IT_MUSIC.ASM line 4275
 * ===================================================================== */
static void UpdateVibrato(slavechn_t *sc)
{
    sample_t *s = SAMPLEHDR(sc->SmpOffs);
    uint8_t depth = s->ViD;
    uint8_t speed, wave, ch;
    int8_t  al;
    int16_t ax;

    if (depth == 0)
        return;

    {
        uint16_t lo = (uint16_t)((sc->ViDepth & 0xFF) + s->ViR);
        ch = (uint8_t)((sc->ViDepth >> 8) + (lo > 0xFF ? 1 : 0));
        if (ch > depth)
            ch = depth;
        sc->ViDepth = (uint16_t)((lo & 0xFF) | (ch << 8));
    }

    speed = s->ViS;
    wave  = s->ViT;
    if (speed == 0)
        return;

    if (wave == 3) {
        al = (int8_t)((Random() & 127) - 64);
    } else {
        uint8_t bl = (uint8_t)(sc->ViP + speed);
        sc->ViP = bl;
        al = FineWaveData[((wave & 3) << 8) + bl];
    }

    ax = (int16_t)(al * (int8_t)ch);
    ax = (int16_t)(ax << 2);            /* SAL AX, 2 */
    if (ax == 0)
        return;

    PitchSlideUpLinear(&HChn[sc->HCOffst & 63], sc, (int16_t)(int8_t)(ax >> 8));
}

/* =====================================================================
 * UpdateEnvelope - IT_MUSIC.ASM line 4514. Returns nonzero (carry) if
 * the envelope is finished.
 * ===================================================================== */
static int UpdateEnvelope(const env_t *env, envstate_t *st, int BP)
{
    uint16_t dx = st->Pos;
    uint8_t  node, nextnode;

    if (dx < st->NextTick) {
        st->Pos = (uint16_t)(dx + 1);
        st->Value += st->Delta;
        return 0;
    }

    /* UpdateEnvelope1: get current node's value, figure out next node */
    node = (uint8_t)st->CurNode;
    nextnode = (uint8_t)(node + 1);

    st->Value = (int32_t)((uint32_t)env->NodePoints[node].Magnitude << 16);
    /* note: zero-extended byte shifted left, as in the original
     * (Xor DH,DH; Mov DL,mag; ShL EDX,16) */
    st->Value = (int32_t)((uint32_t)(uint8_t)env->NodePoints[node].Magnitude
                          << 16);

    if (env->Flags & 6) {       /* any loop at all? */
        uint8_t lpb = env->LpB, lpe = env->LpE;
        int have_loop = 1;

        if (env->Flags & 4) {   /* susloop */
            if (BP == 0) {
                lpb = env->SLB;
                lpe = env->SLE;
            } else if (!(env->Flags & 2)) {
                have_loop = 0;
            }
        }

        if (have_loop && nextnode > lpe) {
            uint16_t tick = env->NodePoints[lpb].Tick;
            /* byte write in the original ([SI+0Ah]): the high byte holds
             * the WAV/hiqual driver's channel filter cutoff - preserve it */
            st->CurNode = (uint16_t)((st->CurNode & 0xFF00) | lpb);
            st->Pos = tick;
            st->NextTick = tick;
            return 0;
        }
    }

    /* UpdateEnvelope2 */
    if (nextnode >= env->Num)
        return 1;               /* carry: envelope done */

    st->CurNode = (uint16_t)((st->CurNode & 0xFF00) | nextnode);
    {
        uint16_t newtick = env->NodePoints[nextnode].Tick;
        uint16_t lasttick = env->NodePoints[nextnode - 1].Tick;
        uint16_t bx = (uint16_t)(newtick - lasttick);
        int16_t  mag;
        int      neg;
        uint32_t eax;

        st->NextTick = newtick;
        st->Pos = (uint16_t)(lasttick + 1);

        /* 8-bit subtract then CBW, as in the original */
        mag = (int16_t)(int8_t)((uint8_t)env->NodePoints[nextnode].Magnitude -
                                (uint8_t)env->NodePoints[nextnode-1].Magnitude);
        neg = (mag < 0);
        if (neg)
            mag = (int16_t)-mag;

        if (bx == 0)
            bx = 1;             /* Sub BX,1; AdC BX,1 */

        eax = (((uint32_t)(uint16_t)mag << 16) / bx);
        st->Delta = neg ? -(int32_t)eax : (int32_t)eax;
    }
    return 0;
}

/* =====================================================================
 * UpdateMIDI - IT_MUSIC.ASM line 4650
 * ===================================================================== */
static void UpdateMIDI(void)
{
    slavechn_t *sc;
    uint16_t cx;

    /* stop cycle */
    sc = SChn;
    for (cx = MAXSLAVECHANNELS; cx != 0; cx--, sc++) {
        if (!(sc->Flags & SF_NOTE_STOP))
            continue;
        if (sc->Smp != 100)
            continue;
        if (!(sc->Flags & SF_CHAN_ON))
            continue;

        sc->Flags = 0;
        MIDITranslate(&HChn[sc->HCOffst & 63], sc, MIDICOMMAND_STOPNOTE);

        if (!(sc->HCN & 0x80)) {
            sc->HCN |= 0x80;
            HChn[sc->HCOffst & 63].Flags &= (uint16_t)~HF_CHAN_ON;
        }
    }

    /* play cycle */
    sc = SChn;
    for (cx = MAXSLAVECHANNELS; cx != 0; cx--, sc++) {
        uint16_t dx;
        hostchn_t *hc;

        if (sc->Smp != 100)
            continue;
        dx = sc->Flags;
        if (!(dx & SF_CHAN_ON))
            continue;

        {
            uint8_t al = (uint8_t)sc->SampleOffset;
            sc->SampleOffset = (sc->SampleOffset & ~0xFF) | 1;
            sc->OldSampleOffset = (sc->OldSampleOffset & ~0xFFu) | al;
        }

        if (dx & SF_CHN_MUTED)
            continue;

        hc = &HChn[sc->HCOffst & 63];
        sc->Flags &= 0x788D;

        if (dx & SF_NEW_NOTE) {
            /* bank select? */
            uint16_t ax = sc->MBank;
            uint8_t bl;

            if (ax != 0xFFFF) {
                bl = (uint8_t)(sc->MCh - 1);
                if (MIDIBanks[bl & 15] != ax) {
                    MIDIBanks[bl & 15] = ax;
                    MIDIPrograms[bl & 15] = 0xFF;
                    MIDITranslate(hc, sc, MIDICOMMAND_BANKSELECT);
                }
            }

            /* program? */
            if (!(sc->MPr & 0x80)) {
                bl = (uint8_t)(sc->MCh - 1);
                if (MIDIPrograms[bl & 15] != sc->MPr) {
                    MIDIPrograms[bl & 15] = sc->MPr;
                    MIDITranslate(hc, sc, MIDICOMMAND_PROGRAMSELECT);
                }
            }

            /* pitch wheel reset */
            if (Song.Header.Flags & ITF_MIDI_PITCH) {
                bl = (uint8_t)(sc->MCh - 1);
                if (MIDIPitch[bl & 15] != 0x2000) {
                    MIDIPitch[bl & 15] = 0x2000;
                    MIDISendFilter(hc, sc, (uint8_t)(0xE0 | bl));
                    MIDISendFilter(hc, sc, 0);
                    MIDISendFilter(hc, sc, 0x40);
                }
            }

            sc->RightVolume = sc->FrequencySet; /* MIDIFSet */
            MIDITranslate(hc, sc, MIDICOMMAND_PLAYNOTE);
        } else if (dx & SF_RECALC_FINALVOL) {
            MIDITranslate(hc, sc, MIDICOMMAND_CHANGEVOLUME);
        }

        if (dx & SF_PAN_CHANGED) {
            uint8_t bl = (uint8_t)(sc->MCh - 1);
            if (MIDIPan[bl & 15] != sc->FPP) {
                MIDIPan[bl & 15] = sc->FPP;
                MIDITranslate(hc, sc, MIDICOMMAND_CHANGEPAN);
            }
        }

        if (dx & SF_FREQ_CHANGE)
            MIDITranslate(hc, sc, MIDICOMMAND_CHANGEPITCH);
    }
}

/* =====================================================================
 * UpdateInstruments - IT_MUSIC.ASM line 4814
 * ===================================================================== */
static void UpdateInstruments(void)
{
    slavechn_t *sc = SChn;
    uint16_t count;

    DoMIDICycle = 0;

    for (count = MAXSLAVECHANNELS; count != 0; count--, sc++) {
        uint16_t cx;
        instrument_t *in;
        int BP;

        if (!(sc->Flags & SF_CHAN_ON))
            continue;

        cx = sc->Flags;

        if (sc->Ins == 0xFF)
            goto Instruments5;

        in = INSTRUMENT(sc->InsOffs);
        BP = cx & SF_NOTE_OFF;  /* sustain released */

        /* pitch envelope */
        if (cx & SF_PITCHENV_ON) {
            if (UpdateEnvelope(&in->PtEnvelope, &sc->PtEnv, BP))
                cx &= (uint16_t)~SF_PITCHENV_ON;
        }

        if (in->PtEnvelope.Flags & 0x80) {
            /* filter envelope */
            if (sc->Smp != 100) {
                int16_t bx = (int16_t)((uint32_t)sc->PtEnv.Value >> 8);
                bx >>= 6;       /* SAR BX, 6: -128..+128 */
                bx += 128;      /* 0..256 */
                /* Cmp BH,1; AdC BL,-1 */
                {
                    uint8_t bl = (uint8_t)bx;
                    if ((bx >> 8) >= 1)
                        bl = (uint8_t)(bl - 1);
                    sc->MBank = (uint16_t)((sc->MBank & 0xFF00) | bl);
                }
                cx |= SF_RECALC_FINALVOL;
            }
        } else {
            int16_t bx = (int16_t)((uint32_t)sc->PtEnv.Value >> 8);
            bx >>= 3;           /* SAR BX, 3 */
            if (bx != 0) {
                PitchSlideUpLinear(&HChn[sc->HCOffst & 63], sc, bx);
                cx |= SF_FREQ_CHANGE;
            }
        }

        /* pan envelope */
        if (cx & SF_PANENV_ON) {
            cx |= SF_RECALC_PAN;
            if (UpdateEnvelope(&in->PEnvelope, &sc->PEnv, BP))
                cx &= (uint16_t)~SF_PANENV_ON;
        }

        /* volume envelope */
        if (cx & SF_VOLENV_ON) {
            cx |= SF_RECALC_VOL;

            if (UpdateEnvelope(&in->VEnvelope, &sc->VEnv, BP)) {
                /* envelope turned off */
                cx &= (uint16_t)~SF_VOLENV_ON;
                if (((sc->VEnv.Value >> 16) & 0xFF) == 0)
                    goto TurnOff17;
                goto Fade19;
            }
            if (cx & SF_FADEOUT)
                goto Fade13;
            if (BP && (in->VEnvelope.Flags & 2))
                goto Fade19;
            goto Instruments5;
        }

        /* Instruments3: no volume envelope */
        if (cx & SF_FADEOUT)
            goto Fade13;
        if (!(cx & SF_NOTE_OFF))
            goto Instruments5;

Fade19:
        cx |= SF_FADEOUT;

Fade13:
        {
            uint16_t fade = in->FadeOut;
            int16_t nf = (int16_t)((int16_t)sc->FadeOut - (int16_t)fade);
            sc->FadeOut = (uint16_t)nf;
            if (nf > 0)
                goto Instruments4;
            sc->FadeOut = 0;
        }

TurnOff17:
        if (!(sc->HCN & 0x80)) {
            sc->HCN |= 0x80;
            HChn[sc->HCOffst & 63].Flags &= (uint16_t)~HF_CHAN_ON;
        }
        cx |= SF_NOTE_STOP;

Instruments4:
        cx |= SF_RECALC_VOL;

Instruments5:
        if (cx & SF_RECALC_VOL) {
            uint32_t t;

            cx &= (uint16_t)~SF_RECALC_VOL;
            cx |= SF_RECALC_FINALVOL;

            if (SoloSample != 0xFF && sc->Smp != SoloSample)
                cx |= SF_CHN_MUTED;
            else if (SoloInstrument != 0xFF && sc->Ins != SoloInstrument)
                cx |= SF_CHN_MUTED;

            t = (uint32_t)(sc->Vol * sc->CVl);          /* 0..4096        */
            t = (t * sc->FadeOut) >> 7;                 /* 0..32768       */
            t = (t * sc->SVl) >> 7;                     /* 0..32768       */
            t = (t * (uint16_t)((uint32_t)sc->VEnv.Value >> 8)) >> 14;
            t = (t * GlobalVolume) >> 7;                /* 0..32768       */

            sc->FV = (uint8_t)(t >> 8);
            sc->Vol16b = (uint16_t)t;
        }

        if (cx & SF_RECALC_PAN) {
            uint8_t dl, al;

            cx &= (uint16_t)~SF_RECALC_PAN;
            cx |= SF_PAN_CHANGED;

            dl = sc->Pan;
            al = dl;
            if (dl != 100) {
                int8_t a;
                int16_t ax;

                a = (int8_t)(32 - dl);
                if (a < 0)
                    a = (int8_t)-a;     /* |32 - pan| */
                a = (int8_t)(32 - a);

                ax = (int16_t)(a * (int8_t)((sc->PEnv.Value >> 16) & 0xFF));
                ax >>= 5;               /* SAR AX, 5 */
                al = (uint8_t)(ax + dl);

                sc->FP = al;

                {
                    int16_t r = (int16_t)((int8_t)(al - 32) *
                                          (int8_t)(Song.Header.Sep >> 1));
                    int8_t v = (int8_t)(r >> 6);
                    if (ReverseChannels)
                        v = (int8_t)-v;
                    al = (uint8_t)(v + 32);
                }
            } else {
                sc->FP = al;
            }
            sc->FPP = al;
        }

        sc->Flags = cx;

        UpdateVibrato(sc);

        if (sc->Smp == 100)
            DoMIDICycle = 1;
    }

    if (DoMIDICycle)
        UpdateMIDI();
}

/* =====================================================================
 * UpdateSamples - IT_MUSIC.ASM line 4417
 * ===================================================================== */
static void UpdateSamples(void)
{
    slavechn_t *sc = SChn;
    uint16_t count;

    for (count = NumChannels; count != 0; count--, sc++) {
        uint16_t bx;

        if (!(sc->Flags & SF_CHAN_ON))
            continue;

        bx = sc->Flags;

        if (bx & SF_RECALC_VOL) {
            uint32_t t;

            bx &= (uint16_t)~SF_RECALC_VOL;
            bx |= SF_RECALC_FINALVOL;

            if (SoloSample != 0xFF && sc->Smp != SoloSample)
                bx |= SF_CHN_MUTED;

            t = (uint32_t)(sc->Vol * sc->CVl);          /* 0..4096        */
            t = (t * sc->SVl) >> 4;                     /* 0..32768       */
            t = (t * GlobalVolume) >> 7;                /* 0..32768       */

            sc->FV = (uint8_t)(t >> 8);
            sc->Vol16b = (uint16_t)t;
        }

        if (bx & SF_RECALC_PAN) {
            uint8_t al;

            bx &= (uint16_t)~SF_RECALC_PAN;
            bx |= SF_PAN_CHANGED;

            al = sc->Pan;
            sc->FP = al;
            if (al != 100) {
                int16_t r = (int16_t)((int8_t)(al - 32) *
                                      (int8_t)(Song.Header.Sep >> 1));
                int8_t v = (int8_t)(r >> 6);
                if (ReverseChannels)
                    v = (int8_t)-v;
                al = (uint8_t)(v + 32);
            }
            sc->FPP = al;
        }

        sc->Flags = bx;

        UpdateVibrato(sc);
    }
}

/* =====================================================================
 * UpdateData - IT_MUSIC.ASM line 5143 (the sequencer core)
 * ===================================================================== */
static void UpdateEffectData(void)
{
    hostchn_t *hc = HChn;
    int i;

    for (i = 0; i < 64; i++, hc++) {
        uint8_t al;

        if (!(hc->Flags & HF_ROW_UPDATED))
            continue;
        al = hc->Msk;
        if (!(al & 0x88))
            continue;

        hc->Msk = al & 0x88;
        InitCommandTable[hc->Cmd & 31](hc);
        hc->Msk = al;
    }
}

static void UpdateNoNewRow(void)
{
    hostchn_t *hc = HChn;
    int i;

    for (i = 0; i < 64; i++, hc++) {
        uint16_t ax = hc->Flags;

        if ((ax & HF_CHAN_ON) && (ax & HF_UPDATE_VOLEFCT_IF_CHAN_ON)) {
            VolumeEffectTable[hc->VCm & 7](hc);
            ax = hc->Flags;
        }

        if (!(ax & 3))
            continue;
        if (!(ax & 2) && !(ax & HF_CHAN_ON))
            continue;

        CommandTable[hc->Cmd & 31](hc);
    }
}

static void UpdateData(void)
{
    if (PlayMode == 1) {
        /* ---- pattern playback ---- */
        ProcessTick--;
        CurrentTick--;
        if (CurrentTick != 0) {
            UpdateNoNewRow();
            return;
        }

        CurrentTick = CurrentSpeed;
        ProcessTick = CurrentSpeed;

        RowDelay--;
        if (RowDelay != 0) {
            UpdateEffectData();
            return;
        }
        RowDelay = 1;
        RowDelayOn = 0;

        {
            uint16_t ax = (uint16_t)(ProcessRow + 1);
            if (ax >= NumberOfRows) {
                if (StopEndOfPlaySection) {
                    Music_Stop();
                    return;
                }
                ax = BreakRow;
                BreakRow = 0;
            }
            ProcessRow = ax;
            CurrentRow = ax;
        }
        UpdateNoteData();
        return;
    }

    if (PlayMode == 0) {
        /* ---- freeplay (editor) mode ---- */
        hostchn_t *hc = HChn;
        int i;

        for (i = 0; i < 64; i++, hc++) {
            uint16_t ax;

            if (hc->CUC != 0) {
                hc->CUC--;
                if (hc->CUC == 0)
                    hc->Flags &= (uint16_t)~0x303;
            }

            ax = hc->Flags;
            if ((ax & HF_CHAN_ON) && (ax & HF_UPDATE_VOLEFCT_IF_CHAN_ON)) {
                VolumeEffectTable[hc->VCm & 7](hc);
                ax = hc->Flags;
            }

            if (!(ax & 2)) {
                if (!(ax & HF_CHAN_ON))
                    continue;
                if (!(ax & 1))
                    continue;
            }
            CommandTable[hc->Cmd & 31](hc);
        }
        return;
    }

    /* ---- PlayMode 2: song playback ---- */
    ProcessTick--;
    CurrentTick--;
    if (CurrentTick != 0) {
        UpdateNoNewRow();
        return;
    }

    CurrentTick = CurrentSpeed;
    ProcessTick = CurrentSpeed;

    RowDelay--;
    if (RowDelay != 0) {
        UpdateEffectData();
        return;
    }
    RowDelay = 1;
    RowDelayOn = 0;

    {
        uint16_t ax = (uint16_t)(ProcessRow + 1);

        if (ax >= NumberOfRows) {
            if (!(OrderLockFlag & 1)) {
                uint16_t bx = ProcessOrder;
                uint16_t dx = 0;
                uint8_t cl;

                bx++;
                for (;;) {
                    if (bx < 0x100) {
                        cl = Song.Orders[bx];
                        if (cl < 200)
                            break;          /* UpdateData_Song3 */
                        bx++;
                        if (cl == 0xFE)
                            continue;
                        StopSong = 1;
                        if (StopEndOfPlaySection) {
                            Music_Stop();
                            return;
                        }
                    }
                    /* UpdateData_Song4 */
                    if (dx != 0) {
                        Music_Stop();
                        return;
                    }
                    dx = 1;
                    bx = 0;
                }
                ProcessOrder = bx;
                CurrentOrder = bx;
                CurrentPattern = cl;
            }
            ax = BreakRow;
            BreakRow = 0;
        }
        ProcessRow = ax;
        CurrentRow = ax;
    }
    UpdateNoteData();
}

/* =====================================================================
 * Update - the driver's per-tick entry point (IT_MUSIC.ASM line 4346)
 * ===================================================================== */
void Update(void)
{
    slavechn_t *sc = SChn;
    uint16_t cx;

    MIDITranslate(&HChn[SChn[0].HCOffst & 63], &SChn[0], MIDICOMMAND_TICK);

    for (cx = MAXSLAVECHANNELS; cx != 0; cx--, sc++) {
        if (!(sc->Flags & SF_CHAN_ON))
            continue;

        if (sc->VS != sc->Vol) {
            sc->Vol = sc->VS;
            sc->Flags |= SF_RECALC_VOL;
        }
        if (sc->FrequencySet != sc->Frequency) {
            sc->Frequency = sc->FrequencySet;
            sc->Flags |= SF_FREQ_CHANGE;
        }
    }

    UpdateData();

    if (Song.Header.Flags & ITF_INSTRUMENTS)
        UpdateInstruments();
    else
        UpdateSamples();
}

/* =====================================================================
 * Music_StopChannels / Music_Stop / play control
 * ===================================================================== */
void Music_StopChannels(void)
{
    int i;

    for (i = 0; i < 64; i++) {
        HChn[i].Flags = 0;
        HChn[i].PLR = 0;        /* Mov Word Ptr [SI+26h], 0 */
        HChn[i].PLC = 0;
    }

    for (i = 0; i < MAXSLAVECHANNELS; i++) {
        slavechn_t *sc = &SChn[i];

        if ((sc->Flags & SF_CHAN_ON) && sc->Smp == 100)
            MIDITranslate(&HChn[sc->HCOffst & 63], sc,
                          MIDICOMMAND_STOPNOTE);
        sc->Flags = 0x200;
    }
}

void Music_Stop(void)
{
    int i;

    if (OrderLockFlag & 1)
        OrderLockFlag = 0;

    for (i = 0; i < MAXSLAVECHANNELS; i++) {
        slavechn_t *sc = &SChn[i];
        if ((sc->Flags & SF_CHAN_ON) && sc->Smp == 100)
            MIDITranslate(&HChn[sc->HCOffst & 63], sc,
                          MIDICOMMAND_STOPNOTE);
    }
    MIDITranslate(NULL, &SChn[0], MIDICOMMAND_STOP);

    PlayMode = 0;

    DecodeExpectedPattern = 0xFFFE;
    DecodeExpectedRow = 0xFFFE;
    RowDelay = 1;
    CurrentRow = 0;
    CurrentOrder = 0;
    CurrentTick = 1;
    BreakRow = 0;

    memset(MIDIPrograms, 0xFF, sizeof(MIDIPrograms));
    memset(MIDIBanks, 0xFF, sizeof(MIDIBanks));
    memset(MIDIPan, 0xFF, sizeof(MIDIPan));

    /* clear host channels */
    for (i = 0; i < 64; i++) {
        hostchn_t *hc = &HChn[i];
        memset(hc, 0, sizeof(*hc));
        hc->HCN = (uint8_t)i;
        hc->CP = Song.Header.ChnlPan[i] & 0x7F;
        hc->CV = Song.Header.ChnlVol[i];
    }

    /* clear slave channels */
    for (i = 0; i < MAXSLAVECHANNELS; i++) {
        slavechn_t *sc = &SChn[i];
        memset(sc, 0, sizeof(*sc));
        sc->Flags = 0x200;
        sc->HCOffst = 0xFFFF;   /* "no host" (offset 0 in the original) */
    }

    GlobalVolume = Song.Header.GV;
    CurrentSpeed = Song.Header.IS;
    ProcessTick = CurrentSpeed;
    Tempo = Song.Header.IT;

    Music_InitTempo();
}

void Music_PlayPattern(uint16_t Pattern, uint16_t NumRows, uint16_t Row)
{
    Music_Stop();

    MIDIPitchDepthSent = 0;
    LastMIDIByte = 0xFF;

    CurrentPattern = Pattern;
    CurrentRow = Row;
    NumberOfRows = NumRows;
    ProcessRow = (uint16_t)(Row - 1);
    PlayMode = 1;
}

void Music_PlaySong(uint16_t Order)
{
    Music_Stop();

    MIDIPitchDepthSent = 0;
    LastMIDIByte = 0xFF;
    StopSong = 0;

    CurrentOrder = Order;
    ProcessOrder = (uint16_t)(Order - 1);
    ProcessRow = 0xFFFE;
    PlayMode = 2;

    MIDITranslate(NULL, &SChn[0], MIDICOMMAND_START);
}

void Music_PlayNote(uint16_t Channel, const uint8_t Note[5], uint8_t DH)
{
    hostchn_t *hc = &HChn[Channel];
    uint8_t dl = 0;
    uint16_t ax;

    if (Note[0] != NONOTE) {
        dl |= 1;
        hc->Nte = Note[0];
    }
    if (Note[1] != 0) {
        dl |= 2;
        hc->Ins = Note[1];
    }
    if (Note[2] != 0xFF) {
        dl |= 4;
        hc->Vol = Note[2];
    }
    ax = (uint16_t)(Note[3] | (Note[4] << 8));
    if (ax != 0)
        dl |= 8;

    hc->Cmd = (uint8_t)ax;
    hc->CmdVal = (uint8_t)(ax >> 8);
    hc->OCm = (uint8_t)ax;
    hc->OCmVal = (uint8_t)(ax >> 8);
    hc->Msk = dl;
    hc->Flags &= (uint16_t)~(3 + 32 + 64 + 256);
    hc->Flags |= (uint16_t)(DH & 0x7F);

    hc->CUC = (uint8_t)CurrentSpeed;

    PreInitCommand(hc);

    if ((hc->Flags & HF_CHAN_ON) && (DH & 128)) {
        slavechn_t *sc = SLAVE(hc);
        sc->Pan = 32;
        sc->PS = 32;
        sc->CVl = 0x40;
    }

    DecodeExpectedRow = 0xFFFE;
}

/* =====================================================================
 * Mute / solo control
 * ===================================================================== */
static void Music_MuteChannel(uint16_t Channel)
{
    slavechn_t *sc = SChn;
    uint16_t cx;

    for (cx = NumChannels; cx != 0; cx--, sc++) {
        if (!(sc->Flags & SF_CHAN_ON))
            continue;
        if ((sc->HCN & 0x7F) != Channel)
            continue;
        sc->Flags |= 0x840;
    }
}

static void Music_UnmuteChannel(uint16_t Channel)
{
    slavechn_t *sc = SChn;
    uint16_t cx;

    SoloSample = 0xFF;
    SoloInstrument = 0xFF;

    for (cx = NumChannels; cx != 0; cx--, sc++) {
        if (!(sc->Flags & SF_CHAN_ON))
            continue;
        if ((sc->HCN & 0x7F) != Channel)
            continue;
        sc->Flags &= (uint16_t)~SF_CHN_MUTED;
        sc->Flags |= SF_RECALC_FINALVOL;
    }
}

void Music_ToggleChannel(uint16_t Channel)
{
    if (Song.Header.ChnlPan[Channel] & 0x80) {
        Song.Header.ChnlPan[Channel] &= 0x7F;
        Music_UnmuteChannel(Channel);
        MuteChannelTable[Channel] = 0;
    } else {
        MuteChannelTable[Channel] ^= 1;
        Song.Header.ChnlPan[Channel] |= 0x80;
        Music_MuteChannel(Channel);
    }
}

void Music_InitMuteTable(void)
{
    memset(MuteChannelTable, 0, sizeof(MuteChannelTable));
    SoloSample = 0xFF;
    SoloInstrument = 0xFF;
}

/* Music_SoloChannel (IT_MUSIC.ASM 6403): if the target is the only
 * unmuted channel, restore every channel muted via the mute table
 * (Music_UnmuteAll path); otherwise mute everything but the target. */
void Music_SoloChannel(uint16_t Channel)
{
    int i, playing = 0;

    for (i = 0; i < 64; i++)
        if (!(Song.Header.ChnlPan[i] & 0x80))
            playing++;

    if (playing == 1 && !(Song.Header.ChnlPan[Channel] & 0x80)) {
        for (i = 63; i >= 0; i--)           /* unmute-all */
            if (MuteChannelTable[i] == 1)
                Music_ToggleChannel((uint16_t)i);
        return;
    }

    for (i = 63; i >= 0; i--) {
        if (i == Channel) {
            if (Song.Header.ChnlPan[i] & 0x80)
                Music_ToggleChannel((uint16_t)i);
        } else {
            if (!(Song.Header.ChnlPan[i] & 0x80))
                Music_ToggleChannel((uint16_t)i);
        }
    }
}

/* Music_ToggleReverse (IT_MUSIC.ASM 7020); the info-line message is the
 * caller's job in this port. */
void Music_ToggleReverse(void)
{
    ReverseChannels ^= 1;
    RecalculateAllVolumes();
}

/* Music_GetLastChannel (IT_MUSIC.ASM 7077): last channel whose muted
 * state came from the user toggle (or is unmuted) - song-muted channels
 * with a clear mute-table entry don't count. */
uint16_t Music_GetLastChannel(void)
{
    uint16_t ax = 0, dx;

    for (dx = 0; dx < 64; dx++)
        if (((Song.Header.ChnlPan[dx] >> 7) ^ MuteChannelTable[dx]) == 0)
            ax = dx;
    return ax;
}

/* Music_NextOrder / Music_LastOrder (IT_MUSIC.ASM 6169/6196): while
 * playing a song, restart processing at the next / previous order. */
void Music_NextOrder(void)
{
    if (PlayMode != 2)
        return;
    PlayMode = 0;
    Music_StopChannels();
    ProcessRow = 0xFFFE;
    CurrentTick = 1;
    RowDelay = 1;
    PlayMode = 2;
}

void Music_LastOrder(void)
{
    if (PlayMode != 2)
        return;
    if ((int16_t)ProcessOrder > 0) {
        PlayMode = 0;
        Music_StopChannels();
        ProcessOrder -= 2;
        ProcessRow = 0xFFFE;
        CurrentTick = 1;
        RowDelay = 1;
        PlayMode = 2;
    }
}

/* =====================================================================
 * Music_InitMusic - engine initialisation (driver-independent parts of
 * the original Music_InitMusic)
 * ===================================================================== */
void Music_InitMusic(void)
{
    DriverFlags = Driver ? Driver->Flags : 0;
    GetChannels();
    Music_InitMuteTable();
    Music_Stop();
}
