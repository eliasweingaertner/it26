# Research: Editor Polish Batch (013) — ASM decode

## R1. Alt-U (I_UpdateInstrument, IT_I.ASM 6575 -> PE_UpdateInstruments,
IT_PE.ASM 10840)

Instrument = `LastInstrument` (port CurInstr, 1-based). For every
pattern 0..PE_GetMaxPattern: decode; for each cell with a real note
(engine byte < NONOTE) and instrument != 0: linear-search the
instrument's NoteSampleTable (120 (note,sample) byte pairs at +40h)
for a pair equal to the cell's (note, ins); on first match at index
dx: cell.note = dx, cell.ins = the instrument number; store pattern.
Not undoable; runs over all patterns; the % display in the original
draws the "Done" text each pattern (vestigial).

## R2. In-list name editing (I_PostSampleList, IT_I.ASM 1219)

- `SamplePos` (init 25; 0..24 = edit positions in the 26-byte name,
  25 = keyjazz mode), `InstrumentPos` (init 0, mouse cap 24).
- Keys when pos < 25: char >= 32 inserts at pos (shift right through
  index 24, index 25 untouched), pos++. Backspace: pos>0 -> pos--,
  delete char at pos (shift left, last byte 0). Delete: delete at pos
  (no cursor move). Left/Right move (Right caps at 25), Home = 0,
  End = 25. When pos == 25: note keys keyjazz (existing behaviour).
- Mouse: sample list pos = (px-40)/8 capped 25 (name starts cell 5);
  instrument list pos = cell-5 capped 24.
- The selected row's name cell at pos renders attr 30h.

## R3. WAV stereo prompt (O1_StereoSampleList, IT_OBJ1.ASM 8788)

Box (26,22)-(54,29) style 3; "Loading Stereo Sample" at (30,24) attr
20h; buttons style 8 "  Left" (30,26)-(39,28) -> 64 and "  Right"
(40,26)-(50,28) -> 64+128; keys 'L'/'R'. The result ORs into the
loader's BP: bit 7 set = take the right channel. `DisableStereoMenu`
suppresses the prompt (bulk paths) -> port hook = NULL means left.

## R4. hi-ASCII (S_DefineHiASCII, IT_S.ASM 1589; called by
Glbl_Shift_F9, IT_G.ASM 440)

Functionally: load font bank B with the plain ROM font so text drawn
with attr bit 3 (the message editor's colour 12) renders real CP437
high-ASCII. Port: `Screen_DefineHiASCII()` copies `IT_FontROM` into
FontB; bank B is reclaimed by the next GenerateCharacters /
DefineSmallNumbers exactly as in the original.

## R5. AIFF/IFF + TX Wave (IT_D_INF.INC 632..726, 1002..1124;
D_LoadSampleData 12-bit branch, IT_DISK.ASM 2867..)

- IFF: 'FORM' at 0 and '8SVX' (flags 1) or '16SV' (flags 3) at 8;
  format code 17, display name "AIFF Sample". Chunk walk from 12
  (size = BE32 at +4; next = +8+size... the original uses only the
  low word of sizes): NAME -> name (BE16 length at +6 capped 25);
  VHDR -> LoopBeg = BE32@+0Ch, t = BE32@+10h (t != 0 sets loop flag),
  LoopEnd = LoopBeg + t, C5Speed = BE16@+14h; BODY -> Length =
  BE32@+4, data offset = chunk+8, GvL 64, Vol 64, Cvt 1 (signed).
  All BSwapped values halve when 16-bit. (16SV loads little-endian --
  original quirk, kept.)
- TX Wave: 16-byte ident "LM8953" + 10 zero bytes at 0; byte@16h
  & 7Fh must be 49h; format code 13, "TX Wave Sample". Flags =
  sample+16bit (+loop when byte@16h == 49h exactly); attack =
  BE24?@18h & 1FFFFh, looplen = @1Bh & 1FFFFh (dword reads, 17-bit
  masks); Length = attack+looplen, LoopBeg = attack, LoopEnd =
  Length; C5Speed by byte@17h: <2 -> 33000, ==2 -> 50000, else
  16000; data offset 20h; Cvt = 11h (signed | TX 12-bit).
- Loader 12-bit branch (BP bit 5 = Cvt bit 4): bytes on disk =
  ceil(samples*3... chunked); each 3-byte group (b0,b1,b2) expands
  in place from the end to two LE 16-bit samples:
  s0 = b0<<8 | (b1 & F0h); s1 = b2<<8 | ((b1<<4) & F0h).

## R6. Fourier analyser (IT_FOUR.ASM; SPECTRUMANALYSER=1 released)

- Entry: F5 key list, Alt-F12 (scan 158h) -> Fourier_Start.
- Music_GetWaveForm: driver hook (DriverFlags bit 2) returning the
  last 2048 output samples as int16 mono into ES:DI; port: a passive
  ring tap in the WAV driver's render path.
- FFT: 2048-point radix-2, float: bit-reversal table (11-bit reverse
  * 8); per stage i=1,2,4..1024: phase recurrence seeded FSinCos
  (-pi/i), butterfly per the FPU sequence; magnitudes sqrt(r^2+i^2) *
  1/128 (Const1_2048 = 3C000000h) for bins 0..1023; ints = mag>>6
  clamped 255.
- Display (original: VESA 8-bit fullscreen; port: 640x400 8-bit
  overlay + 256-colour palette presented by the pixel rasterizer):
  - scrolling spectrogram: one column per frame at x=CurrentOffset
    (wraps at width): rows top..(H-64) plot bins (H-64)..1 (low
    frequencies at the bottom), pixel = the 8-bit magnitude.
  - bar spectrum: bottom 64 rows; row r (top r=0): threshold =
    (63-r)*4... exactly: for CX=64..1 at y=H-CX: BL=(CL-1)*4; for
    the first min(width,1024) bins (centred when width>1024): pixel
    = FFh when mag > BL else 0.
  - palettes: two gradients (Fourier_SetPalette A/B, 6-bit VGA DAC
    <<2), toggled with 'p'.
- Keys (FourierKeyList): '+'/'-' = the F5 volume keys, 'p' palette,
  Ctrl-F5, the playback command chain (F5..F8); ESC (101h) exits
  (Fourier_PostFunction AX=4). On exit the original re-inits the
  text screen and small numbers.
- Port deviation (documented): fixed 640x400 overlay instead of
  VESA 1280/1024/800 mode probing; terminal backend excluded.

## R7. Instrument-record preview — resolved

`LoadInstrumentKeys`/`ViewInstrumentKeys` (IT_DISK.ASM 869..) contain
no note handling and `D_PostLoadInstrument` falls through to AX=0 --
the original does NOT audition instrument records. The port already
matches; docs updated, no code.
