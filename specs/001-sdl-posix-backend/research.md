# Phase 0 Research: SDL Pixel Backend for POSIX

All decisions below resolve the open questions for the SDL backend. There are no
remaining `NEEDS CLARIFICATION` items.

## 1. SDL version — SDL2

- **Decision**: Target **SDL2** (`SDL.h`, `find_package(SDL2)`).
- **Rationale**: SDL2 is ubiquitously packaged on every target distro and on
  macOS (Homebrew `sdl2`), maps directly onto the small surface we need
  (window + streaming texture + renderer + keyboard/text/mouse events), and is
  the lowest-friction optional dependency. The backend uses only long-stable
  SDL2 APIs, so an SDL3 port later is mechanical if ever wanted.
- **Alternatives considered**:
  - *SDL3* — newer (cleaner init/event API) but less universally packaged as of
    now; no capability we need that SDL2 lacks. Rejected to minimise install
    friction for users.
  - *Raw X11 / Cocoa / Wayland* — two+ platform backends to write and maintain
    versus SDL's one; defeats the point of an optional cross-platform lib.
  - *GLFW / raylib* — GLFW has no simple 2D blit path (would force GL); raylib is
    heavier and opinionated. SDL's streaming texture is the natural fit for a
    pre-rasterized 640×400 framebuffer.

## 2. Build integration — optional, off unless SDL2 is found

- **Decision**: Add a CMake option `ITED_SDL` (default `ON` on non-Windows).
  When `ITED_SDL` is on, `find_package(SDL2 QUIET)`; if found, compile
  `src/it_screen_sdl.c` into `ited`, link `SDL2::SDL2`, and define `HAVE_SDL`.
  If SDL2 is not found (or `ITED_SDL=OFF`, or Windows), `HAVE_SDL` stays
  undefined and the build is exactly as today.
- **Rationale**: Satisfies FR-009 / constitution Principle V — `itplay` and
  SDL-less `ited` builds gain no required dependency. `HAVE_SDL` guards both the
  new source and the `extern`/selection code, so the tree compiles cleanly in
  every combination (Windows, POSIX+SDL, POSIX-no-SDL).
- **Alternatives considered**: Always-require SDL on POSIX (rejected — breaks
  no-deps builds and headless/CI); runtime `dlopen` of SDL (rejected —
  needless complexity versus a compile-time guard).

## 3. Backend selection & headless fallback

- **Decision**: Extend `Screen_Init` selection (currently `ITED_TERM` →
  terminal, else Win32 on Windows / terminal on POSIX). New POSIX order:
  1. If `ITED_TERM` is set and non-zero → terminal backend (unchanged override).
  2. Else if `HAVE_SDL` and the SDL backend's `init()` succeeds → SDL backend.
  3. Else → terminal backend (fallback).
  Headless detection is delegated to SDL: the SDL `init()` returns failure when
  `SDL_Init(SDL_INIT_VIDEO)` / `SDL_CreateWindow` fails (no display), and
  `Screen_Init` then falls through to the terminal with a one-line stderr note.
- **Rationale**: Matches FR-005 and the spec's three selection scenarios
  (display → SDL; `ITED_TERM=1` → terminal; headless → terminal, no crash).
  Letting SDL's own init be the display probe avoids brittle `$DISPLAY` /
  `$WAYLAND_DISPLAY` / macOS sniffing and is correct on all three.
- **Alternatives considered**: Manually inspecting `$DISPLAY`/`$WAYLAND_DISPLAY`
  (rejected — misses macOS, fragile under SSH X-forwarding); making terminal the
  POSIX default and SDL opt-in via env var (rejected — spec wants the authentic
  window to be the default when a display exists).

## 4. Pixel format — XRGB8888 streaming texture

- **Decision**: Create an `SDL_PIXELFORMAT_ARGB8888` (alpha ignored) streaming
  texture of 640×400 and copy `Screen_Rasterize`'s output into it each present.
- **Rationale**: `Screen_Rasterize` writes `0x00RRGGBB` `uint32` pixels — exactly
  the layout the Win32 backend feeds `StretchDIBits` as 32-bit `BI_RGB`. On the
  little-endian targets (x86-64, Apple Silicon), a `uint32` `0x00RRGGBB` matches
  SDL's `ARGB8888` byte order, so the existing rasterizer output is consumed
  **unmodified** — guaranteeing `ITED_SHOT` (which uses the same rasterizer)
  stays byte-identical to the on-screen image (SC-002).
- **Alternatives considered**: `SDL_Surface` + `SDL_BlitScaled` (rejected — CPU
  blit, no free GPU scaling, more code); per-pixel format conversion (rejected —
  unnecessary on little-endian and would risk diverging from `ITED_SHOT`).

## 5. Scaling & mouse → logical-pixel mapping

- **Decision**: Open the window at 1280×800 (2× of 640×400) and call
  `SDL_RenderSetLogicalSize(renderer, 640, 400)`. Render the texture with
  `SDL_RenderCopy` (full target). Map mouse events to logical pixels via
  `SDL_RenderWindowToLogical` (SDL ≥ 2.0.18); if compiling against older SDL2,
  fall back to scaling window coords by the renderer's logical scale/viewport.
- **Rationale**: `SDL_RenderSetLogicalSize` gives free integer/letterboxed
  scaling and resize handling, and `RenderWindowToLogical` returns coordinates in
  the 0–639 / 0–399 space the editor expects — so `it_mouse_t.px/py` and the
  `/8` cell coordinates are computed exactly as the Win32 backend does
  (`px/SCALE`, then `/8`). This satisfies the resize and high-DPI edge cases
  (logical size stays 640×400 regardless of physical window/DPI).
- **Alternatives considered**: Fixed non-resizable window (rejected — spec
  edge case requires graceful resize); manual scale math only (kept as the
  pre-2.0.18 fallback, but the SDL helper is preferred where available).

## 6. Keyboard & text input mapping

- **Decision**: Use `SDL_KEYDOWN` for non-text keys → `ITK_*` (a `MapSDLKey`
  switch mirroring `MapVKey`: arrows, PgUp/PgDn, Home/End, Ins/Del, Esc, Enter,
  Backspace, F1–F12, and Tab/Shift-Tab via `SDL_GetModState() & KMOD_SHIFT`),
  and `SDL_TEXTINPUT` for printable ASCII 32–126 (so layout/shift produce the
  right characters, exactly as Win32 uses `WM_CHAR`). Window close
  (`SDL_QUIT` / `SDL_WINDOWEVENT_CLOSE`) → `ITK_QUIT`. Events are buffered in the
  same fixed-size ring-queue pattern the Win32 backend uses (`PushKey`).
- **Rationale**: Reproduces the Win32 backend's exact key semantics (FR-003,
  SC-003): special keys via keycode, printable characters via the text-input
  event so non-US layouts and shifted symbols work. The ring queue keeps the
  non-blocking `Key_Get` contract identical across backends.
- **Alternatives considered**: Deriving ASCII from `SDL_KEYDOWN` keycodes
  (rejected — breaks shifted/locale characters; `SDL_TEXTINPUT` is the correct
  source); blocking event wait (rejected — `Key_Get` is non-blocking by
  contract).

## 7. Threading & event pumping

- **Decision**: Single-threaded UI, identical to Win32. `SDL_Init(VIDEO)` and all
  SDL calls happen on the main thread; `present`, `key`, and `mouse` each pump
  the SDL event queue (`SDL_PumpEvents`/`SDL_PollEvent`) just as the Win32
  backend calls `PumpMessages`. Audio remains on miniaudio's own thread,
  unchanged; `SDL_INIT_AUDIO` is **not** requested.
- **Rationale**: Preserves the proven single-threaded editor loop and keeps SDL
  away from the audio path (no determinism impact). Initing only `SDL_INIT_VIDEO`
  avoids any interaction with miniaudio.
- **Alternatives considered**: A dedicated SDL event thread (rejected —
  unnecessary, and SDL video events must be pumped on the thread that created the
  window on macOS).

## Summary of decisions

| Topic | Decision |
|-------|----------|
| Library | SDL2 (optional, compile-time `HAVE_SDL`) |
| Build | `ITED_SDL` CMake option, default ON on non-Windows; auto-off if SDL2 absent |
| Selection | `ITED_TERM` override → SDL if built & display present → terminal fallback |
| Pixel format | `ARGB8888` streaming texture, rasterizer output used unmodified (little-endian) |
| Scaling/mouse | 2× window + `SDL_RenderSetLogicalSize(640,400)`; mouse via `RenderWindowToLogical` |
| Keys | `SDL_KEYDOWN`→`ITK_*`, `SDL_TEXTINPUT`→ASCII, close→`ITK_QUIT`, ring queue |
| Threading | Single-threaded main-thread SDL; video-only init; audio untouched |
