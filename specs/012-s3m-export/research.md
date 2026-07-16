# Research: S3M Export (012) — D_SaveS3M decode

Source: IT_D_WM.INC 753..1755 (D_SaveS3M), IT_DISK.ASM 3264..3357
(ConvertWriteData / D_SaveSampleDataConvert), 4566..4684 (D_SaveModule
extension logic), 7656..7712 (D_SaveSong Ctrl-S extension replace),
IT_OBJ1.ASM 1447..1516 (format radio buttons), IT_DISK.ASM 611..621
(warning texts). TRACKERVERSION = 217h (SWITCH.INC 44).

## R1. Header (0x60 bytes, all counts little-endian)

- 0..25 song name (song header +4); 26..27 = 0; 28 = 1Ah; 29 = 16 (type);
  30..31 = 0.
- 0x20 OrdNum = lastNonFFOrderIndex + 2 (all-FF list -> 2). Same formula
  as the .IT saver.
- 0x22 SmpNum = Music_GetNumberOfSamples (port count_samples()).
- 0x24 PatNum = PE_GetMaxPattern + 1; if > 100: warning row 23 + clamp to
  100 — but the header-length accumulator keeps the unclamped 2*PatNum
  (quirk, port 1:1).
- 0x26 Flags = (ITFlags & 2) << 2 (vol0 optimizations -> 8).
- 0x28 Cwt/v = 3000h + 217h = 3217h.  0x2A Ffi = 2 (unsigned).
- 0x2C "SCRM".
- Instrument mode (ITFlags & 4): warning row 29 (still saves; sample data
  used, plus the note-remap in R4).
- 0x30 GV = ITGV >> 1. 0x31 IS = speed, 0x32 IT = tempo (IT hdr 32h word).
- 0x33 MV = min(ITMV,127) | (ITFlags&1 ? 80h : 0) (stereo bit; the
  shl/shr trick folds the flag's bit 0 in).
- 0x34 uc = 0. 0x35 dp = 252 (default pans present). 0x36..37 = 0.
- 0x38 dword: obfuscated edit timer, same formula as .IT: (ticks +
  Reserved) ^ 'JTHL', ROR 4, NEG, ROL 7, ^ 'ITRK'.
- 0x3C..0x3F = 0 (3E/3F special parapointer unused).
- 0x40..0x5F channel settings from ChnlPan[0..31]:
  - muted (bit 7): 0xFF, pan-block byte 0.
  - else: type = ((n & 1) << 3) | (n >> 1) where n counts unmuted
    channels from 0 (alternating L1,R1,L2,R2...; keeps counting past 16 —
    quirk); pan value v = ChnlPan (surround/other >64 -> 32), pan byte =
    (max(v>>1,1)-1)>>1 | 20h... precisely: t=v>>1; t=t-1+borrow (0 stays
    0); t>>=1; t|=32.
- Orders at 0x60 (OrdNum bytes): first OrdNum-1 entries filtered
  (0FEh/0FFh pass; 100..253 -> 0FFh), then one 0xFF.
- Parapointer tables: samples at 0x60+OrdNum (word each = filepos>>4),
  patterns following (2*PatNum with the clamp).
- 32-byte default-pan block appended at the end of the header area
  (headerlen += 20h); dp=252 promises it.
- Warnings: any ChnlVol[0..31] != 64 -> row 24; linear slides
  (ITFlags & 8) -> row 25.

## R2. Sample headers (0x50 each, 16-byte aligned)

Per sample i (0-based), IT sample header offsets in brackets:
- 0 type = Flags[12h] & 1; 1..12 DOS filename [+4]; 13..15 = 0 (memseg,
  patched later with the 24-bit data paragraph: byte = bits 16.., word =
  low 16).
- 16..27 Length/LoopBeg/LoopEnd [+30h..3Bh]. 28 Vol [+13h]; 29 = 0.
- 30 pack = 0; 31 flags = (ITFlags>>4 & 1) | (ITFlags<<1 & 4)
  (loop -> 1, 16-bit -> 4).
- 32..35 C5Speed [+3Ch]. 36..47 = 0.
- 48..72 sample name [+14h], NULs -> spaces (25 bytes); 73..75 = 0.
- 76..79 "SCRS".
- Warnings: sustain (Flags&32) or bidi (Flags&64, only checked when a
  loop bit 16|32 is on) -> row 27; GvL[+11h] != 64 -> row 26;
  ViD[+4Dh] != 0 -> row 28.

## R3. Patterns (16-byte aligned, length word inclusive)

64-cell scratch (per channel): lastmask, note, ins, vol, cmd, cmdval,
note2, ins2. Init: note = 0FDh, vol = 0FFh, rest 0. Persists across the
whole pattern. Rows != 64 -> warning row 30; shorter patterns pad with
zero row-terminators to 64 rows, longer ones translate ALL rows (quirk).
Missing patterns = the 64-row empty pattern.

Per packed entry (streamed in IT order):
1. cv==0 -> row terminator byte 0.
2. ch = (cv&7Fh)-1; cv&80h -> read+cache mask. Fields read into the
   scratch: note, ins, vol (vol > 64 -> warning row 33 + value 0FFh),
   cmd+val.
3. Channel check AFTER caching: cv&7Fh > 16 -> warning row 31, cell
   dropped.
4. S3M mask = ch | (DH&33h ? 32:0) | (DH&44h ? 64:0) | (DH&88h ? 128:0).
5. note2/ins2 = note/ins; if note < 120 and note AND ins present
   (DH&11h && DH&22h) and instrument mode: note2 = instrument's
   NoteSampleTable note ([ins+40h+2*note]).
6. If mask&32: n = note2; 0FDh (fade) -> byte 0FFh; >= 0FEh -> 0FEh
   (cut); 12..107 -> ((n-12)/12)<<4 | (n-12)%12; else (n<12 or 108..119)
   -> warning row 32, whole cell dropped.
7. Emit: mask byte; if mask&32: note byte + ins byte (ins2 if DH&22h
   else 0); if mask&64: vol byte (may be the 0FFh substitute); if
   mask&128: cmd/val word with translations:
   - S91 -> X A4h;  V,X -> val >>= 1;  C -> ((val/10)<<4)|(val%10);
   - all else verbatim ('A'+cmd-1 already 1-based letters);
   - the H/U vibrato-depth halving is commented out in 2.17 = stays out.

## R4. Sample data + final passes

- Per sample with data: align 16, record 24-bit data paragraph
  (filepos>>4) in a 100-entry patch table (zeros for absent), write
  sign-converted data: 8-bit bytes ^ 80h; 16-bit high bytes ^ 80h
  (i.e. word ^ 8000h), length = Length << is16.
- Seek 0, write the whole header block (headerlen incl. pan block).
- Per sample: seek (sampleHeaderPara<<4)+0Dh, write 3 bytes:
  para>>16, para & 0xFF, (para>>8) & 0xFF.
- Close. (Write errors: sticky NoSaveError; port deletes the partial
  file like the .IT path's D_DeleteIfError.)

## R5. UI integration

- F10 format radio buttons (style 8, group bound to SaveFormat):
  (69,12)-(77,14) " IT214"=0, (69,15)-(77,17) "  S3M"=1,
  (69,18)-(77,20) " IT2xx"=2, (69,21)-(77,23) " IT215"=3.
- D_SaveModule: typed name without '.' gets ".IT" / ".S3M" by format.
- D_SaveSong (Ctrl-S): copies the loaded FileName up to '.', appends
  "IT" / "S3M" (i.e. replaces the extension); no '.' -> name unchanged.
- Save screens: progress texts rows 17..21 (Sample Headers, Pattern n,
  Sample n, File Header, Done) as the .IT path; warnings rows 23..33 in
  attr 4, sticky; if any fired, wait for a key before returning
  (SaveFormatError path).

## R6. Decisions

- D1: reuse save_block/NoSaveError/delete-on-error and the Save_Progress
  hook; add `Save_S3MWarning(row, text)` hook for the eleven messages.
- D2: sign conversion into a scratch buffer (the original XORs in place
  and restores; the port never mutates the song — same bytes on disk).
- D3: counts via the existing count_samples()/max_pattern() (they are the
  ports of Music_GetNumberOfSamples/PE_GetMaxPattern).
- D4: byte-diff against a DOS-written S3M deferred (no DOSBox here);
  field-level assertions + import round-trip in the selftest instead.
