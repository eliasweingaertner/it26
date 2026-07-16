# Quickstart: validating feature 010

Prerequisites: Windows + VS BuildTools (or Linux + gcc/SDL2), repo at
`C:\Users\elias\fable5\ittrack`, testdata modules present.

## Build

```
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && cl /nologo /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:ited.exe src\it_music.c src\it_effects.c src\it_tables.c src\it_driver.c src\it_load.c src\it_pattern.c src\it_save.c src\it_import.c src\it_ris.c src\it_screen.c src\it_screen_win32.c src\it_vgadata.c src\it_editor.c user32.lib gdi32.lib"
```

## Gates (all must be green)

1. **Determinism ×4** — build `test_pattern.exe`, run against all four
   testdata modules: every one `IDENTICAL` with the §4 known-good hashes.
2. **Roundtrip** — `test_pattern <mod> --roundtrip` ×4: formats 3/0/2 all
   equal the original hash.
3. **Selftest** — `ITED_SELFTEST=1 ited testdata\itdemo.it` reports the
   existing blocks **plus** the new PE-completion checks:
   - length shrink 64→32→undo revert byte-equality, grow 32→128 blank fill
   - mute/solo: Alt-F9 mute, `|` solo+advance, Alt-`\` unmute-all, state
     agreement with the engine flags F5/F11 read
   - view schemes: Ctrl-3 assignment, Ctrl-Shift-2 preset table contents,
     Ctrl-0 removal, width-overflow revert, cursor addressing across a
     mixed-scheme layout
4. **Visual** — `ITED_SHOT=f2.bmp ITED_SHOT_SCREEN=1 ited testdata\itdemo.it`
   after scripted scheme keys; compare against `..\screenshots\` originals for
   each view method and preset.

## Manual smoke (interactive)

- Ctrl-F2 → dialog at (15,19)-(65,33), length thumbbar focused, 32..200;
  OK resizes, ESC cancels; Ctrl-Backspace shows the undo entry.
- `\`, `/`(keypad), `?`, `|`, Alt-F9/F10, Alt-`\` in F2; headers grey out.
- Ctrl-1..5, Ctrl-0, Ctrl-Shift-1..4, Alt-T, Alt-R, Alt-H, Ctrl-T, Ctrl-H —
  status messages match research.md R3 verbatim.
