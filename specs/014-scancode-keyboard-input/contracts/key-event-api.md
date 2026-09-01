# Contract: Key Event API

**Feature**: 014-scancode-keyboard-input

The internal interface between the screen backends and the editor. This is the
contract that changes; everything else in this feature follows from it.

---

## Current shape (to be superseded)

```c
/* it_screen.h */
int Key_Get(void);              /* ASCII char, or ITK_* code, or ITK_NONE */

typedef struct screen_backend_t {
    ...
    int (*key)(void);
} screen_backend_t;
```

Single value; no way to express "physical position" (research R1).

---

## New shape

```c
/* it_screen.h */
typedef struct {
    uint8_t  scan;      /* PC set-1 scancode, +0x80 = E0-extended; 0 = unknown */
    uint8_t  flags;     /* CH bits: 1 pressed, 2 LShift, 4 RShift, 8 LCtrl,
                         *          16 RCtrl, 32 LAlt, 64 RAlt(AltGr)        */
    uint16_t ch;        /* CP437 character, or 0 if the key produced none    */
    int      code;      /* legacy: ASCII char or ITK_* value                 */
} it_key_t;

/* Returns 0 when no key is pending, else 1 and fills *k. */
int Key_GetEvent(it_key_t *k);

/* Retained verbatim. Equivalent to Key_GetEvent() and returning .code.
 * Every existing call site keeps compiling and behaving identically. */
int Key_Get(void);

typedef struct screen_backend_t {
    ...
    int (*key)(void);               /* retained; returns .code               */
    int (*key_event)(it_key_t *k);  /* NULL = backend cannot supply events;
                                     * the wrapper then synthesizes one from
                                     * key() with scan resolved by reverse map */
} screen_backend_t;
```

### Guarantees

1. **`Key_Get()` is unchanged in behaviour.** Migration is incremental; a backend
   or call site that has not moved yet is not broken by one that has.
2. **`scan` is the original's `CL`.** Same numbering as every ported ASM table,
   including the `+0x80` extended convention and the right-Ctrl `9Dh` /
   right-Alt `0B8h` codes (research R2).
3. **`flags` is the original's `CH`.** Bit 0 clear means key release.
4. **`ch` is CP437, not Unicode.** Conversion happens in the backend, at the
   boundary, so no consumer ever sees a Unicode code point.
5. **A backend that cannot supply `scan` sets it to 0** rather than guessing. The
   wrapper fills it from the configured reverse map when one applies.

### Consumer rules

| Consumer | Reads | Must not read |
|---|---|---|
| `key_to_note()` | `scan` | `ch`, `code` |
| Position-dispatched pattern-editor bindings | `scan` | `ch` |
| Text fields, message editor, filename requesters | `ch` | `scan` |
| Alt/Ctrl letter shortcuts | `code` (or `ch`) | `scan` |
| Function/cursor/editing keys | `code` (`ITK_*`) | — |

---

## Backend obligations

| Backend | `scan` | `ch` | Notes |
|---|---|---|---|
| Win32 | `lParam` bits 16-23, `+0x80` if bit 24 | `WM_CHAR`/`WM_SYSCHAR`, converted to CP437 | Pair the pending `WM_KEYDOWN` scancode with the following char message |
| SDL2 | `keysym.scancode` through the HID -> set-1 table | `SDL_TEXTINPUT`, UTF-8 decoded then converted to CP437 | Same pairing rule |
| Terminal | 0, then filled by the reverse map | parser's decoded character | Position for same-character-different-key cases is unrecoverable (research R8) |

---

## Test hooks

```c
/* Feed a synthetic event; mirrors Screen_TermFeedTest from feature 011.
 * Lets the selftest exercise US and German layouts on any host. */
int Screen_KeyFeedTest(const it_key_t *k, int n);
```

The selftest drives this with identical `scan` sequences and differing `ch`
values for the two layouts, and asserts the notes match while the characters
differ (FR-014).
