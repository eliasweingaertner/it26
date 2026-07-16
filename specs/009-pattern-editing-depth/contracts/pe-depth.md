# Contract: interfaces touched by feature 009

No public/library API — the surface is keyboard behaviour plus two
internal boundaries.

## `it_screen.h` (shared key contract, all backends)

- `ITK_*` enum gains the codes in data-model.md "Key layer additions".
  Backends translate; a backend that cannot produce a combo simply never
  emits it (no stub codes). Existing codes keep their values (the enum is
  append-only — selftest scripts and `ited.cfg` do not encode key codes,
  so no migration).
- New events `ITK_SHIFT_DOWN` / `ITK_SHIFT_UP` fire on plain Shift
  press/release (pixel backends). Consumers other than F2 ignore them.

## `it_editor.c` internal (documented for the tasks phase)

- `handle_pattern_key` becomes a dispatch over the ported key table:
  ordered (modifier, key) → handler pairs exactly as IT_PE.ASM 380..860,
  so precedence quirks (e.g. `;` cycling vs keyjazz) fall out of table
  order, not ad-hoc ifs.
- Every mutating handler: `snapshot_undo(type)` first (when the original
  does), mutate the unpacked grid under `ed_lock`, `commit_current_pattern()`
  at the end. No handler touches packed data directly.
- Mark rendering: `draw_pattern` consults the mark rect per cell and
  applies the original mark attribute (`PE_SelectColour` port).
- The undo requester and the multichannel dialog are modal object lists
  built from the `O1_UndoList` / `O1_SelectMultiChannel` data
  (IT_OBJ1.ASM), reusing the existing menu/list widget machinery.

## Widget framework (thumbbars)

- Thumbbar key path accepts '0'..'9' → numeric entry accumulator with
  Enter commit (clamped to [min,max]) / ESC cancel, per `F_PostThumbBar`
  IT_F.ASM 2125.. — applies to every thumbbar screen (F3/F4/F12, pattern
  length dialog).

## Selftest contract

- New `PE OK` block: scripted batteries per story (research.md R9.5).
  The block MUST leave the loaded song unmodified at exit (operate on a
  scratch pattern, restore via the undo/scratch machinery it tests).
- Existing gates unchanged: determinism ×4, `--roundtrip`, `LIB OK` etc.
  The keyjazz reduction MUST NOT break existing selftest note scripts
  (they use Z/Q-row keys only — verified in research R6).
