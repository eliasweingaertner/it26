"""DosMachine: one headless QEMU + FreeDOS session, controlled over QMP."""

from __future__ import annotations

import os
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from . import image
from .keys import parse_key, text_to_keys
from .qmp import QMP

QEMU_DIRS = [r"C:\Program Files\qemu", "/usr/bin", "/usr/local/bin",
             "/opt/homebrew/bin"]


def find_qemu() -> str:
    exe = "qemu-system-i386" + (".exe" if os.name == "nt" else "")
    found = shutil.which(exe)
    if found:
        return found
    for d in QEMU_DIRS:
        p = Path(d) / exe
        if p.exists():
            return str(p)
    raise FileNotFoundError("qemu-system-i386 not found")


def _fix_wav_header(path: Path) -> None:
    """QEMU's wav audiodev leaves the RIFF/data sizes at 0 unless it gets to
    shut down its audio subsystem; fill them in from the file size."""
    with open(path, "r+b") as f:
        head = f.read(44)
        if len(head) < 44 or head[:4] != b"RIFF" or head[36:40] != b"data":
            return
        size = f.seek(0, 2)
        f.seek(4)
        f.write(struct.pack("<I", size - 8))
        f.seek(40)
        f.write(struct.pack("<I", size - 44))


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@dataclass
class TextScreen:
    mode: int            # BIOS video mode (3 = 80-col colour text)
    cols: int
    rows: int
    cursor: tuple[int, int]   # (row, col)
    chars: bytes         # rows*cols code page 437 bytes
    attrs: bytes         # rows*cols attribute bytes

    @property
    def is_text(self) -> bool:
        return self.mode in (0, 1, 2, 3, 7)

    def lines(self) -> list[str]:
        """Rows as Unicode text (CP437 decoded, control glyphs mapped)."""
        out = []
        for r in range(self.rows):
            row = self.chars[r * self.cols:(r + 1) * self.cols]
            out.append("".join(CP437[b] for b in row))
        return out

    def text(self) -> str:
        return "\n".join(l.rstrip() for l in self.lines())

    def attr_lines(self) -> list[str]:
        """Attributes as two hex digits per cell (fg low nibble, bg high)."""
        return [self.attrs[r * self.cols:(r + 1) * self.cols].hex()
                for r in range(self.rows)]


# CP437 with the graphic glyphs for 0x00-0x1F and 0x7F, so every byte maps
# to one visible character and columns stay aligned.
_LOW = ("\u00a0☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼")
CP437 = [(_LOW[i] if i < 32 else "⌂" if i == 0x7F else bytes([i]).decode("cp437"))
         for i in range(256)]


class DosMachine:
    """A FreeDOS VM with C: = boot disk (read-only snapshot) and
    D: = a data disk built from a host folder.

    The data disk is the only state that survives: write files in before
    start(), read them back after stop()."""

    def __init__(self, workdir: str | Path, data_dir: str | Path | None = None,
                 run_bat: str | None = None, disk_mb: int = 64,
                 memory_mb: int = 32, sound: str = "sb16",
                 audio_wav: str | Path | None = None, display: str = "none",
                 cpu: str = "pentium", memory: str = "ems",
                 extra_args: list[str] | None = None):
        self.workdir = Path(workdir).resolve()
        self.workdir.mkdir(parents=True, exist_ok=True)
        self.data_img = self.workdir / "data.img"
        self.data_dir = Path(data_dir) if data_dir else None
        self.run_bat = run_bat
        self.disk_mb = disk_mb
        self.memory_mb = memory_mb
        self.sound = sound
        self.audio_wav = Path(audio_wav).resolve() if audio_wav else None
        self.display = display
        self.cpu = cpu
        self.memory = memory
        self.extra_args = extra_args or []
        self.proc: subprocess.Popen | None = None
        self.qmp: QMP | None = None
        self._tmp = Path(tempfile.mkdtemp(prefix="hdos-"))

    # ---- disk -----------------------------------------------------------

    def prepare_disk(self) -> None:
        image.make_fat16_image(self.data_img, self.disk_mb)
        if self.data_dir:
            image.import_tree(self.data_img, self.data_dir)
        if self.run_bat is not None:
            body = self.run_bat.replace("\r\n", "\n").replace("\n", "\r\n")
            image.write_file(self.data_img, "RUN.BAT", body.encode("cp437"))

    def put_file(self, dos_path: str, data: bytes) -> None:
        self._require_stopped()
        image.write_file(self.data_img, dos_path, data)

    def get_file(self, dos_path: str) -> bytes:
        self._require_stopped()
        return image.read_file(self.data_img, dos_path)

    def list_files(self):
        self._require_stopped()
        return image.list_files(self.data_img)

    def export(self, host_dir: str | Path) -> int:
        self._require_stopped()
        return image.export_tree(self.data_img, Path(host_dir))

    def _require_stopped(self):
        if self.running:
            raise RuntimeError("stop the machine before touching its disk")

    # ---- lifecycle ------------------------------------------------------

    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self, prepare: bool = True, wait_ready: bool = True,
              timeout: float = 60) -> None:
        boot = image.build_boot_image(self.memory)
        if prepare or not self.data_img.exists():
            self.prepare_disk()
        port = _free_port()
        args = [
            find_qemu(), "-machine", "pc", "-cpu", self.cpu,
            "-m", str(self.memory_mb), "-rtc", "base=localtime",
            "-drive", f"file={boot},format=raw,if=ide,index=0,snapshot=on",
            "-drive", f"file={self.data_img},format=raw,if=ide,index=1",
            "-boot", "c", "-vga", "std", "-no-reboot",
            "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
            "-name", "hdos", "-nic", "none",
        ]
        if self.sound != "none":
            if self.audio_wav:
                args += ["-audiodev", f"wav,id=snd0,path={self.audio_wav}"]
            else:
                args += ["-audiodev", "none,id=snd0"]
            args += ["-device", f"{self.sound},audiodev=snd0"]
        if self.display == "none":
            args += ["-display", "none"]
        elif self.display.startswith("vnc"):
            # "vnc" or "vnc:N" -> VNC on 127.0.0.1:590N for watching along
            n = self.display.split(":", 1)[1] if ":" in self.display else "0"
            args += ["-display", "none", "-vnc", f"127.0.0.1:{n}"]
        else:
            args += ["-display", self.display]       # e.g. "sdl", "gtk"
        args += self.extra_args
        log = open(self.workdir / "qemu.log", "wb")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT,
                                     creationflags=flags)
        try:
            self.qmp = QMP("127.0.0.1", port)
        except Exception:
            self.kill()
            raise RuntimeError("QEMU did not start; see "
                               f"{self.workdir / 'qemu.log'}")
        if wait_ready:
            self.wait_for_text("HDOS-READY", timeout=timeout)

    def stop(self, timeout: float = 10) -> None:
        """Power off. Files the program has already closed are on disk."""
        if self.qmp and self.running:
            try:
                self.qmp.cmd("quit")
            except Exception:
                pass
        if self.proc:
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.qmp:
            self.qmp.close()
        self.qmp = None
        self.proc = None
        if self.audio_wav and self.audio_wav.exists():
            _fix_wav_header(self.audio_wav)

    kill = stop

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
        shutil.rmtree(self._tmp, ignore_errors=True)

    # ---- input ----------------------------------------------------------

    def key(self, spec: str, hold_ms: int = 60, gap_ms: int = 60) -> None:
        """Press one key spec, e.g. 'f2', 'ctrl-s', 'alt-enter', 'A'."""
        codes = parse_key(spec)
        self.qmp.cmd("send-key", keys=[{"type": "qcode", "data": c} for c in codes],
                     **{"hold-time": hold_ms})
        time.sleep((hold_ms + gap_ms) / 1000)

    def keys(self, specs: list[str] | str, **kw) -> None:
        if isinstance(specs, str):
            specs = specs.split()
        for s in specs:
            self.key(s, **kw)

    def type(self, text: str, hold_ms: int = 40, gap_ms: int = 40) -> None:
        for codes in text_to_keys(text):
            self.qmp.cmd("send-key", keys=[{"type": "qcode", "data": c} for c in codes],
                         **{"hold-time": hold_ms})
            time.sleep((hold_ms + gap_ms) / 1000)

    def key_down(self, qcode: str, down: bool = True) -> None:
        """Raw press/release for chords that send-key can't express."""
        self.qmp.cmd("input-send-event", events=[{"type": "key", "data": {
            "down": down, "key": {"type": "qcode", "data": qcode}}}])

    def mouse_move(self, dx: int, dy: int) -> None:
        self.qmp.cmd("input-send-event", events=[
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}}])

    def mouse_button(self, button: str = "left", down: bool = True) -> None:
        self.qmp.cmd("input-send-event", events=[
            {"type": "btn", "data": {"button": button, "down": down}}])

    # ---- output ---------------------------------------------------------

    def read_mem(self, addr: int, size: int) -> bytes:
        p = self._tmp / f"mem_{addr:x}_{size:x}.bin"
        self.qmp.cmd("pmemsave", val=addr, size=size, filename=str(p))
        return p.read_bytes()

    def _port_in(self, port: int) -> int:
        return int(self.qmp.hmp(f"i /b 0x{port:x}").split("=")[1], 16)

    def _port_out(self, port: int, value: int) -> None:
        self.qmp.hmp(f"o /b 0x{port:x} 0x{value:x}")

    def vga_regs(self) -> dict:
        """CRTC and graphics-controller registers. The VM is paused for the
        few milliseconds this takes and the index registers are restored,
        so the guest never sees a change."""
        was_running = self.qmp.cmd("query-status")["running"]
        if was_running:
            self.qmp.cmd("stop")
        try:
            crt_idx, gr_idx = self._port_in(0x3D4), self._port_in(0x3CE)
            cr = {}
            for i in (0x01, 0x07, 0x09, 0x0A, 0x0C, 0x0D, 0x0E, 0x0F, 0x12, 0x13):
                self._port_out(0x3D4, i)
                cr[i] = self._port_in(0x3D5)
            self._port_out(0x3CE, 0x06)
            gr6 = self._port_in(0x3CF)
            self._port_out(0x3D4, crt_idx)
            self._port_out(0x3CE, gr_idx)
        finally:
            if was_running:
                self.qmp.cmd("cont")
        return {"cr": cr, "gr6": gr6}

    def _vram_base(self) -> int:
        if not hasattr(self, "_vram"):
            for dev in self.qmp.cmd("query-pci")[0]["devices"]:
                if dev["class_info"]["class"] == 0x0300:
                    self._vram = next(r["address"] for r in dev["regions"]
                                      if r.get("bar") == 0)
        return self._vram

    def screen(self) -> TextScreen:
        """The displayed text page, read from VGA memory as the card shows
        it: geometry, start address and cursor come from the CRTC, not the
        BIOS data area, so programs that program the VGA directly (like
        Impulse Tracker's 80x50 mode) are read correctly."""
        r = self.vga_regs()
        cr = r["cr"]
        graphics = bool(r["gr6"] & 1)
        cols = cr[0x01] + 1
        pitch = cr[0x13] * 2 or cols
        vde = cr[0x12] | ((cr[0x07] & 0x02) << 7) | ((cr[0x07] & 0x40) << 3)
        cell_h = (cr[0x09] & 0x1F) + 1
        scan = (vde + 1) // (2 if cr[0x09] & 0x80 else 1)
        rows = max(1, scan // cell_h)
        start = (cr[0x0C] << 8) | cr[0x0D]
        cur = (cr[0x0E] << 8) | cr[0x0F]
        cur_rc = ((cur - start) // pitch, (cur - start) % pitch)
        if cr[0x0A] & 0x20 or not (0 <= cur_rc[0] < rows):
            cur_rc = (-1, -1)                       # cursor hidden
        # QEMU keeps VRAM plane-interleaved: cell i -> bytes 4i (char), 4i+1
        # (attribute); planes 2/3 hold the font.
        need = (start + rows * pitch) * 4
        vram = self.read_mem(self._vram_base(), min(need, 0x40000))
        chars, attrs = bytearray(), bytearray()
        for row in range(rows):
            base = (start + row * pitch) * 4
            chunk = vram[base:base + cols * 4]
            chars += chunk[0::4]
            attrs += chunk[1::4]
        return TextScreen(0x12 if graphics else 3, cols, rows, cur_rc,
                          bytes(chars), bytes(attrs))

    def screenshot(self, path: str | Path) -> Path:
        """PNG of what the VGA card shows (text or graphics modes)."""
        path = Path(path).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.qmp.cmd("screendump", filename=str(path), format="png")
        return path

    def wait_for_text(self, pattern: str, timeout: float = 30,
                      poll: float = 0.2) -> re.Match:
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        last = ""
        while time.monotonic() < deadline:
            if not self.running:
                raise RuntimeError("QEMU exited while waiting for "
                                   f"{pattern!r}")
            last = self.screen().text()
            m = rx.search(last)
            if m:
                return m
            time.sleep(poll)
        raise TimeoutError(f"{pattern!r} not on screen after {timeout}s.\n"
                           f"Screen:\n{last}")

    def wait_stable(self, quiet: float = 0.5, timeout: float = 10,
                    poll: float = 0.1) -> TextScreen:
        """Wait until the text screen stops changing for `quiet` seconds."""
        deadline = time.monotonic() + timeout
        prev, since = None, time.monotonic()
        while time.monotonic() < deadline:
            s = self.screen()
            sig = (s.chars, s.attrs, s.cursor)
            if sig != prev:
                prev, since = sig, time.monotonic()
            elif time.monotonic() - since >= quiet:
                return s
            time.sleep(poll)
        return s
