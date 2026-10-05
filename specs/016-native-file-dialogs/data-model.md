# Data Model: Native File Dialogs and Platform-Aware Help

Feature: [spec.md](./spec.md) · Research: [research.md](./research.md)

## DialogRequest (`it_dialog_req_t`, `src/it_screen.h`)

| Field | Type | Meaning |
|---|---|---|
| `kind` | enum `IT_DLG_OPEN_MODULE`, `IT_DLG_SAVE_MODULE`, `IT_DLG_OPEN_SAMPLE`, `IT_DLG_OPEN_INSTRUMENT`, `IT_DLG_PICK_FOLDER` | what is chosen; selects title, filters, open/save/folder mode |
| `start_dir` | `const char *` | folder the dialog opens in (R6); invalid/empty → working directory |
| `suggest_name` | `const char *` | save only: pre-filled file name (current song file name, or `UNTITLED.IT`) |
| `save_format` | `int` 0 = IT, 1 = S3M | save only: pre-selected type |

Filters per kind (open dialogs also offer "All files"):

| Kind | Filter |
|---|---|
| OPEN_MODULE | the extensions `Import_KnownExt()` accepts (IT, S3M, MOD, XM, …) |
| SAVE_MODULE | `*.it`, `*.s3m` (R7) |
| OPEN_SAMPLE | extensions `RIS_KnownExt()` accepts, plus module extensions (R8) |
| OPEN_INSTRUMENT | extensions `RI_KnownExt()` accepts, plus module extensions (R8) |
| PICK_FOLDER | none |

## DialogResult (`it_dialog_res_t`)

| Field | Type | Meaning |
|---|---|---|
| `status` | `IT_DLG_CHOSEN` / `IT_DLG_CANCELLED` / `IT_DLG_UNAVAILABLE` / `IT_DLG_REJECTED` | outcome; REJECTED = chosen but unusable (too long; Windows lossy save name without 8.3 support) with `reason` set |
| `path` | `char[IT_DLG_PATH_MAX]` | OS-usable path in the C library's encoding (R5); never cut |
| `display` | `char[IT_DLG_PATH_MAX]` | CP437 rendering of `path`, `?` per unrepresentable character |
| `lossy` | `int` | 1 if `display` contains a substitution |
| `save_format` | `int` | save only: 0 IT / 1 S3M, from type index or extension (R7) |
| `reason` | `const char *` | status-line text for REJECTED / UNAVAILABLE |

`IT_DLG_PATH_MAX` = 1024. A chosen path that does not fit is REJECTED
(FR-013).

**Validation rules**
- PICK_FOLDER: `lossy` → REJECTED ("Folder name has characters it26
  cannot show"); length > 64 (F12 field width) → REJECTED ("Path too long
  for this field").
- SAMPLE/INSTRUMENT: folder part > `sizeof(LsDir)-1` / instrument dir
  limit → REJECTED ("Path too long").
- Modules: base name > the save/load name buffer used for the call →
  REJECTED; the folder goes to `chdir` and has no fixed limit beyond
  `IT_DLG_PATH_MAX`.

## Backend additions (`screen_backend_t`)

| Member | Type | Meaning |
|---|---|---|
| `file_dialog` | `int (*)(const it_dialog_req_t *, it_dialog_res_t *)` | `NULL` = no dialogs on this backend (terminal) |
| `preview_key` | `const char *` | label of the held note-preview key when it is not Caps Lock (`"Right Option"` on macOS SDL); `NULL` = Caps Lock, original help |

## Directory state touched (existing variables, R6)

| Variable | Updated by | Rule |
|---|---|---|
| process working directory | open/save module | `chdir(folder)` after success only |
| `LsDir`, `DirSample` | open sample | as `ls_set_dir()` |
| Load Instrument directory, `DirInstr` | open instrument | as the Load Instrument screen does |
| `DirModule` / `DirSample` / `DirInstr` | F12 folder pick | focused field only, as typed |

## Help line lists

| Item | Rule |
|---|---|
| `HelpContextPtrs[c]` (generated) | unchanged |
| `PortHelpPreview` | replaces `HLP_helpcontext1_181` when `preview_key != NULL` |
| `PortHelpAdd[c]` | appended to contexts global, 2, 5, 7 when `file_dialog != NULL`, after a heading line |
| merged list | built once per context on first use; `draw_help` and `help_key` read through `help_lines(c)` |

## State transitions (one dialog)

```
idle --key--> [backend available?] --no--> status "No file dialog available" --> idle
                     | yes
                     v
               dialog open (UI thread blocked, audio running)
                     |
        cancelled ---+--- chosen ---> validate --rejected--> status reason --> idle
           |                              | ok
           v                              v
   input flush --> idle        input flush --> perform (load/save/set) via the
                                               original screen's code path
                                               --> update directory (success only) --> idle
```
