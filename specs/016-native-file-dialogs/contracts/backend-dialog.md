# Contract: Backend File-Dialog Interface

Internal interface between the shared editor (`it_editor.c`), the screen
layer (`it_screen.c`) and the presentation backends. Types are defined in
[data-model.md](../data-model.md).

## `int Screen_FileDialog(const it_dialog_req_t *req, it_dialog_res_t *res)`

Platform-neutral entry point used by the editor.

1. If the environment variable `ITED_DIALOG_FAKE` is set (test hook):
   `!cancel` → CANCELLED, `!unavailable` → UNAVAILABLE, anything else →
   CHOSEN with that value as `path`; `display` computed by the shared
   UTF-8 → CP437 helper; `save_format` from the extension. No backend
   call.
2. Else if the active backend's `file_dialog` is `NULL` → UNAVAILABLE.
3. Else call it.
4. Apply the shared validation rules (length, F12 `lossy`) → may turn
   CHOSEN into REJECTED.
5. Return `res->status`.

Never holds the engine lock. Never changes directories or song state.

## Backend `file_dialog(req, res)`

**Must**
- Show the native dialog for `req->kind`, owned by / in front of the
  tracker window, starting in `req->start_dir` (fallback: working
  directory).
- Block until the user decides; keep the window responsive where the
  platform needs it (Linux: pump events and present while the helper
  runs).
- Fill `path` (OS-usable, R5), `display` (CP437, `?` substitution),
  `lossy`, and for saves `save_format`.
- Not show its own overwrite confirmation (the editor asks, as F10 does).
- Before returning, flush input: queued tracker key events, pending OS
  key/text events, modifier state; queue one `ITK_SHIFT_RELEASE`.
- Restore the window state (fullscreen, focus) it found.

**Must not**
- Touch song data, directories or editor state.
- Leave any keystroke typed into the dialog in the tracker's queue.

## Backend `preview_key`

`NULL` unless the held note-preview key is not Caps Lock on this
platform. SDL backend on macOS: `"Right Option"`. Read by the help code
only.

## Platform implementations

| File | Backend | Mechanism |
|---|---|---|
| `src/it_dialog_win32.c` | Win32 | `IFileOpenDialog` / `IFileSaveDialog` (COM from C) |
| `src/it_dialog_mac.c` | SDL on macOS | `NSOpenPanel` / `NSSavePanel` via the Objective-C runtime from C |
| `src/it_dialog_posix.c` | SDL on Linux/other POSIX | `zenity`, then `kdialog`, via `posix_spawnp` |
