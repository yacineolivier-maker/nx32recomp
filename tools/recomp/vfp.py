"""Traduction VFP (flottants scalaires) A32 -> C, décodée depuis l'encodage.

Registres : S(k) = c->vr.f[k], D(k) = c->vr.df[k] (s2k/s2k+1 recouvrent dk, hôte petit-boutiste).
NEON (cond=1111, préfixes F2/F3/F4) et quelques encodages ARMv8 sont délégués à neon.py.
"""
import struct

from lifter import Unsupported, bits

CONDS_FP = ["C_EQ", "C_NE", "C_CS", "C_CC", "C_MI", "C_PL", "C_VS", "C_VC",
            "C_HI", "C_LS", "C_GE", "C_LT", "C_GT", "C_LE"]


def vfp_imm(imm8, dbl):
    a, b, cdefgh = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
    if dbl:
        u = (a << 63) | ((b ^ 1) << 62) | ((0xFF if b else 0) << 54) | (cdefgh << 48)
        return repr(struct.unpack("<d", struct.pack("<Q", u))[0])
    u = (a << 31) | ((b ^ 1) << 30) | ((0x1F if b else 0) << 25) | (cdefgh << 19)
    return repr(struct.unpack("<f", struct.pack("<I", u))[0]) + "f"


class Vfp:
    def __init__(self, neon=None):
        self.neon = neon

    @staticmethod
    def regs(w, dbl):
        D, N, M = bits(w, 22, 22), bits(w, 7, 7), bits(w, 5, 5)
        vd, vn, vm = bits(w, 15, 12), bits(w, 19, 16), bits(w, 3, 0)
        if dbl:
            return (D << 4) | vd, (N << 4) | vn, (M << 4) | vm
        return (vd << 1) | D, (vn << 1) | N, (vm << 1) | M

    @staticmethod
    def R(k, dbl):
        return f"c->vr.df[{k}]" if dbl else f"c->vr.f[{k}]"

    def lift(self, w, addr):
        if w >> 28 == 0xF:
            if bits(w, 27, 24) == 0xE and bits(w, 11, 9) == 0b101 and not bits(w, 4, 4):
                return self.v8(w, addr)
            if self.neon:
                return self.neon.lift(w, addr)
            raise Unsupported("neon")
        op = bits(w, 27, 24)
        cp = bits(w, 11, 8)
        if op == 0xE and not bits(w, 4, 4):
            return self.dp(w, addr)
        if op == 0xE:
            return self.xfer(w, addr)
        if bits(w, 27, 25) == 0b110:
            if bits(w, 24, 21) in (0b0010,) and cp in (10, 11):
                return self.mov64(w, addr)
            return self.ldst(w, addr)
        raise Unsupported(f"vfp {w:08X}")

    # --------------------------------------------------------------- calcul
    def dp(self, w, addr):
        dbl = bits(w, 8, 8)
        d, n, m = self.regs(w, dbl)
        Rd, Rn, Rm = self.R(d, dbl), self.R(n, dbl), self.R(m, dbl)
        opc1 = (bits(w, 23, 23) << 2) | bits(w, 21, 20)
        o = bits(w, 6, 6)
        t = "double" if dbl else "float"
        if opc1 == 0:
            return f"{Rd} = {Rd} {'-' if o else '+'} ({t})({Rn} * {Rm});"
        if opc1 == 1:
            return f"{Rd} = -{Rd} {'-' if o else '+'} ({t})({Rn} * {Rm});"
        if opc1 == 2:
            return f"{Rd} = {'-' if o else ''}({t})({Rn} * {Rm});"
        if opc1 == 3:
            return f"{Rd} = {Rn} {'-' if o else '+'} {Rm};"
        if opc1 == 4 and not o:
            return f"{Rd} = {Rn} / {Rm};"
        fma = "fma" if dbl else "fmaf"
        if opc1 == 5:  # VFNMS (o=0) : -d + n*m ; VFNMA (o=1) : -d - n*m
            return f"{Rd} = {fma}({'-' if o else ''}{Rn}, {Rm}, -{Rd});"
        if opc1 == 6:  # VFMA / VFMS
            return f"{Rd} = {fma}({'-' if o else ''}{Rn}, {Rm}, {Rd});"
        if opc1 == 7:
            return self.dp_other(w, addr, dbl, d, m)
        raise Unsupported(f"vfp dp opc1={opc1} o={o}")

    def dp_other(self, w, addr, dbl, d, m):
        opc2 = bits(w, 19, 16)
        opc3 = bits(w, 7, 6)
        Rd, Rm = self.R(d, dbl), self.R(m, dbl)
        t = "double" if dbl else "float"
        if not opc3 & 1:  # VMOV immédiat
            imm8 = (bits(w, 19, 16) << 4) | bits(w, 3, 0)
            return f"{Rd} = {vfp_imm(imm8, dbl)};"
        if opc2 == 0:
            return f"{Rd} = {Rm};" if opc3 == 1 else f"{Rd} = {'fabs' if dbl else 'fabsf'}({Rm});"
        if opc2 == 1:
            return f"{Rd} = -{Rm};" if opc3 == 1 else f"{Rd} = {'sqrt' if dbl else 'sqrtf'}({Rm});"
        if opc2 in (4, 5):
            rhs = f"({t})0" if opc2 == 5 else Rm
            return f"fcmp(c, {Rd}, {rhs});"
        if opc2 == 6:  # VRINTR (opc3=01, mode FPSCR = au plus proche) / VRINTZ (11)
            f = ("trunc" if opc3 == 3 else "nearbyint") + ("" if dbl else "f")
            return f"{Rd} = {f}({Rm});"
        if opc2 == 7:
            if opc3 == 3:  # VCVT double <-> simple
                if dbl:  # source double Dm -> Sd
                    sd = (bits(w, 15, 12) << 1) | bits(w, 22, 22)
                    return f"c->vr.f[{sd}] = (float)c->vr.df[{m}];"
                dd = (bits(w, 22, 22) << 4) | bits(w, 15, 12)
                return f"c->vr.df[{dd}] = (double)c->vr.f[{m}];"
            return f"{Rd} = {'nearbyint' if dbl else 'nearbyintf'}({Rm});"  # VRINTX
        if opc2 == 8:  # VCVT entier -> flottant (source toujours Sm)
            sm = (bits(w, 3, 0) << 1) | bits(w, 5, 5)
            src = f"(int32_t)c->vr.s[{sm}]" if bits(w, 7, 7) else f"c->vr.s[{sm}]"
            return f"{Rd} = ({t}){src};"
        if opc2 in (12, 13):  # VCVT flottant -> entier (destination toujours Sd)
            sd = (bits(w, 15, 12) << 1) | bits(w, 22, 22)
            val = Rm if bits(w, 7, 7) else f"{'nearbyint' if dbl else 'nearbyintf'}({Rm})"
            fn = "cvt_f_s32" if opc2 == 13 else "cvt_f_u32"
            return f"c->vr.s[{sd}] = (uint32_t){fn}((double){val});"
        if opc2 in (10, 11, 14, 15):  # VCVT virgule fixe, en place sur Vd
            to_fixed = bits(w, 18, 18)
            unsigned = bits(w, 16, 16)
            size = 16 if not bits(w, 7, 7) else 32
            imm = (bits(w, 3, 0) << 1) | bits(w, 5, 5)
            frac = size - imm
            scale = float(1 << frac)
            raw = f"c->vr.d[{d}]" if dbl else f"c->vr.s[{d}]"
            if to_fixed:
                fn = "cvt_f_u32" if unsigned else "cvt_f_s32"
                if size == 16:
                    lim = "65535.0" if unsigned else "32767.0"
                    lo = "0.0" if unsigned else "-32768.0"
                    v = f"fmin(fmax((double){Rd} * {scale!r}, {lo}), {lim})"
                    return f"{raw} = (uint32_t){fn}({v}) & 0xFFFFu;" if unsigned else \
                        f"{raw} = (uint32_t)(int32_t){fn}({v});"
                return f"{raw} = (uint32_t){fn}((double){Rd} * {scale!r});"
            if size == 16:
                src = f"(uint16_t){raw}" if unsigned else f"(int16_t){raw}"
            else:
                src = f"(uint32_t){raw}" if unsigned else f"(int32_t){raw}"
            return f"{Rd} = ({t})((double){src} / {scale!r});"
        raise Unsupported(f"vfp autre opc2={opc2} opc3={opc3}")

    # --------------------------------------------------------------- ARMv8 inconditionnelles
    def v8(self, w, addr):
        dbl = bits(w, 8, 8)
        d, n, m = self.regs(w, dbl)
        Rd, Rn, Rm = self.R(d, dbl), self.R(n, dbl), self.R(m, dbl)
        if bits(w, 23, 23) == 0:  # VSEL
            cc = bits(w, 21, 20)
            cond = {0: "C_EQ", 1: "C_VS", 2: "C_GE", 3: "C_GT"}[cc]
            return f"{Rd} = {cond} ? {Rn} : {Rm};"
        if bits(w, 21, 20) == 0:  # VMAXNM / VMINNM
            fn = "maxnm_d" if dbl else "maxnm_f"
            return f"{Rd} = {fn}({Rn}, {Rm}, {0 if bits(w, 6, 6) else 1});"
        if bits(w, 21, 18) == 0b1110 and bits(w, 7, 6) == 1:  # VRINTA/N/P/M
            rm = bits(w, 17, 16)
            fn = ["round", "nearbyint", "ceil", "floor"][rm] + ("" if dbl else "f")
            return f"{Rd} = {fn}({Rm});"
        if bits(w, 21, 18) == 0b1111 and bits(w, 6, 6) == 1:  # VCVTA/N/P/M vers entier
            rm = bits(w, 17, 16)
            fn = ["round", "nearbyint", "ceil", "floor"][rm] + ("" if dbl else "f")
            sd = (bits(w, 15, 12) << 1) | bits(w, 22, 22)
            cv = "cvt_f_s32" if bits(w, 7, 7) else "cvt_f_u32"
            return f"c->vr.s[{sd}] = (uint32_t){cv}((double){fn}({Rm}));"
        raise Unsupported(f"vfp v8 {w:08X}")

    # --------------------------------------------------------------- transferts
    def xfer(self, w, addr):
        cp = bits(w, 11, 8)
        L = bits(w, 20, 20)
        rt = bits(w, 15, 12)
        opc = bits(w, 23, 21)
        if cp == 10 and opc == 0 and bits(w, 6, 5) == 0:  # VMOV core <-> simple
            sn = (bits(w, 19, 16) << 1) | bits(w, 7, 7)
            return f"c->r[{rt}] = c->vr.s[{sn}];" if L else f"c->vr.s[{sn}] = c->r[{rt}];"
        if cp == 10 and opc == 7:  # VMRS / VMSR
            reg = bits(w, 19, 16)
            if reg != 1:
                if L:
                    return f"c->r[{rt}] = 0;" if rt != 15 else ";"
                return ";"
            if L:
                if rt == 15:
                    return "c->n = c->fn; c->z = c->fz; c->c = c->fc; c->v = c->fv;"
                return (f"c->r[{rt}] = (c->fn << 31) | (c->fz << 30) | (c->fc << 29) | (c->fv << 28) "
                        f"| (c->fpscr & 0x0FFFFFFFu);")
            return (f"{{ uint32_t x_ = c->r[{rt}]; c->fn = x_ >> 31; c->fz = (x_ >> 30) & 1; "
                    f"c->fc = (x_ >> 29) & 1; c->fv = (x_ >> 28) & 1; c->fpscr = x_ & 0x0FFFFFFFu; }}")
        if cp == 11:
            if not L and bits(w, 23, 23):  # VDUP depuis core
                bE = (bits(w, 22, 22) << 1) | bits(w, 5, 5)
                q = bits(w, 21, 21)
                dd = (bits(w, 7, 7) << 4) | bits(w, 19, 16)
                size = {0: 4, 1: 2, 2: 1}[bE]
                nb = 16 if q else 8
                ct = {4: "uint32_t", 2: "uint16_t", 1: "uint8_t"}[size]
                return (f"{{ {ct} x_ = ({ct})c->r[{rt}]; for (int i_ = 0; i_ < {nb // size}; i_++) "
                        f"memcpy(&c->vr.b[{dd * 8} + i_ * {size}], &x_, {size}); }}")
            opc1 = bits(w, 22, 21)
            opc2 = bits(w, 6, 5)
            dn = (bits(w, 7, 7) << 4) | bits(w, 19, 16)
            if opc1 & 2:
                size, idx = 1, ((opc1 & 1) << 2) | opc2
            elif opc2 & 1:
                size, idx = 2, ((opc1 & 1) << 1) | (opc2 >> 1)
            elif opc2 == 0:
                size, idx = 4, opc1 & 1
            else:
                raise Unsupported("vmov scalaire")
            off = dn * 8 + idx * size
            ct = {4: "uint32_t", 2: "uint16_t", 1: "uint8_t"}[size]
            if L:
                U = bits(w, 23, 23)
                st = {4: "int32_t", 2: "int16_t", 1: "int8_t"}[size]
                cast = f"(uint32_t)({ct})" if U or size == 4 else f"(uint32_t)(int32_t)({st})"
                return f"{{ {ct} x_; memcpy(&x_, &c->vr.b[{off}], {size}); c->r[{rt}] = {cast}x_; }}"
            return f"{{ {ct} x_ = ({ct})c->r[{rt}]; memcpy(&c->vr.b[{off}], &x_, {size}); }}"
        raise Unsupported(f"vfp xfer {w:08X}")

    def mov64(self, w, addr):
        L = bits(w, 20, 20)
        rt, rt2 = bits(w, 15, 12), bits(w, 19, 16)
        if bits(w, 8, 8):  # Dm
            dm = (bits(w, 5, 5) << 4) | bits(w, 3, 0)
            if L:
                return f"c->r[{rt}] = c->vr.s[{2 * dm}]; c->r[{rt2}] = c->vr.s[{2 * dm + 1}];"
            return f"c->vr.s[{2 * dm}] = c->r[{rt}]; c->vr.s[{2 * dm + 1}] = c->r[{rt2}];"
        sm = (bits(w, 3, 0) << 1) | bits(w, 5, 5)
        if L:
            return f"c->r[{rt}] = c->vr.s[{sm}]; c->r[{rt2}] = c->vr.s[{sm + 1}];"
        return f"c->vr.s[{sm}] = c->r[{rt}]; c->vr.s[{sm + 1}] = c->r[{rt2}];"

    def ldst(self, w, addr):
        P, U, D, W, L = (bits(w, 24, 24), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21), bits(w, 20, 20))
        rn = bits(w, 19, 16)
        vd = bits(w, 15, 12)
        dbl = bits(w, 8, 8)
        imm8 = bits(w, 7, 0)
        base = f"0x{((addr + 8) & ~3) & 0xFFFFFFFF:X}u" if rn == 15 else f"c->r[{rn}]"
        first = (D << 4) | vd if dbl else (vd << 1) | D
        if P and not W:  # VLDR / VSTR
            ea = f"{base} {'+' if U else '-'} {imm8 * 4}"
            if dbl:
                return f"c->vr.d[{first}] = rd64({ea});" if L else f"wr64({ea}, c->vr.d[{first}]);"
            return f"c->vr.s[{first}] = rd32({ea});" if L else f"wr32({ea}, c->vr.s[{first}]);"
        # VLDM / VSTM / VPUSH / VPOP
        count = imm8 // 2 if dbl else imm8
        size = 8 if dbl else 4
        if not P and U:
            start, new = base, f"{base} + {imm8 * 4}"
        elif P and not U and W:
            start, new = f"{base} - {imm8 * 4}", f"{base} - {imm8 * 4}"
        else:
            raise Unsupported("vldm mode")
        out = [f"uint32_t ea_ = {start};"]
        for k in range(count):
            if dbl:
                out.append(f"c->vr.d[{first + k}] = rd64(ea_ + {k * 8});" if L else f"wr64(ea_ + {k * 8}, c->vr.d[{first + k}]);")
            else:
                out.append(f"c->vr.s[{first + k}] = rd32(ea_ + {k * 4});" if L else f"wr32(ea_ + {k * 4}, c->vr.s[{first + k}]);")
        if W:
            out.append(f"c->r[{rn}] = {new};" if rn != 15 else "")
        return "{ " + " ".join(out) + " }"
