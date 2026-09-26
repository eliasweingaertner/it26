# Phase 1 Data Model: Authentic Load Sample Screen

## 1. `slibent_t` (extended) — one list row

Mirrors the original's 96-byte `DiskDataArea` record (research R2). Existing
fields kept; new ones added.

| Field | Record offset | Status | Meaning |
|---|---|---|---|
| `hdr` | +00h..+4Fh | existing | ITS header; `hdr.SampleName` holds `DirectoryMsg` for directories |
| `FileSize` | +50h | existing | bytes |
| `Date`, `Time` | +54h, +56h | **new** | DOS-packed date/time from the host mtime |
| `Format` | +58h | existing | type code; 0 unidentified, 1 dir, ≥ 20h module |
| `SortPri` | +5Ah | **new** | 0 dir, 1 library, 2 recognised, 3 unknown |
| `SrcFile` | — | existing | full host path (port extension) |

Validation: `Format == 1` ⇒ `SortPri == 0`, `hdr.Flags & 1 == 0`.

## 2. Listing

| Field | Meaning |
|---|---|
| `ents[]`, `n` | up to 620 entries (original cap) |
| `cur`, `top` | cursor and scroll (35 visible rows) |
| `dir` | current host directory |
| `in_module` | 0 = directory listing, 1 = samples of a module |
| `audition` | index whose data sits in the check slot (`SampleInMemory`), −1 none |

State transitions: `enter_dir → list → identify all → sort` ; `enter_module →
module list` ; `exit_module → directory list (re-read, edits dropped)`.

## 3. Edits

Written straight into `ents[cur].hdr` (research R8). Loop fields pass through
the same correction as F3's (`D_LSCheckLoopValues` ≡ port's loop clamp). Lost
when the listing is re-read.

## 4. Drive list

`drives[]` letters (Windows) or a single root (POSIX); `drive_cur`, `drive_top`
(10 visible).
