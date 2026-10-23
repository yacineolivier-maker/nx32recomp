"""Mesure la couverture du traducteur sur les mots d'instruction uniques des fonctions découvertes.

Usage : python couverture.py <module.nso> <funcs.json> [--mots sortie.json]
"""
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from nso import Nso  # noqa: E402
from lifter import Lifter, Unsupported  # noqa: E402

try:
    from vfp import Vfp
except ImportError:
    Vfp = None


def mots_uniques(img, funcs):
    """Mots des fonctions découvertes (hors tables de sauts), avec une adresse exemple et un compte."""
    tables = set()
    for f in funcs:
        for base, n, _ in f["jtabs"].values():
            tables.update(range(base, base + 4 * n, 4))
    mots = {}
    vus = set()
    for f in funcs:
        for a in (a for r0, r1 in f["runs"] for a in range(r0, r1, 4)):
            if a in tables or a in vus:
                continue
            vus.add(a)
            w = struct.unpack_from("<I", img, a)[0]
            e = mots.setdefault(w, [a, 0])
            e[1] += 1
    return mots


def main():
    n = Nso(sys.argv[1])
    funcs = json.load(open(sys.argv[2]))
    mots = mots_uniques(n.image, funcs)
    from neon import Neon
    lf = Lifter(vfp=Vfp(neon=Neon()))
    raisons = Counter()
    exemples = {}
    ok = total = 0
    for w, (a, cnt) in mots.items():
        total += cnt
        try:
            lf.lift(w, a)
            ok += cnt
        except Unsupported as e:
            k = str(e)
            raisons[k] += cnt
            exemples.setdefault(k, (w, a))
        except Exception as e:  # bug du traducteur
            k = f"BUG {type(e).__name__}: {e}"
            raisons[k] += cnt
            exemples.setdefault(k, (w, a))
    print(f"mots uniques : {len(mots)} ; instructions couvertes : {ok}/{total} ({ok / total:.2%})")
    for k, v in raisons.most_common(40):
        w, a = exemples[k]
        print(f"{v:8} {k}   ex: {w:08X} @0x{a:X}")
    if "--mots" in sys.argv:
        json.dump({f"{w:08X}": v for w, v in mots.items()}, open(sys.argv[sys.argv.index("--mots") + 1], "w"))


if __name__ == "__main__":
    main()
