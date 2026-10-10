"""PortMachine: drive the it26 port (ited, ITED_REMOTE=1) with the same
interface as DosMachine, so one .hds script runs against both.

Key translation follows the port's Win32 backend (it_screen_win32.c
WndProc) step by step: the same key spec must reach the editor as the
same it_key_t {scan, flags, ch, code} that a real keypress in the window
would produce. The ITK_* values are read from the port's it_screen.h, so
they can't drift from the build.
"""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

from . import capture
from .keys import CHARS, parse_key
from .machine import TextScreen

# PC set-1 scancodes, +0x80 = E0-extended (the port's `scan` encoding)
SCAN = {
    "esc": 0x01, "minus": 0x0C, "equal": 0x0D, "backspace": 0x0E, "tab": 0x0F,
    "bracket_left": 0x1A, "bracket_right": 0x1B, "ret": 0x1C, "ctrl": 0x1D,
    "semicolon": 0x27, "apostrophe": 0x28, "grave_accent": 0x29, "shift": 0x2A,
    "backslash": 0x2B, "comma": 0x33, "dot": 0x34, "slash": 0x35,
    "shift_r": 0x36, "kp_multiply": 0x37, "alt": 0x38, "spc": 0x39,
    "caps_lock": 0x3A, "num_lock": 0x45, "scroll_lock": 0x46,
    "kp_7": 0x47, "kp_8": 0x48, "kp_9": 0x49, "kp_subtract": 0x4A,
    "kp_4": 0x4B, "kp_5": 0x4C, "kp_6": 0x4D, "kp_add": 0x4E, "kp_1": 0x4F,
    "kp_2": 0x50, "kp_3": 0x51, "kp_0": 0x52, "kp_decimal": 0x53,
    "f11": 0x57, "f12": 0x58, "less": 0x56,
    "kp_enter": 0x9C, "ctrl_r": 0x9D, "kp_divide": 0xB5, "alt_r": 0xB8,
    "home": 0xC7, "up": 0xC8, "pgup": 0xC9, "left": 0xCB, "right": 0xCD,
    "end": 0xCF, "down": 0xD0, "pgdn": 0xD1, "insert": 0xD2, "delete": 0xD3,
}
for i, c in enumerate("1234567890"):
    SCAN[c] = 0x02 + i
for row, base in (("qwertyuiop", 0x10), ("asdfghjkl", 0x1E), ("zxcvbnm", 0x2C)):
    for i, c in enumerate(row):
        SCAN[c] = base + i
for i in range(10):
    SCAN[f"f{i + 1}"] = 0x3B + i

PRESSED, LSHIFT, RSHIFT, LCTRL, RCTRL, LALT, RALT = 1, 2, 4, 8, 16, 32, 64
MODS = {"shift": LSHIFT, "shift_r": RSHIFT, "ctrl": LCTRL, "ctrl_r": RCTRL,
        "alt": LALT, "alt_r": RALT}

# qcode -> character it produces (US layout), for the WM_CHAR path
_CHAR_OF = {}
for ch, (q, sh) in CHARS.items():
    if ch not in "\n\t":
        _CHAR_OF[(q, sh)] = ch
for i in range(10):
    _CHAR_OF[(f"kp_{i}", False)] = str(i)
_CHAR_OF[("kp_decimal", False)] = "."
_CHAR_OF[("kp_add", False)] = "+"
_CHAR_OF[("kp_subtract", False)] = "-"


def load_itk(header: Path) -> dict[str, int]:
    """Evaluate the ITK_* enum in it_screen.h."""
    src = header.read_text(encoding="utf-8", errors="replace")
    body = re.search(r"enum\s*\{\s*ITK_NONE(.*?)\};", src, re.S).group(0)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = body[body.index("{") + 1:body.rindex("}")]
    vals, nxt = {}, 0
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, expr = (s.strip() for s in item.split("=", 1))
            nxt = int(expr, 0)
        else:
            name = item
        vals[name] = nxt
        nxt += 1
    return vals


class KeyTranslator:
    def __init__(self, itk: dict[str, int]):
        self.K = itk

    def events(self, spec: str) -> list[tuple[int, int, int, int]]:
        """Key spec -> list of (scan, flags, ch, code), as the Win32
        backend would queue them for one press of that chord."""
        K = self.K
        codes = parse_key(spec)
        mods = [c for c in codes if c in MODS]
        keys = [c for c in codes if c not in MODS]
        flags = PRESSED
        for m in mods:
            flags |= MODS[m]
        shift = bool(flags & (LSHIFT | RSHIFT))
        ctrl = bool(flags & (LCTRL | RCTRL))
        alt = bool(flags & (LALT | RALT))
        out = []
        if shift:                            # WM_KEYDOWN VK_SHIFT
            out.append((0x2A, PRESSED | LSHIFT, 0, K["ITK_SHIFT_PRESS"]))
        if not keys:                         # a lone modifier
            if shift:
                out.append((0x2A, PRESSED | LSHIFT, 0, K["ITK_SHIFT_RELEASE"]))
            return out
        q = keys[0]
        scan = SCAN.get(q, 0)
        main = self._code(q, scan, flags, shift, ctrl, alt)
        if main is not None:
            out.append(main)
        if shift:                            # WM_KEYUP VK_SHIFT keeps the
            s, f = (main[0], main[1]) if main else (0x2A, flags)   # last scan
            out.append((s, f, 0, K["ITK_SHIFT_RELEASE"]))
        return out

    def _code(self, q, scan, flags, shift, ctrl, alt):
        K = self.K

        def ev(code, ch=0):
            return (scan, flags, ch, code)

        fkey = int(q[1:]) if re.fullmatch(r"f\d+", q) else 0
        if q == "tab":
            return ev(K["ITK_SHIFT_TAB"] if shift else K["ITK_TAB"])
        if fkey in (9, 10) and shift and ctrl and not alt:
            return ev(K["ITK_CTRL_SHIFT_F9"] if fkey == 9 else K["ITK_CTRL_SHIFT_F10"])
        if shift and fkey == 9:
            return ev(K["ITK_SHIFT_F9"])
        if shift and fkey == 6:
            return ev(K["ITK_SHIFT_F6"])
        if shift and fkey == 5:
            return ev(K["ITK_SHIFT_F5"])
        if q == "scroll_lock":
            return ev(K["ITK_SCROLL_LOCK"])
        if alt:
            if q in ("ret", "kp_enter"):
                # the window only delivers this in the pattern editor
                # (Screen_AltEnterIsKey); elsewhere it toggles fullscreen
                return ev(K["ITK_ALT_ENTER"])
            if 1 <= fkey <= 8:
                return ev(K["ITK_ALT_F1"] + fkey - 1)
            if len(q) == 1 and q.isalpha():
                return ev(K["ITK_ALT_A"] + ord(q) - ord("a"))
            if len(q) == 1 and q.isdigit():
                return ev(K["ITK_ALT_0"] + int(q))
            m = {"insert": "ALT_INS", "delete": "ALT_DEL", "up": "ALT_UP",
                 "down": "ALT_DOWN", "left": "ALT_LEFT", "right": "ALT_RIGHT",
                 "home": "ALT_HOME", "end": "ALT_END",
                 "backspace": "ALT_BACKSPACE", "f11": "ALT_F11",
                 "f9": "ALT_F9", "f10": "ALT_F10", "f12": "ALT_F12",
                 "backslash": "ALT_BACKSLASH", "equal": "ALT_PLUS",
                 "kp_add": "ALT_PLUS", "minus": "ALT_MINUS",
                 "kp_subtract": "ALT_MINUS"}.get(q)
            if m:
                return ev(K["ITK_" + m])
        if ctrl:
            if q in ("equal", "kp_add"):
                return ev(K["ITK_CTRL_PLUS"])
            if q in ("minus", "kp_subtract"):
                return ev(K["ITK_CTRL_MINUS"])
            if shift and q in "1234" and len(q) == 1:
                return ev(K["ITK_CTRL_SHIFT_1"] + int(q) - 1)
            if not shift and q in "012345" and len(q) == 1:
                return ev(K["ITK_CTRL_0"] + int(q))
            m = {"up": "CTRL_UP", "down": "CTRL_DOWN", "left": "CTRL_LEFT",
                 "right": "CTRL_RIGHT", "home": "CTRL_HOME", "end": "CTRL_END",
                 "pgup": "CTRL_PGUP", "pgdn": "CTRL_PGDN", "insert": "CTRL_INS",
                 "delete": "CTRL_DEL", "backspace": "CTRL_BACKSPACE",
                 "f1": "CTRL_F1", "f2": "CTRL_F2", "f3": "CTRL_F3",
                 "f4": "CTRL_F4", "f5": "CTRL_F5", "f6": "CTRL_F6",
                 "f7": "CTRL_F7"}.get(q)
            if m:
                return ev(K["ITK_" + m])
            if q in ("ret", "kp_enter"):
                return ev(K["ITK_RCTRL_ENTER"] if flags & RCTRL else K["ITK_ENTER"])
            if q in ("h", "i", "m"):
                return ev({"h": 0x08, "i": 0x09, "m": 0x0D}[q])
        if shift:
            m = {"up": "SHIFT_UP", "down": "SHIFT_DOWN", "left": "SHIFT_LEFT",
                 "right": "SHIFT_RIGHT", "pgup": "SHIFT_PGUP",
                 "pgdn": "SHIFT_PGDN", "home": "SHIFT_HOME", "end": "SHIFT_END",
                 "kp_add": "SHIFT_PLUS", "kp_subtract": "SHIFT_MINUS"}.get(q)
            if m:
                return ev(K["ITK_" + m])
        if q == "kp_divide":
            return ev(K["ITK_KP_DIVIDE"])
        if q == "kp_multiply":
            return ev(K["ITK_KP_MULTIPLY"])
        m = {"up": "UP", "down": "DOWN", "left": "LEFT", "right": "RIGHT",
             "pgup": "PGUP", "pgdn": "PGDN", "home": "HOME", "end": "END",
             "insert": "INS", "delete": "DEL", "esc": "ESC", "ret": "ENTER",
             "kp_enter": "ENTER", "backspace": "BACKSPACE"}.get(q)
        if m:
            return ev(K["ITK_" + m])
        if fkey:
            return ev(K["ITK_F1"] + fkey - 1)
        # WM_CHAR / WM_SYSCHAR
        if alt:
            return None                  # WM_SYSCHAR is swallowed
        if ctrl:
            if len(q) == 1 and q.isalpha():
                return ev(ord(q) - ord("a") + 1)   # Ctrl-A..Z, ch = 0
            return None
        c = _CHAR_OF.get((q, shift))
        if c is None:
            return None
        return ev(ord(c), ord(c))


class PortMachine:
    """ited in remote mode. `exe` is ited(.exe); `cwd` is its working
    directory (where it looks for modules, like D:\\ for the DOS side)."""

    def __init__(self, exe: str | Path, cwd: str | Path, header: str | Path | None = None,
                 args: list[str] | None = None, env: dict | None = None,
                 key_gap_ms: int = 20):
        self.exe = Path(exe).resolve()
        self.cwd = Path(cwd).resolve()
        header = Path(header) if header else self.exe.parent.parent / "src" / "it_screen.h"
        self.tr = KeyTranslator(load_itk(header))
        self.args = args or []
        self.env = env or {}
        self.key_gap = key_gap_ms / 1000
        self.proc: subprocess.Popen | None = None
        self._tmp = Path(tempfile.mkdtemp(prefix="hdos-port-"))

    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self, timeout: float = 30) -> None:
        env = dict(os.environ, ITED_REMOTE="1", ITED_NOBANNER="1", **self.env)
        self.proc = subprocess.Popen(
            [str(self.exe), *self.args], cwd=self.cwd, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=open(self._tmp / "stderr.txt", "wb"), text=True, bufsize=1)
        self._expect("@ready", timeout)
        self.sync()

    def _send(self, line: str) -> None:
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _expect(self, want: str = "@ok", timeout: float = 30) -> str:
        # stdout is line-buffered by ited (fflush after each reply); a
        # blocking readline is fine because every command gets a reply
        line = self.proc.stdout.readline().strip()
        while line and not line.startswith("@"):
            line = self.proc.stdout.readline().strip()
        if not line:
            raise RuntimeError(f"ited exited; stderr in {self._tmp / 'stderr.txt'}")
        if line != want:
            raise RuntimeError(f"ited replied {line!r}, expected {want!r}")
        return line

    def sync(self) -> None:
        self._send("sync")
        self._expect()

    def stop(self) -> None:
        if self.running:
            try:
                self._send("quit")
                self.proc.wait(10)
            except Exception:
                self.proc.kill()
        self.proc = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()

    # ---- input (same signatures as DosMachine) --------------------------

    def key(self, spec: str, **_) -> None:
        for scan, flags, ch, code in self.tr.events(spec):
            self._send(f"ev {scan} {flags} {ch} {code}")
        self.sync()
        time.sleep(self.key_gap)

    def keys(self, specs, **kw) -> None:
        if isinstance(specs, str):
            specs = specs.split()
        for s in specs:
            self.key(s, **kw)

    def type(self, text: str, **_) -> None:
        for c in text:
            self.key("enter" if c == "\n" else "space" if c == " " else c)

    # ---- output ---------------------------------------------------------

    def screen(self) -> TextScreen:
        p = self._tmp / "dump.json"
        self._send(f"dump {p}")
        self._expect()
        return capture.load(p)

    def screenshot(self, path: str | Path) -> Path:
        from PIL import Image
        path = Path(path).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        bmp = self._tmp / "shot.bmp"
        self._send(f"shot {bmp}")
        self._expect()
        Image.open(bmp).save(path)
        return path

    def wait_stable(self, quiet: float = 0.5, timeout: float = 10, **_) -> TextScreen:
        # the editor is idle after a sync; let timers (messages, the
        # clock) run for `quiet` like the DOS side does
        self.sync()
        time.sleep(quiet)
        return self.screen()

    def wait_for_text(self, pattern: str, timeout: float = 30, poll: float = 0.1):
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        last = ""
        while time.monotonic() < deadline:
            last = self.screen().text()
            m = rx.search(last)
            if m:
                return m
            time.sleep(poll)
        raise TimeoutError(f"{pattern!r} not on screen after {timeout}s.\n"
                           f"Screen:\n{last}")
