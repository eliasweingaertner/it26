# Tasks: Terminal-Backend Mouse and Modifier Keys

**Input**: Design documents from `specs/011-terminal-input/`
**Prerequisites**: plan.md, research.md
**Repo**: `C:\Users\elias\fable5\ittrack`

## Phase 1: Setup + Foundational

- [X] T001 Verify clean baseline (build + gates per HANDOFF §3/§4).
- [X] T002 Parser skeleton in `src/it_screen.c`: `TermKeyQueue` FIFO,
      `TermMouse` state, incremental state machine `Term_FeedByte`
      (GROUND/ESC/CSI/SS3, numeric parameter collection, private-marker
      handling), `Screen_TermFeedTest` hook declared in `it_screen.h`;
      compiled on all platforms.

## Phase 2: US1 — POSIX modifier keys (P1)

- [X] T003 [US1] Alt-prefix decode (ESC + byte → ITK_ALT_A../ALT_0../
      ALT_BACKSLASH; unmapped consumed); bare-ESC via end-of-buffer (+
      grace re-poll in the POSIX pump).
- [X] T004 [US1] Modified-CSI decode per research R2.2 onto the ITK combo
      codes (arrows/Home/End/PgUp/PgDn/Ins/Del/F1..F12 × Shift/Alt/Ctrl,
      incl. Ctrl-F2/F7, Alt-F9/F10, Shift-F9); fold the existing legacy
      CSI/SS3 handling into the parser; 0x08 → Ctrl-H, 0x7F → Backspace
      on POSIX.
- [X] T005 [US1] modifyOtherKeys/CSI-u decode (research R2.3): CSI 27;m;c~
      and CSI c;mu → ITK_CTRL_0..5, ITK_CTRL_SHIFT_1..4 (both unshifted
      and shifted codepoint forms), ITK_CTRL_PLUS/MINUS, Ctrl-Backspace
      (keypad `/` dropped — indistinguishable in a terminal, see spec
      FR-003); enable `CSI >4;1m` in Term_Init, restore `CSI >4;0m` in
      Term_UnInit.

## Phase 3: US2 — SGR mouse (P2)

- [X] T006 [US2] SGR decode (`CSI < b;x;y M/m`): left press → clamp cells,
      set px/py = cell*8+4, b=1, push ITK_MOUSE; motion (b&32) updates
      position; release clears b; other buttons/wheel consumed.
- [X] T007 [US2] Mode plumbing: `?1002h?1006h` in Term_Init,
      `?1006l?1002l` in Term_UnInit; POSIX pump shared by Term_Key and a
      new Term_Mouse; vtable `mouse` member set → Screen_GetMouse works.

## Phase 4: US3 — Windows console scan codes (P3)

- [X] T008 [US3] Extend the `_getch` 0x00/0xE0 table per research R3
      (Alt letters/digits, Ctrl/Alt grey keys, Shift/Ctrl/Alt F-keys).

## Phase 5: Polish

- [X] T009 Selftest TERM block in `src/it_editor.c` (FR-007 streams:
      Alt prefix, modified CSI, CSI-u both forms, SGR press/drag/release,
      bare ESC, split sequence, junk) asserting ITK codes + mouse state.
- [X] T010 Docs: README terminal-backend notes (new capabilities, px
      approximation, shift press/release limitation), HANDOFF roadmap #7
      closed + status paragraph, it_screen.h comment fixes.
- [X] T011 Final gates (determinism ×4, roundtrip ×4×3, full selftest);
      verified on Linux via WSL Ubuntu (gcc 9.4): full selftest incl.
      TERM OK + determinism ×4 IDENTICAL; staged commits.

## Dependencies

T001 → T002 → {T003,T004,T005} → T006 → T007; T008 independent after
T002; T009 after Phase 3; T010/T011 last.
