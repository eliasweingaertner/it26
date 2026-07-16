/*
 * it_structs.h
 * ------------
 * Native cross-platform port of Impulse Tracker 2.17's player engine.
 * Transliterated from the original x86 assembly source (IT_MUSIC.ASM,
 * IT_M_EFF.INC, the SoundDrivers/ tree) by Jeffrey Lim (Pulse).
 *
 * The host/slave channel layouts below mirror the original byte layouts
 * documented in InternalDocumentation/CHANNEL.TXT.  Offsets from the ASM
 * ([DI+xx] for host channels, [SI+xx] for slave channels) are noted on each
 * field and enforced with static asserts so the port stays 1:1 auditable
 * against the original source.
 */

#ifndef IT_STRUCTS_H
#define IT_STRUCTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Portable "this may legitimately go unreferenced" marker, for feature
 * hooks and 1:1 ASM-audit tables that are compiled in but not yet wired
 * to a caller.  Keeps -Wunused-{function,variable} quiet without deleting
 * intentional code. */
#if defined(__GNUC__) || defined(__clang__)
#  define IT_MAYBE_UNUSED __attribute__((unused))
#else
#  define IT_MAYBE_UNUSED
#endif

/* IT_MUSIC.ASM line 200-203 */
#define MAXSLAVECHANNELS 256
#define NONOTE           0xFD

#pragma pack(push, 1)

/* ---------------------------------------------------------------------------
 * Host channel ("pattern channel"), 80 bytes (HOSTCHANNELSIZE EQU 80).
 * Original ASM addresses these as [DI+offset].
 * ------------------------------------------------------------------------- */
typedef struct hostchn_t {
    uint16_t Flags;       /* +00  HF_* flags below                          */
    uint8_t  Msk;         /* +02  pattern read mask                         */
    uint8_t  Nte;         /* +03  raw note from pattern                     */
    uint8_t  Ins;         /* +04  instrument from pattern                   */
    uint8_t  Vol;         /* +05  volume column byte                        */
    uint8_t  Cmd;         /* +06  command (1=A .. 26=Z)                     */
    uint8_t  CmdVal;      /* +07  command parameter                         */
    uint8_t  OCm;         /* +08  old command (for update)                  */
    uint8_t  OCmVal;      /* +09  old command parameter                     */
    uint8_t  VCm;         /* +0A  volume column command (for update)        */
    uint8_t  VCmVal;      /* +0B  volume column command value               */
    uint8_t  MCh;         /* +0C  MIDI channel (0 = not MIDI)               */
    uint8_t  MPr;         /* +0D  MIDI program                              */
    uint8_t  Nt2;         /* +0E  translated note                           */
    uint8_t  Smp;         /* +0F  translated sample (0-based, 0FFh = none)  */

    uint8_t  DKL;         /* +10  D/K/L effect memory                       */
    uint8_t  EFG;         /* +11  E/F/G effect memory                       */
    uint8_t  O00;         /* +12  Oxx effect memory                         */
    uint8_t  I00;         /* +13  Ixx effect memory                         */
    uint8_t  J00;         /* +14  Jxx effect memory                         */
    uint8_t  M00;         /* +15  Mxx effect memory (unused by engine)      */
    uint8_t  N00;         /* +16  Nxx effect memory                         */
    uint8_t  P00;         /* +17  Pxx effect memory                         */
    uint8_t  Q00;         /* +18  Qxx effect memory                         */
    uint8_t  T00;         /* +19  Txx effect memory                         */
    uint8_t  S00;         /* +1A  Sxx effect memory                         */
    uint8_t  OxH;         /* +1B  high order offset for SAy (Oxx * 10000h)  */
    uint8_t  W00;         /* +1C  Wxx effect memory                         */
    uint8_t  VCE;         /* +1D  volume column effect memory               */
    uint8_t  GOE;         /* +1E  Gxx with old effects flag                 */
    uint8_t  SFx;         /* +1F  S-effect memory (SFx)                     */

    uint8_t  HCN;         /* +20  host channel number                       */
    uint8_t  CUC;         /* +21  command update count (playmode 0)         */
    uint8_t  VSe;         /* +22  volume set                                */
    uint8_t  LTr;         /* +23  last tremolo                              */
    uint16_t SCOffst;     /* +24  slave channel offset (ptr in this port)   */
    uint8_t  PLR;         /* +26  pattern loop row                          */
    uint8_t  PLC;         /* +27  pattern loop count                        */
    uint8_t  PWF;         /* +28  panbrello wave form                       */
    uint8_t  PPo;         /* +29  panbrello position                        */
    uint8_t  PDp;         /* +2A  panbrello depth                           */
    uint8_t  PSp;         /* +2B  panbrello speed                           */
    uint8_t  LPn;         /* +2C  last panbrello / Last pan slide value     */
    uint8_t  LVi;         /* +2D  last vibrato                              */
    uint8_t  CP;          /* +2E  channel pan                               */
    uint8_t  CV;          /* +2F  channel volume                            */

    uint8_t  VCh;         /* +30  volume change (for command D/K/L)         */
    uint8_t  TCD;         /* +31  tremor count down                         */
    uint8_t  Too;         /* +32  tremor on/off (on = 1)                    */
    uint8_t  RTC;         /* +33  retrig count                              */
    int32_t  PortaFreq;   /* +34  portamento (Gxx) target frequency         */
    uint8_t  VWF;         /* +38  vibrato wave form                         */
    uint8_t  VPo;         /* +39  vibrato position                          */
    uint8_t  VDp;         /* +3A  vibrato depth                             */
    uint8_t  VSp;         /* +3B  vibrato speed                             */
    uint8_t  TWF;         /* +3C  tremolo wave form                         */
    uint8_t  TPo;         /* +3D  tremolo position                          */
    uint8_t  TDp;         /* +3E  tremolo depth                             */
    uint8_t  TSp;         /* +3F  tremolo speed                             */

    uint8_t  MiscEfctData[16]; /* +40  misc effect data                     */
} hostchn_t;

/* Host channel flags, CHANNEL.TXT + IT_MUSIC.ASM usage */
#define HF_UPDATE_EFCT_IF_CHAN_ON 0x0001
#define HF_ALWAYS_UPDATE_EFCT     0x0002
#define HF_UPDATE_MODE_MASK       0x0003
#define HF_CHAN_ON                0x0004 /* bit value 4 in doc                */
#define HF_CHAN_CUT               0x0008 /* "channel cut" (no longer used)    */
#define HF_PITCH_SLIDE_ONGOING    0x0010 /* slide in progress (G/L)           */
#define HF_FREEPLAY_NOTE          0x0020 /* don't check channel on/off        */
#define HF_ROW_UPDATED            0x0040
#define HF_APPLY_RANDOM_VOL       0x0080
#define HF_UPDATE_VOLEFCT_IF_CHAN_ON 0x0100
#define HF_ALWAYS_VOLEFCT         0x0200
#define HF_DONT_TOUCH_IN_INT      0x8000

/* Envelope playback state, embedded 3x in the slave channel.            */
typedef struct envstate_t {
    int32_t  Value;       /* +0  current value, 16.16 fixed point          */
    int32_t  Delta;       /* +4  per-tick delta, 16.16 fixed point         */
    uint16_t Pos;         /* +8  position (ticks)                          */
    uint16_t CurNode;     /* +A  current node (low byte used)              */
    uint16_t NextTick;    /* +C  next node's tick                          */
    uint16_t filter;      /* +E  filtera/filterb/filterc (mixer state)     */
} envstate_t;

/* ---------------------------------------------------------------------------
 * Slave channel ("virtual/voice channel"), 128 bytes (SLAVECHANNELSIZE).
 * Original ASM addresses these as [SI+offset].
 * ------------------------------------------------------------------------- */
typedef struct slavechn_t {
    uint16_t Flags;       /* +00  SF_* flags below                          */

    /* +02..+09: "device specific" area. The software (SB16/WAV) drivers
     * keep the mixer's frequency-to-step value and last volumes here.     */
    uint32_t MixStep;     /* +02  16.16 fixed point step (SB16 layout)      */
    uint8_t  LeftVolume2; /* +06  driver scratch                            */
    uint8_t  RightVolume2;/* +07  driver scratch                            */
    uint16_t MixMode;     /* +08  driver scratch / mix mode                 */

    uint8_t  LpM;         /* +0A  loop mode: 0 none / 8 fwd / 24 pingpong   */
    uint8_t  LpD;         /* +0B  loop direction (pingpong), 1 = backwards.
                                  For MIDI: pattern note.                   */
    int32_t  LeftVolume;  /* +0C  final 32-bit left volume (0-16384 range)  */

    int32_t  Frequency;   /* +10  final note frequency (incl. vibrato)      */
    int32_t  FrequencySet;/* +14  calculated note frequency (E/F/G/L)       */
    uint8_t  Bit;         /* +18  sample flags-ish: 2 = 16 bit, 0 = 8 bit   */
    uint8_t  ViP;         /* +19  auto-vibrato position                     */
    uint16_t ViDepth;     /* +1A  auto-vibrato depth (8.8 fixed point)      */
    int32_t  RightVolume; /* +1C  final 32-bit right volume / MIDI FSet     */

    uint8_t  FV;          /* +20  final volume (0-128)                      */
    uint8_t  Vol;         /* +21  volume (altered by R/I)                   */
    uint8_t  VS;          /* +22  volume set (altered by D/K/L)             */
    uint8_t  CVl;         /* +23  channel volume                            */
    uint8_t  SVl;         /* +24  sample global volume                      */
    uint8_t  FP;          /* +25  final pan                                 */
    uint16_t FadeOut;     /* +26  fadeout counter (1024 -> 0)               */
    uint8_t  DCT;         /* +28  duplicate check type                      */
    uint8_t  DCA;         /* +29  duplicate check action                    */
    uint8_t  Pan;         /* +2A  pan: 0-64, 100 = surround, >=128 = muted  */
    uint8_t  PS;          /* +2B  pan set                                   */
    uint32_t OldSampleOffset; /* +2C                                        */

    uint16_t InsOffs;     /* +30  instrument offset -> index in this port   */
    uint8_t  Nte;         /* +32  note                                      */
    uint8_t  Ins;         /* +33  instrument number (0-based, 0xFF none)    */
    uint16_t SmpOffs;     /* +34  sample header offset -> index in port     */
    uint8_t  Smp;         /* +36  sample number (0-based)                   */
    uint8_t  FPP;         /* +37  final playing pan (after reverse)         */
    uint16_t HCOffst;     /* +38  host channel offset -> index in port      */
    uint8_t  HCN;         /* +3A  host channel number, +128 if disowned     */
    uint8_t  NNA;         /* +3B  new note action                           */
    uint8_t  MCh;         /* +3C  MIDI channel                              */
    uint8_t  MPr;         /* +3D  MIDI program                              */
    uint16_t MBank;       /* +3E  MIDI bank / filter cutoff+resonance       */

    int32_t  LoopBeg;     /* +40  loop beginning                            */
    int32_t  LoopEnd;     /* +44  loop end                                  */
    uint16_t SmpErr;      /* +48  mixer fractional position                 */
    uint16_t Vol16b;      /* +4A  16-bit volume (mixer scratch)             */
    int32_t  SampleOffset;/* +4C  current sample position (integer)         */

    /* +50/+60/+70: VEnv/PEnv/PtEnv state, 16 bytes each:
     *   +0 Value (16.16), +4 Delta, +8 Pos, +A CurNode, +C NextTick,
     *   +E filtera/b/c (mixer's filter state words).                     */
    envstate_t VEnv;      /* +50 */
    envstate_t PEnv;      /* +60 */
    envstate_t PtEnv;     /* +70 */
} slavechn_t;

/* Slave channel flags, CHANNEL.TXT */
#define SF_CHAN_ON        0x0001
#define SF_RECALC_PAN     0x0002
#define SF_NOTE_OFF       0x0004
#define SF_FADEOUT        0x0008
#define SF_RECALC_VOL     0x0010
#define SF_FREQ_CHANGE    0x0020
#define SF_RECALC_FINALVOL 0x0040
#define SF_CENTRAL_PAN    0x0080
#define SF_NEW_NOTE       0x0100
#define SF_NOTE_STOP      0x0200
#define SF_LOOP_CHANGED   0x0400
#define SF_CHN_MUTED      0x0800
#define SF_VOLENV_ON      0x1000
#define SF_PANENV_ON      0x2000
#define SF_PITCHENV_ON    0x4000
#define SF_PAN_CHANGED    0x8000

/* ---------------------------------------------------------------------------
 * Envelope / instrument / sample / song header layouts.
 * These mirror the on-disk IT 2.x format (ITTECH.TXT) because the original
 * keeps them in memory in file layout (SongData segment).
 * ------------------------------------------------------------------------- */

typedef struct envnode_t {
    int8_t   Magnitude;   /* y value */
    uint16_t Tick;        /* x value */
} envnode_t;

typedef struct env_t {
    uint8_t   Flags;      /* +0: 1=on, 2=loop, 4=susloop, 8=carry, 128=filter */
    uint8_t   Num;        /* +1: number of node points                        */
    uint8_t   LpB;        /* +2: loop beginning                               */
    uint8_t   LpE;        /* +3: loop end                                     */
    uint8_t   SLB;        /* +4: sustain loop beginning                       */
    uint8_t   SLE;        /* +5: sustain loop end                             */
    envnode_t NodePoints[25]; /* +6: 25 sets of node points                   */
    uint8_t   Reserved;   /* +81                                              */
} env_t;

/* Instrument, 554 bytes on disk ("IMPI"). ASM accesses as [ES:BX+offset]. */
typedef struct instrument_t {
    uint32_t ID;          /* +00  "IMPI"                                    */
    char     DOSFileName[12]; /* +04                                        */
    uint8_t  Zero;        /* +10h always 0                                  */
    uint8_t  NNA;         /* +11h new note action                           */
    uint8_t  DCT;         /* +12h duplicate check type                      */
    uint8_t  DCA;         /* +13h duplicate check action                    */
    uint16_t FadeOut;     /* +14h fadeout (0-128, *2 applied = 256 full)    */
    uint8_t  PPS;         /* +16h pitch-pan separation                      */
    uint8_t  PPC;         /* +17h pitch-pan centre                          */
    uint8_t  GbV;         /* +18h global volume (0-128)                     */
    uint8_t  DfP;         /* +19h default pan (bit 128 = don't use)         */
    uint8_t  RV;          /* +1Ah random volume variation (%)               */
    uint8_t  RP;          /* +1Bh random pan variation                      */
    uint16_t TrkVers;     /* +1Ch                                           */
    uint8_t  NoS;         /* +1Eh number of samples                         */
    uint8_t  x;           /* +1Fh                                           */
    char     InstrumentName[26]; /* +20h                                    */
    uint8_t  IFC;         /* +3Ah initial filter cutoff                     */
    uint8_t  IFR;         /* +3Bh initial filter resonance                  */
    uint8_t  MCh;         /* +3Ch MIDI channel                              */
    uint8_t  MPr;         /* +3Dh MIDI program                              */
    uint16_t MIDIBnk;     /* +3Eh MIDI bank                                 */
    uint8_t  NoteSampleTable[240]; /* +40h note/sample pairs                */
    env_t    VEnvelope;   /* +130h volume envelope                          */
    env_t    PEnvelope;   /* +182h pan envelope                             */
    env_t    PtEnvelope;  /* +1D4h pitch/filter envelope                    */
    uint8_t  Reserved2[4];/* +226h..+229h: total 554 bytes                  */
} instrument_t;

/* Sample header, 80 bytes on disk ("IMPS"). ASM accesses as [offset]. */
typedef struct sample_t {
    uint32_t ID;          /* +00  "IMPS"                                    */
    char     DOSFileName[12]; /* +04                                        */
    uint8_t  Zero;        /* +10h always 0                                  */
    uint8_t  GvL;         /* +11h global volume (0-64)                      */
    uint8_t  Flags;       /* +12h bit0 sample assoc., bit1 16bit, bit2 stereo,
                                  bit3 compressed, bit4 loop, bit5 susloop,
                                  bit6 pingpong, bit7 pingpong sustain      */
    uint8_t  Vol;         /* +13h default volume                            */
    char     SampleName[26]; /* +14h                                        */
    uint8_t  Cvt;         /* +2Eh convert flags                             */
    uint8_t  DfP;         /* +2Fh default pan (bit 128 = use pan)           */
    uint32_t Length;      /* +30h length in samples                         */
    uint32_t LoopBeg;     /* +34h                                           */
    uint32_t LoopEnd;     /* +38h                                           */
    uint32_t C5Speed;     /* +3Ch                                           */
    uint32_t SusLoopBeg;  /* +40h                                           */
    uint32_t SusLoopEnd;  /* +44h                                           */
    uint32_t OffsetInFile;/* +48h sample pointer (file offset on disk).
                                  Unused at runtime in this port; we keep a
                                  flat pointer below.                       */
    uint8_t  ViS;         /* +4Ch vibrato speed                             */
    uint8_t  ViD;         /* +4Dh vibrato depth                             */
    uint8_t  ViR;         /* +4Eh vibrato rate (sweep)                      */
    uint8_t  ViT;         /* +4Fh vibrato waveform                          */

    /* ---- not part of the 80-byte on-disk header: runtime extension ---- */
    void    *Data;        /* sample data, 8-bit signed or 16-bit signed     */
} sample_t;

/* Song header ("IMPM"), in-memory copy of the on-disk layout (0xC0 bytes
 * before the order list). ASM addresses these inside the SongData segment. */
typedef struct songheader_t {
    uint32_t ID;          /* +00  "IMPM"                                    */
    char     SongName[26];/* +04                                            */
    uint16_t PHiligt;     /* +1Eh pattern row hilight info                  */
    uint16_t OrdNum;      /* +20h                                           */
    uint16_t InsNum;      /* +22h                                           */
    uint16_t SmpNum;      /* +24h                                           */
    uint16_t PatNum;      /* +26h                                           */
    uint16_t Cwt;         /* +28h created with tracker version              */
    uint16_t Cmwt;        /* +2Ah compatible with version                   */
    uint16_t Flags;       /* +2Ch bit0 stereo, bit1 vol0 mix, bit2 instruments,
                                  bit3 linear slides, bit4 old effects,
                                  bit5 link G to E/F, bit6 MIDI pitch ctrl,
                                  bit7 request MIDI macros                  */
    uint16_t Special;     /* +2Eh                                           */
    uint8_t  GV;          /* +30h global volume (0-128)                     */
    uint8_t  MV;          /* +31h mix volume (0-128)                        */
    uint8_t  IS;          /* +32h initial speed                             */
    uint8_t  IT;          /* +33h initial tempo                             */
    uint8_t  Sep;         /* +34h panning separation (0-128)                */
    uint8_t  PWD;         /* +35h pitch wheel depth                         */
    uint16_t MsgLgth;     /* +36h                                           */
    uint32_t MsgOffset;   /* +38h                                           */
    uint32_t Reserved;    /* +3Ch                                           */
    uint8_t  ChnlPan[64]; /* +40h                                           */
    uint8_t  ChnlVol[64]; /* +80h                                           */
} songheader_t;

#pragma pack(pop)

/* Layout checks: these offsets must match the ASM [reg+xx] accesses. */
#define IT_OFFSET_ASSERT(t, f, o) \
    _Static_assert(offsetof(t, f) == (o), #t "." #f " offset mismatch")

IT_OFFSET_ASSERT(hostchn_t, Msk, 0x02);
IT_OFFSET_ASSERT(hostchn_t, Cmd, 0x06);
IT_OFFSET_ASSERT(hostchn_t, MCh, 0x0C);
IT_OFFSET_ASSERT(hostchn_t, Smp, 0x0F);
IT_OFFSET_ASSERT(hostchn_t, DKL, 0x10);
IT_OFFSET_ASSERT(hostchn_t, GOE, 0x1E);
IT_OFFSET_ASSERT(hostchn_t, HCN, 0x20);
IT_OFFSET_ASSERT(hostchn_t, SCOffst, 0x24);
IT_OFFSET_ASSERT(hostchn_t, CP, 0x2E);
IT_OFFSET_ASSERT(hostchn_t, VCh, 0x30);
IT_OFFSET_ASSERT(hostchn_t, PortaFreq, 0x34);
IT_OFFSET_ASSERT(hostchn_t, TSp, 0x3F);
IT_OFFSET_ASSERT(hostchn_t, MiscEfctData, 0x40);
_Static_assert(sizeof(hostchn_t) == 80, "HOSTCHANNELSIZE");

IT_OFFSET_ASSERT(slavechn_t, LpM, 0x0A);
IT_OFFSET_ASSERT(slavechn_t, LeftVolume, 0x0C);
IT_OFFSET_ASSERT(slavechn_t, Frequency, 0x10);
IT_OFFSET_ASSERT(slavechn_t, FrequencySet, 0x14);
IT_OFFSET_ASSERT(slavechn_t, Bit, 0x18);
IT_OFFSET_ASSERT(slavechn_t, RightVolume, 0x1C);
IT_OFFSET_ASSERT(slavechn_t, FV, 0x20);
IT_OFFSET_ASSERT(slavechn_t, FadeOut, 0x26);
IT_OFFSET_ASSERT(slavechn_t, Pan, 0x2A);
IT_OFFSET_ASSERT(slavechn_t, InsOffs, 0x30);
IT_OFFSET_ASSERT(slavechn_t, HCN, 0x3A);
IT_OFFSET_ASSERT(slavechn_t, MBank, 0x3E);
IT_OFFSET_ASSERT(slavechn_t, LoopBeg, 0x40);
IT_OFFSET_ASSERT(slavechn_t, SampleOffset, 0x4C);
IT_OFFSET_ASSERT(slavechn_t, VEnv, 0x50);
IT_OFFSET_ASSERT(slavechn_t, PEnv, 0x60);
IT_OFFSET_ASSERT(slavechn_t, PtEnv, 0x70);
_Static_assert(sizeof(slavechn_t) == 128, "SLAVECHANNELSIZE");

IT_OFFSET_ASSERT(instrument_t, NNA, 0x11);
IT_OFFSET_ASSERT(instrument_t, FadeOut, 0x14);
IT_OFFSET_ASSERT(instrument_t, IFC, 0x3A);
IT_OFFSET_ASSERT(instrument_t, MIDIBnk, 0x3E);
IT_OFFSET_ASSERT(instrument_t, NoteSampleTable, 0x40);
IT_OFFSET_ASSERT(instrument_t, VEnvelope, 0x130);
IT_OFFSET_ASSERT(instrument_t, PEnvelope, 0x182);
IT_OFFSET_ASSERT(instrument_t, PtEnvelope, 0x1D4);
_Static_assert(sizeof(instrument_t) == 554, "instrument size");

IT_OFFSET_ASSERT(sample_t, GvL, 0x11);
IT_OFFSET_ASSERT(sample_t, Length, 0x30);
IT_OFFSET_ASSERT(sample_t, C5Speed, 0x3C);
IT_OFFSET_ASSERT(sample_t, ViS, 0x4C);

IT_OFFSET_ASSERT(songheader_t, OrdNum, 0x20);
IT_OFFSET_ASSERT(songheader_t, Flags, 0x2C);
IT_OFFSET_ASSERT(songheader_t, GV, 0x30);
IT_OFFSET_ASSERT(songheader_t, ChnlPan, 0x40);

/* Song flags (header +2Ch) */
#define ITF_STEREO        0x01
#define ITF_VOL0_OPT      0x02  /* obsolete */
#define ITF_INSTRUMENTS   0x04
#define ITF_LINEAR_SLIDES 0x08
#define ITF_OLD_EFFECTS   0x10
#define ITF_LINK_G_TO_EF  0x20
#define ITF_MIDI_PITCH    0x40
#define ITF_REQ_MACROS    0x80

/* A loaded pattern: original keeps the packed on-disk pattern data in
 * memory and the player decodes it row by row. PackedData points to the
 * packed stream (without the 8-byte header). */
typedef struct pattern_t {
    uint16_t Rows;        /* number of rows                                 */
    uint16_t DataLength;  /* length of packed data                          */
    uint8_t *PackedData;  /* NULL = empty pattern (use 64-row empty)        */
} pattern_t;

#endif /* IT_STRUCTS_H */
