# Implementation Plan: Audio Rendering Extensions (after Esa Ruoho's IT 2.354)

**Branch**: master (no feature branch) | **Date**: 2026-10-18 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/018-audio-render-extensions/spec.md`

## Summary

Bring Esa Ruoho's rendering set to it26: pattern → sample (Ctrl-O),
pattern → WAV in a Quicksave folder (Shift-Ctrl-O and the F11/F5 gestures),
sample export, per-channel stems (Shift-Alt-M), whole-song WAV / MWAV from
F10, the Quicksave folder with Alt-W save, and command-line batch renders.

Approach: a small render core (`src/it_render.c`) drives the **unchanged**
engine and WAV driver offline, exactly as `itplay -w` does — `Music_Play*`
with `StopEndOfPlaySection`, then `WAVDriver_Render` until `PlayMode` drops
to 0 — and writes 16-bit PCM. The editor wraps it: it silences the live
audio callback for the duration, saves and restores the engine's play state
and the driver settings, applies a solo for stems, resumes playback, and
turns the result into a file or a sample. Keys, file names and status texts
follow Esa's assembly ([research.md](research.md) R1–R12).

## Technical Context

**Language/Version**: C11 (MSVC, GCC, Clang), as the rest of it26

**Primary Dependencies**: the ported engine (`it_music.c`, `it_driver.c` WAV driver), miniaudio (live device only, unchanged)

**Storage**: WAV files (32-bit float, unclipped, Shift-F5 mixing rate, 1 or 2 channels); imports as 16-bit / 44.1 kHz samples; `ited.cfg` gains `quicksavedir=`

**Testing**: `ITED_SELFTEST` gains an **RND** block (naming, render length and determinism, import fields and slot rule, solo sweep, sample export bytes, Quicksave fallbacks); `tests/test_pattern.c` must stay `IDENTICAL` (engine untouched); port-only harness scripts (remote backend) for the gestures; the existing IT 2.14 comparison scripts must not change (no IT 2.14 counterpart for the new keys)

**Target Platform**: Windows, Linux, macOS (Win32, SDL, terminal and remote backends); batch options need no display

**Project Type**: desktop application (tracker editor `ited`)

**Performance Goals**: faster than real time — measured whole-song renders on the reference machine 16×–100× (research R3); a 64-row pattern import well under 2 s

**Constraints**: engine and driver code unchanged (constitution I/III); live audio must not touch the driver while a render runs; deterministic output (same render twice = same bytes); the 1 MB import cap; 8.3 names

**Scale/Scope**: up to 200 patterns, 99 samples, 64 channels per render job; whole-song renders capped at 30 minutes of audio (as `itplay -w`), Esc aborts

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Engine fidelity (I) + Determinism (III)**: PASS with one justified addition. No change to `it_music.c`, `it_effects.c`, `it_tables.c`, `it_load.c`, `it_structs.h` or `it_pattern.c`. `it_driver.c` gains one more value for its it26 output-format extension (`OutFormat` 5 = float without clamp, for WAV-file renders), next to the existing 24-bit / float / 8-bit extension formats; the original 16-bit path (`OutFormat` 0) and the mixer are untouched. The render core only calls existing entry points (`Music_PlayPattern`, `Music_PlaySong`, `Music_Stop`, `Music_InitStereo`, `Music_InitTempo`, `Music_ResetRNG`, `WAVDriver_*`, `Driver->InitSound`) the way `src/main.c` does. `tests/test_pattern.c` is re-run anyway; renders are deterministic (driver reset + RNG reset at the start of each job).
- **Authentic data (II)**: PASS with a note. The new F10 buttons and the F12 Quicksave row take their coordinates from Esa's `IT_OBJ1.ASM` (WAV 69,24–77,26; MWAV 69,27–77,29; Quicksave input 13,45; directory box 12,41–78,46; "Save all Preferences" moved to 27,47–52,49). These are a fork's tables, not IT 2.14's; README lists them as extension layout.
- **One real engine (IV)**: PASS. Renders run the real engine and mixer; no second mixer. Imported samples go through the real `sample_t` and `Music_AssignSampleToInstrument`.
- **Portability (V)**: PASS. Plain C11 + stdio; the live-audio gate lives in the editor's audio callback (no backend changes); miniaudio untouched.

**Post-design re-check (after Phase 1)**: PASS — the design adds `src/it_render.c` (editor-side, `EDITOR_SOURCES`), extends `it_editor.c`, and adds one output-format value to the it26 extension block of `it_driver.c` (original path untouched; `test_pattern` re-run to prove it).

## Project Structure

### Documentation (this feature)

```text
specs/018-audio-render-extensions/
├── spec.md
├── plan.md              # this file
├── research.md          # Phase 0: Esa's behaviour and the it26 mechanics (R1–R12)
├── data-model.md        # render job, Quicksave folder, counter, names
├── quickstart.md        # end-to-end validation
├── contracts/
│   ├── keys-and-screens.md   # every gesture, screen, precedence and message
│   └── cli.md                # ited batch options
├── checklists/requirements.md
└── tasks.md             # /speckit-tasks
```

### Source Code (repository root)

```text
src/
├── it_render.h          # new: render job, sink, naming, WAV writer API
├── it_render.c          # new: offline render core (engine + WAV driver), WAV writer,
│                        #      Esa's naming rules (LL<HHMMSS>, <PFX><NNNN>, HHMMSSnn, SONGnn)
├── it_editor.c          # gestures (F2/F5/F11/F3/F10/F12, Alt-W), live-audio gate,
│                        # play-state save/resume, import into a sample slot, sample export,
│                        # Shift-Alt-M sweep, F10 WAV/MWAV, Quicksave pref + picker,
│                        # help lines, ITED_SELFTEST RND block, batch CLI options
└── (engine files unchanged)
src/it_driver.c          # one more it26 output format: float without clamp (OutFormat 5)
CMakeLists.txt           # it_render.c in EDITOR_SOURCES
README.md, CHANGELOG.md  # extensions credited to Esa Ruoho
tests/compare/           # unchanged IT 2.14 scripts (regression only)
```

**Structure Decision**: one new editor-side module for everything that is
not UI (render loop, WAV writing, names), so the selftest and the batch CLI
can call it without the screen; all key handling and screen work stays in
`it_editor.c` next to the code it extends.

## Design

### Render core (`it_render.c`)

- `render_job_t`: kind (pattern / song), pattern number and rows, solo
  channel (or −1), abort callback, frame cap.
- `Render_Run(job, sink)`: `Music_Stop`; `Driver->InitSound()`;
  import jobs: `WAVDriver_SetMixSpeed(44100)` and 16-bit output; file jobs:
  the Shift-F5 rate and the unclamped float output (`OutFormat` 5);
  Shift-F5 extras (SB16 filter/feedback, mono-force) off, start ramp on;
  `OutFormat` 5 is internal to renders: Shift-F5 does not offer it (a device
  can't play above full scale), and the live format is restored after the job; `Music_ResetRNG`;
  `Music_InitStereo` / `Music_InitTempo`; `StopEndOfPlaySection = 1`;
  `Music_PlayPattern(pat, rows, 0)` or `Music_PlaySong(0)`; then
  `WAVDriver_Render` in 4096-frame chunks into the sink until `PlayMode == 0`
  (the chunk in which it stops is cut at the last mixed frame), the abort
  callback fires, or the cap is reached. Mono songs: left channel only.
- Sinks: file (WAV writer, 32-bit float, fmt tag 3, correct RIFF sizes,
  1 or 2 channels) or memory (16-bit, grows, for the import).
- Names: `Render_NameTimestamp` ("LL"+HHMMSS), `Render_NameCounter`
  (PFX + per-session 4-digit counter), `Render_NameSampleExport`
  (HHMMSS + 2-digit slot), `Render_NameUser` / `Render_NameChannel`
  (F10 typed name, 8.3 upper-case, MWAV base cut to 6 + 2-digit channel).

### Editor glue (`it_editor.c`)

- **Live-audio gate**: `audio_cb` takes a small `AudioMutex`; while
  `RenderActive` is set it writes silence and does not touch the driver.
  `render_begin()` sets the flag under that mutex (so the callback is never
  mid-tick), snapshots the play state (PlayMode, order, row, pattern, rows)
  and the Shift-F5 driver settings; `render_end()` restores the settings,
  `Driver->InitSound()`, `Music_InitStereo` / `Music_InitTempo`, clears
  the flag, and resumes (single-pattern renders only, research R5).
- **Render folder**: Quicksave → `DirSample` → current folder; validated
  before anything is stopped.
- **Import**: memory sink → `sample_t` in slot max(highest sample, highest
  instrument)+1 (research R6), 1 MB cap, host instrument in instrument mode,
  F3/F4 cursors, status text.
- **Export**: `RIS_SaveWAV` per slot into the render folder (it already
  converts 8-bit to unsigned); the selected-sample variant writes from a
  header copy with 44100 Hz (research R8). Playback continues — it only reads
  sample memory.
- **Stems**: Shift-Alt-M and F10 MWAV run one job per channel with
  `Song.Header.ChnlPan` mute bits set for every other channel (saved and
  restored); a progress line and Esc between and inside channels.
- **Gestures**: per [contracts/keys-and-screens.md](contracts/keys-and-screens.md); `CurKey.flags` tells Shift / Ctrl apart where one code serves several (Ctrl-O vs Shift-Ctrl-O, Ctrl-Right vs Ctrl-Shift-Right).
- **F10**: two more format buttons (SaveFormat 4 = WAV, 5 = MWAV), objects
  21/22 in `req_link` (IT215 Down → 21, WAV Up/Down 20/22, MWAV Up/Down
  21/15); the save path and Ctrl-S / Alt-W render when they are selected,
  as Esa's `D_SaveModule` / `D_SaveSong` do.
- **F12**: Quicksave input row (13,45), box to row 46, button moved to rows
  47–49, Enter = folder picker (the feature 016 dialog where available, the
  typed path otherwise), `quicksavedir=` in `ited.cfg`.
- **Batch**: `ited --render-patterns <module> [--pattern N]` and
  `ited --dump-samples <module>` run after the module and the engine are
  set up and before any window opens ([contracts/cli.md](contracts/cli.md)).

## Complexity Tracking

| Item | Why needed | Simpler alternative rejected because |
|---|---|---|
| Live-audio gate (second mutex + flag) | the render and the live callback would otherwise mix through one driver state at once | stopping/restarting the audio device costs ~100 ms per render and can fail on some devices; holding the engine mutex for the whole render blocks the callback for seconds |
| Per-channel passes for stems | the mixer produces one mix at a time | a per-channel mixer output would change `it_driver.c` (constitution I) |
