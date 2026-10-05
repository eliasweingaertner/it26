---

description: "Task list for 014-scancode-keyboard-input"
---

# Tasks: Scancode-Based Keyboard Input with Layout-Aware Text Entry

**Input**: Design documents from `/specs/014-scancode-keyboard-input/`

**Prerequisites**: [plan.md](plan.md), [spec.md](spec.md), [research.md](research.md), [data-model.md](data-model.md), [contracts/](contracts/)

**Tests**: Test tasks ARE included — the spec requires them explicitly (FR-014,
FR-015, FR-016). They take the form of blocks in the existing `ITED_SELFTEST`
suite plus the standing `test_pattern` gates, not a separate framework.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1..US5)
- Include exact file paths in descriptions

## Path Conventions

Single project, flat layout at repository root `C:\Users\elias\fable5\ittrack`:
`src/`, `tests/`, `testdata/`, `tools/`. Confirmed in plan.md "Structure Decision".

**Note on parallelism**: `src/it_editor.c` is a single 11,384-line file, so tasks
touching it are inherently serial. Genuine `[P]` opportunities exist almost only
across the three backend files. This is called out honestly rather than padded
with `[P]` markers that would cause edit conflicts.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Capture the baseline this feature must not disturb.

- [X] T001 Record baseline gate output — run `build/test_pattern` over `testdata/beyond_network.it`, `testdata/itdemo.it`, `testdata/quests_end.it`, `testdata/synthscape_filters.it` plus `--roundtrip`, and save the hashes to `specs/014-scancode-keyboard-input/baseline.md` for the SC-005 comparison
- [X] T002 Confirm a clean build on both verification hosts per `docs/HANDOFF.md` §3 — MSVC `vcvars64` on Windows and `wsl -d Ubuntu` for the terminal backend (no SDL2 headers there; `src/it_screen_sdl.c` cannot be compile-checked on WSL)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Widen the key event with **zero observable behaviour change**. Plan
Phase A. Every user story depends on this; nothing else may start until T012 is
green.

**⚠️ CRITICAL**: `Key_Get()` must keep returning exactly what it returns today.
The 14 existing call sites and 56 `case '...'` bindings are not touched here.

- [X] T003 Define `it_key_t` (`scan`, `flags`, `ch`, `code`) and the `CH` modifier-bit constants in `src/it_screen.h`, per [contracts/key-event-api.md](contracts/key-event-api.md) and research R2
- [X] T004 Add the optional `int (*key_event)(it_key_t *k)` slot to `screen_backend_t` in `src/it_screen.h`, keeping the existing `int (*key)(void)` slot unchanged
- [X] T005 Implement `Key_GetEvent()` in `src/it_screen.c` and reduce `Key_Get()` (currently `src/it_screen.c:1111`) to a shim over it that returns `.code`; when a backend leaves `key_event` NULL, synthesize the event from `key()`
- [X] T006 Add the Unicode→CP437 conversion table and helper in `src/it_screen.c` per research R7, covering the 128 high codes and returning "unrepresentable" for anything else
- [X] T007 [P] Fill `it_key_t` in `src/it_screen_win32.c`: scancode from `lParam` bits 16-23 with `+0x80` when bit 24 is set, modifier bits from `GetKeyState`, and pair the pending `WM_KEYDOWN` scancode with the following `WM_CHAR`/`WM_SYSCHAR`
- [X] T008 [P] Add the static `SDL_SCANCODE_* → PC set-1` table in `src/it_screen_sdl.c`, covering the 29 note keys, the punctuation keys, and the modifier/extended keys already handled (research R6)
- [X] T009 Fill `it_key_t` in `src/it_screen_sdl.c` from `e.key.keysym.scancode` through the T008 table, pairing with the following `SDL_TEXTINPUT` (UTF-8 decoded, then CP437-converted)
- [X] T010 Fill `it_key_t` in the terminal backend in `src/it_screen.c` with `scan = 0` and the parser's decoded character, leaving position resolution to the reverse map added in T033
- [X] T011 Add the `Screen_KeyFeedTest(const it_key_t *k, int n)` hook in `src/it_screen.c` and declare it in `src/it_screen.h` alongside the feature-011 `Screen_TermFeedTest` hook
- [X] T012 Gate: re-run the T001 commands and the full selftest (`ITED_SELFTEST=1 ITED_TERM=1 ited testdata/itdemo.it`); hashes must be **identical** to `baseline.md` and every existing block must still report OK

**Checkpoint**: Foundation ready — the event carries positions, nothing consumes them yet.

---

## Phase 3: User Story 1 — Positional note entry (Priority: P1) 🎯 MVP

**Goal**: Note entry resolves from the physical key position, so the German
QWERTZ note rows match DOSBox and a US keyboard.

**Independent test**: Play the lower and upper note-row sweeps on the German
keyboard and compare against the same physical keys on a US layout and against
IT 2.14 under DOSBox — 29 of 29 keys must agree (SC-001).

- [X] T013 [US1] Transliterate the 29-entry note table from `IT_I.ASM:333` (decoded in research R3, including the `0FFFFh` terminator) into `src/it_editor.c`, replacing the character-keyed `map[]` at `src/it_editor.c:5788`
- [X] T014 [US1] Rewrite `key_to_note()` (`src/it_editor.c:5786`) to take `it_key_t` and match on `scan` only, preserving the existing base-octave arithmetic and the `note < 0 || note > 119` clamp
- [X] T015 [US1] Migrate the note-entry call sites in `src/it_editor.c` (`handle_pattern_key` at `src/it_editor.c:7495` and the sample/instrument keyjazz paths reached from `smpfield_ckey`/`notewin_ckey`) to `Key_GetEvent()`, leaving all non-note bindings on `.code`
- [X] T016 [US1] Add the `KBD` selftest block in `src/it_editor.c` driving `Screen_KeyFeedTest` with an identical `scan` sequence under simulated US and German character sets, asserting the note output is identical while the characters differ (FR-014)
- [X] T017 [US1] Gate: determinism ×4, round-trip ×4×3, and the full selftest including the new `KBD` block, on Windows and WSL
- [ ] T018 [US1] Manual parity check per [quickstart.md](quickstart.md) §3 on the reporting user's German keyboard against DOSBox — the keys printed `Z` and `Y` are the ones to watch

**Checkpoint**: The reported bug is fixed and independently shippable.

---

## Phase 4: User Story 2 — National characters in text (Priority: P2)

**Goal**: `ä ö ü ß Ä Ö Ü` can be typed into names and the message, and survive a
save/reload round trip.

**Independent test**: Type the seven characters into a sample name, song title,
and the message editor; save, reload, confirm byte-identical (SC-002).

- [X] T019 [US2] Route text-field character input in `src/it_editor.c` to `it_key_t.ch` for the name/title fields reached via `widgets_key` (`src/it_editor.c:624`) and `smpfield_ckey` (`src/it_editor.c:2556`)
- [X] T020 [US2] Reject characters with no CP437 code at the field-input layer in `src/it_editor.c` — leave the field contents and cursor position untouched, no substitution (FR-005)
- [X] T021 [US2] Route the message editor (`handle_message_key`, `src/it_editor.c:5629`) to `it_key_t.ch`, rendering the high range through the feature-013 hi-ASCII font bank
- [ ] T022 [US2] ~~Route the filename requesters in `src/it_editor.c` to `it_key_t.ch`~~ **DEFERRED — see note.** Name fields and the message editor were routed; filename requesters were deliberately left ASCII-only. A CP437 high byte in a filename is handed to `fopen`, which on Windows reinterprets it through the ANSI codepage (CP1252), so `ä` (CP437 0x84) would create a file named `„`. Doing this correctly needs a codepage conversion at the filesystem boundary, which is a separate concern from keyboard input. Creating wrongly-named, hard-to-reopen files is worse than not accepting the character.
- [X] T023 [US2] Extend the `KBD` selftest block in `src/it_editor.c` with the umlaut round trip — the seven characters into a sample name, saved and reloaded byte-identically (FR-015) — and a non-CP437 rejection assertion
- [X] T024 [US2] Gate: determinism ×4, round-trip ×4×3, full selftest, on Windows and WSL
- [ ] T025 [US2] Manual check per [quickstart.md](quickstart.md) §4

**Checkpoint**: Text entry is layout-correct; US1 remains green.

---

## Phase 5: User Story 3 — Shortcuts follow the printed keycap (Priority: P3)

**Goal**: Alt-letter and Ctrl-letter shortcuts resolve by label on every layout —
a **preservation** requirement, since Win32 VK codes and SDL keysyms already give
this behaviour (research R5). The risk is regressing it while migrating bindings.

**Independent test**: Exercise every documented Alt-letter and Ctrl-letter
shortcut on the German keyboard; each fires the command the manual names (SC-003).

- [X] T026 [US3] Audit all 56 `case '...'` bindings in `src/it_editor.c` against the ASM key lists (`IT_PE.ASM`, `IT_F.ASM`, `IT_DISK.ASM`, `IT_OBJ1.ASM`), and record in `specs/014-scancode-keyboard-input/binding-audit.md` which the original dispatches by position versus by character
- [X] T027 [US3] Migrate only the position-dispatched bindings identified in T026 to `it_key_t.scan` in `src/it_editor.c`, leaving every letter-named Alt/Ctrl shortcut on the character layer
- [X] T028 [US3] Resolve research R9 item 1 in `src/it_screen.h` — re-read the `MuteNext` binding in `IT_PE.ASM` and correct either the `ITK_KP_DIVIDE` "scan 135h" comment at `src/it_screen.h:151` or the code it documents; record a deviation note if the original really uses the main-row `35h`
- [X] T029 [US3] Verify the Alt/Ctrl handling in `src/it_screen_win32.c` (`wp >= 'A' && wp <= 'Z'`, `src/it_screen_win32.c:216`) and `src/it_screen_sdl.c` still resolves by label after the T027 migration
- [X] T030 [US3] Extend the `KBD` selftest block in `src/it_editor.c` to assert Alt-Z and Alt-Y fire their documented commands under the simulated German layout
- [X] T031 [US3] Gate: determinism ×4, round-trip ×4×3, full selftest, on Windows and WSL; plus the manual sweep in [quickstart.md](quickstart.md) §5

**Checkpoint**: All three primary behaviours correct on any host layout.

---

## Phase 6: User Story 4 — Explicit layout override (Priority: P4)

**Goal**: A `KEYBOARD.CFG` in the original's format can replace the host layout,
and a broken one never prevents startup.

**Independent test**: Select a shipped layout file in `ited.cfg`, restart, confirm
characters follow the file while note positions are unchanged; then truncate the
file and confirm a clean fallback.

- [X] T032 [US4] Implement the `KEYBOARD.CFG` parser in `src/it_screen.c` per [contracts/keyboard-cfg-format.md](contracts/keyboard-cfg-format.md) — `FileLength` validation, the `(condition, value)` walk to `0FFh`, and bounds checking against the real file size
- [X] T033 [US4] Add the `keyboard_cfg` and `keyboard_layout` keys to the `ited.cfg` read/write paths in `src/it_editor.c`, and implement the terminal backend's character→scancode reverse map (research R8) keyed off `keyboard_layout`, defaulting to `us`
- [X] T034 [US4] Wire the loaded table into the character layer in `src/it_screen.c` so it overrides the host layout for `.ch` only, never for `.scan` (FR-002, FR-009)
- [X] T035 [US4] Implement the failure path in `src/it_screen.c` — missing, unreadable, bad length, or unterminated list reports to the user and falls back to the host layout without failing startup (FR-010)
- [X] T036 [US4] Add a selftest assertion in `src/it_editor.c` that a synthetic malformed table triggers the fallback rather than a crash or a startup failure
- [X] T037 [US4] Gate + manual check per [quickstart.md](quickstart.md) §6 using an assembled table from `fable5\impulsetracker\Keyboard\DE.ASM`

**Checkpoint**: Layouts the host cannot supply are reachable; DOS parity testable.

---

## Phase 7: User Story 5 — Keypress diagnostic view (Priority: P5)

**Goal**: The user can see the position code, character, and modifier state for
any key — the evidence needed to hand-write a layout file.

**Independent test**: Open the view, press the key printed `Z` on a German
keyboard, read back position `15h` and character `z` (SC-007).

- [X] T038 [US5] Add the keypress diagnostic screen to `src/it_editor.c`, modelled on the original's Ctrl-F1 keypress table referenced in the `Keyboard/DE.ASM` header comment, showing `scan` in hex, the translated character, and the decoded `flags` bits
- [X] T039 [US5] Bind the diagnostic view in `src/it_editor.c` and add its key code to `src/it_screen.h` if the original's binding is not already represented
- [X] T040 [US5] Add an `ITED_SHOT_KEYS` capture aid for the new screen in `src/it_editor.c`, matching the existing `ITED_SHOT_*` convention
- [X] T041 [US5] Gate + manual check per [quickstart.md](quickstart.md) §7

---

## Phase 8: Polish & Cross-Cutting Concerns

- [X] T042 Document the two-layer input model and the terminal backend's stated limitation in `README.md` under the existing "Fidelity notes" section
- [X] T043 Update `docs/HANDOFF.md` — status paragraph, the new `KBD` selftest block in the gate list, the `ited.cfg` keys, and the §6 roadmap entry for this feature
- [X] T044 Fold the research R9 items 2 and 3 into `docs/HANDOFF.md` as either resolved or explicitly deferred (Alt+keypad numeric entry; `LastKey` autorepeat behaviour)
- [X] T045 Re-run the complete gate set one final time on Windows and WSL — determinism ×4, round-trip ×4×3, and all selftest blocks including `KBD`
- [X] T046 Compare the final hashes against `specs/014-scancode-keyboard-input/baseline.md` and confirm SC-005 exactly
- [X] T047 Re-copy `specs/` and `.specify/memory/constitution.md` from `C:\Users\elias\fable5` into the `it26` repo snapshot so the committed copy does not drift
- [ ] T048 Stage the work as separate commits per phase (A..E) following the feature 009/010 precedent, using `git commit -F <file>` for the messages

---

## Dependencies & Execution Order

```text
Phase 1 (T001-T002)  Setup / baseline
        │
        ▼
Phase 2 (T003-T012)  Foundational — BLOCKS EVERYTHING
        │
        ├──────────────► Phase 3  US1  (T013-T018)   P1  ◄── MVP
        │                        │
        │                        ▼
        │                Phase 4  US2  (T019-T025)   P2
        │                        │
        │                        ▼
        │                Phase 5  US3  (T026-T031)   P3
        │
        ├──────────────► Phase 6  US4  (T032-T037)   P4
        │                (needs T005/T006 only)
        │
        └──────────────► Phase 7  US5  (T038-T041)   P5
                         (needs T003/T005 only)
                                 │
                                 ▼
                         Phase 8  Polish (T042-T048)
```

**Story dependencies**: US1 is independent once the foundation lands. US2 is
independent of US1 in principle, but both migrate `src/it_editor.c` call sites,
so running them serially avoids conflicts. US3 must follow US1 and US2 because it
audits the bindings those phases have already touched. US4 and US5 are genuinely
independent of US1-US3 and could be done at any point after Phase 2.

## Parallel Opportunities

Real ones are limited by the single-file editor:

- **T007 ‖ T008** — Win32 and SDL backends, different files, no shared state.
- **T006 ‖ T007 ‖ T008** — after T005 lands, the CP437 helper and the two backends
  can proceed together (T006 is in `it_screen.c`, so it must not run concurrently
  with T005 or T010).
- **US4 ‖ US5** — Phase 6 is mostly `it_screen.c`, Phase 7 is mostly
  `it_editor.c`; the only overlap is T033's `ited.cfg` handling.
- Everything inside Phases 3, 4, and 5 is serial: same file, overlapping regions.

## Implementation Strategy

**MVP = Phase 1 + Phase 2 + Phase 3 (T001-T018, 18 tasks).** That delivers
positional note entry, which is the entire reported defect. It is shippable on
its own: the user can compose on a German keyboard against DOSBox parity, with
text entry unchanged from today.

**Incremental delivery**: each phase ends with a gate task, so the tree builds and
every existing gate passes at every checkpoint. Phase 2 in particular is designed
to be a no-op behaviourally — if T012 shows any hash movement or any failed
selftest block, stop and fix before touching a single consumer. That is what
keeps a 14-call-site, 56-binding migration from turning into a bisect hunt.

**Risk order**: Phase 2 is the highest-risk phase (it changes the shape of the
input path for every consumer at once) and the one with the strongest safety net
(zero intended behaviour change, so any gate movement is unambiguous). Phase 5 is
second — it is the phase where a careless migration could move a shortcut nobody
tests manually; the T026 audit document exists to make that reviewable.
