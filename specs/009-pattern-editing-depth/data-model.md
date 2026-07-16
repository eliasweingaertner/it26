# Data Model: Pattern Editing Depth (009)

All state is session-local editor state in `it_editor.c`, mirroring the
IT_PE.ASM variables (research.md R1). No on-disk format changes.

## Cursor (widened)

| Field | Values | Original |
|---|---|---|
| `CurRow`, `CurChan` | as today | `Row`, `Channel` |
| `CurCol` | **0..8** (note, ins-tens, ins-units, vol-tens, vol-units, cmd, p-hi, p-lo — per `PE_PatternCursorPos0..8`) | `PatternCursor` |

The 4-column model is replaced; drawing, mouse hit-testing, Left/Right
movement and digit entry re-key off the 9-position model.

## Mark

| Field | Meaning |
|---|---|
| `BlockMark` | 0 = none, 1 = marked |
| `BlockLeft/Top/Right/Bottom` | normalised rect (channels × rows) |
| `BlockAnchorChan/Row` | shift-marking anchor |
| `BlockReset`, `NoteEntered` | shift-session state |

Invariants: rect always normalised (Alt-B/E swap semantics, R2); clamped
to pattern on use, marks referencing rows beyond a shrunken pattern clamp
exactly as the original's ops do.

## Clipboard

`editcell_t` grid + `int chans, rows`; NULL = empty (original
`BlockDataArea` 0). Persists across pattern switches; replaced by Alt-C /
Alt-Y.

## Edit mask / entry state

| Field | Meaning |
|---|---|
| `EditMask` | field write-enable byte; `,` XORs `MaskChange[CurCol]` |
| `MultiChannelInfo[64]` | per-channel entry flags (Alt-N; 2×Alt-N dialog) |
| `Template` | 0 off / 1 overwrite / 2 mix-pattern / 3 mix-clipboard |
| `LastKeys[3]` | last keystrokes (double-press behaviours: Alt-D/K/X/N/L) |

## Undo ring (10 slots, original bound)

```c
typedef struct undoslot_t {
    editcell_t *cells;      /* full-pattern snapshot (unpacked model) */
    uint16_t    rows;
    uint16_t    pattern;    /* pattern number captured */
    uint8_t     type;       /* 0..22, indexes the original type strings */
} undoslot_t;
```

Push = drop slot 9, shift down, store at 0 (`PE_AddToUndoBuffer` 11319);
sets `Modified`/`PatternModified` except type 22. Revert pushes the
pre-revert state as type 21 ("Redo"). Ctrl-Backspace opens the
`O1_UndoList`-derived requester listing slots by type string.

## Scratch, play mark, toggles

| Field | Meaning |
|---|---|
| `PatternScratch` (cells+rows) | Alt-0 store/restore snapshot, distinct from undo |
| `PlayMarkPattern/Row/On` | Ctrl-F7 play mark; F7 plays from mark else cursor |
| `Amplification` (default 100), `FastVolume` | Alt-J prompt memory, Ctrl-J mode |
| view scheme state | per `QuickViewSetup`/`PE_FastView` (channel widths, dividers, track view) |
| `Centralise`, `Trace`, `Tracking`, `RowHilightOn`, `DivisionsOn`, `DefaultVolumeOn` | F2 toggles |

## Key layer additions (`it_screen.h`)

New `ITK_*` codes: Ctrl-arrows, Ctrl-Home/End, Ctrl/Shift-PgUp/PgDn,
Ctrl-Ins/Del, Ctrl-Backspace, Scroll Lock, Ctrl-digit / Ctrl-Shift-digit,
numpad 4/8 (distinct from main-row), and **shift press/release events**
(`ITK_SHIFT_DOWN`/`ITK_SHIFT_UP`, mirroring scan 2Ah/36h handlers).
Shift-arrows arrive as existing arrows + shift-session state. Terminal
backend: combos it cannot report are documented (roadmap #7).

## Keyjazz map (reduced)

`key_to_note` = exactly the original `KeyBoardTable` (IT_PE.ASM 254):
Z-row 12 semitones + Q-row 17. The `,` `.` `;` `l` `/` extension is
removed in every context (F2, lists, F4 note window).
