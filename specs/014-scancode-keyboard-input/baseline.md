# Gate Baseline (pre-feature-014)

**Captured**: 2026-08-20, at `ittrack` HEAD `8dfb8bd`
**Purpose**: SC-005 / FR-016 reference. Every gate below MUST reproduce these
exact values after the feature lands. Any movement is a defect in 014, not an
accepted deviation — this feature touches no engine code.

## Determinism regression (`test_pattern <module>`)

| Module | Patterns | Idempotency | Audio hash | Verdict |
|---|---|---|---|---|
| `testdata/beyond_network.it` | 179 | pass (0 mismatched) | `a43e6f1967ce5f09` | IDENTICAL |
| `testdata/itdemo.it` | 53 | pass (0 mismatched) | `1e6a5383f7619069` | IDENTICAL |
| `testdata/quests_end.it` | 63 | pass (0 mismatched) | `bb5b15bc7e590d5f` | IDENTICAL |
| `testdata/synthscape_filters.it` | 123 | pass (0 mismatched) | `d561c207a383bc43` | IDENTICAL |

## Round-trip idempotency (`test_pattern <module> --roundtrip`)

Note the argument order: the flag goes **after** the module path
(`tests/test_pattern.c:117` reads `argv[2]`). All 4 modules x 3 formats
(fmt 3, 0, 2) returned `msg=ok -> IDENTICAL`, each at the module's own audio
hash above. 12 of 12.

## Editor selftest (`ITED_SELFTEST=1 ITED_TERM=1 ited testdata/itdemo.it`)

12 blocks green, plus the closing summary line:

```text
[F5 OK] [IMPORT OK] [F3 OK] [SAVE OK] [LIB OK] [PE OK] [PE2 OK]
[TERM OK] [S3M OK] [UPD OK] [FFT OK] [ORD OK]
completed 88 actions, pattern 0, 64 rows, cursor r0 c0 col0,
tempo 125 speed 6 pan[1] 20, GV=128 GlobalVolume=128 MV=48
[GV WIRED OK] [F4 OK]
```

After feature 014 this becomes 13 blocks with the addition of `KBD OK`; the
other 12 and the summary line must be unchanged.

## Build hosts (T002)

- **Windows**: MSVC via `vcvars64.bat`, rebuilt from source and re-ran the
  selftest green.
  **Documentation drift found**: the `cl` line at `docs/HANDOFF.md:589` omits
  `src\it_cornerart.c` and therefore fails at link with unresolved
  `IT_CornerArt` / `IT_CornerArtPal`. The working line needs that source added
  (14 sources, not 13). Fix queued as part of T043.
- **Linux**: WSL Ubuntu reachable, gcc 9.4.0. Terminal backend only — no SDL2
  dev headers on that box, so `src/it_screen_sdl.c` cannot be compile-checked
  there.
