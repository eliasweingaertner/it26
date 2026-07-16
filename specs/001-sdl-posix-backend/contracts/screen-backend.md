# Contract: SDL Backend ↔ Screen Layer (POSIX)

The interface this feature must satisfy. The SDL backend is a peer of
`Screen_BackendWin32` / `Screen_BackendTerm` behind the existing
`screen_backend_t` vtable; the screen layer and the build system are the two
"consumers" of the contract.

## C1 — Vtable contract (`screen_backend_t` in `src/it_screen.h`)

The backend MUST provide a `const screen_backend_t Screen_BackendSDL` with all
five members non-NULL (`mouse` non-NULL because the SDL window supports a mouse):

```c
typedef struct screen_backend_t {
    int  (*init)(void);                          /* 1 = ok, 0 = unavailable   */
    void (*uninit)(void);                         /* idempotent teardown        */
    void (*present)(const screen_cell_t *cells);  /* rasterize + blit + pump    */
    int  (*key)(void);                            /* non-blocking, ITK_*/ASCII  */
    void (*mouse)(it_mouse_t *m);                 /* cell + logical-pixel + btn */
} screen_backend_t;
```

Guarantees the consumer (`it_screen.c`) relies on:

- `init()` returns `0` (never aborts the process) when the window cannot be
  created — this is the signal to fall back to the terminal.
- `key()` returns `ITK_NONE` when nothing is pending, the right `ITK_*` for
  special keys, printable ASCII (32–126) for text, `ITK_MOUSE` on left-press,
  and `ITK_QUIT` after the window is closed.
- `mouse()` fills `it_mouse_t` with `px,py` clamped to 0–639/0–399, `x=px/8`,
  `y=py/8`, `b` bit0 = left button — **identical semantics to `W32_Mouse`**.
- `present()` reads the shared cell buffer via `Screen_Rasterize` (its `cells`
  argument is ignored, as in Win32) and MUST pump the event queue.

New declaration in `it_screen.h`:

```c
#ifdef HAVE_SDL
extern const screen_backend_t Screen_BackendSDL;
#endif
```

## C2 — Backend selection contract (`Screen_Init` in `src/it_screen.c`)

On POSIX (`#ifndef _WIN32`), the selection MUST be:

| Condition (first match wins) | Selected backend |
|------------------------------|------------------|
| `getenv("ITED_TERM")` set and non-`"0"` | `Screen_BackendTerm` |
| `HAVE_SDL` defined **and** `Screen_BackendSDL.init()` succeeds | `Screen_BackendSDL` |
| otherwise | `Screen_BackendTerm` |

Notes:

- The Windows branch is unchanged (Win32 unless `ITED_TERM`).
- When `HAVE_SDL` is undefined, the POSIX path is byte-for-byte today's behaviour
  (terminal only) — guaranteeing the no-SDL build is unaffected.
- On fallback after a failed SDL `init()`, emit one short line to `stderr`
  (e.g. `"ited: no display; using terminal backend"`); do not abort.
- `Screen_Init` calls the selected backend's `init()` exactly once (existing
  contract); avoid double-init by having SDL's `init()` be the probe.

## C3 — Pixel-format contract (`Screen_Rasterize`)

- `Screen_Rasterize(uint32_t *px)` fills 640×400 pixels as `0x00RRGGBB`
  (unchanged; shared with Win32 and `Screen_WriteBMP`).
- The SDL backend MUST present these pixels **without per-pixel conversion** on
  little-endian targets, using an `ARGB8888` texture, so that the on-screen image
  equals the `ITED_SHOT` BMP for the same screen/module (cross-platform parity).
- The backend MUST NOT modify `Screen_Rasterize` or `it_vgadata.c`.

## C4 — Build contract (`CMakeLists.txt`)

- Introduce option `ITED_SDL` (default `ON` for non-Windows).
- When enabled, `find_package(SDL2 QUIET)`. If found:
  - add `src/it_screen_sdl.c` to `ited`'s sources,
  - `target_link_libraries(ited SDL2::SDL2)` (or `${SDL2_LIBRARIES}` +
    include dirs for older config-less finds),
  - `target_compile_definitions(ited PRIVATE HAVE_SDL)`.
- If `ITED_SDL=OFF`, SDL2 not found, or `WIN32`: `HAVE_SDL` undefined, sources
  and links exactly as today.
- `itplay` and `test_pattern` MUST NOT link or require SDL under any
  configuration.

## C5 — Regression contract (non-negotiable, constitution III)

- `tests/test_pattern.c` MUST report `IDENTICAL` for all four testdata modules
  after this change (expected trivially, since no engine/pattern code is
  touched). This is a required gate before the feature is "done".

## Contract tests (verification, see quickstart.md)

| ID | Asserts | How |
|----|---------|-----|
| T-C1 | Vtable wired, window opens, keys/mouse work | Run `ited` on Linux & macOS; exercise keys + mouse |
| T-C2a | Display present → SDL chosen | Launch with a display, confirm window |
| T-C2b | `ITED_TERM=1` → terminal | Launch with the var set, confirm terminal |
| T-C2c | Headless → terminal, no crash | Launch with no display, confirm fallback + message |
| T-C3 | On-screen == `ITED_SHOT` | Capture BMP on POSIX, diff vs Windows BMP (byte-identical) |
| T-C4 | Build matrix | Build with SDL, without SDL, and `itplay` (no SDL) |
| T-C5 | Determinism | `test_pattern` all four modules `IDENTICAL` |
