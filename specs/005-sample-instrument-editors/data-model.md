# Phase 1 Data Model: Sample & Instrument Editors

## Key layer

`ITK_ALT_A..ITK_ALT_Z` (contiguous), `ITK_ALT_0..9`, `ITK_ALT_INS/DEL`,
`ITK_ALT_UP/DOWN`, `ITK_ALT_PLUS/MINUS`, `ITK_CTRL_PLUS/MINUS` in
`it_screen.h`; produced by the Win32 backend from WM_SYSKEYDOWN /
GetKeyState.

## it_editor.c state

| State | Original | Notes |
|---|---|---|
| `SmpAmp` (int) | SampleAmplification | amplify prompt value |
| `SmpNewSize` (uint32) | NewSampleSize | resize prompt value |
| modal `prompt_number()` | GetNumberInput / O1_*List | numeric modal: title, default, max digits; returns 0 = cancelled |
| modal `confirm_box()` | O1_Confirm*List | Yes/No (reuses confirm_overwrite machinery, parameterised text) |
| `quality_dialog()` | O1_ConfirmConvert2List | 3-way: Convert data / Adjust fields / Cancel |
| waveform canvas | S_GetGenerationTableOffset area | `static uint8_t WavePix[176*32]`, regenerated per frame in draw_samples via `Screen_GenerateCharacters(1,22,4,...)` |

## Sample memory helpers (Music_AllocateSample equivalents)

- `smp_realloc(sample_t *s, uint32_t bytes)`: malloc/realloc `Data`
  (+4 frames interpolator padding as the loader does), cap 4177920.
- All destructive ops: `stop_song(); ed_lock(); ...edit...;
  ed_unlock();` then waveform regenerated on next frame.

## Invariants

- Loop clamps (I_CheckLoopValues): Beg ≤ max(len−1,0), End ≤ len,
  End ≤ Beg ⇒ flag cleared; sustain likewise; applied after every
  field edit and length-changing op.
- Reverse mirrors loops (Beg' = len−End, End' = len−Beg).
- Resize scales Length + all 4 loop points by new/old (cap 9999999).
- Slot insert/remove/swap/replace fix instrument NoteSampleTables
  (instrument mode) or pattern instrument bytes (sample mode) exactly
  per R3/R7; pattern remap via unpack→adjust→pack for every pattern
  incl. the current edit grid.
- Determinism/roundtrip gates unchanged (no engine edits).
