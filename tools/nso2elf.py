"""Convertit un NSO 32 bits (AArch32) en ELF32 ARM chargeable par Ghidra/objdump.

Usage : python nso2elf.py <entree.nso> <sortie.elf> [--base 0xADRESSE]

L'ELF a un PT_LOAD par segment (.text RX, .ro R, .data+.bss RW) et un PT_DYNAMIC : Ghidra
retrouve ainsi symboles dynamiques et relocations sans en-têtes de sections.
"""
import argparse
import struct

from nso import Nso

PF_X, PF_W, PF_R = 1, 2, 4


def build(n: Nso, base: int) -> bytes:
    assert n.is32, "seuls les modules AArch32 sont gérés"
    segs = []
    for name, flags in (("text", PF_R | PF_X), ("ro", PF_R), ("data", PF_R | PF_W)):
        moff, size, blob = n.seg[name]
        memsz = size
        if name == "data":
            bss_end = n.mod0["bss_end"] if n.mod0 else moff + size + n.bss_size
            memsz = bss_end - moff
        segs.append((moff, blob, memsz, flags))

    nph = len(segs) + 1
    ehsize, phsize = 52, 32
    data_off = (ehsize + phsize * nph + 0xFFF) & ~0xFFF
    out = bytearray(data_off)
    phdrs = []
    for moff, blob, memsz, flags in segs:
        foff = len(out)
        out += blob
        out += b"\0" * (-len(out) % 0x1000)
        phdrs.append(struct.pack("<8I", 1, foff, base + moff, base + moff, len(blob), memsz, flags, 0x1000))

    # PT_DYNAMIC : taille = entrées jusqu'à DT_NULL incluse
    dyn = n.mod0["dynamic"]
    cnt = 0
    while struct.unpack_from("<i", n.image, dyn + cnt * 8)[0] != 0:
        cnt += 1
    dyn_size = (cnt + 1) * 8
    data_moff = n.seg["data"][0]
    dyn_foff = struct.unpack_from("<I", phdrs[2], 4)[0] + (dyn - data_moff) if dyn >= data_moff else \
        struct.unpack_from("<I", phdrs[1], 4)[0] + (dyn - n.seg["ro"][0])
    phdrs.append(struct.pack("<8I", 2, dyn_foff, base + dyn, base + dyn, dyn_size, dyn_size, PF_R | PF_W, 4))

    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    # e_type ET_DYN, e_machine EM_ARM (40), e_flags EABI v5 + hard-float
    eh = ident + struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, base, ehsize, 0, 0x05000400,
                             ehsize, phsize, nph, 40, 0, 0)
    out[0:ehsize] = eh
    out[ehsize:ehsize + phsize * nph] = b"".join(phdrs)
    return bytes(out)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0)
    a = ap.parse_args()
    data = build(Nso(a.src), a.base)
    open(a.dst, "wb").write(data)
    print(f"{a.dst} : {len(data)} octets")
