# ittrack — native cross-platform port of the Impulse Tracker 2.17 engine

A 1:1 C transliteration of Jeffrey Lim's Impulse Tracker playback engine,
ported directly from the released x86 assembly source
(https://github.com/jthlim/impulse-tracker, the author's own publication;
the herrnst/impulsetracker mirror is byte-identical in every file this
port is based on). This is **not** a generic
module player wired to .IT files — the playing routines are the original
ones, translated instruction-for-instruction:

| Port file        | Original source                | What it is |
|------------------|--------------------------------|------------|
| `src/it_music.c` | `IT_MUSIC.ASM`                 | sequencer core: `Update`, `UpdateData`, `UpdateNoteData`, `UpdateInstruments`, `UpdateSamples`, `UpdateEnvelope`, `UpdateVibrato`, NNA channel allocator, pitch slides (FPU paths), `Music_*` play control |
| `src/it_effects.c` | `IT_M_EFF.INC`               | all effect handlers A–Z, volume-column effects, S-commands, with the original dispatch tables |
| `src/it_tables.c` | `IT_MUSIC.ASM` data            | pitch table, waveform tables, slide LUTs — transcribed verbatim *including the original tables' typos* (they are part of the sound) |
| `src/it_driver.c` | `WAVDRV.ASM` + `MIXWAV.INC` + `WAV.MIX` | the high-quality software driver (= ITWAV.DRV): 256 channels, cubic spline interpolation, the IT resonant filter (`F0 F0` MIDI intercept, coefficients per `Q.INC`), volume ramping, click removal, error-feedback dither |
| `src/it_load.c`  | (re-implementation of the `IT_DISK.ASM` load path against `ITTECH.TXT`) | .IT loader; keeps patterns in the packed on-disk format because the engine decodes packed rows directly, IT 2.14/2.15 sample decompression, embedded/default MIDI macros |
| `src/it_structs.h` | `InternalDocumentation/CHANNEL.TXT` | host/slave channel layouts, byte-for-byte, enforced with `_Static_assert` |

The build configuration mirrors the 2.17 release (`SWITCH.INC`):
`USEFPUCODE=1` (FPU slide math; the 2.14 lookup-table variants are also
ported under `USEFPUCODE=0`), and the driver uses `WAVSWITC.INC` settings
(`VOLUMERAMP=1`, `CUBICINTERPOLATION=1`, `DITHEROUTPUT=1`, `RAMPSPEED=8`,
`RAMPCOMPENSATE=255`).

## Building

Any C11 compiler. With CMake:

    cmake -B build && cmake --build build --config Release

or directly with MSVC:

    cl /std:c11 /O2 /D_CRT_SECURE_NO_WARNINGS /Fe:itplay.exe src\*.c

or gcc/clang (Linux/macOS):

    cc -std=c11 -O2 -o itplay src/*.c -lm -lpthread -ldl

Audio output is miniaudio (vendored, single header) — WASAPI on Windows,
CoreAudio on macOS, ALSA/PulseAudio on Linux.

The editor's authentic pixel window on Linux/macOS uses **SDL2**, an
optional dependency: install the dev package (`libsdl2-dev` /
`SDL2-devel` / `brew install sdl2`) and CMake builds it automatically.
Without SDL2 — or with `-DITED_SDL=OFF` — `ited` still builds and runs
with the terminal backend, and the `itplay` player never needs SDL.

## Usage

Two executables are built, both over the same engine:

    itplay <module.it> [-r hz] [-w out.wav] [-o order] [-q]   # player
    ited   [module.it] [-r hz]                                # editor

`itplay -w` renders to a WAV file — which is what this driver originally
existed for, so it doubles as a regression harness (renders are
bit-deterministic).

`ited` is the editor (see below). It opens a 640x400 pixel window (2x
scaled) that reproduces IT's VGA 80x50 text mode with the real font,
palette and bevel glyphs — via the Win32 backend on Windows and the SDL2
backend on Linux/macOS (built when SDL2 is present; see Building). Set
`ITED_TERM=1` to force the truecolor terminal backend instead, which is
also the automatic fallback when no display is available (resize your
terminal to at least 80x50). On POSIX the terminal backend carries the
full input surface: Alt/Ctrl/Shift key combos via the xterm encodings
(ESC prefix, modified CSI, `modifyOtherKeys`) and mouse via SGR
reporting — clicking, dragging thumbbars and pattern-grid clicks work
over SSH.

## Editor (`ited`)

A cross-platform reproduction of the Impulse Tracker editor — original
screen layouts, the default "Camouflage" palette, the custom UI glyphs
and box bevels, all extracted byte-exact from the released IT 2.14
source — on top of the ported engine, with live playback through the
same WAV/hiqual driver. Screens and keys follow IT 2.x:

| Key | Screen / action |
|-----|-----------------|
| F1  | Help (key reference) |
| F2  | Pattern editor |
| F3 / F4 | Sample list / Instrument list (piano keys audition; **Enter = load sample/instrument** from another module or file, as in IT) |
| F11 / F12 | Order list & panning / Song variables |
| F5  | Play song + live info page (per-channel VU / sample / pan) |
| F6 / F8 | Play current pattern / stop |
| F9  | Load module (file requester) |
| ESC | Main menu (the original menu tree: File, Playback, Sample, Instrument menus) |

All screens are operable with IT's object model: Tab/Shift-Tab and the
arrow keys move the focus between widgets (focused buttons show bright
text, focused sliders a white thumb — as in the original), Left/Right
adjust the focused thumbbar, Space/Enter presses buttons and toggles,
text fields take typed input. The mouse works in the pixel window:
click to focus/activate, drag thumbbars, click list rows and menu
items, click the pattern grid to move the cursor. F12's "Save all
Preferences" writes `ited.cfg` (directories, octave, edit step), read
back at startup.

Pattern editor: arrows / PgUp / PgDn / Home / End move the cursor, Tab
switches channel, `[` `]` change octave, `{` `}` change edit step, `-`
`=` change pattern, Ins/Del push/pull rows. Notes are entered with the
IT piano layout (lower octave `Z..M` = C..B, upper `Q..U` = C..B; `1` =
note cut, `` ` `` = note off); the instrument/volume/effect columns take
digits and effect letters.

The editor keeps patterns in an unpacked grid (`it_pattern.c`) and
re-packs into the player's packed format on every edit, serialised
against the audio thread with a mutex. The pack/unpack is the exact
inverse of the player's decoder; a regression test
(`tests/test_pattern.c`) verifies that round-tripping every pattern of a
module through unpack→pack yields **byte-identical audio**, and that the
round-trip is idempotent.

The display layer (`it_screen.c`) keeps IT's model: an 80x50 buffer of
(character, attribute) cells. The palette, the custom glyphs 128..201
and the box-style tables are generated byte-exact from `IT_S.ASM`
(`src/it_vgadata.c`); the header, pattern editor, sample list, song
variables and load screens use the original layout data from
`IT_F.ASM`/`IT_PE.ASM`/`IT_OBJ1.ASM`. Three interchangeable backends
present the cells behind one `screen_backend_t` vtable: the Win32 pixel
window (Windows), the SDL2 pixel window (Linux/macOS) — both the
authentic look, rendered from the shared `Screen_Rasterize` — and a
24-bit-truecolor VT terminal fallback.

The in-depth sample/instrument editors (waveform view, Alt-key ops,
envelopes), the F5 info page views, the message editor, saving (.IT
writer with the IT215 compressor), module import (S3M/XM/MOD/MTM/669)
and the sample/instrument library (rip from other modules) are all
ported; remaining gaps are listed in `docs/HANDOFF.md` §6 (S3M export,
Alt-F12 Fourier, small F3/F4 polish items).

## Fidelity notes (deviations from the DOS binary)

Mechanical translations:
- Segmented addressing → pointers/indices. Instrument offsets became
  1-based instrument numbers, sample header offsets became 0-based
  indices, channel offsets became table indices. All comparisons and
  state transitions are unchanged.
- The DOS 64KB sample-segment chunking (`MFS`/`MBS`) collapses to single
  inner-loop calls; the chunk arithmetic (`ceil(amount/step)`, loop wrap
  modulo) is identical.

Numerical:
- The original runs its FPU with a 24-bit (single precision) control
  word. Slide math (`2^(x/768)` etc.) is emulated in C `double`/`float`
  with `llrint` for the FIST rounding and the `80000000h` out-of-range
  result reproduced. Differences are confined to the last bit of
  intermediate rounding.

Behavioral:
- The WAV driver's 4-band output equalizer is ported but bypassed by
  default: its parameters lived in the user's saved driver config, and
  the source-default volumes would silence the output.
- The driver renders in real time even when the engine is stopped
  (the original only wrote file blocks while playing).
- Old-format (pre-2.00) instrument conversion is best-effort.
- MIDI *output hardware* is not implemented; the MIDI macro engine
  (`MIDITranslate`) is fully ported because Zxx macros drive the
  resonant filters through it.

Info page (F5):
- IT 2.17 has **no oscilloscope**: what the track view draws are
  velocity bars (a min/max scan of the sample span mixed since the last
  frame, scaled by the channel's final volume). They are ported exactly.
  `Display_SampleDots` is commented out of the 2.17 mode table and is
  excluded here too.
- The F5 Alt combos predate the key layer's Alt support (feature 009),
  so their portable stand-ins remain as aliases: Ctrl-U/Ctrl-D =
  Alt-Up/Alt-Down (window resize), 'r' = Alt-R (reverse output),
  's' = Alt-S (stereo toggle). The shifted-letter aliases (Q/S/G/V/I)
  are as in the original. Alt-F12 (Fourier spectrum analyser) is not
  ported.
- The velocity-bar scan is bounds-clamped to the sample data (the
  original scans raw DOS memory for transient mixer offsets).

Module import:
- S3M/XM/MOD/MTM/669 conversion follows IT 2.17's own importers
  including their quirks (MOD pattern count scans only the first 127
  order entries; XM's saved SmpNum is one high and an XM note byte of
  0 in an uncompressed cell becomes B-0; the S3M Dxy nibble fix is
  dead code and stays a passthrough) — imported modules match what the
  original IT produced, not a "corrected" conversion.

Pattern editor (F2):
- The keyjazz map is the original `KeyBoardTable` (Z-row 12 semitones +
  Q-row 17); an earlier port build also mapped `; , . l /` to notes —
  that extension is removed so `,` is the edit-mask key and `; '` cycle
  the instrument, as in IT.
- Undo is IT's own 10-slot typed history: each destructive block/row op
  snapshots the whole pattern with a type caption, and Ctrl-Backspace
  opens the pick-a-snapshot requester (reverting pushes a Redo entry).
  It is not an unlimited multi-level history — that matches the
  original, not modern trackers.
- Block operations, the edit mask, multichannel entry and template
  stamping follow the `PEFunction_*` handler bodies including their
  quirks (repeat-key alternate behaviours, PEGetVolume default-volume
  lookup for Alt-J on empty cells, transpose clamped to C-0..B-9).
- The multi-scheme pattern views (Ctrl-0..5 fast views,
  Ctrl-Shift-1..4 presets, Alt-T method cycle, Alt-R clear, Alt-H
  division, Ctrl-T view tracking) are ported from the five View*
  renderers in IT_PE.ASM, including the font-bank-B packed-digit cells,
  the small G0..H9 volume-effect glyphs and the half-cell invert cursor
  (`S_InvertCursor` on char 246). The in-F2 mute/solo key family
  (`\`/Alt-F9 toggle, keypad `/` mute+advance, `?` mute-previous,
  Alt-F10 solo, `|` solo+advance, Alt-`\` unmute-all) drives the same
  engine mute table as F5/F11.
- Ctrl-F2 opens the original Set Pattern Length requester (length
  32..200, start/end pattern range). As in IT, `Pattern Length` is not
  re-primed from the current pattern (it persists across invocations)
  and OK always rewrites the range. Deviation: the port pushes one
  "Pattern data" undo snapshot of the **current** pattern first — the
  original's resize is not undoable at all; other patterns in a range
  resize remain non-undoable.
- ViewDivision/ViewTracking/row-hilight+centralise (`PEConfig`) persist
  in `ited.cfg`; the original kept them in IT.CFG's Pattern segment.
- **Not ported (documented leftovers):** MIDI input triggers (no
  MIDI-in exists); the Ctrl-V default-volume display toggle.
- The POSIX terminal backend decodes the full modifier surface since
  feature 011 (ESC-prefix Alt, xterm modified CSI, `modifyOtherKeys`
  level 1 for Ctrl-digits) plus SGR mouse (button-event tracking; px/py
  approximate to the cell centre, so terminal thumbbar drags move in
  8-pixel steps). Terminal limitations: no Shift press/release events
  (F2 chord entry is pixel-backend-only; Shift-arrow marking works),
  and keypad `/` is indistinguishable from `/` (use `?` or Alt-F9 for
  muting). The Windows console (`ITED_TERM=1` on Windows) decodes the
  conio scan-code combos but has no mouse — the Win32 window is the
  primary backend there.

Sample/instrument library (F3/F4 Enter):
- Ripping single samples out of IT/S3M/XM/MOD/MTM/669/FAR/PTM/KRZ/PAT
  files and instruments out of IT/XM/.ITI/.XI follows the original
  `Load*SamplesInModule` / instrument loaders with their quirks kept
  (FAR ignores the file's per-sample volume and reads a hardcoded
  256-entry pattern-size table; PAT takes the loop-end field as the
  sample length; the out-of-slots check uses the free-slot count from
  when the requester was opened; instrument import forces each
  transferred sample's default pan off). ULT ripping is commented out
  in the 2.17 source and is likewise absent here. AIFF/TXWave
  standalone samples are not ported. Deviation from the original: an
  occupied slot asks "Replace sample/instrument N?" before it is
  overwritten (IT overwrites silently); reads are bounds-checked
  rather than trusting DOS scratch buffers.
- Standalone `.WAV` samples load per the original `D_GetSampleInfo8`
  identification (only integer PCM with 8/16 bits qualifies, the
  leading `RIFF` magic is not checked, the `data` chunk is found by
  the original's bounded 3-chunk walk with its 16-bit skip
  arithmetic, length capped at 4,177,910 bytes, C5 speed takes the
  sample rate's low 16 bits only). Deviation: a stereo WAV loads the
  **left channel silently** — the original pops a "Loading Stereo
  Sample" Left/Right prompt; the prompt (and the Right choice) is not
  ported.
- Note keys preview the selected library entry through IT's check
  slot (sample 100) via the ported `Music_PlaySample`; there is no
  key-release note-off (the port's key layer has no release events).
- F3 Alt-O/T/W save the current sample as .ITS / Scream Tracker /
  WAV under its DOS filename, F4 Alt-O saves the instrument as .ITI —
  as in IT, including the WAV header's RIFF size staying 0.

Sample editor (F3):
- IT 2.17 has no freehand waveform drawing, no selection
  cut/copy/paste and no zoom — the authentic surface is the waveform
  view plus the Alt-key operations, all ported. Alt-Y (calculate C5
  speed) is a stub in the 2.17 source and stays one here.
- Loop/speed numeric fields edit through a value prompt instead of the
  original's inline digit entry.
- The Alt modifier works on all backends (pixel backends natively;
  the terminal via ESC-prefix decoding since feature 011).

Save (F10) / message editor:
- The `.IT` writer and the IT 2.14/2.15 sample compressor are ported
  1:1 (SaveFormat default 3 = IT215, as `SWITCH.INC`); S3M export
  (SaveFormat 1) is not ported. Edit-history/timer blocks (Special
  bit 1 in ITTECH terms) are not written — the port keeps no timer
  data, which is also the original's behaviour when none exists.
- Message editor: Alt-C (clear message) has no Alt in the key layer —
  Ctrl-L is the stand-in. Ctrl-T toggles only the text colour (12/6);
  the original also swaps in its hi-ASCII character set
  (`S_DefineHIASCII`), which is not ported yet.

## What about the editor?

This port deliberately preserves everything the editor builds on: the
host/slave channel model, `Music_PlayNote`/freeplay mode (PlayMode 0,
which exists only for the editor), pattern/sample/instrument memory
layouts, and the engine entry points the UI calls. See
`docs/EDITOR-PORT-PLAN.md` for the staged plan and the text-mode screen
groundwork in `src/it_screen.*`.

## License / credits

Impulse Tracker was written by Jeffrey Lim (Pulse), source released
under the BSD-3 license (see `LICENSE.TXT` in the original repository).
This port keeps the original structure and names so it can be audited
against the assembly side by side.
