# Changelog

## Unreleased

**Stereo samples.** Stereo samples load, play, edit and save in stereo
instead of being cut to one channel (README → "Stereo samples").

### New

- Loading a stereo WAV offers **Stereo** (default), Left and Right.
- Stereo samples in IT modules, ITS and ITI files (as OpenMPT and Schism
  Tracker write them) load and play in stereo; pan works as balance.
- F3: two-lane waveform, "Stereo" in the info box, every operation on both
  channels, **Alt-U** converts stereo ↔ mono (mix, left or right).
- IT / ITS / ITI saves keep stereo samples (uncompressed, as OpenMPT);
  WAV exports are 2-channel; S3M export mixes them down with a warning.
- Pattern renders of a stereo song (Ctrl-O & co.) become stereo samples;
  a render into a sample now stops after 10 minutes (was 1 MB).
- **Longer samples:** up to 268,435,456 frames per channel, the limit of
  OpenMPT and Schism Tracker (IT: about 4 MB).
- **More WAV files:** 24- and 32-bit PCM, 32/64-bit float and
  WAVE_FORMAT_EXTENSIBLE load (as 16-bit), and the data chunk is found
  behind any metadata chunks -- WAVs exported from Reaper and other DAWs
  load instead of turning into noise.

- **Load Sample screen:** the waveform appears as soon as the cursor
  rests on a sample (no note key needed), in two lanes for stereo.

### Fixed

- The Load Sample waveform of 16-bit samples was striped: the preview
  read every other low byte as a high byte (IT's D_DrawWaveForm keeps
  one running pointer; the port restarted it per column).

### Changed

- Modules that already contain stereo samples now play them in stereo
  (IT 2.14 plays only the left channel), e.g. `testdata/beyond_network.it`.

## v0.9.0 — 2026-10-18

**Rendering and exporting audio.** Bounce a pattern into a new sample with
one key, write patterns, single channels or the whole song as WAV files for
your DAW, and export a song's samples. The idea and the key layout come
from **Esa Juhani Ruoho (@esaruoho)**, who added this set to the original
DOS source in his fork
[esaruoho/impulse-tracker](https://github.com/esaruoho/impulse-tracker)
(IT 2.354). Thanks, Esa! README → "Rendering and exporting audio" has the
whole guide.

### New

| Key | Where | What |
|---|---|---|
| Ctrl-O | F2 | the pattern → a new sample (looped, "Pattern 005") |
| Shift-Ctrl-O | F2 | the pattern → a WAV in the Quicksave folder |
| Ctrl-O, Shift-G, Right on an order's last digit | F11 | the playing pattern → a new sample |
| Shift-Ctrl-O, Ctrl-G, Shift-Right on the last digit | F11 | the playing pattern → a WAV |
| Shift-Right, Ctrl-O | F5 | the playing pattern → a WAV |
| D, Ctrl-Shift-Right | F5 (Ctrl-Shift-Right also F3, F11) | every sample → its own WAV |
| Ctrl-Alt-Right | F3 | the selected sample → a WAV |
| Shift-Alt-M | F2 | one WAV per channel of the pattern |
| WAV / MWAV buttons | F10 | the whole song → one WAV / one WAV per channel |
| Alt-W / Shift-Alt-W | screens without their own Alt-W / everywhere | save the module into the Quicksave folder / make this folder the Quicksave folder |

- **Quicksave folder:** a new row in F12 (Enter or Ctrl-O picks a folder).
  Renders and sample exports go there.
- **Fast and seamless:** renders use the real engine, run 15–100× faster
  than real time, and a playing song carries on where it was.
- **WAV files for the DAW** are 32-bit float at the Shift-F5 rate and are
  not clipped. **Renders into a sample** are IT-compatible 16-bit,
  44.1 kHz mono samples, in the first slot free in both the sample and
  the instrument list (with an instrument in instrument mode).
- **Readable file names:** `Believe in your dreams - pattern 005 -
  2026-10-18 15.39.28.wav`; sample exports and stems get their own folder
  (`03 Kick hard.wav`, `ch01.wav`, …). Nothing is ever overwritten.
- **Command line:** `ited --render-patterns <module> [--pattern N]` and
  `ited --dump-samples <module>` write the files and exit, without a
  window or a sound device.
- **Esc** (or the render key again) stops a render; F1 lists every new
  key under "it26 additions".

### Notes

- Where it26 differs from Esa's build: long file names instead of DOS
  8.3 names, 32-bit float WAVs, mono imports (IT can't play stereo
  samples), F3's selected-sample export on Ctrl-Alt-Right (Ctrl-Right
  stays IT's "next order"), and F5's Ctrl-O renders, as his source says
  it should.
- F10 has two more format buttons and F12 a Quicksave row (the
  directory box is one row taller, "Save all Preferences" one row lower),
  laid out as in Esa's fork.

**Full commit list:** https://github.com/eliasweingaertner/it26/compare/v0.8.0...v0.9.0

## v0.8.0 — 2026-10-13

**Classic IT, classic interface — with a twist.** it26 is no longer
billed as a strict 1:1 port: the engine and the classic interface stay
true to IT, and this release adds the first real extensions on top, on
keys and mouse buttons IT 2.14 leaves unused. Thanks to **Esa Juhani
Ruoho (@esaruoho)**, whose idea of a headless side-by-side comparison
with the real IT 2.14 (#24) found most of the fixes below.

### New

- **Right-click block menu** in the pattern editor: Copy, Paste, Clear,
  Transpose up/down, Transpose by… (any number of semitones),
  Interpolate notes, Interpolate volume, Interpolate effect. Items with
  an IT key do exactly what the key does and show it.
- **Interpolate notes in a scale** (Ctrl-K, or the menu): fills the
  marked block from its first to its last note with a run in a scale —
  chromatic, major, natural/harmonic/melodic minor, dorian, mixolydian,
  pentatonic major/minor, blues. A note lands on every edit-step row
  (Alt-0…9) and the last row; the scale's notes are spread over them.
  The key is suggested from the song: the block's instrument on the
  unmuted channels, so muting the drum tracks keeps note-triggered drums
  out of it.
- **Mark a block with the mouse**: drag in the pattern editor.
- **Hot-swap samples while the song plays**: loading a sample into a
  slot that already has one keeps the song running; notes still on the
  old sample stop, every later note plays the new one, and the
  instruments using the slot keep using it. "Create host instrument?" is
  only asked for an empty slot.
- **Long folder names** on F9/F10 show their first 12 characters; the
  focused one scrolls through by itself.
- **New Song (Ctrl-N)** has an extra row, Variables Keep / Clear.
- The **F1 help** lists the pattern editor's additions.

### Closer to IT 2.14

Three comparison rounds against the real IT 2.14 running in QEMU +
FreeDOS — screens, dialogs and, new, the mouse on every screen:

- **Screens and dialogs:** the Load Module screen as IT draws it (#30
  #31 #32 #40), F11's second page with channel volumes (#43), the
  Palette Configuration screen on Ctrl-F12 (#44), IT's dialogs for
  Exchange / Swap / Replace / Copy / Resize (#45), "No block is marked"
  and the other block messages as dialogs (#47), the Undo dialog (#48),
  Ctrl-N opens the New Song dialog (#42).
- **Layout and colours:** centred section titles (#33), the edit-mask
  markers under the channels (#34), field colours (#35 #37 #39), the F11
  order list column and `---` for a new song (#38), the F3 header on
  instrument-mode songs (#36), confirm dialog texts (#46), menus (#41).
- **Keys:** F2 after F9 returns to the pattern editor (#29); Alt-N
  without a message (#49); F12 (#50) and F9/F10 move the focus as IT
  links it — Tab, Shift-Tab, the arrows, the format buttons, the
  editable Directory line; Esc on F9/F10 opens the main menu; the main
  menu remembers its position.
- **Mouse:** buttons react only inside their border, the F4 envelope
  editor inserts / picks up / drags nodes as IT, a click on a muted
  pan bar unmutes it, menu clicks.
- **Sample fields:** Speed and the loop numbers on F3 and the Load
  Sample screen are edited in place, digit by digit, as in IT (the
  separate number box is gone); Enter there opens Load Sample.
- **Undo:** a "Pattern n" entry when a pattern is entered, and restoring
  another pattern's entry applies it.
- F12's Initial Tempo/Speed take effect at the next play; "Initialise
  instruments?" is OK / No over the unchanged screen; F4's Default Pan
  shows On while the pan is used.

### Fixes

- **Loop on a sample without loop points** (Space on Loop) did nothing;
  it now sets the loop end to the sample length, as IT.
- Long folder names ran over the F9/F10 drive list.
- New Song: clearing the patterns resets the pattern count (#42).

### Notes

- **Comparison harness** (`tools/compare`, #24): runs IT 2.14 headless
  in QEMU + FreeDOS next to it26 and compares every screen cell by cell;
  it now drives the mouse on both sides too. Reports in `docs/compare/`.
- The README describes where it26 differs: "Classic IT, with a twist".

**Full commit list:** https://github.com/eliasweingaertner/it26/compare/v0.7.0...v0.8.0

## v0.7.0 — 2026-10-05

**The project is now called it26** (GitHub: `eliasweingaertner/it26`):
the 2026 port of IT, and "ittrack" doubled the "tracker" in "IT". The
programs keep their names, `ited` and `itplay`; the downloads are now
named `it26-…`. Thanks again to **Esa Juhani Ruoho (@esaruoho)**, who
suggested the file dialogs and reported the Load Instrument screen.

### New

- **System file dialogs** (#26): next to IT's own file screens, the
  operating system's dialogs as shortcuts, on keys IT 2.14 doesn't use.

  | | Windows / Linux | Mac |
  |---|---|---|
  | Open a module (any screen) | Ctrl-Shift-F9 | Cmd-F9 |
  | Save module as, IT or S3M (any screen) | Ctrl-Shift-F10 | Cmd-F10 |
  | Load a sample / instrument into the current slot (F3 / F4) | Ctrl-O | Cmd-O |
  | Choose the folder for a path field (F12) | Ctrl-O | Cmd-O |

  Results are the same as with IT's screens, and the folder you used
  becomes the current one there too. Windows and macOS use their own
  dialogs; Linux uses `zenity` or `kdialog` if installed. File names with
  characters the tracker can't show still load and save (shown as `?`).
- **F1 help** lists these keys at the bottom of the pages, marked
  "it26 additions". On the Mac it shows the Cmd keys, and the note
  preview line says Right Option (#20).

### Closer to IT 2.14

- **Load Instrument screen** (#27): F4 → Enter (and Ctrl-F4, the
  Instrument Library) now show IT's own screen instead of the module
  file requester: numbered list with instrument name, file name, number
  of samples and size, "Available Samples", and the drive list. Only
  folders, instrument files (.ITI, .XI) and modules with instruments
  (.IT, .XM) are listed; a module opens in place to pick an instrument
  from. The folder is the F12 instrument directory, as in IT. Ctrl-F4
  browses only, as in 2.14.
- **Ctrl-Q works on the file screens** (#27): Load Sample, Load
  Instrument, the libraries and the F9/F10 screens ask "Exit Impulse
  Tracker?" right there; Cancel stays on the screen.
- **The help's Ctrl keys work** (#28): **Ctrl-E** redraws the screen,
  **Ctrl-I** reinitialises the sound driver (reopens the audio device,
  playback continues; handy after unplugging an interface), **Ctrl-M**
  shows/hides the mouse pointer. Ctrl-D (DOS Shell) has nothing to shell
  to, so the help no longer lists it.

### Fixes

- **Linux: Alt hotkeys no longer enter notes.** On X11 the system also
  reports the plain letter as text when Alt is held, so Alt-Q
  (transpose), Alt-B/E (mark block) and friends entered a note as well.
  Both Alt keys work as Alt, as in DOS IT; where the right one is AltGr
  (German and other layouts), AltGr+key still types its character
  (@, € …). The Mac had the same leak for the few Option characters the
  tracker can show (å, for example).

### Notes

- **Windows:** the installer now installs to an "it26" folder with its
  own uninstall entry; remove an older "ittrack" install yourself if you
  like. **Mac:** the app's bundle id changed (`org.it26.ited`); settings
  live in `ited.cfg` and are kept.
- The spec-driven development setup (Spec Kit: specs, plans, templates)
  is now part of the repository.

**Full commit list:** https://github.com/eliasweingaertner/it26/compare/v0.6.0...v0.7.0

## v0.6.0 — 2026-09-30

Thanks again to **Esa Juhani Ruoho (@esaruoho)** for the reports behind
most of these.

### macOS

- **Held keys repeat** (#15). Holding a note key in the pattern editor
  now keeps entering notes, advancing by the cursor step, as in IT. With
  step 4, holding Q fills every 4th row. macOS had opened its accent
  menu instead of repeating: Homebrew's SDL2 runs on SDL3, and SDL3 turns
  "press and hold" on by default. it26 now switches it off for itself.
  Your macOS settings are not changed.
- **Right Option = note preview** (#20). IT plays notes while Caps Lock
  is held. macOS treats Caps Lock as a toggle and shows its own
  indicator, so in the pattern editor holding **Right Option** now does
  the same job. Left Option stays Alt.

### Everywhere

- **F3 sample list as in IT 2.14** (#12): the numbers sit left of the list,
  followed by a divider and the **Play** column (bright for slots with a
  sample, dark for empty ones). The **play dots** next to the numbers
  show which samples are playing (large dot when just triggered) and
  which have played since playback started. F4 shows the same dots for
  instruments. The list keeps its scroll position like the original,
  instead of re-centring.
- **Shift-F5: Miniaudio Driver screen** (a deliberate extension): output
  device, the sample rates that device supports (up to 192 kHz), buffer
  size, 16-bit dithered / 24-bit / 32-bit float output, and on Windows
  exclusive device access, which opens the device at the chosen rate
  (for example 96 or 192 kHz) without resampling. For the lo-fi end:
  8-bit output (truncated like an 8-bit Sound Blaster, or dithered), true
  mono output, and 8 / 11.025 / 16 / 22.05 kHz mixing. On macOS, which has
  no exclusive mode, "Device Rate: Switch" sets the output device to the
  chosen rate (as Audio MIDI Setup would) instead of resampling. It also has
  the Sound Blaster 16 driver's 50%/75% filter and feedback (echo) modes,
  and "ramp volume at start of sample". Defaults keep playback
  bit-identical to before (#16).
- **Message editor:** Alt-C clears the message, as in IT (the old
  stand-in key had stopped working after the hotkey audit).

## v0.5.0 — 2026-09-28

A big fidelity release. Most of it came from side-by-side testing against
the original Impulse Tracker 2.14 by **Esa Juhani Ruoho (@esaruoho)**, who
reported the issues below and contributed PR #3, PR #11 and PR #18. Thank you!

### Behaviour changes you will notice

These keys now work the way they do in IT 2.14:

- **`{` `}`** change the **playback speed** and **`[` `]`** the **global
  volume**, on every screen. The port had put octave and cursor step on
  these keys. In IT, the octave is **keypad `/` `*`** and the cursor
  step is **Alt-0…9**.
- **Keypad `/`** lowers the octave. It no longer mutes the next channel;
  main-row `/` still does, as in IT.
- **Alt-Enter** in the pattern editor stores the pattern (IT's "store
  pattern"). Everywhere else it still toggles fullscreen. On Windows,
  **maximizing the window** also switches to fullscreen.
- **F7** plays from the Ctrl-F7 play mark, or from the cursor row if no
  mark is set. It used to behave like F6.
- **Alt-F4** toggles channel 4 on Windows, as in IT, instead of closing
  the window.
- Note previews on F3/F4 play on channel 1 by default, as in IT (the port
  used channel 41). `<` `>` `,` `.` pick the channel.

### New

- **Load Sample screen** as in the original (F3 → Enter), with the Sample
  Library. Also Ctrl-F3 / Ctrl-F4 for the sample and instrument libraries
  (#2).
- **Pattern Editor Options** — press F2 while already in the pattern
  editor: base octave, cursor step, row highlights, pattern length,
  command/value columns (#4).
- **Context-sensitive F1 help**, taken straight from IT's own help data
  (every page, in the original layout). Done/Esc return to the screen you
  came from (#21, #22, #25).
- **"Enter Value" box** — typing a number on any slider opens it, as in
  IT (#9).
- **Quit confirmation** ("Exit Impulse Tracker?"), and all confirmation
  boxes now use the original raised OK/Cancel buttons; O/C/Y/N work (#5).
- **Sample Amplification box** (F3 Alt-M) and **Volume Amplification box**
  (Ctrl-J, fast volume mode) with sliders, as in IT (#23).
- **Multichannel Selection** dialog (press Alt-N twice in the pattern
  editor).
- **`build_mac.sh`**: a one-step CMake build and launch for macOS
  (PR #18 by @esaruoho).
- **Any file can be loaded as a sample**, like in IT: unrecognised files
  load as raw 8-bit data (#17).
- **Missing hotkeys** — every binding in IT's key tables was checked
  against the port, and the missing ones were added:
  - **Global:** Alt-F1…F8 (toggle channels 1–8), Shift-F6 (play from the
    current order), Ctrl-F5 (play song), Alt-F11 (lock the order list),
    Ctrl-N, Ctrl-P, Ctrl-L/R (load), Ctrl-W (save), and Ctrl-Left/Right
    (previous/next order while playing).
  - **Pattern editor:**
    - playback and navigation: Ctrl-F6 (play from the current row),
      Ctrl-Up/Down (instrument), Alt-Left/Right (channel), Ctrl-PgUp/PgDn
      (top/bottom), Alt-Home/End, Shift + grey +/− (four patterns);
    - editing: Alt-Backspace (revert pattern), Alt-K (slide volumes;
      press twice to wipe);
    - display and preview: Ctrl-V (show default volumes), Caps Lock +
      note (preview without entering);
    - track views: Ctrl-Left/Right move through the track-view channels.
  - **Info page (F5):** Alt-Up/Down, Alt-R, Alt-S, Alt-F9/F10. Q/S/G/V/I
    work in either case.
  - **F3/F4:** Alt-N (multichannel preview), `` ` `` (solo the sample or
    instrument), Ctrl-PgUp/PgDn.
  - **Sliders:** Shift+arrows move by 4 and Ctrl+arrows by 2, everywhere
    including the F11 pan bars and dialogs (#7).

### Fixed

- **Mouse on macOS/Linux:** clicks landed up and to the left of the
  pointer, so buttons and sliders seemed dead (#8).
- **Mouse in dialogs:** every dialog and confirmation box now responds to
  clicks, and sliders can be dragged, including the F11 pan bars (#8).
- **F11:**
  - the panning columns are real sliders (#10);
  - the order field width is fixed (PR #11 by @esaruoho);
  - the cursor returns to the row where you pressed G (#14).
- **F12:**
  - Initial Tempo/Speed are drawn without the stray box (#6);
  - cursor-down reaches every field again (#19, waiting for macOS
    confirmation).
- **F-keys inside Load Sample, Load/Save Module and the libraries** switch
  screens, and the playback keys work there (#13).
- **F3:**
  - sample box and empty slots as in the original (#2);
  - Alt-Ins with template slots fixed (from PR #3 by @esaruoho);
  - Alt-W, Alt-T, Alt-M and the other Alt keys on the piano rows work
    again (#23).
- **F4:** the instrument operations follow the original, and the extra
  "Replace sample?" prompt is gone.
- **F5:** the title reads "Info Page (F5)" (from PR #3).
- **Pattern editor:**
  - Alt-F/Alt-G double and halve the block as IT does, reading and
    writing past the block;
  - the "Enter Value" box is drawn on top of its dialog.
- **Scalable sliders** (F3 vibrato, F4 envelopes/filters, F12) have their
  original width.
- **Windows:**
  - no system beep on Alt shortcuts;
  - Alt shortcuts are no longer taken away by other programs' global
    hotkeys (for example the NVIDIA overlay's Alt-F1…F3) while it26 is
    in front.

### Known issues

Still open: #12 (F3 sample list layout), #15 (macOS: held note keys do not
repeat), #16 (Shift-F5 driver screen), #19 (macOS confirmation pending),
#20 (macOS: Caps Lock is passed to the system).

**Full commit list:** https://github.com/eliasweingaertner/it26/compare/v0.4.0...v0.5.0
