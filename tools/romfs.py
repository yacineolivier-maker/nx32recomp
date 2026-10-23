"""Lecture d'une image RomFS brute (en-tête 0x50) : liste et extraction de fichiers.

Usage : python romfs.py liste <image.romfs>
        python romfs.py extrait <image.romfs> <dossier>
"""
import struct
import sys
from pathlib import Path


def fichiers(chemin):
    """Renvoie [(nom, offset_absolu, taille)] en parcourant la table des métadonnées de fichiers."""
    with open(chemin, "rb") as f:
        h = struct.unpack("<10Q", f.read(0x50))
        hdr_size, dh_off, dh_size, dm_off, dm_size, fh_off, fh_size, fm_off, fm_size, data_off = h
        f.seek(fm_off)
        meta = f.read(fm_size)
    out = []
    o = 0
    while o + 0x20 <= len(meta):
        parent, sibling, d_off, size, hsh, nlen = struct.unpack_from("<IIQQII", meta, o)
        nom = meta[o + 0x20:o + 0x20 + nlen].decode("utf-8", "replace")
        out.append((nom, data_off + d_off, size))
        o += 0x20 + ((nlen + 3) & ~3)
    return out


if __name__ == "__main__":
    cmd, img = sys.argv[1], sys.argv[2]
    if cmd == "liste":
        for n, off, s in fichiers(img):
            print(f"{s:10} {n}")
    else:
        dst = Path(sys.argv[3])
        dst.mkdir(parents=True, exist_ok=True)
        with open(img, "rb") as f:
            for n, off, s in fichiers(img):
                f.seek(off)
                (dst / n).write_bytes(f.read(s))
                print(f"{n} ({s} octets)")
