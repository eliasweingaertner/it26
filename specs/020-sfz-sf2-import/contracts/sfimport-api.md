# Contract: `src/it_sfimport.h`

Editor-side only (not linked into `itplay` / `test_pattern`).

```c
/* status of the last SFI_Load, for the status line */
typedef struct sfi_report_t {
    int needed;     /* unique IT samples the instrument wanted        */
    int loaded;     /* IT samples created (after reduce-to-fit)       */
    int skipped;    /* regions dropped: missing / unsupported audio   */
    char why[64];   /* first reason, e.g. "unsupported format: x.ogg" */
} sfi_report_t;

int SFI_IsSF2(const uint8_t *head, size_t n);          /* "RIFF"....."sfbk" */
int SFI_ScanSF2(const char *path, ilibent_t *ents, int max);
        /* presets as RI_FMT_SF2PRESET rows, sorted bank/program;
           -1 = not readable */
int SFI_CountSFZ(const char *path, char name[27]);
        /* voices needed (>= 0), name = file stem; -1 = unreadable */
int SFI_Load(const ilibent_t *e, int target, sfi_report_t *rep);
        /* RI_OK, RI_ERR_OPEN (nothing usable / unreadable),
           RI_ERR_SLOTS (no free slot at all). Song changed only on RI_OK.
           Caller holds the engine lock. */
```

## it_ris.h additions

```c
#define RI_FMT_SFZ        0x10
#define RI_FMT_SF2PRESET  0x11
#define RI_FMT_SF2        0x18
static inline int RI_IsLibrary(int f) { return (f & 8) != 0; }
extern sfi_report_t RI_LastReport;   /* filled by RI_LoadInstrument for
                                        the new formats */
```

`RI_IdentifyFile`, `RI_ListDirectory`, `RI_ScanModule`,
`RI_LoadInstrument`, `RI_FormatName`, `RI_KnownExt` accept the new
formats. Format names: "SFZ Instrument", "SoundFont 2 Preset",
"SoundFont 2 Library".

## Batch mode

`ited --sfi-test <file-or-dir> [...]`: recursively every `.sfz` and every
preset of every `.sf2`, each loaded into instrument 1 of an empty song;
one line per item `OK|FAIL <needed>/<loaded>/<skipped> <path>[#preset]
<why>`, a summary line, exit code 0. No window, no audio device, writes
no files.
