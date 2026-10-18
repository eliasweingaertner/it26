# Quickstart: validating the rendering extensions

Prerequisites: a build of `ited` (`tools\compare\build_port.bat` or CMake),
`testdata/` modules, an empty test folder for Quicksave.

## Automated

1. `build-compare/_work/selftest.ps1` → all blocks OK, including **RND**:
   names (LL/PFX/HHMMSSnn/SONGnn), render length = rows × row time, two
   renders byte-identical, import fields and slot rule, solo sweep file
   count, sample export bytes, Quicksave fallback order, invalid folder abort.
2. `test_pattern` → four modules `IDENTICAL` (engine unchanged).
3. Comparison harness: the existing `tests/compare/*.hds` scripts show no new
   differences (IT 2.14 has none of these keys).

## Manual / port-only scripts

1. F12: type the test folder into "Quicksave", Save all Preferences; restart → still set.
2. Load `testdata/itdemo.it`, F2, Ctrl-O → new sample "Pattern 000" in slot max(samples, instruments)+1, looped, 44100 Hz, 16-bit; status `Pat 000 -> LL<time>.WAV -> sample NNN` (8.3 name in the sample's field only).
3. Play the song (F5), go to F2, Shift-Ctrl-O → `<Song> - pattern 000 - <date time>.wav` in the Quicksave folder (32-bit float, Shift-F5 rate), playback resumes at the same order/row.
4. F11: Right until digit 2, Right → import; Shift-Right → file; Shift-G / Ctrl-G likewise; plain G still jumps.
5. F5: D → a folder `<Song> - samples <date time>` with `NN <sample name>.wav` per sample, playback continues; Shift-Right → pattern file.
6. F3: Ctrl-Alt-Right on a sample → one file with a 44100 Hz header; Ctrl-Right still goes to the next order; Alt-W still saves the sample.
7. Mute channel 2, F2 Shift-Alt-M → one file per non-empty unmuted channel; mutes unchanged.
8. F10: select MWAV, name `song.wav`, Enter → `song - ch01.wav`… for unmuted channels; Esc during it stops after the current channel.
9. Alt-W → module saved in the Quicksave folder; clear the folder, Alt-W → "Quick-save folder not configured." and F10.
10. `ited --render-patterns testdata/itdemo.it` → one WAV per pattern with data, exit 0, no window; `--dump-samples` likewise.
