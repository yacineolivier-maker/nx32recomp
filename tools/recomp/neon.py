"""Traduction NEON (Advanced SIMD) A32 -> C.

Capstone sert au décodage (mnémonique + opérandes texte) ; les immédiats sont recalculés depuis
l'encodage. Chaque instruction copie ses sources dans des tampons locaux, calcule voie par voie,
puis réécrit la destination : les recouvrements source/destination sont ainsi sans danger.
Les opérations flottantes NEON utilisent la « Standard FPSCR value » : FZ=1, DN=1, arrondi au plus proche
(helpers nf_in / nf_out de cpu.h).
"""
import re
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

from lifter import Unsupported, bits

CT = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}
ST = {8: "int8_t", 16: "int16_t", 32: "int32_t", 64: "int64_t"}


def adv_simd_imm(op, cmode, imm8):
    """AdvSIMDExpandImm -> motif 64 bits."""
    c3 = cmode >> 1
    if c3 == 0:
        e = imm8
        return e | (e << 32)
    if c3 == 1:
        e = imm8 << 8
        return e | (e << 32)
    if c3 == 2:
        e = imm8 << 16
        return e | (e << 32)
    if c3 == 3:
        e = imm8 << 24
        return e | (e << 32)
    if c3 == 4:
        e = imm8
        return e * 0x0001000100010001
    if c3 == 5:
        e = imm8 << 8
        return e * 0x0001000100010001
    if c3 == 6:
        e = (imm8 << 8) | 0xFF if not cmode & 1 else (imm8 << 16) | 0xFFFF
        return e | (e << 32)
    if cmode & 1 == 0 and op == 0:
        return imm8 * 0x0101010101010101
    if cmode & 1 == 0 and op == 1:
        r = 0
        for k in range(8):
            if imm8 & (1 << k):
                r |= 0xFF << (8 * k)
        return r
    if op == 0:  # flottant simple
        a, b, cdefgh = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
        e = (a << 31) | ((b ^ 1) << 30) | ((0x1F if b else 0) << 25) | (cdefgh << 19)
        return e | (e << 32)
    raise Unsupported("imm simd")


class Vec:
    """Opérande vectoriel : registre D/Q ou scalaire d[x]."""

    def __init__(self, txt):
        m = re.fullmatch(r"([dq])(\d+)(?:\[(\d+)\])?", txt.strip())
        if not m:
            raise Unsupported(f"opérande {txt}")
        self.kind, self.num = m.group(1), int(m.group(2))
        self.idx = int(m.group(3)) if m.group(3) is not None else None
        self.off = self.num * (16 if self.kind == "q" else 8)
        self.size = 16 if self.kind == "q" else 8


class Neon:
    def __init__(self):
        self.md = Cs(CS_ARCH_ARM, CS_MODE_ARM)

    def lift(self, w, addr):
        i = next(self.md.disasm(struct.pack("<I", w), addr), None)
        if i is None:
            raise Unsupported(f"neon indécodable {w:08X}")
        mn = i.mnemonic
        base, *types = mn.split(".")
        ops = self.split_ops(i.op_str)
        fn = getattr(self, "op_" + base, None)
        if fn is None:
            raise Unsupported(f"neon {mn}")
        return fn(w, addr, types, ops)

    @staticmethod
    def split_ops(s):
        out, depth, cur = [], 0, ""
        for ch in s:
            if ch in "{[":
                depth += 1
            elif ch in "}]":
                depth -= 1
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    # ---------------------------------------------------------------- outils de génération
    @staticmethod
    def load(name, v: Vec, esize, ct, n_dest=None):
        n = v.size // (esize // 8)
        if v.idx is not None:  # scalaire répliqué sur toute la largeur de la destination
            n = n_dest or n
            return (f"{ct} {name}[{n}]; {{ {ct} s_; memcpy(&s_, &c->vr.b[{v.off + v.idx * esize // 8}], {esize // 8}); "
                    f"for (int i_ = 0; i_ < {n}; i_++) {name}[i_] = s_; }}")
        return f"{ct} {name}[{n}]; memcpy({name}, &c->vr.b[{v.off}], {v.size});"

    @staticmethod
    def store(name, v: Vec):
        return f"memcpy(&c->vr.b[{v.off}], {name}, {v.size});"

    def lanes(self, d: Vec, srcs, esize, ct, expr, dest_in=False):
        n = d.size // (esize // 8)
        code = [self.load(f"s{k}_", v, esize, ct, n) for k, v in enumerate(srcs)]
        if dest_in:
            code.append(self.load("d0_", d, esize, ct))
        else:
            code.append(f"{ct} d0_[{n}];")
        code.append(f"for (int i_ = 0; i_ < {n}; i_++) d0_[i_] = {expr};")
        code.append(self.store("d0_", d))
        return "{ " + " ".join(code) + " }"

    def fbin(self, ops, expr, dest_in=False):
        d, n, m = (Vec(x) for x in ops)
        e = expr.replace("A", "nf_in(s0_[i_])").replace("B", "nf_in(s1_[i_])").replace("D", "nf_in(d0_[i_])")
        return self.lanes(d, [n, m], 32, "float", f"nf_out({e})", dest_in)

    # ---------------------------------------------------------------- chargements / rangements
    def ldst(self, w, addr, types, ops, load):
        esize = int(types[0]) if types else 8
        regs_txt, mem = ops[0], ops[1]
        post = ops[2] if len(ops) > 2 else None
        mm = re.fullmatch(r"\[(r\d+|sb|sl|fp|ip|sp|lr)(?::0x[0-9a-f]+|:\d+)?\](!?)", mem.replace(" ", ""))
        if not mm:
            raise Unsupported(f"vld/vst adresse {mem}")
        rn = self.gpr(mm.group(1))
        wb = mm.group(2) == "!"
        lst = [x.strip() for x in regs_txt.strip("{}").split(",")]
        out = [f"uint32_t ea_ = c->r[{rn}];"]
        total = 0
        if all(re.fullmatch(r"d\d+", x) for x in lst):
            for k, x in enumerate(lst):
                off = int(x[1:]) * 8
                if load:
                    out.append(f"memcpy(&c->vr.b[{off}], HP(ea_ + {8 * k}), 8);")
                else:
                    out.append(f"memcpy(HP(ea_ + {8 * k}), &c->vr.b[{off}], 8);")
            total = 8 * len(lst)
        else:
            eb = esize // 8
            for k, x in enumerate(lst):
                m2 = re.fullmatch(r"d(\d+)\[(\d*)\]", x)
                if not m2:
                    raise Unsupported(f"vld1 liste {regs_txt}")
                off = int(m2.group(1)) * 8
                if m2.group(2) == "":  # toutes les voies
                    if not load:
                        raise Unsupported("vst1 toutes voies")
                    # un seul élément chargé, répliqué dans toutes les voies de chaque registre de la liste
                    out.append(f"for (int i_ = 0; i_ < {8 // eb}; i_++) memcpy(&c->vr.b[{off} + i_ * {eb}], HP(ea_), {eb});")
                else:
                    lo = off + int(m2.group(2)) * eb
                    out.append(f"memcpy(&c->vr.b[{lo}], HP(ea_ + {k * eb}), {eb});" if load
                               else f"memcpy(HP(ea_ + {k * eb}), &c->vr.b[{lo}], {eb});")
            total = eb if lst[0].endswith("[]") else eb * len(lst)
        if post:
            out.append(f"c->r[{rn}] = ea_ + c->r[{self.gpr(post)}];")
        elif wb:
            out.append(f"c->r[{rn}] = ea_ + {total};")
        return "{ " + " ".join(out) + " }"

    @staticmethod
    def gpr(name):
        alias = {"sb": 9, "sl": 10, "fp": 11, "ip": 12, "sp": 13, "lr": 14, "pc": 15}
        return alias[name] if name in alias else int(name[1:])

    def op_vld1(self, w, addr, types, ops):
        return self.ldst(w, addr, types, ops, True)

    def ldst2(self, w, addr, types, ops, load):
        """VLD2/VST2 (structures multiples à 2 éléments) : {Da, Db} (inc 1 ou 2) ou {Dd..Dd+3} (deux paires) ;
        les éléments pairs en mémoire vont au membre A, les impairs au membre B."""
        eb = int(types[0]) // 8
        regs_txt, mem = ops[0], ops[1]
        post = ops[2] if len(ops) > 2 else None
        mm = re.fullmatch(r"\[(r\d+|sb|sl|fp|ip|sp|lr)(?::0x[0-9a-f]+|:\d+)?\](!?)", mem.replace(" ", ""))
        if not mm:
            raise Unsupported(f"vld2/vst2 adresse {mem}")
        rn = self.gpr(mm.group(1))
        wb = mm.group(2) == "!"
        lst = [x.strip() for x in regs_txt.strip("{}").split(",")]
        if not all(re.fullmatch(r"d\d+", x) for x in lst):
            raise Unsupported(f"vld2/vst2 liste {regs_txt}")
        nums = [int(x[1:]) for x in lst]
        if len(nums) == 2:
            a_off, b_off, octets = nums[0] * 8, nums[1] * 8, 8
        elif len(nums) == 4 and nums == list(range(nums[0], nums[0] + 4)):
            a_off, b_off, octets = nums[0] * 8, nums[2] * 8, 16
        else:
            raise Unsupported(f"vld2/vst2 liste {regs_txt}")
        n = octets // eb
        if load:
            corps = (f"memcpy(&c->vr.b[{a_off} + i_ * {eb}], HP(ea_ + 2 * i_ * {eb}), {eb}); "
                     f"memcpy(&c->vr.b[{b_off} + i_ * {eb}], HP(ea_ + (2 * i_ + 1) * {eb}), {eb});")
        else:
            corps = (f"memcpy(HP(ea_ + 2 * i_ * {eb}), &c->vr.b[{a_off} + i_ * {eb}], {eb}); "
                     f"memcpy(HP(ea_ + (2 * i_ + 1) * {eb}), &c->vr.b[{b_off} + i_ * {eb}], {eb});")
        out = [f"uint32_t ea_ = c->r[{rn}];", f"for (int i_ = 0; i_ < {n}; i_++) {{ {corps} }}"]
        if post:
            out.append(f"c->r[{rn}] = ea_ + c->r[{self.gpr(post)}];")
        elif wb:
            out.append(f"c->r[{rn}] = ea_ + {2 * octets};")
        return "{ " + " ".join(out) + " }"

    def op_vld2(self, w, addr, types, ops):
        return self.ldst2(w, addr, types, ops, True)

    def op_vst2(self, w, addr, types, ops):
        return self.ldst2(w, addr, types, ops, False)

    def op_vst1(self, w, addr, types, ops):
        return self.ldst(w, addr, types, ops, False)

    # ---------------------------------------------------------------- arithmétique flottante
    def op_vadd(self, w, a, t, ops):
        if t != ["f32"]:
            return self.intbin(t, ops, "s0_[i_] + s1_[i_]")
        return self.fbin(ops, "A + B")

    def op_vsub(self, w, a, t, ops):
        if t != ["f32"]:
            return self.intbin(t, ops, "s0_[i_] - s1_[i_]")
        return self.fbin(ops, "A - B")

    def op_vmul(self, w, a, t, ops):
        if t != ["f32"]:
            return self.intbin(t, ops, "s0_[i_] * s1_[i_]")
        return self.fbin(ops, "A * B")

    def op_vmla(self, w, a, t, ops):
        self.need_f32(t)
        return self.fbin(ops, "D + nf_out(A * B)", dest_in=True)

    def op_vmls(self, w, a, t, ops):
        self.need_f32(t)
        return self.fbin(ops, "D - nf_out(A * B)", dest_in=True)

    def op_vmax(self, w, a, t, ops):
        if t != ["f32"]:
            return self.int_minmax(t, ops, ">")
        return self.fbin(ops, "fmax_arm(A, B)")

    def op_vmin(self, w, a, t, ops):
        if t != ["f32"]:
            return self.int_minmax(t, ops, "<")
        return self.fbin(ops, "fmin_arm(A, B)")

    def int_minmax(self, t, ops, op):
        size, signe = self.int_type(t)
        ct, st = CT[size], (ST[size] if signe else CT[size])
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], size, ct, f"(({st})s0_[i_] {op} ({st})s1_[i_]) ? s0_[i_] : s1_[i_]")

    def op_vrecps(self, w, a, t, ops):
        self.need_f32(t)
        return self.fbin(ops, "recps_arm(A, B)")

    def op_vrsqrts(self, w, a, t, ops):
        self.need_f32(t)
        return self.fbin(ops, "rsqrts_arm(A, B)")

    def op_vpadd(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        if d.kind != "d":
            raise Unsupported("vpadd q")
        if t != ["f32"]:  # entier : sommes des paires adjacentes de Dn puis de Dm
            size, _ = self.int_type(t)
            ct, k = CT[size], 8 // (size // 8)
            return (f"{{ {ct} t_[{2 * k}], r_[{k}]; memcpy(t_, &c->vr.b[{n.off}], 8); memcpy(t_ + {k}, &c->vr.b[{m.off}], 8); "
                    f"for (int i_ = 0; i_ < {k}; i_++) r_[i_] = ({ct})(t_[2 * i_] + t_[2 * i_ + 1]); "
                    f"memcpy(&c->vr.b[{d.off}], r_, 8); }}")
        return ("{ float n_[2], m_[2], r_[2]; memcpy(n_, &c->vr.b[%d], 8); memcpy(m_, &c->vr.b[%d], 8); "
                "r_[0] = nf_out(nf_in(n_[0]) + nf_in(n_[1])); r_[1] = nf_out(nf_in(m_[0]) + nf_in(m_[1])); "
                "memcpy(&c->vr.b[%d], r_, 8); }") % (n.off, m.off, d.off)

    def unary_f(self, ops, expr):
        d, m = Vec(ops[0]), Vec(ops[1])
        e = expr.replace("A", "nf_in(s0_[i_])")
        return self.lanes(d, [m], 32, "float", e)

    def op_vneg(self, w, a, t, ops):
        d, m = Vec(ops[0]), Vec(ops[1])
        if t != ["f32"]:  # entier : complément à deux
            size, _ = self.int_type(t)
            return self.lanes(d, [m], size, CT[size], f"({CT[size]})(0u - s0_[i_])")
        # vneg/vabs flottants : simple bascule de signe, sans FZ ni DN
        return self.lanes(d, [m], 32, "uint32_t", "s0_[i_] ^ 0x80000000u")

    def op_vabs(self, w, a, t, ops):
        d, m = Vec(ops[0]), Vec(ops[1])
        if t != ["f32"]:
            size, _ = self.int_type(t)
            ct, st = CT[size], ST[size]
            return self.lanes(d, [m], size, ct, f"(({st})s0_[i_] < 0) ? ({ct})(0u - s0_[i_]) : s0_[i_]")
        return self.lanes(d, [m], 32, "uint32_t", "s0_[i_] & 0x7FFFFFFFu")

    def op_vrecpe(self, w, a, t, ops):
        self.need_f32(t)
        return self.unary_f(ops, "recpe_arm(A)")

    def op_vrsqrte(self, w, a, t, ops):
        self.need_f32(t)
        return self.unary_f(ops, "rsqrte_arm(A)")

    def op_vcvt(self, w, a, t, ops):
        if len(ops) != 2:
            raise Unsupported("vcvt virgule fixe")
        d, m = Vec(ops[0]), Vec(ops[1])
        if t == ["f32", "s32"]:
            return self.lanes(d, [m], 32, "uint32_t", "f2u(nf_out((float)(int32_t)s0_[i_]))")
        if t == ["f32", "u32"]:
            return self.lanes(d, [m], 32, "uint32_t", "f2u(nf_out((float)s0_[i_]))")
        if t == ["s32", "f32"]:
            return self.lanes(d, [m], 32, "uint32_t", "(uint32_t)cvt_f_s32((double)nf_in(u2f(s0_[i_])))")
        if t == ["u32", "f32"]:
            return self.lanes(d, [m], 32, "uint32_t", "cvt_f_u32((double)nf_in(u2f(s0_[i_])))")
        raise Unsupported(f"vcvt {t}")

    def cmp(self, t, ops, op):
        d, n = Vec(ops[0]), Vec(ops[1])
        if t != ["f32"]:  # entiers (i : égalité, s : signé, u : non signé)
            size, signe = self.int_type(t)
            ct, st = CT[size], (ST[size] if signe else CT[size])
            un = f"({ct})~({ct})0"
            if ops[2].startswith("#"):
                return self.lanes(d, [n], size, ct, f"(({st})s0_[i_] {op} 0) ? {un} : 0")
            m = Vec(ops[2])
            return self.lanes(d, [n, m], size, ct, f"(({st})s0_[i_] {op} ({st})s1_[i_]) ? {un} : 0")
        if ops[2].startswith("#"):
            return self.lanes(d, [n], 32, "uint32_t", f"(nf_in(u2f(s0_[i_])) {op} 0.0f) ? 0xFFFFFFFFu : 0u")
        m = Vec(ops[2])
        return self.lanes(d, [n, m], 32, "uint32_t", f"(nf_in(u2f(s0_[i_])) {op} nf_in(u2f(s1_[i_]))) ? 0xFFFFFFFFu : 0u")

    def op_vceq(self, w, a, t, ops):
        return self.cmp(t, ops, "==")

    def op_vcgt(self, w, a, t, ops):
        return self.cmp(t, ops, ">")

    def op_vcge(self, w, a, t, ops):
        return self.cmp(t, ops, ">=")

    def op_vclt(self, w, a, t, ops):
        return self.cmp(t, ops, "<")

    def op_vcle(self, w, a, t, ops):
        return self.cmp(t, ops, "<=")

    @staticmethod
    def need_f32(t):
        if t != ["f32"]:
            raise Unsupported(f"type {t}")

    @staticmethod
    def int_type(t):
        """'s32' -> (32, True) ; 'u16', 'i8', '32' -> (taille, False)"""
        m = re.fullmatch(r"([siu]?)(\d+)", t[0]) if t else None
        if not m or int(m.group(2)) not in CT:
            raise Unsupported(f"type {t}")
        return int(m.group(2)), m.group(1) == "s"

    @staticmethod
    def imm(txt):
        return int(txt.strip().lstrip("#"), 0)

    # ---------------------------------------------------------------- entiers : test, décalages, largeur
    def op_vtst(self, w, a, t, ops):
        size, _ = self.int_type(t)
        ct = CT[size]
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], size, ct, f"(s0_[i_] & s1_[i_]) ? ({ct})~({ct})0 : 0")

    def op_vshl(self, w, a, t, ops):
        d = Vec(ops[0])
        if ops[-1].startswith("#"):  # immédiat (0 .. taille-1) : décalage logique à gauche
            size, _ = self.int_type(t)
            m = Vec(ops[1]) if len(ops) == 3 else d
            return self.lanes(d, [m], size, CT[size], f"({CT[size]})(s0_[i_] << {self.imm(ops[-1])})")
        # registre : chaque élément de Vm décalé de l'octet de poids faible (signé) de l'élément de Vn ;
        # négatif = décalage à droite (arithmétique si .s), au-delà de la taille : 0 ou signe
        size, signe = self.int_type(t)
        m, n = (Vec(ops[1]), Vec(ops[2])) if len(ops) == 3 else (d, Vec(ops[1]))
        ct, st = CT[size], ST[size]
        x, sh = "s0_[i_]", "((int8_t)s1_[i_])"
        if signe:
            neg = f"({sh} <= -{size} ? ({ct})((({st}){x}) < 0 ? -1 : 0) : ({ct})((({st}){x}) >> -{sh}))"
        else:
            neg = f"({sh} <= -{size} ? ({ct})0 : ({ct})({x} >> -{sh}))"
        return self.lanes(d, [m, n], size, ct, f"({sh} >= {size} ? ({ct})0 : {sh} >= 0 ? ({ct})({x} << {sh}) : {neg})")

    def op_vshr(self, w, a, t, ops):
        size, signe = self.int_type(t)
        d = Vec(ops[0])
        m = Vec(ops[1]) if len(ops) == 3 else d
        sh = self.imm(ops[-1])  # 1 .. taille
        ct, st = CT[size], ST[size]
        if sh >= size:
            e = f"({ct})((({st})s0_[i_]) < 0 ? -1 : 0)" if signe else f"({ct})0"
        else:
            e = f"({ct})((({st})s0_[i_]) >> {sh})" if signe else f"({ct})(s0_[i_] >> {sh})"
        return self.lanes(d, [m], size, ct, e)

    def op_vsra(self, w, a, t, ops):
        """décalage à droite (immédiat) puis cumul dans la destination"""
        size, signe = self.int_type(t)
        d = Vec(ops[0])
        m = Vec(ops[1]) if len(ops) == 3 else d
        sh = self.imm(ops[-1])
        ct, st = CT[size], ST[size]
        if sh >= size:
            e = f"((({st})s0_[i_]) < 0 ? ({ct})-1 : ({ct})0)" if signe else f"({ct})0"
        else:
            e = f"({ct})((({st})s0_[i_]) >> {sh})" if signe else f"({ct})(s0_[i_] >> {sh})"
        return self.lanes(d, [m], size, ct, f"({ct})(d0_[i_] + {e})", dest_in=True)

    def narrow(self, t, ops, decalage):
        size, _ = self.int_type(t)  # taille des éléments de la source (Q)
        d, m = Vec(ops[0]), Vec(ops[1])
        n, h = 16 // (size // 8), size // 2
        return (f"{{ {CT[size]} s_[{n}]; {CT[h]} r_[{n}]; memcpy(s_, &c->vr.b[{m.off}], 16); "
                f"for (int i_ = 0; i_ < {n}; i_++) r_[i_] = ({CT[h]})(s_[i_] >> {decalage}); "
                f"memcpy(&c->vr.b[{d.off}], r_, 8); }}")

    def op_vmovn(self, w, a, t, ops):
        return self.narrow(t, ops, 0)

    def op_vshrn(self, w, a, t, ops):
        return self.narrow(t, ops, self.imm(ops[2]))

    def op_vmovl(self, w, a, t, ops):
        size, signe = self.int_type(t)  # taille des éléments de la source (D)
        d, m = Vec(ops[0]), Vec(ops[1])
        n = 8 // (size // 8)
        src, dst = (ST[size] if signe else CT[size]), CT[2 * size]
        return (f"{{ {src} s_[{n}]; {dst} r_[{n}]; memcpy(s_, &c->vr.b[{m.off}], 8); "
                f"for (int i_ = 0; i_ < {n}; i_++) r_[i_] = ({dst})s_[i_]; memcpy(&c->vr.b[{d.off}], r_, 16); }}")

    def rev(self, t, ops, conteneur):
        size, _ = self.int_type(t)
        d, m = Vec(ops[0]), Vec(ops[1])
        k = conteneur // (size // 8)  # éléments par conteneur
        return self.lanes(d, [m], size, CT[size], f"s0_[(i_ / {k}) * {k} + ({k} - 1 - i_ % {k})]")

    def op_vrev16(self, w, a, t, ops):
        return self.rev(t, ops, 2)

    def op_vrev32(self, w, a, t, ops):
        return self.rev(t, ops, 4)

    def op_vrev64(self, w, a, t, ops):
        return self.rev(t, ops, 8)

    def op_vcnt(self, w, a, t, ops):
        d, m = Vec(ops[0]), Vec(ops[1])
        return self.lanes(d, [m], 8, "uint8_t", "(uint8_t)__builtin_popcount(s0_[i_])")

    def intbin(self, t, ops, expr):
        size = int(re.sub(r"[a-z]", "", t[0]))
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], size, CT[size], f"({CT[size]})({expr})")

    # ---------------------------------------------------------------- bit à bit, mouvements
    def op_vorr(self, w, a, t, ops):
        if ops[-1].startswith("#"):
            raise Unsupported("vorr imm")
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "s0_[i_] | s1_[i_]")

    def op_vand(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "s0_[i_] & s1_[i_]")

    def op_veor(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "s0_[i_] ^ s1_[i_]")

    def op_vbic(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "s0_[i_] & ~s1_[i_]")

    def op_vbsl(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "(d0_[i_] & s0_[i_]) | (~d0_[i_] & s1_[i_])", dest_in=True)

    def op_vbit(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "(d0_[i_] & ~s1_[i_]) | (s0_[i_] & s1_[i_])", dest_in=True)

    def op_vbif(self, w, a, t, ops):
        d, n, m = (Vec(x) for x in ops)
        return self.lanes(d, [n, m], 64, "uint64_t", "(d0_[i_] & s1_[i_]) | (s0_[i_] & ~s1_[i_])", dest_in=True)

    def op_vmov(self, w, a, t, ops):
        d = Vec(ops[0])
        if not ops[1].startswith("#"):
            m = Vec(ops[1])
            return self.lanes(d, [m], 64, "uint64_t", "s0_[i_]")
        imm8 = (bits(w, 24, 24) << 7) | (bits(w, 18, 16) << 4) | bits(w, 3, 0)
        val = adv_simd_imm(bits(w, 5, 5), bits(w, 11, 8), imm8)
        n = d.size // 8
        return (f"{{ uint64_t x_ = 0x{val:016X}ull; for (int i_ = 0; i_ < {n}; i_++) "
                f"memcpy(&c->vr.b[{d.off} + 8 * i_], &x_, 8); }}")

    def op_vmvn(self, w, a, t, ops):
        d = Vec(ops[0])
        if not ops[1].startswith("#"):
            m = Vec(ops[1])
            return self.lanes(d, [m], 64, "uint64_t", "~s0_[i_]")
        imm8 = (bits(w, 24, 24) << 7) | (bits(w, 18, 16) << 4) | bits(w, 3, 0)
        val = ~adv_simd_imm(bits(w, 5, 5), bits(w, 11, 8), imm8) & 0xFFFFFFFFFFFFFFFF
        n = d.size // 8
        return (f"{{ uint64_t x_ = 0x{val:016X}ull; for (int i_ = 0; i_ < {n}; i_++) "
                f"memcpy(&c->vr.b[{d.off} + 8 * i_], &x_, 8); }}")

    def op_vdup(self, w, a, t, ops):
        d, m = Vec(ops[0]), Vec(ops[1])
        if m.idx is None:
            raise Unsupported("vdup")
        size = int(t[0])
        return self.lanes(d, [m], size, CT[size], "s0_[i_]")

    def op_vext(self, w, a, t, ops):
        d, n, m = Vec(ops[0]), Vec(ops[1]), Vec(ops[2])
        imm = bits(w, 11, 8)  # en octets
        sz = d.size
        return (f"{{ uint8_t t_[32]; memcpy(t_, &c->vr.b[{n.off}], {sz}); memcpy(t_ + {sz}, &c->vr.b[{m.off}], {sz}); "
                f"memcpy(&c->vr.b[{d.off}], t_ + {imm}, {sz}); }}")

    def op_vtbl(self, w, a, t, ops):
        d = Vec(ops[0])
        lst = [Vec(x) for x in ops[1].strip("{}").split(",")]
        m = Vec(ops[2])
        nb = 8 * len(lst)
        cp = " ".join(f"memcpy(t_ + {8 * k}, &c->vr.b[{v.off}], 8);" for k, v in enumerate(lst))
        return (f"{{ uint8_t t_[32], x_[8], r_[8]; {cp} memcpy(x_, &c->vr.b[{m.off}], 8); "
                f"for (int i_ = 0; i_ < 8; i_++) r_[i_] = x_[i_] < {nb} ? t_[x_[i_]] : 0; "
                f"memcpy(&c->vr.b[{d.off}], r_, 8); }}")

    def perm(self, t, ops, kind):
        size = int(t[0])
        d, m = Vec(ops[0]), Vec(ops[1])
        n = d.size // (size // 8)
        ct = CT[size]
        h = n // 2
        if kind == "zip":
            body = (f"for (int i_ = 0; i_ < {h}; i_++) {{ r0_[2*i_] = a_[i_]; r0_[2*i_+1] = b_[i_]; "
                    f"r1_[2*i_] = a_[i_+{h}]; r1_[2*i_+1] = b_[i_+{h}]; }}")
        elif kind == "uzp":
            body = (f"for (int i_ = 0; i_ < {h}; i_++) {{ r0_[i_] = a_[2*i_]; r0_[i_+{h}] = b_[2*i_]; "
                    f"r1_[i_] = a_[2*i_+1]; r1_[i_+{h}] = b_[2*i_+1]; }}")
        else:  # trn
            body = (f"for (int i_ = 0; i_ < {h}; i_++) {{ r0_[2*i_] = a_[2*i_]; r0_[2*i_+1] = b_[2*i_]; "
                    f"r1_[2*i_] = a_[2*i_+1]; r1_[2*i_+1] = b_[2*i_+1]; }}")
        return (f"{{ {ct} a_[{n}], b_[{n}], r0_[{n}], r1_[{n}]; memcpy(a_, &c->vr.b[{d.off}], {d.size}); "
                f"memcpy(b_, &c->vr.b[{m.off}], {m.size}); {body} memcpy(&c->vr.b[{d.off}], r0_, {d.size}); "
                f"memcpy(&c->vr.b[{m.off}], r1_, {m.size}); }}")

    def op_vzip(self, w, a, t, ops):
        return self.perm(t, ops, "zip")

    def op_vuzp(self, w, a, t, ops):
        return self.perm(t, ops, "uzp")

    def op_vtrn(self, w, a, t, ops):
        return self.perm(t, ops, "trn")
