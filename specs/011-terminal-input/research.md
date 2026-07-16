# Research: Terminal-Backend Mouse and Modifier Keys (011)

No ASM decode: this is port-side portability code behind the
`screen_backend_t` vtable (the original is a DOS binary). The contracts to
honour are (a) the editor's ITK_* enum (it_screen.h, features 009/010) and
(b) the pixel backends' mouse semantics (it_screen_win32.c `W32_Mouse` /
`WM_LBUTTONDOWN`).

## R1. Editor-side contracts (from the port sources)

- `Key_Get()` non-blocking; ITK_NONE when idle. ITK codes consumed by the
  editor (verified against it_editor.c key tables as of feature 010):
  - Alt letters `ITK_ALT_A+n`, Alt digits `ITK_ALT_0+n`,
    `ITK_ALT_BACKSLASH`, `ITK_ALT_INS/DEL/UP/DOWN`, `ITK_ALT_PLUS/MINUS`,
    `ITK_ALT_F9/F10`.
  - Ctrl: `ITK_CTRL_UP/DOWN/LEFT/RIGHT/HOME/END/PGUP/PGDN/INS/DEL/
    BACKSPACE`, `ITK_CTRL_PLUS/MINUS`, `ITK_CTRL_F2/F7`, `ITK_CTRL_0..+5`,
    `ITK_CTRL_SHIFT_1..+3`, plain-byte Ctrl-letters (1..26; 0x08=Ctrl-H,
    0x14=Ctrl-T, 0x03=Ctrl-C, 0x13=Ctrl-S...).
  - Shift: `ITK_SHIFT_UP/DOWN/LEFT/RIGHT/PGUP/PGDN/HOME/END`,
    `ITK_SHIFT_TAB`, `ITK_SHIFT_F9`; `ITK_SHIFT_PRESS/RELEASE` are
    key-transition events only the pixel backends can produce.
  - `ITK_KP_DIVIDE` (keypad slash, distinct from '/').
- Mouse (`Screen_GetMouse`): x,y = cell 0..79/0..49; px,py = logical pixels
  0..639/0..399 (thumbbar precision); b bit 0 = left held. `ITK_MOUSE` is
  pushed once per left press (WM_LBUTTONDOWN analogue); drags are observed
  by polling while b&1; release just drops b.
- The backend vtable's `mouse` member may be NULL (= no mouse); making it
  non-NULL is the whole activation switch — `Screen_GetMouse` and the
  widget layer need no change.

## R2. POSIX sequence families to decode

Encodings as emitted by xterm and compatibles (xterm ctlseqs; VTE, kitty,
foot, Windows Terminal, tmux agree on this subset):

1. **Alt prefix**: Alt+X arrives as `ESC` `X` (X any printable/control
   byte). Disambiguation from bare ESC: sequences arrive atomically; if no
   byte is buffered after ESC on the same poll (and a ~20ms grace re-poll),
   it is a bare ESC.
2. **CSI with modifier parameter** `m` = 1 + (1=Shift, 2=Alt, 4=Ctrl):
   - Arrows/Home/End: `CSI 1;m A..D/H/F` (plain: `CSI A..D/H/F`).
   - Nav block: `CSI n;m ~` with n = 2 Ins, 3 Del, 5 PgUp, 6 PgDn,
     1/7 Home, 4/8 End.
   - F-keys: F1..F4 plain = `SS3 P..S` (`ESC O P`..), modified =
     `CSI 1;m P..S`; F5..F12 = `CSI n ~` with n = 15,17,18,19,20,21,23,24
     (modified: `CSI n;m ~`). (The current decoder's `ESC [ 1 ...`
     digit-pair handling is folded into the general CSI parser.)
   - `CSI Z` = Shift-Tab (already handled).
3. **modifyOtherKeys / CSI-u** for combos with no legacy encoding
   (Ctrl-digit, Ctrl-Shift-digit, Ctrl-plus/minus):
   - Enable: `CSI > 4 ; 1 m` (level 1 = only otherwise-unencodable keys;
     level 2 would re-encode plain keys — NOT wanted). Restore with
     `CSI > 4 ; 0 m` (xterm) — kitty/foot speak CSI-u natively and ignore
     it.
   - Forms: xterm `CSI 27 ; m ; code ~`; CSI-u `CSI code ; m u`. `code` is
     the unshifted ASCII codepoint. Ctrl-1 → code 49, m 5. Ctrl-Shift-1 →
     code 49, m 6 (kitty may report the shifted codepoint 33 — accept
     both digits row `!@#$` → 1..4 when m has Ctrl+Shift).
4. **SGR mouse**: enable `CSI ? 1002 h` (button-event tracking: press,
   release, motion-while-pressed) + `CSI ? 1006 h` (SGR encoding). Report:
   `CSI < b ; x ; y M` (press/motion) or `m` (release); x,y 1-based cells.
   b: bit 0..1 button (0 left), bit 5 (32) motion, bits 2..4 modifiers,
   64+ wheel. Only button 0 matters; wheel could map to ITK_UP/DOWN later
   (out of scope). Disable order on uninit: `CSI ? 1006 l` + `CSI ? 1002 l`.
5. **Backspace**: POSIX terminals send DEL 0x7F for the Backspace key;
   byte 0x08 therefore maps to Ctrl-H (the editor's row-hilight toggle),
   matching the Win32 window backend after feature 010. The Windows
   console keeps 8 = Backspace (both keys send 8 there; Backspace wins).

## R3. Windows console (`_getch`) scan-code extension

`_kbhit`/`_getch` prefix 0x00 (Alt/F-keys) or 0xE0 (grey keys) + scan:
- Alt letters: 16..25 = QWERTYUIOP, 30..38 = ASDFGHJKL, 44..50 = ZXCVBNM
  (map through a scan→letter table); Alt digits 120..129 = Alt-1..9,0.
- Ctrl grey keys (0xE0 or 0x00): 115/116 Ctrl-Left/Right, 141/145
  Ctrl-Up/Down, 119/117 Ctrl-Home/End, 132/118 Ctrl-PgUp/PgDn, 146/147
  Ctrl-Ins/Del; Alt grey: 162/163 Alt-Ins/Del, 152/160 Alt-Up/Down.
- F-keys: Shift-F1..F10 = 84..93 (Shift-F9=92), Ctrl-F1..F10 = 94..103
  (Ctrl-F2=95, Ctrl-F7=100), Alt-F1..F10 = 104..113 (Alt-F9=112,
  Alt-F10=113), F11/F12 family 133/134 (plain), 135/136 (shift),
  137/138 (ctrl), 139/140 (alt).
- No mouse and no Shift-arrows on this path (console reports plain
  arrows); the Win32 window backend remains the full-featured surface.

## R4. Architecture decision

One platform-neutral incremental parser (compiled everywhere, unit-testable
on Windows):

- `Term_FeedByte(uint8_t)` — state machine (GROUND / ESC / CSI / SS3
  collecting params) that pushes ITK codes into a small FIFO and updates
  the mouse mirror. Exposed for the selftest as `Screen_TermFeedTest`
  (returns popped keys) + mouse readback via the normal struct.
- POSIX `Term_Key`/`Term_Mouse` share a pump: `read()` all available bytes
  into the parser, then pop one key / copy mouse state. ESC-grace: if the
  stream ends right after ESC, one 20ms re-poll before emitting ITK_ESC.
- Windows console path keeps `_getch` scan codes (R3) — no VT input mode
  (conio bypasses VT translation; ReadConsoleInput would be a rewrite for
  a secondary path).
- px/py = x*8+4 / y*8+4 (cell centre) — documented approximation.

## R5. Decisions

- **D1**: modifyOtherKeys level 1 (not 2) — plain keys keep legacy
  encodings; only the otherwise-impossible combos gain sequences.
- **D2**: Accept both xterm `CSI 27;m;c~` and CSI-u `c;m u` forms.
- **D3**: Mouse buttons other than left, and the wheel, are consumed
  silently (parity with the pixel backends, which only track left).
- **D4**: `ITK_SHIFT_PRESS/RELEASE` (chord entry / marking anchor reset)
  stay pixel-only; README documents it.
- **D5**: The selftest TERM block runs the parser directly (no tty), so
  SC-001 is provable headless on Windows; the interactive Linux pass is
  recorded like the macOS deferral if no Linux box is reachable this
  session (try WSL first).
