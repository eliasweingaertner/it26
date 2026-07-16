# Phase 0 Research: Sample & Instrument Editors (IT_I.ASM)

## R1. What IT 2.17 actually has (spec corrections)

- **No freehand draw, no selection cut/copy/paste, no zoom.** The spec's
  US2 wording ("draw, cut/copy/paste a selection") describes later
  trackers; IT 2.17's sample editing = the F3 waveform *view* plus
  Alt-key whole-sample / loop-region operations. US1's "zoom and
  scroll" does not exist either — the waveform box always shows the
  whole sample. Both recorded as spec deviations; the feature delivers
  the authentic op set below.
- **I_CalculateC5Speed (Alt-Y) is a stub in 2.17** (`Mov AX,1; Ret`,
  body commented out) — ported as the same no-op.
- Alt-O/Alt-T/Alt-W (save sample as ITS/S3I/RAW) are disk ops →
  deferred to feature 006 (sample library).

## R2. Waveform view — I_DrawWaveForm (IT_I.ASM 1846)

- Canvas 176×32 px → `S_GenerateCharacters(first=1, 22 chars, 4 chars)`
  (font bank B chars 1..88). Screen: box (54,25)-(77,30) style 9
  (`InstWaveFormBox`); cells (55..76, 26..29) display chars
  1+(row*22)+col with attr 0Dh (`InstWaveFormText`).
- Per column (176): min/max scan of the column's sample span (16-bit:
  high bytes, `Inc SI`, step 2). Column bounds via 32.16 fixed step
  (len*65536/176). Column smoothing: min = min(cur_min, prev_max),
  max = max(cur_max, prev_min); prev seeded 0x7F80 (no smoothing on
  column 0).
- Row mapping: v(-128..127) → `((v>>1)+2)>>2` (SAR twice with +0x202
  between) giving -16..15; top row = 16-max (31 when 32); draw
  max-min+1 pixels down.
- Loop markers (flag bit 4): col = (175*point + len/2)/len for
  begin/end; 32 rows of dashed pixels `((row_1based)>>1)&1` (2-px
  dashes) written at both columns — the writes can *clear* waveform
  pixels (authentic). Sustain (bit 5): 1-px alternation starting 1.
- Redrawn on sample change/ops (I_RedrawWave); port: regenerate each
  frame like the F4 envelope canvas.

## R3. Alt-key op set (SampleGlobalKeyList, IT_OBJ1.ASM 3337)

| Key | Proc | Behaviour (all confirmed from the procs) |
|---|---|---|
| Alt-A | I_ConvertSample | confirm; XOR 80h each byte (16-bit: high bytes only) |
| Alt-B | I_CutSampleBeforeLoop | needs LoopBeg≠0; confirm; cut = min(LoopBeg, SusLoopBeg if sus flag); subtract from all 4 loop dwords (clamp 0), Length -= cut, memmove data left |
| Alt-D | I_DeleteSample | confirm; stop; free data + clear name (Music_ReleaseSample/ClearSampleName) |
| Alt-E/F | I_ResizeSample(NoInt) | prompt new length (default = current); I_ReMix: fixed-point resample (16.16 step = old/new), optional linear interpolation `(s0*(256-f)+s1*f+0x80)>>8` (16-bit 32-bit analogue), Length = new, the 5 loop dwords scaled *new/old (cap 9999999); max byte size 4177920 |
| Alt-G | I_ReverseSample | reverse data; loops mirrored: Beg' = len-End, End' = len-Beg (both loops) |
| Alt-H | I_CenterSample | scan min/max; offset = -(min+max)/2 (SAR); confirm dialog showing DC; add offset to every sample |
| Alt-I | I_InvertSample | no confirm; negate every sample (byte/word) |
| Alt-J | I_ScaleSampleVolumes | prompt %; ALL 99 samples' GvL = GvL*amp/100 cap 64 |
| Alt-L | I_CutSample | needs LoopEnd≠0; confirm; Length = max(LoopEnd, SusLoopEnd) |
| Alt-M | I_AmplifySample | scan max abs dev; suggested% = 0x320000/(dev<<8) 8-bit or /dev16 (cap 400, i.e. normalize); prompt; mult = amp*65536/100; v = clip((v*mult+0x8000)>>16) 8-bit to [-128,127], 16-bit [-32768,32767] |
| Alt-N | I_ToggleMultiChannel | audition multichannel toggle |
| Alt-Q | I_ToggleSampleQuality | 3-way dialog: 1 = convert data (16→8 high bytes / 8→16 v<<8), 2 = reinterpret raw (flip flag; Length + 4 loop dwords <<1 or >>1) |
| Alt-R | I_ReplaceSample | prompt n; all refs to current → n (instrument mode: NoteSampleTable bytes; sample mode: pattern ins bytes via PE_SwapInstruments replace mode DH|=80h) |
| Alt-S | I_SwapSamples | prompt n; swap 80-byte headers AND swap refs n↔cur |
| Alt-X | I_ExchangeSamples | prompt n; swap headers only (incl. data), no ref fixes |
| Alt-Y | I_CalculateC5Speed | stub (no-op) in 2.17 |
| Alt-+/- | Double/HalveSampleSpeed | C5 <<1 (cap 9999999) / >>1 |
| Ctrl-+/- | SemiUp/Down | C5 += (C5*255392045)>>32 (overflow keeps old); C5 = (C5*4053909306)>>32 |
| Alt-Ins/Del | I_InsertSample/RemoveSample | Insert: needs slot 99 free & cur<98; shift headers down, clear slot; refs ≥ cur(1-based) incremented (cap <99) in instrument mode, else PE_InsertInstrument (pattern ins bytes). Remove: only when current slot has no data; shift up, clear 99; refs ≥ cur decremented; sample mode: PE_DeleteInstrument |
| Alt-C | I_ClearSampleName | clear DOSFileName + SampleName, SamplePos=0 |

After every data op: I_DrawWaveForm + Music_SoundCardLoadAllSamples
(no-op for the WAV driver) and usually Music_Stop first.

## R4. Loop clamping — I_CheckLoopValues / I_CheckSusLoopValues

On loop-field edits: Beg ≤ max(len-1,0); End ≤ len; if End ≤ Beg →
clear the loop flag (bit 4 / bit 5). Then Music_RegetLoopInformation
(engine re-fetches loop info for playing slaves) + redraw waveform.

## R5. F3 editable fields (O1_SampleList objects 29..47)

Filename (12-char input), C5 speed (7-digit num), Loop toggle
(Off/On/Ping Pong tri-state — flag bits 4|64), Loop Beg/End (7-digit),
SusLoop toggle (bits 5|128), SusLoop Beg/End, vibrato radio buttons
(already ported), Default Volume/Global Volume/Vib Speed/Depth/Rate
thumbbars (already ported), Default Pan toggle+bar (already ported).
Loop-field commits go through I_CheckLoopValues; speed field cap
9999999.

## R6. Instrument list ops (InstrumentListKeys / global list)

Alt-Ins/Del insert/remove instrument slot (554-byte shifts + pattern
ins-byte remap via PE_InsertInstrument/PE_DeleteInstrument in
instrument mode; Remove also clears slot 99 and calls
Music_ClearInstrument + I_MapEnvelope). I_ExchangeInstruments,
I_SwapInstruments (swap 554-byte headers + pattern refs),
I_ReplaceInstrument (refs only, replace mode), I_CopyInstrument
(prompt n; copy current header over n), I_ScaleInstrumentVolumes
(all GbV*amp/100 cap 128). Alt-C clear instrument name. Note-table
ops (I_Note*): transpose ±, sample set/pickup, insert/delete rows etc.
on the F4 note window.

## R7. PE_ pattern remaps (IT_PE.ASM)

PE_InsertInstrument / PE_DeleteInstrument / PE_SwapInstruments walk
every pattern's packed data adjusting instrument bytes (the packed
walk uses the same mask semantics as the player decoder). Port
implementation: direct packed-stream walk helper in the editor
(read mask/skip fields, adjust ins byte when mask bit 1/0x20).
PE_SwapInstruments: DH bit 7 set = replace (all DL→DH&0x7F), clear =
swap DL↔DH.

## R8. Envelope presets (PresetEnvelopes, IT_I.ASM 367)

Preset envelope table used by the F4 envelope editor (keys on the
envelope: Ctrl-1..Ctrl-6 style?) — small predefined node sets. Check
`I_VolumeEnvelopeEnter` / VolumeEnvelopeNodeKeys for the binding when
implementing (F4 flashes "not ported yet" today).

## R9. Alt-key infrastructure

The port's key layer has no Alt modifier (HANDOFF roadmap #5). Feature
005 adds it: Win32 `WM_SYSKEYDOWN` + `GetKeyState(VK_MENU)` → new
`ITK_ALT('A'..'Z')`, `ITK_ALT_INS/DEL`, `ITK_ALT_PLUS/MINUS`,
`ITK_CTRL_PLUS/MINUS` codes. The F5 stand-ins (r/s/Ctrl-U/Ctrl-D)
remain as documented aliases.

## R10. Dialogs

Number/percent prompts reuse the editor's modal machinery: value
dialogs with a numeric input primed with a default (amplify suggested
%, resize current length, swap/exchange/replace sample number), OK /
Cancel. Confirm dialogs: "Convert sample?" / "Cut sample?" / "Delete
sample?" / centre-DC / quality 3-way ("Convert data / Leave (adjust
fields) / Cancel"-style per O1_ConfirmConvert2List).
