"""Édition de liens statique des modules NSO : image mémoire unique de l'invité.

Usage : python link.py <dossier_exefs> <sortie_dir>

Place les modules dans l'ordre de Horizon (rtld, main, subsdk*, sdk) à partir de BASE, applique les
relocations (REL, addend implicite) et résout les symboles dans la portée globale (ordre de chargement).
Produit : image.bin (mémoire de BASE à la fin du dernier .bss), layout.json (modules, symboles).
"""
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "recomp"))
from analyse import Module  # noqa: E402

BASE = 0x00200000
ALIGN = 0x10000
ORDRE = ["rtld", "main"] + [f"subsdk{i}" for i in range(10)] + ["sdk"]  # modules absents ignorés
R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE = 2, 21, 22, 23
STB_WEAK = 2


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    mods = []
    adr = BASE
    for nom in ORDRE:
        p = src / nom
        if not p.exists():
            continue
        m = Module(str(p))
        fin = m.nso.mod0["bss_end"]
        mods.append(dict(nom=nom, m=m, base=adr, taille=fin))
        adr = (adr + fin + ALIGN - 1) & ~(ALIGN - 1)
    image = bytearray(adr - BASE)
    for d in mods:
        img = d["m"].img
        o = d["base"] - BASE
        image[o:o + len(img)] = img

    # portée globale : premier module définissant le symbole ; un fort l'emporte sur un faible
    globaux = {}
    for d in mods:
        for s in d["m"].nso.symbols():
            if not s["defined"]:
                continue
            prev = globaux.get(s["name"])
            if prev is None or (prev[1] == STB_WEAK and s["bind"] != STB_WEAK):
                globaux[s["name"]] = (d["base"] + s["value"], s["bind"], d["nom"], s["type"])

    non_resolus = {}
    for d in mods:
        m, b = d["m"], d["base"]
        syms = m.nso.symbols_all() if hasattr(m.nso, "symbols_all") else None
        table = dynsym_table(m)
        for where, typ, isym in m.relocations():
            P = b + where - BASE
            val = struct.unpack_from("<I", image, P)[0]
            if typ == R_ARM_RELATIVE:
                nv = val + b
            else:
                nom, defini, valeur = table[isym]
                if nom in globaux:
                    S = globaux[nom][0]
                elif defini:
                    S = b + valeur
                else:
                    non_resolus.setdefault(nom, []).append(d["nom"])
                    S = 0
                nv = (val + S) if typ == R_ARM_ABS32 else S
            struct.pack_into("<I", image, P, nv & 0xFFFFFFFF)

    (out / "image.bin").write_bytes(image)
    layout = dict(base=BASE, fin=adr, modules=[
        dict(nom=d["nom"], module=d["m"].nso.module_name, base=d["base"], taille=d["taille"],
             text=[d["base"] + d["m"].nso.seg["text"][0], d["m"].nso.seg["text"][1]],
             init=d["m"].dyn.get(12, [None])[0], init_array=d["m"].dyn.get(25, [None])[0],
             init_arraysz=d["m"].dyn.get(27, [0])[0]) for d in mods],
        symboles={k: [v[0], v[2]] for k, v in globaux.items() if v[3] == 2},
        non_resolus=sorted(non_resolus))
    json.dump(layout, open(out / "layout.json", "w"), indent=1)
    for d in mods:
        print(f"{d['nom']:9} 0x{d['base']:08X}-0x{d['base'] + d['taille']:08X} {d['m'].nso.module_name[-24:]}")
    print(f"image : 0x{BASE:08X}-0x{adr:08X} ({len(image) / 1048576:.1f} Mo), symboles globaux : {len(globaux)}")
    print(f"non résolus : {len(non_resolus)} {sorted(non_resolus)[:20]}")


def dynsym_table(m):
    """Index de symbole -> (nom, défini, valeur) à partir de la table dynamique brute (Elf32_Sym)."""
    n = m.nso
    ro = n.seg["ro"][0]
    str_off, str_size = n.dynstr
    sym_off, sym_size = n.dynsym
    strtab = n.image[ro + str_off:ro + str_off + str_size]
    out = []
    for i in range(sym_size // 16):
        name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", n.image, ro + sym_off + i * 16)
        s = strtab[name:strtab.index(b"\0", name)].decode("utf-8", "replace") if name < len(strtab) else ""
        out.append((s, shndx != 0, value))
    return out


if __name__ == "__main__":
    main()
