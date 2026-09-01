# Implementation Plan: Scancode-Based Keyboard Input with Layout-Aware Text Entry

**Branch**: `014-scancode-keyboard-input` | **Date**: 2026-08-20 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `/specs/014-scancode-keyboard-input/spec.md`

## Summary

Impulse Tracker resolves note entry from the **physical key position** and text
entry from a **separately translated character** — `K_GetKey` returns both, as
`CX`/`DX` (research R1). The port collapsed them into one `int`, so on a German
QWERTZ keyboard the two note rows are cross-wired at the keys printed Y and Z,
punctuation bindings are scattered, and umlauts cannot be typed at all.

The fix restores the original's two-layer model: widen the key event to carry
`(scan, flags, ch, code)`, route note entry and position-dispatched bindings off
`scan`, route text entry off `ch`, and leave the Alt/Ctrl letter shortcuts on the
character layer — which is what `Keyboard/DE.ASM` proves the original itself does
(research R5). Scancodes come from the OS (`lParam` bits 16-23 on Win32, a
HID -> set-1 table on SDL); characters come from the host layout, with an optional
`KEYBOARD.CFG` loader in the original's format as an override.

## Technical Context

**Language/Version**: C11 (existing codebase; no new language features required)

**Primary Dependencies**: Win32 API (`it_screen_win32.c`), SDL2 (`it_screen_sdl.c`),
POSIX termios / ANSI parser (`it_screen.c`). **No new external dependencies.**
Vendored `external/miniaudio.h` untouched.

**Storage**: `ited.cfg` gains `keyboard_cfg` and `keyboard_layout`. Optional
binary `KEYBOARD.CFG` read at startup (format in
[contracts/keyboard-cfg-format.md](contracts/keyboard-cfg-format.md)).

**Testing**: `tests/test_pattern.c` determinism ×4 and round-trip ×4×3 gates
(must not move); `ITED_SELFTEST=1 ITED_TERM=1` block suite, gaining a `KBD`
block driven by a new `Screen_KeyFeedTest` hook so layout behaviour is testable
on any host without a German keyboard attached.

**Target Platform**: Windows (Win32 pixel backend), Linux/macOS (SDL2 pixel
backend + terminal backend). Verification hosts: Windows natively, Linux via
WSL Ubuntu (terminal backend only — no SDL2 headers there).

**Project Type**: Desktop application — an editor over the ported IT engine.

**Performance Goals**: Key handling stays off the audio path entirely. Lookups
are a 29-entry linear scan (as the original) and a 128-entry CP437 table; both
are per-keystroke, so no measurable cost.

**Constraints**: Platform specifics stay behind the `screen_backend_t` vtable
(constitution V). `Key_Get()` must keep working unchanged throughout the
migration so the 14 existing call sites and 56 character-case bindings are never
simultaneously broken.

**Scale/Scope**: `src/it_editor.c` is 11,384 lines with 14 `Key_Get()` call sites
and 56 `case '...'` bindings; three backends; one new optional file parser.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Gates derived from the constitution (`.specify/memory/constitution.md` v1.0.0):

- **Engine fidelity (I) + Determinism (III)**: **PASS — no engine files touched.**
  This feature is confined to `it_screen.h`, `it_screen.c`, the three backends,
  and `it_editor.c`. None of `it_music.c`, `it_effects.c`, `it_tables.c`,
  `it_driver.c`, `it_load.c`, `it_structs.h`, `it_pattern.c` is modified. The
  determinism ×4 and round-trip ×4×3 gates are re-run regardless and must come
  back byte-identical to the baseline; any movement is a defect in this feature,
  not an accepted deviation.
- **Authentic data (II)**: **PASS.** Every behavioural constant is decoded from
  the ASM and recorded in [research.md](research.md): the 29-entry note table
  from `IT_I.ASM:333`, the `CH`/`CL` bit layout from `IT_K.ASM:1174-1250`, the
  translation-table format from `IT_K.ASM:1274-1290` and `Keyboard/DE.ASM`, and
  the label-based Alt remapping from `Keyboard/DE.ASM`. Nothing is eyeballed.
  `it_vgadata.c` is not touched.
- **One real engine (IV)**: **PASS.** Note events continue to reach the engine
  through `Music_PlayNote` under `ed_lock()`/`ed_unlock()`; only the derivation
  of the note number changes. No substitute input or engine path is introduced.
- **Portability (V)**: **PASS with one recorded limitation.** All new platform
  code sits behind the `screen_backend_t` vtable via a new optional `key_event`
  slot; a backend that cannot supply positions leaves it NULL and the wrapper
  synthesizes. The terminal backend genuinely cannot obtain scancodes from a
  standard terminal (research R8); this is documented to the user as a
  limitation of that backend rather than hidden. Code stays C11 on all three
  platforms.

**Post-Phase-1 re-check**: unchanged — the design in
[contracts/key-event-api.md](contracts/key-event-api.md) keeps `Key_Get()`
byte-compatible, adds no dependency, and confines platform code to the backends.
No entry is required in Complexity Tracking.

## Project Structure

### Documentation (this feature)

```text
specs/014-scancode-keyboard-input/
├── plan.md                         # This file
├── spec.md                         # Feature specification
├── research.md                     # Phase 0: the ASM decode contract (R1-R9)
├── data-model.md                   # Phase 1: it_key_t and friends
├── quickstart.md                   # Phase 1: how to validate
├── checklists/requirements.md      # Spec quality checklist
├── contracts/
│   ├── key-event-api.md            # Backend <-> editor key interface
│   └── keyboard-cfg-format.md      # Original's layout file format
└── tasks.md                        # Phase 2 output (/speckit-tasks)
```

### Source Code (repository root)

```text
src/
├── it_screen.h              # it_key_t, Key_GetEvent, backend vtable slot
├── it_screen.c              # Key_Get shim, reverse map, KEYBOARD.CFG loader,
│                            #   CP437 conversion, terminal backend events
├── it_screen_win32.c        # scancode from lParam 16-23/24; WM_CHAR pairing
├── it_screen_sdl.c          # SDL_SCANCODE -> set-1 table; TEXTINPUT pairing
└── it_editor.c              # key_to_note on scan; text fields on ch;
                             #   keypress diagnostic view

tests/
└── test_pattern.c           # unchanged; re-run as the regression gate

testdata/                    # unchanged reference modules
tools/                       # unchanged
```

**Structure Decision**: The existing flat `src/` layout is kept. This feature
adds no new translation unit: the key-event plumbing belongs in `it_screen.*`
next to the existing `Key_Get`, and the consumers are already in `it_editor.c`.
Introducing a new module would split the input path across files for no benefit,
and would break the correspondence with the original's single `IT_K.ASM`.

## Implementation Phases

Staged so the tree builds and every gate passes after each phase — matching the
staged-commit approach used for features 009 and 010.

**Phase A — widen the event, change no behaviour.**
Add `it_key_t`, `Key_GetEvent()`, and the `key_event` vtable slot. `Key_Get()`
becomes a shim over it. All three backends fill `code` exactly as today and
populate `scan`/`flags`/`ch` on a best-effort basis. Gates must be green with
zero observable change. *This is the phase that de-risks the other four.*

**Phase B — positional note entry (P1, the reported bug).**
Transliterate the 29-entry table from `IT_I.ASM:333` and switch `key_to_note()`
(`it_editor.c:5786`) to match on `scan`. Add the `KBD` selftest block with the
US/German parity assertion. After this phase the user's DOSBox comparison passes.

**Phase C — character layer (P2).**
CP437 conversion at the backend boundary; text fields, message editor, and
filename requesters consume `ch`; non-CP437 input rejected cleanly. Extend the
`KBD` block with the umlaut round trip.

**Phase D — audit the position-dispatched bindings (P3).**
Walk the 56 `case '...'` sites against the ASM key lists and move to `scan` only
those the original dispatches positionally, leaving the letter-named Alt/Ctrl
shortcuts on the character layer. Resolve research R9 item 1 (the keypad-divide
`135h` / `1B5h` discrepancy) here.

**Phase E — override and diagnostics (P4, P5).**
`KEYBOARD.CFG` loader with the fallback behaviour of FR-010, `ited.cfg` keys, and
the keypress diagnostic view.

## Complexity Tracking

> No constitution violations. Table intentionally empty.
