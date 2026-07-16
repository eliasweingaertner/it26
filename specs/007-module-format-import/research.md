# Phase 0 Research: Module Format Import (IT_D_RM.INC / PE_TRANS.INC)

## R1. Architecture in the original

`D_PreLoadModule` clears everything (patterns, samples, names,
instruments, message, timer), opens the file. Each `D_Load*` builds the
song header/orders/sample headers directly in the song segment, loads
sample data via `D_LoadSampleData` (reads Length×width bytes from the
current file position, converts per Cvt: bit0 clear = unsigned→signed,
bit2 = delta-encoded (XM), bit3+Flags bit3 = IT compressed), and
translates patterns via the `PE_Translate*Pattern` procs which fill the
PE's 320-byte/row buffer and pack it (`PEFunction_StorePattern` = the
exact codec = the port's `Pattern_Pack`). `D_PostLoadModule` closes.
Dispatch is by requester file type (extension-driven with content
sniffing for MOD variants).

**Port**: new `src/it_import.c` — `Import_LoadModule(path)` sniffs
(IMPM → `Music_LoadIT`; "Extended Module: " → XM; "SCRM"@0x2C → S3M;
"MTM" → MTM; 'if'/'JN' word → 669; MOD magic @1080 → 31-instr MOD;
.MOD extension fallback → 15-instr). Patterns build `editcell_t` rows →
`Pattern_Pack`. Sample data read+converted inline; message cleared
(MTM *imports* its comment into the message, 40-char lines, NULs →
spaces). `Save_LoadTime` set; MIDI macros = loader defaults.

## R2. S3M (D_LoadS3M + PE_TranslateS3MPattern)

Header: name 25; Flags = 0x10 | (mastervol&0x80 ? 1) | (s3mflags&8 ? 2);
GV = gv<<1; MV = mastervol&127; IS/IT from header; Sep 128; Special 0;
Reserved = header dword @0x38 (de-obfuscated 'ITRK'/'JTHL' when Cwt ≥
0x3208). Channel pans ch0..31 from @0x40: ≥128 → 32+128; else &127:
≤7 → 0, ≤15 → 64, else 32; |= original bit7. ch32.. = 32+128; vols 64.
Orders 0xFF-filled then OrdNum raw. Default-pan block (byte@0x35 ==
252): 32 bytes; bit5 → pan = (v&0xF)*4+2 (keep bit7). Samples (80-byte
PARA-pointed headers): IMPS, filename 12@+1, GvL 64, Flags = present
(len≠0) | 16-bit ((f>>1)&2) | loop (f&1)<<4; Vol@0x1C; name 25@0x30;
Cvt 0 (unsigned); DfP 32; Len/LoopBeg/LoopEnd/C5 from header; data
para<<4 remembered, loaded per sample after. Patterns: len-worded
packed rows ×64: b==0 row end; ch=b&31; b&32: note FE→254, >7F skip,
else (hi*12+lo)+12; ins >99→0; b&64: vol ≤64 or FF else 64; b&128:
cmd letter A.. verbatim except: Cxx BCD→dec, Vxx param<<1 (sat FF),
XA4→S91 / Xxx param<<1 (sat FF), Dxy dead-code passthrough (the
original's nibble-fix tests AL not AH — authentic, keep verbatim).

## R3. MOD (D_LoadMOD, TranslateMODCommand, PE_TranslateMODPattern)

Variants: sig@1080: M.K./M!K!/FLT4→4ch, NCHN→N, NNCH→NN (MODChannelTable
{4,4,4,6,8,4,8,4} for other codes); no sig → 15-instrument (orders@470,
list@472, patterns@600); 31-instr: orders count@950, list@952,
patterns@1084. Header: Flags 0x31, GV 128 MV 48, 125/6, Sep 64. Pans
≤8ch: L,R,R,L (+L,R)(+R,L) = 0,64,64,0,0,64,64,0; >8: all 32; rest
32+128. Patterns count = max of FIRST 127 order bytes + 1 (authentic
127 scan). Samples: name 22, len/beg/end big-endian words ×2 (loop
fields not doubled for 15-instr), loop flag if replen>1, present if
len>1, C5 = FineTuneTable[finetune&15] {8363,8413,8463,8529,8581,8651,
8723,8757,7895,7941,7985,8046,8107,8169,8232,8280}, Vol@25, DfP 32,
Cvt 1 (signed). LoopBeg > len → 0; LoopEnd = beg+replen*2 capped len.
Data sequential after patterns. Pattern cells (4 bytes): period 12 bits
→ first MODPeriodTable[i] ≤ period → note i+36 (72-entry table, none →
no note); sample = b0&0xF0 | b2>>4; effect per TranslateMODCommand:
0/J(arp, 000 = nothing), 1/F, 2/E (param 0 drops cmd), 3/G, 4/H,
5/L (0→G), 6/K (0→H), 7/R, 8/X (A4→S91), 9/O, A/D (both nibbles → keep
high), B/B, C/volume column (cap 64), D/C (BCD), E-sub: E0→S0x(≠0),
E1/E2→F/E |0xF0 (0 drops), E3→S1x, E4→S3x, E5→S2x, E6→SBx, E7→S4x,
E8→S8x, E9→Q (0 drops), EA→D x0F? = x<<4|0x0F, EB→D 0xF0|x, EC..EF →
SCx/SDx/SEx/SFx, F: ≤0x20 → A else T. Param-0 rule (Command36): whole
command dropped when param 0.

## R4. MTM (D_LoadMTM + PE_TranslateMTMPattern)

Header 66 bytes: name@4 (20), PatNum@26(last+1: value stored +?
[60026] = last pattern; written as-is to PatNum then loop ≤), OrdNum =
[27]+1... (order count byte@27, +1 on copy), SmpNum@30, tracks@24
(word22?), pans@34: PanningPositions[v&15] {0,4,9,13,17,21,26,30,34,
38,43,47,51,55,60,64}; ch32+ 160; Flags 0x31, GV128 MV48 125/6 Sep128.
Samples (37-byte): name 22 (NUL-stops copy, rest zeroed), len dword@22,
loopbeg@26, loopend@30, finetune@34&15 → FineTuneTable, vol@35,
16-bit = (b36&1)<<1, loop if (end-start) > 2, DfP 32 Cvt... AL 0 →
Cvt 0? (`Xor AL` before → Cvt 0 = unsigned? MTM samples are unsigned
8-bit ✓). Orders: 128 raw + FF fill (count+1 copied). Patterns: 32
track numbers (words) per pattern; each track = 192 bytes (64 rows × 3)
read from track area (track n at trackbase + (n-1)*192); track 0 =
empty; assembled channel-major then translated: 3-byte events: b0>>2 =
pitch (0 → none, else +36+1? `Add AL,36` after 6-bit pitch, 0 → NONOTE),
ins = (b0&3)<<4 | b1>>4, vol none, effect = b1&0xF / b2 via
TranslateMODCommand. Comment → song message (NUL→space, CR every 40).
Sample data sequential at end.

## R5. 669 (D_Load669 + PE_Translate669Pattern)

Header 0x1F1: sig word 'if'/'JN'; msg 3×36@2 (first 25 → song name);
NOS@0x6E, NOP@0x6F, orders 128@0x71 (+128 FF); Flags 0x19 (stereo|
linear|old fx), GV128 MV48, 78/6, Sep 64; pans LRLR LRLR (words 0x4000
×4), rest 32+128. Samples (25 bytes): name 13 (also DOSFileName), len
dword, loopbeg, loopend; present if len≠0 (Vol 60, flags 1); loop if
end ≤ len and end-beg ≥ 2 → flags 0x11; C5 8363; Cvt 0 (unsigned),
DfP 32. Patterns 0x600 = 64 rows × 8 ch × 3 bytes; per pattern: tempo
byte @0xF1+p → A-cmd on row 0 channel 8; break row @0x171+p: < 31 →
rows = 32 + C00 at that row channel 8, else rows = br+1. Cells: b0 <
0xFE: note = (b0>>2)+36, ins = ((b0&3)<<4 | b1>>4)+1, vol = (b1&0xF)<<2
(0..60); b0 == 0xFE: vol only = (b1&0xF)<<2; 0xFF: none. Effects
(b2 ≠ 0xFF): cmd nibble: 0→F, 1→E, 2→G (stored to per-channel repeat
memory), 3→EF1-style one-shot (E, param 0xF1? `AX = 'E'+0xF100` = EF1,
memory cleared), 4→H (param |0x80 = speed 8), 5→A (one-shot, memory
cleared); b2 == 0xFF → repeat channel memory. New note clears the
channel memory. Effect param = low nibble.

## R6. XM (D_LoadXM + the four Translate procs)

Header: name@17 (20); InsNum@64100+12; SmpNum starts at 1 (authentic
off-by-one: final = 1+Σ); PatNum@+10; Flags = ((xmflags&1)<<3)|0x35;
GV128 MV48; IS = spd (0→6), IT = bpm; Sep 128; channels@+8: pans 32,
rest 160; orders@+20 len@+4: entries ≥200 → FF; entries ≥ patcount
bump patcount (scratch). Patterns: 4-byte len hdr + header (rows@5,
datalen@7); empty datalen → skip (pattern stays empty); rows ≤ 200
normal; > 200 split: first rows>>1, second (rows+1)>>1 rows as a NEW
pattern number (patcount++) inserted after every order occurrence
(D_InsertOrder). Pattern data: packed: b&0x80: bits 1/2/4/8/16 =
note/ins/vol/effect/param present; else b = note and all 5 bytes
follow. Note: >96 → 0xFF(off) else +11 (0 → 11, authentic); ins >99 →
0 + kill note (note byte → 0xFD unless it was FF); ins valid + note
off → ins 0. Volume byte → IT vol: 0x10..0x50 → v-16; 6x → 95+x; 7x →
85+x; 8x → 75+x; 9x → 65+x; Ax → none; Bx → 203+(x≤3?x:x-1); Cx →
x*5+128; Dx/Ex → none; Fx → XMEffectG[x] {193,197,198,199,199,200,200,
201,201,202}; else none (0xFF). Effects (letter,param) → IT cmd, with
the column-swap: when the effect is "memory" (param 0 for F/E/G/H) or
maps to the vol column (C set-vol, 8 pan, K keyoff w/ vol effect) and
the XM vol byte ≥ 0x60 held an effect, the vol effect moves to the
IT effect column (TranslateXMEffectVolume: 6x→Dx, 7x→Dx0, 8x→D0x|F0
(FF→FE), 9x→Dx0|0F, Ax→Hx0, Bx→H0(x≤4?x:x-1), Cx→S8x, Dx→Px0, Ex→P0x,
Fx→G x0) and the vol column gets the effect's vol-encoding (105+x F,
115+x E, 193 G, 203 H, X→pan>>2+carry+128, C→value, K clears to FF).
Full effect map: 0→J, 1→F, 2→E, 3→G, 4→H (param hi-nibble: depth ≤4
keep else -1), 5→L, 6→K, 7→R, 8→X, 9→O, A→D, B→B, C→vol column, D→C
(BCD), E-sub (like MOD but E3x→S1?? actually: E0→drop, E1/E2→F/E|F0
(0 = memory, no |F0), E3→drop, E4→S3x, E5→drop?? no: E4x→S|30h?
transliterate from the proc: E4→S3x('S',|0x30), E6→SBx, E7→S4x,
E8→S8x, E9→Q (0→vol path), EA/EB→D (0 = memory), EC→SCx, ED→SDx,
EE→SEx, EF→drop), F ≤0x20→A else T, G(0x10)→V (≥0x40 → 0x80, else
<<1), H(0x11)→W (nibbles <<1, sat 0xF / 0xF0), R(0x1B)→Q, T(0x1D)→I
(zero nibbles → 1), X(0x21)→X1x→F|0xEx? (X1x → F Ex, X2x → E Ex:
param = 0xE0|x), Z(0x23)→Z. Rows < 32 → MaxRow 31 + C00 break at the
last row's first free effect slot.
Instruments: name@4 (22) → InstrumentName; empty instrument (numsmp@27
== 0) → header only. Fadeout = (xmfade@239 + 15)>>5 cap 256.
NoteSampleTable: notes 0..11 & 108..119 = (note,0); 12..107: sample =
xmtable[96]@33 + current sample base (SmpNum before this instrument's
samples are added), cap 99; note identity. Volume envelope: XM env off
→ IT {Flags 5, Num 2, nodes (64@0),(0@1), SLB/SLE 0} (keyoff-cut
emulation); on → Flags = on|loop|((xmflags&2)<<1 = susloop from XM
sustain)... transliterate: Flags = (xm&2)<<1 | 3; nodes ≤ 12, mag cap
64; loop pts from hdr when XM loop flag else num-1/num-1; SLB=SLE=
sustain pt; xmflags==7 && sustain ≥ loopend → clear susloop; XM
without sustain flag → susloop at num-1/num-1 + susloop ON (hold-at-
end emulation). Pan envelope: Flags = direct map (on|loop(xm&4→bit1)|
sus(xm&2→bit2)); mag = y-32; sustain ≥ end (when sus set) → clear
susloop. Sample headers (40 bytes each, all read before any data):
len/loopstart/looplen dwords (bytes; >>1 if 16-bit; cap 4177910),
LoopEnd = start+len; vol@12, finetune@13 (sar 4 → index), type@14
(bits 0-1 loop type → IT loop | pingpong(bit6), bit4 16-bit), pan@15 →
DfP = (pan>>2)+carry(bit1)+0x80; relnote@16 signed; name@18 (22);
C5 = (PitchTable[relnote+60] * FineTuneTable[(ft>>4)&15]) >> 16;
Cvt = 5 (signed+delta); vibrato: ViS/ViD = swap(word@237), ViR =
(0x100 - sweep@236)&0xFF, ViT = type@235; data offset = current file
pos, then skip len bytes; data loaded at the very end (delta-decoded
by Cvt 5).

## R7. Verification

No non-IT modules exist in testdata/ — synthetic minimal test files
(one sine sample, small patterns) are generated by
`tools/gen_import_tests.py` for .S3M/.MOD/.XM/.MTM/.669; the selftest
loads each and requires a non-silent 2-second render; the IT gates are
untouched. Deviations: none by design (all conversion rules
transliterated); requester filter extended to the five formats.
