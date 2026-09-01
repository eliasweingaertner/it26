# Contract: KEYBOARD.CFG Layout File Format

**Feature**: 014-scancode-keyboard-input

The optional layout override (FR-009). This is a **compatibility contract with
the original**, not a design of ours: the files Impulse Tracker shipped
(`Keyboard/*.ASM`, assembled to `.COM`) must load unchanged.

Source of truth: the header comment of `fable5\impulsetracker\Keyboard\DE.ASM`,
`Keyboard\KEYBOARD.TXT`, and the parser at `IT_K.ASM:1274-1290`.

---

## File layout

The file is a DOS `.COM` image assembled at `ORG 100h`:

```text
offset 0   : FileLength   DW      ; length of the table that follows
offset 2   : StartKeyboardTable
             <entries>
             EndKeyboardTable
```

## Entry structure

```text
entry   := keycode:u8  condition_list
condition_list := { condition:u8  value:u16 } ... 0FFh
```

- **keycode** — the scancode (the original's `CL`, research R2), matched against
  the physical key. The shipped files write these in decimal (`DB 21` = `15h`).
- **condition** — when this value applies:

| Code | Condition |
|---|---|
| 0 | no Shift/Ctrl/Alt |
| 1 | Shift with Caps Lock off, **or** Caps Lock on; no Ctrl/Alt |
| 2 | Shift with Caps Lock on, **or** Caps Lock off; no Ctrl/Alt |
| 3 | Shift |
| 4 | Ctrl |
| 5 | either Alt |
| 6 | Left Alt |
| 7 | Right Alt (AltGr) |
| 8 | Num Lock on, no Ctrl/Alt |
| 9 | Num Lock off, no Ctrl/Alt |
| 0FFh | end of this key's list |

- **value** — one of:
  - a character (`DW 'a'`),
  - a control code for Ctrl combinations (`DW 1ah` for Ctrl-Z),
  - `scancode << 8` for Alt combinations (`DW 2c00h` = Alt at US position `2Ch`),
    which is the mechanism that makes Alt shortcuts follow the printed keycap
    (research R5).

The list is walked in order; the first matching condition wins. A key absent from
the table produces no character.

## Loader requirements

1. Accept the shipped `.COM` files byte-for-byte; do not require reassembly.
2. Validate `FileLength` against the actual file size and bound every walk by it;
   a truncated or malformed file must be rejected, not walked off the end.
3. On any failure — missing, unreadable, bad length, unterminated list — report
   it to the user and fall back to the host layout (FR-010). Never fail startup.
4. Character values are CP437 already (the original stored DOS characters), so
   they pass through to `it_key_t.ch` without conversion.
5. The loaded table affects `ch` only. It MUST NOT be consulted by note
   resolution (FR-002).

## Selection

`ited.cfg` key `keyboard_cfg` holds the path. Empty or absent = host layout.
