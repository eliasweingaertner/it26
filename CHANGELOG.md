# Changelog

## Unreleased

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
    hotkeys (for example the NVIDIA overlay's Alt-F1…F3) while ittrack is
    in front.

### Known issues

Still open: #12 (F3 sample list layout), #15 (macOS: held note keys do not
repeat), #16 (Shift-F5 driver screen), #19 (macOS confirmation pending),
#20 (macOS: Caps Lock is passed to the system).

**Full commit list:** https://github.com/eliasweingaertner/ittrack/compare/v0.4.0...v0.5.0
