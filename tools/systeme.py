"""Extrait les archives de données système (RomFS) d'une NAND (la vôtre) : polices partagées, NgWord, fuseaux…

Usage : python systeme.py [dataId_hex ...]     (sans argument : liste les titres disponibles)
Sortie : $NX_DONNEES/systeme/<DATAID>.romfs (RomFS brut, servi par fsp-srv OpenDataStorageByDataId).
Variables : NX_NAND (dossier system/Contents/registered ; défaut : NAND d'Eden dans %APPDATA%), NX_HACTOOL
(hactool, défaut : PATH), NX_CLES (défaut : ~/.switch/prod.keys), NX_DONNEES (défaut : donnees).
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HACTOOL = os.environ.get("NX_HACTOOL") or shutil.which("hactool") or sys.exit("hactool introuvable (NX_HACTOOL ou PATH)")
NAND = Path(os.environ.get("NX_NAND") or Path(os.environ.get("APPDATA", "")) / "eden" / "nand" / "system" / "Contents" / "registered")
SORTIE = Path(os.environ.get("NX_DONNEES") or "donnees") / "systeme"
CLES = Path(os.environ.get("NX_CLES") or Path.home() / ".switch" / "prod.keys")


def hactool(*args):
    return subprocess.run([str(HACTOOL), "-k", str(CLES), *args], capture_output=True, text=True, errors="replace")


def titres():
    """Associe title ID -> fichier NCA (contenu de type Data / PublicData)."""
    out = {}
    for nca in sorted(NAND.glob("*.nca")):
        if nca.name.endswith(".cnmt.nca"):
            continue
        r = hactool(str(nca))
        tid = re.search(r"Title ID:\s+([0-9a-fA-F]{16})", r.stdout)
        typ = re.search(r"Content Type:\s+(\w+)", r.stdout)
        if tid and typ:
            out.setdefault(tid.group(1).upper(), []).append((typ.group(1), nca))
    return out


if __name__ == "__main__":
    t = titres()
    if len(sys.argv) == 1:
        for tid, l in sorted(t.items()):
            print(tid, ", ".join(f"{ty}:{p.name}" for ty, p in l))
        sys.exit()
    SORTIE.mkdir(parents=True, exist_ok=True)
    for x in sys.argv[1:]:
        tid = x.upper().rjust(16, "0")
        cands = [p for ty, p in t.get(tid, []) if ty in ("Data", "PublicData", "Program")]
        if not cands:
            print(f"{tid} : absent de la NAND")
            continue
        dst = SORTIE / f"{tid}.romfs"
        r = hactool(f"--romfs={dst}", str(cands[0]))
        print(f"{tid} -> {dst} ({dst.stat().st_size if dst.exists() else 0} octets)")
