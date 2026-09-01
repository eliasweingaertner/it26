"""Assemble an Impulse Tracker keyboard layout table into KEYBOARD.CFG.

The original shipped its layouts as TASM sources (Keyboard/*.ASM in the
BSD release) that were assembled with TASM + TLINK /TDC and renamed to
KEYBOARD.CFG. This does the same job without a DOS toolchain: it reads
the DB/DW directives between StartKeyboardTable and EndKeyboardTable and
emits `FileLength DW` followed by the table bytes, which is exactly what
ited's Key_LoadLayout() expects (see specs/014-scancode-keyboard-input/
contracts/keyboard-cfg-format.md).

    python tools/asm_keyboard_cfg.py path/to/DE.ASM DE.CFG

Then point ited at it with `keyboard_cfg=DE.CFG` in ited.cfg. Note this
is only needed when the host keyboard layout is unavailable or when
reproducing a specific DOS layout exactly -- ited uses the host layout
by default and needs no configuration.
"""
import io, re, sys, struct
src = io.open(sys.argv[1], encoding='latin-1').read()
# keep only the table body
body = src.split('StartKeyboardTable:', 1)[1].split('EndKeyboardTable', 1)[0]
out = bytearray()
for raw in body.splitlines():
    line = raw.split(';', 1)[0].strip()
    if not line:
        continue
    m = re.match(r'^(DB|DW)\s+(.*)$', line, re.I)
    if not m:
        continue
    kind, val = m.group(1).upper(), m.group(2).strip()
    if val.startswith("'") and val.endswith("'") and len(val) == 3:
        n = ord(val[1])
    elif val.startswith('"') and val.endswith('"') and len(val) == 3:
        n = ord(val[1])
    elif re.match(r'^[0-9a-fA-F]+h$', val):
        n = int(val[:-1], 16)
    elif re.match(r'^\d+$', val):
        n = int(val)
    else:
        continue
    out += bytes([n & 0xFF]) if kind == 'DB' else struct.pack('<H', n & 0xFFFF)
open(sys.argv[2], 'wb').write(struct.pack('<H', len(out)) + bytes(out))
print('table bytes:', len(out))
