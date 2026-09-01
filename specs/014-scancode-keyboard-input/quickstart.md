# Quickstart: Validating Scancode-Based Keyboard Input

**Feature**: 014-scancode-keyboard-input

How to prove the feature works. Build commands follow `docs/HANDOFF.md` §3.

---

## Prerequisites

- Windows: MSVC `vcvars64` shell (plain `cmake` is not on PATH; VS BuildTools
  ships cmake+ninja inside that shell). Run builds through the PowerShell tool.
- Linux check: WSL Ubuntu (`wsl -d Ubuntu`). No SDL2 headers there — the Linux
  gate is the terminal backend only.
- A German keyboard layout installed on the host for the manual comparison, plus
  DOSBox with IT 2.14 from `fable5\it214-bin` for the parity reference.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

---

## 1. Automated: the gates that must not move (FR-016, SC-005)

```sh
build/test_pattern testdata/beyond_network.it
build/test_pattern testdata/itdemo.it
build/test_pattern testdata/quests_end.it
build/test_pattern testdata/synthscape_filters.it
build/test_pattern --roundtrip testdata/itdemo.it
```

**Expected**: every module reports `IDENTICAL`; the round trip is stable across
all four modules times three repetitions. These hashes must equal the
pre-change baseline exactly — this feature touches no engine code, so any
movement here means something went wrong.

## 2. Automated: the selftest, including the new keyboard block

```sh
ITED_SELFTEST=1 ITED_TERM=1 ited testdata/itdemo.it
```

**Expected**: all pre-existing blocks still report OK, plus a new `KBD OK`
block. Do not let the script send ENTER while an F3/F4 list widget has focus —
that opens a blocking modal requester.

The keyboard block drives `Screen_KeyFeedTest` (see
[contracts/key-event-api.md](contracts/key-event-api.md)) and asserts:

- **US vs German note parity (FR-014)**: the same `scan` sequence with US
  characters and with German characters produces identical note output, while the
  characters differ. This is SC-001 in automated form and runs on any host.
- **National characters round-trip (FR-015)**: `ä ö ü ß Ä Ö Ü` typed into a
  sample name survive save and reload byte-identically.
- **Rejection (FR-005)**: a character with no CP437 code leaves the field and
  cursor untouched.

---

## 3. Manual: the parity comparison that motivated the feature (SC-001)

On the German keyboard, in the pattern editor in edit mode:

1. Press the lower row left to right — the keys printed `y s x d c v g b h n j m`.
   **Expected**: `C-n C#n D-n D#n E-n F-n F#n G-n G#n A-n A#n B-n` for base
   octave *n*, a continuous chromatic run with no gaps or repeats.
2. Press the upper row — `q 2 w 3 e r 5 t 6 z 7 u i 9 o 0 p`.
   **Expected**: the chromatic run continuing one octave up.
3. Run the same two sweeps in IT 2.14 under DOSBox on the same physical keys.
   **Expected**: identical note sequences. 29 of 29 keys.

The two keys to watch are the ones printed **Z** and **Y**: before this feature
they produce each other's notes.

## 4. Manual: text entry (SC-002)

1. F3, name a sample `Röhrenbass`. F2 song title `Größenwahn`. Message editor
   (F9), type a line with `ä ö ü ß Ä Ö Ü`.
2. Save (F10), quit, reload.
   **Expected**: every character identical, rendered with correct glyphs from the
   feature-013 hi-ASCII font bank.
3. With a layout that can produce one, type a non-CP437 character into a name.
   **Expected**: nothing is entered; the cursor does not move; the field is intact.

## 5. Manual: shortcuts follow the keycap (SC-003)

Press Alt plus the key printed `Z`, then Alt plus the key printed `Y`.
**Expected**: the commands the manual documents as Alt-Z and Alt-Y — not each
other's. Repeat for the Ctrl-letter shortcuts.

## 6. Manual: the layout override (FR-009, FR-010)

1. Copy `fable5\impulsetracker\Keyboard\DE.ASM`'s assembled form (or any shipped
   `.CFG`) next to the executable, set `keyboard_cfg` in `ited.cfg`, restart.
   **Expected**: characters follow the file; the loaded layout is confirmed on
   screen; note positions are unchanged.
2. Truncate the file to a few bytes and restart.
   **Expected**: a clear message, startup continues on the host layout.

## 7. Manual: the diagnostic view (FR-013, SC-007)

Open the keypress diagnostic, press the key printed `Z`.
**Expected**: position code `15h`, character `z`, and the modifier state — which
is exactly the evidence needed to hand-write a layout file.

---

## Terminal backend note

On the terminal backend `scan` is inferred from the character using
`keyboard_layout` in `ited.cfg` (default `us`). A German user must set it there;
keys that differ only by position (main-row versus keypad `/`) remain
indistinguishable. This is a documented limitation, not a defect — see
[research.md](research.md) R8.
