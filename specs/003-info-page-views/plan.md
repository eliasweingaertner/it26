# Implementation Plan: Info Page (F5) — View Methods, Split Windows, Solo

**Branch**: `003-info-page-views` | **Date**: 2026-07-02 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/003-info-page-views/spec.md`

## Summary

Port the rest of `IT_DISPL.ASM`'s info page. The original F5 is natively a
**multi-window page**: up to 5 stacked view windows (`DisplayWindows`,
default 3 — track view / Variables / 24-channel), each with its own view
method out of the 11 in `DisplayDataModes` (HostChannel track view,
5/8/10/18/24/36/64-channel pattern views, Variables, NoteDots, Details).
`DrawDisplayData` walks the windows; `PostDisplayData` +
`DisplayListKeys` implement the key model (channel select, method
PgUp/PgDn cycling, window Tab/Shift-Tab, Ins/Del split/merge, Alt-Up/Down
resize, Ctrl-F fullscreen, Alt-F9/'Q' toggle channel, Alt-F10/'S' solo,
'V' velocity/volume bars, 'I' instrument/sample names, Alt-S stereo).

**Spec correction (research R1)**: IT 2.17 draws **no waveform
oscilloscope** on F5. The track view's signal display is the *velocity
bar* — a min/max scan of the actual sample data over the span mixed since
the last frame, scaled by final volume (`Display_HostChannel`; 'V'
toggles plain volume bars). `Music_GetOutputWaveform` is a dead extern in
the original, and `Display_SampleDots` is commented out of the 2.17 mode
table. US1 therefore delivers the authentic velocity bars; sample dots
are excluded (deviation from the drafted spec recorded in the README
fidelity notes; NoteDots + the 'I' name toggle cover the intent).

Solo (US3) needs `Music_SoloChannel` (and Alt-R `Music_ToggleReverse`)
ported 1:1 into the engine from `IT_MUSIC.ASM` — a small addition gated
by the determinism regression.

## Technical Context

**Language/Version**: C11.

**Primary Dependencies**: None new. Extends `it_editor.c`'s F5 screen;
uses the existing engine channel tables (`HChn`/`SChn`) under
`Engine_Lock`; adds `Music_SoloChannel`/`Music_ToggleReverse` to
`it_music.c` (1:1 from IT_MUSIC.ASM).

**Storage**: N/A (window layout is session state, as in IT).

**Testing**: determinism gate ×4; `ITED_SELFTEST` extended to cycle
methods/windows; `ITED_SHOT` static captures (live views need playback —
verified interactively from a window as with the current track view).

**Target Platform**: all three backends (cell-buffer drawing only).

**Performance Goals**: full redraw each frame as today; the velocity-bar
sample scan is bounded by bytes mixed per frame per channel.

**Constraints**: layout/behaviour from `IT_DISPL.ASM` only; engine reads
under `Engine_Lock`; the two engine additions are transliterations, not
new behaviour, and the no-solo path is bit-identical.

**Scale/Scope**: `it_editor.c` +~900 lines (window table, 11 methods,
key model); `it_music.c/h` +~60 lines (`Music_SoloChannel`,
`Music_ToggleReverse`).

## Constitution Check

- **Engine fidelity (I) + Determinism (III)** — ✅ PASS. The only engine
  edits are 1:1 ports of `Music_SoloChannel`/`Music_ToggleReverse`
  (mute-table manipulation, no mixing-path change); gate re-run ×4.
- **Authentic data (II)** — ✅ PASS. All geometry/colours/behaviour from
  `IT_DISPL.ASM` (boxes drawn by each method, `GetChannelColour`,
  `DisplayWindows` defaults, `DisplayDataModes` order). The drafted
  spec's "oscilloscope"/"sample dots" wishes are corrected to what 2.17
  actually shipped — inventing a scope would violate this principle.
- **One real engine (IV)** — ✅ PASS. Views read `HChn`/`SChn`/sample
  memory; solo goes through the engine's mute machinery.
- **Portability (V)** — ✅ PASS. Cell-buffer drawing only.

**Result**: All gates pass. Complexity Tracking empty.

## Project Structure

### Documentation (this feature)

```text
specs/003-info-page-views/
├── plan.md              # This file
├── research.md          # Phase 0 (IT_DISPL decode; oscilloscope correction)
├── data-model.md        # Phase 1 (window table, method state, key map)
├── quickstart.md        # Phase 1 (verify methods/windows/solo)
├── contracts/
│   └── info-page.md     # Phase 1 (per-method layout + key contract)
├── checklists/requirements.md
└── tasks.md             # Phase 2
```

### Source Code (repository root: `it26/`)

```text
it26/
└── src/
    ├── it_editor.c   # EDIT: replace the first-pass draw_info with the
    │                 #   DisplayWindows engine (11 methods + key model)
    ├── it_music.c    # ADD: Music_SoloChannel, Music_ToggleReverse (1:1)
    ├── it_music.h    # ADD: prototypes
    └── it_screen.c   # UNCHANGED (font bank B already available)
```

**Structure Decision**: single project; F5 code stays in `it_editor.c`
next to the other screens.

## Complexity Tracking

> No constitution violations. Section intentionally empty.
