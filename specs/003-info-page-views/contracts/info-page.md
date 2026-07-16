# Contract: Info page (F5) views & keys

## C1. Window engine

- Default layout on entry: 3 windows — track view rows 12..31,
  Variables rows 32..34, 24-channel rows 35..49 (2.17 defaults).
- Non-first windows with method ∉ {track view, note dots} draw one row
  taller so their box top border replaces the previous window's bottom
  border (DrawDisplayData quirk).
- Ins splits (cap 5 windows, needs len > 6), Del merges (min 1), resize
  moves borders keeping len ≥ 3, Tab/Shift-Tab move focus, PgUp/PgDn
  cycle the focused window's method mod 11, Ctrl-F toggles fullscreen of
  the focused window.

## C2. Track view (method 0) — Display_HostChannel

- Boxes: (4,top)-(29,·) style 27, (30,·)-(62,·) style 30-ish second box,
  (63,·)-(73,·) when stereo. Channel numbers x=2 attr per
  DrawChannelNumbers; per active channel: sample number "01"-"99"
  ("--" ≥ 100) attr 06h, "/ii" in instrument mode, ':' attr 7 (6 after
  note-off, 4 when FV=0), 25-char sample/instrument name attr 06h,
  velocity/volume bar at x=36.. (chars 176/179/182 + 173+n tip, attr
  05h / 01h muted), pan display in the stereo box (thumb chars 155..,
  or Left/Surround/Right words).
- Velocity bar value = clamp scan: span = SampleOffset−OldSampleOffset
  (loop-aware per the proc), min/max over the actual sample bytes
  (16-bit: high bytes), value = (max−min)·FV >> 8 rounded.

## C3. Pattern views (methods 1..7)

Per Display_5/8/10/18/24/36/64Channel: header row per density
(" Channel xx " / "  xx  " / "xx"), rows = playing pattern rows centred
on the current row (current row hilighted per the proc), cells decoded
with the player-exact repeat-bit decoder; muted channels per
GetChannelColour. Exact column formats per each proc (ported literally).

## C4. Variables view (method 8)

Two text lines at (2, top+1) attr 20h (23h when focused):
"Active Channels: n (v)" — n = slave channels with SF_CHAN_ON, v =
channels with a host owner (word +38h ≠ 0) — and
"  Global Volume: g" via Music_GetDisplayVariables equivalents.

## C5. Note dots (method 9)

Box (4,top)-(78,·) style 27; 73 columns = notes 30..102; one row per
host channel from topchan; per active slave channel on that host row:
dot glyph 193+(FV+7)>>4, attr 2+(smp&3) (ins&3 when MIDI), 1 muted;
disowned only overwrite larger; empty cells glyph 193 attr 6.

## C6. Details view (method 10)

Display_Details table: per channel NNA state, frequency, sample
position, Smp, FVl/Vl/CV/SV/VE/Fde/Pn/PE columns per DetailsMsg header
(ported literally from the proc).

## C7. Keys

Per research R3 table. Soloing uses the engine's Music_SoloChannel;
unsolo restores the prior mute state exactly (engine behaviour).
Alt-only combos without portable aliases: substitutes/flashes per
research R3, documented in README fidelity notes.

## C8. Gates

Determinism ×4 after the Music_SoloChannel/ToggleReverse additions;
ITED_SELFTEST extended to cycle all 11 methods and split/resize/merge.
