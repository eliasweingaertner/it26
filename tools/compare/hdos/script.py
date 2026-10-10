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
                      "mask"}:
            raise ValueError(f"line {n}: unknown command {op!r}")
        # 'type' keeps its argument verbatim (after one separating space)
        cmds.append((op, raw.lstrip()[len(op) + 1:] if op == "type" else arg.strip()))
    return cmds


def masks(text: str) -> list[tuple[int, int, int, int]]:
    return [tuple(int(x) for x in arg.split(",")) for op, arg in parse(text)
            if op == "mask"]


def run(machine, script_text: str, out_dir: str | Path, target: str = "dos",
        log=print) -> list[Path]:
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    captures = []
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
        elif op == "capture":
            scr = machine.screen()
            captures.append(capture.save(scr, out / arg))
            machine.screenshot(out / f"{arg}.png")
    return captures
