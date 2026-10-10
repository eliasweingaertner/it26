/*
 * it_music.h
 * ----------
 * Engine state + internal interfaces, ported from IT_MUSIC.ASM (IT 2.17).
 * Names follow the original source so the port can be audited side by side.
 */

#ifndef IT_MUSIC_H
#define IT_MUSIC_H

#include "it_structs.h"

/* Build switches, mirroring SWITCH.INC of the 2.17 release build. */
#ifndef USEFPUCODE
#define USEFPUCODE 1    /* 1 = FPU slide code (IT 2.15+), 0 = lookup tables */
#endif

/* ---- Song storage (replaces the DOS "SongData" segment) ---- */

#define MAX_ORDERS      256
#define MAX_PATTERNS    200
#define MAX_SAMPLES     100
#define MAX_INSTRUMENTS 100

typedef struct song_t {
    songheader_t Header;                 /* ES:[0]                  */
    uint8_t      Orders[MAX_ORDERS];     /* ES:[100h]               */
    instrument_t Ins[MAX_INSTRUMENTS];   /* via table at ES:[64710] */
    sample_t     Smp[MAX_SAMPLES];       /* via table at ES:[64912] */
    pattern_t    Patterns[MAX_PATTERNS];
} song_t;

extern song_t Song;

/* MIDI macro data area, layout identical to ITMIDI.CFG / the embedded MIDI
 * configuration in .IT files (9+16+128 macros, 32 bytes each).            */
#define MIDIDATAAREA_SIZE (9*32 + 16*32 + 128*32)
extern char MIDIDataArea[MIDIDATAAREA_SIZE];
void SetDefaultMIDIDataArea(void);  /* stock ITMIDI.CFG (it_load.c) */

/* D_LoadSampleData port (it_load.c): read + convert one sample's data
 * from a whole-file image per the header's Flags/Cvt (incl. IT214/215
 * decompression); allocates s->Data, rewrites Flags/Cvt like the
 * original. For the sample/instrument library (it_ris.c). */
int Load_SampleData(const uint8_t *filedata, size_t size, sample_t *s);
/* stereo Left/Right requester hook (O1_StereoSampleList; feature 013):
 * returns 64 = left, 192 = right; NULL = left (headless paths). */
extern int (*Load_StereoChoice)(void);
/* load-progress hook (D_LoadIT's log, IT_D_RM.INC 2360): the editor
 * draws "<what> n" at (4,row); NULL = silent (headless paths) */
enum { LOAD_HEADER, LOAD_INSTRUMENT, LOAD_SHEADER, LOAD_SAMPLE,
       LOAD_PATTERN };
extern void (*Load_Progress)(int row, int what, int n);
/* pre-2.00 instrument conversion, shared with the library loaders */
void Load_OldInstrument(const uint8_t *src, instrument_t *in);

/* MIDICOMMAND_* equates, IT_MUSIC.ASM line 205-214 (offsets into
 * MIDIDataArea, except CHANGEPITCH which is handled internally). */
#define MIDICOMMAND_START         0x0000
#define MIDICOMMAND_STOP          0x0020
#define MIDICOMMAND_TICK          0x0040
#define MIDICOMMAND_PLAYNOTE      0x0060
#define MIDICOMMAND_STOPNOTE      0x0080
#define MIDICOMMAND_CHANGEVOLUME  0x00A0
#define MIDICOMMAND_CHANGEPAN     0x00C0
#define MIDICOMMAND_BANKSELECT    0x00E0
#define MIDICOMMAND_PROGRAMSELECT 0x0100
#define MIDICOMMAND_CHANGEPITCH   0xFFFF

/* ---- Channel tables ---- */

extern hostchn_t  HChn[64];     /* HostChannelInformationTable  */
extern slavechn_t SChn[MAXSLAVECHANNELS]; /* SlaveChannelInformationTable */
extern uint8_t    OrderLockFlag;  /* Alt-F11 order lock (bit 0) */
extern uint8_t    SamplePlayTable[128], InstrumentPlayTable[128];
extern uint8_t    MuteChannelTable[64];

#define SLAVE(hc)  (&SChn[(hc)->SCOffst])
#define HOSTCH(sc) (&HChn[(sc)->HCOffst])

/* Instrument n is 1-based ([ES:64710+n*2]); sample idx is 0-based
 * ([ES:64912+idx*2]). InsOffs holds n (1-based), SmpOffs holds idx. */
#define INSTRUMENT(n)  (&Song.Ins[(n) - 1])
#define SAMPLEHDR(idx) (&Song.Smp[idx])

/* ---- Engine globals (zero/non-zero blocks of IT_MUSIC.ASM) ---- */

extern uint16_t LastSample;
extern uint16_t PlayMode;        /* 0 = freeplay, 1 = pattern, 2 = song */
extern uint16_t CurrentOrder;
extern uint16_t CurrentPattern;
extern uint16_t CurrentRow;
extern uint16_t ProcessOrder;
extern uint16_t ProcessRow;
extern uint16_t BreakRow;
extern uint8_t  RowDelay;
extern uint8_t  RowDelayOn;
extern uint16_t NumberOfRows;
extern uint16_t CurrentTick;
extern uint16_t CurrentSpeed;
extern uint16_t ProcessTick;
extern uint8_t  Tempo;
extern uint8_t  GlobalVolume;
extern uint16_t NumChannels;
extern uint8_t  SoloSample;      /* 0xFF = none, else sample (0-based)   */
extern uint8_t  SoloInstrument;  /* 0xFF = none, else instrument (0-based)*/
extern uint8_t  StopSong;        /* set when the order list runs out      */
extern uint8_t  PatternLooping;
extern uint8_t  ReverseChannels;

extern uint16_t DecodeExpectedPattern;
extern uint16_t DecodeExpectedRow;

extern slavechn_t *LastSlaveChannel; /* NULL = none */

/* ---- Driver interface (DriverVariableTable / function pointers) ---- */

typedef struct sounddriver_t {
    const char *Name;
    uint16_t MaxChannels;       /* DriverMaxChannels   */
    uint16_t Flags;             /* DriverFlags: bit0 = MIDI out supported,
                                   bit1 = HiQual, bit2 = waveform data    */
    int  (*InitSound)(void);
    void (*UninitSound)(void);
    void (*SetTempo)(uint8_t Tempo);
    void (*SetMixVolume)(uint8_t Vol);     /* 0..128 */
    void (*SetStereo)(uint8_t Stereo);
    /* The original passes channel context in DI (host) / SI (slave). */
    void (*MIDIOut)(hostchn_t *hc, slavechn_t *sc, uint8_t Byte);
} sounddriver_t;

extern const sounddriver_t *Driver;
extern uint16_t DriverFlags;       /* cached Driver->Flags */
extern uint16_t StopEndOfPlaySection;

/* ---- Engine API (Music_* in the original) ---- */

void Music_InitMusic(void);        /* engine + channel state initialisation */
void Music_Stop(void);
void Music_PlaySong(uint16_t Order);
void Music_PlayPartSong(uint16_t Order, uint16_t Row);
uint16_t Music_IncreaseVolume(void);
uint16_t Music_DecreaseVolume(void);
int  Music_ToggleSolo(int instrument, uint8_t num);
void Music_PlayPattern(uint16_t Pattern, uint16_t NumRows, uint16_t Row);
void Music_PlayNote(uint16_t Channel, const uint8_t Note[5], uint8_t DH);
void Music_PlaySample(uint8_t Note, uint8_t SmpNum, uint16_t Channel);
void Music_StopChannels(void);
void Music_InitTempo(void);
void Music_InitMixTable(void);
void Music_InitStereo(void);
void Music_InitMuteTable(void);
void Music_ToggleChannel(uint16_t Channel);
void Music_SoloChannel(uint16_t Channel);
void Music_UnmuteAll(void);
void Music_ToggleReverse(void);
uint16_t Music_GetLastChannel(void);
void Music_NextOrder(void);
void Music_LastOrder(void);
void Music_SetGlobalVolume(uint8_t Vol);
uint16_t Music_IncreaseSpeed(void);
uint16_t Music_DecreaseSpeed(void);
void GetChannels(void);
void RecalculateAllVolumes(void);

/* Per-tick update, called by the sound driver once every 2.5/Tempo sec. */
void Update(void);

/* ---- Internals shared between it_music.c and it_effects.c ---- */

uint8_t Random(void);            /* returns AL (low byte of Seed1)       */
void    Music_ResetRNG(void);    /* restore Seed1/Seed2 to power-on state */
/* editor -> engine: a pattern's PackedData buffer was just replaced
 * (freed + reallocated). If it is the one being decoded, invalidate the
 * cached decode cursor so the next tick re-derives it from the new
 * buffer. Must be called under the engine lock. */
void    Music_NotifyPatternRepacked(uint16_t patnum);
/* reset all 99 instruments to the default template (used when switching a
 * sample-mode module into instrument mode; IT_MUSIC.ASM 3316) */
void    Music_ClearAllInstruments(void);
/* stamp one instrument with the pristine InstrumentHeader template */
void    Music_InitInstrument(instrument_t *in);
/* blank = byte-equal to the template (or all-zero, a port extension) */
int     Music_InstrumentIsBlank(const instrument_t *in);
/* last non-blank instrument slot, 0..99 (IT_MUSIC.ASM 5460) */
int     Music_GetNumberOfInstruments(void);
void    Music_InitSample(sample_t *s);          /* SampleHeader template */
int     Music_SampleIsBlank(const sample_t *s);
int     Music_GetNumberOfSamples(void);
void    Music_StampBlankSamples(void);
/* host a sample in an instrument: same-numbered slot if blank, else the
 * first blank one; returns the 1-based instrument or 0 (IT_MUSIC.ASM 6672) */
int     Music_AssignSampleToInstrument(int smp0);
/* queue the order to play when the current pattern ends (IT_MUSIC.ASM 7131) */
void    Music_SetNextOrder(uint16_t order);
void GetLoopInformation(slavechn_t *sc);
/* Returns the allocated slave channel or NULL; *hflags is the caller's
 * working copy of the host flags low byte (CH in the original), bit 4
 * is cleared on failure. */
slavechn_t *AllocateChannelPtr(hostchn_t *hc, uint8_t *hflags);
void InitPlayInstrument(hostchn_t *hc, slavechn_t *sc, uint16_t InsNum);
void ApplyRandomValues(hostchn_t *hc);
void PitchSlideUp(hostchn_t *hc, slavechn_t *sc, int16_t Val);
void PitchSlideUpLinear(hostchn_t *hc, slavechn_t *sc, int16_t Val);
void PitchSlideUpAmiga(hostchn_t *hc, slavechn_t *sc, int16_t Val);
void PitchSlideDown(hostchn_t *hc, slavechn_t *sc, int16_t Val);
#if !USEFPUCODE
void PitchSlideDownLinear(hostchn_t *hc, slavechn_t *sc, int16_t Val);
void PitchSlideDownAmiga(hostchn_t *hc, slavechn_t *sc, int16_t Val);
#endif
void MIDITranslate(hostchn_t *hc, slavechn_t *sc, uint16_t MacroOffset);
void SetFilterCutoff(slavechn_t *sc, uint8_t Val);    /* BL = value */
void SetFilterResonance(slavechn_t *sc, uint8_t Val);

extern const uint32_t PitchTable[132];
/* FineSineData/FineRampDownData/FineSquareWave, contiguous as in the
 * original binary (UpdateVibrato indexes with waveform*256+position). */
extern const int8_t FineWaveData[768];
#define FineSineData     (&FineWaveData[0])
#define FineRampDownData (&FineWaveData[256])
#define FineSquareWave   (&FineWaveData[512])
#if !USEFPUCODE
extern const uint32_t FineLinearSlideUpTable[16];
extern const uint32_t LinearSlideUpTable[257];
extern const uint16_t FineLinearSlideDownTable[16];
extern const uint16_t LinearSlideDownTable[257];
#endif

/* Effect handlers (IT_M_EFF.INC) -------------------------------------- */

typedef void (*effect_fn)(hostchn_t *hc);

extern const effect_fn InitCommandTable[32];
extern const effect_fn CommandTable[32];
extern const effect_fn VolumeEffectTable[8];

void InitNoCommand(hostchn_t *hc);
void InitCommandA(hostchn_t *hc); void InitCommandB(hostchn_t *hc);
void InitCommandC(hostchn_t *hc); void InitCommandD(hostchn_t *hc);
void InitCommandE(hostchn_t *hc); void InitCommandF(hostchn_t *hc);
void InitCommandG(hostchn_t *hc); void InitCommandH(hostchn_t *hc);
void InitCommandI(hostchn_t *hc); void InitCommandJ(hostchn_t *hc);
void InitCommandK(hostchn_t *hc); void InitCommandL(hostchn_t *hc);
void InitCommandM(hostchn_t *hc); void InitCommandN(hostchn_t *hc);
void InitCommandO(hostchn_t *hc); void InitCommandP(hostchn_t *hc);
void InitCommandQ(hostchn_t *hc); void InitCommandR(hostchn_t *hc);
void InitCommandS(hostchn_t *hc); void InitCommandT(hostchn_t *hc);
void InitCommandU(hostchn_t *hc); void InitCommandV(hostchn_t *hc);
void InitCommandW(hostchn_t *hc); void InitCommandX(hostchn_t *hc);
void InitCommandY(hostchn_t *hc); void InitCommandZ(hostchn_t *hc);

void NoCommand(hostchn_t *hc);
void CommandD(hostchn_t *hc); void CommandE(hostchn_t *hc);
void CommandF(hostchn_t *hc); void CommandG(hostchn_t *hc);
void CommandH(hostchn_t *hc); void CommandI(hostchn_t *hc);
void CommandJ(hostchn_t *hc); void CommandK(hostchn_t *hc);
void CommandL(hostchn_t *hc); void CommandN(hostchn_t *hc);
void CommandP(hostchn_t *hc); void CommandQ(hostchn_t *hc);
void CommandR(hostchn_t *hc); void CommandS(hostchn_t *hc);
void CommandT(hostchn_t *hc); void CommandW(hostchn_t *hc);
void CommandY(hostchn_t *hc);

void VolumeCommandC(hostchn_t *hc); void VolumeCommandD(hostchn_t *hc);
void VolumeCommandE(hostchn_t *hc); void VolumeCommandF(hostchn_t *hc);
void VolumeCommandG(hostchn_t *hc);

/* Helper shared by effects: InitVolumeEffect, called from InitNoCommand /
 * InitCommandG paths (lives in it_effects.c). */
void InitVolumeEffect(hostchn_t *hc);

#endif /* IT_MUSIC_H */
