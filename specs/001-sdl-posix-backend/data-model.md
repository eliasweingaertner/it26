# Phase 1 Data Model: SDL Pixel Backend for POSIX

This feature is presentation/input only — it introduces **no persisted data** and
no new engine structures. The "entities" are the in-memory interface objects and
the mappings the backend implements. They are described here so the contract and
tasks are unambiguous.

## Entities

### Presentation backend (`screen_backend_t`)

The existing vtable (`it_screen.h`) the SDL backend must implement. Unchanged
shape; a third instance is added.

| Field | Type | Responsibility for the SDL backend |
|-------|------|------------------------------------|
| `init` | `int (*)(void)` | Init `SDL_INIT_VIDEO`, create window (1280×800), renderer, 640×400 streaming texture; `SDL_StartTextInput()`. Return `1` on success, `0` on failure (no display / SDL error) so `Screen_Init` can fall back. |
| `uninit` | `void (*)(void)` | Destroy texture, renderer, window; `SDL_QuitSubSystem(SDL_INIT_VIDEO)`. Idempotent. |
| `present` | `void (*)(const screen_cell_t *)` | Call `Screen_Rasterize` into the texture's locked pixels (or a static 640×400 buffer then `SDL_UpdateTexture`), `RenderClear`, `RenderCopy`, `RenderPresent`; pump events. Ignores the `cells` arg (rasterizer reads the shared buffer), exactly like `W32_Present`. |
| `key` | `int (*)(void)` | Pump events; return next queued `ITK_*`/ASCII or `ITK_NONE`; `ITK_QUIT` once closed. Non-blocking. |
| `mouse` | `void (*)(it_mouse_t *)` | Pump events; fill clamped `px/py` (0–639/0–399), `x=px/8`, `y=py/8`, `b` = left-button bit. |

New public symbol: `extern const screen_backend_t Screen_BackendSDL;`
(guarded by `#ifdef HAVE_SDL`).

### Cell buffer (`screen_cell_t[80][50]`)

Owned by `it_screen.c`, **shared and unchanged**. The SDL backend never reads it
directly; it goes through `Screen_Rasterize(uint32_t *px)`.

### Input event

Transient, not stored. Produced from SDL events, consumed via `Key_Get` /
`Screen_GetMouse`. Two kinds:

- **Key event** → an `int`: an `ITK_*` enum value (special keys, `ITK_QUIT`,
  `ITK_MOUSE`) or a printable ASCII code (32–126).
- **Mouse state** → `it_mouse_t { x, y, px, py, b }`: cell coords, logical-pixel
  coords, left-button-held bit.

### SDL backend internal state (file-local, in `it_screen_sdl.c`)

| State | Purpose |
|-------|---------|
| `SDL_Window *Wnd` | The 2×-scaled window. |
| `SDL_Renderer *Ren` | Renderer with `SDL_RenderSetLogicalSize(640,400)`. |
| `SDL_Texture *Tex` | 640×400 `ARGB8888` streaming texture. |
| `int KeyQueue[64]; KeyHead, KeyTail` | Ring buffer of pending keys (mirrors Win32). |
| `int WantQuit` | Set on `SDL_QUIT`/window-close. |
| `int MousePX, MousePY, MouseB` | Last logical-pixel position + button state. |

## Mappings (the substance of the port)

### Key mapping `MapSDLKey(SDL_Keysym)` → `ITK_*`

Mirror of `MapVKey` in `it_screen_win32.c`:

| SDL keycode | Result |
|-------------|--------|
| `SDLK_UP/DOWN/LEFT/RIGHT` | `ITK_UP/DOWN/LEFT/RIGHT` |
| `SDLK_PAGEUP/PAGEDOWN` | `ITK_PGUP/PGDN` |
| `SDLK_HOME/END` | `ITK_HOME/END` |
| `SDLK_INSERT/DELETE` | `ITK_INS/DEL` |
| `SDLK_ESCAPE` | `ITK_ESC` |
| `SDLK_RETURN/KP_ENTER` | `ITK_ENTER` |
| `SDLK_BACKSPACE` | `ITK_BACKSPACE` |
| `SDLK_F1..F12` | `ITK_F1 + (kc - SDLK_F1)` |
| `SDLK_TAB` | `ITK_SHIFT_TAB` if `KMOD_SHIFT` else `ITK_TAB` |
| (printable) | *not here* — delivered via `SDL_TEXTINPUT` as ASCII |
| else | `ITK_NONE` |

`SDL_TEXTINPUT.text[0]` in 32..126 → push as ASCII (equivalent to `WM_CHAR`).

### Mouse mapping

`SDL_MOUSEMOTION` / `SDL_MOUSEBUTTONDOWN` (left) / `SDL_MOUSEBUTTONUP` (left):
convert window (x,y) → logical via `SDL_RenderWindowToLogical` →
`MousePX/MousePY` (clamped to 0–639/0–399). Button-down pushes `ITK_MOUSE` and
sets `MouseB=1`; button-up clears `MouseB`. `mouse()` fills `x=px/8`, `y=py/8` —
identical to `W32_Mouse`.

### Window-close mapping

`SDL_QUIT` or `SDL_WINDOWEVENT` with `event.window.event ==
SDL_WINDOWEVENT_CLOSE` → set `WantQuit`, push `ITK_QUIT` (mirrors `WM_CLOSE`).

## Validation rules / invariants

- `init()` MUST return 0 (not crash/exit) when no display is available, enabling
  the terminal fallback.
- `present`/`key`/`mouse` MUST each pump the event queue so a window with focus
  stays responsive even if the editor calls only one of them in a tick (matches
  Win32, where each entry point calls `PumpMessages`).
- Logical coordinates MUST remain in 0–639 / 0–399 regardless of physical window
  size or DPI (guaranteed by `SDL_RenderSetLogicalSize`).
- Rasterizer output MUST be fed to SDL unmodified (no per-pixel conversion) on
  little-endian targets, preserving `ITED_SHOT` parity.
