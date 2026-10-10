"""report.md: DOS original vs port, one section per capture, side by side.

Each section ends with a Crit block for review notes:

    <!-- crit:01_load_module -->
    ...your notes...
    <!-- /crit:01_load_module -->

Regenerating the report keeps whatever is written between those markers.
"""

from __future__ import annotations

import re
from pathlib import Path

from . import capture

CRIT_RX = re.compile(r"<!-- crit:(\S+) -->\n(.*?)<!-- /crit:\1 -->", re.S)
EMPTY_CRIT = "**Crit:** \n"


def write(out_dir: str | Path, names: list[str], masks=(), title: str = "",
          strict: bool = False,
          dos_label: str = "IT 2.14 (DOS/QEMU)", port_label: str = "it26 port") -> Path:
    out = Path(out_dir)
    dos, port = out / "dos", out / "port"
    path = out / "report.md"
    old = {}
    if path.exists():
        old = dict(CRIT_RX.findall(path.read_text(encoding="utf-8")))

    rows, body = [], []
    for name in names:
        ja, jb = dos / f"{name}.json", port / f"{name}.json"
        if not (ja.exists() and jb.exists()):
            status, detail, npx = "missing", "capture missing on one side", None
        else:
            d = capture.compare(capture.load(ja), capture.load(jb), masks,
                                blank_equiv=not strict)
            status = "same" if d.equal else "DIFF"
            detail = d.report(max_rows=60)
            npx = None
            pa, pb = ja.with_suffix(".png"), jb.with_suffix(".png")
            if pa.exists() and pb.exists():
                npx = capture.diff_png(pa, pb, out / f"{name}.diff.png")
        ncells = "" if status != "DIFF" else detail.split(" cells")[0]
        rows.append(f"| [{name}](#{name}) | {status} | {ncells} | "
                    f"{'' if npx is None else npx} | "
                    f"{'✍' if old.get(name, EMPTY_CRIT).strip() not in ('', EMPTY_CRIT.strip()) else ''} |")

        body.append(f'<a id="{name}"></a>\n## {name} — {status}\n')
        body.append('<table><tr>'
                    f'<th>{dos_label}</th><th>{port_label}</th></tr><tr>'
                    f'<td><img src="dos/{name}.png" width="480"></td>'
                    f'<td><img src="port/{name}.png" width="480"></td>'
                    '</tr></table>\n')
        if status == "DIFF":
            if npx is not None:
                body.append(f'<details><summary>pixel diff ({npx} px)</summary>\n\n'
                            f'<img src="{name}.diff.png" width="640">\n\n</details>\n')
            body.append(f"<details><summary>cell diff</summary>\n\n```\n{detail}\n```\n\n</details>\n")
        body.append(f"<!-- crit:{name} -->\n{old.get(name, EMPTY_CRIT)}<!-- /crit:{name} -->\n")

    mask_txt = ", ".join(f"({a},{b})-({c},{d})" for a, b, c, d in masks) or "none"
    head = [f"# {title or 'IT 2.14 vs it26'}\n",
            f"Masked cells (row,col): {mask_txt}. "
            f"{'Strict: 00h and 20h differ.' if strict else 'Characters 00h and 20h count as equal (both blank).'} Rows marked A = DOS, B = port; "
            "`^` = character differs, `~` = colour only.\n",
            "Write review notes after **Crit:** in each section; regenerating "
            "keeps them.\n",
            "| capture | result | cells | pixels | crit |", "|---|---|---|---|---|",
            *rows, ""]
    path.write_text("\n".join(head) + "\n" + "\n".join(body), encoding="utf-8")
    return path
