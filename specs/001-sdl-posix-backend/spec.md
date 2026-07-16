# Feature Specification: SDL Pixel Backend for POSIX

**Feature Branch**: `001-sdl-posix-backend`

**Created**: 2026-06-16

**Status**: Draft

**Input**: User description: "SDL backend for POSIX — give Linux/macOS users the authentic 640x400 VGA pixel window (with mouse) that Windows users already get via the Win32 backend, behind the existing `screen_backend_t` vtable. Roadmap item #1 in docs/HANDOFF.md §6."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Authentic pixel window on Linux/macOS (Priority: P1)

A musician or developer on Linux or macOS launches the editor (`ited`) and is
presented with the same pixel-accurate Impulse Tracker window that Windows users
see today: the 80×50 cell screen rendered with the real 8×8 VGA glyph bitmaps,
the Camouflage palette, the custom UI glyphs and box bevels, integer-scaled to
640×400 logical pixels. Today these users get only the truecolor terminal
fallback, which approximates the custom glyphs with Unicode and depends on
terminal capabilities.

**Why this priority**: This is the entire point of the feature — the authentic
visual experience is currently Windows-only. Everything else (mouse, input
parity, configuration) is in service of, or refinement on, this window existing
at all. Delivered alone it already makes the editor usable as intended on two
new platforms.

**Independent Test**: On a Linux and a macOS machine, run `ited <module.it>` and
confirm a graphical window opens showing the editor screens rendered from the
real glyph bitmaps (not Unicode approximations), visually matching the Windows
window and the reference screenshots in `../screenshots/`. The
pixel-exact `ITED_SHOT` BMP captured on POSIX is identical to the one captured
on Windows for the same screen and module.

**Acceptance Scenarios**:

1. **Given** a Linux desktop session, **When** the user runs `ited`, **Then** a
   resizable/scaled 640×400 pixel window opens displaying the active editor
   screen with the authentic VGA font, palette and box glyphs.
2. **Given** a macOS desktop session, **When** the user runs `ited`, **Then**
   the same authentic pixel window opens and behaves identically.
3. **Given** the pixel window is open, **When** the user closes it via the
   window manager, **Then** the editor exits cleanly (the same quit semantics as
   the Win32 backend's window-close).
4. **Given** the same module and screen, **When** a screen is captured to a BMP
   on POSIX and on Windows, **Then** the two images are byte-identical.

---

### User Story 2 - Full keyboard and mouse parity (Priority: P2)

A user driving the editor in the POSIX pixel window expects every input to work
exactly as it does in the Win32 window: all function keys, navigation keys,
note-entry keys, Tab/Shift-Tab focus movement, and a working mouse (click to
focus/activate widgets, pixel-precise thumbbar dragging, list-row and menu-item
clicks, clicking the pattern grid to move the cursor).

**Why this priority**: The window is only useful if it is operable. Keyboard and
mouse parity is what turns "the window renders" (P1) into "the editor works."
It is separated from P1 because a render-only window is already independently
demonstrable and verifiable.

**Independent Test**: With the POSIX window open, exercise the documented key
map (F1–F12, arrows, PgUp/PgDn, Home/End, Ins/Del, Tab/Shift-Tab, note keys,
octave/edit-step keys, pattern keys) and confirm each produces the same editor
behaviour as on Windows; then drag a thumbbar, click a menu item, and click a
pattern cell and confirm the cursor/value updates pixel-precisely.

**Acceptance Scenarios**:

1. **Given** the POSIX pixel window has focus, **When** the user presses any
   editor key, **Then** the key is reported to the editor with the same `ITK_*`
   code / ASCII value the Win32 backend would report.
2. **Given** a focused thumbbar, **When** the user drags it with the mouse,
   **Then** the value tracks the pointer pixel-precisely (1 px = 1 unit on
   classic bars), matching the Win32 behaviour.
3. **Given** any widget, menu item, or pattern cell, **When** the user clicks
   it, **Then** it is focused/activated/selected exactly as on Windows, and the
   reported mouse coordinates include both cell (0–79, 0–49) and logical-pixel
   (0–639, 0–399) positions.

---

### User Story 3 - Predictable backend selection (Priority: P3)

A user (or an automated/headless environment) needs control over which backend
runs, and a sensible default. On POSIX with a graphical display available, the
SDL pixel window is the default; `ITED_TERM=1` forces the terminal backend as
before; and when no display is available the editor falls back to the terminal
rather than failing.

**Why this priority**: Selection logic is a refinement that makes the feature
robust across real-world POSIX environments (SSH sessions, CI, headless boxes).
The feature delivers value without it (a user with a display gets the window),
but without graceful selection the tool would break in headless contexts and
disrupt the existing terminal/`ITED_SHOT` workflows.

**Independent Test**: Run `ited` in three POSIX contexts — graphical desktop,
`ITED_TERM=1` set, and a headless/no-display session — and confirm the chosen
backend matches expectations in each, with no crash and a clear message when
falling back.

**Acceptance Scenarios**:

1. **Given** a POSIX session with a usable graphical display, **When** `ited`
   starts with no overriding environment variable, **Then** the SDL pixel
   backend is selected.
2. **Given** `ITED_TERM=1` is set, **When** `ited` starts on POSIX, **Then** the
   terminal backend is selected regardless of display availability.
3. **Given** no graphical display is available and `ITED_TERM` is unset, **When**
   `ited` starts, **Then** it falls back to the terminal backend (or reports a
   clear message) instead of crashing.
4. **Given** any backend selection, **When** the non-interactive verification
   modes run (`ITED_SHOT`, `ITED_DUMP`, `ITED_SELFTEST`), **Then** they continue
   to work unchanged, requiring no window or display.

---

### Edge Cases

- **No display / headless (SSH, CI, no X11/Wayland/Quartz)**: must not crash;
  fall back to the terminal backend with a clear message.
- **SDL runtime/library missing or fails to initialise**: the editor must
  degrade gracefully to the terminal backend rather than aborting.
- **Window resized or moved by the window manager**: the 640×400 content stays
  integer-scaled and correctly positioned; mouse cell/pixel mapping stays
  accurate after resize.
- **High-DPI / Retina displays**: logical 640×400 pixels map predictably so the
  rendered glyphs stay crisp and the reference-screenshot comparison still holds.
- **Rapid input and held keys**: key repeat and modifier handling match the
  Win32 backend's reporting so behaviour does not diverge by platform.
- **Window loses/regains focus**: input is suppressed/resumed cleanly without
  stuck modifier or mouse-button state.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: On POSIX platforms, the editor MUST present the 80×50 cell buffer
  as an integer-scaled 640×400 logical-pixel window rendered from the real 8×8
  glyph bitmaps and the Camouflage palette — visually equivalent to the existing
  Win32 pixel window.
- **FR-002**: The new backend MUST be implemented behind the existing
  `screen_backend_t` vtable in `it_screen.h` (`init`, `uninit`, `present`,
  `key`, `mouse`), reusing the shared, backend-independent cell buffer,
  control-code renderer, box drawing and `Screen_Rasterize` rasterizer. It MUST
  NOT introduce a parallel rendering path or duplicate the rasterizer.
- **FR-003**: The backend MUST report keyboard input using the same `ITK_*`
  codes and ASCII values as the Win32 backend, covering all keys the editor
  consumes (function keys, navigation, editing, focus, note entry,
  octave/edit-step, pattern navigation), including a window-close event reported
  as `ITK_QUIT`.
- **FR-004**: The backend MUST support the mouse via the vtable `mouse` member,
  reporting cell coordinates (0–79, 0–49), logical-pixel coordinates (0–639,
  0–399) and left-button state, with pixel-precise thumbbar positioning matching
  the original's `8010h` mouse-event behaviour.
- **FR-005**: Backend selection on POSIX MUST default to the SDL pixel window
  when a graphical display is available, MUST honour `ITED_TERM=1` to force the
  terminal backend, and MUST fall back to the terminal backend (without crashing)
  when no display is available or SDL cannot initialise.
- **FR-006**: The terminal backend MUST remain the dependency-free fallback and
  MUST continue to function unchanged on POSIX.
- **FR-007**: The non-interactive verification modes (`ITED_SHOT` pixel-exact
  BMP, `ITED_DUMP` ASCII, `ITED_SELFTEST`) MUST continue to work on POSIX with no
  window or display required, producing output identical to Windows for the same
  module and screen.
- **FR-008**: Engine behaviour and audio output MUST be unaffected; this feature
  changes only the presentation/input layer, and the audio-determinism
  regression (`tests/test_pattern.c`) MUST remain `IDENTICAL` for all four
  testdata modules.
- **FR-009**: The new SDL dependency MUST be optional in the build: builds that
  do not request it (and the existing player `itplay`) MUST continue to build
  and run with no new required dependency, and the build documentation MUST
  describe how to enable the SDL backend on Linux and macOS.
- **FR-010**: Window lifecycle (open on start, clean teardown on quit, graceful
  handling of resize and focus changes) MUST match the Win32 backend's user-
  visible behaviour.

### Key Entities

- **Presentation backend**: A selectable implementation of the
  `screen_backend_t` interface responsible for turning the shared cell buffer
  into visible output and turning platform input into editor key/mouse events.
  Existing instances: Win32 pixel window, VT/ANSI truecolor terminal. This
  feature adds: SDL pixel window (POSIX).
- **Cell buffer**: The shared 80×50 array of (character, attribute) cells that
  every backend presents; owned by the backend-independent core, not duplicated
  per backend.
- **Input event**: A keyboard key (ASCII or `ITK_*` code, including `ITK_QUIT`
  and `ITK_MOUSE`) or a mouse state (cell + logical-pixel coordinates + button),
  reported uniformly across backends.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Linux and macOS users can launch the editor and get the authentic
  pixel window with zero Windows-only steps — feature parity across all three
  platforms for opening and viewing every editor screen (F1–F12, menus, load
  requester).
- **SC-002**: For every editor screen, a POSIX `ITED_SHOT` capture is
  byte-identical to the Windows capture of the same screen and module
  (pixel-exact visual parity).
- **SC-003**: 100% of the editor keys and mouse interactions that work in the
  Win32 window produce the same behaviour in the POSIX window (no
  platform-specific input gaps).
- **SC-004**: In a headless POSIX environment the editor starts successfully via
  the terminal fallback in 100% of cases, with no crash.
- **SC-005**: The audio-determinism regression remains `IDENTICAL` for all four
  testdata modules, confirming the change is presentation-only.
- **SC-006**: Builds without the SDL backend enabled, and the `itplay` player,
  continue to build and run with no newly required dependency.

## Assumptions

- "POSIX" here means Linux and macOS desktop environments; other Unix-likes are
  expected to work via the same path but are not explicitly targeted.
- SDL is the chosen cross-platform windowing/input library for the POSIX pixel
  backend (per roadmap item #1 in `docs/HANDOFF.md` §6); it is treated as an
  optional, opt-in build dependency so the no-dependency terminal fallback and
  the `itplay` player are unaffected.
- The existing `Screen_Rasterize` (cells → 640×400 RGB) is backend-neutral and
  is reused as-is; no changes to glyph/palette/box data or the cell-buffer model
  are required.
- Audio continues to run through the existing miniaudio path; this feature does
  not touch the audio backend.
- The Win32 backend is the behavioural reference for input mapping, mouse
  semantics, window-close/quit, and visual output; "parity with Windows" is the
  acceptance bar.
- A future terminal-backend mouse (roadmap item #7) is out of scope here; this
  feature delivers mouse support only for the SDL pixel window.
- The 2× integer scaling used today (640×400 → 1280×800) is the expected default
  presentation scale, consistent with the Win32 backend.
