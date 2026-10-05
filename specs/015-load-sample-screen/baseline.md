# Gate Baseline (pre-feature-015)

**Captured**: 2026-09-26 at it26 `278cb25`.

| Module | Audio hash |
|---|---|
| beyond_network | `a43e6f1967ce5f09` |
| itdemo | `1e6a5383f7619069` |
| quests_end | `bb5b15bc7e590d5f` |
| synthscape_filters | `d561c207a383bc43` |

Round-trip (`test_pattern <m> --roundtrip`): 12/12 IDENTICAL.

Selftest: `F5 IMPORT F3 SAVE LIB PE PE2 TERM S3M UPD FFT ORD KBD INS` +
`GV WIRED` + `F4`, all OK. Feature 015 adds `LSS`.
