# Quickstart: validating SFZ / SF2 import

## Build

```
cmake --build build --config Release --target ited test_pattern
```

## 1. Fixtures + selftest (SC-003, SC-005)

```
python tools/gen_import_tests.py testdata      # also writes testdata/sfi_fixture/
ITED_SELFTEST=1 build/Release/ited.exe testdata/itdemo.it
```
Expect `SFI OK` next to the other blocks. The block checks, for the
fixture SFZ and both SF2 presets: listing (format, name, sample count),
note table, C5Speed per key (±1 Hz of the formula), loops, GvL/DfP, stereo
data equal to the source frames, envelope nodes, filter, reduce-to-fit
with only a few free slots, and the "no free slot" refusal.

## 2. Regression (SC-005)

`test_pattern` on the four modules: hashes as in HANDOFF §4, all
`IDENTICAL`.

## 3. Real collections (SC-001, SC-002, SC-004)

```
build/Release/ited.exe --sfi-test D:/CommonHome/sound/sfz > sfz.log
build/Release/ited.exe --sfi-test D:/CommonHome/sound/sf2 > sf2.log
```
Summary lines give OK/FAIL counts; FAIL lines name the reason. Targets:
≥ 95 % SFZ OK (excluding files whose samples are absent), ≥ 98 % SF2
presets OK.

## 4. By hand

F4 → Enter → browse to an .sfz folder: rows show "SFZ Instrument" and
the sample count; Enter loads; play keys across the keyboard. Enter on an
.sf2 → presets list "000:000 Piano" …; Enter loads; Esc/"." leaves.
