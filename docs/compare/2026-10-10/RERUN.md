# it26 vs IT 2.14: rerun after the fixes for #29-#40

Rerun: 2026-10-10, both sides fresh. IT 2.14 (`IT /S0`) under QEMU +
FreeDOS vs it26 with the fix commits for #29-#40 on top of the harness
commit. Same setup as the first run ([FINDINGS.md](FINDINGS.md)).

```
python tools/compare/compare.py compare-run --script tests/compare/it_screens.hds   --out build-compare/rerun/it_screens
python tools/compare/compare.py compare-run --script tests/compare/f2_after_f9.hds  --out build-compare/rerun/f2_after_f9
python tools/compare/compare.py compare-run --script tests/compare/pe_markers.hds   --out build-compare/rerun/pe_markers
python tools/compare/compare.py compare-run --script tests/compare/header_modes.hds --out build-compare/rerun/header_modes
python tools/compare/compare.py run --data $IT214_DIR --cmd "IT /S5" --script tests/compare/it_play_jeff93.hds --out build-compare/rerun/it_play_jeff93 --audio build-compare/rerun/it_play_jeff93/jeff93.wav
```

`pe_markers.hds` (#34) and `header_modes.hds` (#36) are new.
`it_play_jeff93.hds` is a DOS-only `run` script (audio capture). It has
no port side.

## Status of the findings

| # | Finding | Issue | Fix commit | Rerun |
|---|---|---|---|---|
| F1 | F2 → F9 → F2 opens Pattern Editor Options | #29 | b5d04c2 | matches |
| F2 | Load Module file list: divider, title column | #30 | 1706bb0 | matches |
| F3 | Load Module file info box | #31 | 1706bb0 | matches (values = host file) |
| F4 | Filename mask `*.IT` only | #32 | 1706bb0 | matches |
| F5 | Section titles one column left | #33 | bef6ae4 | matches; "Pattern Edit"/"Pattern Editor" wording is 2.17 source |
| F6 | Copyright year | not filed | - | 2.17 source, unchanged |
| F7 | Pattern editor row-47 markers | #34 | 1a1b12a | matches for all 6 cursor columns |
| F8 | F3 info panel colours | #35 | 484b8c8 | matches |
| F9 | F3 header "Instrument" vs "Sample" | #36 | f8a1af1, 52b3735 | matches on F2/F3/F4/F9/Load Sample, incl. F3 → F4 sample-to-instrument |
| F10 | F4 colours | #37 | 4e86165 (+ 484b8c8 for the Filename field) | matches |
| F11 | F11 order list column, `000` vs `---` | #38 | a957eeb | matches |
| F12 | F12 Song Name colour | #39 | 484b8c8 | matches |
| F13 | Help without Ctrl-D | not filed | - | intentional (9c8bbf6) |
| F14 | Load progress screen; empty screen after load | #40 | 21e5154 | after-load screen matches; progress log for .IT (importers: format line only) |

## What still differs, and why

Every capture of the four compare scripts was diffed cell by cell. The
only rows left are:

- Row 1: copyright `1995-1997` vs `1995-2000` (F6, 2.17 source).
- Rows 6-7, cols 62-79: FreeMem/FreeEMS (masked; DOS memory).
- Row 9, time: the clock (masked).
- Row 11 on the pattern editor: "Pattern Edit (F2)" vs "Pattern Editor
  (F2)" (2.17 source).
- F9 / Load Sample, rows 13-15 and 42-47: drives, directories, file
  dates and the path (host vs FreeDOS D:\).
- F12 rows 42-44: the directory paths.
- Help rows 38-44: Ctrl-D left out (F13).

## New: F15, Load Sample list order

IT 2.14 lists the folder in directory order, with the module
`CHRIS31B.IT` ("Library") between `BUGS.TXT` and `CONTRIB.TXT`. it26
puts directories, then libraries, then files, as `D_SlowSampleSort`
(`IT_DISK.ASM:10673`, sort priority directory / library / recognised /
unknown). In the original, that sort runs in the idle list only after
every file has been identified and the sample cache file saved, and
only while the cursor is on the first entry (`D_LoadSampleNames`,
`IT_DISK.ASM:6283`). Even 8 s after opening the screen, 2.14 under
QEMU had not sorted. A failed cache-file write on D: or a 2.14/2.17
difference could explain this; not investigated further. Not filed; the
port keeps the sorted order for now.
