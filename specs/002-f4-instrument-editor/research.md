# Phase 0 Research: F4 Instrument Editor

## R1. Where the authentic F4 layout lives

**Decision**: Source the page from the four object lists in `IT_OBJ1.ASM`
(jthlim repo, byte-identical to the mirror):

| Object list | Line | Drives |
|---|---|---|
| `O1_InstrumentListGeneral` | 5629 | General tab (NNA/DCT/DCA, filename, note-translation window) |
| `O1_InstrumentListVolume`  | 5896 | Volume tab (vol env, carry, loops, GbV/FadeOut/RV) |
| `O1_InstrumentListPanning` | 6107 | Panning tab (pan env, DfP, PPC/PPS, pan swing) |
| `O1_InstrumentListPitch`   | 6251 | Pitch tab (pitch env, cutoff/resonance, MIDI ch/prog/bank) |

Shared members: `InstrumentListHeader` ("Instrument List (F4)"),
`InstrumentNameBox` (4,12)-(30,48) style 27, `InstrumentWindow` (custom draw =
left list), the four tab buttons at (31,12)-(41,14) / (43,12)-(53,14) /
(55,12)-(65,14) / (67,12)-(77,14), `InstrumentEnvelopeBox` (31,17)-(77,26)
style 27, and the VE/VEL/VESL boxes at (53,27)-(63,30) / (53,31)-(63,35) /
(53,36)-(63,40) style 27 reused by all three envelope tabs.

**Rationale**: Constitution Principle II — layout comes from the ASM object
tables, closing the last eyeballed screen.

**Alternatives considered**: Keep refining against screenshots — rejected, it is
the violation this feature exists to remove.

## R2. Object-type semantics (from `IT_M.ASM` dispatch tables, lines 97–159)

| Type | Draw handler | Meaning for this feature |
|---|---|---|
| 0  | `F_DrawBoxObject` | box: x0,y0,x1,y1 (bytes) + style byte |
| 1  | `F_DrawTextObject` | text: x,y, attr, control-coded string (0xFF=skip, CR=newline) |
| 2  | `F_DrawButtonObject` | button: nav words (up,down,left,right), handler, arg, group id, coords, style 8 |
| 9  | `F_DrawThumbBar` | classic thumbbar: x,y, min,max, attr, struct-offset, nav |
| 13 | `F_Draw3Num` | 3-digit numeric field bound to a byte at struct-offset (loop node numbers) |
| 14 | `F_DrawScalableThumbBar` | thumbbar with trailing width word (16 cells here) |
| 15 | `F_CallFarFunction` | custom draw/pre/post far-function triple |
| 16 | `F_DrawStringInput` | editable string (instrument DOS filename, 12 chars) |
| 17 | `F_DrawToggle` | On/Off toggle bound to (struct-offset, bit mask) |

The existing `it_editor.c` widget framework already models 0/1/2/9/14/16
(boxes/labels are drawn directly; buttons, thumbbars, toggles, string inputs are
`widget_t`s). **New for this feature**: type 13 (numeric byte field with
min/max = env node count), type 17 bound to *instrument envelope flag bytes*,
and three type-15 custom objects (instrument left list already exists; note
window and envelope display are new).

## R3. Value bindings are instrument-struct offsets — port as direct field refs

The object tables bind widgets to hex offsets inside the 554-byte instrument.
`it_structs.h` pins the same layout, so bindings become plain C field refs:

| ASM offset | `instrument_t` field | Widgets |
|---|---|---|
| 0x11/0x12/0x13 | `NNA/DCT/DCA` | existing NNA/DCT/DCA radio buttons (General) |
| 0x14 | `FadeOut` (0..256 in UI ×?) | `InstrumentFadeOut2` scalable bar, min 0 max 256 |
| 0x16/0x17 | `PPS/PPC` | Pitch-Pan Separation bar (−32..32), `I_DrawPitchPanCenter` custom |
| 0x18 | `GbV` | Global Volume bar 0..128 |
| 0x19 | `DfP` | Default Pan toggle (bit 0x80 inverted = "use") + value bar 0..64 |
| 0x1A/0x1B | `RV/RP` | Volume Swing 0..100 / Pan Swing 0..64 |
| 0x3A/0x3B | `IFC/IFR` | Default Cutoff / Resonance bars 0..127 (FILTERENVELOPES=1) |
| 0x3C/0x3D | `MCh/MPr` | MIDI Channel 0..17 / Program −1..127 |
| 0x3E/0x3F | `MIDIBnk` lo/hi | MIDI Bank Low/High −1..127 |
| 0x130/0x182/0x1D4 | `VEnvelope/PEnvelope/PtEnvelope` `.Flags` | env On (bit 1), Loop (2), SusLoop (4), Carry (8) toggles |
| +2/+3, +4/+5 of each env | `.LpB/.LpE/.SLB/.SLE` | type-13 numeric fields (max = `Num`−1) |

**Decision**: Bind widgets directly to `Song.Ins[CurInstr-1]` fields; no shadow
model (Principle IV).

## R4. FILTERENVELOPES build switch

`SWITCH.INC` sets `FILTERENVELOPES = 1` for 2.17 (`TRACKERVERSION = 217h`).
**Decision**: Port the `IF FILTERENVELOPES` variant of the Pitch tab: MIDI box
(53,41)-(71,48), Default Cutoff (54,42) and Default Resonance (54,43) bars, MIDI
Channel/Program/Bank at rows 44–47, and the pitch-envelope filter mode
(env `Flags` bit 0x80 = filter envelope, toggled per `IT_I.ASM`).

## R5. Custom-draw objects to port from `IT_I.ASM`

| ASM proc | Line | Port target |
|---|---|---|
| `I_DrawInstrumentWindow` | 4653 | already exists as the left list — verify colours/geometry against the proc, keep |
| `I_DrawNoteWindow` | 5374 | NEW: note-translation table in `InstrumentTranslateBox` (31,15)-(42,48): 33 visible rows, each "note → sample:note", editable (piano keys set entry, arrows move) |
| `I_DrawEnvelope` / `I_PreEnvelope` / `I_PostEnvelope` | 6678/6766/6781 | NEW: envelope display inside (31,17)-(77,26): 9-row cell grid using half-block glyphs, node markers, loop/susloop columns; Post handles node add/insert/delete and value/tick nudging keys; presets (`ENABLEPRESETENVELOPES=1`) Alt-0..9 optional |
| `I_DrawPitchPanCenter` | 8799 | NEW: PPC field drawn as note name (C-0..B-9) at (54,45) |

**Decision**: Implement as the widget framework's custom-draw widget type
(pre/draw/post callbacks mirroring the far-function triple), keeping the
envelope editor's key model from `I_PostEnvelope` (left/right select node,
alt-arrows move node, ins/del add/remove, etc. — read the proc while porting).

## R6. Envelope display geometry

`InstrumentEnvelopeBox` interior is (32,18)-(76,25): 45×8 cells = 90×? — the
original draws the envelope onto a character grid via custom glyph rows (the
same technique as the pattern-view separators); `I_DrawEnvelope` writes 2-pixel
columns per node span using chars 128..201 range glyphs. The port renders the
same cell-based approximation the ASM computes (value 0..64 mapped to the 8-row
grid; time axis = node ticks scaled to 45 columns×2). Exact glyph choices come
from the proc during implementation — **not** eyeballed.

## R7. Keyboard map for the page

`InstrumentGlobalKeyList` (IT_OBJ1.ASM 6462): Ctrl/plain Up/Down = sample
up/down… Alt-D delete, Alt-R replace, Alt-S swap, Alt-P copy, Alt-X exchange,
Alt-N multichannel, Space = list-space handler, note keys audition via
`I_PlayNote`. **Decision**: keep the already-ported subset working; add the
missing Alt-keys only where their handlers already exist in the port (delete /
copy / swap exist for samples; instrument variants are small). Anything not
ported is flagged on the info line, consistent with current practice.
