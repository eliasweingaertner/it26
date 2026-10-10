"""it26 vs IT 2.14 comparison harness (see tools/compare/README.md).

  python tools/compare/compare.py build
  python tools/compare/compare.py run --data C:/path/it214 --cmd "IT /S5" \
        --script tests/it_smoke.hds --out out/dos [--export out/dos-disk] [--vnc]
  python tools/compare/compare.py compare out/dos/main.json out/port/main.json \
        [--mask 9,62,9,79] [--no-attrs]
  python tools/compare/compare.py comparedir out/dos out/port [--mask ...]
  python tools/compare/compare.py compare-run --script tests/compare/it_screens.hds \
        --data <IT 2.14 folder, or env IT214_DIR> --out build-compare/it_screens
      runs the script on both sides and writes out/cmp/report.md
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

from hdos import DosMachine, capture, image, report, script
from hdos.port import PortMachine


REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_PORT = REPO / "build-hdos" / ("ited.exe" if os.name == "nt" else "ited")


def masks_arg(values):
    return [tuple(int(x) for x in v.split(",")) for v in values or []]


def cmd_build(a):
    for mem in ("ems", "xms", "none"):
        print(image.build_boot_image(mem, force=a.force))


def cmd_run(a):
    text = Path(a.script).read_text(encoding="utf-8") if a.script else ""
    out = Path(a.out)
    run_bat = "@ECHO OFF\n" + a.cmd + "\n" if a.cmd else None
    m = DosMachine(out / "_vm", data_dir=a.data, run_bat=run_bat,
                   memory=a.memory, sound=a.sound,
                   audio_wav=a.audio, display="vnc" if a.vnc else "none")
    try:
        m.start(timeout=120)
        if a.cmd:
            m.wait_stable(quiet=1.0, timeout=30)
        caps = script.run(m, text, out)
        print(f"{len(caps)} captures in {out}")
    finally:
        m.stop()
    if a.export:
        n = m.export(a.export)
        print(f"exported {n} files from D: to {a.export}")


def cmd_compare(a):
    d = capture.compare(capture.load(a.a), capture.load(a.b),
                        masks_arg(a.mask), attrs=not a.no_attrs)
    print(d.report())
    return 0 if d.equal else 1


def cmd_comparedir(a):
    da, db = Path(a.a), Path(a.b)
    worst = 0
    for ja in sorted(da.glob("*.json")):
        jb = db / ja.name
        if not jb.exists():
            print(f"[missing] {ja.stem}")
            worst = 1
            continue
        d = capture.compare(capture.load(ja), capture.load(jb),
                            masks_arg(a.mask), attrs=not a.no_attrs)
        print(f"[{'same' if d.equal else 'DIFF'}] {ja.stem}")
        if not d.equal:
            worst = 1
            print(d.report(max_rows=a.max_rows))
            pa, pb = ja.with_suffix(".png"), jb.with_suffix(".png")
            if pa.exists() and pb.exists():
                n = capture.diff_png(pa, pb, db / f"{ja.stem}.diff.png")
                print(f"  {n} pixels differ -> {db / (ja.stem + '.diff.png')}")
    return worst


def cmd_compare_run(a):
    if not a.data:
        sys.exit("compare-run: pass --data or set IT214_DIR to the IT 2.14 folder")
    text = Path(a.script).read_text(encoding="utf-8")
    out = Path(a.out)
    names = [arg for op, arg in script.parse(text, "dos") if op == "capture"]
    if a.report_only:
        a.dos_only = a.port_only = True
    if not a.port_only:
        shutil.rmtree(out / "dos", ignore_errors=True)
        m = DosMachine(out / "_vm", data_dir=a.data,
                       run_bat=f"@ECHO OFF\n{a.cmd}\n", memory=a.memory)
        try:
            m.start(timeout=120)
            m.wait_stable(quiet=1.0, timeout=30)
            script.run(m, text, out / "dos", "dos")
        finally:
            m.stop()
    if not a.dos_only:
        shutil.rmtree(out / "port", ignore_errors=True)
        # the port gets its own copy of the same files as D:
        cwd = out / "_port_cwd"
        shutil.rmtree(cwd, ignore_errors=True)
        shutil.copytree(a.data, cwd)
        p = PortMachine(a.port_exe, cwd)
        try:
            p.start()
            script.run(p, text, out / "port", "port")
        finally:
            p.stop()
    r = report.write(out, names, script.masks(text) + masks_arg(a.mask),
                     title=f"{Path(a.script).name}: IT 2.14 vs it26",
                     strict=a.strict)
    print(f"report: {r}")


def main(argv=None):
    p = argparse.ArgumentParser(prog="hdos")
    sub = p.add_subparsers(dest="cmd_name", required=True)

    b = sub.add_parser("build", help="build the FreeDOS boot images")
    b.add_argument("--force", action="store_true")
    b.set_defaults(fn=cmd_build)

    r = sub.add_parser("run", help="boot, run a program, play a script")
    r.add_argument("--data", help="host folder copied to D:")
    r.add_argument("--cmd", help="DOS command line run from D:\\ (e.g. 'IT /S5')")
    r.add_argument("--script", help="hds script file")
    r.add_argument("--out", required=True, help="capture output dir")
    r.add_argument("--export", help="copy D: back to this folder afterwards")
    r.add_argument("--memory", default="ems", choices=["ems", "xms", "none"])
    r.add_argument("--sound", default="sb16", help="sb16, gus, adlib or none")
    r.add_argument("--audio", help="record guest audio to this WAV file")
    r.add_argument("--vnc", action="store_true", help="watch on VNC :5900")
    r.set_defaults(fn=cmd_run)

    for name, fn in (("compare", cmd_compare), ("comparedir", cmd_comparedir)):
        c = sub.add_parser(name)
        c.add_argument("a")
        c.add_argument("b")
        c.add_argument("--mask", action="append",
                       help="row0,col0,row1,col1 to ignore (inclusive)")
        c.add_argument("--no-attrs", action="store_true")
        c.add_argument("--max-rows", type=int, default=20)
        c.set_defaults(fn=fn)

    cr = sub.add_parser("compare-run", help="run a script on DOS and the port, write report.md")
    cr.add_argument("--script", required=True)
    cr.add_argument("--data", default=os.environ.get("IT214_DIR"),
                    help="IT 2.14 folder (default: env IT214_DIR): D: on DOS, cwd for the port")
    cr.add_argument("--cmd", default="IT /S0")
    cr.add_argument("--port-exe", default=str(DEFAULT_PORT))
    cr.add_argument("--out", required=True)
    cr.add_argument("--memory", default="ems", choices=["ems", "xms", "none"])
    cr.add_argument("--mask", action="append")
    cr.add_argument("--dos-only", action="store_true", help="re-run DOS side only")
    cr.add_argument("--port-only", action="store_true", help="re-run port side only")
    cr.add_argument("--strict", action="store_true", help="00h and 20h differ")
    cr.add_argument("--report-only", action="store_true", help="just rebuild report.md")
    cr.set_defaults(fn=cmd_compare_run)

    a = p.parse_args(argv)
    return a.fn(a) or 0


if __name__ == "__main__":
    sys.exit(main())
