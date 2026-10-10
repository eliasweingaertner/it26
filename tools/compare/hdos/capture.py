"""Screen captures as files, and cell-by-cell comparison of two captures.

A capture is <name>.json:
    {"cols": 80, "rows": 50, "cursor": [row, col] | [-1, -1],
     "chars": "<hex, rows*cols bytes, code page 437>",
     "attrs": "<hex, rows*cols attribute bytes>"}
plus <name>.txt (readable text) and, when available, <name>.png.

Any program that can produce this JSON (e.g. a port with a dump hook) can be
compared against the DOS original with `compare`.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

from .machine import CP437, TextScreen


def save(screen: TextScreen, path_stem: str | Path) -> Path:
    stem = Path(path_stem)
    stem.parent.mkdir(parents=True, exist_ok=True)
    data = {"cols": screen.cols, "rows": screen.rows,
            "cursor": list(screen.cursor),
            "chars": screen.chars.hex(), "attrs": screen.attrs.hex()}
    stem.with_suffix(".json").write_text(json.dumps(data))
    stem.with_suffix(".txt").write_text(screen.text() + "\n", encoding="utf-8")
    return stem.with_suffix(".json")


def load(path: str | Path) -> TextScreen:
    d = json.loads(Path(path).read_text())
    return TextScreen(3, d["cols"], d["rows"], tuple(d.get("cursor", (-1, -1))),
                      bytes.fromhex(d["chars"]), bytes.fromhex(d["attrs"]))


@dataclass
class Diff:
    a: TextScreen
    b: TextScreen
    cells: list[tuple[int, int, str]] = field(default_factory=list)  # row, col, what
    geometry: str = ""

    @property
    def equal(self) -> bool:
        return not self.cells and not self.geometry

    def report(self, max_rows: int = 50) -> str:
        if self.geometry:
            return f"geometry differs: {self.geometry}"
        if not self.cells:
            return "identical"
        rows = sorted({r for r, _, _ in self.cells})
        nch = sum(1 for c in self.cells if "char" in c[2])
        nat = sum(1 for c in self.cells if "attr" in c[2])
        out = [f"{len(self.cells)} cells differ ({nch} char, {nat} attr) "
               f"in {len(rows)} rows"]
        w = self.a.cols
        for r in rows[:max_rows]:
            la = "".join(CP437[x] for x in self.a.chars[r * w:(r + 1) * w])
            lb = "".join(CP437[x] for x in self.b.chars[r * w:(r + 1) * w])
            marks = [" "] * w
            for rr, c, what in self.cells:
                if rr == r:
                    marks[c] = "^" if "char" in what else "~"
            out.append(f"row {r:2d} A|{la}|")
            out.append(f"       B|{lb}|")
            out.append(f"         {''.join(marks)}")
        if len(rows) > max_rows:
            out.append(f"... {len(rows) - max_rows} more rows")
        return "\n".join(out)


def compare(a: TextScreen, b: TextScreen,
            masks: list[tuple[int, int, int, int]] = (),
            attrs: bool = True, blank_equiv: bool = True) -> Diff:
    """Compare two captures. masks: (row0, col0, row1, col1) inclusive
    rectangles to ignore (clocks, dates, free memory, ...). blank_equiv
    treats character 00h and 20h as the same (both draw as blank; IT
    clears with either, depending on the code path)."""
    d = Diff(a, b)
    if (a.cols, a.rows) != (b.cols, b.rows):
        d.geometry = f"{a.cols}x{a.rows} vs {b.cols}x{b.rows}"
        return d

    def masked(r, c):
        return any(r0 <= r <= r1 and c0 <= c <= c1 for r0, c0, r1, c1 in masks)

    for i in range(a.cols * a.rows):
        r, c = divmod(i, a.cols)
        if masked(r, c):
            continue
        what = []
        ca, cb = a.chars[i], b.chars[i]
        if blank_equiv:
            ca, cb = ca or 0x20, cb or 0x20
        if ca != cb:
            what.append("char")
        if attrs and a.attrs[i] != b.attrs[i]:
            what.append("attr")
        if what:
            d.cells.append((r, c, "+".join(what)))
    return d


def diff_png(png_a: str | Path, png_b: str | Path, out: str | Path) -> int:
    """Write an image highlighting differing pixels; returns their count.
    Images of different sizes are compared over the common area."""
    from PIL import Image, ImageChops
    ia = Image.open(png_a).convert("RGB")
    ib = Image.open(png_b).convert("RGB")
    w, h = min(ia.width, ib.width), min(ia.height, ib.height)
    ia, ib = ia.crop((0, 0, w, h)), ib.crop((0, 0, w, h))
    delta = ImageChops.difference(ia, ib).convert("L").point(lambda v: 255 if v > 8 else 0)
    n = sum(1 for v in delta.getdata() if v)
    red = Image.new("RGB", (w, h), (255, 0, 0))
    vis = Image.composite(red, ia.point(lambda v: v // 3), delta)
    vis.save(out)
    return n


def crop_pair(png_a: str | Path, png_b: str | Path, out: str | Path,
              rows: tuple[int, int], cols: tuple[int, int] = (0, 79),
              labels: tuple[str, str] = ("IT 2.14", "it26"), scale: int = 2,
              cell: tuple[int, int] = (8, 8)) -> Path:
    """Side-by-side crop of the same cell rectangle from two screenshots
    (rows/cols inclusive), for findings and issue reports."""
    from PIL import Image, ImageDraw
    cw, ch = cell
    box = (cols[0] * cw, rows[0] * ch, (cols[1] + 1) * cw, (rows[1] + 1) * ch)
    parts = [Image.open(p).convert("RGB").crop(box) for p in (png_a, png_b)]
    w, h = parts[0].width * scale, parts[0].height * scale
    label_h, gap = 18, 12
    img = Image.new("RGB", (w * 2 + gap, h + label_h), (40, 40, 40))
    d = ImageDraw.Draw(img)
    for i, (p, lab) in enumerate(zip(parts, labels)):
        x = i * (w + gap)
        img.paste(p.resize((w, h), Image.NEAREST), (x, label_h))
        d.text((x + 4, 3), lab, fill=(255, 255, 255))
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    return out
