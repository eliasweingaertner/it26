# Phase 1 Data Model: Scancode-Based Keyboard Input

**Feature**: 014-scancode-keyboard-input
**Date**: 2026-08-20

Entities are named after their originals where one exists (constitution I/II).

---

## 1. `it_key_t` — one key event

Mirrors the original's `CX`/`DX` pair (research R1/R2). Replaces the bare `int`
that `Key_Get()` returns today.

| Field | Type | Meaning |
|---|---|---|
| `scan` | `uint8_t` | PC set-1 scancode; `+0x80` for the E0-extended variant. `0` when the source cannot supply one (terminal backend, synthesized events). |
| `flags` | `uint8_t` | The original's `CH`: bit0 pressed, bit1 LShift, bit2 RShift, bit3 LCtrl, bit4 RCtrl, bit5 LAlt, bit6 RAlt(AltGr). |
| `ch` | `uint16_t` | Translated character as CP437, or `0` when the key produced none. |
| `code` | `int` | The existing legacy code: an ASCII character or an `ITK_*` value. Preserved so the 14 existing call sites and 56 `case '...'` bindings keep working during and after migration. |

**Validation rules**

- `scan == 0` is legal and means "position unknown"; consumers that require a
  position fall back to the reverse map (R8) or ignore the event.
- `ch` is CP437, never Unicode. Values `>= 0x80` require the feature-013 hi-ASCII
  font bank to render.
- `flags` bit0 clear = key release. Only press events carry a `ch` (the original
  translates on press only, `IT_K.ASM:1251`).

**Derived predicates** (helpers, not stored): `shift = flags & 6`,
`ctrl = flags & 24`, `alt = flags & 96`, `altgr = flags & 64`.

---

## 2. Note position table

The 29 `(scancode, semitone)` pairs decoded in research R3, transliterated from
`IT_I.ASM:333` with its `0FFFFh` terminator. Immutable, layout-independent, and
the only input to note resolution.

| Field | Type | Meaning |
|---|---|---|
| `scan` | `uint8_t` | Physical position |
| `semitone` | `uint8_t` | Offset from the base octave, 0..28 |

**Relationship**: consumed by `key_to_note()`; independent of the layout
definition. Changing the active layout MUST NOT change this table's effect.

---

## 3. Layout definition

Maps a scancode to the character it produces under a modifier condition. Two
sources, one shape.

| Field | Type | Meaning |
|---|---|---|
| `scan` | `uint8_t` | Key position |
| `condition` | `uint8_t` | Original's condition code 0-9 (research R4) |
| `value` | `uint16_t` | Character, control code, or `scancode << 8` for an Alt remap |

**Sources**

- *Host* (default): not materialised as a table at all — the OS supplies the
  character directly per press. The struct above is the file-backed form only.
- *File*: parsed from `KEYBOARD.CFG` in the original's format.

**State transitions**: `none -> host` at startup; `host -> file` if a valid file
is configured; `file -> host` on any parse or read failure, with a message.

---

## 4. Scancode source map (SDL only)

Static `SDL_SCANCODE_* -> PC set-1` table. Needed because SDL reports USB HID
codes (research R6). Covers the keys the ported ASM tables reference; unmapped
codes yield `scan == 0`.

---

## 5. CP437 mapping

Bidirectional correspondence between the host's Unicode characters and the byte
codes the tracker stores and renders (research R7).

| Direction | Use |
|---|---|
| Unicode -> CP437 | text field input; unmapped input is rejected |
| CP437 -> glyph | rendering, already provided by feature 013 |

---

## 6. Configuration keys (`ited.cfg`)

| Key | Values | Default | Effect |
|---|---|---|---|
| `keyboard_cfg` | path or empty | empty | Layout definition file to load; empty = host layout |
| `keyboard_layout` | layout id | `us` | Reverse map used by the terminal backend only (research R8) |

---

## Entity relationships

```text
backend key event ──► it_key_t ──┬──► note position table ──► note
   (scan, flags)                 │        (scan only)
   (ch via host layout           │
    or layout definition)        ├──► text field ──► CP437 mapping ──► module file
                                 │        (ch only)
                                 └──► Alt/Ctrl shortcut dispatch
                                          (ch / legacy code only)
```

The two arms never cross: no path from `ch` into note resolution, and no path
from `scan` into text entry. That separation is the whole feature.
