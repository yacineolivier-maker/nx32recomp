"""Dossier du profil du jeu (hle.txt, <module>_graines.txt) : variable NX_PROFIL_JEU, sinon profil/ à la racine du dépôt."""
import os
from pathlib import Path


def dossier_profil():
    return Path(os.environ.get("NX_PROFIL_JEU") or Path(__file__).resolve().parent.parent.parent / "profil")
