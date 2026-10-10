"""Key names -> QEMU qcodes (US keyboard layout, which is what FreeDOS uses
without KEYB).

A key spec is one key with optional modifiers joined by '-' or '+':
    "f2"  "esc"  "enter"  "ctrl-s"  "alt-f12"  "shift-tab"  "ctrl+alt+del"
A single printable character also works: "a", "A" (adds shift), "#".
"""

from __future__ import annotations

MODIFIERS = {
    "ctrl": "ctrl", "control": "ctrl", "lctrl": "ctrl", "rctrl": "ctrl_r",
    "alt": "alt", "lalt": "alt", "ralt": "alt_r", "altgr": "alt_r",
    "shift": "shift", "lshift": "shift", "rshift": "shift_r",
}

ALIASES = {
    "enter": "ret", "return": "ret", "escape": "esc", "space": "spc",
    "bksp": "backspace", "bs": "backspace", "del": "delete", "ins": "insert",
    "pageup": "pgup", "pagedown": "pgdn", "pgdown": "pgdn",
    "capslock": "caps_lock", "numlock": "num_lock", "scrolllock": "scroll_lock",
    "kpenter": "kp_enter", "kp+": "kp_add", "kp-": "kp_subtract",
    "kp*": "kp_multiply", "kp/": "kp_divide", "kp.": "kp_decimal",
    "printscreen": "print", "prtsc": "print", "pause": "pause",
}

NAMED = {
    "ret", "esc", "spc", "tab", "backspace", "up", "down", "left", "right",
    "pgup", "pgdn", "home", "end", "insert", "delete", "caps_lock",
    "num_lock", "scroll_lock", "kp_enter", "kp_add", "kp_subtract",
    "kp_multiply", "kp_divide", "kp_decimal", "print", "pause", "less",
    "minus", "equal", "bracket_left", "bracket_right", "semicolon",
    "apostrophe", "grave_accent", "backslash", "comma", "dot", "slash",
    "ctrl", "ctrl_r", "alt", "alt_r", "shift", "shift_r", "sysrq",
} | {f"f{i}" for i in range(1, 13)} | {f"kp_{i}" for i in range(10)}

# printable char -> (qcode, needs_shift)
CHARS: dict[str, tuple[str, bool]] = {" ": ("spc", False), "\n": ("ret", False),
                                      "\t": ("tab", False)}
for c in "abcdefghijklmnopqrstuvwxyz":
    CHARS[c] = (c, False)
    CHARS[c.upper()] = (c, True)
for c in "0123456789":
    CHARS[c] = (c, False)
for plain, shifted, q in [
    ("-", "_", "minus"), ("=", "+", "equal"), ("[", "{", "bracket_left"),
    ("]", "}", "bracket_right"), (";", ":", "semicolon"),
    ("'", '"', "apostrophe"), ("`", "~", "grave_accent"),
    ("\\", "|", "backslash"), (",", "<", "comma"), (".", ">", "dot"),
    ("/", "?", "slash"),
]:
    CHARS[plain] = (q, False)
    CHARS[shifted] = (q, True)
for d, s in zip("1234567890", "!@#$%^&*()"):
    CHARS[s] = (d, True)


def parse_key(spec: str) -> list[str]:
    """Return the qcodes to hold down together for one key spec."""
    if len(spec) == 1:
        q, sh = CHARS[spec]
        return ["shift", q] if sh else [q]
    if spec.lower() in ALIASES:            # "kp+", "kp-", ...
        return [ALIASES[spec.lower()]]
    parts = spec.split("-") if "-" in spec[:-1] else spec.split("+")
    if spec.endswith("--"):                 # "ctrl--" -> ctrl + '-'
        parts = spec[:-2].split("-") + ["-"]
    mods, out = [], []
    for i, p in enumerate(parts):
        low = p.lower()
        last = i == len(parts) - 1
        if not last and low in MODIFIERS:
            mods.append(MODIFIERS[low])
            continue
        if not last:
            raise ValueError(f"unknown modifier {p!r} in {spec!r}")
        if len(p) == 1:
            q, sh = CHARS[p]
            if sh and "shift" not in mods:
                mods.append("shift")
            out.append(q)
        else:
            q = ALIASES.get(low, low)
            if q not in NAMED and not (len(q) == 1 and q.isalnum()):
                raise ValueError(f"unknown key {p!r} in {spec!r}")
            out.append(q)
    return mods + out


def text_to_keys(text: str) -> list[list[str]]:
    return [parse_key(c) for c in text]
