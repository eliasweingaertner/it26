# it26 vs IT 2.14: rerun after the round-2 fixes (#41-#51)

Rerun: 2026-10-11, both sides fresh, all eight compare scripts of both
rounds (`it_screens`, `f2_after_f9`, `pe_markers`, `header_modes`,
`modals`, `modals2`, `song_vars`, `screens2`) plus the DOS-only
`it_play_jeff93` audio run. Same setup as the [findings](FINDINGS.md).

## Status of the findings

| # | Finding | Issue | Fix commit | Rerun |
|---|---|---|---|---|
| G1 | Main menu opens on "View Patterns"; submenus forget their position | - | a0d22be | matches |
| G2 | Parent menu item highlighted under its submenu | #41 | d89567f | matches |
| G3 | Ctrl-N clears at once, no New Song dialog | #42 | df8ec5d | matches |
| G4 | F11 Channel Volume page missing | #43 | a313288 | matches |
| G5 | Ctrl-F12 Palette Configuration missing | #44 | dcc4c86 | matches |
| G6 | F3 Exchange / Swap / Replace / Resize prompts | #45 | d7ab6a5 | matches (and F4 Swap, Copy) |
| G7 | "Delete sample?" one column off | #46 | a80f6d5 | matches; "Sample Amplification %" is 2.17 source (see below) |
| G8 | "No block is marked" as a status message | #47 | 6f50b79 | matches |
| G9 | Undo dialog missing when empty, layout | #48 | be7b61e | matches |
| G10 | Alt-N status message | #49 | 9318d46 | matches |
| G11 | F12 Tab wraps from "Save all Preferences" | #50 | 2a4ce9a | matches |
| G12 | Blank cells after button / menu labels in 23h instead of 20h | #51 | - | **not fixed**, see below |
| G13 | F10 Filename pre-filled | - | - | kept (port convenience) |
| G14 | Ctrl-F1 key table | - | - | documented, unchanged |

With the fixes the `modals2` F4 part runs for the first time (before,
Alt-Y left the main menu open on both sides): F4 Delete, Copy and Swap
match too.

## What still differs, and why

Round 1 rows unchanged: copyright year, FreeMem/EMS, clock, the 2.17
wording of "Pattern Editor (F2)" / "Pattern Editor Options", Ctrl-D left
out of the help, host drives / paths / file dates, and F15 (Load Sample
sort order).

New this round:

- **"Sample Amplification %"** one column right of 2.14: the 2.17 source
  has `SampleAmpText` at (27,27) with three leading spaces, i.e. column
  30; IT 2.14 draws it at 29. A 2.14/2.17 difference.
- **F10 "IT215" button**: 2.17 source, behind `IF DDCOMPRESS`.
- **Ctrl-F1** (G14) and the F10 Filename (G13): deliberate.
- **G12 / #51**, invisible: the cells after a button's or menu item's
  label after the focus has been there (F12 radio buttons, menu items).
  `F_PostButtonObject` paints the focused button's inner row 20h on every
  key, and IT doesn't fill button interiors when it draws them, so what
  those cells show depends on what was on screen before (the previous
  screen, earlier visits). An attempt to model it fixed one capture and
  broke others; reverted. Spaces on the same background either way.

## Gates

- Selftest: 20/20 blocks.
- Determinism (`test_pattern`, four modules): IDENTICAL, hashes
  unchanged.
