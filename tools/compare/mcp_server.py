"""MCP server: lets Claude Code boot FreeDOS in QEMU and drive DOS programs.

Registered for this repository in .mcp.json (uses tools/compare/.venv).
"""

from __future__ import annotations

import shlex
import time
from pathlib import Path

from mcp.server.mcpserver import Image, MCPServer

from hdos import DosMachine, capture

ROOT = Path(__file__).resolve().parent
SESSIONS = ROOT / ".work" / "sessions"

mcp = MCPServer("hdos")
machines: dict[str, DosMachine] = {}


def _m(session: str) -> DosMachine:
    m = machines.get(session)
    if not m:
        raise ValueError(f"no session {session!r}; call dos_start first")
    return m


def _screen_text(m: DosMachine, attrs: bool = False) -> str:
    s = m.screen()
    head = f"[{s.cols}x{s.rows}{' graphics mode' if not s.is_text else ''}, cursor {s.cursor}]"
    if not s.is_text:
        return head + " (use dos_screenshot to see it)"
    body = "\n".join(f"{i:2d}|{line}" for i, line in enumerate(s.lines()))
    if attrs:
        body += "\n-- attributes (hex per cell) --\n" + "\n".join(
            f"{i:2d}|{a}" for i, a in enumerate(s.attr_lines()))
    return head + "\n" + body


@mcp.tool()
def dos_start(session: str = "main", data_dir: str = "", command: str = "",
              memory: str = "ems", sound: str = "sb16", vnc: bool = False,
              audio_wav: str = "") -> str:
    """Boot FreeDOS in headless QEMU. data_dir (a host folder, 8.3 file names)
    is copied to drive D:, which is the current drive. `command` is run from
    D:\\ after boot (e.g. 'IT /S5'). memory: ems (JemmEx), xms, none.
    sound: sb16, gus, adlib, none. vnc=True lets a human watch on
    127.0.0.1:5900. Returns the screen after startup settles."""
    if session in machines and machines[session].running:
        raise ValueError(f"session {session!r} is running; dos_stop it first")
    run_bat = f"@ECHO OFF\n{command}\n" if command else None
    m = DosMachine(SESSIONS / session, data_dir=data_dir or None,
                   run_bat=run_bat, memory=memory, sound=sound,
                   audio_wav=audio_wav or None,
                   display="vnc" if vnc else "none")
    m.start(timeout=120)
    machines[session] = m
    m.wait_stable(quiet=1.0, timeout=30)
    return _screen_text(m)


@mcp.tool()
def dos_keys(keys: str, session: str = "main", settle_ms: int = 400) -> str:
    """Press keys in order, space separated. Names: f1..f12, esc, enter, tab,
    space, backspace, up/down/left/right, pgup/pgdn, home/end, ins/del,
    kp_0..kp_9, kp+ kp- kp* kp/, single characters, and modifiers joined with
    '-': ctrl-s, alt-f12, shift-tab, ctrl-alt-del. Returns the screen once it
    has been unchanged for settle_ms."""
    m = _m(session)
    m.keys(shlex.split(keys))
    m.wait_stable(quiet=settle_ms / 1000, timeout=15)
    return _screen_text(m)


@mcp.tool()
def dos_type(text: str, session: str = "main", settle_ms: int = 400) -> str:
    """Type literal text (US layout; '\\n' presses Enter). Returns the screen."""
    m = _m(session)
    m.type(text.replace("\\n", "\n"))
    m.wait_stable(quiet=settle_ms / 1000, timeout=15)
    return _screen_text(m)


@mcp.tool()
def dos_screen(session: str = "main", attrs: bool = False) -> str:
    """Current text screen, read from VGA memory (exact characters, CP437
    decoded, rows numbered). attrs=True adds colour attributes per cell."""
    return _screen_text(_m(session), attrs)


@mcp.tool()
def dos_screenshot(session: str = "main", save_as: str = "") -> Image:
    """PNG of the display, for VGA graphics modes or to check custom fonts
    and colours. Optionally also saved to save_as."""
    m = _m(session)
    path = Path(save_as) if save_as else SESSIONS / session / "screenshot.png"
    m.screenshot(path)
    return Image(path=str(path))


@mcp.tool()
def dos_capture(name: str, out_dir: str, session: str = "main") -> str:
    """Save the screen as <out_dir>/<name>.json/.txt/.png for comparisons."""
    m = _m(session)
    p = capture.save(m.screen(), Path(out_dir) / name)
    m.screenshot(Path(out_dir) / f"{name}.png")
    return str(p)


@mcp.tool()
def dos_wait_for(pattern: str, session: str = "main", timeout_s: float = 30) -> str:
    """Wait until the regex appears on the text screen; returns the screen."""
    m = _m(session)
    m.wait_for_text(pattern, timeout=timeout_s)
    return _screen_text(m)


@mcp.tool()
def dos_wait(ms: int, session: str = "main") -> str:
    """Let the machine run for ms milliseconds, then return the screen."""
    time.sleep(ms / 1000)
    return _screen_text(_m(session))


@mcp.tool()
def dos_mouse(dx: int = 0, dy: int = 0, click: str = "", session: str = "main") -> str:
    """Move the mouse relatively (mickeys) and optionally click
    ('left', 'right', 'middle'). Needs a DOS mouse driver in the guest."""
    m = _m(session)
    if dx or dy:
        m.mouse_move(dx, dy)
    if click:
        m.mouse_button(click, True)
        time.sleep(0.08)
        m.mouse_button(click, False)
    m.wait_stable(quiet=0.3, timeout=10)
    return _screen_text(m)


@mcp.tool()
def dos_stop(session: str = "main", export_dir: str = "") -> str:
    """Power off. If export_dir is given, copy everything on D: there (files
    the program saved, logs, WAV renders...)."""
    m = _m(session)
    m.stop()
    msg = "stopped"
    if export_dir:
        n = m.export(export_dir)
        msg += f"; exported {n} files to {export_dir}"
    return msg


@mcp.tool()
def dos_files(session: str = "main") -> str:
    """List files on D: (machine must be stopped)."""
    return "\n".join(f"{p}  {n}" for p, n in _m(session).list_files())


@mcp.tool()
def dos_compare(a_json: str, b_json: str, masks: str = "", attrs: bool = True) -> str:
    """Compare two capture .json files cell by cell. masks: ';'-separated
    'row0,col0,row1,col1' rectangles to ignore (e.g. clocks)."""
    mk = [tuple(int(x) for x in r.split(",")) for r in masks.split(";") if r.strip()]
    d = capture.compare(capture.load(a_json), capture.load(b_json), mk, attrs)
    return d.report()


if __name__ == "__main__":
    mcp.run()
