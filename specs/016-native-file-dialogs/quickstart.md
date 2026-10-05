# Quickstart: Validating Native File Dialogs

Feature: [spec.md](./spec.md) · Contracts: [keys-and-help](./contracts/keys-and-help.md),
[backend-dialog](./contracts/backend-dialog.md)

All commands run from `it26/`.

## 1. Build

```sh
# Windows (Developer PowerShell / vcvars64): CMake as in CI
cmake -B build && cmake --build build --config Release --target ited
# macOS
./build_mac.sh
# Linux (SDL)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DITED_SDL=ON && cmake --build build --target ited
```

## 2. Automated checks (any platform, headless)

```sh
ITED_SELFTEST=1 ITED_TERM=1 ./ited testdata/itdemo.it
```
Expected: all existing blocks OK **plus** `[DLG OK]`. The DLG block uses
`ITED_DIALOG_FAKE` internally (see backend contract) and verifies: open =
same song as F9 load; Save As byte-identical to F10 in IT and S3M; Ctrl-O
sample/instrument = same slot contents as the load screens; F12 field set;
cancel changes nothing; overlong path rejected; `?` display substitution;
Shift-F9 still opens the message editor.

Determinism gate (engine untouched, must stay green):

```sh
./build/test_pattern     # all four testdata modules IDENTICAL
```

## 3. Manual checks per platform

Use an empty song or audio routed to a silent device when testing
playback.

| # | Do | Expect |
|---|---|---|
| 1 | Pattern editor, Ctrl-Shift-F9, pick a module in another folder | module loads; F9 afterwards lists that folder |
| 2 | Ctrl-Shift-F9, Cancel | nothing changes, no key stuck (try Shift marking, Right Option preview on Mac) |
| 3 | Play a song, Ctrl-Shift-F9, wait 10 s, Cancel | playback never stops |
| 4 | Ctrl-Shift-F10, new folder, name `test` (no extension) | `test.it` written; reload = same song |
| 5 | Ctrl-Shift-F10, name `test.s3m` | S3M written (same warnings as F10 S3M export) |
| 6 | Ctrl-Shift-F10 onto an existing file | the tracker asks once to overwrite |
| 7 | F3, slot 05, Ctrl-O, pick a WAV | slot 05 loaded; same as via Load Sample screen |
| 8 | F3, Ctrl-O, pick a module | the sample library opens inside that module (as the library requester's Enter does) |
| 9 | F4, Ctrl-O, pick an `.iti` | instrument loaded into the current slot |
| 10 | F12, focus Sample path, Ctrl-O, pick folder | field shows it; Load Sample opens there |
| 11 | F12, focus a non-path control, Ctrl-O | nothing |
| 12 | Shift-F9 | message editor (unchanged) |
| 13 | Fullscreen, Ctrl-Shift-F9 | dialog in front, focus in dialog; fullscreen restored after |
| 14 | Hold Ctrl-Shift-F9 | one dialog only |
| 15 | macOS: F1 in pattern editor | "Right Option+Key  Preview note", columns unchanged |
| 16 | Windows/Linux: F1 in pattern editor | original "Caps Lock+Key" line |
| 17 | F1 on F3/F4/F12 and global section | "it26 additions" lines present |
| 18 | Linux without zenity/kdialog | status "No file dialog available" |
| 19 | macOS/Linux: pick a file whose name has emoji | loads; name shows `?` for the emoji |
| 20 | Windows: pick a file named with characters outside the ANSI code page | loads (8.3 short path used); name shows `?` |
| 21 | Terminal backend (`ITED_TERM=1`): Ctrl-Shift-F9 | status "No file dialog available" |

## 4. Docs

README "Fidelity notes" lists the new keys and the macOS help wording;
CHANGELOG has an entry.
