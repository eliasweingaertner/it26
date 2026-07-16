# Quickstart: verifying the sample & instrument editors

Build per HANDOFF §3.

## Scenario 1 — waveform view (SC: US1)

`ited testdata\itdemo.it`, F3: the waveform of the selected sample
renders in the bottom-right box with loop markers when the sample
loops; cursor down changes samples and the waveform follows.
`ITED_SHOT=f3.bmp ITED_SHOT_SCREEN=2` captures it headless.

## Scenario 2 — loop editing (US1)

Set Loop to On and move Loop End below Loop Beg — the loop switches
Off (clamp). Values above the length clamp. Auditioning (piano keys)
uses the new loop through the real engine.

## Scenario 3 — destructive ops (US2)

On a sample: Alt-I invert (waveform flips), Alt-G reverse (waveform
mirrors, loops mirrored), Alt-M amplify (suggested % = normalize),
Alt-H centre, Alt-Q quality convert 8↔16, Alt-E resize, Alt-B/Alt-L
cut before/after loop, Alt-+/Alt--/Ctrl-+/Ctrl-- speed. Each redraws
and auditions correctly.

## Scenario 4 — slot ops (US2/US3)

Alt-Ins/Alt-Del insert/remove sample and instrument slots with
reference fixups (pattern bytes in sample mode, note tables in
instrument mode); Alt-S/Alt-X/Alt-R swap/exchange/replace via number
prompt.

## Scenario 5 — envelope presets (US3)

On an F4 envelope: digit 0..9 loads a preset, Alt-digit stores one;
Enter-grab auto-enables the envelope.

## Scenario 6 — gates

Determinism ×4 IDENTICAL; `--roundtrip` ×4 IDENTICAL;
`ITED_SELFTEST=1` reports `F3 OK`.
