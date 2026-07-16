# Quickstart: Pattern Editing Depth (009)

Validation walkthrough — run from `C:\Users\elias\fable5\ittrack`.
Because delivery is staged (plan.md Structure Decision), run the relevant
part after each stage; the full pass gates the feature as done.

## Prerequisites

- Build per `docs/HANDOFF.md` §3 (`ited` + `test_pattern`; `itplay` and
  `test_pattern` need `it_save.c` in their source lists).

## Gate 1 — selftest (headless, every stage)

```
ITED_SELFTEST=1 ./ited testdata/itdemo.it
```

All existing gates plus the new `PE OK` block: mark/op batteries, undo
revert byte-equality, mask/template entry assertions. The block must
leave the song unmodified (its own final assertion).

## Gate 2 — determinism + roundtrip (final stage, and after any engine touch)

```
./test_pattern testdata/beyond_network.it
./test_pattern testdata/itdemo.it        # + --roundtrip
./test_pattern testdata/quests_end.it
./test_pattern testdata/synthscape_filters.it
```

All `IDENTICAL` with the HANDOFF §4 hashes.

## Manual checks by story

**US5 plumbing**: in F2, `,` toggles the mask bit for the cursor field
(status feedback per original); `;` still cycles instrument down; `,` `.`
`;` `l` `/` no longer enter notes anywhere; digits typed on any thumbbar
(e.g. F12 tempo) enter a number, Enter commits, ESC cancels.

**US1 marking/block ops**: mark with Shift+arrows and Alt-B/E/D/L
(repeat Alt-D doubles, repeat Alt-L widens to all channels); Alt-C copy,
move, Alt-P/O/M paste variants (repeat Alt-M switches precedence);
Alt-Q/A transpose; Alt-F/G double/halve; Alt-J amplify (Ctrl-J fast);
Alt-K slide (2× wipes); Alt-X effect slide (2× wipes); Alt-V/W volumes;
Alt-Y swap, Alt-Z cut; Alt-U unmark. Verify against the DOS original in
DOSBox for a spot-check battery.

**US2 entry pipeline**: set masks and enter notes (only enabled fields
written); Alt-N multichannel (2× opens the channel dialog), entry
advances across enabled channels; Alt-I template modes stamp the
clipboard transposed; `:` turns template off.

**US3 rows/undo**: Ins/Del track-local; Alt-Ins/Del all channels;
Ctrl-Ins/Del roll; Backspace steps back; Ctrl-Backspace opens the undo
requester listing typed entries; reverting restores byte-identically and
offers Redo.

**US4 navigation/conveniences**: the R5 registry key-by-key — pattern
±1/±4/order-follow, view schemes Ctrl-0..5 / Ctrl-Shift-1..4, Alt-T
track view, toggles (centralise/trace/tracking/hilight/division),
pattern length dialog, Alt-0 scratch store/restore, Ctrl-F7 + F7 play
mark, Alt-F9/F10 mute/solo keys.

## Cross-platform

Linux/SDL: full selftest + a keyboard spot-check of the new combos
(Ctrl-arrows, Ctrl-Backspace, Shift-marking). Terminal backend: verify
graceful absence (no crashes on unmapped combos) and the documented
leftover note.

## Docs to verify updated

- `README.md` fidelity notes: keyjazz map reduced to the original
  KeyBoardTable (extension removed); undo = original 10-slot history.
- `docs/HANDOFF.md`: §2 feature block, §6 roadmap #5 marked done with
  leftovers (terminal combos → #7, MIDI-in triggers excluded).
