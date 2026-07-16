# ittrack — a native cross-platform port of Impulse Tracker

TL;DR A 1:1 conversion of Jeffrey Lim's fantastic Impulse Tracker to modern operating systems. Originally written in pure (!) 486 x86 assembly code, this is the first near-complete 1:1 translation. The conversion was carried out fully with Antrophic's Fable 5 and Opus 4.8 models.

## Background
About 30 years ago I was a quite a nerdy teenager. My parents had just bought a 486 DX computer with a terrible Sound Blaster Pro clone. A little while a modem, BBS access and AOL followed. Somewhere I first downloaded the legendary [Scream Tracker 3](https://en.wikipedia.org/wiki/Scream_Tracker) (S3M) by the also legendary Future Crew. A short time later I learned about [Impulse Tracker](https://en.wikipedia.org/wiki/Impulse_Tracker) (IT) by Jeffrey Lim. I started jamming with it and wrote quite some songs, which I gladly never released :-) Impulse Tracker is a fantastic piece of software, and arguably one of the best trackers ever written. It sounds fantastic. Its usability is outstanding. At its resource usage was already low back in the day. One of the reasons was that it was brilliantly designed and really heavily optimized. It is written in pure x86 assembly code. That is one of the reasons it never got directly ported to modern operating systems. Since then, there have been repeated efforts to bring the "feel" of IT to the more modern world. One great and famous example is [Schism Tracker](https://schismtracker.org/), which combines an Impulse-Tracker-aligned interface with the [Modplug playback](https://openmpt.org/legacy_software) engine.

### 30 years later - we have AI
30 years later, we find ourselves in the middle of the AI revolution. The nerdy teenager has become an engineering manager who also has obtained a PhD in computer science pver a decade ago. The nerd in me still enjoys jamming some times, mostly in [Reaper](https://www.reaper.fm/) and [Renoise](https://www.renoise.com/). But deep in my heart I am still obsessed with Impulse Tracker. And yes, I hate Suno! :P

Once Fable 5 became available I couldn't resist and put it to an extreme test: Can an AI model of this class create a working 1:1 port of Impulse Tracker for modern operating systems? Quite shockingly, the answer is yes. I threw Fable 5 (and in parts Opus 4.8) at it, using [spec-kit](https://github.com/github/spec-kit) to steer its progress. 

After burning a quite a bunch of tokens, I am happy to present you an AI port of Impulse Tracker!

## Now we let Fable explain the rest :P

A 1:1 C port of Jeffrey Lim's Impulse Tracker for Windows, Linux and
macOS: the complete 2.17 playback engine transliterated
instruction-for-instruction from the released x86 assembly source
(https://github.com/jthlim/impulse-tracker, the author's own
publication; the herrnst/impulsetracker mirror is byte-identical in
every file this port is based on), and on top of it a faithful
reproduction of the editor — the original screen layouts, font,
palette, key surface and behaviours, taken from the same source
rather than approximated. This is **not** a generic module player
wired to .IT files — the playing routines are the original ones:

| Port file        | Original source                | What it is |
|------------------|--------------------------------|------------|
| `src/it_music.c` | `IT_MUSIC.ASM`                 | sequencer core: `Update`, `UpdateData`, `UpdateNoteData`, `UpdateInstruments`, `UpdateSamples`, `UpdateEnvelope`, `UpdateVibrato`, NNA channel allocator, pitch slides (FPU paths), `Music_*` play control |
| `src/it_effects.c` | `IT_M_EFF.INC`               | all effect handlers A–Z, volume-column effects, S-commands, with the original dispatch tables |
| `src/it_tables.c` | `IT_MUSIC.ASM` data            | pitch table, waveform tables, slide LUTs — transcribed verbatim *including the original tables' typos* (they are part of the sound) |
| `src/it_driver.c` | `WAVDRV.ASM` + `MIXWAV.INC` + `WAV.MIX` | the high-quality software driver (= ITWAV.DRV): 256 channels, cubic spline interpolation, the IT resonant filter (`F0 F0` MIDI intercept, coefficients per `Q.INC`), volume ramping, click removal, error-feedback dither |
| `src/it_load.c`  | (re-implementation of the `IT_DISK.ASM` load path against `ITTECH.TXT`) | .IT loader; keeps patterns in the packed on-disk format because the engine decodes packed rows directly, IT 2.14/2.15 sample decompression, embedded/default MIDI macros |
| `src/it_structs.h` | `InternalDocumentation/CHANNEL.TXT` | host/slave channel layouts, byte-for-byte, enforced with `_Static_assert` |
| `src/it_save.c`  | `IT_DISK.ASM`                  | .IT writer with the IT 2.14/2.15 sample compressor, and the `D_SaveS3M` S3M exporter with its lossy-conversion quirks |
| `src/it_import.c` | IT's own importers             | whole-module S3M/XM/MOD/MTM/669 conversion, quirks included |
| `src/it_ris.c`   | `D_GetSampleInfo` + `Load*SamplesInModule` | standalone sample/instrument identification and the library rippers (WAV, IFF 8SVX/16SV, TX16W, .ITS/.ITI/.XI, and samples/instruments out of other modules) |
| `src/it_editor.c` | `IT_F.ASM`, `IT_PE.ASM`, `IT_OBJ1.ASM`, `IT_S.ASM`, `IT_FOUR.ASM` | the editor: IT's object model, every screen's layout data, the key layers, the Fourier spectrum analyser |
| `src/it_screen*.c`, `src/it_vgadata.c` | `IT_S.ASM` | 80x50 cell display layer with the byte-exact VGA font/palette/glyph data, behind three presentation backends (Win32, SDL2, VT terminal) |
| `src/it_pattern.c` | (port-side)                   | unpacked pattern grid for editing; re-packs to the on-disk format the engine plays, verified byte-identical |
| `src/main.c`     | (port-side)                    | `itplay` command-line player / WAV renderer |

The build configuration mirrors the 2.17 release (`SWITCH.INC`):
`USEFPUCODE=1` (FPU slide math; the 2.14 lookup-table variants are also
ported under `USEFPUCODE=0`), and the driver uses `WAVSWITC.INC` settings
(`VOLUMERAMP=1`, `CUBICINTERPOLATION=1`, `DITHEROUTPUT=1`, `RAMPSPEED=8`,
`RAMPCOMPENSATE=255`).

## Building

Any C11 compiler; there are no required external dependencies. Audio
output is miniaudio (vendored, single header) — WASAPI on Windows,
CoreAudio on macOS, ALSA/PulseAudio on Linux.

### CMake (all platforms)

    cmake -B build
    cmake --build build --config Release

builds three targets:

| Target | What it is |
|--------|------------|
| `itplay` | command-line player / WAV renderer |
| `ited`   | the editor |
| `test_pattern` | regression harness (pattern round-trip + save-path audio gate) |

On Windows run this from a **x64 Native Tools** (vcvars64) prompt —
the VS Build Tools ship `cmake` and `ninja` inside it even when
neither is on the general PATH.

The editor's authentic pixel window on Linux/macOS uses **SDL2**, an
optional dependency: install the dev package (`libsdl2-dev` /
`SDL2-devel` / `brew install sdl2`) and CMake picks it up
automatically. Without SDL2 — or with `-DITED_SDL=OFF` — `ited` still
builds and runs with the terminal backend, and `itplay` /
`test_pattern` never need SDL.

### Direct compiler invocations (no CMake)

MSVC, from a vcvars64 prompt (`^` continues the line in cmd; put it
on one line in PowerShell):

    cl /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:itplay.exe ^
       src\it_music.c src\it_effects.c src\it_tables.c src\it_driver.c ^
       src\it_load.c src\it_pattern.c src\it_save.c src\main.c

    cl /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:ited.exe ^
       src\it_music.c src\it_effects.c src\it_tables.c src\it_driver.c ^
       src\it_load.c src\it_pattern.c src\it_save.c src\it_import.c ^
       src\it_ris.c src\it_screen.c src\it_screen_win32.c ^
       src\it_vgadata.c src\it_editor.c user32.lib gdi32.lib

gcc/clang on Linux/macOS (terminal-backend editor; use CMake if you
want the SDL2 pixel window):

    cc -std=c11 -O2 -o itplay src/it_music.c src/it_effects.c \
       src/it_tables.c src/it_driver.c src/it_load.c src/it_pattern.c \
       src/it_save.c src/main.c -lm -lpthread -ldl

    cc -std=c11 -O2 -o ited src/it_music.c src/it_effects.c \
       src/it_tables.c src/it_driver.c src/it_load.c src/it_pattern.c \
       src/it_save.c src/it_import.c src/it_ris.c src/it_screen.c \
       src/it_vgadata.c src/it_editor.c -lm -lpthread -ldl

`test_pattern` is the engine sources plus `tests/test_pattern.c`.
Python is only needed to regenerate the checked-in test fixtures
(`tools/gen_import_tests.py`) or the VGA data tables
(`tools/gen_vgadata.py` — `src/it_vgadata.c` is generated, never
hand-edited); building requires neither.

### GNU Make (POSIX quick path)

On Linux, macOS or MSYS2/Git Bash a `Makefile` mirrors the CMake source
lists as a no-deps convenience path:

    make            # itplay + ited + test_pattern
    make test       # determinism regression (must stay IDENTICAL)
    make ITED_SDL=0 # ited without the SDL2 pixel backend
    make help       # targets, knobs, and which backends this host builds

SDL2 is auto-detected via `pkg-config`/`sdl2-config`. CMake stays the
canonical cross-platform build (and the only supported path on
MSVC/Windows).

### The VGA ROM font (not committed)

The editor's authentic look uses the IBM VGA ROM 8x8 CP437 font — the
font `int 10h AX=1112h` loads, a dump of IBM's VGA BIOS character ROM.
Those bytes are potentially copyrighted, so this repo does **not**
redistribute them: `tools/IBM_VGA_8x8.bin` is `.gitignore`d and fetched
on demand by `make font` from
[spacerace/romfont](https://github.com/spacerace/romfont/tree/master/font-bin),
pinned to a commit and verified by SHA-256 before use. An ordinary build
needs nothing extra — `src/it_vgadata.c` (which bakes the font into C
tables) is generated *and committed*; the font is only required to
regenerate it (`make vgadata`).

### Verifying a build

    ITED_SELFTEST=1 ITED_TERM=1 ited testdata/itdemo.it    # scripted editor smoke test
    test_pattern testdata/itdemo.it --roundtrip            # pack/unpack + save-path audio gate
    itplay -w out.wav testdata/itdemo.it                   # renders are bit-deterministic

The selftest prints 11 `OK` blocks (F5/IMPORT/F3/SAVE/LIB/PE/PE2/
TERM/S3M/UPD/FFT) and a summary line; `--roundtrip` re-renders the
module after saving it in every format and demands byte-identical
audio.

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
backend on Linux/macOS (built when SDL2 is present; see Building).
**Alt-Enter** toggles borderless fullscreen (letterboxed, aspect
preserved). Set
`ITED_TERM=1` to force the truecolor terminal backend instead, which is
also the automatic fallback when no display is available (resize your
terminal to at least 80x50). On POSIX the terminal backend carries the
full input surface: Alt/Ctrl/Shift key combos via the xterm encodings
(ESC prefix, modified CSI, `modifyOtherKeys`) and mouse via SGR
reporting — clicking, dragging thumbbars and pattern-grid clicks work
over SSH.

The pixel output carries a small "2026 AI PORT" corner-art badge in
the top-right corner marking the port; 10 seconds after startup — or
as soon as the mouse touches it — it slides out of view
(`ITED_NOBANNER=1` hides it entirely — see the fidelity notes).

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
| F5  | Play song + live info page (all 11 view methods: track view, global volumes, note dots, …) |
| F6 / F8 | Play current pattern / stop |
| F9 / F10 | Load module (file requester) / Save module (.IT — IT214/IT215 compressed — or S3M export) |
| Shift-F9 | Song message editor |
| F11 / F12 | Order list & panning / Song variables |
| Alt-F12 | Fourier spectrum analyser (scrolling spectrogram + bar spectrum over live playback) |
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
24-bit-truecolor VT terminal fallback. On the pixel backends Alt-Enter
toggles borderless fullscreen (aspect-preserving, letterboxed; a host
convenience, not an IT key).

The in-depth sample/instrument editors (waveform view, Alt-key ops,
envelopes), the F5 info page views, the message editor, saving (.IT
writer with the IT215 compressor), module import (S3M/XM/MOD/MTM/669)
and the sample/instrument library (rip from other modules) are all
ported; remaining items are listed in `docs/HANDOFF.md` §6 (macOS
verification pass, Ctrl-V default-volume display toggle).

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
- A "2026 AI PORT" corner-art badge (76x72, `art/corner.bmp`,
  embedded as `src/it_cornerart.c`) is blitted into the top-right
  corner of the pixel output (next to the header's copyright line)
  to distinguish the port from the original at a glance. It holds
  for 10 seconds after startup — or until the mouse touches it —
  then slides out diagonally towards the top-right and stays gone.
  It is painted at the rasterizer
  stage, so text cells, dump hashes and the terminal backend are
  untouched. `ITED_NOBANNER=1` hides it (e.g. for screenshot
  comparisons against the DOS original).

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
  are as in the original.
- Alt-F12 opens the Fourier spectrum analyser (`IT_FOUR.ASM`,
  SPECTRUMANALYSER build): the transliterated 2048-point FFT over the
  driver's output tap, the scrolling spectrogram + 64-line bar
  spectrum, both gradient palettes ('p' toggles), +/- order keys and
  F5/F6/F8 playback. Deviation: the original switches to a VESA mode
  (1280/1024/800); the port renders a 640x400 palettized overlay
  through the normal pixel presentation — the terminal backend shows
  no analyser.
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
  in the 2.17 source and is likewise absent here. IFF 8SVX/16SV
  ("AIFF Sample") and Yamaha TX16W ("TX Wave Sample", 12-bit packed)
  standalone samples load per `D_GetSampleInfo` including its quirks
  (the VHDR loop fields read from the original's offsets, the chunk
  walk uses low-word sizes with no pad skip, 16SV data loads
  little-endian). Deviation from the original: an
  occupied slot asks "Replace sample/instrument N?" before it is
  overwritten (IT overwrites silently); reads are bounds-checked
  rather than trusting DOS scratch buffers.
- Standalone `.WAV` samples load per the original `D_GetSampleInfo8`
  identification (only integer PCM with 8/16 bits qualifies, the
  leading `RIFF` magic is not checked, the `data` chunk is found by
  the original's bounded 3-chunk walk with its 16-bit skip
  arithmetic, length capped at 4,177,910 bytes, C5 speed takes the
  sample rate's low 16 bits only). A stereo WAV pops the original
  "Loading Stereo Sample" Left/Right requester (keys L/R); headless
  paths (selftest, captures) keep the silent-left default.
- Note keys preview the selected library entry through IT's check
  slot (sample 100) via the ported `Music_PlaySample`; there is no
  key-release note-off (the port's key layer has no release events).
  Instrument records have no note preview — neither does the
  original's Load Instrument screen (its key lists carry no note
  handling), so this is faithful, not a gap.
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
- Sample and instrument names edit directly on the F3/F4 lists
  (feature 013): F3 carries the original's name cursor
  (Left/Right/Home/End; typing inserts, Backspace/Delete edit; the
  right stop keeps keyjazz), F4 uses the original's Spacebar-toggled
  edit mode (ESC/Enter leave). F4 Alt-U runs the original's "update
  pattern data" (remap matching (note, sample) pairs to the selected
  instrument through its note-sample table).
- The Alt modifier works on all backends (pixel backends natively;
  the terminal via ESC-prefix decoding since feature 011).

Save (F10) / message editor:
- The `.IT` writer and the IT 2.14/2.15 sample compressor are ported
  1:1 (SaveFormat default 3 = IT215, as `SWITCH.INC`). The F10 screen
  carries the original's four format radio buttons (IT214/S3M/IT2xx/
  IT215); a typed name without a dot gets `.IT` or `.S3M` per format
  and Ctrl-S replaces the loaded name's extension likewise
  (`D_SaveModule`/`D_SaveSong`).
- S3M export (SaveFormat 1) is the `D_SaveS3M` transliteration
  including its lossy-conversion quirks: patterns always emit 64 rows
  (shorter padded, longer written in full), cells on channels 17+ or
  with notes outside C-1..B-8 are dropped, vol-column values above 64
  write 0xFF, S91 becomes XA4, Vxx/Xxx halve, Cxx re-encodes decimal,
  the note-range check fires even off a stale cached note (ASM quirk),
  and the >100-pattern header keeps the original's over-allocated
  parapointer space. Every unrepresentable feature flashes the
  original's exact warning text on its original row and the save then
  waits for a key, as IT does. Deviation note: a byte-diff against a
  DOS-written S3M has not been run (no DOS box in the loop); layout is
  asserted field-wise by the selftest plus an import round trip.
- The message editor loads the hi-ASCII charset on entry
  (S_DefineHiASCII, feature 013): font bank B holds the plain CP437
  ROM font, so colour-12 text (Ctrl-T toggles 12/6) displays real
  high-ASCII characters — colour-6 text shows the IT UI glyphs for
  128..201, as in DOS IT.
- Edit-history/timer blocks (Special
  bit 1 in ITTECH terms) are not written — the port keeps no timer
  data, which is also the original's behaviour when none exists.
- Message editor: Alt-C (clear message) is not wired in this screen's
  key handler — Ctrl-L is the stand-in.

## Project docs

The editor was built in staged, individually gated features on top of
the ported engine. `docs/EDITOR-PORT-PLAN.md` is the original staged
plan; `docs/HANDOFF.md` is the living project handbook — current
status, build/verification commands with the expected determinism
hashes, a file map, layout facts and the roadmap. The only open items
are a macOS verification pass and the Ctrl-V default-volume display
toggle.

## License / credits

Impulse Tracker was written by Jeffrey Lim (Pulse), source released
under the BSD-3 license (see `LICENSE.TXT` in the original repository).
This port keeps the original structure and names so it can be audited
against the assembly side by side.
