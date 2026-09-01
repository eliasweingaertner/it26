# Phase 0 Research: Scancode-Based Keyboard Input

**Feature**: 014-scancode-keyboard-input
**Date**: 2026-08-20

This is the ASM decode contract for the feature. Per constitution principle II,
implementation pulls behaviour from these findings, not from guesswork. Line
references are into the BSD source at `fable5\impulsetracker`.

---

## R1 — The original's key event is TWO values, not one

**Decision**: Model a key event as `(input, translated)` exactly as the original does.

**Evidence**: `IT_K.ASM:1108`

```asm
Proc            K_GetKey Far                    ; CX/DX = input/translated
```

`CX` is the raw input (physical position + modifier state); `DX` is the character
produced by the active translation table. Every consumer in the original picks
one or the other. Our port collapsed both into a single `int` (`it_screen.h:127`,
`Key_Get()` at `it_screen.c:1111`), which is the root cause of the reported bug —
no downstream remapping can recover information the API never carried.

**Alternatives considered**: Keeping one value and remapping characters back to
positions at the point of use. Rejected: the mapping is not injective (dead keys
produce no character at all; AltGr layers produce characters with no US
equivalent), and it would have to be repeated at all 14 `Key_Get()` call sites.

---

## R2 — CX bit layout (the "input" half)

**Decision**: `CL` = scancode byte, `CH` = press flag + live modifier state.

**Evidence**: `IT_K.ASM:1174-1195` builds CX:

- `And AL, AL / JS ... / Inc BH` — BH becomes 1 for a make code (high bit clear
  = key pressed), stays 0 for a break code (key released).
- `And AX, 07Fh` strips the break bit; `Add SI, AX`; `Mov CX, SI / Mov CH, BH`.
- E0-prefixed (extended) keys take `Add SI, 128` at `IT_K.ASM:1156`, so the
  extended variant of a key is `scancode + 0x80`. Confirmed by the modifier
  probes below, which read right-Ctrl at `9Dh` (`1Dh + 80h`) and right-Alt at
  `0B8h` (`38h + 80h`). E1 (Pause) is folded to `SI = 64`.

`IT_K.ASM:1216-1250` then ORs the live modifier state into CH:

| CH bit | Value | Meaning                          | Probe             |
|--------|-------|----------------------------------|-------------------|
| 0      | 1     | key pressed (0 = released)       | make/break bit    |
| 1      | 2     | Left Shift held                  | `KeyBoardTable+2Ah`  |
| 2      | 4     | Right Shift held                 | `KeyBoardTable+36h`  |
| 3      | 8     | Left Ctrl held                   | `KeyBoardTable+1Dh`  |
| 4      | 16    | Right Ctrl held                  | `KeyBoardTable+9Dh`  |
| 5      | 32    | Left Alt held                    | `KeyBoardTable+38h`  |
| 6      | 64    | Right Alt (AltGr) held           | `KeyBoardTable+0B8h` |

So a plain key-down is `CX = 0x100 | scancode`, which is exactly why the note
table's entries read `12Ch`, `11Fh`, `12Dh` — `0x100 | 0x2C`, `0x100 | 0x1F`,
`0x100 | 0x2D`. Caps Lock (`13Ah`) and Num Lock (`145h`) are toggled inside
`K_GetKey` itself (`IT_K.ASM:1196-1210`), guarded by `LastKey` so they fire only
on the first press, not on autorepeat.

---

## R3 — Note entry matches the scancode ONLY

**Decision**: `key_to_note()` must take the scancode and ignore the character.

**Evidence**: `IT_I.ASM:333`

```asm
KeyBoardTable   DW  12Ch, 0, 11Fh, 1, 12Dh, 2, 120h, 3, 12Eh, 4
                DW  12Fh, 5, 122h, 6, 130h, 7, 123h, 8, 131h, 9
                DW  124h, 10, 132h, 11, 110h, 12, 103h, 13, 111h, 14
                DW  104h, 15, 112h, 16, 113h, 17, 106h, 18, 114h, 19
                DW  107h, 20, 115h, 21, 108h, 22, 116h, 23, 117h, 24
                DW  10Ah, 25, 118h, 26, 10Bh, 27, 119h, 28, 0FFFFh
```

and the lookup at `IT_I.ASM:1332-1345`:

```asm
Mov     SI, Offset KeyBoardTable
LodsW
Cmp     AX, 0FFFFh
JE      ...
Mov     BX, AX
LodsW
Cmp     BL, CL          ; <-- low byte only: the SCANCODE
JNE     ...
```

`Cmp BL, CL` compares only the low bytes. The `1xx` high byte is never tested
here; the press/release distinction is handled separately by `Test CH, Not 1`
and `And CH, CH` around it. The same table is consumed at `IT_I.ASM:5022`,
`IT_I.ASM:5562` and `IT_DISK.ASM:5805`.

Decoded, the 29 entries are:

| Scancode | US key | Semitone | | Scancode | US key | Semitone |
|---|---|---|---|---|---|---|
| 2Ch | Z | 0  | | 10h | Q | 12 |
| 1Fh | S | 1  | | 03h | 2 | 13 |
| 2Dh | X | 2  | | 11h | W | 14 |
| 20h | D | 3  | | 04h | 3 | 15 |
| 2Eh | C | 4  | | 12h | E | 16 |
| 2Fh | V | 5  | | 13h | R | 17 |
| 22h | G | 6  | | 06h | 5 | 18 |
| 30h | B | 7  | | 14h | T | 19 |
| 23h | H | 8  | | 07h | 6 | 20 |
| 31h | N | 9  | | 15h | Y | 21 |
| 24h | J | 10 | | 08h | 7 | 22 |
| 32h | M | 11 | | 16h | U | 23 |
|    |   |    | | 17h | I | 24 |
|    |   |    | | 0Ah | 9 | 25 |
|    |   |    | | 18h | O | 26 |
|    |   |    | | 0Bh | 0 | 27 |
|    |   |    | | 19h | P | 28 |

On a German QWERTZ board scancode `15h` is the key printed **Z** and `2Ch` is the
key printed **Y** — which is precisely why the reporting user's `y s x d c v g b
h n j m` and `q 2 w 3 e r 5 t 6 z 7 u` sweeps work in DOSBox and fail here.

---

## R4 — The translation table (the "translated" half)

**Decision**: Port the table format as the optional override; use the host layout
as the default source of the same information.

**Evidence**: `IT_K.ASM:96-98` holds a replaceable far pointer:

```asm
TranslationTable        Label   DWord
TranslationTableOffset  DW      Offset  USKeyboardTable
TranslationTableSegment DW      Keyboard
```

`K_GetKey` translates only on a press (`IT_K.ASM:1251`, `Test CH, 1`) and walks
the table at `IT_K.ASM:1274-1290`: read a keycode byte, compare against `CL`
(the scancode again), then walk `(condition, word)` pairs until `0FFh`.

Format, from the header comment of `Keyboard/DE.ASM` and `Keyboard/KEYBOARD.TXT`:

- File is a `.COM` at `ORG 100h` opening with `FileLength DW`.
- Body: keycode byte, then repeated (condition byte, return-value word), then `0FFh`.
- Conditions: `0` no modifier · `1` shift-with-caps-off **or** caps-on ·
  `2` shift-with-caps-on **or** caps-off · `3` shift · `4` ctrl ·
  `5` either alt · `6` left alt · `7` right alt (AltGr) · `8` numlock on ·
  `9` numlock off · `0FFh` end of list.
- IT shipped 16 tables (`BE CA CH DE DK ES FI FR IT NO PO SE UK US` and peers);
  the user copies one to `KEYBOARD.CFG` next to the executable.

---

## R5 — Alt/Ctrl shortcuts are LABEL-based, and the original says so explicitly

**Decision**: Resolve Alt-letter and Ctrl-letter shortcuts through the character
layer, not the position layer.

**Evidence**: `Keyboard/DE.ASM` — the two swapped keys carry Alt return values
that point at the *other* key's US scancode:

```asm
DB  21          ; Z      <- scancode 15h, the physical US-Y position
 DB 1 / DW 'Z'  ; types Z
 DB 2 / DW 'z'
 DB 4 / DW 1ah  ; Ctrl-Z control character
 DB 5 / DW 2c00h ; Alt   -> scancode 2Ch, i.e. US Alt-Z
 DB 0FFh

DB  44          ; Y      <- scancode 2Ch, the physical US-Z position
 DB 1 / DW 'Y'
 DB 2 / DW 'y'
 DB 4 / DW 19h
 DB 5 / DW 1500h ; Alt   -> scancode 15h, i.e. US Alt-Y
 DB 0FFh
```

This settles the design question outright: **note entry is positional, Alt/Ctrl
shortcuts follow the printed keycap.** Alt combinations are returned as
`scancode << 8` (BIOS-style, low byte zero); Ctrl combinations are returned as
the corresponding control character (`1ah` for Ctrl-Z, `19h` for Ctrl-Y).

**Consequence for the port**: our Win32 backend already gets this accidentally
right, because Windows assigns letter VK codes by produced character rather than
by position (`it_screen_win32.c:216`, `wp >= 'A' && wp <= 'Z'`). The SDL backend
also uses `keysym.sym`, which is layout-mapped. So story P3 is mostly a
*preservation* requirement — do not regress it while making notes positional.

---

## R6 — Where the host gives us each half

**Decision**: Take the scancode from the OS key event and the character from the
OS text event, pairing them per key press.

| Backend | Scancode source | Character source |
|---|---|---|
| Win32 (`it_screen_win32.c`) | `WM_KEYDOWN`/`WM_SYSKEYDOWN` `lParam` bits 16-23 = OEM set-1 scancode; bit 24 = extended flag | `WM_CHAR` / `WM_SYSCHAR` (already layout-translated, honours dead keys and AltGr) |
| SDL2 (`it_screen_sdl.c`) | `e.key.keysym.scancode` (`SDL_SCANCODE_*`, USB HID numbering) | `SDL_TEXTINPUT` (UTF-8, already composed) |
| Terminal (`it_screen.c`) | not available — see R8 | the decoded character from the feature-011 parser |

Win32 is a direct fit: `lParam` bits 16-23 plus `0x80` when bit 24 is set yields
exactly the original's `CL`, including the right-Ctrl `9Dh` / right-Alt `0B8h`
convention from R2. **No translation table is needed on Windows.**

SDL is USB HID numbering, not PC set-1, so a static `SDL_SCANCODE_* -> set-1`
table is required. It only needs to cover the keys the ported ASM tables
reference (the 29 note keys, the punctuation keys, and the modifier/extended
keys already handled), not all 240-odd HID codes.

Pairing rule: a key press records its scancode as pending; the text event that
immediately follows attaches to it. A press with no following text event (a
cursor key, a dead key) yields an event with a scancode and no character. This
also handles composed sequences correctly, since the OS delivers the composed
character as a single text event.

**Alternatives considered**: deriving the character ourselves from the scancode
plus a bundled layout table on every platform (i.e. always using the R4 path).
Rejected — it would reimplement what the OS already does correctly, and would
silently break for layouts we did not bundle, dead keys, and IMEs.

---

## R7 — Character set: what reaches the module file

**Decision**: Translate the host's Unicode character to CP437 for storage and
rendering; reject anything CP437 cannot represent.

Module files store single bytes, and the editor already renders the high range
through the CP437 glyphs installed by feature 013
(`Screen_DefineHiASCII`, `IT_FontROM` into font bank B). So the mapping needed is
Unicode -> CP437 over the 128 high codes; the seven German letters land at
`ä 84h · ö 94h · ü 81h · ß E1h · Ä 8Eh · Ö 99h · Ü 9Ah`. Characters with no
CP437 code (Polish `ł`, Cyrillic, CJK) are rejected at the field-input layer,
leaving the field and cursor untouched — transliterating them would silently
corrupt the user's text, and the file format cannot carry them.

---

## R8 — Terminal backend: an honest limitation

**Decision**: Infer position from the character using a configured layout;
default US; document what is lost.

A standard terminal delivers characters (and the feature-011 escape sequences),
never physical key positions. There is no way to recover the scancode for a
letter key. The reverse map is therefore layout-dependent: with the default US
assumption the terminal backend behaves exactly as the port does today; a German
user sets the layout in `ited.cfg` and gets a German reverse map.

What remains unavailable on the terminal backend regardless: distinguishing keys
that produce the same character at different positions (main-row `/` versus
keypad `/`), and any key whose character the terminal does not transmit. This is
stated to the user rather than papered over.

---

## R9 — Open items to verify during implementation

1. **Keypad-divide code.** `it_screen.h:151` documents `ITK_KP_DIVIDE` as "scan
   135h", but by R2 the E0-extended keypad `/` is `1B5h` (`0x35 + 0x80`, pressed)
   while `135h` is the main-row `/`. Re-read the `MuteNext` binding in
   `IT_PE.ASM` and correct whichever side is wrong; note it as a deviation if the
   original really does use the main-row code.
2. **Alt+keypad numeric entry.** `IT_K.ASM:1259-1271` implements the DOS
   Alt-plus-keypad-digits character entry via `KeypadValueFlag`, releasing the
   composed character on Alt release. This is how a DOS user typed arbitrary
   CP437 codes. Not required by the spec; record whether the host layout path
   already provides an equivalent before deciding to port it.
3. **Ctrl-F1 keypress table — layout NOT decoded before implementing.**
   The screen was built from the `Keyboard/*.ASM` header comment alone
   ("the value in the keypress table in IT on Ctrl-F1"), without reading
   `K_DrawTables` (`IT_K.ASM:1522`). The original draws two 256-entry hex
   grids -- `KeyboardBuffer` at cell (2,15) and the `KeyBoardTable`
   key-down map at (29,15), 32 rows x 8 columns each, with attribute 3
   instead of 2 on any scancode currently held (`Cmp AL,1 / SBB AH,-1`).
   The port's screen is a 12-row event log instead: a constitution II
   deviation, now flagged in README and HANDOFF. A faithful port needs a
   live 256-entry key-down table, which requires key-up events for every
   key; the backends report releases for Shift only.

4. **`LastKey` autorepeat suppression.** The original suppresses repeated
   identical `CX` values for Caps/Num Lock only. Confirm the port's autorepeat
   behaviour for note keys matches the original before and after the change.
