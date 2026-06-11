# Porting the Impulse Tracker editor — staged plan

Status: **Stages 1–3 (core) implemented and tested.** A working
cross-platform terminal editor (`ited`) is built alongside the player,
covering the pattern editor, sample/instrument lists, order list, song
variables, help, live playback and module loading. Remaining: the
in-depth sample/instrument *editors*, file browser, mouse, and a
pixel-accurate font backend (Stage 4 below).

The playback engine port was done first because everything else builds
on it, and it is the part that must be 1:1. The editor is the larger
half of the original code base (~1.5 MB of assembly):

| Original           | Size  | What it is |
|--------------------|-------|------------|
| `IT_OBJ1.ASM`      | 379KB | UI object system (lists, buttons, input fields, dialogs) |
| `IT_PE.ASM`        | 352KB | pattern editor |
| `IT_I.ASM`         | 254KB | instrument/sample editors and lists |
| `IT_DISPL.ASM`     | 128KB | screen primitives, info pages, oscilloscope/spectrum |
| `IT_F.ASM`         | 166KB | file requester / load-save screens |
| `IT_K.ASM`         |  49KB | keyboard handling |
| `IT_MOUSE.ASM`     |  41KB | mouse handling |
| `IT_DISK.ASM`      | 316KB | disk I/O, module load/save in all formats |

## What the engine port already guarantees the editor

- **Freeplay mode** (`PlayMode 0`) is ported — it exists *only* for the
  editor (keyboard jamming, command update counters per channel).
- **`Music_PlayNote`** (the 5-byte note-entry API used when you press a
  note key in the pattern editor) is ported.
- **Host/slave channel layouts** are byte-identical, so the channel
  info pages (`F5`/info screens) can be ported against the same data.
- Patterns are kept in the **packed in-memory format** the editor's
  pattern allocator (`Music_AllocatePattern`/`Music_GetPattern`) uses.
- Instruments/samples/song header keep the on-disk layout in memory,
  exactly like the DOS original's SongData segment.

## Implemented so far

- `src/it_screen.*` — 80x50 text framebuffer, VT/ANSI backend with
  damage tracking, cross-platform non-blocking keyboard (Win conio /
  POSIX termios).
- `src/it_pattern.*` — unpacked pattern grid + pack/unpack that is the
  exact inverse of the player's decoder, mutex-serialised against the
  audio thread. Verified by `tests/test_pattern.c` (idempotent +
  byte-identical audio after round-tripping every pattern).
- `src/it_editor.c` — the editor itself: pattern editor (F2), sample
  list (F3), instrument list (F4), order list (F11), song variables
  (F12), help (F1); IT piano note entry, octave/edit-step, row
  insert/delete, pattern navigation; live play/stop via the engine; a
  one-line module loader (F9); `ITED_SELFTEST` non-interactive smoke
  test.

## Stage 1 — platform layer (DONE, `src/it_screen.*`)

- 80x50 text-cell framebuffer with the IT palette; VT/ANSI terminal
  backend with damage tracking. An SDL backend with the original 8x8
  font can be added behind the same interface for pixel accuracy.
- Non-blocking keyboard polling with extended-key codes
  (`Key_Get` ≈ `K_GetKey`).
- TODO: key *chord* state (IT distinguishes press/release for jamming),
  mouse events, and the screen-saver/idle hooks.

## Stage 2 — `S_*` primitives + object system

Port `IT_DISPL.ASM`'s drawing primitives (`S_DrawString`, `S_DrawBox`
with the four bevel styles, `S_DrawSmallBox`, header/footer chrome) on
top of `it_screen`, then the `IT_OBJ1.ASM` object lists (each screen in
IT is a list of typed UI objects with a shared event loop). The object
records are well-specified in `InternalDocumentation/OBJECT.TXT`.

## Stage 3 — screens, in dependency order

1. Info page (F5) — read-only, exercises host/slave channel data.
2. Order list / song variables (F11/F12 screens).
3. Sample list (F3) then instrument list (F4) — list objects + the
   engine's `Music_PlaySample`/`Music_PlayNote` for auditioning.
4. Pattern editor (F2) — the largest screen; the engine side
   (`Music_AllocatePattern`, packed row encode/decode, `PE_*` helpers)
   is specified by the pattern format and `IT_PE.ASM`.
5. File requester (F9/F10) + IT/S3M/MOD import from `IT_DISK.ASM`.
6. Sample editor operations, message editor, MIDI config screens.

## Stage 4 — the *real* IT UI (next phase)

The core editor works but is a terminal approximation. This stage pushes
toward an authentic, layout-faithful Impulse Tracker. See `HANDOFF.md` §6
for the full roadmap. In rough priority order:

1. **Pixel-accurate UI.** Add an **SDL backend behind the existing
   `Screen_*` interface**, using IT's original 8x8 VGA font and bevel
   glyphs (the biggest remaining fidelity gap — the terminal backend uses
   single-line Unicode for box edges). Keep the terminal backend as the
   no-deps fallback. Match exact object placement against `IT_OBJ1.ASM` /
   `IT_PE.ASM` / `IT_DISPL.ASM` and `OBJECT.TXT`.
2. **Player / main-menu screen** — IT's main screen, message editor, the
   help and order/transport presentation as IT lays them out.
3. **File browser** — port `IT_F.ASM` requester + `IT_DISK.ASM` load/save
   (IT/S3M/MOD import); replaces the current one-line path prompt.
4. **Mouse** — port `IT_MOUSE.ASM` (needs SDL or terminal mouse reporting).
5. **In-depth sample/instrument editors** (`IT_I.ASM`).
6. **Config persistence** (the original wrote its config into the
   driver/EXE files — a config file replaces that).
