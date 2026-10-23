"""Aide au diagnostic sur l'image liée.

  python diag.py str 0xADRESSE...      chaîne C à une adresse invitée
  python diag.py sym 0xADRESSE...      symbole connu le plus proche (fonction englobante)
  python diag.py pile <run.log>        symbolise les adresses de la pile d'appels d'un journal
"""
import bisect
import json
import re
import sys
from pathlib import Path

B = Path(__file__).resolve().parent.parent / "build"
L = json.load(open(B / "lien" / "layout.json"))
IMG = (B / "lien" / "image.bin").read_bytes()
BASE = L["base"]


def chaine(a):
    o = a - BASE
    fin = IMG.index(b"\0", o)
    return IMG[o:fin].decode("utf-8", "replace")


_syms = sorted((v[0], k) for k, v in L["symboles"].items())
_addrs = [a for a, _ in _syms]
_funcs = []
for m in L["modules"]:
    fj = B / f"{m['nom']}_funcs.json"
    if fj.exists():
        _funcs += [(m["base"] + f["start"], m["nom"]) for f in json.load(open(fj))]
_funcs.sort()
_faddrs = [a for a, _ in _funcs]


def symbole(a):
    i = bisect.bisect_right(_addrs, a) - 1
    j = bisect.bisect_right(_faddrs, a) - 1
    mod = next((m["nom"] for m in L["modules"] if m["base"] <= a < m["base"] + m["taille"]), "?")
    fn = f"f_{_faddrs[j]:08X}+0x{a - _faddrs[j]:X}" if j >= 0 else "?"
    s = f"{_syms[i][1]}+0x{a - _syms[i][0]:X}" if i >= 0 and a - _syms[i][0] < 0x4000 else ""
    return f"{a:08X} [{mod}] {fn} {s}"


if __name__ == "__main__":
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "str":
        for x in args:
            print(f"{x}: {chaine(int(x, 16))!r}")
    elif cmd == "sym":
        for x in args:
            print(symbole(int(x, 16)))
    elif cmd == "pile":
        txt = Path(args[0]).read_text(encoding="utf-8", errors="replace")
        for x in re.findall(r"(?:lr|pile|trace)\s*[:=]\s*([0-9A-F]{8})", txt):
            print(symbole(int(x, 16)))
