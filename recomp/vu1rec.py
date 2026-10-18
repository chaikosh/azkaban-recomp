#!/usr/bin/env python3
"""VU1 microprogram recompiler.

The game uploads its VU1 microprograms at run time (VIF MPG).  The runtime saves every distinct
16 KiB code image it runs together with the entry points and jump targets it saw
(HP_VUCOLLECT=dir: code_<hash>.bin, entries.txt).  This script turns each image into one C function
that runs the program natively, with the same semantics as the interpreter in runtime/vu1.c:

  - upper and lower instructions of a pair read the registers as they were before the pair;
    results commit lower first, then upper, then ACC, VI and I;
  - Q, P, R, CLIP, MAC/status and data memory stores take effect immediately, upper before lower;
  - branches and the E bit have one delay slot; XGKICK sends its packet one pair late.

Control flow is resolved statically where possible (branch targets, fall-through, delay slots
inlined at each branch); JR/JALR go through a table of all compiled pairs.  Anything else (an
unknown jump target, a branch in a delay slot) hands the state to the interpreter, which finishes
the program.

Usage: vu1rec.py <collect dir> <out.c>
"""
import glob
import os
import sys

E_BIT = 0x40000000
I_BIT = 0x80000000


def sx(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class Image:
    def __init__(self, hsh, data):
        self.hash = hsh
        self.lo = [int.from_bytes(data[8 * n:8 * n + 4], 'little') for n in range(2048)]
        self.up = [int.from_bytes(data[8 * n + 4:8 * n + 8], 'little') for n in range(2048)]
        self.entries = set()
        self.jr_targets = set()

    # control flow of the lower instruction at pair n: None or (kind, static target, conditional, link)
    def branch(self, n):
        if self.up[n] & I_BIT:
            return None
        lo = self.lo[n]
        opc = lo >> 25
        imm11 = sx(lo, 11)
        tgt = (n + 1 + imm11) & 0x7ff
        if opc == 0x20:
            return ('b', tgt, False, False)
        if opc == 0x21:
            return ('b', tgt, False, True)
        if opc == 0x24:
            return ('jr', None, False, False)
        if opc == 0x25:
            return ('jr', None, False, True)
        if opc in (0x28, 0x29, 0x2c, 0x2d, 0x2e, 0x2f):
            return ('b', tgt, True, False)
        return None

    def ebit(self, n):
        return bool(self.up[n & 0x7ff] & E_BIT)

    def reachable(self):
        seen = set()
        work = sorted(self.entries | self.jr_targets)
        while work:
            n = work.pop() & 0x7ff
            if n in seen:
                continue
            seen.add(n)
            for s in self.successors(n):
                if (s & 0x7ff) not in seen:
                    work.append(s & 0x7ff)
        return seen

    def successors(self, n):
        if self.ebit(n):
            return []
        br = self.branch(n)
        if not br:
            return [(n + 1) & 0x7ff]
        d = (n + 1) & 0x7ff
        if self.branch(d) or self.ebit(d):
            return []   # handed to the interpreter
        kind, tgt, cond, link = br
        out = []
        if tgt is not None:
            out.append(tgt)
        if cond or link:
            out.append((n + 2) & 0x7ff)
        if kind == 'jr':
            out.extend(sorted(self.jr_targets))
        return out

    def upper_sets_mac(self, n):
        up = self.up[n]
        f = up & 63
        if f < 60:
            return f < 16 or 24 <= f < 28 or f in (28, 30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45, 46)
        code = (((up >> 6) & 31) << 2) | (up & 3)
        return code < 16 or 24 <= code <= 28 or code in (30, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 45, 46)

    def lower_reads_mac(self, n):
        return not (self.up[n] & I_BIT) and (self.lo[n] >> 25) in (0x18, 0x1a, 0x1b)

    def mac_liveness(self, pcs):
        """Pairs whose upper MAC result can be read later (FMEQ/FMAND/FMOR), by a backward
        dataflow over every way a pair can run (label, branch delay slot, E-bit delay slot).
        Over-approximates; a hand-over to the interpreter counts as a read."""
        pcs = set(pcs)
        allp = pcs | {(n + 1) & 0x7ff for n in pcs}
        succ = {m: set() for m in allp}
        live_end = {m: False for m in allp}
        for m in allp:
            prev = (m - 1) & 0x7ff
            if m in pcs:
                succ[m].add((m + 1) & 0x7ff)
            if prev in pcs and self.ebit(prev):
                pass   # program ends after this pair: flags are dead
            elif prev in pcs and self.branch(prev):
                if self.branch(m) or self.ebit(m):
                    live_end[m] = True   # handed to the interpreter
                else:
                    kind, tgt, cond, link = self.branch(prev)
                    if tgt is not None:
                        succ[m].add(tgt)
                    if cond or link:
                        succ[m].add((prev + 2) & 0x7ff)
                    if kind == 'jr':
                        succ[m] |= set(self.jr_targets)
                        live_end[m] = True   # an unknown target goes to the interpreter
        live_in = {m: False for m in allp}
        changed = True
        while changed:
            changed = False
            for m in allp:
                out = live_end[m] or any(live_in.get(x, True) for x in succ[m])
                need = self.lower_reads_mac(m) or out
                li = False if self.upper_sets_mac(m) else need
                if li != live_in[m]:
                    live_in[m] = li
                    changed = True
        needed = set()
        for m in allp:
            out = live_end[m] or any(live_in.get(x, True) for x in succ[m])
            if self.upper_sets_mac(m) and (self.lower_reads_mac(m) or out):
                needed.add(m)
        return needed

    def reads_status(self, pcs):
        for n in pcs:
            for m in (n, (n + 1) & 0x7ff):
                if not (self.up[m] & I_BIT) and (self.lo[m] >> 25) in (0x14, 0x16, 0x17):
                    return True
        return False

    def reads_flags(self, pcs):
        for n in pcs:
            for m in (n, (n + 1) & 0x7ff):   # delay slots are inlined
                if self.up[m] & I_BIT:
                    continue
                opc = self.lo[m] >> 25
                if opc in (0x14, 0x16, 0x17, 0x18, 0x1a, 0x1b):
                    return True
        return False


COMP = 'xyzw'


def dest_comps(dest):
    return [k for k in range(4) if dest & (8 >> k)]


class Gen:
    def __init__(self, img, flags):
        self.img = img
        self.flags = flags
        pcs = img.reachable()
        self.status = img.reads_status(pcs)          # status flags read anywhere: keep vu_set_mac
        self.mac_pcs = img.mac_liveness(pcs) if flags else set()
        self.cur = 0

    def mac(self, dest):
        if not self.flags or self.cur not in self.mac_pcs:
            return []
        if self.status:
            return [f'vu_set_mac(&s, &ur, {dest});']
        return [f's.mac = vu_mac(&ur, {dest});']

    # ---- upper: returns (lines, commit) where commit is None, ('vf', reg, dest) or ('acc', dest)
    def upper(self, up):
        dest, ft, fs, fd, bc = (up >> 21) & 15, (up >> 16) & 31, (up >> 11) & 31, (up >> 6) & 31, up & 3
        f = up & 63
        L = []

        def operand_b(src, k):
            return {0: f's.vf[{ft}].f[{bc}]', 1: 's.q', 2: 's.i', 3: f's.vf[{ft}].f[{k}]'}[src]

        def arith(kind, src, comps):
            for k in comps:
                a = f'vin(s.vf[{fs}].f[{k}])'
                b = f'vin({operand_b(src, k)})'
                if kind == 0:
                    e = f'vclampf({a} + {b})'
                elif kind == 1:
                    e = f'vclampf({a} - {b})'
                elif kind == 2:
                    e = f'vclampf(s.acc.f[{k}] + {a} * {b})'
                elif kind == 3:
                    e = f'vclampf(s.acc.f[{k}] - {a} * {b})'
                elif kind == 4:
                    e = f'vmax({a}, {b})'
                elif kind == 5:
                    e = f'vmin({a}, {b})'
                else:
                    e = f'vclampf({a} * {b})'
                L.append(f'ur.f[{k}] = {e};')

        if f < 60:
            if f < 28:
                kind, src = f >> 2, 0
            elif f == 46:   # OPMSUB
                L.append(f'ur.f[0] = vclampf(s.acc.f[0] - s.vf[{fs}].f[1] * s.vf[{ft}].f[2]);')
                L.append(f'ur.f[1] = vclampf(s.acc.f[1] - s.vf[{fs}].f[2] * s.vf[{ft}].f[0]);')
                L.append(f'ur.f[2] = vclampf(s.acc.f[2] - s.vf[{fs}].f[0] * s.vf[{ft}].f[1]);')
                L.append('ur.f[3] = 0;')
                L += self.mac(0xe)
                return L, ('vf', fd, 0xe)
            else:
                table = {28: (6, 1), 29: (4, 2), 30: (6, 2), 31: (5, 2), 32: (0, 1), 33: (2, 1), 34: (0, 2), 35: (2, 2),
                         36: (1, 1), 37: (3, 1), 38: (1, 2), 39: (3, 2), 40: (0, 3), 41: (2, 3), 42: (6, 3), 43: (4, 3),
                         44: (1, 3), 45: (3, 3), 47: (5, 3)}
                if f not in table:
                    return [], None
                kind, src = table[f]
            arith(kind, src, dest_comps(dest))
            if kind < 4 or kind == 6:
                L += self.mac(dest)
            return L, ('vf', fd, dest)
        code = (((up >> 6) & 31) << 2) | (up & 3)
        acc_ops = {28: (6, 1), 30: (6, 2), 32: (0, 1), 33: (2, 1), 34: (0, 2), 35: (2, 2), 36: (1, 1), 37: (3, 1),
                   38: (1, 2), 39: (3, 2), 40: (0, 3), 41: (2, 3), 42: (6, 3), 44: (1, 3), 45: (3, 3)}
        if code < 16 or 24 <= code <= 27 or code in acc_ops or code == 46:
            if code == 46:   # OPMULA
                L.append(f'ur.f[0] = vclampf(s.vf[{fs}].f[1] * s.vf[{ft}].f[2]);')
                L.append(f'ur.f[1] = vclampf(s.vf[{fs}].f[2] * s.vf[{ft}].f[0]);')
                L.append(f'ur.f[2] = vclampf(s.vf[{fs}].f[0] * s.vf[{ft}].f[1]);')
                L.append('ur.f[3] = s.acc.f[3];')
                L += self.mac(0xe)
                return L, ('acc', 0xe)
            if code < 16:
                kind, src = code >> 2, 0
            elif code <= 27:
                kind, src = 6, 0
            else:
                kind, src = acc_ops[code]
            arith(kind, src, dest_comps(dest))
            L += self.mac(dest)
            return L, ('acc', dest)
        if 16 <= code <= 19:   # ITOF
            sc = ['1.0f', '1.0f / 16', '1.0f / 4096', '1.0f / 32768'][code - 16]
            for k in dest_comps(dest):
                L.append(f'ur.f[{k}] = (float)s.vf[{fs}].i[{k}] * ({sc});')
            return L, ('vf', ft, dest)
        if 20 <= code <= 23:   # FTOI
            sc = ['1.0f', '16.0f', '4096.0f', '32768.0f'][code - 20]
            for k in dest_comps(dest):
                L.append(f'ur.i[{k}] = vftoi(s.vf[{fs}].f[{k}] * {sc});')
            return L, ('vf', ft, dest)
        if code == 29:   # ABS
            for k in dest_comps(dest):
                L.append(f'ur.f[{k}] = fabsf(s.vf[{fs}].f[{k}]);')
            return L, ('vf', ft, dest)
        if code == 31:   # CLIPw
            L.append(f'{{ float w = fabsf(vin(s.vf[{ft}].f[3])); uint32_t c = 0;')
            for k, (p, m) in enumerate(((1, 2), (4, 8), (16, 32))):
                L.append(f'  if (s.vf[{fs}].f[{k}] > w) c |= {p}; if (s.vf[{fs}].f[{k}] < -w) c |= {m};')
            L.append('  s.clip = ((s.clip << 6) | c) & 0xffffff; }')
            return L, None
        return [], None

    # ---- lower: returns (lines, vf_commit, vi_commit, kick) ; branch handled by the caller
    def lower(self, lo, n):
        opc = lo >> 25
        dest, ft, fs = (lo >> 21) & 15, (lo >> 16) & 31, (lo >> 11) & 31
        it, is_ = ft & 15, fs & 15
        imm11 = sx(lo, 11)
        VIS, VIT = f's.vi[{is_}]', f's.vi[{it}]'
        L = []
        vf = None    # ('lr', reg, dest)
        vi = None    # (reg, expr)  computed into viv before commit
        kick = None
        if opc == 0x40:
            f = lo & 63
            idr = (lo >> 6) & 15
            if f == 0x30:
                return L, None, (idr, f'(uint16_t)({VIS} + {VIT})'), None
            if f == 0x31:
                return L, None, (idr, f'(uint16_t)({VIS} - {VIT})'), None
            if f == 0x32:
                imm5 = sx(lo >> 6, 5)
                return L, None, (it, f'(uint16_t)({VIS} + {imm5})'), None
            if f == 0x34:
                return L, None, (idr, f'(uint16_t)({VIS} & {VIT})'), None
            if f == 0x35:
                return L, None, (idr, f'(uint16_t)({VIS} | {VIT})'), None
            if f < 0x3c:
                return L, None, None, None
            code = (((lo >> 6) & 31) << 2) | (lo & 3)
            fsf, ftf = (lo >> 21) & 3, (lo >> 23) & 3
            S, T = f's.vf[{fs}]', f's.vf[{ft}]'
            if code == 48:
                L.append(f'lr = {S};')
                return L, ('lr', ft, dest), None, None
            if code == 49:
                L.append(f'lr.u[0] = {S}.u[1]; lr.u[1] = {S}.u[2]; lr.u[2] = {S}.u[3]; lr.u[3] = {S}.u[0];')
                return L, ('lr', ft, dest), None, None
            if code == 52:   # LQI
                L.append(f'memcpy(&lr, vu_dmem({VIS}), 16);')
                return L, ('lr', ft, dest), (is_, f'(uint16_t)({VIS} + 1)'), None
            if code == 53:   # SQI
                L.append(f'vu_store({VIT}, &{S}, {dest});')
                return L, None, (it, f'(uint16_t)({VIT} + 1)'), None
            if code == 54:   # LQD
                L.append(f'memcpy(&lr, vu_dmem((uint16_t)({VIS} - 1)), 16);')
                return L, ('lr', ft, dest), (is_, f'(uint16_t)({VIS} - 1)'), None
            if code == 55:   # SQD
                L.append(f'vu_store((uint16_t)({VIT} - 1), &{S}, {dest});')
                return L, None, (it, f'(uint16_t)({VIT} - 1)'), None
            if code == 56:
                L.append(f's.q = vdiv(vin({S}.f[{fsf}]), vin({T}.f[{ftf}]));')
                return L, None, None, None
            if code == 57:
                L.append(f's.q = sqrtf(fabsf(vin({T}.f[{ftf}])));')
                return L, None, None, None
            if code == 58:
                L.append(f'{{ float d = sqrtf(fabsf(vin({T}.f[{ftf}]))); s.q = d == 0.0f ? 3.402823e38f : vclampf(vin({S}.f[{fsf}]) / d); }}')
                return L, None, None, None
            if code == 59:
                return L, None, None, None
            if code == 60:
                return L, None, (it, f'(uint16_t){S}.u[{fsf}]'), None
            if code == 61:
                L.append(f'{{ int32_t m = (int16_t){VIS}; lr.i[0] = lr.i[1] = lr.i[2] = lr.i[3] = m; }}')
                return L, ('lr', ft, dest), None, None
            if code == 62:
                return L, None, (it, f'(uint16_t)vu_load_word({VIS}, {dest})'), None
            if code == 63:
                L.append(f'vu_store_word({VIS}, {VIT}, {dest});')
                return L, None, None, None
            if code == 64:
                L.append('{ uint32_t x = s.r; int b = ((x >> 4) ^ (x >> 22)) & 1; s.r = 0x3f800000u | (((x << 1) | b) & 0x7fffff); }')
                L.append('lr.u[0] = lr.u[1] = lr.u[2] = lr.u[3] = s.r;')
                return L, ('lr', ft, dest), None, None
            if code == 65:
                L.append('lr.u[0] = lr.u[1] = lr.u[2] = lr.u[3] = s.r;')
                return L, ('lr', ft, dest), None, None
            if code == 66:
                L.append(f's.r = 0x3f800000u | ({S}.u[{fsf}] & 0x7fffff);')
                return L, None, None, None
            if code == 67:
                L.append(f's.r = 0x3f800000u | ((s.r ^ {S}.u[{fsf}]) & 0x7fffff);')
                return L, None, None, None
            if code == 100:
                L.append('lr.f[0] = lr.f[1] = lr.f[2] = lr.f[3] = s.p;')
                return L, ('lr', ft, dest), None, None
            if code == 104:
                return L, None, (it, '(uint16_t)s.top'), None
            if code == 105:
                return L, None, (it, '(uint16_t)s.itop'), None
            if code == 108:
                return L, None, None, VIS
            x, y, z, w = (f'{S}.f[{k}]' for k in range(4))
            sq = f'{x} * {x} + {y} * {y} + {z} * {z}'
            efu = {
                112: f's.p = {sq};',
                113: f'{{ float d = {sq}; s.p = d != 0 ? 1.0f / d : 3.4e38f; }}',
                114: f's.p = sqrtf({sq});',
                115: f'{{ float d = sqrtf({sq}); s.p = d != 0 ? 1.0f / d : 3.4e38f; }}',
                116: f's.p = {x} != 0 ? atanf({y} / {x}) : 0;',
                117: f's.p = {x} != 0 ? atanf({z} / {x}) : 0;',
                118: f's.p = {x} + {y} + {z} + {w};',
                120: f's.p = sqrtf(fabsf({S}.f[{fsf}]));',
                121: f'{{ float d = sqrtf(fabsf({S}.f[{fsf}])); s.p = d != 0 ? 1.0f / d : 3.4e38f; }}',
                122: f's.p = {S}.f[{fsf}] != 0 ? 1.0f / {S}.f[{fsf}] : 3.4e38f;',
                124: f's.p = sinf({S}.f[{fsf}]);',
                125: f's.p = atanf({S}.f[{fsf}]);',
                126: f's.p = expf(-{S}.f[{fsf}]);',
            }
            if code in efu:
                L.append(efu[code])
            return L, None, None, None
        if opc == 0x00:
            L.append(f'memcpy(&lr, vu_dmem((uint16_t)({VIS} + {imm11})), 16);')
            return L, ('lr', ft, dest), None, None
        if opc == 0x01:
            L.append(f'vu_store((uint16_t)({VIT} + {imm11}), &s.vf[{fs}], {dest});')
            return L, None, None, None
        if opc == 0x04:
            return L, None, (it, f'(uint16_t)vu_load_word((uint16_t)({VIS} + {imm11}), {dest})'), None
        if opc == 0x05:
            L.append(f'vu_store_word((uint16_t)({VIS} + {imm11}), {VIT}, {dest});')
            return L, None, None, None
        if opc in (0x08, 0x09):
            imm15 = (lo & 0x7ff) | ((lo >> 10) & 0x7800)
            op = '+' if opc == 0x08 else '-'
            return L, None, (it, f'(uint16_t)({VIS} {op} {imm15})'), None
        imm24 = lo & 0xffffff
        if opc == 0x10:
            return L, None, (1, f'(uint16_t)((s.clip & 0xffffff) == {imm24}u)'), None
        if opc == 0x11:
            L.append(f's.clip = {imm24}u;')
            return L, None, None, None
        if opc == 0x12:
            return L, None, (1, f'(uint16_t)((s.clip & {imm24}u) != 0)'), None
        if opc == 0x13:
            return L, None, (1, f'(uint16_t)(((s.clip | {imm24}u) & 0xffffff) == 0xffffff)'), None
        imm12 = (lo & 0x7ff) | ((lo >> 10) & 0x800)
        if opc == 0x14:
            return L, None, (it, f'(uint16_t)((s.status & 0xfff) == {imm12}u)'), None
        if opc == 0x15:
            L.append(f's.status = (s.status & 0x3f) | ({imm12}u & 0xfc0);')
            return L, None, None, None
        if opc == 0x16:
            return L, None, (it, f'(uint16_t)(s.status & {imm12}u)'), None
        if opc == 0x17:
            return L, None, (it, f'(uint16_t)(s.status | {imm12}u)'), None
        if opc == 0x18:
            return L, None, (it, f'(uint16_t)((s.mac & 0xffff) == {VIS})'), None
        if opc == 0x1a:
            return L, None, (it, f'(uint16_t)(s.mac & {VIS})'), None
        if opc == 0x1b:
            return L, None, (it, f'(uint16_t)(s.mac | {VIS})'), None
        if opc == 0x1c:
            return L, None, (it, '(uint16_t)(s.clip & 0xfff)'), None
        # branches: link registers here, the jump itself in branch_cond()
        if opc in (0x21, 0x25):
            return L, None, (it, f'(uint16_t){(n + 2) & 0xffff}'), None
        return L, None, None, None

    def branch_cond(self, lo, n):
        """C expressions (taken, target) for the branch at pair n, using pre-commit VI values."""
        opc = lo >> 25
        is_, it = ((lo >> 11) & 31) & 15, ((lo >> 16) & 31) & 15
        VIS, VIT = f's.vi[{is_}]', f's.vi[{it}]'
        tgt = (n + 1 + sx(lo, 11)) & 0x7ff
        conds = {0x20: '1', 0x21: '1', 0x28: f'{VIT} == {VIS}', 0x29: f'{VIT} != {VIS}',
                 0x2c: f'(int16_t){VIS} < 0', 0x2d: f'(int16_t){VIS} > 0', 0x2e: f'(int16_t){VIS} <= 0',
                 0x2f: f'(int16_t){VIS} >= 0'}
        if opc in (0x24, 0x25):
            return '1', f'({VIS} & 0x7ff)'
        return conds[opc], str(tgt)

    def body(self, n, kick_check, tk_var=None):
        """C for one pair: execute, commit, XGKICK.  If tk_var is set and the pair branches, the
        branch decision is stored in tk_var / tk_var+'t' (taken, target)."""
        up, lo = self.img.up[n], self.img.lo[n]
        self.cur = n
        out = [f'/* {n:04x}: {up:08x} {lo:08x} */']
        ulines, ucommit = self.upper(up)
        out += ulines
        lcommit = vicommit = kick = None
        br = self.img.branch(n)
        if br and tk_var:
            c, t = self.branch_cond(lo, n)
            out.append(f'{tk_var} = {c}; {tk_var}t = {t};')
        if not (up & I_BIT):
            llines, lcommit, vicommit, kick = self.lower(lo, n)
            out += llines
        if vicommit:
            out.append(f'viv = {vicommit[1]};')
        if kick:
            out.append(f'kv = {kick};')
        # commit: lower VF, upper VF, ACC, VI, I
        if lcommit and lcommit[1] > 0:
            for k in dest_comps(lcommit[2]):
                out.append(f's.vf[{lcommit[1]}].u[{k}] = lr.u[{k}];')
        if ucommit:
            if ucommit[0] == 'vf' and ucommit[1] > 0:
                for k in dest_comps(ucommit[2]):
                    out.append(f's.vf[{ucommit[1]}].u[{k}] = ur.u[{k}];')
            elif ucommit[0] == 'acc':
                for k in dest_comps(ucommit[1]):
                    out.append(f's.acc.u[{k}] = ur.u[{k}];')
        if vicommit and vicommit[0] > 0:
            out.append(f's.vi[{vicommit[0]}] = viv;')
        if up & I_BIT:
            out.append(f'{{ union {{ uint32_t u; float f; }} c = {{{lo}u}}; s.i = c.f; }}')
        if kick_check:
            out.append('if (due >= 0) { vu1_xgkick((uint32_t)due); due = -1; }')
        if kick:
            out.append('due = kv;')
        return out

    def function(self):
        img = self.img
        pcs = sorted(img.reachable())
        labels = set(pcs)
        targets = set(img.entries) | set(img.jr_targets)
        for n in pcs:
            br = img.branch(n)
            if br and not img.ebit(n) and br[1] is not None:
                targets.add(br[1])
            if br and not img.ebit(n) and (br[2] or br[3]):
                targets.add((n + 2) & 0x7ff)
        prev_kick = lambda m: (not (img.up[(m - 1) & 0x7ff] & I_BIT)) and img.lower_is_kick((m - 1) & 0x7ff)
        name = f'vu1rec_{img.hash}'
        out = [f'static int {name}(VU1 *g, uint32_t start, VuResume *res) {{']
        out.append('    VU1 s = *g;')
        out.append('    res->jr_miss = 0;')
        out.append('    int due = -1, tk = 0, tk2 = 0; uint32_t tkt = 0, tk2t = 0, jr_pc = 0, tpc = 0;')
        out.append('    V4 ur, lr; uint16_t viv; int kv;')
        out.append('    (void)tk2; (void)tk2t; (void)ur; (void)lr; (void)viv; (void)kv;')
        jt = ', '.join(f'[{n}] = &&L_{n:04x}' for n in pcs)
        out.append(f'    static void *const jt[2048] = {{{jt}}};')
        out.append('    jr_pc = start & 0x7ff;')
        out.append('    if (!jt[jr_pc]) goto jr_unknown;')
        out.append('    goto *jt[jr_pc];')
        prev = None
        for n in pcs:
            if prev is not None and prev != n - 1:
                pass
            out.append(f'L_{n:04x}: {{')
            kc = True   # any compiled pair can be a jump target (JR goes through jt)
            br = img.branch(n)
            e = img.ebit(n)
            d = (n + 1) & 0x7ff
            if e:
                out += ['    ' + l for l in self.body(n, kc, 'tk' if br else None)]
                out.append('    /* E bit: the next pair runs, then the program ends */')
                out += ['    ' + l for l in self.body(d, prev_kick(d), None)]
                if br:
                    out.append(f'    tpc = tk ? (tkt & 0x7ff) : {(n + 2) & 0x7ff};')
                else:
                    out.append(f'    tpc = {(n + 2) & 0x7ff};')
                out.append('    goto done;')
            elif br:
                out += ['    ' + l for l in self.body(n, kc, 'tk')]
                dbr = img.branch(d)
                out.append('    /* delay slot */')
                out += ['    ' + l for l in self.body(d, prev_kick(d), 'tk2' if dbr else None)]
                if dbr or img.ebit(d):
                    out.append(f'    res->pc = tk ? (tkt & 0x7ff) : {(n + 2) & 0x7ff};')
                    out.append(f'    res->branch_pending = {"tk2" if dbr else "0"}; res->branch_target = {"tk2t" if dbr else "0"};')
                    out.append(f'    res->ending = {1 if img.ebit(d) else 0};')
                    out.append('    goto bail;')
                else:
                    kind, tgt, cond, link = br
                    if kind == 'jr':
                        out.append('    jr_pc = tkt & 0x7ff;')
                        out.append('    if (!jt[jr_pc]) goto jr_unknown;')
                        out.append('    goto *jt[jr_pc];')
                    elif cond:
                        out.append(f'    if (tk) goto L_{tgt:04x};')
                        out.append(f'    goto L_{(n + 2) & 0x7ff:04x};')
                    else:
                        out.append(f'    goto L_{tgt:04x};')
            else:
                out += ['    ' + l for l in self.body(n, kc, None)]
                nx = (n + 1) & 0x7ff
                out.append(f'    goto L_{nx:04x};')
            out.append('}')
            prev = n
        out.append('jr_unknown:')
        out.append('    res->pc = jr_pc; res->branch_pending = 0; res->branch_target = 0; res->ending = 0;')
        out.append('    res->jr_miss = 1;')
        out.append('bail:')
        out.append('    res->due = due;')
        out.append('    *g = s;')
        out.append('    return 1;')
        out.append('done:')
        out.append('    if (due >= 0) vu1_xgkick((uint32_t)due);')
        out.append('    s.pc = tpc;')
        out.append('    *g = s;')
        out.append('    return 0;')
        out.append('}')
        # the pairs this function was compiled from (labels and delay slots): it is used only while
        # micro memory holds exactly these
        used = sorted(set(pcs) | {(n + 1) & 0x7ff for n in pcs})
        ranges = []
        for n in used:
            if ranges and ranges[-1][1] == n:
                ranges[-1][1] = n + 1
            else:
                ranges.append([n, n + 1])
        key = 1469598103934665603
        for n in used:
            for b in img.lo[n].to_bytes(4, 'little') + img.up[n].to_bytes(4, 'little'):
                key = ((key ^ b) * 1099511628211) & 0xffffffffffffffff
        flat = ', '.join(f'{a}, {b}' for a, b in ranges)
        out.append(f'static const uint16_t ranges_{img.hash}[] = {{{flat}, 0xffff}};')
        bm = [0] * 256
        for n in pcs:
            bm[n >> 3] |= 1 << (n & 7)
        out.append(f'static const uint8_t labels_{img.hash}[256] = {{{", ".join(str(x) for x in bm)}}};')
        self.key = key
        return out, len(pcs)


def lower_is_kick(self, n):
    lo = self.lo[n]
    return (lo >> 25) == 0x40 and (lo & 63) >= 0x3c and ((((lo >> 6) & 31) << 2) | (lo & 3)) == 108


Image.lower_is_kick = lower_is_kick


def main():
    src, dst = sys.argv[1], sys.argv[2]
    images = {}
    for path in sorted(glob.glob(os.path.join(src, 'code_*.bin'))):
        h = os.path.basename(path)[5:-4]
        with open(path, 'rb') as f:
            images[h] = Image(h, f.read())
    ent = os.path.join(src, 'entries.txt')
    if os.path.exists(ent):
        for line in open(ent):
            parts = line.split()
            if len(parts) < 2 or parts[0] not in images:
                continue
            if len(parts) >= 3 and parts[2] == 'jr':
                images[parts[0]].jr_targets.add(int(parts[1], 16) & 0x7ff)
            else:
                images[parts[0]].entries.add(int(parts[1], 16) & 0x7ff)
    out = ['/* Generated by recomp/vu1rec.py from the VU1 code images the game uploaded. Do not edit. */',
           '#include "vu1_core.h"',
           '']
    table = []
    total = 0
    for h, img in sorted(images.items()):
        if not img.entries:
            continue
        flags = img.reads_flags(img.reachable())
        g = Gen(img, flags)
        lines, n = g.function()
        total += n
        out += lines
        out.append('')
        table.append((h, flags, g.key))
    out.append('const VuRecEntry g_vu1rec_table[] = {')
    for h, flags, key in table:
        out.append(f'    {{0x{h}ull, vu1rec_{h}, {1 if flags else 0}, ranges_{h}, 0x{key:016x}ull, labels_{h}}},')
    out.append('    {0, 0, 0, 0, 0, 0},')
    out.append('};')
    with open(dst, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print(f'vu1rec: {len(table)} code images, {total} pairs -> {dst}')


if __name__ == '__main__':
    main()
