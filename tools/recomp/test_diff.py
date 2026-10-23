"""Tests différentiels du traducteur : chaque instruction traduite en C est comparée à Unicorn.

Usage : python test_diff.py <module.nso> <mots.json> [--max N] [--etats K] [--filtre HEX...] [--graine S]

Seules les instructions sans effet sur le flot de contrôle sont testées ici (les branchements le
seront au niveau des fonctions). Les instructions qui font fauter Unicorn (accès hors fenêtre) sont
ignorées pour l'état concerné.
"""
import argparse
import ctypes
import json
import os
import random
import shutil
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_EXEC, UC_PROT_READ, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_C13_C0_3, UC_ARM_REG_CPSR, UC_ARM_REG_D0,
                               UC_ARM_REG_FPEXC, UC_ARM_REG_FPSCR, UC_CPU_ARM_MAX)
import unicorn.arm_const as A
UREGS = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR]

ICI = Path(__file__).resolve().parent
RACINE = ICI.parent.parent
sys.path.insert(0, str(ICI.parent))
sys.path.insert(0, str(ICI))
from nso import Nso  # noqa: E402
from lifter import Lifter, Unsupported  # noqa: E402
from vfp import Vfp  # noqa: E402

try:
    from neon import Neon
except ImportError:
    Neon = None

FENETRE, TAILLE_FEN = 0x10000000, 0x20000
# clang : NX_LLVM (dossier bin de LLVM), sinon celui du PATH ; vcvars64.bat : NX_VCVARS
LLVM = Path(os.environ.get("NX_LLVM") or Path(shutil.which("clang") or "clang").parent)
VCVARS = os.environ.get("NX_VCVARS",
                        r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat")


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32),
                ("c", ctypes.c_uint32), ("v", ctypes.c_uint32), ("q", ctypes.c_uint32), ("ge", ctypes.c_uint32),
                ("vb", ctypes.c_uint8 * 256), ("fpscr", ctypes.c_uint32), ("fn", ctypes.c_uint32),
                ("fz", ctypes.c_uint32), ("fc", ctypes.c_uint32), ("fv", ctypes.c_uint32),
                ("tpidruro", ctypes.c_uint32), ("excl_addr", ctypes.c_uint32), ("excl_valid", ctypes.c_uint32),
                ("tpidrurw", ctypes.c_uint32), ("excl_seq", ctypes.c_uint32), ("excl_val", ctypes.c_uint64)]


def est_flot(c_code):
    return any(k in c_code for k in ("rt_call_indirect", "return", "rt_svc", "rt_undefined", "goto"))


def generer(lf, mots, maxn, filtre, rng, lot=None):
    tests = []
    for hexw, (addr, cnt) in mots.items():
        w = int(hexw, 16)
        if filtre and not any(hexw.startswith(f) for f in filtre):
            continue
        try:
            code = lf.lift(w, addr)
        except Unsupported:
            continue
        if est_flot(code):
            continue
        tests.append((w, addr, cnt, code))
    if lot:  # balayage exhaustif découpé en lots : "i/N"
        i, n = map(int, lot.split("/"))
        tests.sort(key=lambda t: t[0])
        return [t for k, t in enumerate(tests) if k % n == i]
    # échantillon pondéré : les plus fréquentes d'abord, puis tirage aléatoire
    tests.sort(key=lambda t: -t[2])
    if len(tests) > maxn:
        tete = tests[:maxn // 2]
        reste = rng.sample(tests[maxn // 2:], maxn - len(tete))
        tests = tete + reste
    return tests


def compiler(tests, dossier):
    src = ['#include "cpu.h"', "#include <stdio.h>", "#include <stdlib.h>", "uint8_t *g_mem;",
           "void rt_call_indirect(Cpu *c, uint32_t t) { abort(); }",
           "void rt_svc(Cpu *c, uint32_t i) { abort(); }",
           "void rt_undefined(Cpu *c, uint32_t a) { abort(); }",
           "void rt_unimpl(Cpu *c, uint32_t a, const char *w) { abort(); }",
           "volatile uint32_t rt_excl_seq[4096]; volatile long rt_excl_verrou[4096];",
           "__declspec(dllexport) void set_mem(uint8_t *p) { g_mem = p; }"]
    for k, (w, addr, cnt, code) in enumerate(tests):
        src.append(f"static void t{k}(Cpu *c) {{ /* {w:08X} @{addr:X} */ {code} }}")
    src.append("static GuestFn table[] = {" + ", ".join(f"t{k}" for k in range(len(tests))) + "};")
    src.append("__declspec(dllexport) void run(int k, Cpu *c) { table[k](c); }")
    c_path = dossier / "test_diff.c"
    c_path.write_text("\n".join(src), encoding="utf-8")
    dll = dossier / "test_diff.dll"
    bat = dossier / "compile.bat"
    bat.write_text(f'@call "{VCVARS}" >nul\r\n"{LLVM / "clang.exe"}" -O1 -shared -ffp-contract=off -fno-strict-aliasing '
                   f'-Wno-everything -I "{RACINE / "runtime"}" -o "{dll}" "{c_path}"\r\n', encoding="mbcs")
    r = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit("échec de compilation")
    return dll


def etat_aleatoire(rng):
    regs = []
    for i in range(15):
        x = rng.random()
        if x < 0.45:
            regs.append(FENETRE + 0x8000 + (rng.randrange(0x4000) & ~3))
        elif x < 0.6:
            regs.append(rng.choice([0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, rng.randrange(256)]))
        else:
            regs.append(rng.getrandbits(32))
    regs[13] = FENETRE + 0x10000
    flags = [rng.getrandbits(1) for _ in range(4)]
    vfp = bytearray()
    for _ in range(64):
        x = rng.random()
        if x < 0.5:
            vfp += struct.pack("<f", rng.uniform(-1000, 1000))
        elif x < 0.6:
            vfp += struct.pack("<f", rng.choice([0.0, -0.0, 1.0, 0.5, 2.0 ** 31, -2.0 ** 31, 1e-3]))
        else:
            vfp += struct.pack("<I", rng.getrandbits(32))
    # quelques doubles plausibles
    for k in range(0, 32, 3):
        vfp[k * 8:k * 8 + 8] = struct.pack("<d", rng.uniform(-1e6, 1e6))
    mem = rng.randbytes(TAILLE_FEN)
    return regs, flags, bytes(vfp), mem


class Unicorn:
    def __init__(self, image):
        self.mu = mu = Uc(UC_ARCH_ARM, UC_MODE_ARM, cpu=UC_CPU_ARM_MAX)
        taille = (len(image) + 0xFFF) & ~0xFFF
        mu.mem_map(0, taille)
        mu.mem_write(0, image)
        mu.mem_protect(0, taille, UC_PROT_READ | UC_PROT_EXEC)  # une écriture dans le code fait fauter
        mu.mem_map(FENETRE, TAILLE_FEN)
        mu.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)
        mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    def run(self, addr, regs, flags, vfp, mem, tls):
        mu = self.mu
        mu.mem_write(FENETRE, mem)
        for i in range(15):
            mu.reg_write(UREGS[i], regs[i])
        cpsr = mu.reg_read(UC_ARM_REG_CPSR) & ~0xF80F0000 & ~0x20
        cpsr |= (flags[0] << 31) | (flags[1] << 30) | (flags[2] << 29) | (flags[3] << 28)
        mu.reg_write(UC_ARM_REG_CPSR, cpsr)
        mu.reg_write(UC_ARM_REG_FPSCR, 0)
        for k in range(32):
            mu.reg_write(UC_ARM_REG_D0 + k, struct.unpack_from("<Q", vfp, k * 8)[0])
        mu.reg_write(UC_ARM_REG_C13_C0_3, tls)
        mu.emu_start(addr, addr + 4, count=1)
        r = [mu.reg_read(UREGS[i]) for i in range(15)]
        cpsr = mu.reg_read(UC_ARM_REG_CPSR)
        fl = [(cpsr >> 31) & 1, (cpsr >> 30) & 1, (cpsr >> 29) & 1, (cpsr >> 28) & 1, (cpsr >> 27) & 1, (cpsr >> 16) & 15]
        v = b"".join(struct.pack("<Q", mu.reg_read(UC_ARM_REG_D0 + k)) for k in range(32))
        fpscr = mu.reg_read(UC_ARM_REG_FPSCR)
        return r, fl, v, fpscr >> 28, bytes(mu.mem_read(FENETRE, TAILLE_FEN))


def flottants_egaux(a, b):
    """Compare deux bancs VFP mot à mot ; deux NaN (simple ou double) sont considérés égaux."""
    if a == b:
        return True
    for k in range(32):
        x, y = a[k * 8:k * 8 + 8], b[k * 8:k * 8 + 8]
        if x == y:
            continue
        dx, dy = struct.unpack("<d", x)[0], struct.unpack("<d", y)[0]
        if dx != dx and dy != dy:
            continue
        ok = True
        for h in (0, 4):
            fx, fy = struct.unpack_from("<f", x, h)[0], struct.unpack_from("<f", y, h)[0]
            if x[h:h + 4] != y[h:h + 4] and not (fx != fx and fy != fy):
                ok = False
        if not ok:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("mots")
    ap.add_argument("--max", type=int, default=3000)
    ap.add_argument("--etats", type=int, default=6)
    ap.add_argument("--filtre", nargs="*")
    ap.add_argument("--graine", type=int, default=1)
    ap.add_argument("--lot", help="balayage exhaustif : i/N")
    a = ap.parse_args()
    rng = random.Random(a.graine)

    image = Nso(a.nso).image
    mots = json.load(open(a.mots))
    lf = Lifter(vfp=Vfp(neon=Neon() if Neon else None))
    tests = generer(lf, mots, a.max, a.filtre, rng, a.lot)
    dossier = RACINE / "build" / ("test_diff" + (f"_lot{a.lot.split('/')[0]}" if a.lot else ""))
    dossier.mkdir(parents=True, exist_ok=True)
    print(f"{len(tests)} instructions à tester, compilation…", flush=True)
    dll = ctypes.CDLL(str(compiler(tests, dossier)))

    # mémoire invitée côté C : 4 Gio réservés, image + fenêtre engagées
    k32 = ctypes.windll.kernel32
    k32.VirtualAlloc.restype = ctypes.c_void_p
    k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    base = k32.VirtualAlloc(None, 1 << 32, 0x2000, 0x04)
    taille_img = (len(image) + 0xFFF) & ~0xFFF
    k32.VirtualAlloc(base, taille_img, 0x1000, 0x04)
    k32.VirtualAlloc(base + FENETRE, TAILLE_FEN, 0x1000, 0x04)
    ctypes.memmove(base, image, len(image))
    ancien = ctypes.c_uint32()
    k32.VirtualProtect(ctypes.c_void_p(base), taille_img, 0x02, ctypes.byref(ancien))  # PAGE_READONLY
    dll.set_mem(ctypes.c_void_p(base))
    uc = Unicorn(image)

    echecs = Counter()
    exemples = {}
    passes = 0
    for k, (w, addr, cnt, code) in enumerate(tests):
        for e in range(a.etats):
            regs, flags, vfp, mem = etat_aleatoire(rng)
            tls = 0x10008000
            try:
                ur, ufl, uv, ufp, umem = uc.run(addr, regs, flags, vfp, mem, tls)
            except UcError:
                continue
            ctypes.memmove(base + FENETRE, mem, TAILLE_FEN)
            cpu = Cpu()
            for i in range(15):
                cpu.r[i] = regs[i]
            cpu.r[15] = addr + 8
            cpu.n, cpu.z, cpu.c, cpu.v = flags
            ctypes.memmove(cpu.vb, vfp, 256)
            cpu.tpidruro = tls
            try:
                dll.run(k, ctypes.byref(cpu))
            except OSError as ex:  # violation d'accès côté C alors qu'Unicorn n'a pas fauté
                cle = f"{w:08X}"
                echecs[cle] += 1
                exemples.setdefault(cle, (addr, [f"plantage C : {ex}"], code))
                break
            cr = list(cpu.r)[:15]
            cfl = [cpu.n, cpu.z, cpu.c, cpu.v, cpu.q, cpu.ge]
            cv = bytes(cpu.vb)
            cmem = ctypes.string_at(base + FENETRE, TAILLE_FEN)
            diffs = []
            if cr != ur:
                diffs.append("regs " + ", ".join(f"r{i}: C={cr[i]:08X} U={ur[i]:08X}" for i in range(15) if cr[i] != ur[i]))
            if cfl[:4] != ufl[:4] or cfl[4] != ufl[4] or cfl[5] != ufl[5]:
                diffs.append(f"drapeaux C={cfl} U={ufl}")
            if not flottants_egaux(cv, uv):
                d = [k2 for k2 in range(32) if cv[k2 * 8:k2 * 8 + 8] != uv[k2 * 8:k2 * 8 + 8]]
                diffs.append("vfp " + ", ".join(f"d{k2}: C={cv[k2*8:k2*8+8].hex()} U={uv[k2*8:k2*8+8].hex()}" for k2 in d[:3]))
            cfp = (cpu.fn << 3) | (cpu.fz << 2) | (cpu.fc << 1) | cpu.fv
            if cfp != ufp:
                diffs.append(f"fpscr C={cfp:04b} U={ufp:04b}")
            if cmem != umem:
                diffs.append("mémoire")
            if diffs:
                cle = f"{w:08X}"
                echecs[cle] += 1
                exemples.setdefault(cle, (addr, diffs, code))
                break
        else:
            passes += 1
    print(f"réussis : {passes}/{len(tests)}  échecs : {len(echecs)}")
    from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    for cle in list(echecs)[:40]:
        addr, diffs, code = exemples[cle]
        i = next(md.disasm(struct.pack("<I", int(cle, 16)), addr), None)
        txt = f"{i.mnemonic} {i.op_str}" if i else "?"
        print(f"- {cle} @{addr:X} {txt}\n    {' | '.join(diffs)}\n    C: {code[:300]}")
    json.dump({k: exemples[k][1] for k in echecs}, open(dossier / "echecs.json", "w"), indent=1)


if __name__ == "__main__":
    main()
