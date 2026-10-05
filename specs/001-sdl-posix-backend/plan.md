# Implementation Plan: SDL Pixel Backend for POSIX

**Branch**: `001-sdl-posix-backend` | **Date**: 2026-06-16 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-sdl-posix-backend/spec.md`

## Summary

Add a third presentation backend — an SDL pixel window — so Linux and macOS
users get the authentic 640×400 VGA editor window (with mouse) that Windows
users already get from the Win32 backend, instead of only the truecolor terminal
fallback. The work slots behind the existing `screen_backend_t` vtable in
`it_screen.h`, reuses the shared backend-neutral `Screen_Rasterize` (cells →
640×400 RGB) and glyph/palette data verbatim, and mirrors `it_screen_win32.c`'s
behaviour (key mapping, `ITK_QUIT` on close, mouse cell+pixel reporting). SDL is
an **optional** build dependency: the `itplay` player and any build without SDL
are unaffected, and the terminal stays the no-deps fallback (including headless
POSIX). No engine, pattern, or UI-layout code is touched.

## Technical Context

**Language/Version**: C11 (project-wide requirement; `_Static_assert`,
`/std:c11`).

**Primary Dependencies**: **SDL2** (new, optional, POSIX-only link) for window +
input; reuses the existing shared `it_screen.c` rasterizer and `it_vgadata.c`
glyph/palette/box data. Audio path (miniaudio) is untouched.

**Storage**: N/A (no new persisted state; `ited.cfg` prefs unaffected).

**Testing**: `tests/test_pattern.c` audio-determinism regression (must remain
`IDENTICAL` for all four testdata modules — trivially, since no engine code
changes, but run as the gate); `ITED_SHOT` pixel-exact BMP captured on POSIX
must be byte-identical to the Windows capture for the same screen/module;
`ITED_SELFTEST` scripted smoke; manual window verification on Linux and macOS.

**Target Platform**: Linux (X11/Wayland via SDL) and macOS (Cocoa via SDL).
Windows is unaffected and keeps the Win32 backend.

**Project Type**: Native cross-platform desktop application (tracker editor)
layered over the ported IT engine.

**Performance Goals**: Present the 80×50 cell buffer at interactive rates each
editor loop iteration with no perceptible input latency; integer-scaled 640×400
→ window (2× default), letterboxed via SDL logical size.

**Constraints**: SDL strictly optional — no new *required* dependency for
`itplay` or for non-SDL `ited` builds; must not alter audio determinism; the
640×400 logical mapping must keep `ITED_SHOT` output byte-identical across
platforms; single-threaded UI (SDL inited/pumped on the main thread, as Win32
is), audio stays on its own miniaudio thread.

**Scale/Scope**: One new backend source (`src/it_screen_sdl.c`, ~180 lines
mirroring the Win32 backend), one `extern` declaration in `it_screen.h`, ~10–15
lines of selection logic in `Screen_Init` (`it_screen.c`), and a guarded CMake
option. No changes to engine, pattern, loader, or any `Screen_*` drawing/
rasterizing code.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Engine fidelity (I) + Determinism (III)** — ✅ PASS. No engine/pattern code
  (`it_music.c`, `it_effects.c`, `it_tables.c`, `it_driver.c`, `it_load.c`,
  `it_structs.h`, `it_pattern.c`) is touched. The determinism regression is run
  as a gate and is expected to stay `IDENTICAL`.
- **Authentic data (II)** — ✅ PASS. No UI layout/color/glyph is introduced or
  eyeballed; the backend reuses the existing `Screen_Rasterize` output and
  `it_vgadata.c` data verbatim. `it_vgadata.c` is not edited.
- **One real engine (IV)** — ✅ PASS. No engine structures or entry points are
  altered; this is presentation/input only.
- **Portability (V)** — ✅ PASS. This feature *is* an instance of Principle V:
  the new platform code lives entirely behind the `screen_backend_t` vtable; the
  cell buffer, control-code renderer, box drawing and rasterizer remain shared
  and untouched; `external/miniaudio.h` is not edited; everything stays C11.

**Result**: All gates pass. No violations → Complexity Tracking is empty.

## Project Structure

### Documentation (this feature)

```text
specs/001-sdl-posix-backend/
├── plan.md              # This file (/speckit-plan command output)
├── research.md          # Phase 0 output (SDL2 vs SDL3, selection, pixel format)
├── data-model.md        # Phase 1 output (backend, input-event, pixel mappings)
├── quickstart.md        # Phase 1 output (build + verify on Linux/macOS)
├── contracts/
│   └── screen-backend.md # Phase 1 output (vtable + selection + pixel contract)
├── checklists/
│   └── requirements.md  # Spec quality checklist (already present)
└── tasks.md             # Phase 2 output (/speckit-tasks — NOT created here)
```

### Source Code (repository root: `it26/`)

```text
it26/
├── CMakeLists.txt              # ADD: optional SDL2 discovery + ITED_SDL option;
│                               #      link SDL2 + compile it_screen_sdl.c when enabled
├── src/
│   ├── it_screen.h             # ADD: extern const screen_backend_t Screen_BackendSDL
│   │                           #      (guarded by HAVE_SDL); doc the selection rule
│   ├── it_screen.c             # EDIT: Screen_Init selection — POSIX prefers SDL when
│   │                           #       built-in + display present + !ITED_TERM, else term
│   ├── it_screen_sdl.c         # NEW: SDL2 backend (init/uninit/present/key/mouse),
│   │                           #      mirrors it_screen_win32.c; uses Screen_Rasterize
│   ├── it_screen_win32.c       # UNCHANGED (Windows backend)
│   └── it_vgadata.c            # UNCHANGED (generated; do not edit)
└── tests/
    └── test_pattern.c          # UNCHANGED (run as the determinism gate)
```

**Structure Decision**: Single existing native project (`it26/`). The feature
adds exactly one source file plus a guarded extern and a small selection branch,
following the established two-backend pattern (`Screen_BackendWin32` /
`Screen_BackendTerm`). No new directories or modules; the SDL backend is a peer
of the Win32 backend behind the same vtable.

## Complexity Tracking

> No constitution violations. Section intentionally empty.
