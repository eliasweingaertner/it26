# Quickstart: Sample / Instrument Library

## Build & gates

```
cd C:\Users\elias\fable5\ittrack
cmd /c "call \"...\vcvars64.bat\" && cl ... /Fe:ited.exe ... src\it_ris.c ..."   # see HANDOFF §3
test_pattern <each of the 4 testdata modules> --roundtrip                        # all IDENTICAL
python tools\gen_import_tests.py testdata                                        # once
set ITED_SELFTEST=1 & set ITED_TERM=1 & ited testdata\itdemo.it                  # expect LIB OK
```

## Manual walkthrough (US1..US3)

1. `ited testdata\itdemo.it`, F3, pick an empty slot (e.g. 20), press
   **Enter** → the Load Sample requester lists modules/sample files.
2. Cursor a module: the info box shows its sniffed format name. Enter →
   the library view (row 1 = `····Directory····` exit row, then
   records: name, format, length).
3. Play note keys (Z/S/X… Q/2/W…) — the selected record auditions
   through check slot 100 without being imported (US3).
4. Enter on a record → it lands in slot 20 (occupied slots ask
   "Replace sample N?"), back on F3; audition with the piano keys
   (US1). The rest of the song is untouched.
5. F4, pick a slot, **Enter** → Load Instrument; the header row shows
   "Available Samples: n". Enter an .IT/.XM module → instrument
   records with sample counts; Enter one → instrument + its samples
   land in free slots, note table remapped; if the song is in sample
   mode you're asked "Enable instrument mode?" (US2). With too few
   free slots the load refuses with "Out of sample space!".
6. Direct file loads: Enter on `.ITS` loads the sample straight into
   the slot; `.ITI`/`.XI` likewise for instruments.
7. Saves: F3 **Alt-O/T/W** = .ITS/Scream Tracker/WAV, F4 **Alt-O** =
   .ITI (uses the DOS filename field; empty → "(No Filename?)").

## Screenshots (headless)

```
set ITED_SHOT=lib.bmp & set ITED_SHOT_SCREEN=10 & ited testdata\itdemo.it   # sample library
set ITED_SHOT_SCREEN=11 ...                                                 # instrument library
set ITED_SHOT_LIB=testdata\import_test.xm ...                               # other source
```
