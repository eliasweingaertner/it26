# Quickstart: validating stereo samples

## Automated

1. `test_pattern` on the four testdata modules → all `IDENTICAL` with the known hashes (mono path untouched).
2. `build-compare/_work/selftest.ps1` → all blocks OK, including **STR**:
   - a synthetic stereo sample (left = sine, right = silence) plays at centre pan → right output silent apart from nothing, left = the sine; hard-left / hard-right balance as in SC-004
   - `testdata/openmpt-testpattern-stereo.it` loads with 3 stereo samples (16-bit loop, 16-bit, 8-bit ping-pong); IT save + reload with SaveFormat IT214 / IT215 / IT2xx → identical data, flag 0x04 set, stereo samples uncompressed; ITS / ITI round trips
   - each F3 operation on stereo = the operation on each channel as mono (SC-003)
   - Alt-U: stereo → mono (mix / left / right), mono → stereo
   - S3M export of a stereo sample → mono mix + warning
   - render import of a stereo song → stereo sample
3. Existing comparison scripts (IT 2.14) unchanged where no stereo sample is involved.

## Manual

1. Load a stereo WAV (Load Sample screen) → "Stereo / Left / Right", Enter → F3 shows two lanes and "Stereo".
2. Play it in a pattern; pan it left/right (default pan, X commands) → balance.
3. Save the module (IT215), reload → still stereo; open in OpenMPT / Schism Tracker → stereo.
4. Open the module in IT 2.14 (comparison harness) → loads, left channel plays.
5. F3 Alt-U → mono (Mix) and back to stereo.
6. Feature 018: F2 Ctrl-O in a stereo song → stereo sample; F5 D → stereo WAVs.
