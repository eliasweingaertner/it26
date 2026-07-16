# Feature Specification: Terminal-Backend Mouse and Modifier Keys

**Feature Branch**: `011-terminal-input`

**Created**: 2026-07-11

**Status**: Draft

**Input**: User description: "Roadmap #7: terminal-backend mouse (xterm SGR
mouse reporting) and terminal Alt/Ctrl/Shift keys (ESC-prefix sequences and
xterm modified-CSI encodings). The editor side is backend-agnostic since
feature 009 gave it the full ITK_* modifier-combo codes; only the VT/ANSI
terminal backend in it_screen.c cannot produce them (and reports no mouse)."

## Context

The terminal backend is the no-dependency fallback (and the only backend on a
headless POSIX box). Since feature 009/010 the editor's F2 surface leans
heavily on Alt/Ctrl/Shift combos (block ops, view schemes, mute/solo,
Ctrl-F2) and the widget layer is mouse-operable — none of which the terminal
backend can currently deliver. This feature is **port-side portability
code**, not an ASM port: the original is a DOS program; the constitution's
1:1 rule applies to the editor/engine behaviour behind the backend interface,
which is untouched here.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Full modifier keys in a POSIX terminal (Priority: P1)

A user running `ited` over SSH (or on a headless box) needs the same key
surface as the pixel backends: Alt-letter block operations, Ctrl-arrows,
Shift-arrow marking, Ctrl-0..5 view schemes, Alt-F9/F10 mute/solo, Ctrl-F2.
Modern terminals encode these as ESC-prefixed bytes (Alt), CSI sequences with
the xterm modifier parameter (arrows, nav keys, F-keys), and — for
otherwise-unencodable combos like Ctrl-digit — the xterm `modifyOtherKeys`
CSI 27 / CSI-u encodings. The backend requests the richer encoding on init,
parses all three families, and emits the editor's existing ITK_* codes.

**Why this priority**: The terminal backend is functionally frozen out of
everything features 009/010 added; this is the roadmap-#7 gap itself.

**Independent Test**: Feed the byte sequences a terminal emits (canned
streams) through the parser and assert the emitted ITK codes; interactively
verify in a real terminal on Linux.

**Acceptance Scenarios**:

1. **Given** the terminal backend, **When** `ESC b` arrives, **Then**
   Key_Get returns ITK_ALT_B (likewise a..z, 0..9, `\`).
2. **Given** `ESC [ 1 ; 5 C` (Ctrl-Right), **Then** ITK_CTRL_RIGHT is
   returned; the full xterm modifier table (shift 2, alt 3, ctrl 5,
   ctrl+shift 6) maps onto the existing ITK_SHIFT_*/ITK_CTRL_*/ITK_ALT_*
   codes for arrows, Home/End, PgUp/PgDn, Ins/Del and F1..F12.
3. **Given** `modifyOtherKeys` reports Ctrl-3 (`ESC [ 27 ; 5 ; 51 ~` or
   `ESC [ 51 ; 5 u`), **Then** ITK_CTRL_0+3 is returned; Ctrl-Shift-1..4
   likewise.
4. **Given** a bare ESC press, **Then** ITK_ESC is returned (no swallowed
   keys, no misread Alt prefix).
5. **Given** an unknown or incomplete escape sequence, **Then** it is
   consumed silently — no garbage characters reach the editor.

---

### User Story 2 - Mouse in a POSIX terminal (Priority: P2)

The user clicks buttons, drags thumbbars and clicks pattern cells in the
terminal exactly as in the pixel window. The backend enables xterm SGR mouse
reporting (`?1002h` button-event tracking + `?1006h` SGR encoding), parses
`ESC [ < b ; x ; y M/m`, maintains the `it_mouse_t` state (cell coords,
approximated logical pixels, button bit) and pushes ITK_MOUSE on left-button
press — the exact contract of the Win32/SDL backends, so the editor's widget
and drag code works unchanged.

**Why this priority**: Completes the terminal backend's parity; the widget
layer is already mouse-first for thumbbars.

**Independent Test**: Canned SGR press/drag/release streams through the
parser: assert ITK_MOUSE emission, coordinates and button state transitions;
interactive verification on Linux.

**Acceptance Scenarios**:

1. **Given** a left press at column 10, row 5 (`ESC [ < 0 ; 10 ; 5 M`),
   **Then** Key_Get yields ITK_MOUSE and Screen_GetMouse reports x=9, y=4,
   b=1 (px/py at the cell's pixel position).
2. **Given** a drag (motion events with button held, `b & 32`), **Then**
   Screen_GetMouse tracks the moving position with b=1 and no extra
   ITK_MOUSE is pushed.
3. **Given** the release (`m` final byte), **Then** b returns to 0.
4. **Given** coordinates beyond the 80x50 cell grid (oversized terminal
   window), **Then** they clamp to the grid as the pixel backends clamp.
5. **Given** init/uninit, **Then** mouse reporting is enabled/disabled so
   the shell is left clean (no stray reports after exit).

---

### User Story 3 - Windows console modifier keys (Priority: P3)

`ITED_TERM=1` on Windows uses the `_getch` console path. Its 0x00/0xE0
prefix codes already distinguish Alt-letters, Alt/Ctrl F-keys and
Ctrl-arrows/nav keys; the decoder just never mapped them. Extend the
scan-code table so the Windows console gets the same Alt/Ctrl surface
(mouse stays a pixel-backend feature on Windows).

**Why this priority**: Smallest audience (the Win32 window is the primary
backend on Windows), but it makes the headless Windows selftest exercise
the same editor paths.

**Independent Test**: Scan-code table review + interactive smoke in a
Windows console.

**Acceptance Scenarios**:

1. **Given** the Windows console path, **When** Alt-B (0x00 + 48) arrives,
   **Then** ITK_ALT_B is returned; Alt-letters, Alt-digits, Ctrl-arrows,
   Ctrl-Home/End/PgUp/PgDn/Ins/Del, Alt-F9/F10, Ctrl-F2/F7 and Shift-F9
   map likewise.
2. **Given** keys the console cannot distinguish (e.g. Shift-arrows in
   `_getch`), **Then** behaviour is unchanged (plain arrows).

---

### Edge Cases

- Bare ESC vs ESC-as-prefix: with a raw non-blocking tty, a lone ESC has no
  follow-up byte in the buffer; terminals transmit multi-byte sequences
  atomically. The parser must treat "ESC with nothing buffered" as ITK_ESC.
- Sequences split across reads (slow SSH): the parser must be incremental
  (carry partial state between polls) and never block the UI thread.
- Mouse reports arriving while the editor is polling Screen_GetMouse (not
  Key_Get): the pump must be shared — bytes parsed during a mouse poll must
  queue any key events for the next Key_Get and vice versa.
- Terminals without SGR mouse / modifyOtherKeys support: the enables are
  ignored by the terminal; plain operation is unaffected.
- ITK_SHIFT_PRESS/RELEASE (chord entry, shift-marking anchor) cannot exist
  in a terminal (no key-up events): Shift-arrow marking works via the
  ITK_SHIFT_* movement codes; chord entry stays pixel-backend-only.
  Documented limitation.
- The existing `Ctrl-H = 0x08 vs Backspace` collision: POSIX terminals send
  DEL (0x7F) for Backspace, so 0x08 can safely become Ctrl-H; the Windows
  console sends 8 for both — Backspace wins there (unchanged).

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The POSIX terminal backend MUST decode ESC-prefixed printable
  bytes as Alt combos: `ESC a..z` → ITK_ALT_A.., `ESC 0..9` → ITK_ALT_0..,
  `ESC \` → ITK_ALT_BACKSLASH; unmapped ESC-prefixed bytes are consumed.
- **FR-002**: The backend MUST decode the xterm modified-CSI encodings
  (`CSI 1;m {A-F,H}`, `CSI n;m ~`, `SS3`/`CSI 1;m {P-S}` F-keys) with the
  modifier parameter (2=Shift, 3=Alt, 5=Ctrl, 6=Ctrl+Shift) onto the
  existing ITK combo codes for arrows, Home/End, PgUp/PgDn, Ins/Del and
  F1..F12 (at minimum every combo the editor consumes: Ctrl/Shift/Alt
  arrows, Ctrl-Home/End, Ctrl/Shift-PgUp/PgDn, Shift-Home/End,
  Ctrl/Alt-Ins/Del, Ctrl-F2, Ctrl-F7, Alt-F9, Alt-F10, Shift-F9).
- **FR-003**: The backend MUST request `modifyOtherKeys` level 1 on init
  (restore level 0 on uninit) and decode both `CSI 27;m;c~` and CSI-u
  (`CSI c;m u`) forms, at minimum for Ctrl-0..5 → ITK_CTRL_0.. and
  Ctrl-Shift-1..4 → ITK_CTRL_SHIFT_1.., Ctrl-plus/minus and
  Ctrl-Backspace. (Keypad `/` is indistinguishable from the main-row
  `/` in a terminal — documented limitation; `?` covers mute-previous.)
- **FR-004**: The backend MUST enable SGR mouse reporting (`?1002h` +
  `?1006h`) on init and disable it on uninit; parse press/drag/release;
  maintain `it_mouse_t` (x,y cell coords clamped to 80x50; px,py derived
  logical pixels; b bit 0 = left held); push ITK_MOUSE exactly once per
  left-button press. The backend's `mouse` vtable member becomes non-NULL,
  matching the Win32/SDL contract so editor code is unchanged.
- **FR-005**: Input handling MUST be an incremental, platform-neutral byte
  parser feeding a key queue plus mouse state, shared by the key and mouse
  poll entry points; partial sequences carry over; nothing blocks; unknown
  sequences are consumed silently.
- **FR-006**: The Windows console path MUST extend its 0x00/0xE0 scan-code
  table with the Alt-letter/digit codes, Ctrl-arrows/Home/End/PgUp/PgDn/
  Ins/Del, Alt-Ins/Del, Alt-F9/F10, Ctrl-F2/F7, Shift-F9; existing
  mappings are unchanged.
- **FR-007**: The byte parser MUST be verifiable headless on any platform:
  the editor selftest gains a TERM block feeding canned byte streams
  (Alt prefix, modified CSI, CSI-u, SGR mouse press/drag/release, bare ESC,
  split sequences, junk) and asserting the emitted ITK codes and mouse
  state.
- **FR-008**: Engine files and editor logic MUST remain untouched;
  changes confine to `it_screen.c`/`it_screen.h` (plus the selftest block
  in `it_editor.c` and docs).
- **FR-009**: All terminal mode changes (mouse reporting, modifyOtherKeys)
  MUST be reverted by `Screen_UnInit` so the shell is left clean.
- **FR-010**: README (terminal backend notes) and HANDOFF (roadmap #7,
  status paragraph) MUST be updated; the ITK_* enum comments that say the
  terminal backend produces no combos must be corrected.

### Key Entities

- **Input pump / byte parser**: incremental decoder from raw bytes to ITK
  codes + mouse state; the single shared source for key and mouse polls.
- **Key queue**: small FIFO (as in the Win32 backend) decoupling parse time
  from poll time.
- **Mouse state**: the `it_mouse_t` mirror (cell x/y, pixel px/py, button).
- **Terminal modes**: the DECSET/modifyOtherKeys enables owned by
  init/uninit.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The selftest TERM block passes headless on Windows (and on
  POSIX when run there): every canned sequence maps to the asserted ITK
  code; the SGR stream produces the asserted press/drag/release mouse
  states; bare-ESC and junk streams produce exactly ITK_ESC / nothing.
- **SC-002**: All existing gates stay green (determinism ×4, roundtrip
  ×4×3, full selftest) — no engine or editor-logic change.
- **SC-003**: In a real POSIX terminal, the F2 modifier surface (Alt block
  ops, Ctrl-0..5 views, mute/solo family, Ctrl-F2 dialog) and widget mouse
  operation work; verified on Linux when hardware/WSL is available, else
  recorded as the same deferred-verification note as the macOS pass.
- **SC-004**: After exit (normal or Ctrl-C), the shell shows no mouse
  escape spew and keys type normally (modes restored).

## Assumptions

- xterm-compatible encodings (xterm, VTE, kitty, foot, Windows Terminal,
  tmux with default settings) are the target; terminals that ignore the
  enables degrade to today's behaviour.
- The editor consumes only the ITK codes listed; combos terminals cannot
  express (ITK_SHIFT_PRESS/RELEASE, Ctrl-Alt keys) remain absent and the
  editor already tolerates that.
- Approximated px/py (cell-derived) is acceptable for thumbbar precision in
  the terminal (8-pixel granularity), as cells are the terminal's atom.
