# Contract: Load Sample / Sample Library screen

The UI contract: what is on screen, where, in which colour, and which keys do
what. Every row cites its ASM source (research R1, R5, R6).

## Layout (80×50 cells)

| Element | Cells | Attr | Source |
|---|---|---|---|
| Header | "Load Sample" / "Sample Library (Ctrl-F3)" | header style | LoadSampleHeader / ViewSampleHeader |
| List box | (5,12)-(44,48) style 27 | — | LoadSampleBox |
| Row numbers | col 2, rows 13..47, 3 digits | 20h | PE_ConvAX2Num |
| Divider | col 31, rows 13..47, char A8h | 02h | D_DrawLoadSampleWindow |
| Name / filename | col 6 (25) / col 32 (12) | by type: 6 unidentified, 5 dir, 2 unknown, 3 other | same |
| Cursor | 38 cells from col 6 | 30h, divider cell 32h | D_PreLoadSampleWindow |
| Drive box | (45,12)-(54,23); "Drive X:" from (46,13), 10 rows | 05h, cursor 30h | D_LSDrawDriveWindow |
| Info box | (63,12)-(77,23), labels at (55,13) | 20h | LSInfoBox/Text |
| Fields | Filename (64,13) … SusLEnd (64,20) | 02h, 7-digit zero-pad | LS*Input |
| Quality / Length | (64,21) / (64,22) | 02h | D_DrawLoadSampleWindow |
| Waveform box | (45,24)-(77,29); glyphs 31×4 at (46,25) | 0Dh | LSWaveFormBox |
| Parameter box | (45,30)-(77,42) style 9, labels (48,33) | 20h | LSParametersBox/Text |
| Thumbbars | (63,33),(63,34),(63,37),(63,38),(63,39) | thumbbar | LS*Input |
| File info | box (52,43)-(77,48), labels (46,44), values col 53 | 05h | LSFileInfo* |

Empty listing: "No files." at (6,13), attr 05h.

## Keys (list focused)

| Key | Action |
|---|---|
| Up/Down/PgUp/PgDn/Home/End | move |
| Enter | dir → enter; module → list its samples; sample → load (Load Sample only) |
| Space | rename entry's sample name |
| Right / Tab | focus drive box |
| Delete | delete file from disk after confirmation (default Cancel) |
| Note keys | audition into check slot; waveform appears |
| Esc | leave |

## Capture aid

`ITED_DUMP=13` / `ITED_SHOT_SCREEN=13` → Load Sample; `14` → Sample Library.
`ITED_SHOT_DIR=<path>` selects the listed directory (default `testdata`).
