# Quickstart: verifying the F4 instrument editor

## Build (Windows, as in HANDOFF §3)

```
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && cl /nologo /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:ited.exe src\it_music.c src\it_effects.c src\it_tables.c src\it_driver.c src\it_load.c src\it_pattern.c src\it_screen.c src\it_screen_win32.c src\it_vgadata.c src\it_editor.c user32.lib gdi32.lib"
```

## Scenario 1 — authentic layout (SC-001)

```
set ITED_SHOT=f4_general.bmp
set ITED_SHOT_SCREEN=3
ited testdata\itdemo.it
```

Compare against `..\screenshots\` references. Repeat for each tab (the shot hook
takes the active tab; drive tabs via ITED_SELFTEST or capture from a live
window). Boxes, labels, buttons and bars must sit at the contract coordinates.

## Scenario 2 — operable tabs (SC-002)

Run `ited testdata\itdemo.it`, F4, click/Tab to each tab and change every
control (toggles flip On/Off, bars drag, loop fields accept digits). Reopen the
tab: values persist. Envelope On + audition (piano keys) audibly applies the
envelope (SC-003).

## Scenario 3 — envelope node editing

On the Volume tab select the envelope display, use the `I_PostEnvelope` keys to
add/move/delete nodes and set loops; the graph and `env_t` update, and playback
reflects the shape.

## Scenario 4 — determinism gate (SC-004)

```
test_pattern testdata\beyond_network.it   # a43e6f19...
test_pattern testdata\itdemo.it           # 1e6a5383...
test_pattern testdata\quests_end.it       # bb5b15bc...
test_pattern testdata\synthscape_filters.it # d561c207...
```

All four must print `IDENTICAL`.

## Scenario 5 — regression of prior F4 behaviour

Instrument list navigation, NNA/DCT/DCA radios, and note audition work as
before; `ITED_SELFTEST=1 ited testdata\itdemo.it` passes.
