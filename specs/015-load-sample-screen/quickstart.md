# Quickstart: validating the Load Sample screen

Build per `docs/HANDOFF.md` §3 (MSVC line now includes `it_cornerart.c`).

## 1. Gates that must not move

```sh
test_pattern testdata/<m>.it            # x4, IDENTICAL, hashes as baseline
test_pattern testdata/<m>.it --roundtrip  # 12/12
ITED_SELFTEST=1 ITED_TERM=1 ited testdata/itdemo.it   # all blocks OK + LSS
```

## 2. Automated screen check

The new `LSS` selftest block builds a listing over a generated fixture folder
(two subdirectories, one WAV, one ITS, one junk file) and asserts: directories
first as dotted "Directory" rows, `\` pinned first, junk listed as unknown,
preview fields of the WAV/ITS equal what F3 shows after loading them, and an
edited loop begin survives the load.

## 3. Visual parity

```sh
ITED_DUMP=13 ITED_SHOT_DIR=<folder> ited     # plain-text dump
ITED_SHOT=ls.bmp ITED_SHOT_SCREEN=13 ITED_SHOT_DIR=<folder> ited
```

Compare against IT 2.14 in DOSBox on the same folder (user's screenshot
`load-sample.png` is the reference): box positions, row numbers, dotted
directory rows, divider, drive box, info box labels, file-info formatting
(`September 26, 2026`, `10:52pm`, 9-digit size).

## 4. Manual

Browse, audition a sample (waveform appears), edit loop begin, move away and
back (edit still there), load (F3 shows the edit), enter a module (its samples
listed), Ctrl-F3 (Sample Library header), Delete on a scratch file (confirm,
file gone).
