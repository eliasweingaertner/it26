# Research: Pattern Editor Completion (feature 010)

Decoded from `impulse-tracker-jthlim` (canonical 2.17 source). All line
references are into `IT_PE.ASM` unless stated. This document resolves every
NEEDS CLARIFICATION from the plan's Technical Context.

## R1. Pattern-length dialog (`PE_SetPatternLength`, 11692..11757)

**Key**: Ctrl-F2 (`DB 3 / DW 13Ch`, key table 829..831). There is also a
`SHOWPATTERNLENGTH`-gated Right-Ctrl+Enter `PE_ShowPatternLength` (359..363)
— that build switch is OFF in 2.17; do not port it.

**Dialog objects** (`IT_OBJ1.ASM` 659..730, `O1_SetPatternLength`, default
focus object = 4):

- Box (15,19)-(65,33) style 3; header "Set Pattern Length" at (31,21) attr 20h.
- Static text attr 20h at (19,24): "Pattern Length", 2 blank lines,
  " Start Pattern", "   End Pattern".
- Thumb boxes (33,23)-(56,25) and (33,26)-(60,29), style 25.
- Three type-9 thumbbars bound directly to the CS variables:
  - length at (34,24), range **32..200**, step 1 → `PatternSetLength` (init 64)
  - start  at (34,27), range **0..199** → `PatternLengthStart`
  - end    at (34,28), range **0..199** → `PatternLengthEnd`
  - Tab order per next/prev fields: 4→5→6→7.
- OK button (35,30)-(44,32) style 8, text "   OK", **returns 1**; ESC returns 0
  (`ESC&ReturnList`). Note `M_Object1List` is called with CX=4 (initial focus
  = the length thumbbar).

**Semantics** (`PE_SetPatternLength`):
1. On entry: `PatternLengthStart = PatternLengthEnd = PatternNumber`.
   `PatternSetLength` is **not** re-primed from the current pattern (the
   original's re-prime is commented out, 11695..11697) — it persists across
   invocations, initial value 64. Port this quirk.
2. If dialog returns 0 (ESC) → nothing happens.
3. If OK: `PEFunction_StorePattern` (**correction**: this is the pack/commit
   routine — `Music_ReleasePattern` + `EncodePattern`, IT_PE.ASM 8077 — NOT
   an undo push; the original's Ctrl-F2 resize is **not undoable at all**),
   then for every pattern p in [start..end]: load p, decode, set
   `MaxRow = PatternSetLength - 1`, `PatternModified = 1`, and store
   (repack). Finally restore the original `PatternNumber` and re-decode.
   - Rows below the new length survive because decode unpacks into the fixed
     200-row edit grid and the repack simply stops at the new MaxRow; growing
     appends empty rows (grid beyond old length is empty after decode).
   - **Port deviation (documented)**: the port pushes ONE type-22
     ("Pattern data") snapshot of the current pattern before resizing, so
     the current pattern is recoverable via Ctrl-Backspace (precedent:
     feature 006's "Replace?" safety prompt). Patterns other than the
     current one in a range resize remain non-undoable, as in the original.
     Noted in README fidelity notes.
4. No no-op check: OK always rewrites the range. Port as-is.

**Port mapping**: the port's editcell grid + `Pattern_Pack`/`Unpack` replace
DecodePattern/StorePattern; per-pattern row counts already exist
(`pattern length` in the song structs). All mutation under
`Engine_Lock`/`Unlock`. Undo snapshot via the existing feature-009 ring
(`pe_undo_store`) with the standard type code.

## R2. Mute/solo key family (10974..11039)

| Key | Table entry | Handler | Behaviour |
|---|---|---|---|
| Alt-F9 (`2/143h`) and `\` (`1/'\'`… actually `DB 1, DW '\'`) | 797..799, 809..811 | `PEFunction_Alt_F9` | `Music_ToggleChannel(Channel)` |
| keypad `/` (`0/135h`) | 801..803 | `PEFunction_MuteNext` | Alt_F9 then `PEFunction_Tab` (advance to next channel) |
| `?` (`1/'?'`) | 805..807 | `PEFunction_MutePrevious` | `Channel -= 1` with **AdC 0 clamp at 0** (`Sub Channel,1; AdC Channel,0`), then Alt_F9 (no Tab) |
| Alt-F10 (`2/144h`) | 821..823 | `PEFunction_Alt_F10` | `Music_SoloChannel(Channel)` |
| `\|` (`1/'\|'`) | 813..815 | `PEFunction_SoloGotoNext` | Alt_F10 then `PEFunction_Tab` |
| Alt-`\` (`2/12Bh`) | 817..819 | `PEFunction_UnmuteAll` | `Music_UnmuteAll` |

All return AX=1 (redraw). `Music_ToggleChannel` / `Music_SoloChannel` /
`Music_UnmuteAll` already exist in the port (`it_music.c`, used by F5/F11) —
**no engine change**. The F2 header already renders muted channels attr 10h.
`PEFunction_Tab` is the existing next-channel move (ported in 009).

## R3. View schemes

### Data model (855..925)

- `ViewChannels` (890): 100 words, each `low byte = channel`, `high byte =
  view method`, terminated by `0FFFFh` entries — an **ordered list of visible
  channels**, not an indexed map. Default: all `0FFFFh` = "default view"
  (every channel, method 0, handled by the drawer's fallback).
- `ViewMethodInfo` (910..924): 5 methods, widths **13, 10, 7, 3, 2**
  (ViewFull, ViewCompress, ViewAllSmall, ViewNote, ViewTiny).
- `CursorPositions` (855..860): 5 rows × 9 bytes — per-method per-cursor-column
  data. Rows 0..2 = x-offsets for methods 0..2
  (`0,2,4,5,7,8,10,11,12` / `0,2,3,4,5,6,7,8,9` / `0,2,3,3,4,4,5,6,6`);
  rows 3..4 (`20h,2,1,2,1,2,0,1,2` / `10h,1,0,1,0,1,0,1,1`) are the
  ViewNote/ViewTiny rows consumed by `PE_HilightView` (BP=+27/+36) — decode
  exactly from `PE_HilightView` during implementation.
- Header captions by width (863..869): 13→`" Channel xx "` (ChannelMsg),
  10→`"Channel xx"`, 7(AllSmall)→`"Chnl xx"` (ChannelMsg7), 3→`" xx"`
  (ChannelMsg4), 2→`"xx"` (ChannelMsg5). (`ChannelMsg3/6` are used by other
  widths in `PE_DrawPatternEdit`'s width switch at ~2287 — decode there.)
- `ViewDivision` (888, default 1): draw the char-168 divider column between
  tracks. `ViewWidth` (889), `NumChannelsEdit` (880), `StartChannelEdit` (873).

### Handlers

- `PE_CheckWidth` (8711..8763): walks `ViewChannels`, sums method widths
  (+2 border, +count-1 dividers if `ViewDivision`); **fails (CF=1) if total
  ≥ 76** — callers revert on failure. On success sets `ViewWidth` and
  `NumChannelsEdit = (74 - ViewWidth)/14` remaining default-view channels
  (0 if ViewWidth ≥ 74).
- `PEFunction_Ctrl0..5` (10115..10165) → `PE_FastView(AX=0..5)`
  (10254..10315): AX-1 = method; **Ctrl-0 (method FFh) deletes the current
  channel's entry** (compacting the list); Ctrl-1..5 set/append the current
  channel's method (append at terminator if absent), reverting on
  `PE_CheckWidth` failure.
- `PEFunction_Ctrl_Shift1..4` (10210..10250) → `PEFunction_QuickViewSetup`:
  presets fill `ViewChannels` with channels 0..N-1 all using one method:
  - Ctrl-Shift-1: method **1**, 6 channels with division (7 without)
  - Ctrl-Shift-2: method **2**, 9 with division (10 without)
  - Ctrl-Shift-3: method **3**, 18 with division (24 without)
  - Ctrl-Shift-4: method **4**, 24 with division (36 without)
  Rest of table = FFFFh. Then `PE_CheckWidth`, and **enables tracking if
  off** (calls `PEFunction_ToggleTracking`).
- `Alt-T` (678) → `PEFunction_ViewTrack` (8767..): cycle the current
  channel's method +1 (wrap **after method 4** back to "remove entry" — the
  removal path at ViewTrack8 compacts like Ctrl-0).
- `Alt-R` (694) → `PEFunction_ClearViews`: reset table (decode at
  implementation; clears to all-FFFFh + CheckWidth).
- `Alt-H` (690) → `PEFunction_ToggleDivision` (10095): XOR ViewDivision,
  revert if CheckWidth fails.

### Renderers (9035..10091)

`PE_DrawPatternEdit` (2100..) iterates visible channels: entries from
`ViewChannels` first, then `NumChannelsEdit` default full-width channels.
Per method it calls `ViewFull` (9074), `ViewCompress` (9209), `ViewAllSmall`
(9342), `ViewNote` (9479), `ViewTiny` (9818), sharing `ViewCommon` (9035).
Cell formats per method (decode each proc during implementation):

- ViewFull: the existing 13-col `nnn ii vv exx` (already ported).
- ViewCompress: 10 cols `nnn ii vve` — no param, volume packed.
- ViewAllSmall: 7 cols — packed digit pairs (font-B small numbers, as F5).
- ViewNote: 3 cols — note only.
- ViewTiny: 2 cols — font-B packed note.

The F5 multi-channel views already ported the font-B packed-digit machinery
(`S_DefineSmallNumbers` glyphs) — reuse it.

### Toggles (11189..11268)

| Key | Handler | State | Messages |
|---|---|---|---|
| Ctrl-T | `PEFunction_ToggleTracking` (11189) | `ViewChannelTracking ^= 1` | "View-Channel cursor tracking enabled/disabled" |
| Ctrl-H | `PEFunction_ToggleRowHilight` (11209) | `CentraliseCursor ^= 2` (bit 1) | "Row hilight enabled/disabled" |
| Ctrl-C (already ported 009) | `PEFunction_ToggleCentralise` (11230) | `CentraliseCursor ^= 1` (bit 0) | "Centralise cursor enabled/disabled" |
| Alt-H | `PEFunction_ToggleDivision` (10095) | `ViewDivision ^= 1` | (none — silent, reverts if too wide) |
| Alt-R | `PEFunction_ClearViews` | reset ViewChannels | (decode) |

`ViewChannelTracking` (891, default 0): when a view table is active, the
cursor's channel column follows/locks per the tracking rules in the cursor
drawing code (`PE_HilightView` / cursor-geometry walk) — decode with the
renderer. `CentraliseCursor` default 0E8h (bit 1 row-hilight ON, bit 0
centralise OFF; upper bits are MIDI-record flags, out of scope).
Persistence: these live in the `Pattern` segment saved into IT.CFG by the
original's config writer; the port persists them in `ited.cfg` alongside the
existing prefs.

## R4. Decisions

- **D1**: The original's resize is not undoable (see R1 correction); the
  port adds ONE type-22 snapshot of the current pattern as a documented
  safety deviation. Range patterns other than the current stay
  non-undoable, as in the original.
- **D2**: `PatternSetLength` persists across dialog invocations (original's
  re-prime is commented out) — port the quirk.
- **D3**: MIDI trigger keys (841..851) and `PE_CycleMIDIPlayTrigger` /
  `PE_ToggleDefaultVolume` remain out of scope (no MIDI-in; default-volume
  toggle is a separate small item, not in this feature).
- **D4**: Cursor geometry becomes table-driven from `CursorPositions` +
  `ViewMethodInfo` widths for all 5 methods; block ops/entry stay
  cell-model-based (display-only change), matching FR-005.
- **D5**: Reuse the F5 font-B packed-digit renderer for methods 2/4.
- **D6**: Engine untouched; all state editor-side in `it_editor.c`.
