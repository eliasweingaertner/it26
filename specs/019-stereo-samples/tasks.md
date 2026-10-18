---

description: "Task list for feature 019: Stereo Samples"
---

# Tasks: Stereo Samples

**Input**: Design documents from `specs/019-stereo-samples/`

**Prerequisites**: plan.md, spec.md, research.md (R1–R10), data-model.md, contracts/file-formats.md, contracts/keys-and-ui.md, quickstart.md

**Tests**: The spec's success criteria require automated checks: `test_pattern` (mono path bit-identical, SC-001) and a new **STR** block in the `ITED_SELFTEST` harness (`src/it_editor.c`, run by `build-compare/_work/selftest.ps1`). Reference file: `testdata/openmpt-testpattern-stereo.it` (OpenMPT 1.32; samples 1 = 16-bit stereo loop, L/R uncorrelated; 2 = 16-bit stereo; 3 = 8-bit stereo ping-pong).

**Organization**: by user story (US1–US5). Paths relative to the repository root.

## Format: `[ID] [P?] [Story] Description`

## Reference facts (verified while planning)

- Sample flags: 0x01 present, 0x02 16-bit, **0x04 stereo**, 0x08 compressed, 0x10 loop, 0x20 sustain loop, 0x40 ping-pong, 0x80 ping-pong sustain. `sample_t` / `slavechn_t` layouts are pinned — no new fields.
- Loader: `Load_SampleData` (src/it_load.c 270) — stereo today only for standalone files (`Cvt & 32`), compacted to one channel via `Load_StereoChoice` (returns 64 left / 192 right); `s->Flags &= ~0x0C` (~399) drops stereo + compressed; interpolator padding written after the data (~402); `DecompressIT8/16` (120/184). Called from it_ris.c 894 and 1595 (library / ITS / ITI / WAV) and the module loader.
- Saver: `save_sample_raw` (src/it_save.c 287), `save_sample_compressed` (295), chosen at ~597; S3M warnings via `s3m_warn(row, msg)` (635).
- Mixer: `GetWaveform` (src/it_driver.c ~230), `MixInner` (~288) using `FilterValues[sc - SChn]`; voice loop in `M32MixHandler` (692) gets `SAMPLEHDR(sc->SmpOffs)` at ~897; `FilterValues` reset/filter setup ~746, copied for NNA at ~933; `ResetFilters` (178).
- Editor sample ops (src/it_editor.c): `smp_alloc` 2340, `smp_free_data` 2351, `smp_op_convert` 2362, `_invert` 2381, `_centre` 2403, `_amplify` 2449, `_reverse` 2509, `_cut_before` 2544, `_cut_after` 2572, `_resize` 2592, `_quality` 2684, `_delete` 2738, `_insert_slot` 2908, `_remove_slot` 2929, `_swap` 2948, `_exchange` 2967, `_replace` 2982, `smp_save_disk` 3217, `draw_waveform` 2220 (`WavePix` 176×32), `draw_samples` 3341, `smp_hot_swap` 11887, `lib_load_sample_entry` 11937, `stereo_choice_prompt` 11660, `render_import` 7445, `export_samples` 7597; byte-size computations `Length << is16` at 2563, 17004, 18205, 18242, 19464 (selftest).
- RIS: `RIS_SaveWAV`, `RIS_SaveITS`, `RI_SaveITI`, `smp_bytes` (src/it_ris.c ~1817–1960). Render: `Render_SaveSampleWav`, the memory sink (src/it_render.c).

---

## Phase 1: Setup

- [X] T001 Add the reference module `testdata/openmpt-testpattern-stereo.it` to git (1.18 MB) and confirm `.gitignore` keeps `testdata/*.bak` out
- [X] T002 Document flag 0x04 = stereo in the `sample_t` comment in `src/it_structs.h` (comment only, no layout change)

---

## Phase 2: Foundational (blocks every story)

- [X] T003 Channel helpers in `src/it_editor.c` (static, near `smp_alloc`), mirrored as small inline helpers usable from it_ris.c / it_render.c / it_load.c / it_save.c via a new header section in `src/it_structs.h` or `src/it_music.h` (no struct change): `SMP_CHANNELS(s)` (1 / 2), `SMP_WIDTH(s)` (1 / 2), `smp_chan_ptr(s, c)` = `Data + c × (Length + 4) × width`, `smp_alloc_frames(frames, width, channels)` (allocates `channels × (frames + 4) × width`, zero pad), `smp_pad(s)` (interpolator padding after each channel: last value twice, then zeros — as it_load.c ~402), `smp_data_bytes(s)` = `Length × width × channels`
- [X] T004 Size cap: everywhere `SMP_MAXBYTES` limits a sample, count both channels (R10) — `smp_alloc` callers in `src/it_editor.c`
- [X] T005 STR selftest scaffolding in `src/it_editor.c`: block "STR" (`[STR OK]` / `[STR FAIL]`), helper to build a synthetic stereo sample (left = sine, right = silence or a different sine; 8- and 16-bit), helper to compare channel data, work folder `st_str_work` removed afterwards

**Checkpoint**: helpers exist; nothing behaves differently yet.

---

## Phase 3: User Story 1 — Load and play a stereo sample (P1) 🎯 MVP

**Goal**: stereo samples load (IT modules, ITS, WAV/AIFF) and play in stereo with balance pan.

**Independent Test**: spec US1 — the OpenMPT reference file loads with 3 stereo samples; a left-only synthetic sample at centre pan gives right output silent; hard left / hard right per SC-004; `test_pattern` IDENTICAL.

- [X] T006 [US1] `Load_SampleData` in `src/it_load.c`: when the header has flag 0x04, allocate two channel blocks (T003 layout) and read Length frames per channel from the file (planar); compressed (flags 0x04 + 0x08) → decompress the left stream, then the right stream (`DecompressIT8/16` twice from the running reader); apply byte-swap / delta / sign conversion per channel; keep 0x04 in the `Flags &= ~0x0C` rewrite; pad each channel; clamp loops as now
- [X] T007 [US1] Standalone stereo files (`Cvt & 32`) in `src/it_load.c`: `Load_StereoChoice` may return 256 = keep stereo → deinterleave into the two blocks and set 0x04; 64 / 192 unchanged (IT); a NULL hook keeps IT's left-channel behaviour
- [X] T008 [US1] Requester `stereo_choice_prompt` in `src/it_editor.c`: box widened for three buttons — **Stereo** (focused, returns 256), Left (64), Right (192); keys S / L / R, Enter / Space on the focused button, Esc = Left (IT), mouse via `button_hit` (contracts/keys-and-ui.md)
- [X] T009 [US1] Mixer in `src/it_driver.c`: `static chnfilter_t FilterValuesR[MAXSLAVECHANNELS]`; reset and set up wherever `FilterValues[i]` is (filter setup ~746, `ResetFilters` 178); copied with it on NNA (~933)
- [X] T010 [US1] Mixer in `src/it_driver.c`: stereo inner loop `MixInnerStereo` beside `MixInner` (same signature + right-channel data pointer): left = `GetWaveform(L, …, &FilterValues[i])` × LVC, right = `GetWaveform(R, …, &FilterValuesR[i])` × RVC, same ramp updates and LastLeft/RightValue bookkeeping; surround: x = (xL + xR) / 2 through the existing surround formula; the voice loop (~897) selects it when `s->Flags & 4`, passing `smp_chan_ptr(s, 1)`; mono code path untouched
- [X] T011 [US1] Mixer position-only / zero-volume variants and `MixNoLoop` / `MixForwardsLoop` / `MixPingPongLoop` in `src/it_driver.c` pass the right-channel pointer through (or a static per-voice pointer set in the voice loop), so loops, ping-pong and click removal work identically for stereo
- [X] T012 [US1] Run `test_pattern` on the four modules (build via the scratchpad `build_tp.cmd` pattern) → all `IDENTICAL` with the known hashes
- [X] T013 [US1] STR selftest in `src/it_editor.c`: load `testdata/openmpt-testpattern-stereo.it` → samples 1–3 have 0x04, lengths / loops as in the file, channel data equal to the file's planar blocks (compare the first 64 frames of each channel against a direct read of the file)
- [X] T014 [US1] STR selftest: render (feature 018 `Render_Run`, file job) a pattern playing a synthetic left-only stereo sample at pan 32 / 0 / 64 → centre: right silent, left = sine; hard left: left = the sine at full level, right silent; hard right: both outputs silent (the sample's right channel is empty) — SC-004; and a stereo sample with equal channels renders identically to its mono twin
- [X] T015 [US1] STR selftest: requester returns (feed keys S / L / R / Esc through `Screen_KeyFeedTest`) and WAV loading of a generated 2-channel WAV with each choice

**Checkpoint**: stereo plays; mono bit-identical.

---

## Phase 4: User Story 2 — Keep stereo when saving and exchanging (P1)

**Independent Test**: the reference file saved as IT (IT214, IT215, IT2xx) and reloaded → identical stereo data, stereo samples uncompressed; ITS / ITI round trips; S3M downmix + warning.

- [X] T016 [US2] IT writer in `src/it_save.c`: stereo samples → flag 0x04 kept, 0x08 cleared, data = left block then right block (no padding), always `save_sample_raw`-style even when compression is selected; mono unchanged (R1)
- [X] T017 [US2] ITS / ITI writers in `src/it_ris.c` (`RIS_SaveITS`, `RI_SaveITI`): same layout; `smp_bytes` replaced by channel-aware sizes
- [X] T018 [US2] S3M export in `src/it_save.c`: a stereo sample is written as its mono mix ((L+R)/2, rounded toward zero for negatives as C division) from a temporary buffer; one `s3m_warn` line "Warning: Stereo samples converted to mono" (row after the existing warnings)
- [X] T019 [US2] STR selftest: reference file → save with `SaveFormat` 0 / 2 / 3 into `st_str_work`, reload each → per sample flags, Length, loops and both channels byte-identical; the saved stereo headers have 0x08 clear; mono synthetic sample in the same song still compressed when 0 / 3
- [X] T020 [US2] STR selftest: ITS and ITI round trips of a stereo sample; compressed-stereo reading: a synthetic module written by the selftest with a stereo sample compressed as two streams (using the existing compressor per channel) loads back identical
- [X] T021 [US2] STR selftest: S3M export of a song with a stereo sample → the written sample equals the mono mix; the warning callback received the stereo line once

---

## Phase 5: User Story 3 — Edit stereo samples in F3 (P2)

**Independent Test**: SC-003 — each operation on a stereo sample = the same operation on each channel as a mono sample.

- [X] T022 [P] [US3] Per-channel operations in `src/it_editor.c`: `smp_op_convert`, `smp_op_invert`, `smp_op_centre`, `smp_op_amplify` (one peak across both channels), `smp_op_reverse`, `smp_op_quality` (8↔16-bit per channel) — loop over `smp_chan_ptr(s, c)` for c < channels
- [X] T023 [US3] Length-changing operations in `src/it_editor.c`: `smp_op_cut_before`, `smp_op_cut_after`, `smp_op_resize` (both modes) — allocate with `smp_alloc_frames`, process each channel, pad each channel
- [X] T024 [US3] Whole-sample operations in `src/it_editor.c`: `smp_op_delete`, `smp_op_insert_slot`, `smp_op_remove_slot`, `smp_op_swap`, `smp_op_exchange`, `smp_op_replace`, copy / paste of samples, `smp_hot_swap`, `lib_load_sample_entry` — copy full data size (`smp_data_bytes` + padding), keep 0x04
- [X] T025 [US3] Remaining byte-size sites in `src/it_editor.c` (2563, 17004, 18205, 18242 and any `Length << ((Flags & 2)` found by grep) switched to the helpers
- [X] T026 [US3] F3 display in `src/it_editor.c`: `draw_waveform` draws a stereo sample's left channel in the upper 16 pixel rows and the right channel in the lower 16 (same loop markers); `draw_samples` shows "Stereo" after the quality text (e.g. "16 bits Stereo")
- [X] T027 [US3] Alt-U in `handle_sample_altkey` (`src/it_editor.c`): stereo → dialog "Convert to mono" with Mix / Left / Right / Cancel (IT box style, as the stereo requester); mono with data → `confirm_box` "Convert to stereo?"; conversions keep loops and are done under `ed_lock` (voices on the slot cut first as `smp_hot_swap` does)
- [X] T028 [US3] WAV saving of stereo samples: `RIS_SaveWAV` (src/it_ris.c) and `Render_SaveSampleWav` (src/it_render.c) write 2 channels interleaved for 0x04 samples
- [X] T029 [US3] STR selftest: for each operation in T022–T024 apply to a synthetic stereo sample and to two mono samples holding its channels → equal results (SC-003); Alt-U mix / left / right / to-stereo results; stereo WAV export header (2 channels) and interleaving

---

## Phase 6: User Story 4 — Stereo renders into samples (P3)

- [X] T030 [US4] `src/it_render.c` memory sink: for stereo songs keep two channels (planar into `smp_alloc_frames`-compatible memory); the 1 MB cap counts both channels; `src/it_editor.c` `render_import` builds a 0x04 sample from it; mono songs unchanged
- [X] T031 [US4] Update the RND selftest expectations in `src/it_editor.c` (import of itdemo — check its stereo flag: stereo songs now give stereo samples) and add an STR check: import from a stereo song is stereo with channels equal to a file render of the same pattern at 44.1 kHz
- [X] T032 [US4] Update feature 018 docs where they say imports are mono: `README.md` (Rendering section), `specs/018-audio-render-extensions/` notes (a pointer to 019 is enough)

---

## Phase 7: User Story 5 — Discoverable (P3)

- [X] T033 [US5] F1: `help_lines` in `src/it_editor.c` adds for the sample list (ctx 2) "Alt-U  Convert sample: stereo <-> mono"; selftest DLG expectations updated
- [X] T034 [P] [US5] README: an "it26 additions" entry "Stereo samples" (loading, balance pan, Alt-U, saving uncompressed as OpenMPT, IT 2.14 plays the left channel, S3M downmix) and the fidelity-notes entry
- [X] T035 [P] [US5] `docs/HANDOFF.md` section for feature 019 (layout, mixer branch, FilterValuesR, files) and `CHANGELOG.md` Unreleased entry

---

## Phase 8: Polish

- [X] T036 `test_pattern` again (IDENTICAL), full selftest (all blocks OK incl. STR, RND, DLG), and the existing IT 2.14 comparison scripts for F3 (`mouse_f3`, `hot_swap`, `hot_swap_empty`) — no differences beyond the known ones
- [ ] T037 Manual quickstart checks (quickstart.md Manual 1–6): listen to the reference file, balance pan, save → open in OpenMPT / Schism (by the user), IT 2.14 opens the saved file (comparison harness: load it on the DOS side)
- [X] T038 Mark tasks done here and record anything not run

## Implementation notes (2026-10-18)

- T001: the reference module is ready but stays untracked until the user commits (feature 019 is local until release).
- T012 / T036: three modules `IDENTICAL` with the known hashes. `beyond_network.it` changed (`a43e6f19` → `b4ee2da5`): it holds 37 genuine stereo samples (planar data, IT 2.17-compatible header) that IT 2.14 played left-only; it26 now plays them in stereo. HANDOFF records the new hash. Round trips (fmt 3/0/2) identical for all five modules, the reference included.
- T021: the S3M warning fires once per stereo sample on the same row (34), as IT's other per-sample S3M warnings do; the selftest checks it fired.
- T029: amplify is checked separately (one factor from the peak of both channels, so the mono twins would get different suggestions).
- T036: IT 2.14 comparisons `mouse_f3`, `hot_swap`, `hot_swap_empty` -- only the known differences (copyright year, the hot-swap extension, the file list landing on another file because the folder's contents changed).
- T037: manual listening, OpenMPT / Schism re-open and IT 2.14 opening a saved stereo module are left to the user.
- Follow-ups requested during implementation: sample length limit = OpenMPT/Schism `MAX_SAMPLE_LENGTH` (0x10000000 frames per channel) instead of IT's ~4 MB; render imports stop after 10 minutes instead of 1 MB; WAV reader extension for 24/32-bit PCM, float, EXTENSIBLE and data chunks after metadata chunks (Reaper exports), converted to 16-bit. Selftests: STR (wide WAVs, a 6 MB sample through load + IT save), LIB (float / 24-bit fixtures now load).

---

## Dependencies & Execution Order

- Setup → Foundational (T003–T005) → stories.
- US1 (T006–T015) first: loading and the mixer. **MVP**.
- US2 (T016–T021) needs T006 (load) for its round trips.
- US3 (T022–T029) needs T003 and T006.
- US4 (T030–T032) needs US1 (stereo playback) and feature 018's render core.
- US5 and Polish last.

## Parallel Opportunities

- T022 (per-channel ops) alongside T016–T018 (savers): different files / functions.
- T034, T035 (docs) alongside implementation.
- Mixer (T009–T011, it_driver.c) alongside the loader (T006–T007, it_load.c) once T003 exists.

## Implementation Strategy

1. MVP: Phases 1–3 — stereo samples load and play, mono bit-identical (`test_pattern`).
2. US2 — saving / interchange (the reference file round trip).
3. US3 — the editor; US4 — renders; US5 — docs.
4. Commit per phase locally with the user's go-ahead; push only when the user decides (no spoilers before release).
