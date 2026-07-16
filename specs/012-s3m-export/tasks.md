# Tasks: S3M Export (SaveFormat 1)

**Repo**: `C:\Users\elias\fable5\ittrack`

- [X] T001 Baseline gates green (post-011 tree).
- [X] T002 [US1] `Save_S3MModule` in src/it_save.c per research R1..R4:
      header/orders/channel settings/pan block, aligned sample headers,
      pattern translation with the drop/clamp quirks, unsigned sample
      data, final header + memseg patch passes; `Save_S3MWarning` hook;
      delete-on-error; decl + SaveFormat comment update in it_save.h.
- [X] T003 [US2] Editor: F10 format radio buttons (R5 coordinates,
      mouse + keyboard), extension append .IT/.S3M by format
      (req_do_save), Ctrl-S extension replace (quick_save), dispatch to
      the S3M writer when SaveFormat == 1, warning rows drawn via the
      hook + key-wait when any warning fired.
- [X] T004 Selftest S3M block: save itdemo as .S3M, assert SCRM magic,
      counts, GV/speed/tempo, channel settings, dp=252; re-import via
      the S3M importer, assert speed/tempo/GV and sample-data byte
      equality; clean up temp files and reload itdemo.
- [X] T005 Docs: README save section + fidelity notes (S3M ported,
      warnings, deferred DOS byte-diff), HANDOFF state/§2/§6.
- [X] T006 Final gates Windows + WSL: determinism ×4 + roundtrip ×4×3
      IDENTICAL, selftest 10/10 OK (incl. S3M) on both; staged commits.
