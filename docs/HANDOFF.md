# itplay — Session handoff / resume guide

> Read this first when resuming in a fresh agent session. It is a snapshot
> of **where the project stands**, **how to build and verify it**, **how the
> code is organised**, and **what to do next**. For the staged editor plan
> see `EDITOR-PORT-PLAN.md`; for the user-facing overview see `../README.md`.

Last updated: 2026-06-11.

---

## 1. What this project is

A native, cross-platform C port of **Impulse Tracker 2.17** (Jeffrey Lim's
DOS tracker), ported **1:1 from the original x86 assembly** — not a generic
mod player wired to `.IT` files. Two hard rules set by the project owner:

1. **The playback engine must stay 1:1 with the original ASM.** Keep the
   original label/function names, the goto-mirrored control flow, and the
   data tables byte-exact (*including the original tables' typos* — they are
   part of the sound). Struct layouts are pinned with `_Static_assert`
   against the original `InternalDocumentation/CHANNEL.TXT`.
2. **Do not swap in a different engine.** Everything is built on the ported
   engine's real data structures.

The **editor** is a faithful UI/behaviour port (not literal ASM
transliteration) layered on top of the real engine.

### Source provenance
- Original ASM, two clones (verified byte-identical in every engine file
  this port touches):
  - `C:\Users\elias\fable5\impulsetracker` — herrnst mirror
  - `C:\Users\elias\fable5\impulse-tracker-jthlim` — Jeffrey Lim's own repo,
    canonical upstream. Diff vs mirror: only removed the dormant
    `WAREZWAVE` block from `WAVDRV.ASM` (never ported) + two `.BAT` fixes.
- The port lives in `C:\Users\elias\fable5\itplay`.

---

## 2. Current status — what works

**Engine (DONE, 1:1, owner-verified with his own modules):**
- Sequencer core, all A–Z effects, volume-column effects, S-commands, NNA
  channel allocator, envelopes, vibrato, FPU pitch-slide math.
- High-quality WAV/hiqual software driver (= `ITWAV.DRV`): 256 channels,
  cubic-spline interpolation, the IT resonant filter (`F0 F0` MIDI
  intercept), volume ramping, click removal, error-feedback dither.
- `.IT` loader incl. 2.14/2.15 sample decompression, MIDI macros.
- Real-time playback via vendored **miniaudio**; offline WAV render is
  bit-deterministic (used as a regression harness).

**Editor (`ited`) — CORE DONE:**
- 80x50 text framebuffer, VT/ANSI **24-bit truecolor** backend with damage
  tracking; cross-platform non-blocking keyboard (Win conio / POSIX
  termios).
- Screens: pattern editor (F2), sample list (F3), instrument list (F4),
  order list (F11), song variables (F12), help (F1). IT piano note entry,
  octave/edit-step, row insert/delete, pattern navigation, live play/stop,
  one-line module loader (F9).
- Pattern grid kept unpacked in memory; pack/unpack is the **exact inverse**
  of the player's decoder (mask-based cell model), mutex-serialised against
  the audio thread via `Engine_Lock`/`Unlock` hooks.
- Themed to match IT: real default **"Camouflage" palette** (`IT_S.ASM`
  `PaletteDefs`), bevelled tan panels, boxed transport/status header,
  beat/measure row highlighting (every 4th/16th row), IT pattern colours.

**Not done yet** (this is the next phase — see §6): the full IT
sample/instrument *editors*, file browser, player/main-menu screen, mouse,
and a pixel-accurate VGA-font backend. The editor has **not been
interactively tested in a real terminal** — only via the non-interactive
self-test and ASCII screen dumps. Needs a >=80x50 truecolor terminal.

---

## 3. Build & run

**It is not (yet) a git repo on disk** — see §5; an initial commit is being
created in this session.

### CMake (preferred, all platforms)
```
cd C:\Users\elias\fable5\itplay
cmake -B build && cmake --build build --config Release
```
Produces `itplay` (player) and `ited` (editor).

### MSVC directly (Windows) — the command used this session
```
cmd /c "call \"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" >nul 2>&1 && cl /nologo /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fe:ited.exe src\it_music.c src\it_effects.c src\it_tables.c src\it_driver.c src\it_load.c src\it_pattern.c src\it_screen.c src\it_editor.c"
```
`/std:c11` is **required** (for `_Static_assert`); `/D_CRT_SECURE_NO_WARNINGS`
silences CRT warnings. Swap the trailing sources + `/Fe` for `itplay.exe`
(`src\main.c` instead of `it_screen.c`+`it_editor.c`) or `test_pattern.exe`
(`tests\test_pattern.c`).

### gcc/clang (Linux/macOS)
```
cc -std=c11 -O2 -o itplay src/*.c -lm -lpthread -ldl      # NB: excludes editor; pick sources as in CMake
```
POSIX links `Threads`, `m`, `dl`.

### Run
```
itplay <module.it> [-r hz] [-w out.wav] [-o order] [-q]   # player / WAV render
ited   [module.it] [-r hz]                                # editor (needs 80x50 truecolor term)
```

---

## 4. How to verify changes (regression harness)

The audio-determinism test is the safety net. **Run it after any engine or
pattern-format change** — it must stay byte-identical.

```
cmake --build build --config Release --target test_pattern   # or compile tests/test_pattern.c per §3
./test_pattern <module.it>     # run for each of the 4 testdata modules
```
It checks: (a) unpack→pack idempotency, (b) **byte-identical audio** after
round-tripping every pattern through unpack→pack, (c) a double-render
determinism control. Known-good FNV-1a hashes (all four `IDENTICAL`):
- `beyond_network`  `a43e6f19...`
- `itdemo`          `1e6a5383...`
- `quests_end`      `bb5b15bc...`
- `synthscape`      `d561c207...`

Non-interactive editor checks (no terminal/audio needed):
- `ITED_SELFTEST=1 ited <mod>` — runs a scripted smoke test of editor actions.
- `ITED_DUMP=<screen#> ited <mod>` — writes `screen_dump.txt` (current screen
  as plain ASCII, box glyphs approximated). Use this to inspect layout while
  iterating on the UI without a terminal.

Test modules live in `testdata/` (`beyond_network.it`, `itdemo.it`,
`quests_end.it`, `synthscape_filters.it`).

---

## 5. File map

```
itplay/
  CMakeLists.txt           two targets: itplay (engine+main), ited (engine+screen+editor)
  README.md                user-facing overview + fidelity notes
  docs/
    HANDOFF.md             <- this file
    EDITOR-PORT-PLAN.md    staged editor plan (stages 1-3 done, 4 = next phase)
  external/
    miniaudio.h            vendored single-header audio (do not edit)
  testdata/                4 .IT modules for regression
  tests/
    test_pattern.c         determinism/roundtrip regression
  src/
    --- ENGINE (1:1 ASM transliteration — keep faithful) ---
    it_music.c   (2373)    IT_MUSIC.ASM: sequencer, Update*, NNA, slides, Music_* control
    it_effects.c (1875)    IT_M_EFF.INC: all effect handlers A-Z, vol-column, S-commands
    it_tables.c  (216)     pitch/waveform/slide LUTs — verbatim incl. original typos
    it_driver.c  (1115)    WAVDRV+MIXWAV+WAV.MIX: hiqual driver; DoTick wraps Update()+mix
                           in Engine_Lock/Unlock; WAV_InitSound resets OutFilled/OutPos
    it_load.c    (523)     .IT loader (IT_DISK.ASM load path vs ITTECH.TXT)
    it_music.h   (218)     engine API; Music_ResetRNG added for deterministic tests
    it_structs.h (400)     host/slave channel + instrument/sample layouts, _Static_assert pinned
    --- EDITOR (faithful UI/behaviour port over the engine) ---
    it_screen.c  (298)     80x50 framebuffer, truecolor VT backend, Camouflage palette,
                           CP437->UTF8, Key_Get, Screen_DumpPlain
    it_screen.h  (56)      Screen_* + ITK_* key enum
    it_pattern.c (209)     unpacked grid <-> packed format; exact inverse of player decoder
    it_pattern.h (72)      editcell_t (explicit mask: CM_NOTE/INS/VOL/CMD), Pattern_* API,
                           Engine_Lock/Unlock hooks
    it_editor.c  (1050)    the TUI: panels, screens, key handling, ITED_SELFTEST/ITED_DUMP
    main.c       (218)     player CLI
```

### Key data-model facts (don't relearn the hard way)
- **Pattern cell mask is explicit, not value-sentinel.** `editcell_t.mask`
  carries `CM_NOTE|CM_INS|CM_VOL|CM_CMD` bits. Earlier we inferred presence
  from sentinel values (note==0, vol==255) and it broke round-trip audio
  because "absent" and "value 0/255" are different to the player. Keep the
  mask authoritative.
- **On-disk packed format == player runtime format.** The loader stores raw
  packed rows; the player decodes repeat bits (`0x10/0x20/0x40`) at runtime
  via persisted per-channel state + the `& 0x33` test in `PreInitCommand`.
  `it_pattern.c` unpack reproduces that state machine exactly.
- **Driver determinism:** `WAV_InitSound` must reset the static `OutFilled`/
  `OutPos` output FIFO, or re-renders in the same process leak stale samples
  (this was a real latent bug, now fixed).
- **Engine RNG:** `Music_ResetRNG()` seeds `Seed1=0x1234, Seed2=0x5678` for
  reproducible renders.

### Editor color/layout constants (`it_editor.c`)
`ATTR(fg,bg) = (fg&15)|((bg&15)<<4)`. Element colours: note=3 (light),
empty=14, instr=10 (teal), vol=6 (green), fx=5 (yellow), sep=1. Row tier
backgrounds: normal=0, beat(every 4)=15, measure(every 16)=14, current=1.
Layout: `PE_TOP=10, PE_HDR=9, PE_LEFT=5, PE_CHANW=14, PE_BOTTOM=48`.
`panel(x0,y0,x1,y1,interior,title)` draws the bevelled tan box
(face=color2, highlight=color3 top/left, shadow=color1 bottom/right).

---

## 6. Next phase — "make it the *real* IT UI"

The owner wants to push the editor toward the authentic Impulse Tracker
experience. Roadmap (rough priority order; expand `EDITOR-PORT-PLAN.md`
Stage 4 as these land):

1. **UI rework to match the true IT screens 1:1.** Current chrome is a
   reasonable approximation; the goal is pixel/layout-faithful screens.
   - Biggest fidelity gap: box glyphs use single-line Unicode, not IT's
     custom 8x8 VGA bevel font. A **pixel-accurate SDL backend** behind the
     same `Screen_*` interface, using the original font + bevel glyphs, is
     the real fix. The terminal backend stays as the no-deps fallback.
   - Match exact object placement against `IT_OBJ1.ASM` / `IT_PE.ASM` /
     `IT_DISPL.ASM` and `InternalDocumentation/OBJECT.TXT`.
2. **Player / main-menu UI** — the IT main screen, message editor, the
   F1-help and order/transport presentation as IT actually lays them out.
3. **File browser** (F9/F10) — port the `IT_F.ASM` file requester +
   `IT_DISK.ASM` load/save (IT/S3M/MOD import). Replaces the current
   one-line path prompt.
4. **Mouse** — port `IT_MOUSE.ASM` event model (needs SDL backend or
   terminal mouse reporting).
5. **In-depth sample & instrument editors** (`IT_I.ASM`) — envelopes,
   sample draw/loop/zoom ops, the full instrument page.
6. **Config persistence** — IT wrote config into the driver/EXE; replace
   with a config file.

### Working agreements when continuing
- Engine code stays 1:1; if you must touch it, re-run the §4 regression and
  confirm all four modules remain `IDENTICAL`.
- Editor code is a faithful UI/behaviour port — reference the ASM for layout
  and behaviour, but idiomatic C is fine.
- When adding an SDL backend, keep it **behind the existing `Screen_*`
  interface** so the terminal build keeps working with no SDL dependency.
- Use `ITED_DUMP` to iterate on layout without a live terminal.
```
