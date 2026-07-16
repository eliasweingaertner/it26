# Contract: F3 sample editor & instrument ops

## C1. Waveform view

Box (54,25)-(77,30) style 9; cells (55..76, 26..29) = font-B chars
1..88 attr 0Dh; canvas 176×32 per research R2 (column min/max with
previous-column smoothing, row map ((v>>1)+2)>>2, loop dashes 2-px /
sustain 1-px that may clear waveform pixels). Empty sample ⇒ blank
canvas. Regenerated whenever F3 is drawn.

## C2. Editable fields

Filename (12 chars), C5 speed (number, cap 9999999), Loop tri-state
Off/Forwards/Ping Pong (flag bits 4 / 4+64), Loop Beg/End, SusLoop
tri-state (bits 5 / 5+128), SusLoop Beg/End. Every commit runs the
I_CheckLoopValues clamps and refreshes playing slaves' loop state
under lock.

## C3. Sample ops (Alt keys, research R3 table)

All arithmetic transliterated; confirms/prompts per R10. After each
data-changing op the waveform redraws and playback was stopped first.
Insert/Remove/Swap/Replace fix references (NoteSampleTable in
instrument mode / pattern instrument bytes in sample mode).
Alt-O/T/W (disk saves) flash "feature 006"; Alt-Y is the authentic
no-op stub.

## C4. Instrument ops

Alt-Ins/Del slot insert/remove (554-byte shifts + remaps +
Music_ClearInstrument on remove), Alt-S swap / Alt-X exchange /
Alt-R replace / Alt-P copy (per original bindings on the instrument
list: swap/exchange/replace/copy via number prompt), Alt-J scale
instrument volumes (GbV*amp/100 cap 128). Note window: Alt-A all =
sample++ series, Alt-N/P next/previous fill, Alt-Up/Down transpose
±1, Alt-Ins/Del row insert/delete, Enter pickup, </> sample dec/inc.

## C5. Envelope presets (F4)

Digit '0'..'9' on the envelope loads preset n (Magnitude −=
compensate on pan/pitch); Alt-digit saves current envelope to preset
n (Flags & 0x7F, Magnitude += compensate). Presets default to the
flat 2-node 32-amplitude envelope. Enter grab additionally enables
the envelope (I_EnvelopeSelected; pitch tab sets the filter flag when
the envelope was off).

## C6. Gates

Determinism ×4 + roundtrip ×4 unchanged. Selftest: render F3 with
waveform, run invert twice (data restored), reverse twice (data +
loops restored), amplify 100 (data unchanged), quality toggle
round-trip, loop clamp check — report `F3 OK`.
