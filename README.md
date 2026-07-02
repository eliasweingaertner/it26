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
terminal to at least 80x50).

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
| F3 / F4 | Sample list / Instrument list (piano keys audition) |
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

This is a focused editor — the in-depth IT sample/instrument *editors*
(envelopes, waveform ops), the info page's oscilloscope/other view
modes, message editor, and saving are future work (see `docs/HANDOFF.md`
§6 and `docs/EDITOR-PORT-PLAN.md`).

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
