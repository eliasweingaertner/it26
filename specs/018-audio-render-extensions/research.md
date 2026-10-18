# Research: Audio Rendering Extensions

Sources: Esa Ruoho's fork at `esaruoho/impulse-tracker` main (2026-09-25,
v2.354): `IT_MUSIC.ASM`, `IT_PE.ASM`, `IT_DISPL.ASM`, `IT_DISK.ASM`,
`IT_I.ASM`, `IT_G.ASM`, `IT_OBJ1.ASM`, his `features/*.feature` cards and
README; it26 `src/`. Line numbers refer to his tree at that commit.

## R1 — What a pattern render plays

- **Decision**: one pass of the pattern, rows 0..n−1, n = the pattern's row count (64 when the header says 0, at most 200), stopped by the engine's own end-of-play-section; no release tail.
- **Rationale**: `Music_ToggleWAVRender` sets `StopEndOfPlaySection = 1`, reads the row count from the pattern header (`WAV_HaveRows` / `WAV_RowsOK`, IT_MUSIC.ASM ~6555) and calls `Music_PlayPattern(AX, BX, 0)`. it26's `it_music.c` already has `StopEndOfPlaySection` (2258: pattern end → `Music_Stop`; 2340: song end → `Music_Stop`).
- **Alternatives**: render N extra ticks for release tails — not Esa's behaviour.

## R2 — Format

- **Decision** (user, 2026-10-18): two formats.
  - **Import renders** (into a sample slot): 44100 Hz mix, stored 16-bit, stereo when the song's stereo flag is set — Esa's format and IT-compatible (IT samples are 8- or 16-bit only; C5 speed 44100).
  - **WAV-file renders** (Quicksave, stems, F10 WAV/MWAV, batch): the Shift-F5 mixing rate, written as 32-bit IEEE float (WAVE_FORMAT_IEEE_FLOAT, fmt tag 3) **without clipping**: the mixer's 32-bit sum scaled by 1/2^29 (the driver's full scale) and written as is, peaks above 1.0 kept.
- **Rationale**: Esa's ITWAV.DRV writes 16-bit; the user wants no limits for material that is mixed further. The driver's existing float output (`OutFormat` 2, it_driver.c ~1135) clamps to ±1.0 and counts clips, so an unclamped variant is needed: a new output mode beside the existing it26 extension formats (16/24/float/8-bit), leaving the original 16-bit path (`OutFormat` 0, used by `test_pattern`) untouched.
- **Alternatives**: 16-bit (Esa) — clips loud stems; 24-bit integer — still clips at full scale; reading `MixBuffer` from outside the driver — the buffer is static to it_driver.c.

## R3 — Speed (real time or not)

- **Decision**: every render runs as fast as the CPU mixes, including whole songs.
- **Rationale**: Esa's pattern renders are already "faster than realtime" (WAV_SyncRenderLoop); only his whole-song path stays timer-paced because of the DOS driver model. Measured with `itplay -w` (same engine and driver) on the reference machine: JEFF93.IT 16×, synthscape_filters.it 21×, itdemo.it 36×, beyond_network.it 101× real time.
- **Alternatives**: timer-paced song renders — slower, same bytes.

## R4 — Keeping live audio away during a render

- **Decision**: the editor's `audio_cb` locks a dedicated `AudioMutex` around its call into the driver and outputs silence while `RenderActive` is set; the render sets the flag under that mutex, renders on the main thread, resets the driver (`Driver->InitSound()`), restores the Shift-F5 settings and clears the flag.
- **Rationale**: `WAVDriver_RenderAny` keeps FIFO and filter state (`OutPos`, `OutFilled`, dither, click removal) outside the engine lock (`DoTick` locks only around `Update` + `M32MixHandler`), so two threads in the driver would corrupt each other. `WAV_InitSound` (it_driver.c 997) resets all of that state and allocates nothing. Esa does the equivalent by swapping drivers (`Music_UnloadDriver` / `Music_AutoDetectSoundCard`).
- **Alternatives**: `ma_device_stop`/`start` around each render (slow, device-dependent); holding the engine mutex for the whole render (callback blocks for seconds → underruns and a possible OS-level stall).

## R5 — Stop and resume

- **Decision**: snapshot PlayMode, CurrentOrder, CurrentRow, CurrentPattern, NumberOfRows before the render; after a **single-pattern** render resume with `Music_PlayPattern(pattern, rows, row)` (PlayMode 1) or `Music_PlayPartSong(order, row)` (PlayMode 2); stems, whole-song and batch renders do not resume; nothing resumes from PlayMode 0.
- **Rationale**: `WAV_ResumeArmed` / `WAV_ResumePlayMode` / `Music_ResumeAfterRender` (IT_MUSIC.ASM 10114) and `WAV_LeaveMode` latching `WAV_DoResumeOnLeave` only when not multi and not song mode (card wav-render-keep-playback, commits ed62137, ac3332f). His MIDI-clock resume trigger belongs to his MIDI sync feature and is left out.

## R6 — Import into a sample slot

- **Decision**: slot = max(highest used sample, highest used instrument) + 1; fail with "Import failed: no free sample slot" beyond 99. Sample: flags present + forward loop (+16-bit, +stereo per the render), default volume 64, global volume 64, Cvt signed, default pan byte 32 (no pan), length in frames, loop 0..length, sustain loop off, C5 speed 44100, file-name field = the WAV name (12 chars), name "Pattern NNN". Data capped at 1 MB (64 × 16 KB pages) → "Pattern truncated to 1MB; sample N". F3 cursor (and the "current sample") → the slot. Instrument mode: `Music_AssignSampleToInstrument` (same-numbered instrument if blank else first blank) and the F4 cursor → it. Status "Pat NNN -> <file> -> sample NNN" + " in inst MMM".
- **Rationale**: `Music_ImportRenderedPattern` (IT_MUSIC.ASM 7121ff): `WAVI_HaveMax`, `WAV_LoopRender DB 1`, the IMPS header block (`+12h` flags, `+13h`/`+11h` = 64, `+2Eh` = 1, `+2Fh` = 32, `+30h` length, `+34h`/`+38h` loop, `+3Ch` rate, `+4` filename, `+14h` "Pattern NNN"), `WAV_Page` cap 64, `WAV_TruncatedMsg`, `D_SetCurrentSample` / `PE_SetLastInstrument`, `WAV_ImpRunMsg` built from `WAV_IMP_Pfx/Mid1/Mid2`.
- **Deviation (stereo)**: Esa sets the stereo bit (`+12h` bit 2) and keeps interleaved data; IT 2.17's mixer has no stereo samples (`sc->Bit = s->Flags & 2` only; IT_DISK.ASM `D_LoadSampleData` converts stereo WAVs through `O1_StereoSampleList`), so such a sample plays at twice the speed. it26 mixes the render to mono ((L+R)/2, IT's "Both") so imports stay IT-compatible.
- **Note**: Esa stages the import WAV in a local `C:\TEMP` (`D_GotoImportStageDir`) because reading network files into EMS crashes DOS; it26 imports from memory, so no file is written for an import render. The name still goes into the sample's file-name field.

## R7 — Names

> **Superseded for files on disk (2026-10-18)**: it26 writes descriptive long names, one folder per multi-file export (spec Clarifications "follow-up: file names"). Esa's rules below remain for reference and for the imported sample's 8.3 file-name field.


- **Decision**:
  - single-pattern renders: `LL` + HHMMSS (local 24-hour clock) + `.WAV` (`WAV_BuildTimestampBasename`, 2870);
  - Shift-Alt-M sweep, whole-song without a typed name, batch: `<PFX><NNNN>.WAV` — PFX = song name bytes 0..2 upper-cased, non-alphanumeric → `X`, a 0 or space before 3 characters pads the rest with `X`, empty name (first byte 0 or space) → `PTN`; NNNN = `WAV_RenderCounter`, incremented before use, starting at 0 per session (`WAV_BuildCounterName`, 6189);
  - F10 WAV with a typed name: the name as typed, upper-cased, `.WAV` if none; MWAV: base cut to 6 characters + 2-digit 1-based channel + extension (`WAV_BuildChannelFilename`, 2714);
  - sample export: HHMMSS of the export start + 2-digit slot + `.WAV` (`D_DumpBuildRunPrefix` / `D_DumpBuildName`); Esa's status text still says "SMPnn.WAV" (`DumpAllSamplesMsg`); it26 shows the real pattern and folder instead ("Dumped N samples as HHMMSSnn.WAV in <folder>", 2026-10-18, user report).
- **Rationale**: as cited. Batch forces counter naming (`WAV_BatchNaming`) because several renders finish within one second.

## R8 — Sample export

- **Decision**: slots 1..99, non-empty only, each through `RIS_SaveWAV` (8-bit written unsigned, 16-bit signed) into the render folder; stereo samples keep 2 channels (to be added to `RIS_SaveWAV`'s header path if not yet handled — check in implementation); playback continues. Selected-sample export (F3 Ctrl-Right) writes from a header copy whose C5 speed is set to 44100 ("Saving selected sample as C-4 44.1kHz WAV..." / "Saved C-4 44.1kHz WAV (sample N)"), "Selected sample is empty." on an empty slot. "No samples to dump" when there is nothing; otherwise "Dumped N samples as <HHMMSS>nn.WAV in <folder>".
- **Rationale**: `D_DumpAllSamplesWAV` with `DumpOnlySample` (IT_DISK.ASM), `D_DumpAll_KeepSampleRate`, messages at IT_DISK.ASM 768–803. The EMS interlock and the in-place sign-flip bug fix are DOS specifics; it26 reads sample memory without modifying it.

## R9 — Gestures and precedence

- **Decision**: see [contracts/keys-and-screens.md](contracts/keys-and-screens.md).
- **Rationale**: key tables `IT_PE.ASM` 763 (F2 Ctrl-O → `PEFunction_RenderPattern`, Shift via `K_IsKeyDown`), 1167–1205 (F11 rows), `IT_DISPL.ASM` 225–262 (F5 rows), `IT_I.ASM` 442–448 (F3 Ctrl-Right / Shift-Right), `IT_OBJ1.ASM` 3324 (Alt-W in the global key list), `IT_PE.ASM` 8267 (Shift-Alt-M → `PEFunction_StartMultiWAVKey`).
- **Corrections**: F5 Ctrl-O renders (Esa's `Display_RenderQuicksave` takes a held Ctrl for the sample dump — his comment says render). F3 Ctrl-Right exports one sample, Ctrl-Shift-Right all (his `I_SaveSelectedSampleWAV`).

## R10 — Stems and whole song

- **Decision**: Shift-Alt-M: channels whose pattern holds a note 0..119 (C-0..B-9) and that are not muted; F10 MWAV: every unmuted channel (Esa's song mode skips only on mute — no emptiness test); each pass solos one channel; mute state saved and restored; Esc stops after the channel being rendered; no import; no resume. F10 WAV: `Music_PlaySong(0)` to the song end (`StopEndOfPlaySection`), 30-minute cap as `itplay -w`.
- **Rationale**: `Music_StartMultiWAV`, `WAV_MultiAdvance` (`WAV_MultiSongFlag` bypasses `PE_ChannelIsEmpty`), `PE_ChannelIsEmpty` (IT_PE.ASM 8206), `Music_StartFullSongWAV` (2662), `Music_StartFullSongMWAV` (2900).

## R11 — Re-entry

- **Decision**: renders are synchronous on the main thread; the render loop polls keys between chunks; Esc or any render gesture stops the render, the file is finished with correct sizes, the status line reads "Render halted -- finalizing to Quicksave ..." while it closes.
- **Rationale**: `WAV_AlreadyActive` / `WAV_FinalizeRequest` (card wav-render-reentry-guard). In it26 there is no driver swap to wedge, so the guard reduces to "a second gesture stops like Esc".

## R12 — Quicksave folder and Alt-W

- **Decision**: `quicksavedir=` in `ited.cfg` (written by Save all Preferences, and at once by Shift-Alt-W as Esa's `D_MemorizeQuickSaveDir` → `D_SaveDirectoryConfiguration`); F12 row "Quicksave" (label text and coordinates from Esa's `DirectoryText2` / `QuickSaveDirectoryInput`); Enter = folder picker; Alt-W: empty folder → "Quick-save folder not configured." + F10; else save the module there with its current name and format (`D_QuickSave` → `D_SaveSong`); Shift-Alt-W: Quicksave = the current module folder, saved, "Quick-save folder memorized.".
- **Audit (T034, IT 2.14 key tables, `DW 1100h`)**: Alt-W is bound on F2 (`PEFunction_WipeExcessVolumes`, IT_PE.ASM 674), F3 (`D_SaveRawSample`, IT_OBJ1.ASM 3437) and F4 (`I_InstrumentClear`, IT_I.ASM 518); nowhere else. So the global Alt-W quicksave works on every other screen (F1 help excluded); Shift-Alt-W is bound nowhere in IT 2.14 and works everywhere.
- **F3 Ctrl-Right (decided 2026-10-18)**: IT 2.14 binds Ctrl-Left/Right globally (DisplayMinus/Plus = previous/next order), so Esa's F3 Ctrl-Right "export selected sample" would shadow it; it26 keeps IT's next order and puts the single-sample export on Ctrl-Alt-Right (Win32 delivers `ITK_ALT_RIGHT` + Ctrl, SDL `ITK_CTRL_RIGHT` + Alt; both accepted).
- **Precedence**: F3 keeps Alt-W (save sample as WAV, `SampleGlobalKeyList` precedes `GlobalKeyList`); every other screen gets the global Alt-W, as in Esa's fork. it26-specific: Alt-W in the pattern editor is IT's "Wipe volume" (`PE` Alt-W) — screen lists win there too, so Alt-W quicksave works where no screen binds Alt-W. **To verify per screen in implementation** against IT's key tables (the 2026-09 hotkey audit list).
