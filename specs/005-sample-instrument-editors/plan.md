# Implementation Plan: In-Depth Sample & Instrument Editors

**Branch**: `005-sample-instrument-editors` | **Date**: 2026-07-02 | **Spec**: `specs/005-sample-instrument-editors/spec.md`

## Summary

Port IT_I.ASM's sample-editing surface 1:1: the F3 waveform view
(`I_DrawWaveForm`, 176×32 canvas → font-B chars 1..88 in box
(54,25)-(77,30)), the editable loop/sustain/C5-speed/filename fields
with `I_CheckLoopValues` clamping, and the complete Alt-key operation
set (convert/invert/centre/amplify/reverse/resize/quality/cut×2/
insert/remove/swap/exchange/replace/scale/speed×4). Instrument-side:
slot ops + note-table Alt ops + envelope presets (digit load /
Alt-digit save). Prerequisite: an Alt-modifier key layer (Win32
`WM_SYSKEYDOWN`), which also fulfils part of HANDOFF roadmap #5.

Spec deviations (research R1): IT 2.17 has no freehand draw, no
selection cut/copy/paste, no zoom, and Alt-Y (C5 calc) is a stub —
US1/US2 map to the authentic op set; deviations documented in README.

## Technical Context

**Language/Version**: C11, existing toolchain.

**Primary Dependencies**: none new; all in `it_editor.c` +
`it_screen*` key layer.

**Storage**: n/a (sample memory ops via malloc/realloc replacing the
original's DOS/EMS juggling — `Music_AllocateSample`/`ReleaseSample`
equivalents; max sample size 4177920 bytes as the original).

**Testing**: determinism gate ×4 + `--roundtrip` ×4 unchanged;
selftest extended with waveform + op checks (`F3 OK`).

**Target Platform**: Windows/Linux/macOS; Alt keys Win32-first.

**Project Type**: existing single-project port.

**Performance Goals**: ops on 4M-sample data complete instantly
(single linear passes).

**Constraints**: ops stop playback first (`Music_Stop`) as the
original does; sample memory changes under `ed_lock`.

**Scale/Scope**: ~900 lines in it_editor.c, ~40 in the key layer.

## Constitution Check

- **Engine fidelity (I) / Determinism (III)**: engine files untouched
  (loop re-fetch uses the existing `GetLoopInformation` per active
  slave under lock). Gates re-run after implementation.
- **Authentic data (II)**: waveform canvas geometry, box coords, dash
  patterns, row mapping from IT_I.ASM/IT_OBJ1.ASM (research R2); op
  arithmetic transliterated (R3), including the exact semitone
  multipliers 255392045/4053909306 and amplify 16.16 scaling.
- **One real engine (IV)**: ops edit `Song.Smp[].Data` directly (the
  memory the mixer reads); pattern instrument-byte remaps go through
  `Pattern_Unpack`/`Pattern_Pack` (the exact codec).
- **Portability (V)**: Alt keys via the backend; the terminal backend
  simply doesn't produce them (documented limitation).

## Project Structure

```text
specs/005-sample-instrument-editors/
├── plan.md, research.md, data-model.md, quickstart.md
├── contracts/sample-editor.md
└── tasks.md

ittrack/src/
├── it_screen.h / it_screen_win32.c   # ITK_ALT_* key codes
├── it_editor.c                       # waveform view, fields, ops
ittrack/docs/HANDOFF.md, ittrack/README.md
```

## Complexity Tracking

No violations. Deviations in research R1 → README fidelity notes.
