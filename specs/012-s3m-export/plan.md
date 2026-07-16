# Implementation Plan: S3M Export (SaveFormat 1)

**Branch**: `012-s3m-export` | **Date**: 2026-07-11 | **Spec**: spec.md

**Repo**: `C:\Users\elias\fable5\ittrack`

## Summary

Transliterate D_SaveS3M into `Save_S3MModule` (src/it_save.c) with the
existing block-writer plumbing plus a warning hook; wire SaveFormat 1
into the F10 requester (four original format radio buttons, .S3M
extension) and Ctrl-S (extension replace); selftest S3M block does a
save -> field verify -> re-import round trip.

## Technical Context

C11; files: src/it_save.c/.h (writer + hooks), src/it_editor.c
(buttons, extension/dispatch, warning drawing + keywait, selftest),
README/HANDOFF. Engine untouched (constitution); it_import.c is the
read-side and stays as-is. Gates: determinism/roundtrip/selftest on
Windows + WSL.

## Phases

1. Writer core in it_save.c per research R1..R4 (T002).
2. Editor wiring per R5 (T003).
3. Selftest + gates + docs (T004..T006).
