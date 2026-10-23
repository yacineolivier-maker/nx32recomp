"""Traduction d'instructions A32 en C.

Les instructions entières sont décodées depuis les bits de l'encodage (exactitude des retenues et
décalages) ; VFP/NEON passent par vfp.py. Chaque méthode renvoie le corps C de l'instruction, sans la
condition : `wrap_cond` l'entoure d'un `if`. Les écritures de PC sont déléguées au contexte de
fonction (ctx) qui sait s'il s'agit d'un retour, d'un saut interne ou d'un appel terminal.
"""

CONDS = ["C_EQ", "C_NE", "C_CS", "C_CC", "C_MI", "C_PL", "C_VS", "C_VC",
         "C_HI", "C_LS", "C_GE", "C_LT", "C_GT", "C_LE", None, None]


class Unsupported(Exception):
    pass


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sx(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def ror(x, n):
    n &= 31
    return ((x >> n) | (x << (32 - n))) & 0xFFFFFFFF if n else x


class PcSink:
    """Contexte minimal : toute écriture de PC devient un saut indirect générique."""

    def jump(self, expr, addr, kind):
        return f"rt_call_indirect(c, {expr}); return;"

    def branch(self, target, addr, link):
        if link:
            return f"c->r[14] = 0x{addr + 4:X}u; rt_call_indirect(c, 0x{target:X}u);"
        return f"rt_call_indirect(c, 0x{target:X}u); return;"


class Lifter:
    def __init__(self, ctx=None, vfp=None):
        self.ctx = ctx or PcSink()
        self.vfp = vfp

    # ------------------------------------------------------------------ utilitaires
    def reg(self, n, addr, extra=8):
        return f"0x{(addr + extra) & 0xFFFFFFFF:X}u" if n == 15 else f"c->r[{n}]"

    def set_reg(self, n, expr, addr, kind="jump"):
        if n == 15:
            return self.ctx.jump(f"({expr})", addr, kind)
        return f"c->r[{n}] = {expr};"

    def shift_imm(self, w, addr, need_carry):
        """Opérande Rm décalé par immédiat -> (expr, expr_retenue)."""
        rm = self.reg(bits(w, 3, 0), addr)
        typ, n = bits(w, 6, 5), bits(w, 11, 7)
        if typ == 0:
            if n == 0:
                return rm, "c->c"
            return f"({rm} << {n})", f"(({rm} >> {32 - n}) & 1)"
        if typ == 1:
            if n == 0:
                return "0u", f"({rm} >> 31)"
            return f"({rm} >> {n})", f"(({rm} >> {n - 1}) & 1)"
        if typ == 2:
            if n == 0:
                return f"(uint32_t)((int32_t){rm} >> 31)", f"({rm} >> 31)"
            return f"(uint32_t)((int32_t){rm} >> {n})", f"(({rm} >> {n - 1}) & 1)"
        if n == 0:  # RRX
            return f"(({rm} >> 1) | (c->c << 31))", f"({rm} & 1)"
        return f"ror32({rm}, {n})", f"(({rm} >> {n - 1}) & 1)"

    # ------------------------------------------------------------------ point d'entrée
    def lift(self, w, addr):
        cond = w >> 28
        if cond == 0xF:
            return self.uncond(w, addr)
        body = self.dispatch(w, addr)
        return self.wrap_cond(cond, body)

    @staticmethod
    def wrap_cond(cond, body):
        if CONDS[cond] is None:
            return body
        return f"if ({CONDS[cond]}) {{ {body} }}"

    def dispatch(self, w, addr):
        op1 = bits(w, 27, 25)
        if op1 in (0, 1):
            return self.dp_space(w, addr)
        if op1 == 2:
            return self.ldst(w, addr)
        if op1 == 3:
            if w & 0x10:
                return self.media(w, addr)
            return self.ldst(w, addr)
        if op1 == 4:
            return self.ldm_stm(w, addr)
        if op1 == 5:
            target = (addr + 8 + sx(bits(w, 23, 0), 24) * 4) & 0xFFFFFFFF
            return self.ctx.branch(target, addr, link=bool(w & (1 << 24)))
        if bits(w, 27, 24) == 0xF:
            return f"rt_svc(c, 0x{bits(w, 23, 0):X}u);"
        return self.coproc(w, addr)

    # ------------------------------------------------------------------ traitement de données
    def dp_space(self, w, addr):
        I = bits(w, 25, 25)
        op = bits(w, 24, 21)
        S = bits(w, 20, 20)
        if not I:
            if bits(w, 7, 4) == 0b1001:
                return self.mul_sync(w, addr)
            if bits(w, 7, 7) and bits(w, 4, 4):
                return self.extra_ldst(w, addr)
            if op >> 2 == 0b10 and not S:
                return self.misc(w, addr)
        else:
            if op == 0b1000 and not S:  # MOVW
                imm = (bits(w, 19, 16) << 12) | bits(w, 11, 0)
                return self.set_reg(bits(w, 15, 12), f"0x{imm:X}u", addr)
            if op == 0b1010 and not S:  # MOVT
                imm = (bits(w, 19, 16) << 12) | bits(w, 11, 0)
                rd = bits(w, 15, 12)
                return f"c->r[{rd}] = (c->r[{rd}] & 0xFFFFu) | 0x{imm << 16:X}u;"
            if op >> 2 == 0b10 and not S:  # MSR immédiat / hints (nop, yield...)
                if bits(w, 19, 16) == 0:
                    if bits(w, 7, 0) in (1, 2):  # YIELD / WFE : attente active -> pause + barrière du compilateur
                        return "rt_pause();"
                    return ";"
                raise Unsupported("msr imm")
        return self.data_proc(w, addr)

    def data_proc(self, w, addr):
        I, op, S = bits(w, 25, 25), bits(w, 24, 21), bits(w, 20, 20)
        rn, rd = bits(w, 19, 16), bits(w, 15, 12)
        logical = op in (0, 1, 8, 9, 12, 13, 14, 15)
        need_carry = S and logical
        pre = []
        if I:
            rot = bits(w, 11, 8) * 2
            val = ror(bits(w, 7, 0), rot)
            o2 = f"0x{val:X}u"
            co = f"{val >> 31}u" if rot else "c->c"
        elif not bits(w, 4, 4):
            o2, co = self.shift_imm(w, addr, need_carry)
        else:
            if bits(w, 3, 0) == 15 or bits(w, 11, 8) == 15:
                raise Unsupported("décalage registre avec pc")
            fn = ["sh_lsl", "sh_lsr", "sh_asr", "sh_ror"][bits(w, 6, 5)]
            pre.append(f"uint32_t co_; uint32_t o2_ = {fn}(c->r[{bits(w, 3, 0)}], c->r[{bits(w, 11, 8)}], c->c, &co_);")
            o2, co = "o2_", "co_"
        a = self.reg(rn, addr)
        s = 1 if S and rd != 15 else 0
        if S and rd == 15:
            raise Unsupported("dp S avec rd=pc (retour d'exception)")

        if op in (8, 9, 10, 11):  # TST TEQ CMP CMN : drapeaux seulement
            if op == 8:
                body = f"uint32_t co2_ = {co}; nz32(c, {a} & {o2}); c->c = co2_;"
            elif op == 9:
                body = f"uint32_t co2_ = {co}; nz32(c, {a} ^ {o2}); c->c = co2_;"
            elif op == 10:
                body = f"adc32(c, {a}, ~{o2}, 1, 1);"
            else:
                body = f"adc32(c, {a}, {o2}, 0, 1);"
            return "{ " + " ".join(pre + [body]) + " }"

        if op == 0:
            res = f"{a} & {o2}"
        elif op == 1:
            res = f"{a} ^ {o2}"
        elif op == 2:
            res = f"adc32(c, {a}, ~{o2}, 1, {s})"
        elif op == 3:
            res = f"adc32(c, {o2}, ~{a}, 1, {s})"
        elif op == 4:
            res = f"adc32(c, {a}, {o2}, 0, {s})"
        elif op == 5:
            res = f"adc32(c, {a}, {o2}, c->c, {s})"
        elif op == 6:
            res = f"adc32(c, {a}, ~{o2}, c->c, {s})"
        elif op == 7:
            res = f"adc32(c, {o2}, ~{a}, c->c, {s})"
        elif op == 12:
            res = f"{a} | {o2}"
        elif op == 13:
            res = o2
        elif op == 14:
            res = f"{a} & ~{o2}"
        else:
            res = f"~{o2}"
        if logical and S:
            body = [f"uint32_t co2_ = {co};", f"uint32_t r_ = nz32(c, {res});", "c->c = co2_;",
                    self.set_reg(rd, "r_", addr)]
        else:
            body = [f"uint32_t r_ = {res};", self.set_reg(rd, "r_", addr)]
        return "{ " + " ".join(pre + body) + " }"

    # ------------------------------------------------------------------ multiplications / exclusifs
    def mul_sync(self, w, addr):
        if bits(w, 24, 24):
            return self.sync(w, addr)
        op, S = bits(w, 23, 21), bits(w, 20, 20)
        rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
        R = lambda x: f"c->r[{x}]"
        flag = (lambda e: f"nz32(c, {e})") if S else (lambda e: e)
        if op == 0:
            return f"c->r[{rd}] = {flag(f'{R(rn)} * {R(rm)}')};"
        if op == 1:
            return f"c->r[{rd}] = {flag(f'{R(rn)} * {R(rm)} + {R(ra)}')};"
        if op == 2:  # UMAAL
            return (f"{{ uint64_t p_ = (uint64_t){R(rn)} * {R(rm)} + {R(ra)} + {R(rd)}; "
                    f"c->r[{ra}] = (uint32_t)p_; c->r[{rd}] = (uint32_t)(p_ >> 32); }}")
        if op == 3:
            return f"c->r[{rd}] = {R(ra)} - {R(rn)} * {R(rm)};"
        signed = op in (6, 7)
        acc = op in (5, 7)
        t = "int64_t" if signed else "uint64_t"
        ct = "int32_t" if signed else "uint32_t"
        e = f"({t})({ct}){R(rn)} * ({t})({ct}){R(rm)}"
        if acc:
            e = f"(uint64_t)({e}) + (((uint64_t){R(rd)} << 32) | {R(ra)})"
        body = f"uint64_t p_ = (uint64_t)({e}); c->r[{ra}] = (uint32_t)p_; c->r[{rd}] = (uint32_t)(p_ >> 32);"
        if S:
            body += " c->n = (uint32_t)(p_ >> 63); c->z = p_ == 0;"
        return "{ " + body + " }"

    def sync(self, w, addr):
        """LDREX/STREX (+ variantes b/h/d) et LDA/STL/LDAEX/STLEX (ARMv8).
        Exclusifs : ldrex mémorise adresse et valeur ; strex = comparaison-échange atomique hôte."""
        op = bits(w, 22, 21)          # 0 mot, 1 double, 2 octet, 3 demi
        L = bits(w, 20, 20)
        rn, rt = bits(w, 19, 16), bits(w, 15, 12)
        ex = bits(w, 9, 8)            # 3 = exclusif (ARMv7 et ARMv8), 2 = acquire/release exclusif, 0 = lda/stl
        if bits(w, 23, 23) == 0:
            raise Unsupported("swp")
        exclusive = ex in (2, 3)
        acq_rel = ex in (0, 2)        # LDA/STL/LDAEX/STLEX : sémantique acquisition/libération (ARMv8, RCsc)
        taille = {0: 4, 1: 8, 2: 1, 3: 2}[op]
        a = f"c->r[{rn}]"
        if L:
            # LDA/LDAEX : lecture atomique « acquire » (aucun accès ultérieur ne remonte avant elle, ni dans le
            # compilateur C ni dans le processeur) ; LDREX : lecture atomique simple
            suffixe = {1: "64", 0: "32", 2: "8", 3: "16"}[op]
            lect = f"lda{suffixe}({a})" if acq_rel else f"ldx{suffixe}({a})"
            if op == 1:
                body = f"uint64_t v_ = {lect}; c->r[{rt}] = (uint32_t)v_; c->r[{rt + 1}] = (uint32_t)(v_ >> 32);"
                if exclusive:
                    body += f" c->excl_val = v_;"
            else:
                body = f"uint32_t v_ = {lect}; c->r[{rt}] = v_;"
                if exclusive:
                    body += " c->excl_val = v_;"
            if exclusive:
                body = f"c->excl_addr = {a}; c->excl_valid = 1; c->excl_seq = excl_seq_lire({a}); " + body
            return "{ " + body + " }"
        rs = bits(w, 3, 0)
        if not exclusive:  # STL : rangement « release », séquentiellement cohérent avec les LDA suivants (RCsc)
            if op == 1:
                return f"stl64({a}, ((uint64_t)c->r[{rs + 1}] << 32) | c->r[{rs}]);"
            return f"stl{ {0: '32', 2: '8', 3: '16'}[op] }({a}, c->r[{rs}]);"
        v = f"(((uint64_t)c->r[{rs + 1}] << 32) | c->r[{rs}])" if op == 1 else f"(uint64_t)c->r[{rs}]"
        return f"{{ uint32_t ok_ = excl_strex(c, {a}, {v}, {taille}); c->r[{rt}] = ok_ ? 0 : 1; c->excl_valid = 0; }}"

    # ------------------------------------------------------------------ divers (bx, clz, mrs...)
    def misc(self, w, addr):
        op2 = bits(w, 6, 4)
        op = bits(w, 22, 21)
        rm = bits(w, 3, 0)
        if bits(w, 7, 7) and not bits(w, 4, 4):  # SMULxy / SMLAxy / SMLAWy / SMULWy / SMLALxy
            return self.smul16(w, addr)
        if op2 == 1 and op == 1:  # BX
            return self.ctx.jump(f"c->r[{rm}]", addr, "ret" if rm == 14 else "bx")
        if op2 == 3 and op == 1:  # BLX registre
            return f"{{ uint32_t t_ = c->r[{rm}]; c->r[14] = 0x{addr + 4:X}u; rt_call_indirect(c, t_); }}"
        if op2 == 1 and op == 3:  # CLZ
            return f"c->r[{bits(w, 15, 12)}] = clz32(c->r[{rm}]);"
        if op2 == 0 and op in (0, 2) and not bits(w, 9, 9):  # MRS
            if op == 2:
                raise Unsupported("mrs spsr")
            return (f"c->r[{bits(w, 15, 12)}] = (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) "
                    f"| (c->q << 27) | (c->ge << 16) | 0x10u;")
        if op2 == 0 and op == 1:  # MSR APSR registre
            mask = bits(w, 19, 16)
            r = f"c->r[{rm}]"
            out = []
            if mask & 8:
                out.append(f"c->n = {r} >> 31; c->z = ({r} >> 30) & 1; c->c = ({r} >> 29) & 1; "
                           f"c->v = ({r} >> 28) & 1; c->q = ({r} >> 27) & 1;")
            if mask & 4:
                out.append(f"c->ge = ({r} >> 16) & 15;")
            return "{ " + " ".join(out) + " }" if out else ";"
        if op2 == 5:  # QADD/QSUB/QDADD/QDSUB
            rn, rd = bits(w, 19, 16), bits(w, 15, 12)
            A, B = f"(int64_t)(int32_t)c->r[{rm}]", f"(int64_t)(int32_t)c->r[{rn}]"
            if op & 2:
                B = f"(int64_t)(int32_t)sat_s(2 * {B}, 32, c)"
            e = f"{A} + {B}" if op & 1 == 0 else f"{A} - {B}"
            return f"c->r[{rd}] = sat_s({e}, 32, c);"
        if bits(w, 7, 7) and not bits(w, 4, 4):  # SMULxy / SMLAxy / SMLAWy / SMULWy / SMLALxy
            return self.smul16(w, addr)
        raise Unsupported(f"misc op={op} op2={op2}")

    def smul16(self, w, addr):
        op = bits(w, 22, 21)
        rd, ra, rm, rn = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
        x, y = bits(w, 5, 5), bits(w, 6, 6)
        half = lambda r, top: f"(int32_t)(int16_t)(c->r[{r}] >> {16 if top else 0})"
        if op == 0:  # SMLAxy (Q si débordement)
            return f"{{ int64_t s_ = (int64_t)({half(rn, x)} * {half(rm, y)}) + (int32_t)c->r[{ra}]; " \
                   f"if (s_ != (int32_t)s_) c->q = 1; c->r[{rd}] = (uint32_t)s_; }}"
        if op == 3:  # SMULxy
            return f"c->r[{rd}] = (uint32_t)({half(rn, x)} * {half(rm, y)});"
        if op == 1:
            p = f"(((int64_t)(int32_t)c->r[{rn}] * {half(rm, y)}) >> 16)"
            if x:  # SMULWy
                return f"c->r[{rd}] = (uint32_t){p};"
            return f"{{ int64_t s_ = {p} + (int32_t)c->r[{ra}]; if (s_ != (int32_t)s_) c->q = 1; c->r[{rd}] = (uint32_t)s_; }}"
        # SMLALxy
        return (f"{{ int64_t acc_ = (int64_t)(((uint64_t)c->r[{rd}] << 32) | c->r[{ra}]); "
                f"acc_ += (int64_t)({half(rn, x)} * {half(rm, y)}); "
                f"c->r[{ra}] = (uint32_t)acc_; c->r[{rd}] = (uint32_t)((uint64_t)acc_ >> 32); }}")

    # ------------------------------------------------------------------ chargements / rangements
    def addressing(self, w, addr, off_expr):
        P, U, W = bits(w, 24, 24), bits(w, 23, 23), bits(w, 21, 21)
        rn = bits(w, 19, 16)
        base = self.reg(rn, addr)
        sign = "+" if U else "-"
        off = f"({base} {sign} {off_expr})"
        ea = off if P else base
        wb = None
        if not P or W:
            wb = f"c->r[{rn}] = {off};"
        return ea, wb, rn

    def ldst(self, w, addr):
        I, B, L = bits(w, 25, 25), bits(w, 22, 22), bits(w, 20, 20)
        rt = bits(w, 15, 12)
        if I:
            if bits(w, 3, 0) == 15:
                raise Unsupported("ldr index pc")
            o, _ = self.shift_imm(w, addr, False)
        else:
            o = f"0x{bits(w, 11, 0):X}u"
        ea, wb, rn = self.addressing(w, addr, o)
        return self.emit_ldst(L, "8" if B else "32", False, rt, ea, wb, rn, addr)

    def emit_ldst(self, L, size, signed, rt, ea, wb, rn, addr):
        out = [f"uint32_t ea_ = {ea};"]
        if L:
            ld = f"rd{size}(ea_)"
            if signed:
                ld = f"(uint32_t)(int32_t)(int{size}_t){ld}"
            out.append(f"uint32_t v_ = {ld};")
            if wb:
                out.append(wb)
            out.append(self.set_reg(rt, "v_", addr, "ldr"))
        else:
            out.append(f"wr{size}(ea_, {self.reg(rt, addr, 12 if rt == 15 else 8)});")
            if wb:
                out.append(wb)
        return "{ " + " ".join(out) + " }"

    def extra_ldst(self, w, addr):
        L, imm = bits(w, 20, 20), bits(w, 22, 22)
        sh = bits(w, 6, 5)
        rt = bits(w, 15, 12)
        if imm:
            o = f"0x{(bits(w, 11, 8) << 4) | bits(w, 3, 0):X}u"
        else:
            o = self.reg(bits(w, 3, 0), addr)
        ea, wb, rn = self.addressing(w, addr, o)
        if sh == 1:
            return self.emit_ldst(L, "16", False, rt, ea, wb, rn, addr)
        if L:
            return self.emit_ldst(1, "8" if sh == 2 else "16", True, rt, ea, wb, rn, addr)
        if sh == 2:  # LDRD
            out = [f"uint32_t ea_ = {ea};", f"uint32_t lo_ = rd32(ea_), hi_ = rd32(ea_ + 4);"]
            if wb:
                out.append(wb)
            out += [f"c->r[{rt}] = lo_;", f"c->r[{rt + 1}] = hi_;"]
        else:  # STRD
            out = [f"uint32_t ea_ = {ea};", f"wr32(ea_, c->r[{rt}]); wr32(ea_ + 4, c->r[{rt + 1}]);"]
            if wb:
                out.append(wb)
        return "{ " + " ".join(out) + " }"

    def ldm_stm(self, w, addr):
        P, U, S, W, L = (bits(w, 24, 24), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20))
        if S:
            raise Unsupported("ldm/stm ^")
        rn = bits(w, 19, 16)
        regs = [r for r in range(16) if w & (1 << r)]
        n = len(regs)
        if U:
            start = f"c->r[{rn}] + {4 if P else 0}"
            new = f"c->r[{rn}] + {4 * n}"
        else:
            start = f"c->r[{rn}] - {4 * n if P else 4 * n - 4}"
            new = f"c->r[{rn}] - {4 * n}"
        out = [f"uint32_t ea_ = {start}; uint32_t nb_ = {new};"]
        if L:
            for k, r in enumerate(regs):
                if r == 15:
                    out.append(f"uint32_t pc_ = rd32(ea_ + {4 * k});")
                else:
                    out.append(f"uint32_t t{r}_ = rd32(ea_ + {4 * k});")
            if W and rn not in regs:
                out.append(f"c->r[{rn}] = nb_;")
            for r in regs:
                if r != 15:
                    out.append(f"c->r[{r}] = t{r}_;")
            if 15 in regs:
                out.append(self.ctx.jump("pc_", 0, "ret"))
        else:
            for k, r in enumerate(regs):
                out.append(f"wr32(ea_ + {4 * k}, {self.reg(r, addr)});")
            if W:
                out.append(f"c->r[{rn}] = nb_;")
        return "{ " + " ".join(out) + " }"

    # ------------------------------------------------------------------ media
    def media(self, w, addr):
        op1 = bits(w, 24, 20)
        op2 = bits(w, 7, 5)
        rd, rn, rm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
        R = lambda x: f"c->r[{x}]"
        if op1 >> 3 == 0b01 and op2 & 3 == 3 and bits(w, 9, 4) & 0b111 == 0b111:  # extensions
            rot = bits(w, 11, 10) * 8
            src = f"ror32({R(rm)}, {rot})" if rot else R(rm)
            kind = bits(w, 22, 20)
            ext = {0b010: "(uint32_t)(int32_t)(int8_t)", 0b011: "(uint32_t)(int32_t)(int16_t)",
                   0b110: "(uint32_t)(uint8_t)", 0b111: "(uint32_t)(uint16_t)"}
            if kind in ext:
                e = f"{ext[kind]}{src}"
                return f"{R(rd)} = {e};" if rn == 15 else f"{R(rd)} = {R(rn)} + {e};"
            if kind == 0b100:  # UXTB16 / UXTAB16
                e = f"({src} & 0x00FF00FFu)"
                if rn == 15:
                    return f"{R(rd)} = {e};"
                return (f"{{ uint32_t e_ = {e}; {R(rd)} = ((({R(rn)} & 0xFFFF) + (e_ & 0xFFFF)) & 0xFFFF) | "
                        f"((({R(rn)} >> 16) + (e_ >> 16)) << 16); }}")
            if kind == 0b000:  # SXTB16
                e = (f"(((uint32_t)(int32_t)(int8_t){src} & 0xFFFF) | "
                     f"(((uint32_t)(int32_t)(int8_t)({src} >> 16)) << 16))")
                if rn == 15:
                    return f"{R(rd)} = {e};"
            raise Unsupported(f"extension {kind}")
        if op1 == 0b01011 and op2 == 1:  # REV
            return f"{R(rd)} = __builtin_bswap32({R(rm)});"
        if op1 == 0b01011 and op2 == 5:  # REV16
            return f"{R(rd)} = ((({R(rm)} >> 8) & 0x00FF00FFu) | (({R(rm)} << 8) & 0xFF00FF00u));"
        if op1 == 0b01111 and op2 == 5:  # REVSH
            return f"{R(rd)} = (uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t){R(rm)});"
        if op1 == 0b01111 and op2 == 1:  # RBIT
            return f"{R(rd)} = __builtin_bitreverse32({R(rm)});"
        if op1 == 0b01000 and op2 & 1 == 0:  # PKHBT / PKHTB
            imm = bits(w, 11, 7)
            if op2 & 2:
                sh = f"(uint32_t)((int32_t){R(rm)} >> {imm if imm else 31})"
                return f"{R(rd)} = ({R(rn)} & 0xFFFF0000u) | ({sh} & 0xFFFFu);"
            return f"{R(rd)} = ({R(rn)} & 0xFFFFu) | (({R(rm)} << {imm}) & 0xFFFF0000u);"
        if op1 >> 1 in (0b0101, 0b0111) and op2 & 1 == 0:  # SSAT / USAT
            imm = bits(w, 11, 7)
            sh = (f"(int64_t)(int32_t)((uint32_t)((int32_t){R(rm)} >> {imm if imm else 31}))" if bits(w, 6, 6)
                  else f"(int64_t)(int32_t)({R(rm)} << {imm})")
            n = bits(w, 20, 16)
            if op1 >> 1 == 0b0101:
                return f"{R(rd)} = sat_s({sh}, {n + 1}, c);"
            return f"{R(rd)} = sat_u({sh}, {n}, c);"
        if op1 >> 1 in (0b1101, 0b1111) and op2 & 3 == 2:  # SBFX / UBFX
            lsb, wm1 = bits(w, 11, 7), bits(w, 20, 16)
            width = wm1 + 1
            if op1 >> 1 == 0b1111:
                mask = (1 << width) - 1
                return f"{R(rd)} = ({R(rm)} >> {lsb}) & 0x{mask:X}u;"
            return f"{R(rd)} = (uint32_t)((int32_t)({R(rm)} << {32 - lsb - width}) >> {32 - width});"
        if op1 >> 1 == 0b1110 and op2 & 3 == 0:  # BFI / BFC
            lsb, msb = bits(w, 11, 7), bits(w, 20, 16)
            mask = ((1 << (msb - lsb + 1)) - 1) << lsb
            if rm == 15:
                return f"{R(rd)} &= 0x{~mask & 0xFFFFFFFF:X}u;"
            return f"{R(rd)} = ({R(rd)} & 0x{~mask & 0xFFFFFFFF:X}u) | (({R(rm)} << {lsb}) & 0x{mask:X}u);"
        if op1 in (0b10001, 0b10011) and op2 == 0:  # SDIV / UDIV (rd en 19-16, rn 3-0, rm 11-8)
            d, n_, m_ = bits(w, 19, 16), bits(w, 3, 0), bits(w, 11, 8)
            if op1 == 0b10001:
                return (f"{{ int32_t a_ = (int32_t)c->r[{n_}], b_ = (int32_t)c->r[{m_}]; "
                        f"c->r[{d}] = b_ == 0 ? 0 : (a_ == INT32_MIN && b_ == -1) ? (uint32_t)INT32_MIN : (uint32_t)(a_ / b_); }}")
            return f"{{ uint32_t b_ = c->r[{m_}]; c->r[{d}] = b_ ? c->r[{n_}] / b_ : 0; }}"
        if op1 == 0b10101 and op2 in (0, 1):  # SMMLA / SMMUL (+R)
            d, a_, m_, n_ = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            rnd = " + 0x80000000LL" if op2 == 1 else ""
            prod = f"(int64_t)(int32_t)c->r[{n_}] * (int32_t)c->r[{m_}]"
            if a_ == 15:
                return f"c->r[{d}] = (uint32_t)((uint64_t)({prod}{rnd}) >> 32);"
            return f"c->r[{d}] = (uint32_t)((uint64_t)((((int64_t)(int32_t)c->r[{a_}]) << 32) + {prod}{rnd}) >> 32);"
        if op1 == 0b10101 and op2 in (6, 7):  # SMMLS
            d, a_, m_, n_ = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            rnd = " + 0x80000000LL" if op2 == 7 else ""
            return (f"c->r[{d}] = (uint32_t)((uint64_t)((((int64_t)(int32_t)c->r[{a_}]) << 32) - "
                    f"(int64_t)(int32_t)c->r[{n_}] * (int32_t)c->r[{m_}]{rnd}) >> 32);")
        if op1 == 0b10000 and op2 in (0, 1) and bits(w, 15, 12) != 15:  # SMLAD(X)
            d, a_, m_, n_ = bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 8), bits(w, 3, 0)
            mm = f"ror32(c->r[{m_}], 16)" if op2 else f"c->r[{m_}]"
            return (f"{{ uint32_t m2_ = {mm}; int64_t s_ = (int64_t)((int16_t)c->r[{n_}] * (int16_t)m2_) + "
                    f"(int64_t)((int16_t)(c->r[{n_}] >> 16) * (int16_t)(m2_ >> 16)) + (int32_t)c->r[{a_}]; "
                    f"if (s_ != (int32_t)s_) c->q = 1; c->r[{d}] = (uint32_t)s_; }}")
        if op1 == 0b11111 and op2 == 7:
            return f"rt_undefined(c, 0x{addr:X}u);"
        raise Unsupported(f"media op1={op1:05b} op2={op2:03b}")

    # ------------------------------------------------------------------ coprocesseurs, VFP, NEON
    def coproc(self, w, addr):
        coproc = bits(w, 11, 8)
        if coproc == 15 and bits(w, 27, 21) == 0b1100010:  # MRRC/MCRR p15 (compteur générique)
            rt, rt2, crm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
            if bits(w, 20, 20) and crm == 14:  # CNTPCT / CNTVCT
                return f"{{ uint64_t t_ = rt_ticks(); c->r[{rt}] = (uint32_t)t_; c->r[{rt2}] = (uint32_t)(t_ >> 32); }}"
            raise Unsupported(f"mrrc/mcrr p15 crm={crm}")
        if coproc == 15 and bits(w, 27, 24) == 0xE and bits(w, 4, 4):  # MRC/MCR p15
            L = bits(w, 20, 20)
            crn, crm, opc1, opc2 = bits(w, 19, 16), bits(w, 3, 0), bits(w, 23, 21), bits(w, 7, 5)
            rt = bits(w, 15, 12)
            if L and (crn, crm, opc1, opc2) == (13, 0, 0, 3):
                return f"c->r[{rt}] = c->tpidruro;"
            if (crn, crm, opc1, opc2) == (13, 0, 0, 2):
                return f"c->r[{rt}] = c->tpidrurw;" if L else f"c->tpidrurw = c->r[{rt}];"
            if L and (crn, crm, opc1, opc2) == (14, 0, 0, 0):  # CNTFRQ
                return f"c->r[{rt}] = 19200000u;"
            if not L and crn == 7 and crm in (5, 10):  # barrières CP15 historiques (DMB/DSB)
                return "__sync_synchronize();"
            raise Unsupported(f"cp15 crn={crn} crm={crm} opc1={opc1} opc2={opc2} L={L}")
        if coproc in (10, 11) and self.vfp:
            return self.vfp.lift(w, addr)
        raise Unsupported(f"coproc {coproc}")

    def uncond(self, w, addr):
        if bits(w, 27, 25) == 0b101:  # BLX immédiat (bascule en Thumb)
            raise Unsupported("blx imm (Thumb)")
        if w & 0xFFF000F0 == 0xF5700010:  # CLREX
            return "c->excl_valid = 0;"
        if w & 0xFFFFFFF0 in (0xF57FF040, 0xF57FF050):  # DSB DMB : barrière complète (ordre entre threads)
            return "__sync_synchronize();"
        if w & 0xFFFFFFF0 == 0xF57FF060:  # ISB
            return ";"
        if w & 0xFD70F000 == 0xF550F000 or w & 0xFD70F000 == 0xF510F000 or w & 0xFF70F000 == 0xF450F000:  # PLD/PLDW/PLI
            return ";"
        if self.vfp:
            return self.vfp.lift(w, addr)
        raise Unsupported(f"inconditionnelle {w:08X}")
