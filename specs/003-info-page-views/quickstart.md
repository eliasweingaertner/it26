# Quickstart: verifying the info page views

Build per HANDOFF §3 (ited + test_pattern).

## Scenario 1 — default multi-window layout & velocity bars (SC-001)

`ited testdata\itdemo.it`, F5 (starts playback and switches). Expect the
2.17 default: track view (rows 12..31) with per-channel velocity bars
moving with the audio (flat when silent), Variables strip, 24-channel
view below. 'V' switches to steady volume bars and back.

## Scenario 2 — all view methods (SC-002)

With F5 focused, PgDn cycles the focused window through: track → 5ch →
8ch → 10ch → 18ch → 24ch → 36ch → 64ch → Variables → note dots →
details → wraps. Each renders live while playing.

## Scenario 3 — split windows (SC-003)

Tab focuses each window; Ins splits the focused window (up to 5),
Ctrl-U/Ctrl-D move the border (Alt-Up/Down in original IT), Del merges,
Ctrl-F fullscreens the focused window and back.

## Scenario 4 — solo / mute (SC-004)

Up/Down select a channel (gutter colour 13h); shift-'Q' mutes/unmutes,
shift-'S' solos — only that channel audible; shift-'S' behaviour on
restore matches the engine's Music_SoloChannel semantics.

## Scenario 5 — determinism gate (SC-005)

`test_pattern` ×4 testdata modules — all `IDENTICAL` (the engine gained
Music_SoloChannel/Music_ToggleReverse; no-solo path must be untouched).

## Scenario 6 — selftest

`ITED_SELFTEST=1 ited testdata\itdemo.it` — the extended script cycles
all 11 methods, splits/resizes/merges windows, and reports `F5 OK`.
