"""Test scripts: one command per line, run against any backend that offers
key / type / screen / screenshot (DosMachine does).

    # comment
    key f2                 press one key (see keys.py for names)
    keys ctrl-s alt-f12    press several keys in order
    type HELLO.IT          type literal text (rest of line, as-is)
    wait 500               sleep, milliseconds
    stable [ms]            wait until the screen is unchanged for ms (default 500)
    waittext <regex>       wait until the regex matches the screen text
    capture <name>         save <name>.json/.txt/.png into the output dir
    mask r0,c0,r1,c1       ignore this cell rectangle when comparing (all
                           captures; e.g. the clock). Not a runtime step.

Mouse (left button; cells are 8x8 logical pixels of the 640x400 screen,
a cell is targeted at its centre):
    click <col> <row>      move there, press, release
    clickpx <x> <y>        the same at a logical pixel
    drag <c0> <r0> <c1> <r1>   press at the first cell, move to the second
                           in steps with the button held, release
    dragpx <x0> <y0> <x1> <y1>
    mousemove <col> <row>  just move
    mousedown / mouseup    press / release where the pointer is
A script that uses the mouse gets the DOS mouse driver loaded, and the
pointer is parked bottom-right before every capture; cell (49,79), where
IT 2.14 then still draws a corner of it, is masked automatically.

A line prefixed with "@dos " or "@port " only runs on that target, for
the places where the two differ by design (IT 2.14's sound card dialog at
startup, for example):
    @dos waittext Continue
    @dos key enter

The same script file is meant to drive both the DOS original and a port,
so that the captures with the same name can be compared.
"""

from __future__ import annotations

import shlex
import time
from pathlib import Path

from . import capture


def parse(text: str, target: str = "dos") -> list[tuple[str, str]]:
    cmds = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@"):
            tgt, _, line = line[1:].partition(" ")
            if tgt != target:
                continue
            raw = line
        op, _, arg = line.partition(" ")
        op = op.lower()
        if op not in {"key", "keys", "type", "wait", "stable", "waittext", "capture",
                      "mask"} | MOUSE_OPS:
            raise ValueError(f"line {n}: unknown command {op!r}")
        # 'type' keeps its argument verbatim (after one separating space)
        cmds.append((op, raw.lstrip()[len(op) + 1:] if op == "type" else arg.strip()))
    return cmds


MOUSE_OPS = {"click", "clickpx", "drag", "dragpx", "mousemove", "mousedown",
             "mouseup"}
PARK_MASK = (49, 79, 49, 79)


def uses_mouse(text: str) -> bool:
    return any(op in MOUSE_OPS for op, _ in parse(text, "dos"))


def masks(text: str) -> list[tuple[int, int, int, int]]:
    m = [tuple(int(x) for x in arg.split(",")) for op, arg in parse(text)
         if op == "mask"]
    if uses_mouse(text):
        m.append(PARK_MASK)
    return m


def _cell(c: str, r: str) -> tuple[int, int]:
    return int(c) * 8 + 4, int(r) * 8 + 4


def run(machine, script_text: str, out_dir: str | Path, target: str = "dos",
        log=print) -> list[Path]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    captures = []
    mouse = uses_mouse(script_text)
    if mouse:
        machine.mouse_park()
    for op, arg in parse(script_text, target):
        log(f"> {op} {arg}")
        if op == "key":
            machine.key(arg)
        elif op == "keys":
            machine.keys(shlex.split(arg))
        elif op == "type":
            machine.type(arg)
        elif op == "wait":
            time.sleep(int(arg) / 1000)
        elif op == "stable":
            machine.wait_stable(quiet=(int(arg) if arg else 500) / 1000)
        elif op == "waittext":
            machine.wait_for_text(arg)
        elif op in ("click", "clickpx"):
            a = arg.split()
            x, y = _cell(*a) if op == "click" else (int(a[0]), int(a[1]))
            machine.mouse_to(x, y)
            machine.mouse_down()
            machine.mouse_up()
        elif op in ("drag", "dragpx"):
            a = arg.split()
            if op == "drag":
                (x0, y0), (x1, y1) = _cell(a[0], a[1]), _cell(a[2], a[3])
            else:
                x0, y0, x1, y1 = (int(v) for v in a)
            machine.mouse_to(x0, y0)
            machine.mouse_down()
            machine.mouse_to(x1, y1, home=False, steps=8)
            machine.mouse_up()
        elif op == "mousemove":
            machine.mouse_to(*_cell(*arg.split()))
        elif op == "mousedown":
            machine.mouse_down()
        elif op == "mouseup":
            machine.mouse_up()
        elif op == "capture":
            if mouse:
                machine.mouse_park()
            scr = machine.screen()
            captures.append(capture.save(scr, out / arg))
            machine.screenshot(out / f"{arg}.png")
    return captures
