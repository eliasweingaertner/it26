# Tasks: Pattern Editor Completion

**Input**: Design documents from `specs/010-pattern-editor-completion/`
**Prerequisites**: plan.md, research.md, data-model.md, quickstart.md
**Repo**: `C:\Users\elias\fable5\ittrack` (all file paths below are repo-relative)

**Tests**: selftest additions are explicitly required by FR-010 / SC-005 and
are included per story.

## Phase 1: Setup

- [X] T001 Verify clean baseline: build `ited.exe` + `test_pattern.exe` per
      HANDOFF Â§3 and run the Â§4 gates (determinism Ã—4, `--roundtrip` Ã—4,
      `ITED_SELFTEST=1`) so post-change failures are attributable. No file
      changes.

## Phase 2: Foundational

- [X] T002 Add the feature-010 state block to `src/it_editor.c`: the mirrors
      from data-model.md (`pe_pattern_set_length/start/end`,
      `pe_view_channels[100]`, `pe_view_method_info[5]`,
      `pe_cursor_positions[5][9]`, `pe_view_division`, `pe_view_width`,
      `pe_num_channels_edit`, `pe_view_channel_tracking`,
      `pe_centralise_cursor` â€” folding the existing 009 centralise flag into
      bit 0), each with an IT_PE.ASM line-reference comment. Implement
      `pe_check_width()` 1:1 from IT_PE.ASM 8711..8763 (fail â‰¥76, ViewWidth,
      NumChannelsEdit=(74-w)/14). Extend `ited.cfg` read/write with
      ViewDivision/Tracking/CentraliseCursor.

## Phase 3: User Story 1 â€” pattern-length dialog (P1)

**Goal**: Ctrl-F2 opens the original Set Pattern Length requester; OK resizes
[start..end] with original semantics; undoable for the current pattern.

**Independent test**: quickstart.md gate 3 bullet 1 + manual Ctrl-F2 smoke.

- [X] T003 [US1] Build the `O1_SetPatternLength` modal in `src/it_editor.c`
      on the existing modal-widget machinery: box (15,19)-(65,33) style 3,
      header/static text attr 20h, thumb boxes style 25, three type-9
      thumbbars bound to the R1 statics (32..200 / 0..199 / 0..199), OK
      button (35,30)-(44,32) style 8 returning 1, ESC returning 0, initial
      focus = length bar (IT_OBJ1.ASM 659..730 verbatim).
- [X] T004 [US1] Wire Ctrl-F2 in the F2 key table to `pe_set_pattern_length()`
      in `src/it_editor.c`: prime start/end=current pattern (NOT the length â€”
      R1 quirk), run modal; on OK push one undo snapshot of the current
      pattern (standard type), then for each pattern in [start..end]
      unpack â†’ set row count to `pe_pattern_set_length` â†’ pack under
      `Engine_Lock`/`Unlock`; re-decode current pattern; redraw.
- [X] T005 [US1] Selftest (ITED_SELFTEST block in `src/it_editor.c`): on the
      scratch pattern, shrink 64â†’32 (verify row count + discarded rows),
      grow â†’128 (blank fill), Ctrl-Backspace-revert byte-equality against the
      pre-resize snapshot; leave song data untouched.

## Phase 4: User Story 2 â€” in-F2 mute/solo (P2)

**Goal**: the full original key family drives the engine mute state from F2.

**Independent test**: quickstart.md gate 3 bullet 2.

- [X] T006 [US2] Add the six key handlers to the F2 key table in
      `src/it_editor.c` per research R2: `\`/Alt-F9 toggle, keypad `/`
      mute+Tab, `?` channel-1-clamped-at-0 then toggle, Alt-F10 solo, `|`
      solo+Tab, Alt-`\` unmute-all â€” all via the existing
      `Music_ToggleChannel`/`Music_SoloChannel`/`Music_UnmuteAll`.
- [X] T007 [US2] Selftest: script each key, assert engine channel-flag state
      after each step and header-attr agreement (grid redraw shows 10h for
      muted), and that F5/F11 read the same flags; restore state after.

## Phase 5: User Story 3 â€” multi-scheme views + toggles (P3)

**Goal**: five per-channel view methods, scheme keys, presets, toggles.

**Independent test**: quickstart.md gate 3 bullet 3 + gate 4 ITED_SHOT
comparisons.

- [X] T008 [US3] Implement the scheme mutators in `src/it_editor.c` per
      research R3: `pe_fast_view()` (Ctrl-0 delete/compact, Ctrl-1..5
      set/append, revert on width failure; IT_PE.ASM 10254..10315),
      `pe_quick_view_setup()` + the four Ctrl-Shift presets (1/6-7, 2/9-10,
      3/18-24, 4/24-36, auto-enable tracking; 10169..10250),
      `PEFunction_ViewTrack` Alt-T cycle (8767..), `PEFunction_ClearViews`
      Alt-R, `PEFunction_ToggleDivision` Alt-H (10095..10111), and the
      Ctrl-T/Ctrl-H toggles with their verbatim status messages
      (11189..11226). Wire all keys into the F2 table.
- [X] T009 [US3] Rework the F2 grid drawer in `src/it_editor.c` to iterate
      `pe_view_channels` entries then `pe_num_channels_edit` default
      channels, dispatching per method; implement the four new cell
      renderers 1:1 from ViewCompress (9209)/ViewAllSmall (9342)/ViewNote
      (9479)/ViewTiny (9818) + ViewCommon (9035), reusing the F5 font-B
      packed-digit glyphs for the small formats; width-matched channel
      captions (PE_DrawPatternEdit ~2287 switch); division char-168 columns
      honouring `pe_view_division`.
- [X] T010 [US3] Port cursor geometry: per-method column x-offsets/widths
      from `pe_cursor_positions` (decode PE_HilightView for methods 3/4),
      horizontal scroll/`StartChannelEdit` clamping, and
      `pe_view_channel_tracking` behaviour; verify block marking, paste,
      template entry and digit entry operate unchanged on underlying cells
      in mixed layouts.
- [X] T011 [US3] Selftest: Ctrl-3 assignment reflected in
      `pe_view_channels`; Ctrl-Shift-2 exact preset table contents; Ctrl-0
      removal/compaction; width-overflow revert (assign wide methods until
      `pe_check_width` fails, assert table unchanged); cursor addressing
      walk across a mixed-scheme layout (move through all 9 columns of a
      ViewFull and a ViewCompress channel); toggle messages verbatim.

## Phase 6: Polish

- [X] T012 [P] ITED_SHOT captures of each view method + the four presets on
      itdemo; side-by-side compare vs `..\screenshots\`; fix deviations
      (layout only from ASM tables).
- [X] T013 [P] Docs: README fidelity notes (persistent PatternSetLength,
      single-pattern range-resize undo), HANDOFF Â§2/Â§6 update (feature 010
      done, leftovers cleared), spec checklists.
- [X] T014 Final gates per quickstart.md (determinism Ã—4, roundtrip Ã—4,
      full selftest, Linux-parity note if applicable) and commit(s) in the
      established stage-N style.

## Dependencies

- T001 â†’ T002 â†’ {US1: T003â†’T004â†’T005} and {US2: T006â†’T007} and
  {US3: T008â†’T009â†’T010â†’T011} â†’ T012/T013 [P] â†’ T014.
- US1, US2, US3 are mutually independent after T002; US2 is the smallest.

## Implementation strategy

MVP = Phase 3 (US1). Ship order US1 â†’ US2 â†’ US3 as separate commits
(stage style of feature 009); T012/T013 parallelizable at the end.
