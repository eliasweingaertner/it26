# tools/compare — it26 vs the real IT 2.14, screen by screen

Runs the original IT 2.14 headless in QEMU + FreeDOS and it26 headless in
its remote mode. Both get the **same keystroke script**, and every
`capture` step saves the screen on each side. The result is a report with
the two screenshots side by side, a pixel diff, and a cell-by-cell diff of
characters and colours. Idea: issue #24.

```
 tests/compare/*.hds ──┬──► IT 2.14 in QEMU/FreeDOS ──► build-compare/<run>/dos/*.json|png
   (one script)        │     keys: QMP send-key; screen: VGA VRAM + CRTC regs
                       └──► ited (ITED_REMOTE=1) ─────► build-compare/<run>/port/*.json|png
                             keys: it_key_t events on stdin; screen: cell buffer
                                         │
                                         ▼
                         build-compare/<run>/report.md (+ Crit notes per screen)
```

## One-time setup (Windows)

```
winget install SoftwareFreedomConservancy.QEMU
python -m venv tools/compare/.venv
tools/compare/.venv/Scripts/python -m pip install -r tools/compare/requirements.txt
tools\compare\build_port.bat                # Release ited.exe in build-hdos/
set IT214_DIR=C:\path\to\it214              # the unpacked IT 2.14 distribution
```

FreeDOS 1.4 is downloaded the first time the harness runs, into
`tools/compare/.work/`. IT 2.14 itself is not in the repository; point
`IT214_DIR` (or `--data`) at your copy. Linux and macOS work the same way
with `qemu-system-i386` on the PATH and `build-hdos/ited`.

In Git Bash, `export MSYS_NO_PATHCONV=1`. Otherwise DOS switches like `/S0`
get turned into Windows paths.

## Run a comparison

```
python tools/compare/compare.py compare-run \
    --script tests/compare/it_screens.hds --out build-compare/it_screens
```

- `--dos-only` / `--port-only` re-run one side. `--report-only` just
  rebuilds `report.md`, keeping the **Crit:** notes.
- `--strict` stops treating characters 00h and 20h as equal.
- Masks for cells that always differ (clock, FreeMem) go in the script:
  `mask 9,62,9,79`.

## Script format (`tests/compare/*.hds`)

```
@dos waittext Continue     # lines with @dos / @port run on one side only
@dos key enter             # (IT 2.14's sound card dialog at startup)
@port keys f3 f9
stable 800                 # wait until the screen is quiet (ms)
capture 01_load_module     # -> dos/01_load_module.*, port/01_load_module.*
key f2                     # f1..f12, esc, enter, tab, arrows, ctrl-s, alt-f12, A ...
type JEFF93.IT             # literal text
waittext Drifting Onwards  # regex on the text screen
mask 9,62,9,79             # row0,col0,row1,col1, ignored in every capture
```

## The port side: ITED_REMOTE

`src/it_screen_remote.c` is a `screen_backend_t` with no window. It reads
commands on stdin:

- `ev scan flags ch code` queues an `it_key_t`.
- `sync` replies `@ok` once the editor has taken every queued event and gone
  back to idle polling.
- `dump file.json` writes the cell buffer, and `shot file.bmp` calls
  `Screen_WriteBMP`.
- `quit` exits.

The key translation lives in `hdos/port.py` (`KeyTranslator`). It mirrors
`it_screen_win32.c`'s WndProc step by step, and reads the `ITK_*` values
from `src/it_screen.h`. One limitation: Alt-Enter is always delivered as the
key, because the remote side doesn't know which screen is up.

## Interactive use from Claude Code

`.mcp.json` registers `tools/compare/mcp_server.py`. It provides
`dos_start`, `dos_keys`, `dos_type`, `dos_screen`, `dos_screenshot`,
`dos_capture`, `dos_wait_for`, `dos_stop` and `dos_compare`, so a session
can poke at the real IT 2.14 while working on a screen.

## Notes

- The reference is the IT **2.14** binary, while the port follows the
  **2.17** source. Before filing a difference, check the string or layout in
  the ASM. Several (the copyright year, "Pattern Editor (F2)") are 2.17
  changes.
- Some differences come from the environment rather than the port: drive
  list, directory paths, "No dirs." vs `..`, FreeMem/FreeEMS, the clock.
- Findings so far: `docs/compare/2026-10-10/FINDINGS.md`.
