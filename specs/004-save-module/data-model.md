# Phase 1 Data Model: Save Module & Message Editor

## it_save.c (new)

| Item | Original | Notes |
|---|---|---|
| `int Save_ITModule(const char *path)` | `D_SaveIT` | returns 0 on any write error (file deleted), 1 on success; takes the engine lock only around snapshotting live counts |
| `uint8_t SaveFormat` | `SaveFormat` (IT_DISK 420) | 3 = IT215 (default), 0 = IT214, 2 = uncompressed |
| `static uint8_t Bits8[256]` etc. | LUTs at DiskDataArea:10240 | built by transliterated table-setup code on first use |
| `static void write_bits(...)` | `WriteBits` | LSB-first accumulator into the block buffer |
| compressed block buffer | PatternDataArea:2 | `static uint8_t` 32KB+slack; u16 length prefix |
| `void Save_GetMessage(...)`/`Msg` externs | `Msg_GetMessageLength/Offset` | message buffer lives in it_editor.c, exposed via it_save.h |

Sample-header scratch: an 80-byte on-disk image per sample built from
`sample_t` (fields already in file layout; `OffsetInFile` patched at
data-write time; runtime `Data` pointer not written). Song memory is
never modified by saving.

## it_editor.c additions

| State | Original | Notes |
|---|---|---|
| `char MessageData[8000]` | IT_MSG MessageData | CR-separated, NUL-terminated |
| `MsgTopLine, MsgPos, MsgEdit, MsgColour` | TopLine/CurrentPosition/Edit/CharacterColour | colour 12 ↔ 6 via Ctrl-T |
| `SCR_MESSAGE` screen | mode 9 (message) | rows 13..47, 35 lines, x=2 |
| save requester state | mode 10 + `SaveFileName`/`FileSpecifier` | reuses Req* arrays; editable filename field; overwrite confirm flag |

## it_load.c addition

Message read when `Special & 1`: seek `MsgOffset`, read
`min(MsgLgth, 7999)` bytes into the editor's buffer via a registered
sink (`extern` hook `IT_LoadMessage(handler)` style is overkill — a
plain `char *Music_MessageBuf` extern owned by the editor keeps the
module boundary: the loader writes it, both sides see it via
`it_save.h`). Cleared on every load.

## Invariants

- Save→load round-trip: identical rendered audio; message identical;
  OrdNum/InsNum/SmpNum/PatNum re-derived identically on reload.
- Saving never mutates `Song`, `MessageData`, or playback state.
- All disk writes go through one `save_block()` with sticky error
  handling; error ⇒ target deleted, "Unable to save" status.
