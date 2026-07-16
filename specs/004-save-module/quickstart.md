# Quickstart: verifying save + message editor

Build per HANDOFF §3 (ited + test_pattern).

## Scenario 1 — round-trip (SC-001, SC-005)

`test_pattern testdata\itdemo.it --roundtrip` (and the other three
modules): prints the original hash, saves to a temp `.IT`, reloads,
re-renders — expect `ROUNDTRIP IDENTICAL` and the canonical hash
unchanged.

## Scenario 2 — F10 save flow (SC-003, SC-004)

`ited testdata\itdemo.it`, edit a pattern cell, F10 → save requester
with filename primed `ITDEMO.IT`. Change the name to `T.IT`, Enter —
file written, "Saved." status. F10, Enter on the same name — overwrite
confirm appears; No → file untouched; repeat with Yes → written.
`ited T.IT` shows the edit.

## Scenario 3 — message editor (SC-003)

Open the message editor, Enter to edit, type multi-line text past
column 75 (wraps), Ctrl-Y deletes a line, Esc to view. F10 save, reload
— message intact.

## Scenario 4 — interop (SC-002)

Open a saved file in an independent IT-compatible player (e.g.
OpenMPT/schismtracker) — loads and plays.

## Scenario 5 — selftest

`ITED_SELFTEST=1 ited testdata\itdemo.it` — extended script edits the
message, saves, reloads, verifies; reports `SAVE OK`.
