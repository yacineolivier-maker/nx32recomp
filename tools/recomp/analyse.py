"""Découverte des fonctions d'un module NSO AArch32 (mode A32).

Usage : python analyse.py <module.nso> <sortie.json> [--stats]

Graines : symboles exportés, init/fini arrays, cibles de relocations pointant dans .text,
puis cibles des BL/BLX trouvées par parcours récursif. Chaque fonction est parcourue en suivant
les branches ; les B vers un autre début de fonction sont des appels terminaux.
"""
import json
import struct
import sys
from collections import Counter
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_INS_ADD, ARM_INS_LDR, ARM_INS_B, ARM_INS_BX, ARM_INS_MOV, ARM_INS_BL, ARM_INS_BLX, ARM_INS_UDF, ARM_CC_AL, ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG, ARM_REG_LR, ARM_REG_PC

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from nso import Nso  # noqa: E402
from profil import dossier_profil  # noqa: E402

DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28
DT_REL, DT_RELSZ, DT_JMPREL, DT_PLTRELSZ = 17, 18, 23, 2
R_ARM_ABS32, R_ARM_RELATIVE = 2, 23


class Module:
    def __init__(self, path):
        self.nso = n = Nso(path)
        self.img = n.image
        self.text_end = n.seg["text"][1]
        self.md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
        self.md.detail = True
        self.md.skipdata = True  # continuer après un mot invalide (pseudo-instruction id 0)
        self.cache = {}
        self.dyn = {}
        d = n.mod0["dynamic"]
        i = 0
        while True:
            tag, val = struct.unpack_from("<iI", self.img, d + i * 8)
            if tag == 0:
                break
            self.dyn.setdefault(tag, []).append(val)
            i += 1

    def u32(self, a):
        return struct.unpack_from("<I", self.img, a)[0]

    def in_text(self, a):
        return 0 <= a < self.text_end and a % 4 == 0

    def insn(self, a):
        """Instruction à l'adresse a (None si invalide). Décode par blocs de 4 Ko : en A32 la taille
        fixe rend le décodage linéaire exact, et c'est bien plus rapide qu'un appel par instruction."""
        if a not in self.cache:
            blk = a & ~0xFFF
            end = min(blk + 0x1000, len(self.img))
            for x in range(blk, end, 4):
                self.cache[x] = None
            for i in self.md.disasm(self.img[blk:end], blk):
                if i.id != 0 and i.size == 4:
                    self.cache[i.address] = i
            if a not in self.cache:
                self.cache[a] = None
        return self.cache[a]

    def relocations(self):
        """(emplacement, type, valeur cible) pour DT_REL et DT_JMPREL (REL : addend implicite)."""
        out = []
        for off_tag, sz_tag in ((DT_REL, DT_RELSZ), (DT_JMPREL, DT_PLTRELSZ)):
            if off_tag not in self.dyn:
                continue
            off, sz = self.dyn[off_tag][0], self.dyn[sz_tag][0]
            for i in range(sz // 8):
                where, info = struct.unpack_from("<II", self.img, off + i * 8)
                out.append((where, info & 0xFF, info >> 8))
        return out

    def seeds(self):
        s = {}
        for sym in self.nso.symbols():
            if sym["defined"] and sym["type"] == 2 and self.in_text(sym["value"]):  # STT_FUNC
                s[sym["value"]] = sym["name"]
        for tag in (12, 13):  # DT_INIT, DT_FINI
            for v in self.dyn.get(tag, []):
                if self.in_text(v):
                    s.setdefault(v, None)
        i0 = self.insn(0)  # point d'entrée du module (rtld : « b _start »)
        if i0 is not None and i0.mnemonic == "b":
            s.setdefault(0, None)
        for arr, size in ((DT_INIT_ARRAY, DT_INIT_ARRAYSZ), (DT_FINI_ARRAY, DT_FINI_ARRAYSZ)):
            if arr in self.dyn:
                a, n = self.dyn[arr][0], self.dyn[size][0]
                for k in range(n // 4):
                    t = self.u32(a + 4 * k)
                    if self.in_text(t):
                        s.setdefault(t, None)
        reloc_targets = set()
        for where, typ, _ in self.relocations():
            if typ == R_ARM_RELATIVE:
                t = self.u32(where)
                if self.in_text(t):
                    reloc_targets.add(t)
        return s, reloc_targets


def is_return(i):
    m = i.mnemonic.split(".")[0]
    ops = i.operands
    if m.startswith("bx") and ops and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_LR:
        return True
    if (m.startswith("pop") or m.startswith("ldm")) and any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ops):
        return True
    if m.startswith("mov") and len(ops) == 2 and ops[0].reg == ARM_REG_PC and ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_LR:
        return True
    if m.startswith("ldr") and ops and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_PC \
            and len(ops) > 1 and ops[1].type == ARM_OP_MEM and ops[1].mem.base == 13:  # ldr pc,[sp],#4 (ARM_REG_SP)
        return True
    return False


def writes_pc(i):
    return ARM_REG_PC in i.regs_access()[1]


class Analyse:
    def __init__(self, mod: Module, graines=()):
        self.m = mod
        self.graines = set(graines)  # adresses (relatives au module) trouvées manquantes à l'exécution
        self.funcs = {}          # début -> dict
        self.names = {}
        self.bad = Counter()

    def jump_table(self, a, i):
        """Table de sauts clang : cmp rI,#N / bhi défaut / ... / add rT,pc,#K / ldr rI,[rI,rT] / add pc,rI,rT.
        La table (N+1 mots) contient des décalages relatifs à sa base. Renvoie (base, n, cibles) ou None."""
        m = self.m
        ops = i.operands
        if len(ops) != 3 or ops[1].type != ARM_OP_REG or ops[2].type != ARM_OP_REG or i.cc != ARM_CC_AL:
            return None
        regs = {ops[1].reg, ops[2].reg}
        base = n = None
        cherche_cmp = True
        for k in range(1, 25):
            p = m.insn(a - 4 * k)
            if p is None:
                break
            po = p.operands
            if base is None and p.mnemonic == "add" and len(po) == 3 and po[0].reg in regs                     and po[1].type == ARM_OP_REG and po[1].reg == ARM_REG_PC and po[2].type == ARM_OP_IMM:
                base = p.address + 8 + po[2].imm
            if p.cc == ARM_CC_AL and (p.id == ARM_INS_B or p.id == ARM_INS_BX or writes_pc(p)):
                cherche_cmp = False  # frontière de bloc : un cmp plus haut appartient à un autre bloc
                if base is not None:
                    break
            if cherche_cmp and n is None and p.mnemonic == "cmp" and len(po) == 2 and po[1].type == ARM_OP_IMM:
                n = po[1].imm + 1
                if base is not None:
                    break
        if base is None:
            return None
        if n is None:  # taille déduite de la table : entrées avant la première cible, visant du code valide
            fin = 0xFFFFFFFF
            n = 0
            while n < 1024 and base + 4 * n < fin:
                d = struct.unpack_from("<i", m.img, base + 4 * n)[0]
                t = (base + d) & 0xFFFFFFFF
                if d <= 0 or d % 4 or not m.in_text(t) or m.insn(t) is None:
                    break
                fin = min(fin, t)
                n += 1
            if n == 0:
                return None
        if n > 4096:
            return None
        targets = []
        for k in range(n):
            t = (base + struct.unpack_from("<i", m.img, base + 4 * k)[0]) & 0xFFFFFFFF
            if not m.in_text(t):
                return None
            targets.append(t)
        return base, n, targets

    def walk(self, start, starts):
        m = self.m
        insns, work, calls, tails, indirect = set(), [start], set(), set(), 0
        ptrs = set()
        cjumps = {}
        tables_vues = set()
        jtabs = {}
        while work:
            a = work.pop()
            while True:
                if a in insns or not m.in_text(a):
                    break
                i = m.insn(a)
                if i is None:
                    self.bad["invalide"] += 1
                    break
                insns.add(a)
                mn = i.mnemonic
                cond = i.cc
                nxt = a + 4
                if i.id in (ARM_INS_BL, ARM_INS_BLX):
                    op = i.operands[0]
                    if op.type == ARM_OP_IMM:
                        calls.add(op.imm)
                    else:
                        indirect += 1
                    a = nxt
                    continue
                if i.id == ARM_INS_B:
                    t = i.operands[0].imm
                    if t in starts and t != start:
                        tails.add(t)
                    else:
                        work.append(t)
                    if cond == ARM_CC_AL:
                        break
                    a = nxt
                    continue
                if is_return(i):
                    if cond == ARM_CC_AL:
                        break
                    a = nxt
                    continue
                if writes_pc(i):
                    if i.id == ARM_INS_ADD:
                        tbl = self.jump_table(a, i)
                        if tbl:
                            jtabs[a] = tbl
                            work.extend(tbl[2])
                            tables_vues.add(tbl[0])  # l'adr vers la table n'est pas un pointeur de fonction
                            break
                    plage = self.computed_jump(a, i)
                    if plage:  # saut calculé dans une boucle déroulée de la fonction (ex. __udivmodsi4)
                        cjumps[a] = plage
                        work.extend(range(plage[0], plage[1] + 4, 4))
                        if cond == ARM_CC_AL:
                            break
                        a = nxt
                        continue
                    indirect += 1
                    self.bad["saut_indirect"] += 1
                    if cond == ARM_CC_AL:
                        break
                    a = nxt
                    continue
                if i.id == ARM_INS_UDF:
                    break
                if i.id == ARM_INS_ADD:
                    p = self.pc_relative(a, i)
                    if p is not None:
                        ptrs.add(p)
                a = nxt
        return dict(insns=insns, calls=calls, tails=tails, jtabs=jtabs, indirect=indirect, ptrs=ptrs - tables_vues, cjumps=cjumps)

    def computed_jump(self, a, i):
        """`bx rX` / `mov pc, rX` dont rX dérive d'un `add rX, pc, #imm` proche et pointe après le saut :
        renvoie la plage [a+4, base] des cibles possibles, sinon None."""
        ops = i.operands
        if i.id not in (ARM_INS_BX, ARM_INS_MOV) or not ops or ops[-1].type != ARM_OP_REG:
            return None
        rx = ops[-1].reg
        if rx in (ARM_REG_LR, ARM_REG_PC):
            return None
        for k in range(1, 10):
            p = self.m.insn(a - 4 * k)
            if p is None:
                return None
            po = p.operands
            if p.id == ARM_INS_ADD and len(po) == 3 and po[0].reg == rx and po[1].type == ARM_OP_REG                     and po[1].reg == ARM_REG_PC and po[2].type == ARM_OP_IMM:
                base = p.address + 8 + po[2].imm
                if a < base <= a + 0x1000:
                    return (a + 4, base)
                return None
        return None

    def pc_relative(self, a, i):
        """Adresse calculée relativement à PC : `add rd, pc, #imm` (adr) ou `ldr rX, [pc, #k]` puis
        `add rd, pc, rX` (code PIC sans relocation). Renvoie la cible ou None."""
        ops = i.operands
        if len(ops) != 3:  # les formes conditionnelles (addeq rd, pc, rX) comptent aussi
            return None
        if ops[1].type == ARM_OP_REG and ops[1].reg == ARM_REG_PC and ops[2].type == ARM_OP_IMM:
            return (a + 8 + ops[2].imm) & 0xFFFFFFFF
        regs = [o.reg for o in ops[1:] if o.type == ARM_OP_REG]
        if ARM_REG_PC not in regs or len(regs) != 2 or ops[2].shift.type:
            return None
        rx = regs[0] if regs[1] == ARM_REG_PC else regs[1]
        for k in range(1, 25):  # remonter jusqu'au chargement du littéral (jusqu'à 24 instructions)
            p = self.m.insn(a - 4 * k)
            if p is None:
                return None
            po = p.operands
            if p.id == ARM_INS_LDR and len(po) == 2 and po[0].reg == rx and po[1].type == ARM_OP_MEM                     and po[1].mem.base == ARM_REG_PC and po[1].mem.index == 0:
                lit = self.m.u32(p.address + 8 + po[1].mem.disp)
                return (a + 8 + lit) & 0xFFFFFFFF
            if rx in p.regs_access()[1]:
                return None
        return None

    def run(self):
        m = self.m
        named, reloc_targets = m.seeds()
        self.names = dict(named)
        starts = set(named) | {t for t in reloc_targets if self.plausible(t)} | self.graines
        if self.graines:
            print(f"graines supplémentaires : {len(self.graines)}")
        pending = list(starts)
        while pending:
            s = pending.pop()
            if s in self.funcs:
                continue
            f = self.walk(s, starts)
            self.funcs[s] = f
            ptrs = {p for p in f["ptrs"] if self.plausible(p) and (p not in f["insns"] or self.prologue(p))}
            for t in f["calls"] | f["tails"] | ptrs:
                if m.in_text(t) and t not in starts:
                    starts.add(t)
                    pending.append(t)
        # balayage des prologues hors code couvert : rappels et pointeurs calculés que rien ne référence
        couvert = set()
        for f in self.funcs.values():
            couvert |= f["insns"]
        nouveaux = [a for a in range(0, m.text_end, 4) if a not in couvert and a not in starts and self.prologue(a)]
        print(f"prologues isolés ajoutés : {len(nouveaux)}")
        pending = list(nouveaux)
        starts.update(nouveaux)
        while pending:
            s = pending.pop()
            if s in self.funcs:
                continue
            f = self.walk(s, starts)
            self.funcs[s] = f
            ptrs = {p for p in f["ptrs"] if self.plausible(p) and (p not in f["insns"] or self.prologue(p))}
            for t in f["calls"] | f["tails"] | ptrs:
                if m.in_text(t) and t not in starts:
                    starts.add(t)
                    pending.append(t)
        # 2e passe : des B internes ont pu viser des débuts découverts plus tard -> les re-parcourir
        for s in list(self.funcs):
            self.funcs[s] = self.walk(s, starts)
        return self

    def prologue(self, t):
        """Début de fonction typique : push {..., lr} / stmdb sp!, {..., lr}."""
        i = self.m.insn(t)
        return i is not None and i.mnemonic in ("push", "stmdb") and "lr" in i.op_str

    def plausible(self, t):
        """Une cible de relocation dans .text est un début de fonction si elle décode en ARM valide."""
        i = self.m.insn(t)
        return i is not None and not i.mnemonic.startswith("udf")

    def stats(self):
        m = self.m
        covered = set()
        for f in self.funcs.values():
            covered |= f["insns"]
        mn = Counter()
        for a in covered:
            mn[m.insn(a).mnemonic] += 1
        print(f"fonctions        : {len(self.funcs)} (nommées : {len(self.names)})")
        print(f"instructions     : {len(covered)} ({len(covered) * 4 / m.text_end:.1%} du .text)")
        print(f"tables de sauts  : {sum(len(f['jtabs']) for f in self.funcs.values())}")
        print(f"anomalies        : {dict(self.bad)}")
        return mn

    def save(self, path):
        out = []
        for s in sorted(self.funcs):
            f = self.funcs[s]
            runs = []  # plages [début, fin) d'instructions réellement atteintes
            for a in sorted(f["insns"]):
                if runs and runs[-1][1] == a:
                    runs[-1][1] = a + 4
                else:
                    runs.append([a, a + 4])
            out.append(dict(start=s, name=self.names.get(s), size=(max(f["insns"]) + 4 - s) if f["insns"] else 0,
                            runs=runs,
                            n=len(f["insns"]), calls=sorted(f["calls"]), tails=sorted(f["tails"]),
                            jtabs={str(k): v for k, v in f["jtabs"].items()},
                            cjumps={str(k): v for k, v in f["cjumps"].items()}))
        json.dump(out, open(path, "w"))


if __name__ == "__main__":
    mod = Module(sys.argv[1])
    graines = []
    fg = dossier_profil() / f"{Path(sys.argv[1]).name}_graines.txt"
    if fg.exists():
        graines = [int(x, 16) for x in fg.read_text().split() if x.strip()]
    an = Analyse(mod, graines).run()
    mn = an.stats()
    an.save(sys.argv[2])
    if "--stats" in sys.argv:
        base = Counter()
        for k, v in mn.items():
            base[k.split(".")[0]] += v
        print(f"mnémoniques distincts : {len(base)}")
        print(", ".join(f"{k}:{v}" for k, v in base.most_common()))
