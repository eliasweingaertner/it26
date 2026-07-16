# Data Model: Pattern Editor Completion (feature 010)

All entities are editor-side statics in `src/it_editor.c`, mirroring the
IT_PE.ASM `Pattern` segment variables (research.md R1/R3).

## pe_pattern_set_length (u16)

Mirror of `PatternSetLength`. Range 32..200 (thumbbar bounds). Initial 64.
**Persists across dialog invocations** (original quirk, research D2).

## pe_pattern_length_start / pe_pattern_length_end (u16)

Mirrors of `PatternLengthStart/End`. Range 0..199. Re-primed to the current
pattern number every time the dialog opens.

## pe_view_channels[100] (u16)

Mirror of `ViewChannels`: ordered list of `(method << 8) | channel` entries
terminated by 0xFFFF fill; all-0xFFFF = default view. Mutated only by the
Ctrl-0..5 / Ctrl-Shift-1..4 / Alt-T / Alt-R handlers, always validated by
`pe_check_width()` with revert-on-failure.

## pe_view_method_info[5]

Mirror of `ViewMethodInfo`: `{draw_fn, width}` pairs — ViewFull/13,
ViewCompress/10, ViewAllSmall/7, ViewNote/3, ViewTiny/2.

## pe_cursor_positions[5][9] (u8)

Mirror of `CursorPositions`: per-method per-cursor-column geometry rows
(methods 0..2 = x offsets; rows 3..4 = the ViewNote/ViewTiny data consumed by
the hilight path). Copied byte-exact with an ASM line-reference comment.

## View/toggle state (mirrors, persisted in ited.cfg)

- `pe_view_division` (u8, default 1) — divider column on/off.
- `pe_view_width` (u16) / `pe_num_channels_edit` (u16) — derived by
  `pe_check_width()`; not persisted.
- `pe_view_channel_tracking` (u8, default 0).
- `pe_centralise_cursor` (u8, default 0xE8) — bit 0 centralise, bit 1
  row-hilight; upper bits reserved (MIDI record flags, unused). The port's
  existing centralise flag from 009 merges into bit 0 of this byte.

## Channel captions

`" Channel xx "` / `"Channel xx"` / `"Chnl xx"` / `" xx"` / `"xx"`
(+ `ChannelMsg3/6` variants where PE_DrawPatternEdit's width switch selects
them) — static strings, xx replaced with the 1-based channel number.

## Relationships / invariants

- Sum of visible widths (+2 border, +dividers) must stay < 76 —
  `pe_check_width` is the single validator; every mutator reverts on failure.
- Resize writes go: undo snapshot (current pattern, standard type) → for each
  pattern in [start..end]: unpack → set length → pack, under
  `Engine_Lock`/`Unlock`. Pattern lengths are the engine's real per-pattern
  row counts.
- Mute/solo has **no editor-side state**: engine channel flags are the single
  source of truth shared with F5/F11.
