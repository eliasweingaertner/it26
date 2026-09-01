# T026 — Binding Audit: position-dispatched vs character-dispatched

**Feature**: 014-scancode-keyboard-input
**Date**: 2026-08-20

Purpose: decide, for each editor binding, whether it must resolve from the
physical key position (`it_key_t.scan`) or from the translated character
(`it_key_t.ch` / `.code`). Getting this backwards on a non-US layout either
scatters a binding to a strange physical key or moves it away from its printed
keycap.

## The original answers this itself

`IT_PE.ASM`'s key table is not a flat list of codes — every entry opens with a
**type byte** that says which dispatch mode it uses. From `IT_PE.ASM:790-812`:

```asm
        DB      3       ; Ctrl...
        DW      106h    ; '5'                <- code: 100h | scancode 06h
        DW      Offset PEFunction_Ctrl5

        DB      1
        DW      '\'                          <- CHARACTER
        DW      Offset PEFunction_Alt_F9

        DB      0
        DW      135h                         <- code: 100h | scancode 35h
        DW      Offset PEFunction_MuteNext

        DB      1
        DW      '?'                          <- CHARACTER
        DW      Offset PEFunction_MutePrevious
```

| Type byte | Meaning | Resolve from |
|---|---|---|
| 0 | raw key code (`100h \| scancode`) | **position** |
| 1 | character | **character** |
| 2 | Alt + key code | position, but see note |
| 3 | Ctrl + key code | position, but see note |

Tally over the table (`IT_PE.ASM:600-1000`), 71 entries:

| Type | Count | Kind |
|---|---|---|
| 0 | 15 | position |
| 1 | 36 | character |
| 2 | 8 | Alt + code |
| 3 | 12 | Ctrl + code |

**Note on types 2 and 3.** These name a scancode, but `Keyboard/DE.ASM` remaps
Alt and Ctrl values per key so that the *printed keycap* selects the command
(research R5: the German table returns `2c00h` for Alt on the key labelled Z,
which is the US Z scancode). The net effect on a non-US layout is
label-based dispatch, which is what our Win32 VK codes and SDL keysyms already
produce. **No change needed** — this is the preservation requirement of US3.

## Type-0 (position) entries and their port status

The 15 type-0 entries are dominated by keys that have no character at all, where
position versus character is a distinction without a difference:

| Code | Key | Port status |
|---|---|---|
| `146h` | Scroll Lock | `ITK_SCROLL_LOCK` — distinct code, correct |
| `10Fh` | Tab | `ITK_TAB` — distinct code, correct |
| `1C8h`/`1D0h`/`1CBh`/`1CDh` | Up/Down/Left/Right | `ITK_*` — correct |
| `1C9h`/`1D1h` | PgUp/PgDn | `ITK_*` — correct |
| `0B02h`..`0B05h` | LShift+LCtrl+`1`..`4` | `ITK_CTRL_SHIFT_1..4` — correct |
| **`135h`** | **main-row `/`** | **was wrong — fixed, see below** |

Extended keys arrive as `1xxh` because `K_GetKey` adds 128 to E0-prefixed
scancodes (`IT_K.ASM:1156`), e.g. Up = `E0 48` → `0xC8` → `1C8h`. Our `ITK_*`
codes already carry these as distinct values, so they were never exposed to the
layout problem.

## The one real defect found: `135h`

`it_screen.h:151` documented `ITK_KP_DIVIDE` as "MuteNext, scan 135h — distinct
from the free main-row `/`". Both halves were wrong:

- `135h` is `100h | 35h` = the **main-row `/`** position. The keypad divide is
  `E0 35` → `1B5h`, and `1B5h` appears nowhere in the table.
- So the original binds `PEFunction_MuteNext` to the main-row `/` *position*,
  and `PEFunction_MutePrevious` to the *character* `?` — the same physical key
  on a US board, split by Shift. Our port had MuteNext on the keypad and left
  the main-row key doing nothing.

**Fix applied** (`src/it_editor.c`, `pe_mute_next()` + the position check at the
top of `handle_pattern_key`): the main-row `/` position now fires MuteNext, as
the original does. On a German board that is the `-` key, which is the authentic
positional result.

**Deviation deliberately kept and flagged** (constitution II): the keypad `/`
also still fires MuteNext. That binding is not in the original's table, but it is
behaviour this port has shipped since feature 010 and the `PE2` selftest block
asserts on it. The authentic binding was added alongside rather than replacing
it, so nothing regresses.

## Type-1 (character) entries — no action

All 36 stay on the character layer, which is where the port already had them.
This includes the mute/solo family (`\`, `?`, `|`), the view toggles, and the
letter commands. On a German keyboard these follow the printed keycap, matching
DOSBox.

## Scope note

This audit covers `IT_PE.ASM`'s pattern-editor table, which is where positional
dispatch actually occurs and where the reported bug lived. The `IT_F.ASM`,
`IT_DISK.ASM`, and `IT_OBJ1.ASM` tables were spot-checked for type-0 entries
carrying printable characters and none were found — their bindings are function
keys and characters. If a future fidelity report points at one of those screens,
re-run this tally against that file.
