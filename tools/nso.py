"""Outils NSO (exécutables Switch) : info, décompression en image plate, symboles dynamiques.

Usage :
  python nso.py info  <fichier.nso>
  python nso.py dump  <fichier.nso> <sortie.bin>     # image mémoire plate + <sortie>.json (layout)
  python nso.py syms  <fichier.nso> [--undef|--def]  # liste des symboles dynamiques
"""
import json
import struct
import sys

import lz4.block

SEGS = ("text", "ro", "data")


class Nso:
    def __init__(self, path):
        raw = open(path, "rb").read()
        if raw[:4] != b"NSO0":
            raise ValueError(f"{path}: pas un NSO")
        self.flags = struct.unpack_from("<I", raw, 0xC)[0]
        self.seg = {}
        file_sizes = struct.unpack_from("<3I", raw, 0x60)
        for i, name in enumerate(SEGS):
            foff, moff, size = struct.unpack_from("<3I", raw, 0x10 + i * 0x10)
            blob = raw[foff:foff + file_sizes[i]]
            if self.flags & (1 << i):
                blob = lz4.block.decompress(blob, uncompressed_size=size)
            assert len(blob) == size, (name, len(blob), size)
            self.seg[name] = (moff, size, blob)
        self.bss_size = struct.unpack_from("<I", raw, 0x3C)[0]
        self.module_id = raw[0x40:0x60].rstrip(b"\0").hex()
        self.dynstr = struct.unpack_from("<2I", raw, 0x90)
        self.dynsym = struct.unpack_from("<2I", raw, 0x98)

        end = max(m + s for m, s, _ in self.seg.values())
        img = bytearray(end)
        for m, s, b in self.seg.values():
            img[m:m + s] = b
        self.image = bytes(img)

        # architecture : la section .dynamic contient des Elf32_Dyn (8 octets) ou Elf64_Dyn (16)
        self.is32 = False
        mod0 = struct.unpack_from("<I", self.image, 4)[0]
        self.mod0 = None
        if self.image[mod0:mod0 + 4] == b"MOD0":
            dyn, bss_s, bss_e = struct.unpack_from("<3i", self.image, mod0 + 4)
            self.mod0 = dict(offset=mod0, dynamic=mod0 + dyn, bss_start=mod0 + bss_s, bss_end=mod0 + bss_e)
            # en 64 bits, le 2e mot de chaque entrée Elf64_Dyn (moitié haute du tag) est nul
            d = mod0 + dyn
            tags64 = [struct.unpack_from("<Q", self.image, d + i * 16)[0] for i in range(4)]
            self.is32 = not all(t < 0x100 for t in tags64)

        # nom du module (chaîne en tête de .ro : u32 0, u32 len, chemin)
        ro_off = self.seg["ro"][0]
        zero, ln = struct.unpack_from("<2I", self.image, ro_off)
        self.module_name = ""
        if zero == 0 and 0 < ln < 0x200:
            self.module_name = self.image[ro_off + 8:ro_off + 8 + ln].split(b"\0")[0].decode("utf-8", "replace")

    def symbols(self):
        ro_off = self.seg["ro"][0]
        str_off, str_size = self.dynstr
        sym_off, sym_size = self.dynsym
        strtab = self.image[ro_off + str_off:ro_off + str_off + str_size]
        out = []
        # Elf32_Sym (16 octets) pour les modules 32 bits, Elf64_Sym (24 octets) sinon
        for i in range(sym_size // (16 if self.is32 else 24)):
            if self.is32:
                name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", self.image, ro_off + sym_off + i * 16)
            else:
                name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.image, ro_off + sym_off + i * 24)
            if name >= len(strtab):
                continue
            s = strtab[name:strtab.index(b"\0", name)].decode("utf-8", "replace")
            if s:
                out.append(dict(name=s, value=value, size=size, type=info & 0xF, bind=info >> 4, defined=shndx != 0))
        return out


def cmd_info(path):
    n = Nso(path)
    print(f"fichier   : {path}")
    print(f"module    : {n.module_name or '?'}")
    print(f"module_id : {n.module_id}")
    print(f"arch      : {'AArch32' if n.is32 else 'AArch64'}")
    for name in SEGS:
        m, s, _ = n.seg[name]
        print(f".{name:<5} : 0x{m:08X}  taille 0x{s:08X} ({s / 1048576:.2f} Mo)")
    print(f".bss    : taille 0x{n.bss_size:08X}")
    if n.mod0:
        print("MOD0      : " + ", ".join(f"{k}=0x{v:X}" for k, v in n.mod0.items()))
    syms = n.symbols()
    print(f"symboles  : {len(syms)} ({sum(s['defined'] for s in syms)} définis, {sum(not s['defined'] for s in syms)} importés)")


def cmd_dump(path, out):
    n = Nso(path)
    open(out, "wb").write(n.image)
    layout = dict(module=n.module_name, arch="AArch32" if n.is32 else "AArch64", module_id=n.module_id, bss_size=n.bss_size, mod0=n.mod0,
                  segments={k: dict(offset=m, size=s) for k, (m, s, _) in n.seg.items()})
    json.dump(layout, open(out + ".json", "w"), indent=2)
    print(f"{out} : {len(n.image)} octets")


def cmd_syms(path, flt=None):
    for s in Nso(path).symbols():
        if flt == "--undef" and s["defined"] or flt == "--def" and not s["defined"]:
            continue
        print(f"{'D' if s['defined'] else 'U'} 0x{s['value']:010X} {s['name']}")


if __name__ == "__main__":
    a = sys.argv[1:]
    {"info": lambda: cmd_info(a[1]), "dump": lambda: cmd_dump(a[1], a[2]),
     "syms": lambda: cmd_syms(a[1], a[2] if len(a) > 2 else None)}[a[0]]()
