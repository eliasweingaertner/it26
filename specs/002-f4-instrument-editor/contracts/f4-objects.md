# Contract: F4 object tables → widgets

The implementation MUST reproduce these objects (source: `IT_OBJ1.ASM`, lines
5629–6560; coordinates are 80×50 cells; box style numbers are S_DrawBox styles).

## C1. Shared chrome (all four tabs)

| Object | Data |
|---|---|
| Header | `Instrument List (F4)` info line |
| `InstrumentNameBox` | box (4,12)-(30,48) style 27 |
| `InstrumentWindow` | custom: left instrument list (existing) |
| Tab buttons | General (31,12)-(41,14) " General"; Volume (43,12)-(53,14) " Volume"; Panning (55,12)-(65,14) " Panning"; Pitch (67,12)-(77,14) "  Pitch"; style 8, label attr per focus rules |

## C2. General tab (`O1_InstrumentListGeneral`)

| Object | Data |
|---|---|
| `InstrumentTranslateBox` | box (31,15)-(42,48) style 27 |
| `InstrumentNoteWindow` | custom: note-translation table inside the box |
| `InstrumentNNADivision` | text (44,15) attr 20h: 35× char 134 divider |
| `InstrumentGeneralNNAText` | text (54,17) attr 20h "New Note Action" |
| NNA buttons | (45,18)-(77,20) "  Note Cut"; (45,21)-(77,23) "  Continue"; (45,24)-(77,26) "  Note Off"; (45,27)-(77,29) "  Note Fade"; radio on `NNA`=0..3 |
| `InstrumentDCTDivision` | text (44,30) attr 20h: 35× char 134 |
| `InstrumentGeneralDCTText` | text (47,32) attr 20h "Duplicate Check Type & Action" |
| DCT buttons | (45,33)-(60,35) "  Disabled"; (45,36)-(60,38) "  Note"; (45,39)-(60,41) "  Sample"; (45,42)-(60,44) "  Instrument"; radio on `DCT`=0..3 |
| DCA buttons | (61,33)-(77,35) "  Note Cut"; (61,36)-(77,38) "  Note Off"; (61,39)-(77,41) "  Note Fade"; radio on `DCA`=0..2 |
| `InstrumentFileDivision` | text (44,45) attr 20h: 35× char 154 |
| `InstrumentGeneralFileNameText` | text (47,47) attr 20h "Filename" |
| `InstrumentFilenameBox` | box (55,46)-(73,48) style 27 |
| `InstrumentFileName` | string input (56,47), 12 visible+edit chars, → `DOSFileName` |

## C3. Envelope tabs — shared frame (Volume/Panning/Pitch)

| Object | Data |
|---|---|
| `InstrumentEnvelopeBox` | box (31,17)-(77,26) style 27 |
| `InstrumentEnvelope` | custom: envelope display + node editor (`I_DrawEnvelope`) |
| `InstrumentVEBox` | box (53,27)-(63,30) style 27 |
| `InstrumentVELBox` | box (53,31)-(63,35) style 27 |
| `InstrumentVESLBox` | box (53,36)-(63,40) style 27 |
| Env text | Volume: (38,28) "Volume Envelope"/"Carry"; Panning: (37,28) "Panning Envelope"/"Carry"; Pitch: (35,28) "Frequency Envelope"/"Carry"; attr 20h |
| On toggle | (54,28) → env `Flags` bit 1 |
| Carry toggle | (54,29) → bit 8 |
| Loop toggle | (54,32) → bit 2; `InstrumentVELText` (40,32) attr 20h "Envelope Loop/Loop Begin/Loop End" |
| Loop Begin/End | type-13 numeric at (54,33)/(54,34) → `LpB`/`LpE` |
| SusLoop toggle | (54,37) → bit 4; `InstrumentVESLText` (40,37) "Sustain Loop/SusLoop Begin/SusLoop End" |
| SusLoop Begin/End | (54,38)/(54,39) → `SLB`/`SLE` |

## C4. Volume tab extras

| Object | Data |
|---|---|
| `InstrumentGlobalVolumeBox` | box (53,41)-(71,44) style 27 |
| `InstrumentGlobalVolumeText` | text (39,42) attr 20h " Global Volume"/" Fadeout"//""/"Volume Swing %" |
| `InstrumentVolume2` | thumbbar (54,42) 0..128 attr 18h → `GbV` |
| `InstrumentFadeOut2` | scalable bar (54,43) 0..256 attr 14h width 16 → `FadeOut` |
| `InstrumentRandomVolBox` | box (53,45)-(71,47) style 27 |
| `InstrumentRandomVolBar` | scalable bar (54,46) 0..100 attr 1Ah width 16 → `RV` |

## C5. Panning tab extras

| Object | Data |
|---|---|
| `InstrumentDefaultPanBox` | box (53,41)-(63,48) style 27 |
| `InstrumentDefaultPanText` | text (33,42) attr 20h: " Default Pan"/" Pan Value"//"" /" Pitch-Pan Center"/"Pitch-Pan Separation"/" Pan swing" (with 0xFF skips 9/11/4/11) |
| `InstrumentDefaultPanToggle` | (54,42) → `DfP` bit 0x80 |
| `InstrumentDefaultPanValue` | thumbbar (54,43) 0..64 attr 19h → `DfP` low bits |
| `InstrumentPanBoxFiller` | text (54,44) attr 02h: 9× char 9Ah |
| `InstrumentPitchPanCenter` | custom (54,45): PPC as note name |
| `InstrumentPitchPanSeparation` | thumbbar (54,46) −32..32 attr 16h → `PPS` |
| `InstrumentPanSwing` | thumbbar (54,47) 0..64 attr 1Bh → `RP` |

## C6. Pitch tab extras (FILTERENVELOPES=1 layout)

| Object | Data |
|---|---|
| `InstrumentMIDIBox1` | box (53,41)-(71,48) style 27 |
| `InstrumentMIDIText` | text (36,42) attr 20h: "Default Cutoff"/"Default Resonance"/"MIDI Channel"/"MIDI Program"/"MIDI Bank Low"/"MIDI Bank High" |
| `InstrumentFilterCutoff` | scalable bar (54,42) 0..127 attr 3Ah width 16 → `IFC` |
| `InstrumentFilterResonance` | scalable bar (54,43) 0..127 attr 3Bh width 16 → `IFR` |
| `InstrumentMIDIChannel` | scalable bar (54,44) 0..17 attr 3Ch width 16 → `MCh` |
| `InstrumentMIDIProgram` | scalable bar (54,45) −1..127 attr 3Dh width 16 → `MPr` |
| `InstrumentMIDIBank1/2` | scalable bars (54,46)/(54,47) −1..127 → `MIDIBnk` lo/hi |

## C7. Behaviour contracts

- Tab buttons switch the built object list; the active tab's button renders in
  the "selected" style as IT does (verify `I_GetInstrumentScreen` pre-handler).
- Envelope node editor implements `I_PostEnvelope` keys: node select
  (left/right), node value/tick move (alt+arrows), insert/delete node, and
  clamps per data-model rules.
- All engine-visible mutations under `Engine_Lock`.
- Determinism gate `IDENTICAL` ×4 after the change.
