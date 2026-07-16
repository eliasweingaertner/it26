# Contract: C API surface touched by feature 008

No public API additions — the feature extends existing contracts. Callers
(`it_editor.c`) compile unchanged except the one dispatch tweak listed.

## `it_ris.h` (unchanged signatures, extended behaviour)

```c
int RIS_KnownExt(const char *name);
```
- **New**: returns 1 for `.WAV` (case-insensitive), in addition to the
  feature-006 set.

```c
int RIS_ScanModule(const char *path, slibent_t *ents, int max);
```
- **New**: a file whose bytes satisfy research.md R1 yields exactly **1**
  record with `Format` 5 or 7 and `hdr` per data-model.md. Identification is
  content-keyed (no extension check), ordered with the other sniffs.
- A `.WAV`-named file failing R1 (non-PCM tag, 24-bit, no `data` chunk in 3)
  falls through the remaining sniffs and returns -1 (requester shows
  "Unknown sample source"), matching the original's unknown handling.

```c
const char *RIS_FormatName(uint8_t fmt);
```
- **New**: `5 → "8 Bit WAV Format"`, `7 → "16 Bit WAV Format"` (verbatim
  IT_DISK.ASM 556..557).

```c
int RIS_LoadSample(const slibent_t *e, sample_t *dst);
```
- Unchanged; works for WAV records once `Load_SampleData` handles Cvt bit 5.

## `it_music.h` / `it_load.c` (engine — constitution I applies)

```c
int Load_SampleData(const uint8_t *filedata, size_t size, sample_t *s);
```
- **New**: honours `s->Cvt & 32` (stereo): reads `Length << is16 << 1`
  bytes from `OffsetInFile`, runs the unsigned→signed pass over the full
  interleaved buffer when `Cvt` bit 0 is clear, then compacts to the left
  channel in place (research.md R3). Postconditions unchanged: `s->Data`
  flat signed mono, `Flags &= ~0x0C`, `Cvt = 1`, interpolation padding
  appended.
- **Invariant**: behaviour for `Cvt & 32 == 0` is byte-identical to before
  (no module loader sets bit 5) — guarded by the determinism regression.

## `it_editor.c` (internal)

- `lib_open_source`: the single-record direct-load branch also fires for
  `SLib[0].Format == 5 || SLib[0].Format == 7`.
- Selftest: WAV block per research.md R6 inside the LIB section; `LIB OK`
  now implies the WAV gates.

## `tools/gen_import_tests.py`

- Generates `testdata/lib_test8.wav`, `testdata/lib_test16.wav`,
  `testdata/lib_testst.wav` (deterministic PCM content, distinct L/R in the
  stereo file) plus the two negative fixtures used by the selftest
  (non-PCM tag, 24-bit).
