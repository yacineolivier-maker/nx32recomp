"""Reporte les adresses absolues de build/manquantes.txt dans <profil>/<module>_graines.txt (profil : NX_PROFIL_JEU) (relatives au module).

Usage : python graines.py <dossier_build>
Ensuite : relancer analyse.py sur les modules concernés, puis gen.py.
"""
import json
import sys
from pathlib import Path

from profil import dossier_profil

build = Path(sys.argv[1])
cfg = dossier_profil()
L = json.load(open(build / "lien" / "layout.json"))
src = build / "manquantes.txt"
if not src.exists():
    sys.exit("rien à faire")
modifs = set()
for ligne in src.read_text().split():
    a = int(ligne, 16)
    for m in L["modules"]:
        if m["base"] <= a < m["base"] + m["taille"]:
            f = cfg / f"{m['nom']}_graines.txt"
            deja = set(f.read_text().split()) if f.exists() else set()
            rel = f"{a - m['base']:X}"
            if rel not in deja:
                with open(f, "a") as fh:
                    fh.write(rel + "\n")
                modifs.add(m["nom"])
                print(f"{m['nom']} + 0x{rel}")
src.unlink()
print("modules à réanalyser :", " ".join(sorted(modifs)))
