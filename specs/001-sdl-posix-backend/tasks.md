---
description: "Task list for SDL Pixel Backend for POSIX"
---

# Tasks: SDL Pixel Backend for POSIX

**Input**: Design documents from `/specs/001-sdl-posix-backend/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/screen-backend.md, quickstart.md

**Tests**: No unit-test framework is requested for this feature. The project's
verification mechanism is the `tests/test_pattern.c` audio-determinism harness,
`ITED_SHOT` pixel-exact BMP parity, and manual/scripted window checks. These are
captured as **verification tasks** at the end of each story and in Polish, not as
failing-first TDD tests.

**Organization**: Tasks are grouped by user story. All source paths are under the
`ittrack/` repository.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: US1, US2, US3 (maps to spec.md user stories)

## Path Conventions

Native single project rooted at `ittrack/`. Sources in `ittrack/src/`, tests in
`ittrack/tests/`, docs in `ittrack/docs/`, build config `ittrack/CMakeLists.txt`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare the build environment for an optional SDL2 dependency.

- [X] T001 ✅ VERIFIED on Linux (Ubuntu 24.04, GCC 13.3, CMake 3.28, SDL2 2.30 via `libsdl2-dev`). `cmake -B build-linux && cmake --build build-linux` succeeds; `ited` links `libSDL2-2.0.so.0`, `it_screen_sdl.c` compiles with `HAVE_SDL`, and `itplay`/`test_pattern` link no SDL.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Wire an optional, guarded SDL backend skeleton into the build, the
header, and backend selection so it is reachable and the tree compiles in all
configurations. No behaviour yet.

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T002 Add the optional SDL build to `ittrack/CMakeLists.txt`: introduce option `ITED_SDL` (default `ON` for non-Windows); when enabled, `find_package(SDL2 QUIET)`, and if found append `src/it_screen_sdl.c` to `EDITOR_SOURCES`, `target_link_libraries(ited SDL2::SDL2)`, and `target_compile_definitions(ited PRIVATE HAVE_SDL)`. Leave `itplay`/`test_pattern` and the Windows path untouched. (Contract C4)
- [X] T003 Create `ittrack/src/it_screen_sdl.c` skeleton wrapped in `#ifdef HAVE_SDL`: include `<SDL.h>` and `it_screen.h`; declare file-local state (`SDL_Window *Wnd`, `SDL_Renderer *Ren`, `SDL_Texture *Tex`, `int KeyQueue[64]`, `KeyHead`, `KeyTail`, `WantQuit`, `MousePX`, `MousePY`, `MouseB`) and a `PushKey` ring helper (mirroring `it_screen_win32.c`); add stub `init/uninit/present/key/mouse` and `const screen_backend_t Screen_BackendSDL = { ... }`.
- [X] T004 [P] Add `extern const screen_backend_t Screen_BackendSDL;` under `#ifdef HAVE_SDL` to `ittrack/src/it_screen.h`, and update the backend-selection doc comment to mention the SDL backend. (Contract C1)
- [X] T005 [P] Add a minimal SDL branch to `Screen_Init` in `ittrack/src/it_screen.c` so that on POSIX, when `HAVE_SDL` is defined and `ITED_TERM` is unset, `Screen_BackendSDL` is selected (full ordering/fallback hardened in US3). Keep the Windows branch unchanged.

**Checkpoint**: `ited` builds and links with SDL on POSIX (backend stubbed), builds without SDL unchanged, and `itplay`/`test_pattern` are unaffected.

---

## Phase 3: User Story 1 - Authentic pixel window on Linux/macOS (Priority: P1) 🎯 MVP

**Goal**: A real 640×400 VGA pixel window opens on Linux/macOS, rendered from the
shared rasterizer with the authentic font/palette/glyphs, and closes cleanly.

**Independent Test**: Run `ited testdata/itdemo.it` on Linux and macOS → authentic
window appears and renders every screen; closing it exits cleanly; `ITED_SHOT`
BMP is byte-identical to the Windows capture.

**Note**: All tasks below edit the single file `ittrack/src/it_screen_sdl.c`, so
they are sequential (no `[P]`).

- [X] T006 [US1] Implement `init()` in `ittrack/src/it_screen_sdl.c`: `SDL_Init(SDL_INIT_VIDEO)` (video only — never audio), create a 1280×800 window, create a renderer, `SDL_RenderSetLogicalSize(Ren, 640, 400)`, create a 640×400 `SDL_PIXELFORMAT_ARGB8888` streaming texture, `SDL_StartTextInput()`; return `0` (not abort) on any failure so the caller can fall back. (Contract C1, research §2/§4/§5)
- [X] T007 [US1] Implement `uninit()` in `ittrack/src/it_screen_sdl.c`: destroy texture, renderer, window (guard NULLs), `SDL_QuitSubSystem(SDL_INIT_VIDEO)`; make it idempotent.
- [X] T008 [US1] Implement the SDL event pump in `ittrack/src/it_screen_sdl.c` (a `PumpEvents` helper called by present/key/mouse): handle `SDL_QUIT` and `SDL_WINDOWEVENT`/`SDL_WINDOWEVENT_CLOSE` → set `WantQuit` and `PushKey(ITK_QUIT)` (mirrors `WM_CLOSE`).
- [X] T009 [US1] Implement `present()` in `ittrack/src/it_screen_sdl.c`: call `Screen_Rasterize` into a static 640×400 `uint32_t` buffer, `SDL_UpdateTexture` (or lock/copy/unlock) with **no per-pixel conversion**, then `SDL_RenderClear`/`SDL_RenderCopy`/`SDL_RenderPresent`, and pump events. (Contract C3)
- [X] T010 [US1] Implement a minimal `key()` in `ittrack/src/it_screen_sdl.c`: pump events, return `ITK_QUIT` when `WantQuit`, else dequeue the ring (`ITK_NONE` if empty) — enough that window-close exits the editor loop cleanly.
- [X] T011 [US1] ✅ VERIFIED on Linux (X11). Window opens at 1280×800 with the authentic VGA font, Camouflage palette and box glyphs; F1–F12 switch screens; ESC opens the menu; closing the window exits cleanly (rc=0). **Pixel parity:** POSIX vs Windows (`ited.exe` via wine) BMPs were captured for screens 0–8 and compared with `cmp`. The shared rasterizer output is byte-identical; the *only* divergences are two platform-specific **content** fields rendered by `it_editor.c` (out of this feature's scope): `FreeMem` (Win32 `free_mem_k()` reports real RAM, capped 999999k; POSIX hardcodes 65536k) and the cwd path string (native vs wine `Z:\`). E.g. screen 0 differed in exactly 363 bytes, all confined to text row 6 / cols 71–77 (the FreeMem digits); everything else byte-identical. ⚠️ Note: the spec's literal "byte-identical BMP" cannot hold against a real Windows host because of `free_mem_k()`; the *rendering path* added by this feature is provably pixel-exact.

**Checkpoint**: MVP — the authentic window works and matches Windows pixel-for-pixel; input is not yet wired (US2).

---

## Phase 4: User Story 2 - Full keyboard and mouse parity (Priority: P2)

**Goal**: Every key and mouse interaction in the SDL window behaves exactly as in
the Win32 window.

**Independent Test**: With the window focused, exercise all keys (F1–F12,
navigation, editing, Tab/Shift-Tab, note entry) and the mouse (drag a thumbbar,
click menu items/list rows/pattern grid) → identical behaviour to Windows.

**Note**: T012–T014 edit the single file `ittrack/src/it_screen_sdl.c`
(sequential); T015 is verification.

- [X] T012 [US2] Implement `MapSDLKey()` and wire it into the event pump in `ittrack/src/it_screen_sdl.c`: on `SDL_KEYDOWN` map arrows, `PAGEUP/PAGEDOWN`, `HOME/END`, `INSERT/DELETE`, `ESCAPE`, `RETURN`/`KP_ENTER`, `BACKSPACE`, `F1..F12` → the matching `ITK_*`; map `SDLK_TAB` → `ITK_SHIFT_TAB` if `SDL_GetModState() & KMOD_SHIFT` else `ITK_TAB`; `PushKey` the result. (data-model key map)
- [X] T013 [US2] Handle `SDL_TEXTINPUT` in the event pump in `ittrack/src/it_screen_sdl.c`: for each printable byte in 32..126, `PushKey` it as ASCII (equivalent to Win32 `WM_CHAR`), so shifted/locale characters are correct.
- [X] T014 [US2] Implement `mouse()` and its events in `ittrack/src/it_screen_sdl.c`: track `SDL_MOUSEMOTION`/`SDL_MOUSEBUTTONDOWN`(left)/`SDL_MOUSEBUTTONUP`(left); convert window→logical via `SDL_RenderWindowToLogical` (fallback to renderer scale/viewport math for SDL < 2.0.18); clamp to 0–639/0–399 into `MousePX/MousePY`; on left-down set `MouseB=1` and `PushKey(ITK_MOUSE)`, on up clear it; in `mouse()` fill `m->px/py`, `m->x=px/8`, `m->y=py/8`, `m->b` — identical to `W32_Mouse`. (Contract C1, data-model mouse map)
- [X] T015 [US2] ✅ VERIFIED on Linux (X11). Keyboard parity confirmed: F-keys, arrows, Tab/Shift-Tab focus changes, and piano note entry all behave as on Windows. Mouse coordinate mapping confirmed **exact** via a probe (`SDL_RenderWindowToLogical`: non-HiDPI here, window→logical→cell maps 1:1) and an in-app click log: every click mapped to the precise cell. A reported "can't jump between channels with the mouse" was diagnosed as **not a backend bug** — the click log showed all clicks landed at cell-rows 7–14 (the upper info/header area, incl. the "Channel 0X" header labels), above the note grid; `pattern_click()` (shared with Win32) only switches `CurChan` for clicks on note cells at screen row ≥15. Clicking an actual note cell moves the cursor/channel correctly, identical to the Win32 backend.

**Checkpoint**: The SDL window is fully operable — keyboard and mouse match the Win32 backend.

---

## Phase 5: User Story 3 - Predictable backend selection (Priority: P3)

**Goal**: Robust backend selection — SDL when a display exists, `ITED_TERM=1`
forces the terminal, headless falls back gracefully — and SDL stays an optional
dependency across the build matrix.

**Independent Test**: Run `ited` with a display, with `ITED_TERM=1`, and headless;
confirm the chosen backend in each, no crash, and a clear fallback message. Build
with SDL, with `ITED_SDL=OFF`, and confirm `itplay` never links SDL.

- [X] T016 [P] [US3] Finalize the selection order in `Screen_Init` in `ittrack/src/it_screen.c` (POSIX branch): (1) `getenv("ITED_TERM")` set and non-`"0"` → `Screen_BackendTerm`; (2) else `HAVE_SDL` and `Screen_BackendSDL.init()` succeeds → SDL; (3) else `Screen_BackendTerm` after one short `stderr` note (e.g. "ited: no display; using terminal backend"). Ensure `init()` is invoked exactly once (SDL init is the display probe). (Contract C2)
- [X] T017 [P] [US3] Confirm/finish the optional-dependency behaviour in `ittrack/CMakeLists.txt`: `ITED_SDL` default `ON` on non-Windows and auto-resolves to `HAVE_SDL` undefined when SDL2 is absent, when `ITED_SDL=OFF`, or on `WIN32`; verify `itplay` and `test_pattern` link no SDL in any configuration. (Contract C4)
- [X] T018 [US3] ✅ VERIFIED on Linux. display → SDL (`x11`) window; `ITED_TERM=1` → terminal backend (renders truecolor UI, no SDL probe); `-DITED_SDL=OFF` builds `ited` with no SDL (`HAVE_SDL` undefined, `ldd` clean); `itplay`/`test_pattern` link no SDL in either config. **Bug found & fixed during headless check:** SDL 2.30 ships an `offscreen` video driver, so `SDL_Init(SDL_INIT_VIDEO)` *succeeds* with no display (selects `offscreen`) instead of failing — which wrongly selected the SDL backend and never fell back to the terminal (no window, no note; a headless box would appear to hang). Fixed in `it_screen_sdl.c` `SDL_BInit()`: after `SDL_Init`, reject `SDL_GetCurrentVideoDriver()` of `"offscreen"`/`"dummy"` (uninit + return 0) so the caller falls back. After the fix, headless (`DISPLAY=:99` or unset DISPLAY/WAYLAND) correctly prints `ited: no display; using terminal backend` and runs the terminal backend with no crash.

**Checkpoint**: All three user stories are independently functional; SDL is fully optional.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Constitution gates and project hygiene that span all stories.

- [X] T019 [P] Determinism gate (constitution III): **PASSED on Windows (MSVC)** — all four modules `IDENTICAL` with the known-good hashes (`a43e6f19`, `1e6a5383`, `bb5b15bc`, `d561c207`). build `test_pattern` and run it for all four testdata modules (`beyond_network`, `itdemo`, `quests_end`, `synthscape_filters`); confirm every result is `IDENTICAL` with the known-good FNV-1a hashes. (quickstart Scenario 5)
- [X] T020 [P] Non-regression: **Windows confirmed** — `ited` (Win32 backend) and `test_pattern` build clean with MSVC (exit 0, no warnings); `it_screen_win32.c` unchanged. POSIX terminal-via-`ITED_TERM=1` path is unchanged by inspection (only `Screen_Init` selection edited; terminal backend code untouched) — confirm on a POSIX run as part of T022.
- [X] T021 [P] Update docs: in `ittrack/docs/HANDOFF.md` move the SDL backend out of §6 "next phase" and into the done list (note it's behind the vtable, video-only init, optional `ITED_SDL` build); in `ittrack/README.md` document the three backends and the SDL2 build/run instructions for Linux/macOS.
- [~] T022 ✅ LINUX DONE / ⏳ macOS PENDING. Full `quickstart.md` validation run end-to-end on **Linux** (Ubuntu 24.04 / GCC 13.3 / SDL2 2.30, X11) — Scenarios 1–6 all pass: authentic window (1), keyboard+mouse parity (2), backend selection incl. the headless `offscreen`-driver fix (3), pixel parity modulo the platform-specific `FreeMem`/cwd-path content fields (4), determinism 4× IDENTICAL (5), build matrix incl. `itplay` no-SDL (6). **macOS (Cocoa) pass still to run on Mac hardware** before the feature is marked fully done across both target platforms.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies — start immediately.
- **Foundational (Phase 2)**: Depends on Setup — **BLOCKS all user stories**.
- **User Stories (Phase 3–5)**: All depend on Foundational completion.
- **Polish (Phase 6)**: Depends on the desired user stories being complete.

### User Story Dependencies

- **US1 (P1)**: After Foundational. The MVP; no dependency on US2/US3.
- **US2 (P2)**: After Foundational. Extends the same backend file's event pump
  (shares the `PushKey` ring from Foundational/US1) but is independently testable.
- **US3 (P3)**: After Foundational. Hardens the selection/build that Foundational
  stubbed; independently testable. Best validated after US1 (so the SDL window
  actually opens during the display→SDL check).

### Within Each User Story

- US1: T006 → T007 → T008 → T009 → T010 (same file, ordered) → T011 (verify).
- US2: T012 → T013 → T014 (same file, ordered) → T015 (verify).
- US3: T016 ∥ T017 (different files) → T018 (verify).

### Parallel Opportunities

- Foundational: T004 (`it_screen.h`) and T005 (`it_screen.c`) run in parallel
  after T003 creates the symbol; T002 (CMake) is independent of T003–T005.
- US3: T016 (`it_screen.c`) and T017 (`CMakeLists.txt`) are different files — `[P]`.
- Polish: T019, T020, T021 are independent (`[P]`); T022 runs last.
- US1, US2, US3 all touch overlapping files (`it_screen_sdl.c` for US1/US2), so
  US1 and US2 are best done by one developer in sequence; US3 (different files)
  can proceed in parallel with US2.

---

## Parallel Example: Foundational Phase

```bash
# After T003 creates Screen_BackendSDL, these two edit different files:
Task: "T004 Add extern Screen_BackendSDL to ittrack/src/it_screen.h"
Task: "T005 Add minimal SDL selection branch to ittrack/src/it_screen.c"
# T002 (CMakeLists.txt) can run alongside all of the above.
```

## Parallel Example: US3 + Polish

```bash
# US3 — different files:
Task: "T016 Finalize selection order in ittrack/src/it_screen.c"
Task: "T017 Confirm ITED_SDL optional build in ittrack/CMakeLists.txt"

# Polish — independent verifications/docs:
Task: "T019 Run test_pattern determinism gate (4 modules IDENTICAL)"
Task: "T020 Verify Win32 + terminal backends non-regressed"
Task: "T021 Update HANDOFF.md and README.md"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Phase 1: Setup (install SDL2 dev, confirm baseline build).
2. Phase 2: Foundational (CMake option, backend skeleton, header extern, minimal
   selection) — **blocks everything**.
3. Phase 3: US1 — implement init/uninit/present + close handling.
4. **STOP and VALIDATE**: authentic window renders on Linux/macOS and matches the
   Windows `ITED_SHOT` byte-for-byte. This is a demonstrable MVP.

### Incremental Delivery

1. Setup + Foundational → backend reachable, tree compiles in all configs.
2. US1 → authentic window (MVP) → demo.
3. US2 → keyboard + mouse parity → demo.
4. US3 → robust selection + optional-build matrix → demo.
5. Polish → determinism gate, non-regression, docs, full quickstart.

### Notes

- `[P]` = different files, no incomplete dependencies.
- US1/US2 share `it_screen_sdl.c`; keep them sequential to avoid conflicts.
- This feature touches **no** engine/pattern/UI-data code, so the determinism
  gate (T019) is expected to pass trivially — but it is a required, non-negotiable
  checkpoint per constitution Principle III before the feature is "done".
- Do not edit `it_screen_win32.c`, `it_vgadata.c`, `Screen_Rasterize`, or
  `external/miniaudio.h`.
