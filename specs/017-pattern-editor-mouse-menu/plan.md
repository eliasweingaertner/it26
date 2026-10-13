# Implementation Plan: Pattern Editor Mouse Selection, Context Menu and Scale-Aware Note Interpolation

**Branch**: master (no feature branch) | **Date**: 2026-10-13 | **Spec**: [spec.md](spec.md)

## Summary

Port extensions in the pattern editor: mouse drag marks a block, a right
click opens a block menu (IT's block keys plus Clear, Transpose by... and a
scale-aware Interpolate notes), Ctrl-K runs Interpolate notes. Built inside
the existing editor: no new files, no new dependencies.

## Technical Context

**Language**: C11, as the rest of it26 | **Dependencies**: none new (`math.h` for `sqrt`)
**Testing**: `ITED_SELFTEST` (DLG block for the help lines); port-only harness scripts via `PortMachine` (remote backend, `mouse x y b`)
**Platforms**: Win32, SDL (macOS/Linux), terminal (SGR mouse), remote

## Constitution Check

- 1:1 fidelity: unaffected. All new behaviour hangs off input IT 2.14 ignores (pattern-area mouse, right click, Ctrl-K); verified with the round 3 mouse compare scripts. Documented as fidelity notes in README.
- Extensions are opt-in by input, not by mode; no saved-file differences.

## Design

### Input

- `ITK_MOUSE_RIGHT` (src/it_screen.h) next to `ITK_MOUSE`. Win32 `WM_RBUTTONDOWN`, SDL `SDL_BUTTON_RIGHT`, terminal SGR button 2 press, remote `mouse x y b` bit 1.
- Global key handler: `ITK_MOUSE_RIGHT` on `SCR_PATTERN` → `pe_context_menu()`.
- Pattern editor key handler: `0x0B` (Ctrl-K) → `ctx_interp_notes()`.

### Drag selection (`src/it_editor.c`)

- `pattern_cell_at()` maps pointer → (channel, row); `pattern_click()` sets cursor and anchor (`PeDrag`); the main loop calls `pattern_drag()` while the button is held, writing `BlockMark/Top/Bottom/Left/Right`.

### Context menu

- `PeContextItems[]` (`menuitem_t`), drawn by the existing `run_menu()` (style 3 box, W 35, H 3·N+3, clamped at the pointer, no title).
- Handlers `ctx_copy/paste/semi_up/semi_down/interp_vol/interp_fx` call the IT key functions; `ctx_clear`, `ctx_transpose_n` (+ `transpose_dialog`) and `ctx_interp_notes` are new.
- Undo: `UndoTypeName[26]`, types 23 (clear), 24 (note interpolation), 25 (transposition).

### Interpolate notes

- `InterpScales[]` (name, 12-bit mask), `InterpRoots[]`; session state `InterpScale/Root/Step` (Step reset to `pe_skip()` on each open); `interp_slots(h, step)` = note rows.
- `interp_dialog()` / `interp_draw()`: box 18,20–61,35; fields at rows 23/26/29 with `<` `>`; OK / Cancel; keys Up/Down/Tab, Left/Right; mouse halves of a field step back/forward.
- Per channel: run = top note, in-scale pitches between, bottom note; rows 0, Step, 2·Step, … and the last; run indices spread evenly across the used rows.

### Key detection

- `interp_histograms()`: per-instrument pitch-class histograms over the current pattern (live `Grid`) and every distinct order-list pattern (`Pattern_Unpack` into `OpGrid`), skipping channels with `ChnlPan & 0x80`; notes without an instrument count for the channel's last one.
- `interp_key_of()`: Krumhansl-Schmuckler correlation against major/minor profiles; needs ≥ 8 notes and correlation ≥ 0.5.
- `interp_detect_key()`: top note's instrument → non-drum instruments (drum = ≥ 4 notes, ≥ 80 % on one pitch class) → fail (caller uses top note). Source string `InterpSource` shown next to the root.

### Help

- `help_lines()`: for context 1 (pattern editor) append `PortHelpPeK`, `PortHelpPeMenu`, `PortHelpPeDrag` under `PortHelpHead`, after the dialog lines when those exist.

## Project Structure

```text
specs/017-pattern-editor-mouse-menu/
├── spec.md
├── plan.md
└── tasks.md
src/it_screen.h, it_screen.c, it_screen_win32.c, it_screen_sdl.c, it_screen_remote.c   right click
src/it_editor.c                                                                       everything else
README.md                                                                             fidelity notes
```
