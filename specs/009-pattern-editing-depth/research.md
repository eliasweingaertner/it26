# Research: Pattern Editing Depth (009)

Sources read: `impulse-tracker-jthlim/IT_PE.ASM` (key table 380..860, state
variables 213..345, handler bodies as cited), `PE_TRANS.INC`, `IT_DISK.ASM`
430..436 (`KeyBoardTable`), `IT_F.ASM` 2073..2320 (`F_PostThumbBar`),
`IT_OBJ1.ASM` (`O1_UndoList`, `O1_SelectMultiChannel`). Port: `it_editor.c`
(`handle_pattern_key`, widget framework), `it_pattern.c`, `it_screen.h`.

## R0. Corrections to the spec's assumptions (authoritative)

Three things the spec (and the HANDOFF roadmap note) had wrong, settled by
reading the ASM:

1. **`PE_TRANS.INC` is NOT the block-op include.** It contains the
   XM/MOD/MTM/669/S3M *pattern format converters* — already ported in
   feature 007 (`it_import.c`). All block/edit semantics live in
   `IT_PE.ASM` itself (`PEFunction_*`, lines 3335..12310). Wherever the
   spec says "PE_TRANS.INC semantics", read "the IT_PE.ASM handler
   bodies".
2. **Undo is a 10-entry typed history with a picker UI, not single-step.**
   `UndoBuffer` (IT_PE.ASM 283) holds 10 (segment, type) pairs;
   `PE_AddToUndoBuffer` (11319) releases the oldest, shifts down, stores
   newest — each entry is a full encoded pattern snapshot plus a type code
   0..22 with display strings (300..322). **Ctrl-Backspace opens the undo
   requester** (`PEFunction_Undo` 11461 → `O1_UndoList`, drawn by
   `PEFunction_DrawUndo/PreUndo/PostUndo` 11476..11620): the user picks
   which snapshot to revert to. Reverting itself pushes a "Redo" entry
   (type 21). Types map 1:1 to the operations that snapshot (see R3).
3. **The `,` mask key is not an editor dialog.** `PEFunction_SetMask`
   (3752): `EditMask ^= MaskChange[PatternCursor]` — it toggles the mask
   bit *for the field under the cursor*. The mask state is the `EditMask`
   byte; `MaskChange[]` maps the 9 cursor columns to field bits.

## R1. State model (IT_PE.ASM 213..345) → port equivalents

| ASM variable | Meaning | Port note |
|---|---|---|
| `Row/Channel/PatternCursor` | cursor row / channel / column 0..8 | port has CurRow/CurChan/CurCol **0..3** — the original has **9** cursor columns (note, ins tens/units, vol tens/units, cmd, param hi/lo + shared handlers `PE_PatternCursorPos0..8`); the port must widen its column model or map faithfully |
| `SkipValue` | edit step 0..16 | exists (`EditStep`) |
| `MultiChannelInfo[64]` | per-channel multichannel entry flag | new |
| `BlockMark, BlockLeft/Top/Right/Bottom` | mark rect (channels × rows), normalised | new |
| `BlockAnchorChannel/Row, BlockReset, NoteEntered` | Shift-marking anchor state | new |
| `BlockDataArea` | clipboard segment (0 = empty) | new: heap clipboard |
| `EditMask` + `MaskChange[]` | field write-enable byte | new |
| `Template` | 0 off / 1 overwrite / 2 mix-pattern / 3 mix-clipboard | new |
| `UndoBuffer[10]` + types | undo history | new |
| `PlayMarkPattern/Row/On` | play mark | new |
| `Amplification`, `FastVolumeAmplification` | Alt-J % memory, Ctrl-J fast mode | new |
| `Modified/PatternModified` | dirty tracking (`PE_AddToUndoBuffer` sets both except type 22) | new |
| `LastKeyBoard1..3` | last-keys memory driving double-press behaviours (Alt-D grow, 2×Alt-K/X, double Alt-N dialog, Alt-L widen) | new: port needs a last-key history |

## R2. Marking (decoded 1:1)

- **Shift+movement** (`PEFunction_Press_Shift` 3493, key list entries DB 4):
  Shift-press records anchor (`BlockAnchorChannel/Row`, `BlockReset=1`);
  shifted movement keys route to the same movement handlers which, seeing
  the shift state, extend the mark from the anchor. Release with no note
  entered leaves the mark; if a note was entered during shift, release
  restores the anchored cursor and runs `PE_GotoNextInput` (3510).
- **Alt-B** (`MarkBeginBlock` 5555): no mark → 1×1 mark at cursor; with a
  mark, sets the left/top edge with **normalisation by swap** (if cursor >
  right, old right becomes left).
- **Alt-E** (`MarkEndBlock` 5603): mirror image for right/bottom.
- **Alt-D** (`AltD` 5651): first press marks the cursor channel from the
  current row spanning `RowHiLight2` rows (the *major* row hilight, i.e.
  one "bar"), clamped to `MaxRow`; an immediately repeated Alt-D
  (`LastKeyBoard2` check) doubles the marked length in place.
- **Alt-L** (`AltL` 5994): marks the current track full-height; repeated
  Alt-L widens to all 64 channels (LastKeyBoard2 check at 5999).
- **Alt-U** (`UnMarkBlock` 7093): clears `BlockMark`.
- Rendering: marked cells get the mark colour via `PE_SelectColour` (8856)
  — port maps to its attr pipeline in `draw_pattern`.

## R3. Block operations — the authoritative key map

From the key table (579..860) and the undo-type strings (300..322), which
double as the operation ↔ key ↔ snapshot registry:

| Key | Handler (line) | Semantics (contract) | Undo type |
|---|---|---|---|
| Alt-Q / Alt-A | `SemiUp` 7119 / `SemiDown` 7193 | transpose marked block ±1 semitone, clamp 0..119 (`MAXNOTE`), special notes untouched | 2 / 3 |
| Alt-F / Alt-G | `BlockDouble` 6326 / `BlockHalve` 6240 | spread rows to double length / compress to half (original discards odd rows) | 4 / 5 |
| Alt-J | `VolumeAmp` 7418 | amplify volumes by `Amplification`% (prompt; Ctrl-J `ToggleFastVolume` 11622 skips the prompt) | 6 |
| Alt-K | `AltK` 5805 | volume/pan **slide** across the block (interpolate first→last); **2×Alt-K** = wipe volumes/pannings (recover variant) | 7 / 8 |
| Alt-M | `BlockMix` 7007 (+`SecondBlockMix` 6898) | mix clipboard into pattern; repeated Alt-M switches precedence | 9 |
| Alt-O | `BlockOverWrite` 6707 | paste, clipboard fields overwrite all | 10 |
| Alt-P | `BlockPaste` 6782 | paste (fields per original rules) | 11 |
| Alt-C | `BlockCopy` 6580 | copy mark → clipboard (no undo entry) | — |
| Alt-S | `AltS` 5700 | set sample/instrument of all notes in block to current | 12 |
| Alt-V | `BlockVolume` 8417 | set volume/panning of block (uses `PEGetVolume` 5761 defaults) | 13 |
| Alt-W | `WipeExcessVolumes` 8474 | remove volumes equal to the default (excess) | 14 |
| Alt-X | `SlideCommand` 7267 / `WipeCommands` 7355 | effect-data slide across block; **2×Alt-X** wipes effects+data | 15 / 16 |
| Alt-Y | `BlockSwap` 6430 | swap clipboard and marked region | 17 |
| Alt-Z | `WipeBlock` 6038 | cut/clear the marked block | 18 |
| Alt-Ins / Alt-Del | `RowInsert` 4937 / `RowDelete` 4882 | insert/delete row(s) across **all channels** | 19 / 20 |
| Ctrl-Ins / Ctrl-Del | `RollDown` 6172 / `RollUp` 6104 | roll whole pattern with wraparound | — (verify in body) |
| Ins / Del | `Insert` 4805 / `Delete` 4733 | current-**track** row insert/delete | — |
| Ctrl-Backspace | `Undo` 11461 | opens the `O1_UndoList` requester over the 10-slot history; revert pushes Redo (21) | 21/22 |

Error paths shared by the ops: `NoBlockMarkedMessage` 6663,
`OutOfMemoryMessage` 6676, `NoBlockData` 6694 (status-line flashes).

Implementation transliterates each handler body from the cited lines; this
table is the scope/键 contract, not a substitute for the bodies.

## R4. Note entry pipeline (mask / multichannel / template)

- `PE_NewNote` (4650) is the original entry point: consults `EditMask`
  (which fields to write), `LastKeyBoard*` for repeats, template state,
  and `PE_GotoNextInput` (4087) for skip/multichannel advancement.
- **Mask**: `EditMask ^= MaskChange[PatternCursor]` on `,` (R0.3). The
  port's simplified always-write-ins behaviour is replaced by mask
  consultation; the mask default matches the original's initial value.
- **Multichannel**: Alt-N toggles `MultiChannelInfo[Channel]`; a repeated
  Alt-N (LastKeyBoard2 == 3100h) opens the `O1_SelectMultiChannel` object
  list (checkbox dialog for all 64 channels). `PE_GotoNextInput` advances
  to the next enabled channel after entry; Backspace honours it too
  (3786).
- **Template** (`Template` 0..3, Alt-I `ToggleTemplate` 8660 cycles,
  Shift-; = ':' `TemplateOff` 8682): with a clipboard present, note entry
  stamps via `PE_Template` (4555) → `TemplateOverwrite` 4246 /
  `TemplateMixPattern` 4306 / `TemplateMixClipBoard` 4389 /
  `TemplateNotesOnly` 4472, transposing the clipboard so its first note
  matches the entered note. Status messages `TemplateMsg1..3` (343..345).

## R5. Navigation & conveniences (line registry)

Movement/view: `Ctrl_PgUp/PgDn` 3335/3346 (top/bottom of pattern? bodies
define), `Alt_Home/End` 3531/3600, `ShiftPgUp/PgDn` 3634/3646, `Home` 3658
(cascading: start of field → channel → row 0 quirk), `End` 3684,
`AltLeft/Right` 10393/10376 (channel ±1 keeping column), `ViewLeft/Right`
10321/10348, `AltUp/Down` 10438/10461, `Ctrl_Home/End` 10410/10424.
Patterns: `NextPattern/LastPattern` 8202/8238 (trace-aware),
`Next4/Last4Patterns` 8273/8300, `Next/LastOrderPattern` 8364/8326.
Schemes: `Ctrl0..5` 10115..10168 → `QuickViewSetup` 10169 (n-channel
layouts with dividers), `Ctrl_Shift1..4` 10210..10253 → `PE_FastView`
10254; `ViewTrack` (Alt-T) 8767, `ClearViews` (Alt-R?) 8835, per the key
table entries. Toggles: `ToggleCentralise` (Ctrl-C) 11230 +
`PE_CentraliseCursor` 3583, `ToggleTrace` (Scroll Lock) 11251,
`ToggleTracking` 11189, `ToggleRowHilight` 11209, `ToggleDivision` 10095.
Pattern length: `PE_SetPatternLength` 11692 (+`GetPatternLength` 7553,
`PE_ShowPatternLength` 7707) — thumbbar dialog, resize preserves data per
body. Scratch: `PEFunction_StorePattern` 8077 / `StoreCurrentPattern` 8138
/ `RestoreData` 8610 / `Alt0` 8402 (store/restore via `EncodePattern`
7896/`DecodePattern` 7734). Play aids: `SetPlayMark` (Ctrl-F7) 11095,
`PE_F7` 11128 (play from mark/current), `PE_PlayCurrentPosition` 11075,
`PlayCurrentNote/Row` 8538/8575 (4/8 on numpad per key table). Mute/solo:
`Alt_F9` 10974, `MuteNext/Previous` 10988/10998, `Alt_F10` 11009,
`SoloGotoNext` 11023, `UnmuteAll` 11032. `PickUp` (Enter) 3857 — grabs the
cell's note/ins/vol/effect into the entry state (already partially in the
port; align fully). `ToggleDefaultVolume` 8641.

## R6. Key-layer prerequisites

- New `ITK_*` codes needed (from the key table's modifier+scancode pairs):
  Ctrl-arrows, Ctrl-Home/End, Ctrl/Shift-PgUp/PgDn, Shift-arrows (as
  distinct events or a queryable shift state — the original uses explicit
  press/release handlers for Shift, scan 2Ah/36h, entries at key table
  505..523: the port needs **shift press/release events** or equivalent),
  Alt-letter coverage exists since 005; Alt-Ins/Del exist; Ctrl-Ins/Del,
  Ctrl-Backspace, Scroll Lock, numpad 4/8 distinction, Ctrl-digit and
  Ctrl-Shift-digit combos.
- Win32: WM_KEYDOWN/UP already distinguishes these; SDL2 likewise;
  terminal cannot report several (documented roadmap-#7 leftover).
- **Keyjazz reduction**: the authoritative `KeyBoardTable` (IT_PE.ASM
  254..261 = IT_DISK.ASM copy): Z-row scan codes for 12 semitones +
  Q-row for 17 more — **no** `,` `.` `;` `l` `/`. Port's `key_to_note`
  drops the five extras everywhere (F2, lists, F4 note window all share
  it). `,` frees for SetMask; `.` stays the field-clear key (original
  uses it likewise); `;`/`'` stay instrument cycling (ported, 96a607d).

## R7. Thumbbar numeric entry

`F_PostThumbBar` (IT_F.ASM 2125): digits '0'..'9' (2196..2200) branch to
the numeric-entry path (`F_PostThumbBar30`): an input prompt accumulates
digits, Enter commits clamped to [min,max], ESC cancels. Applies to all
thumbbars via the shared post-handler — the port adds it to its widget
framework's thumbbar key path (benefits F3/F12/F4 pages as in IT 2.17).

## R8. Undo buffer mechanics (contract)

- 10 slots, each: heap snapshot of the **whole pattern** (original:
  `EncodePattern` into an allocated segment) + type code + pattern number
  (type 22 = "Pattern N" caption entry semantics per `DrawUndo`).
- `PE_AddToUndoBuffer(type)` before each mutating op listed in R3;
  releases slot 9, shifts 0..8 → 1..9, stores new at 0; sets
  `Modified/PatternModified` (except type 22).
- Ctrl-Backspace = modal requester listing the slots with the type
  strings; selecting entry N reverts the pattern to that snapshot and
  pushes the pre-revert state as "Redo" (type 21).
- Port representation: `struct { editcell_t *cells; int rows; int
  pattern; uint8_t type; }` ring of 10; snapshots taken from the unpacked
  grid under the engine lock; memory released oldest-first as the
  original does.

## R9. Decisions

1. **Stage the implementation by user story** (P1 marking+block ops →
   P2 mask/template/rows/undo → P3 nav/conveniences → key plumbing first
   where prerequisite). Rationale: each story is independently gate-able
   with the selftest; the full surface in one pass is too big to verify
   honestly.
2. **Cursor column model widens to the original's 9 positions**
   (`PE_PatternCursorPos0..8`) as part of P1 groundwork — several
   handlers (mask per-field, digit entry, End/Home cascade) key off it.
   The port's 4-column model is a lossy simplification the depth work
   cannot sit on.
3. **Shift marking via explicit shift press/release events** in the
   backends (mirroring the original's 2Ah/36h handlers) rather than
   polling — keeps the editor backend-agnostic; terminal backend simply
   never emits them (documented).
4. **Clipboard/undo live on the heap in the unpacked cell model** and
   round-trip through `Pattern_Pack/Unpack` only at pattern-commit
   boundaries, preserving the explicit-mask cell contract (constitution
   IV; the 006-era "mask is authoritative" lesson).
5. **Selftest coverage**: scripted key battery per story (mark → op →
   pack → unpack → assert cells; undo revert byte-equality; mask/template
   entry assertions), plus the standing determinism ×4 and roundtrip
   gates. New `PE OK` selftest block.
6. Config persistence: only what IT.CFG persists (fast-volume %, view
   schemes are runtime; verify against `IT_DISK.ASM` config block during
   implementation) — clipboard/undo/mask are session-local as in the
   original.
