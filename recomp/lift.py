"""Emit C for every discovered EE function."""
import os
from r5900 import BRANCH, BRANCH_L, JUMP, CALL, JR, JALR, TRAP

RA = 31


def h(v):
    return f"0x{v & 0xFFFFFFFF:08x}u"


def fname(a):
    return f"f_{a:08x}"


# ---- non-control-flow instructions -----------------------------------------

def simm(i):
    return f"{i.imm}"


def ea(i):
    return f"EA({i.rs}, {i.imm})"


def gpr_write(rd, expr):
    return f"R({rd}) = {expr};" if rd else ""


def vdest(raw):
    return (raw >> 21) & 15


def vcomps(dest):
    return [k for k in range(4) if dest & (8 >> k)]


def vu_fields(raw):
    return (raw >> 16) & 31, (raw >> 11) & 31, (raw >> 6) & 31, raw & 3  # ft, fs, fd, bc


VU_BIN = {"add": "+", "sub": "-", "mul": "*"}


def lift_vu(i):
    raw, op = i.raw, i.op
    ft, fs, fd, bc = vu_fields(raw)
    dest = vdest(raw)
    ks = vcomps(dest)
    S = "s.f[{k}]"
    out = [f"VF s = c->vf[{fs}], t = c->vf[{ft}];"]

    def src_t(kind, k):
        if kind == "bc":
            return f"t.f[{bc}]"
        if kind == "q":
            return "c->vq"
        if kind == "i":
            return "c->vi_i"
        return f"t.f[{k}]"

    # classify: v<op>[a][bc|q|i]
    name = op[1:]
    kind = "v"
    to_acc = False
    for stem in ("madda", "msuba", "adda", "suba", "mula", "madd", "msub", "add", "sub", "mul", "max", "mini"):
        if name.startswith(stem):
            rest = name[len(stem):]
            if rest in ("", "x", "y", "z", "w", "q", "i"):
                to_acc = stem.endswith("a") and stem not in ("madd",)
                core = stem[:-1] if to_acc else stem
                if stem == "mini":
                    to_acc, core = False, "mini"
                if rest in ("x", "y", "z", "w"):
                    kind = "bc"
                elif rest == "q":
                    kind = "q"
                elif rest == "i":
                    kind = "i"
                else:
                    kind = "v"
                return out + vu_arith(core, kind, to_acc, fd, ks, src_t)
    if op == "vminii":
        return out + vu_arith("mini", "i", False, fd, ks, src_t)
    if op == "vmaxi":
        return out + vu_arith("max", "i", False, fd, ks, src_t)
    if op in ("vopmula", "vopmsub"):
        lines = []
        prods = [("1", "2"), ("2", "0"), ("0", "1")]  # x: s.y*t.z, y: s.z*t.x, z: s.x*t.y
        for k, (a, b) in enumerate(prods):
            p = f"s.f[{a}] * t.f[{b}]"
            if op == "vopmula":
                lines.append(f"c->vacc.f[{k}] = vclamp({p});")
            elif fd:
                lines.append(f"float o{k} = vclamp(c->vacc.f[{k}] - {p});")
        if op == "vopmsub" and fd:
            lines += [f"c->vf[{fd}].f[{k}] = o{k};" for k in range(3)]
        return out + lines
    if op == "vabs":
        return out + ([f"c->vf[{ft}].f[{k}] = fabsf(s.f[{k}]);" for k in ks] if ft else [])
    if op == "vmove":
        return out + ([f"c->vf[{ft}].u[{k}] = s.u[{k}];" for k in ks] if ft else [])
    if op == "vmr32":
        return out + ([f"c->vf[{ft}].u[{k}] = s.u[{(k + 1) & 3}];" for k in ks] if ft else [])
    if op.startswith("vitof"):
        sh = int(op[5:])
        return out + ([f"c->vf[{ft}].f[{k}] = vu_toi_f(s.i[{k}], {sh});" for k in ks] if ft else [])
    if op.startswith("vftoi"):
        sh = int(op[5:])
        return out + ([f"c->vf[{ft}].i[{k}] = vu_ftoi(s.f[{k}], {sh});" for k in ks] if ft else [])
    if op == "vclipw":
        return out + [f"vu0_clip(c, &s, t.f[3]);"]
    if op in ("vdiv", "vsqrt", "vrsqrt"):
        fsf, ftf = (raw >> 21) & 3, (raw >> 23) & 3
        if op == "vdiv":
            return out + [f"c->vq = vu_div(c, s.f[{fsf}], t.f[{ftf}]);"]
        if op == "vsqrt":
            return out + [f"c->vq = sqrtf(fabsf(t.f[{ftf}]));"]
        return out + [f"c->vq = vu_div(c, s.f[{fsf}], sqrtf(fabsf(t.f[{ftf}])));"]
    if op in ("vwaitq", "vnop"):
        return []
    if op in ("viadd", "visub", "viand", "vior"):
        it, is_, id_ = ft & 15, fs & 15, fd & 15
        o = {"viadd": "+", "visub": "-", "viand": "&", "vior": "|"}[op]
        return [f"c->vi[{id_}] = (uint16_t)(c->vi[{is_}] {o} c->vi[{it}]);"] if id_ else []
    if op == "viaddi":
        it, is_ = ft & 15, fs & 15
        imm5 = fd - 32 if fd & 16 else fd
        return [f"c->vi[{it}] = (uint16_t)(c->vi[{is_}] + ({imm5}));"] if it else []
    if op == "vmtir":
        it, fsf = ft & 15, (raw >> 21) & 3
        return [f"c->vi[{it}] = (uint16_t)c->vf[{fs}].u[{fsf}];"] if it else []
    if op == "vmfir":
        is_ = fs & 15
        return [f"c->vf[{ft}].i[{k}] = (int16_t)c->vi[{is_}];" for k in ks] if ft else []
    if op in ("vlqi", "vlqd", "vsqi", "vsqd"):
        if op in ("vlqi", "vlqd"):
            is_ = fs & 15
            lines = []
            if op == "vlqd":
                lines.append(f"if ({is_}) c->vi[{is_}]--;")
            lines.append(f"{{ const VF *m = (const VF *)(g_vu0_mem + ((c->vi[{is_}] & 0xff) << 4));")
            lines += [f"  c->vf[{ft}].u[{k}] = m->u[{k}];" for k in ks] if ft else []
            lines.append("}")
            if op == "vlqi":
                lines.append(f"if ({is_}) c->vi[{is_}]++;")
            return lines
        it = ft & 15
        lines = []
        if op == "vsqd":
            lines.append(f"if ({it}) c->vi[{it}]--;")
        lines.append(f"{{ VF *m = (VF *)(g_vu0_mem + ((c->vi[{it}] & 0xff) << 4));")
        lines += [f"  m->u[{k}] = c->vf[{fs}].u[{k}];" for k in ks]
        lines.append("}")
        if op == "vsqi":
            lines.append(f"if ({it}) c->vi[{it}]++;")
        return lines
    if op in ("vilwr", "viswr"):
        it, is_ = ft & 15, fs & 15
        k = ks[0] if ks else 0
        if op == "vilwr":
            return [f"c->vi[{it}] = *(uint16_t *)(g_vu0_mem + ((c->vi[{is_}] & 0xff) << 4) + {4 * k});"] if it else []
        return [f"*(uint32_t *)(g_vu0_mem + ((c->vi[{is_}] & 0xff) << 4) + {4 * k}) = c->vi[{it}];"]
    if op == "vrinit":
        return [f"c->vr = 0x3f800000u | (c->vf[{fs}].u[{(raw >> 21) & 3}] & 0x7fffff);"]
    if op == "vrxor":
        return [f"c->vr = 0x3f800000u | ((c->vr ^ c->vf[{fs}].u[{(raw >> 21) & 3}]) & 0x7fffff);"]
    if op in ("vrget", "vrnext"):
        lines = []
        if op == "vrnext":
            lines.append("{ uint32_t x = c->vr; int b = ((x >> 4) ^ (x >> 22)) & 1; "
                         "c->vr = 0x3f800000u | (((x << 1) | b) & 0x7fffff); }")
        lines += [f"c->vf[{ft}].u[{k}] = c->vr;" for k in ks] if ft else []
        return lines
    return None


def vu_arith(core, kind, to_acc, fd, ks, src_t):
    lines = []
    for k in ks:
        s, t = f"s.f[{k}]", src_t(kind, k)
        if core in ("add", "sub", "mul"):
            e = f"vclamp({s} {VU_BIN[core]} {t})"
        elif core == "madd":
            e = f"vclamp(c->vacc.f[{k}] + {s} * {t})"
        elif core == "msub":
            e = f"vclamp(c->vacc.f[{k}] - {s} * {t})"
        elif core == "max":
            e = f"fmaxf({s}, {t})"
        elif core == "mini":
            e = f"fminf({s}, {t})"
        else:
            return None
        if to_acc:
            lines.append(f"c->vacc.f[{k}] = {e};")
        else:
            lines.append(f"float o{k} = {e};")
    if not to_acc and fd:
        lines += [f"c->vf[{fd}].f[{k}] = o{k};" for k in ks]
    return lines


def lift_mmi(i):
    rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
    op = i.op
    A, B = f"c->r[{rs}]", f"c->r[{rt}]"
    pre = f"U128 a = {A}, b = {B}, d;"
    lanes = {"w": (4, "sw", "w", 32), "h": (8, "sh", "h", 16), "b": (16, "sbt", "b", 8)}

    def lanewise(expr, t):
        n, sf, uf, _ = lanes[t]
        return [pre, f"for (int k = 0; k < {n}; k++) {expr.format(sf=sf, uf=uf)};",
                f"c->r[{rd}] = d;"] if rd else []

    simple = {
        "paddw": ("d.{uf}[k] = a.{uf}[k] + b.{uf}[k]", "w"),
        "psubw": ("d.{uf}[k] = a.{uf}[k] - b.{uf}[k]", "w"),
        "paddh": ("d.{uf}[k] = a.{uf}[k] + b.{uf}[k]", "h"),
        "psubh": ("d.{uf}[k] = a.{uf}[k] - b.{uf}[k]", "h"),
        "paddb": ("d.{uf}[k] = a.{uf}[k] + b.{uf}[k]", "b"),
        "psubb": ("d.{uf}[k] = a.{uf}[k] - b.{uf}[k]", "b"),
        "pmaxw": ("d.{sf}[k] = a.{sf}[k] > b.{sf}[k] ? a.{sf}[k] : b.{sf}[k]", "w"),
        "pminw": ("d.{sf}[k] = a.{sf}[k] < b.{sf}[k] ? a.{sf}[k] : b.{sf}[k]", "w"),
        "pmaxh": ("d.{sf}[k] = a.{sf}[k] > b.{sf}[k] ? a.{sf}[k] : b.{sf}[k]", "h"),
        "pminh": ("d.{sf}[k] = a.{sf}[k] < b.{sf}[k] ? a.{sf}[k] : b.{sf}[k]", "h"),
        "pcgtw": ("d.{uf}[k] = a.{sf}[k] > b.{sf}[k] ? ~0u : 0", "w"),
        "pcgth": ("d.{uf}[k] = a.{sf}[k] > b.{sf}[k] ? 0xffff : 0", "h"),
        "pcgtb": ("d.{uf}[k] = a.{sf}[k] > b.{sf}[k] ? 0xff : 0", "b"),
        "pceqw": ("d.{uf}[k] = a.{uf}[k] == b.{uf}[k] ? ~0u : 0", "w"),
        "pceqh": ("d.{uf}[k] = a.{uf}[k] == b.{uf}[k] ? 0xffff : 0", "h"),
        "pceqb": ("d.{uf}[k] = a.{uf}[k] == b.{uf}[k] ? 0xff : 0", "b"),
        "padduw": ("{{ uint64_t s = (uint64_t)a.w[k] + b.w[k]; d.w[k] = s > 0xffffffffu ? 0xffffffffu : (uint32_t)s; }}", "w"),
        "paddsw": ("{{ int64_t s = (int64_t)a.sw[k] + b.sw[k]; d.sw[k] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s; }}", "w"),
        "psubsw": ("{{ int64_t s = (int64_t)a.sw[k] - b.sw[k]; d.sw[k] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s; }}", "w"),
        "paddsh": ("d.sh[k] = sat16(a.sh[k] + b.sh[k])", "h"),
        "psubsh": ("d.sh[k] = sat16(a.sh[k] - b.sh[k])", "h"),
        "pabsw": ("d.sw[k] = b.sw[k] == INT32_MIN ? INT32_MAX : (b.sw[k] < 0 ? -b.sw[k] : b.sw[k])", "w"),
        "pabsh": ("d.sh[k] = b.sh[k] == -32768 ? 32767 : (b.sh[k] < 0 ? -b.sh[k] : b.sh[k])", "h"),
    }
    if op in simple:
        e, t = simple[op]
        return lanewise(e, t)
    if op in ("pand", "por", "pxor", "pnor"):
        o = {"pand": "a.q & b.q", "por": "a.q | b.q", "pxor": "a.q ^ b.q", "pnor": "~(a.q | b.q)"}[op]
        return [pre, f"d.q = {o};", f"c->r[{rd}] = d;"] if rd else []
    shifts = {"psllh": ("h", "<<", "h"), "psrlh": ("h", ">>", "h"), "psrah": ("h", ">>", "sh"),
              "psllw": ("w", "<<", "w"), "psrlw": ("w", ">>", "w"), "psraw": ("w", ">>", "sw")}
    if op in shifts:
        t, o, f = shifts[op]
        n = 8 if t == "h" else 4
        amt = sa & (15 if t == "h" else 31)
        return [pre, f"for (int k = 0; k < {n}; k++) d.{f}[k] = b.{f}[k] {o} {amt};", f"c->r[{rd}] = d;"] if rd else []
    if not rd and op not in ("pmthi", "pmtlo", "pmthl.lw"):
        return []
    body = {
        "pextlw": "d.w[0] = b.w[0]; d.w[1] = a.w[0]; d.w[2] = b.w[1]; d.w[3] = a.w[1];",
        "pextuw": "d.w[0] = b.w[2]; d.w[1] = a.w[2]; d.w[2] = b.w[3]; d.w[3] = a.w[3];",
        "pextlh": "for (int k = 0; k < 4; k++) { d.h[2*k] = b.h[k]; d.h[2*k+1] = a.h[k]; }",
        "pextuh": "for (int k = 0; k < 4; k++) { d.h[2*k] = b.h[4+k]; d.h[2*k+1] = a.h[4+k]; }",
        "pextlb": "for (int k = 0; k < 8; k++) { d.b[2*k] = b.b[k]; d.b[2*k+1] = a.b[k]; }",
        "pextub": "for (int k = 0; k < 8; k++) { d.b[2*k] = b.b[8+k]; d.b[2*k+1] = a.b[8+k]; }",
        "ppacw": "d.w[0] = b.w[0]; d.w[1] = b.w[2]; d.w[2] = a.w[0]; d.w[3] = a.w[2];",
        "ppach": "for (int k = 0; k < 4; k++) { d.h[k] = b.h[2*k]; d.h[4+k] = a.h[2*k]; }",
        "ppacb": "for (int k = 0; k < 8; k++) { d.b[k] = b.b[2*k]; d.b[8+k] = a.b[2*k]; }",
        "pcpyld": "d.d[0] = b.d[0]; d.d[1] = a.d[0];",
        "pcpyud": "d.d[0] = a.d[1]; d.d[1] = b.d[1];",
        "pcpyh": "for (int k = 0; k < 4; k++) { d.h[k] = b.h[0]; d.h[4+k] = b.h[4]; }",
        "pexew": "d.w[0] = b.w[2]; d.w[1] = b.w[1]; d.w[2] = b.w[0]; d.w[3] = b.w[3];",
        "pexcw": "d.w[0] = b.w[0]; d.w[1] = b.w[2]; d.w[2] = b.w[1]; d.w[3] = b.w[3];",
        "prot3w": "d.w[0] = b.w[1]; d.w[1] = b.w[2]; d.w[2] = b.w[0]; d.w[3] = b.w[3];",
        "prevh": "for (int k = 0; k < 4; k++) { d.h[k] = b.h[3-k]; d.h[4+k] = b.h[7-k]; }",
        "pexeh": "d = b; d.h[0] = b.h[2]; d.h[2] = b.h[0]; d.h[4] = b.h[6]; d.h[6] = b.h[4];",
        "pexch": "d = b; d.h[1] = b.h[2]; d.h[2] = b.h[1]; d.h[5] = b.h[6]; d.h[6] = b.h[5];",
        "pinth": "for (int k = 0; k < 4; k++) { d.h[2*k] = b.h[k]; d.h[2*k+1] = a.h[4+k]; }",
        "pinteh": "for (int k = 0; k < 4; k++) { d.h[2*k] = b.h[2*k]; d.h[2*k+1] = a.h[2*k]; }",
        "plzcw": "d = c->r[%d]; d.w[0] = plzcw1(a.w[0]); d.w[1] = plzcw1(a.w[1]);" % rd,
        "qfsrv": "d = op_qfsrv(a, b, (uint32_t)c->sa);",
        "pmfhi": "d = c->hi;",
        "pmflo": "d = c->lo;",
        "pmfhl.lw": "d.w[0] = c->lo.w[0]; d.w[1] = c->hi.w[0]; d.w[2] = c->lo.w[2]; d.w[3] = c->hi.w[2];",
        "pmfhl.uw": "d.w[0] = c->lo.w[1]; d.w[1] = c->hi.w[1]; d.w[2] = c->lo.w[3]; d.w[3] = c->hi.w[3];",
    }
    if op in body:
        return [pre, body[op], f"c->r[{rd}] = d;"]
    if op == "pmthi":
        return [f"c->hi = {A};"]
    if op == "pmtlo":
        return [f"c->lo = {A};"]
    return None


def lift_insn(i, nxt=None):
    """C statements for a straight-line instruction (control flow handled elsewhere)."""
    op, rs, rt, rd, sa = i.op, i.rs, i.rt, i.rd, i.sa
    imm, uimm = i.imm, i.uimm
    if op is None:
        return [f"rt_unimpl(c, {h(i.pc)}, \"undecodable {i.raw:08x}\");"]
    if i.raw == 0:
        return []
    # ALU, immediate
    if op in ("addiu", "addi"):
        return [gpr_write(rt, f"SX(RW({rs}) + {h(imm)})")]
    if op in ("daddiu", "daddi"):
        return [gpr_write(rt, f"R({rs}) + (uint64_t)(int64_t){imm}")]
    if op == "slti":
        return [gpr_write(rt, f"RS({rs}) < (int64_t){imm}")]
    if op == "sltiu":
        return [gpr_write(rt, f"R({rs}) < (uint64_t)(int64_t){imm}")]
    if op == "andi":
        return [gpr_write(rt, f"R({rs}) & {uimm:#x}")]
    if op == "ori":
        return [gpr_write(rt, f"R({rs}) | {uimm:#x}")]
    if op == "xori":
        return [gpr_write(rt, f"R({rs}) ^ {uimm:#x}")]
    if op == "lui":
        return [gpr_write(rt, f"SX({h(uimm << 16)})")]
    # ALU, register
    r3 = {
        "addu": "SX(RW({s}) + RW({t}))", "add": "SX(RW({s}) + RW({t}))",
        "subu": "SX(RW({s}) - RW({t}))", "sub": "SX(RW({s}) - RW({t}))",
        "daddu": "R({s}) + R({t})", "dadd": "R({s}) + R({t})",
        "dsubu": "R({s}) - R({t})", "dsub": "R({s}) - R({t})",
        "and": "R({s}) & R({t})", "or": "R({s}) | R({t})", "xor": "R({s}) ^ R({t})",
        "nor": "~(R({s}) | R({t}))", "slt": "RS({s}) < RS({t})", "sltu": "R({s}) < R({t})",
        "sllv": "SX(RW({t}) << (RW({s}) & 31))", "srlv": "SX(RW({t}) >> (RW({s}) & 31))",
        "srav": "SX(RSW({t}) >> (RW({s}) & 31))", "dsllv": "R({t}) << (R({s}) & 63)",
        "dsrlv": "R({t}) >> (R({s}) & 63)", "dsrav": "(uint64_t)(RS({t}) >> (R({s}) & 63))",
    }
    if op in r3:
        if op in ("daddu", "or") and rt == 0:
            return [gpr_write(rd, f"R({rs})")]
        return [gpr_write(rd, r3[op].format(s=rs, t=rt))]
    sh = {
        "sll": f"SX(RW({rt}) << {sa})", "srl": f"SX(RW({rt}) >> {sa})", "sra": f"SX(RSW({rt}) >> {sa})",
        "dsll": f"R({rt}) << {sa}", "dsrl": f"R({rt}) >> {sa}", "dsra": f"(uint64_t)(RS({rt}) >> {sa})",
        "dsll32": f"R({rt}) << {sa + 32}", "dsrl32": f"R({rt}) >> {sa + 32}",
        "dsra32": f"(uint64_t)(RS({rt}) >> {sa + 32})",
    }
    if op in sh:
        return [gpr_write(rd, sh[op])]
    if op == "movz":
        return [f"if (R({rt}) == 0) R({rd}) = R({rs});"] if rd else []
    if op == "movn":
        return [f"if (R({rt}) != 0) R({rd}) = R({rs});"] if rd else []
    # HI/LO
    hl = {"mfhi": ("hi", 0), "mflo": ("lo", 0), "mfhi1": ("hi", 1), "mflo1": ("lo", 1)}
    if op in hl:
        r, p = hl[op]
        return [gpr_write(rd, f"c->{r}.d[{p}]")]
    hl = {"mthi": ("hi", 0), "mtlo": ("lo", 0), "mthi1": ("hi", 1), "mtlo1": ("lo", 1)}
    if op in hl:
        r, p = hl[op]
        return [f"c->{r}.d[{p}] = R({rs});"]
    md = {"mult": ("op_mult", "RSW", 0), "multu": ("op_multu", "RW", 0), "mult1": ("op_mult", "RSW", 1),
          "multu1": ("op_multu", "RW", 1), "madd": ("op_madd", "RSW", 0), "maddu": ("op_maddu", "RW", 0),
          "madd1": ("op_madd", "RSW", 1), "maddu1": ("op_maddu", "RW", 1)}
    if op in md:
        fn, acc, p = md[op]
        return [f"{fn}(c, {rd}, {acc}({rs}), {acc}({rt}), {p});"]
    dv = {"div": ("op_div", "RSW", 0), "divu": ("op_divu", "RW", 0), "div1": ("op_div", "RSW", 1),
          "divu1": ("op_divu", "RW", 1)}
    if op in dv:
        fn, acc, p = dv[op]
        return [f"{fn}(c, {acc}({rs}), {acc}({rt}), {p});"]
    if op == "mfsa":
        return [gpr_write(rd, "c->sa")]
    if op == "mtsa":
        return [f"c->sa = R({rs});"]
    if op == "mtsab":
        return [f"c->sa = (RW({rs}) & 15) ^ ({uimm} & 15);"]
    if op == "mtsah":
        return [f"c->sa = ((RW({rs}) & 7) ^ ({uimm} & 7)) << 1;"]
    if op in ("sync", "cache", "pref"):
        return []
    if op == "break":
        return [f"rt_break(c, {h(i.pc)}, {(i.raw >> 6) & 0xFFFFF});"]
    traps = {"teq": "R({s}) == R({t})", "tne": "R({s}) != R({t})", "tge": "RS({s}) >= RS({t})",
             "tgeu": "R({s}) >= R({t})", "tlt": "RS({s}) < RS({t})", "tltu": "R({s}) < R({t})"}
    if op in traps:
        return [f"if ({traps[op].format(s=rs, t=rt)}) rt_trap(c, {h(i.pc)}, \"{op}\");"]
    itraps = {"teqi": "RS({s}) == {v}", "tnei": "RS({s}) != {v}", "tgei": "RS({s}) >= {v}",
              "tlti": "RS({s}) < {v}", "tgeiu": "R({s}) >= (uint64_t){v}", "tltiu": "R({s}) < (uint64_t){v}"}
    if op in itraps:
        return [f"if ({itraps[op].format(s=rs, v=f'(int64_t){imm}')}) rt_trap(c, {h(i.pc)}, \"{op}\");"]
    # loads / stores
    loads = {"lb": "SX((int8_t)ld8({a}))", "lbu": "ld8({a})", "lh": "SX((int16_t)ld16({a}))",
             "lhu": "ld16({a})", "lw": "SX(ld32({a}))", "lwu": "ld32({a})", "ld": "ld64({a})"}
    if op in loads:
        if not rt:
            return [f"(void){loads[op].format(a=ea(i))};"]
        return [f"R({rt}) = {loads[op].format(a=ea(i))};"]
    stores = {"sb": "st8({a}, (uint8_t)R({t}))", "sh": "st16({a}, (uint16_t)R({t}))",
              "sw": "st32({a}, RW({t}))", "sd": "st64({a}, R({t}))"}
    if op in stores:
        return [stores[op].format(a=ea(i), t=rt) + ";"]
    if op == "lq":
        return [f"c->r[{rt}] = ld128({ea(i)});"] if rt else []
    if op == "sq":
        return [f"st128({ea(i)}, c->r[{rt}]);"]
    if op in ("lwl", "lwr", "ldl", "ldr", "swl", "swr", "sdl", "sdr"):
        return [f"op_{op}(c, {rt}, {ea(i)});"]
    if op == "lwc1":
        return [f"c->f[{rt}].u = ld32({ea(i)});"]
    if op == "swc1":
        return [f"st32({ea(i)}, c->f[{rt}].u);"]
    if op == "lqc2":
        return [f"c->vf[{rt}].q = ld128({ea(i)});"] if rt else [f"(void)ld128({ea(i)});"]
    if op == "sqc2":
        return [f"st128({ea(i)}, c->vf[{rt}].q);"]
    # COP0
    if op == "mfc0":
        return [gpr_write(rt, f"SX(rt_mfc0(c, {rd}, {i.raw & 0x7ff}))")]
    if op == "mtc0":
        return [f"rt_mtc0(c, {rd}, RW({rt}));"]
    if op == "ei":
        return ["rt_ei(c);"]
    if op == "di":
        return ["rt_di(c);"]
    # COP1
    fs, ft, fd = rd, rt, sa
    F = lambda n: f"c->f[{n}].f"
    if op == "mfc1":
        return [gpr_write(rt, f"SX(c->f[{fs}].u)")]
    if op == "mtc1":
        return [f"c->f[{fs}].u = RW({rt});"]
    if op == "cfc1":
        return [gpr_write(rt, "SX(c->fcr31)" if fs == 31 else "SX(0x2e30u)")]
    if op == "ctc1":
        return [f"c->fcr31 = RW({rt});"] if fs == 31 else []
    fbin = {"add.s": "fclamp({a} + {b})", "sub.s": "fclamp({a} - {b})", "mul.s": "fclamp({a} * {b})",
            "div.s": "fpu_div(c, {a}, {b})", "max.s": "fmaxf({a}, {b})", "min.s": "fminf({a}, {b})",
            "rsqrt.s": "fpu_rsqrt(c, {a}, {b})"}
    if op in fbin:
        return [f"{F(fd)} = {fbin[op].format(a=F(fs), b=F(ft))};"]
    if op == "sqrt.s":
        return [f"{F(fd)} = fpu_sqrt(c, {F(ft)});"]
    if op == "abs.s":
        return [f"c->f[{fd}].u = c->f[{fs}].u & 0x7fffffffu;"]
    if op == "neg.s":
        return [f"c->f[{fd}].u = c->f[{fs}].u ^ 0x80000000u;"]
    if op == "mov.s":
        return [f"c->f[{fd}].u = c->f[{fs}].u;"]
    if op == "cvt.w.s":
        return [f"c->f[{fd}].i = f2i_sat({F(fs)});"]
    if op == "cvt.s.w":
        return [f"{F(fd)} = (float)c->f[{fs}].i;"]
    acc = {"adda.s": "fclamp({a} + {b})", "suba.s": "fclamp({a} - {b})", "mula.s": "fclamp({a} * {b})",
           "madda.s": "fclamp(c->acc.f + {a} * {b})", "msuba.s": "fclamp(c->acc.f - {a} * {b})"}
    if op in acc:
        return [f"c->acc.f = {acc[op].format(a=F(fs), b=F(ft))};"]
    if op == "madd.s":
        return [f"{F(fd)} = fclamp(c->acc.f + {F(fs)} * {F(ft)});"]
    if op == "msub.s":
        return [f"{F(fd)} = fclamp(c->acc.f - {F(fs)} * {F(ft)});"]
    cmp = {"c.f.s": "0", "c.eq.s": "{a} == {b}", "c.lt.s": "{a} < {b}", "c.le.s": "{a} <= {b}"}
    if op in cmp:
        return [f"SETFC(c, {cmp[op].format(a=F(fs), b=F(ft))});"]
    # COP2
    if op == "qmfc2":
        return [f"c->r[{rt}] = c->vf[{rd}].q;"] if rt else []
    if op == "qmtc2":
        return [f"c->vf[{rd}].q = c->r[{rt}];"] if rd else []
    if op == "cfc2":
        return [gpr_write(rt, f"SX(vu0_cfc2(c, {rd}))")]
    if op == "ctc2":
        return [f"vu0_ctc2(c, {rd}, RW({rt}));"]
    if op.startswith("v"):
        r = lift_vu(i)
        if r is not None:
            return ["{ " + " ".join(r) + " }"] if r else []
    if (i.raw >> 26) == 28:
        r = lift_mmi(i)
        if r is not None:
            return ["{ " + " ".join(r) + " }"] if r else []
    return [f"rt_unimpl(c, {h(i.pc)}, \"{op}\");"]


# ---- branches -----------------------------------------------------------------

def cond_expr(i):
    op, rs, rt = i.op, i.rs, i.rt
    base = op[:-1] if op.endswith("l") and op not in ("bltzal", "bgezal") else op
    if op in ("bltzall", "bgezall"):
        base = op[:-1]
    m = {
        "beq": f"R({rs}) == R({rt})", "bne": f"R({rs}) != R({rt})",
        "blez": f"RS({rs}) <= 0", "bgtz": f"RS({rs}) > 0",
        "bltz": f"RS({rs}) < 0", "bgez": f"RS({rs}) >= 0",
        "bltzal": f"RS({rs}) < 0", "bgezal": f"RS({rs}) >= 0",
        "bc1f": "!FCOND(c)", "bc1t": "FCOND(c)",
        "bc0f": "!rt_cpcond0(c)", "bc0t": "rt_cpcond0(c)",
        "bc2f": "!(vu0_cfc2(c, 29) & 1)", "bc2t": "(vu0_cfc2(c, 29) & 1)",
    }
    if base not in m:
        # bc1fl -> bc1f etc.
        base = base.rstrip("l")
    return m[base]


class FuncEmitter:
    def __init__(self, an, f, funcs):
        self.an, self.f, self.funcs = an, f, funcs
        self.lines = []
        self.need_labels = set(f.labels) | {f.start}

    def call_expr(self, target):
        if target in self.funcs:
            return f"{fname(target)}(c);"
        return f"rt_call(c, {h(target)});"

    def ds_code(self, pc):
        ds = self.f.insns.get(pc + 4)
        if ds is None:
            return ""
        if ds.kind and ds.op not in ("syscall", "break"):
            return f"rt_unimpl(c, {h(ds.pc)}, \"branch in delay slot\");"
        return " ".join(x for x in self.stmt(ds) if x)

    def stmt(self, i):
        if i.op == "syscall":
            return [f"rt_syscall(c, {h(i.pc)});"]
        return lift_insn(i)

    def emit(self):
        f = self.f
        out = self.lines
        pcs = sorted(f.insns)
        consumed = set()
        for pc in pcs:
            ins = f.insns[pc]
            if ins.delay:
                consumed.add(pc + 4)
        body = []
        prev_ft = None   # fall-through address of the previous emitted item, None = no fall-through
        k = 0
        while k < len(pcs):
            pc = pcs[k]
            i = f.insns[pc]
            if pc in consumed and pc not in self.need_labels:
                k += 1
                continue
            if prev_ft is not None and prev_ft != pc:
                body.append(self.goto_or_fall(prev_ft))
            if pc in self.need_labels or pc in consumed:
                body.append(f"L_{pc:08x}:; BB({h(pc)});")
            if pc in consumed:
                # entered at a delay slot directly: run it, then continue after the branch
                body.append("  " + " ".join(self.stmt(i)))
                prev_ft = pc + 4
                k += 1
                continue
            text, prev_ft = self.emit_one(i)
            body.extend("  " + t for t in text if t)
            k += 1
        if prev_ft is not None:
            body.append(self.goto_or_fall(prev_ft))
        out.append(f"void {fname(f.start)}(EE *c) {{")
        out.append("  RA_ENTER();")
        out.extend(body)
        out.append("}")
        return out

    def goto_or_fall(self, target):
        if target in self.f.insns:
            self.need_labels.add(target)
            return f"  goto L_{target:08x};"
        if target in self.funcs:
            return f"  {fname(target)}(c); return;  /* falls into next function */"
        return f"  rt_bad_jump(c, {h(target)}, {h(target)});"

    def emit_one(self, i):
        """Returns (lines, fall-through pc or None)."""
        pc, k = i.pc, i.kind
        f = self.f
        if k is None or (k == TRAP and i.op == "syscall"):
            if i.op in ("ldl", "lwl", "sdl", "swl") and self.pair(i):
                return self.pair(i)
            if i.op in ("ldr", "lwr", "sdr", "swr") and pc in self.paired:
                return [], pc + 4
            return self.stmt(i), pc + 4
        ds = self.ds_code(pc)
        if k in (BRANCH, BRANCH_L):
            cond = cond_expr(i)
            back = "POLL(); " if i.target <= pc else ""
            if i.link:
                call = self.call_expr(i.target)
                if k == BRANCH:
                    return [f"{{ int t = {cond}; {ds} R(31) = {h(pc + 8)}; if (t) {{ {call} }} }}"], pc + 8
                return [f"{{ R(31) = {h(pc + 8)}; if ({cond}) {{ {ds} {call} }} }}"], pc + 8
            self.need_labels.add(i.target)
            if i.target not in f.insns:
                tgt = f"{{ {self.call_expr(i.target)} return; }}"
            else:
                tgt = f"{{ {back}goto L_{i.target:08x}; }}"
            if k == BRANCH:
                return [f"{{ int t = {cond}; {ds} if (t) {tgt} }}"], pc + 8
            return [f"if ({cond}) {{ {ds} {tgt[1:-1]} }}"], pc + 8
        if k == JUMP:
            if i.target in f.tailcalls or i.target not in f.insns:
                return [f"{{ {ds} {self.call_expr(i.target)} return; }}"], None
            back = "POLL(); " if i.target <= pc else ""
            self.need_labels.add(i.target)
            return [f"{{ {ds} {back}goto L_{i.target:08x}; }}"], None
        if k == CALL:
            return [f"{{ {ds} R(31) = {h(pc + 8)}; {self.call_expr(i.target)} }}"], pc + 8
        if k == JALR:
            rd = i.rd
            link = f"R({rd}) = {h(pc + 8)}; " if rd else ""
            return [f"{{ uint32_t t = RW({i.rs}); {ds} {link}rt_call(c, t); }}"], pc + 8
        if k == JR:
            if i.rs == RA:
                return [f"{{ {ds} RA_LEAVE({h(pc)}); return; }}"], None
            if pc in f.jumptables:
                table, targets = f.jumptables[pc]
                cases = []
                for t in sorted(set(targets)):
                    self.need_labels.add(t)
                    cases.append(f"case {h(t)}: goto L_{t:08x};")
                return [f"{{ uint32_t t = RW({i.rs}); {ds} POLL(); switch (t) {{ {' '.join(cases)} "
                        f"default: rt_bad_jump(c, {h(pc)}, t); }} }}"], None
            return [f"{{ uint32_t t = RW({i.rs}); {ds} rt_call(c, t); return; }}"], None
        if k == TRAP:
            if i.op == "eret":
                return [f"rt_eret(c, {h(pc)}); return;"], None
            if i.op == "break":
                return self.stmt(i), pc + 4
        return [f"rt_unimpl(c, {h(pc)}, \"{i.op}\");"], None

    paired = set()

    def pair(self, i):
        """ldl/ldr, lwl/lwr, sdl/sdr, swl/swr on the same span -> one unaligned access."""
        nx = self.f.insns.get(i.pc + 4)
        if nx is None or nx.pc in self.need_labels:
            return None
        partner = {"ldl": "ldr", "lwl": "lwr", "sdl": "sdr", "swl": "swr"}[i.op]
        width = 8 if i.op[1] == "d" else 4
        if nx.op != partner or nx.rt != i.rt or nx.rs != i.rs or i.imm != nx.imm + width - 1:
            return None
        if i.op in ("ldl", "lwl") and i.rt == i.rs:
            return None
        if i.delay or nx.delay:
            return None
        self.paired.add(nx.pc)
        a = f"EA({nx.rs}, {nx.imm})"
        rt = i.rt
        if i.op == "ldl":
            return ([f"R({rt}) = ld64({a});"] if rt else []), nx.pc + 4
        if i.op == "lwl":
            return ([f"R({rt}) = SX(ld32({a}));"] if rt else []), nx.pc + 4
        if i.op == "sdl":
            return [f"st64({a}, R({rt}));"], nx.pc + 4
        return [f"st32({a}, RW({rt}));"], nx.pc + 4


def load_hooks():
    hooks = {}
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hooks.txt")
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#")[0].split()
            if len(line) >= 2:
                hooks[int(line[0], 16)] = line[1]
    return hooks


def emit_all(elf, an, out, per_file=150):
    gen = os.path.join(out, "gen")
    os.makedirs(gen, exist_ok=True)
    funcs = an.funcs
    starts = sorted(funcs)
    hooks = load_hooks()
    with open(os.path.join(gen, "funcs.h"), "w") as fh:
        fh.write("#pragma once\n#include \"ee.h\"\n")
        for a in starts:
            fh.write(f"void {fname(a)}(EE *c);\n")
        for a, name in sorted(hooks.items()):
            fh.write(f"void {fname(a)}_orig(EE *c);\nvoid {name}(EE *c);\n")
    unimpl = {}
    nfiles = 0
    for n in range(0, len(starts), per_file):
        chunk = starts[n:n + per_file]
        lines = ["#include \"funcs.h\"", "#include \"ee_gen.h\"", ""]
        for a in chunk:
            fe = FuncEmitter(an, funcs[a], funcs)
            fe.paired = set()
            fe.emit()                      # first pass collects every label a goto needs
            fe.lines, fe.paired = [], set()
            body = fe.emit()
            if a in hooks:
                # the original body stays callable as f_X_orig; f_X goes to the host
                body[0] = body[0].replace(f"void {fname(a)}(EE *c)", f"void {fname(a)}_orig(EE *c)")
                lines.append(f"void {fname(a)}(EE *c) {{ {hooks[a]}(c); }}")
            lines.extend(body)
            lines.append("")
            for ln in body:
                if "rt_unimpl(" in ln:
                    key = ln.split('"')[1] if '"' in ln else ln
                    unimpl[key] = unimpl.get(key, 0) + 1
        with open(os.path.join(gen, f"code_{nfiles:03d}.c"), "w") as fh:
            fh.write("\n".join(lines) + "\n")
        nfiles += 1
    with open(os.path.join(gen, "table.c"), "w") as fh:
        fh.write("#include \"funcs.h\"\n#include \"ee_gen.h\"\n")
        fh.write("const FuncEntry g_func_table[] = {\n")
        for a in starts:
            fh.write(f"  {{{h(a)}, {fname(a)}}},\n")
        fh.write("};\n")
        fh.write(f"const uint32_t g_func_count = {len(starts)};\n")
        fh.write(f"const uint32_t g_entry_point = {h(elf.entry)};\n")
    with open(os.path.join(out, "image.bin"), "wb") as fh:
        fh.write(elf.image)
    with open(os.path.join(out, "image.txt"), "w") as fh:
        fh.write(f"{elf.base:#x} {len(elf.image):#x} {elf.entry:#x}\n")
    print(f"[emit] {len(starts)} functions in {nfiles} files; unimplemented: {unimpl}")
