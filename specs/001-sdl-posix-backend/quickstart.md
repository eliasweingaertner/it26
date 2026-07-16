# Quickstart: Verify the SDL POSIX Backend

Validation guide proving the feature works end-to-end. Run on **Linux** and
**macOS** (the new targets); the Windows and no-SDL paths are regression-checked
too. Implementation detail lives in `tasks.md`; this is the run/verify guide.

## Prerequisites

- A C11 toolchain + CMake ≥ 3.16 (already required by the project).
- **SDL2 development package** on the build machine:
  - Debian/Ubuntu: `sudo apt install libsdl2-dev`
  - Fedora: `sudo dnf install SDL2-devel`
  - macOS (Homebrew): `brew install sdl2`
- A test module (e.g. `testdata/itdemo.it`).
- A Windows-captured reference BMP for the parity check (or capture once on
  Windows: `ITED_SHOT=win.bmp ITED_SHOT_SCREEN=1 ited testdata/itdemo.it`).

## Build

```sh
cd ittrack
cmake -B build && cmake --build build --config Release
```

Expected: configuration prints that SDL2 was found and the SDL backend is
enabled; `ited`, `itplay`, `test_pattern` all build.

## Scenario 1 — Authentic pixel window (User Story 1 / SC-001)

```sh
./build/ited testdata/itdemo.it
```

**Expected**: a 1280×800 window opens showing the editor rendered with the real
VGA font, Camouflage palette and box glyphs (not Unicode approximations). F1–F12
switch screens; ESC opens the menu. Closing the window exits `ited` cleanly.

## Scenario 2 — Keyboard & mouse parity (User Story 2 / SC-003)

With the window focused:

- Press F2/F3/F4/F11/F12, arrows, PgUp/PgDn, Home/End, Ins/Del, Tab/Shift-Tab,
  and note keys → behaviour matches the Windows window.
- Drag a thumbbar (e.g. on F12) → value tracks the pointer pixel-precisely.
- Click a menu item and a pattern cell → focus/selection/cursor update as on
  Windows.

## Scenario 3 — Backend selection (User Story 3 / SC-004)

```sh
ITED_TERM=1 ./build/ited testdata/itdemo.it     # → truecolor terminal backend
./build/ited testdata/itdemo.it                 # (with display) → SDL window
# headless (no $DISPLAY/$WAYLAND_DISPLAY, e.g. bare SSH):
unset DISPLAY WAYLAND_DISPLAY; ./build/ited testdata/itdemo.it
#   → prints a one-line note and runs the terminal backend, no crash
```

## Scenario 4 — Pixel-exact parity with Windows (SC-002)

```sh
ITED_SHOT=posix.bmp ITED_SHOT_SCREEN=1 ./build/ited testdata/itdemo.it
cmp posix.bmp win.bmp        # exit 0 → byte-identical
```

**Expected**: `posix.bmp` is byte-identical to the Windows capture of the same
screen/module (the shared rasterizer guarantees this). Try several
`ITED_SHOT_SCREEN` values (0..8).

## Scenario 5 — Audio determinism gate (SC-005, constitution III)

```sh
cmake --build build --target test_pattern
for m in beyond_network itdemo quests_end synthscape_filters; do
  ./build/test_pattern testdata/$m.it
done
```

**Expected**: every module reports `IDENTICAL` (matching the known-good FNV-1a
hashes). This must hold since no engine/pattern code changed.

## Scenario 6 — Build matrix (SC-006 / FR-009)

```sh
cmake -B build-nosdl -DITED_SDL=OFF && cmake --build build-nosdl   # no SDL link
# itplay never links SDL in any build:
ldd ./build/itplay | grep -i sdl    # → no output
```

**Expected**: `ITED_SDL=OFF` builds `ited` with the terminal backend only and no
SDL dependency; `itplay` never references SDL. On a machine without SDL2 the
default configure also succeeds with SDL disabled.

## Non-interactive smoke (no window needed)

```sh
ITED_SELFTEST=1 ./build/ited testdata/itdemo.it   # scripted editor smoke test
ITED_DUMP=1 ./build/ited testdata/itdemo.it       # writes screen_dump.txt
```

## Done criteria

- Scenarios 1–4 pass on both Linux and macOS.
- Scenario 5 (`IDENTICAL` ×4) and Scenario 6 (build matrix) pass.
- No regression to the Windows Win32 backend or the terminal backend.
