# Research: Load Samples & Instruments from Other Modules

Decoded from `impulse-tracker-jthlim/IT_D_RIS.INC` (1498 lines),
`IT_D_RI.INC` (771 lines) and the driving code in `IT_DISK.ASM`
(records, dispatch tables, `LSWindow_Enter`, `LIWindow_Enter`,
`LoadSample`, `D_LoadSampleData`, preview machinery). This file is the
porting contract; section numbers are referenced from tasks.md.

## R1. The library model (IT_DISK.ASM)

The load-sample / load-instrument screens list **records** in
`DiskDataArea`:

* **Sample record = 96 bytes** — an ITS header (0x50 bytes, exactly our
  `sample_t` on-disk prefix) plus: `+0x50` file size in bytes, `+0x54`
  DOS date/time, `+0x58` format code, `+0x59` channels (MOD only;
  `InSampleFormat`/`InSampleChannels` are stored as one word).
  `+0x48` (ITS `SamplePointer`) holds the **file offset of the sample
  data inside the source file**; `+4` (DOSFileName) holds the *source
  file's* name — written by `TransferFileName` together with `'IMPS'`
  and the date.
* **Instrument record = 48 bytes** — `+0` format code, `+1` DOS
  filename (13), `+15` instrument name (25), `+40` word number of
  samples, `+42` word 0, `+44` dword file offset of the instrument
  header (`LoadInstrumentOffset`). Written by `TransferInstrumentName`
  + per-format tail.

Format codes (sample side, `SampleFormatNames` / `LoadSamplesInModuleTable`):
1=directory, 2=ITS, 3=ST sample, 5/7=WAV 8/16, 8=XM smp, 9=PTM smp,
10=MTM smp, 11=669 smp, 12=FAR smp, 13=TXWave, 14=MOD smp, 15=KRZ,
16=PAT, 17=AIFF; **0x20+n = module containing samples** where n indexes
`LoadSamplesInModuleTable`: 0=S3M, 1=IT, 2=XM, 3=PTM, 4=MTM, 5=669,
6=FAR, 7=MOD, 8=MOD(15-instr — same routine), 9=KRZ, 10=PAT.
`LoadULTSamplesInModule` is **commented out** in 2.17 — ULT is not a
supported source and stays unsupported (spec FR-002a lists it; the
source of truth overrides: IT itself cannot rip ULT).

Instrument-side codes (`InstrumentLoaderTable`, index = code−3):
3=.ITI file, 4=.XI file, 5=instrument **in** .IT module,
6=instrument **in** .XM module, 8=.IT module (browse), 9=.XM module
(browse).

Flow (`LSWindow_Enter`): Enter on a module record (code ≥ 0x20) →
remember filename/format, open file, `NumSamples=1`, call the scan
routine (fills records from index 1), close, write record 0 =
`ExitLibraryDirectory` (a fake "Directory" row named
`········Directory········`, code 1), set `SamplesInModule=1`.
Enter on that Directory row → restore directory listing
(`SamplesInModule=0`). Enter on a sample record (code < 0x20, ≠1) →
`Music_Stop`, release check sample, optional "Initialise instrument?"
prompt (instrument mode + target slot previously empty), then
`LoadSample` into the slot chosen on F3, and jump back to F3.
There is **no overwrite warning** in the original; spec FR-004 adds one
(documented deviation).

`LIWindow_Enter` (instrument side): code 1 → exit library; codes 8/9 →
scan module into records + `ExitInstrumentLibraryDirectory` row; codes
3..6 → the actual load (R5).

## R2. Sample scanners (IT_D_RIS.INC) — one record per sample

All scanners: skip empty samples, `GvL=64`, name from source,
`TransferFileName` stamps IMPS/filename/date, `NumSamples++`.
Per format (offsets are into the source file structures):

* **MOD** (`LoadMODSamplesInModule`): read 1084-byte header; pattern
  count = max of the **first 127** order bytes (`AX=07F00h` loop) + 1;
  first data offset = 1084 + patterns·channels·256 (channels from the
  requester's classifier = `InSampleChannels`); 31 headers at +20,
  stride 30. Keep sample if big-endian length word > 1. Flg = 1
  (|0x10 if loop length word > 1); Vol from +25; name 22; Cvt=1,
  DfP=32; Length/LoopBeg/LoopEnd = big-endian words ×2, LoopEnd =
  beg+len; C5 = `FineTuneTable[finetune&15]`; data offsets chained
  (running `EBP += length`). File size = length. **The same routine
  serves the 15-instrument variant (table entry 8) with unchanged
  31-instrument offsets** — authentic.
* **S3M**: read 2000-byte header; for each of `[+0x22]` sample paras
  at `[+0x20]`: seek para×16, read 80 bytes; keep if type==1 &&
  length!=0. Flg = 1 | (flag&1 ? 0x10 loop) | (flag&4 ? 2 16-bit)
  (via the `And AX,401h` trick); Vol +0x1C; name 25 @+0x30; **Cvt=0
  (unsigned)**, DfP=32; Length/Beg/End dwords +0x10/+0x14/+0x18; C5
  dword +0x20; file size = length (×2 if 16-bit); data offset =
  memseg (`[+0x0D]`hi | `[+0x0E]`lo) ×16.
* **FAR**: header 98 bytes; seek past text `[+96]`; read up to byte
  869; **pattern count hardcoded 256** (a `Comment ~` block shows the
  abandoned order-scan); sum 256 pattern-length words at +357 → skip;
  read 8-byte sample map; count bits = sample count; headers 48 bytes
  each follow; keep if length dword @+32 ≠ 0 (and >1 for NumSamples++
  — a record written for length 1 is overwritten by the next: keep
  quirk). Flg = 1 | (word@+46: bit0 16-bit → ×2? `And AX,801h;
  ShL AX,1` → Flg |= 2·16bit | 0x10·loop(bit11)); Vol=64 fixed;
  name = ASCIIZ up to 25; Cvt=1, DfP=32; Length/Beg(+38)/End(+42)
  halved if 16-bit; C5 = 8363; data offset = current file pos; then
  seek += length bytes.
* **MTM**: header 66 bytes; first data offset = 194 + NOS·37 +
  comment length (+28) + 64·(LastPatternSaved+1) + 192·TracksSaved,
  seeded into **record 0's** `+0x48` with `+0x30`=0 so the chain
  `ptr = prev.ptr + prev.Length` works; 37-byte sample headers; keep
  if length dword @+22 ≠ 0. Flg = 1 (|0x11 if loopend−loopbeg > 2);
  Vol @+35; name 22 ASCIIZ; Cvt(=0? — the tail `Xor AX,AX` clears it,
  then) DfP=32 via `Mov AH,32`; Cvt byte = 0 → **unsigned**; length
  @+22, beg @+26, end @+30; C5 = FineTuneTable[+34 & 15].
* **669**: header 0x1F1; first data offset = 0x1F1 + 0x19·NOS +
  0x600·NumPatterns, seeded via record 0 like MTM; 0x19-byte headers;
  keep if length @13 ≠ 0. Flg = 1 (|0x11 if loopend @21 ≤ length);
  Vol = 64; name 13 bytes (rest zero); Cvt=0 (unsigned), DfP=32;
  loop begin @17; loop end @21 only when looped, else 0; C5 = 8363.
* **IT**: read 2000-byte header (authentic 2000-byte cap on the offset
  table — our port reads the real table; bounds-clamped deviation);
  for each of SmpNum offsets at `0xC0 + (InsNum+idx)·4 + [+0x20]`:
  seek, read the 80-byte ITS header **verbatim** into the record; keep
  if Flg bit0; file size = Length (×2 if 16-bit); `TransferFileName`
  then overwrites ID/filename/date. Cvt/Flg (incl. compressed bit 3)
  and SamplePointer stay — the loader handles decompression.
* **XM** (`LoadXMHeader` + `LoadXMSamplesInModule`): read 74-byte
  header, seek to +60+headersize, skip NumPatterns pattern blocks
  (9-byte header: dword headerlen, word datalen @+7); per instrument:
  dword size field + rest; NumSamples word @+27; read 40-byte sample
  headers ×N; per sample keep if length ≠ 0. Flg = 1 | 2·(type&0x10)
  | loop bits from type&3 when looplen @+8 > 1 (`0x10|((type-1)<<6)`
  → 0x10 forward, 0x50 pingpong); Vol @+12; name 22 @+18; Cvt=5
  (signed+delta), DfP = (pan@+15 >>2) +adc 0x80 (pan/4 rounded, use-pan
  bit set); Length/Beg/End via `LoadXISample5` (clamp 4177910, ÷2 if
  16-bit), End = Beg+LoopLen; C5 = `PitchTable[relnote+60] ·
  FineTuneTable[(finetune>>4)&15] >> 16`; data offsets = file pos
  after the instrument's headers + running byte sum (EDX).
* **KRZ** (`LoadKRZSamples`): 32-byte header; `[+4]` big-endian =
  sample-data start; walk blocks from offset 32: read 1000 bytes,
  advance by `(bswap16(word@+6)+7)&0xFFFC`; keep objects with type
  byte `[+4]` in 0x98..0x9B. Header pointer = `bswap16(word@+8)+20`
  into the block (=`BX`). Flg = 3 (16-bit) | 0xC0 if `[BX+1]`&2
  pingpong | 0x10 if !( `[BX+1]`&0x80 ) and loopbeg≠loopend; Vol=64;
  name ASCIIZ @+10 up to 26; Cvt=3 (**signed + big-endian**), DfP=32;
  start = bswap32@BX+8 (=EBP); Length = bswap32@BX+20 − EBP; loop
  beg/end = bswap32@BX+16/+20 − EBP, **skip the sample if end < beg**;
  file size = 2·Length; C5: period = bswap32@BX+28; C5 = period ? 
  (10^9 / period, or 44100 if that quotient is 0) : 22000; data
  offset = sample-data start + 2·EBP.
* **PAT** (`LoadPATSamples`): read 129+63+47 bytes (file + instrument
  + layer headers); wave count = byte `[+129+63+6]`; per wave: read
  96-byte wave header; Flg = 1 | 2·(modes&1) | 0x10·(modes&4) |
  0x40·(modes&8) (modes byte @+55); Vol=64; name = instrument name
  (@129+2, 16) + `':'` + wave name (@+0, 7); Cvt = (modes&2)?0:1
  (unsigned bit ^1), DfP=32; **Length = dword @+16 (the loop-end
  field — authentic quirk)**, LoopBeg @+12, LoopEnd @+16, all halved
  if 16-bit; C5 = word @+20; data offset = current pos; then seek +=
  dword @+8 (true data size).

## R3. Loading one sample (`LoadSample` + `D_LoadSampleData`)

`LoadSample(slot, record)`: if Length==0 or !(Flg&1) → just copy the
header. Else `Music_Stop`, open `record+4` filename, seek to
`[+0x48]`, `D_LoadSampleData`, close; then copy 0x48 header bytes into
the song slot, zero the slot's `OffsetInFile`, copy the 4 vibrato
bytes, `Music_SoundCardLoadSample`.

`D_LoadSampleData` conversion flags: `BP = Cvt<<1 | (Flg&2)>>1 |
(Flg&8)<<12` →
bit0 16-bit, bit1 **signed** (clear = XOR 0x80/0x8000), bit2 byte-swap
(16-bit big-endian, swapped **before** delta), bit3 delta (16-bit wide
when bit0), bit4 byte-delta (forces byte-wise delta even on 16-bit —
PTM), bit5 TX 12-bit (standalone TXWave only — not ported), bit6
stereo prompt (WAV only — not ported), bit15 IT-compressed (2.14/2.15
decompressor, IT215 when Cvt&4). Order: byte-swap → delta (running
accumulator across the whole sample) → unsigned→signed. Afterwards the
header's `Flg &= ~0x0C` and `Cvt = 1`.
Port: `Load_SampleData(filedata, size, sample_t*)` exported from
`it_load.c` (reuses `DecompressIT8/16`), superset of the existing
loader path; pads +4 for the cubic interpolator and clamps loops like
`Music_LoadIT` does.

## R4. Preview (check slot) and lazy info

`MAX_SAMPLES` slot **99** (0-based; "sample 100") is the check slot.
On the load screen, a note key (IT piano layout) plays
`12·BaseOctave + note` (≤119) — `LoadSample(99)` on demand
(`SampleInMemory`/`SampleCheck` track loaded state, stereo prompt
disabled), then `Music_PlaySample(note, smp 100)`; key release sends a
note-off through `Music_PlayNote` (NoteData 0xFF, DH=32). Selection
change copies the record to `CheckDataArea`. In the file list, a
record whose format byte is 0 is lazily classified+read when the
cursor lands on it (`D_LoadSampleHeader`).

## R5. Instrument side (IT_D_RI.INC + `LIWindow_Enter`)

Scanners (codes 8/9) build 48-byte records:
* **IT**: per instrument offset (dword table at `0xC0+[+0x20]`): read
  554-byte header; mark `InstrumentTable[s]=1` for every note-table
  sample byte < 100; count marks 99→1 (index 0 never counted); skip
  the instrument if 0; record: name = 25 bytes @+0x20, count, offset.
* **XM**: per instrument block (running file pos = offset): size
  dword + rest; skip if NumSamples word @+27 == 0; read 40-byte
  sample headers; record: name 22 @+4, count; advance past data
  (sum of sample byte lengths).

Loaders (Enter on a record / standalone file; `LoadInstrumentOffset` =
record `+44`, `NumInstrumentSamples` = record `+40`):
1. **Out-of-slots check**: `UnusedSamples` = 99 − (slots with Flg bit0),
   computed **when the requester opens**; if record count >
   UnusedSamples → "Out of samples" message, abort (checked *before*
   the exclusive-sample release — authentic conservatism).
2. `Music_Stop`; release samples referenced **only** by the target
   instrument (mark target's 120 note-table samples, unmark those used
   by any other instrument 0..98, `Music_ReleaseSample` the rest).
3. Reopen file, dispatch:
   * `LoadITInstrument` (.ITI): read 554-byte header (ConvertOld if
     Cmwt<0x200), then NoS×80 sample headers verbatim; data offsets in
     the .ITI itself.
   * `LoadXIInstrument` (.XI): header 298 bytes; build an ITI image:
     fadeout=(x+15)>>5 cap 256, PPC=60/PPS=0, GBV=128/DfP=32, name
     @+21; note table = 12 empty + 96 entries (sample+1) + 12 empty;
     vol/pan envelopes from flags @+266/+267 (`And AX,402h` bit
     shuffle | bit0), node counts @+258/259 capped 12, loops @+261
     /+264, sustain @+260/+263 duplicated; 12 nodes of (y @+2 capped
     64 [pan: −32], word x) from the 6-byte-stride point arrays
     (vol @+66+96=+162, pan following); pitch envelope = 2 nodes
     (0,0)-(99,0); if <2 vol nodes force flat 64/64 pair, same for
     pan at 0; then NoS×40 XM sample headers → ITS records exactly as
     the XM scanner (Cvt=5, data chained from `XISampleOffset` =
     298 + 40·NoS).
   * `LoadInITInstrument` (code 5): read module header (2000), seek
     `LoadInstrumentOffset`, read 554; ConvertOld if module Cmwt <
     0x200; walk the 120-entry note table, for each new sample number
     (≤ module SmpNum, not yet seen) assign the next compact index,
     seek the module's sample-offset table entry and read that 80-byte
     header to slot; rewrite the note table through the map; NoS =
     count.
   * `LoadInXMInstrument` (code 6): seek offset, read size+rest,
     name @+4, NoS @+27, current pos → `XISampleOffset`, then the XI
     conversion chain above.
4. Transfer into the song: find NoS free sample slots (Flg bit0
   clear, 1-based scan), copy each 80-byte header (zeroing the file
   offset, then **word `+0x2E` = 1** → Cvt=1, DfP=0 — authentic
   overwrite), load data (seek `+0x48`, `D_LoadSampleData`) when Flg
   bit0; copy the instrument header (0x40 bytes), remap the 120
   note-table sample bytes through the allocation map, copy the
   remaining 250 bytes; `Music_SoundCardLoadAllSamples`; if not in
   instrument mode, prompt "Enable instrument mode?" → set Song flag
   bit 2.

## R6. Sample/instrument disk saves (deferred from 005)

`D_SaveSample` (IT_DISK.ASM): Alt-O = save ITS ("Impulse Tracker
sample saved"), Alt-T = Scream Tracker sample, Alt-W = raw/WAV —
2.17 builds with `SAVESAMPLEWAV=1` → 44100 Hz mono WAV header, 8/16
bit per sample flags. ITS save = 0x50 header (SamplePointer = 0x50)
+ raw data via `D_SaveSampleData` (**uncompressed**, unsigned removed
— data is already signed in memory, written verbatim). Instrument
save (F4 Alt-O... `SaveITIList`): 554-byte header + NoS sample
headers + data, "Instrument saved". Exact key list re-checked from
IT_I.ASM during implementation; filenames from the sample/instrument
DOS filename field, error "No Filename?" when empty.

## R7. Port mapping

* New `src/it_ris.c/h`: record structs (`slibent_t` embeds
  `sample_t`), `RIS_ScanModule`, `RIS_LoadSample`,
  `RIS_LoadStandalone(.ITS)`, `RI_ScanModule`, `RI_LoadInstrument`,
  `RIS/RI_FormatName`. Whole source file read into memory once
  (matches `it_import.c` style); scanning and loading are pure memory
  ops over `rd`-style readers.
* `it_load.c` exports `Load_SampleData` (R3) and keeps the
  decompressors private.
* Editor: Enter on the F3 sample list opens the sample library
  requester; Enter on the F4 instrument list opens the instrument
  requester (authentic IT behaviour). Both requesters reuse the F9
  chrome/boxes with the two-level list (files ↔ library), the
  Directory exit row first, note-key preview through slot 99, and the
  spec-mandated occupied-slot confirm (deviation from the original —
  documented in README).
* Serialisation: loads stop playback first (`stop_song`), slot
  mutation under the engine lock, never holding it across file I/O
  that can call locking engine entry points (deadlock lesson from
  007).
* Formats NOT ported, per the 2.17 source itself: ULT (commented
  out), TXWave 12-bit, WAV/AIFF standalone, stereo prompt. Noted in
  README.
