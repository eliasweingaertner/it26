# Implementation Plan: Terminal-Backend Mouse and Modifier Keys

**Branch**: `011-terminal-input` | **Date**: 2026-07-11 | **Spec**: spec.md

**Repo**: `C:\Users\elias\fable5\ittrack`

## Summary

Give the VT/ANSI terminal backend (it_screen.c) the full input surface the
pixel backends gained in features 009/010: a platform-neutral incremental
escape-sequence parser (Alt prefix, xterm modified-CSI, modifyOtherKeys/
CSI-u, SGR mouse) feeding a key FIFO + an it_mouse_t mirror, enabled/
disabled with the terminal modes in init/uninit; extend the Windows-console
scan-code table; prove the parser headless via a selftest TERM block.

## Technical Context

- **Language**: C11, MSVC (Windows) / GCC or Clang (POSIX); no new deps.
- **Files touched**: `src/it_screen.c` (parser, pump, modes, console
  table), `src/it_screen.h` (test hook decl, comment fixes),
  `src/it_editor.c` (selftest TERM block only), README/HANDOFF.
- **Constraints (constitution)**: engine untouched; editor logic untouched;
  backend stays behind `screen_backend_t`; determinism gates must stay
  green; deviations (px approximation, no shift press/release) documented.
- **Verification**: headless selftest on Windows; full gate set; WSL/Linux
  interactive pass if available, else deferred-verification note.

## Project Structure

No new files. The parser lives next to the terminal backend in
`it_screen.c` (compiled on all platforms; only the read pump is
platform-conditional).

## Phases

- Phase 1 (foundational): parser skeleton + key FIFO + test hook.
- Phase 2 (US1): Alt prefix + modified CSI + CSI-u/modifyOtherKeys keys.
- Phase 3 (US2): SGR mouse decode + mode enables + Term mouse vtable entry.
- Phase 4 (US3): Windows console scan-code extension.
- Phase 5: selftest TERM block, docs, gates.
