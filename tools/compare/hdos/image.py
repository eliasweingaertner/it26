"""Disk images: the FreeDOS boot disk (C:) and per-session data disks (D:).

C: is built once from the FreeDOS 1.4 LiteUSB image with the installer
removed from the startup path. QEMU opens it with snapshot=on, so a session
never changes it.

D: is a fresh FAT16 hard-disk image made from a host folder for every
session. When the machine is stopped, its contents can be copied back into a
host folder (`export_tree`). Never touch a data image while QEMU is running.
"""

from __future__ import annotations

import os
import shutil
import struct
import urllib.request
import zipfile
from pathlib import Path

from pyfatfs.PyFatFS import PyFatFS

ROOT = Path(__file__).resolve().parent.parent   # tools/compare
WORK = ROOT / ".work"                            # gitignored
CACHE = WORK / "cache"
IMAGES = WORK / "images"

# memory manager line for FDCONFIG.SYS, per boot image flavour
MEMORY = {
    "ems": "DEVICE=C:\\FREEDOS\\BIN\\JEMMEX.EXE NOVME\n",  # XMS+EMS+UMBs, V86
    "xms": "DEVICE=C:\\FREEDOS\\BIN\\HIMEMX.EXE\n",        # XMS, real mode
    "none": "",                                          # plain real mode
}

FREEDOS_URL = ("https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/"
               "distributions/1.4/FD14-LiteUSB.zip")

PART_LBA = 63          # first partition starts at sector 63 (CHS-aligned)
HEADS, SPT = 16, 63

FDCONFIG = """\
!COUNTRY=001,437
!LASTDRIVE=Z
!BUFFERS=20
!FILES=40
DOS=HIGH
DOS=UMB
{memdev}SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P=C:\\FDAUTO.BAT
"""

# BLASTER matches QEMU's sb16 defaults (iobase 0x220, irq 5, dma 1, hdma 5).
# HDOS-READY / HDOS-DONE are markers the controller waits for on screen.
FDAUTO = """\
@ECHO OFF
SET DOSDIR=C:\\FREEDOS
SET PATH=C:\\FREEDOS\\BIN;D:\\
SET TEMP=D:\\
SET BLASTER=A220 I5 D1 H5 P330 T6
D:
CD \\
ECHO HDOS-READY
IF EXIST D:\\RUN.BAT CALL D:\\RUN.BAT
ECHO HDOS-DONE
"""


def _lines_crlf(text: str) -> bytes:
    return text.replace("\n", "\r\n").encode("cp437")


def fetch_freedos() -> Path:
    """Download and unpack the FreeDOS LiteUSB image into cache/freedos."""
    dest = CACHE / "freedos"
    img = dest / "FD14LITE.img"
    if img.exists():
        return img
    dest.mkdir(parents=True, exist_ok=True)
    zpath = dest / "FD14-LiteUSB.zip"
    if not zpath.exists():
        print(f"downloading {FREEDOS_URL}")
        urllib.request.urlretrieve(FREEDOS_URL, zpath)
    with zipfile.ZipFile(zpath) as z:
        z.extract("FD14LITE.img", dest)
    return img


def build_boot_image(memory: str = "ems", force: bool = False) -> Path:
    """Create images/boot-<memory>.img: FreeDOS that boots straight to
    D:\\RUN.BAT. `memory` picks the memory manager (see MEMORY)."""
    out = IMAGES / f"boot-{memory}.img"
    if out.exists() and not force:
        return out
    src = fetch_freedos()
    IMAGES.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    shutil.copyfile(src, tmp)
    fs = PyFatFS(str(tmp), offset=PART_LBA * 512, preserve_case=False)
    try:
        fs.writebytes("/fdconfig.sys",
                      _lines_crlf(FDCONFIG.format(memdev=MEMORY[memory])))
        fs.writebytes("/fdauto.bat", _lines_crlf(FDAUTO))
        if fs.exists("/setup.bat"):
            fs.remove("/setup.bat")
    finally:
        fs.close()
    os.replace(tmp, out)
    return out


# --------------------------------------------------------------------------
# FAT16 data disks

def _chs(lba: int) -> bytes:
    c = lba // (HEADS * SPT)
    if c > 1023:
        return b"\xfe\xff\xff"
    h = (lba // SPT) % HEADS
    s = lba % SPT + 1
    return bytes([h, ((c >> 2) & 0xC0) | s, c & 0xFF])


def make_fat16_image(path: Path, size_mb: int = 64, label: str = "HDOS") -> None:
    """Write an empty, MBR-partitioned FAT16 disk of size_mb megabytes."""
    cyls = max(1, size_mb * 1024 * 1024 // (HEADS * SPT * 512))
    total = cyls * HEADS * SPT
    psecs = total - PART_LBA
    root_entries, reserved, nfats = 512, 1, 2
    root_secs = root_entries * 32 // 512
    for spc in (1, 2, 4, 8, 16, 32, 64):
        fat_secs = 1
        while True:
            data = psecs - reserved - nfats * fat_secs - root_secs
            clusters = data // spc
            need = ((clusters + 2) * 2 + 511) // 512
            if need <= fat_secs:
                break
            fat_secs = need
        if clusters <= 65524:
            break
    if clusters < 4085:
        raise ValueError("disk too small for FAT16")

    bs = bytearray(512)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<HBHBHHBHHHII", bs, 11,
                     512, spc, reserved, nfats, root_entries,
                     psecs if psecs < 65536 else 0, 0xF8, fat_secs,
                     SPT, HEADS, PART_LBA, psecs if psecs >= 65536 else 0)
    struct.pack_into("<BBBI11s8s", bs, 36, 0x80, 0, 0x29, 0x48444F53,
                     label.upper().ljust(11)[:11].encode(), b"FAT16   ")
    bs[510:512] = b"\x55\xAA"

    mbr = bytearray(512)
    entry = (bytes([0x00]) + _chs(PART_LBA) + bytes([0x06]) + _chs(total - 1)
             + struct.pack("<II", PART_LBA, psecs))
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"

    fat = bytearray(fat_secs * 512)
    fat[0:4] = b"\xF8\xFF\xFF\xFF"

    with open(path, "wb") as f:
        f.truncate(total * 512)
        f.write(mbr)
        f.seek(PART_LBA * 512)
        f.write(bs)
        f.seek((PART_LBA + reserved) * 512)
        for _ in range(nfats):
            f.write(fat)


def _dos_name(name: str) -> str:
    stem, dot, ext = name.upper().rpartition(".")
    if not dot:
        stem, ext = ext, ""
    if not stem or len(stem) > 8 or len(ext) > 3 or " " in name:
        raise ValueError(f"not an 8.3 name, DOS would not see it as-is: {name!r}")
    return name.upper()


def import_tree(img: Path, host_dir: Path, dos_dir: str = "/") -> int:
    """Copy host_dir (recursively) into the image. Returns the file count."""
    fs = PyFatFS(str(img), offset=PART_LBA * 512, preserve_case=False)
    n = 0
    try:
        for cur, dirs, files in os.walk(host_dir):
            rel = Path(cur).relative_to(host_dir)
            parts = [_dos_name(p) for p in rel.parts]
            base = "/".join([dos_dir.rstrip("/")] + parts) or "/"
            base = "/" + base.strip("/")
            if base != "/" and not fs.exists(base):
                fs.makedirs(base)
            for fn in files:
                data = (Path(cur) / fn).read_bytes()
                fs.writebytes(base.rstrip("/") + "/" + _dos_name(fn), data)
                n += 1
    finally:
        fs.close()
    return n


def write_file(img: Path, dos_path: str, data: bytes) -> None:
    fs = PyFatFS(str(img), offset=PART_LBA * 512, preserve_case=False)
    try:
        parent = "/" + "/".join(dos_path.strip("/").split("/")[:-1])
        if parent != "/" and not fs.exists(parent):
            fs.makedirs(parent)
        fs.writebytes("/" + dos_path.strip("/").upper(), data)
    finally:
        fs.close()


def read_file(img: Path, dos_path: str) -> bytes:
    fs = PyFatFS(str(img), offset=PART_LBA * 512, read_only=True)
    try:
        return fs.readbytes("/" + dos_path.strip("/"))
    finally:
        fs.close()


def list_files(img: Path) -> list[tuple[str, int]]:
    fs = PyFatFS(str(img), offset=PART_LBA * 512, read_only=True)
    try:
        return [(p, fs.getsize(p)) for p in fs.walk.files("/")]
    finally:
        fs.close()


def export_tree(img: Path, host_dir: Path) -> int:
    """Copy every file from the image into host_dir. Returns the file count."""
    fs = PyFatFS(str(img), offset=PART_LBA * 512, read_only=True)
    n = 0
    try:
        for p in fs.walk.files("/"):
            out = host_dir / p.lstrip("/")
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(fs.readbytes(p))
            n += 1
    finally:
        fs.close()
    return n
