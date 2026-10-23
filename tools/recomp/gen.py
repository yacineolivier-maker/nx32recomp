"""Génération du C recompilé pour tous les modules liés.

Usage : python gen.py <dossier_exefs> <dossier_build>   (lit build/lien/layout.json et build/<mod>_funcs.json)

Sortie dans <dossier_build>/gen/ : funcs.h (prototypes), table.c (adresse -> fonction), rc_NNN.c.
Convention : `void f_XXXXXXXX(Cpu *c)` pour la fonction invitée à l'adresse absolue XXXXXXXX.
Un appel invité devient un appel C ; un retour (saut vers le LR reçu à l'entrée, `ra_`) devient `return`.
"""
import json
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from lifter import Lifter, Unsupported  # noqa: E402
from neon import Neon  # noqa: E402
from nso import Nso  # noqa: E402
from profil import dossier_profil  # noqa: E402
from vfp import Vfp  # noqa: E402

PAR_FICHIER = 1500
RE_GOTO = re.compile(r"goto L_([0-9A-F]+);")


class FuncCtx:
    def __init__(self, gen, f, base):
        self.gen = gen
        self.base = base
        self.start = base + f["start"]
        self.insns = {base + a for r0, r1 in f["runs"] for a in range(r0, r1, 4)}
        self.jtabs = {base + int(k): [base + t for t in v[2]] for k, v in f["jtabs"].items()}
        # sauts calculés internes : toutes les instructions de la plage sont des cibles possibles
        for k, (lo, hi) in f.get("cjumps", {}).items():
            self.jtabs[base + int(k)] = list(range(base + lo, base + hi + 4, 4))

    def fn(self, t):
        return f"f_{t:08X}" if t in self.gen.starts else None

    def call(self, t):
        f = self.fn(t)
        return f"{f}(c);" if f else f"rt_call_indirect(c, 0x{t:X}u);"

    def branch(self, target, addr, link):
        if link:
            return f"c->r[14] = 0x{addr + 4:X}u; {self.call(target)}"
        if target in self.insns:
            return f"goto L_{target:X};"
        return f"{self.call(target)} return;"

    def jump(self, expr, addr, kind):
        if addr in self.jtabs:
            cases = " ".join(f"case 0x{t:X}u: goto L_{t:X};" for t in sorted(set(self.jtabs[addr])))
            return (f"{{ uint32_t t_ = {expr}; switch (t_) {{ {cases} "
                    f"default: rt_bad_jump(c, 0x{addr:X}u, t_); return; }} }}")
        return f"{{ uint32_t t_ = {expr}; if (t_ != ra_) rt_call_indirect(c, t_); return; }}"


class Gen:
    def __init__(self, exefs, build):
        self.build = Path(build)
        self.layout = json.load(open(self.build / "lien" / "layout.json"))
        self.image = (self.build / "lien" / "image.bin").read_bytes()
        self.B = self.layout["base"]
        self.mods = []
        self.starts = set()
        for m in self.layout["modules"]:
            fj = self.build / f"{m['nom']}_funcs.json"
            if not fj.exists():
                print(f"(pas de fonctions pour {m['nom']})")
                continue
            funcs = json.load(open(fj))
            self.mods.append((m, funcs))
            self.starts.update(m["base"] + f["start"] for f in funcs)
        self.lifter = Lifter(vfp=Vfp(neon=Neon()))
        # remplacements natifs : <profil>/hle.txt (adresse hexadécimale absolue ou nom de symbole, un par ligne)
        self.hle = set()
        self.hle_noms = {}
        fh = dossier_profil() / "hle.txt"
        if fh.exists():
            for ligne in fh.read_text(encoding="utf-8").splitlines():
                ligne = ligne.split("#")[0].strip()
                if not ligne:
                    continue
                if ligne in self.layout["symboles"]:
                    self.hle.add(self.layout["symboles"][ligne][0])
                    self.hle_noms[ligne] = self.layout["symboles"][ligne][0]
                elif re.fullmatch(r"[0-9A-Fa-f]+", ligne):
                    self.hle.add(int(ligne, 16))
                else:
                    print(f"(hle.txt : symbole {ligne} absent de ce jeu, ignoré)")
        self.stats = dict(fonctions=0, instructions=0, non_traduites=0)

    def word(self, a):
        return struct.unpack_from("<I", self.image, a - self.B)[0]

    def gen_func(self, f, base):
        ctx = FuncCtx(self, f, base)
        self.lifter.ctx = ctx
        lignes = []
        addrs = sorted(ctx.insns)
        tables = set()
        for k, v in f["jtabs"].items():
            tb, n, _ = v
            tables.update(base + tb + 4 * i for i in range(n))
        prec_tombe = False
        for idx, a in enumerate(addrs):
            if a in tables:
                continue
            if prec_tombe and idx and addrs[idx - 1] + 4 != a:
                lignes.append(f"  goto L_{addrs[idx - 1] + 4:X};")
            w = self.word(a)
            try:
                code = self.lifter.lift(w, a)
            except Unsupported as e:
                code = f'rt_unimpl(c, 0x{a:X}u, "{e}"); return;'
                self.stats["non_traduites"] += 1
            lignes.append((a, code))
            fin_inconditionnelle = not code.startswith("if (") and (
                code.rstrip().endswith("return;") or code.rstrip().endswith("return; } }") or
                re.fullmatch(r"goto L_[0-9A-F]+;", code.strip()) is not None)
            prec_tombe = not fin_inconditionnelle
            self.stats["instructions"] += 1
        # sortie de la dernière instruction par chute : continuer à l'adresse suivante
        if prec_tombe and addrs:
            suiv = addrs[-1] + 4
            lignes.append(f"  {ctx.call(suiv)} return;")
        # étiquettes réellement utilisées
        cibles = set()
        for item in lignes:
            txt = item[1] if isinstance(item, tuple) else item
            cibles.update(int(x, 16) for x in RE_GOTO.findall(txt))
        nom = "orig" if ctx.start in self.hle else "f"  # fonction remplacée : le runtime fournit f_X
        out = [f"void {nom}_{ctx.start:08X}(Cpu *c) {{", "  uint32_t ra_ = c->r[14]; (void)ra_;",
               f"  RC_ENTER(0x{ctx.start:X}u);"]
        # du code partagé peut précéder l'entrée : on commence toujours par l'entrée
        emises = [it[0] for it in lignes if isinstance(it, tuple)]
        if ctx.start not in emises:  # entrée qui n'est pas une instruction (donnée prise pour du code)
            out.append(f'  rt_unimpl(c, 0x{ctx.start:X}u, "entrée de fonction invalide"); return;')
        elif emises[0] != ctx.start:
            out.append(f"  goto L_{ctx.start:X};")
            cibles.add(ctx.start)
        for item in lignes:
            if isinstance(item, tuple):
                a, code = item
                if a in cibles:
                    out.append(f"L_{a:X}:;")
                out.append(f"  {code}")
            else:
                out.append(item)
        manquantes = cibles - {a for a in addrs}
        for t in sorted(manquantes):  # cible hors des instructions découvertes
            out.append(f"L_{t:X}: rt_call_indirect(c, 0x{t:X}u); return;")
        out.append("}")
        self.stats["fonctions"] += 1
        return "\n".join(out)

    def run(self):
        dst = self.build / "gen"
        dst.mkdir(exist_ok=True)
        for p in dst.glob("rc_*.c"):
            p.unlink()
        noms = sorted(self.starts)
        with open(dst / "funcs.h", "w") as h:
            h.write('#pragma once\n#include "cpu.h"\nvoid rt_bad_jump(Cpu *c, uint32_t at, uint32_t t);\n')
            for a in noms:
                h.write(f"void f_{a:08X}(Cpu *c);\n")
            for nom, a in sorted(self.hle_noms.items()):  # noms stables pour runtime/hle.c
                h.write(f"#define HLE_{nom} f_{a:08X}\n#define ORIG_{nom} orig_{a:08X}\n")
            for a in sorted(self.hle):
                h.write(f"void orig_{a:08X}(Cpu *c); /* original d'une fonction remplacée */\n")
        with open(dst / "table.c", "w") as t:
            t.write('#include "funcs.h"\n#include "table.h"\n')
            t.write(f"const RcEntry rc_table[{len(noms)}] = {{\n")
            for a in noms:
                t.write(f"  {{0x{a:08X}u, f_{a:08X}}},\n")
            t.write(f"}};\nconst uint32_t rc_table_size = {len(noms)};\n")
        n_fichier = 0
        buf = []
        t0 = time.time()

        def vider():
            nonlocal n_fichier, buf
            if buf:
                (dst / f"rc_{n_fichier:03d}.c").write_text('#include "funcs.h"\n\n' + "\n\n".join(buf), encoding="utf-8")
                n_fichier += 1
                buf = []

        for m, funcs in self.mods:
            for f in funcs:
                if not f["runs"]:
                    continue
                buf.append(self.gen_func(f, m["base"]))
                if len(buf) >= PAR_FICHIER:
                    vider()
            vider()
            print(f"{m['nom']:9} {len(funcs)} fonctions ({time.time() - t0:.0f} s)", flush=True)
        print(f"total : {self.stats}, fichiers : {n_fichier}")


if __name__ == "__main__":
    Gen(sys.argv[1], sys.argv[2]).run()
