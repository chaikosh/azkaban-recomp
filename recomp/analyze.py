"""Function discovery and control-flow analysis for the EE executable."""
import collections
from r5900 import decode, BRANCH, BRANCH_L, JUMP, CALL, JR, JALR, TRAP

RA = 31


class Func:
    def __init__(self, start):
        self.start = start
        self.insns = {}          # pc -> Insn, including delay slots
        self.labels = set()      # pcs that are branch targets inside the function
        self.calls = set()       # direct call targets
        self.tailcalls = set()   # j/b to another function's entry
        self.jumptables = {}     # jr pc -> (table va, [targets])
        self.indirect = set()    # unresolved jr pcs (dispatched at run time)
        self.end = start

    def __repr__(self):
        return f"<Func {self.start:08x}..{self.end:08x} {len(self.insns)} insns>"


class Analyzer:
    def __init__(self, elf):
        self.elf = elf
        self.lo, self.hi = elf.text_range()
        self.cache = {}
        self.funcs = {}
        self.seed_origin = {}
        self.jt_words = set()    # data addresses that belong to jump tables
        self.data_refs = collections.defaultdict(list)  # code va -> data words pointing at it

    def insn(self, pc):
        i = self.cache.get(pc)
        if i is None:
            i = self.cache[pc] = decode(self.elf.u32(pc), pc)
        return i

    def in_text(self, va):
        return self.lo <= va < self.hi and not va & 3

    # -- seeds -------------------------------------------------------------
    def collect_seeds(self):
        seeds = {self.elf.entry: "entry"}
        for pc in range(self.lo, self.hi, 4):
            i = self.insn(pc)
            if i.op == "jal" and self.in_text(i.target):
                seeds.setdefault(i.target, "jal")
        # Code pointers stored in data: vtables, callback tables, static ctors.
        for name in (".data", ".rodata", ".sdata"):
            s = self.elf.sections.get(name)
            if not s:
                continue
            for va in range(s.addr, s.addr + s.size - 3, 4):
                w = self.elf.u32(va)
                if self.in_text(w):
                    seeds.setdefault(w, "data")
                    self.data_refs[w].append(va)
        # Address constants built in code (lui/addiu pairs) that point at code.
        hi_regs = {}
        for pc in range(self.lo, self.hi, 4):
            i = self.insn(pc)
            if i.op == "lui":
                hi_regs[i.rt] = i.uimm << 16
            elif i.op in ("addiu", "ori") and i.rs in hi_regs:
                v = (hi_regs[i.rs] + (i.imm if i.op == "addiu" else i.uimm)) & 0xFFFFFFFF
                if self.in_text(v) and self.looks_like_entry(v):
                    seeds.setdefault(v, "const")
            if i.kind or i.op in ("jr", "jalr"):
                hi_regs.clear()
        # Prologues right after a function end (possibly after nop padding).
        for pc in range(self.lo + 8, self.hi, 4):
            i = self.insn(pc)
            if i.op == "addiu" and i.rs == 29 and i.rt == 29 and i.imm < 0:
                q = pc - 4
                while q > self.lo and self.insn(q).raw == 0 and pc - q < 64:
                    q -= 4
                j = self.insn(q - 4)
                if (j.op == "jr" and j.rs == RA) or j.kind in (JUMP,):
                    seeds.setdefault(pc, "prologue")
        return seeds

    def looks_like_entry(self, va):
        """The previous instruction pair ends a function (jr ra + delay slot) or is padding."""
        if va == self.lo:
            return True
        p2 = self.insn(va - 8) if va - 8 >= self.lo else None
        p1 = self.insn(va - 4)
        return (p2 is not None and p2.op == "jr" and p2.rs == RA) or p1.raw == 0

    # -- per-function CFG --------------------------------------------------
    def const_before(self, f, pc, reg, depth=0):
        """Value of `reg` just before `pc`, if it is built from lui/addiu/ori in the same block."""
        if reg == 0:
            return 0
        p = pc - 4
        while p >= self.lo and pc - p < 96 and depth < 8:
            i = self.insn(p)
            if i.kind:
                return None
            if i.op == "lui" and i.rt == reg:
                return i.uimm << 16
            if i.op in ("addiu", "daddiu") and i.rt == reg:
                b = self.const_before(f, p, i.rs, depth + 1)
                return None if b is None else (b + i.imm) & 0xFFFFFFFF
            if i.op == "ori" and i.rt == reg:
                b = self.const_before(f, p, i.rs, depth + 1)
                return None if b is None else b | i.uimm
            if i.op in ("daddu", "addu", "or") and i.rd == reg and i.rt == 0:
                return self.const_before(f, p, i.rs, depth + 1)
            if self.writes(i, reg):
                return None
            p -= 4
        return None

    @staticmethod
    def writes(i, reg):
        op = i.op or ""
        if op in ("sw", "sh", "sb", "sd", "sq", "swc1", "sqc2", "swl", "swr", "sdl", "sdr",
                  "cache", "pref", "sync", "mtc1", "ctc1", "qmtc2", "ctc2", "mthi", "mtlo",
                  "mthi1", "mtlo1", "mtsa", "mtc0") or op.startswith("b") or op[:1] == "v":
            return False
        if op in ("lui", "addiu", "daddiu", "slti", "sltiu", "andi", "ori", "xori", "lb", "lh",
                  "lw", "lbu", "lhu", "lwu", "ld", "lq", "lwl", "lwr", "ldl", "ldr", "daddi",
                  "addi", "mfc1", "cfc1", "qmfc2", "cfc2", "mfc0"):
            return i.rt == reg
        return i.rd == reg

    def find_jumptable(self, f, jr):
        """Match `lw reg, off(base); jr reg` where base = const + scaled index."""
        reg = jr.rs
        p = jr.pc - 4
        load = None
        while p >= self.lo and jr.pc - p < 48:
            i = self.insn(p)
            if i.kind:
                break
            if i.op == "lw" and i.rt == reg:
                load = i
                break
            if self.writes(i, reg):
                return None
            p -= 4
        if load is None:
            return None
        base = None
        q = load.pc - 4
        while q >= self.lo and load.pc - q < 48:
            i = self.insn(q)
            if i.kind:
                break
            if i.op in ("addu", "daddu") and i.rd == load.rs:
                for a in (i.rs, i.rt):
                    c = self.const_before(f, q, a)
                    if c is not None and c:
                        base = c
                        break
                break
            if self.writes(i, load.rs):
                break
            q -= 4
        if base is None:
            base = self.const_before(f, load.pc, load.rs)
            if base is None:
                return None
        table = (base + load.imm) & 0xFFFFFFFF
        bound = self.find_bound(f, jr.pc, None)
        targets = []
        n = 0
        while self.elf.contains(table + 4 * n) and (bound is None or n < bound) and n <= 4096:
            t = self.elf.u32(table + 4 * n)
            if not self.in_text(t):
                break
            targets.append(t)
            n += 1
        if not targets:
            return None
        for k in range(len(targets)):
            self.jt_words.add(table + 4 * k)
        return table, targets

    def find_bound(self, f, pc, idx_reg):
        """sltiu t, idx, N guarding the switch -> N entries."""
        p = pc - 4
        while p >= self.lo and pc - p < 96:
            i = self.insn(p)
            if i.op == "sltiu":
                return i.uimm
            p -= 4
        return None

    def build(self, start):
        f = Func(start)
        work = [start]
        seen = set()
        while work:
            pc = work.pop()
            while True:
                if pc in seen or not self.in_text(pc):
                    break
                if pc != start and pc in self.funcs_starts and pc not in f.labels:
                    # ran into the next function: tail fall-through
                    f.tailcalls.add(pc)
                    break
                seen.add(pc)
                i = self.insn(pc)
                f.insns[pc] = i
                if i.delay:
                    ds = self.insn(pc + 4)
                    f.insns[pc + 4] = ds
                    seen.add(pc + 4)
                k = i.kind
                if k is None:
                    pc += 4
                    continue
                if k in (BRANCH, BRANCH_L):
                    if i.link:
                        f.calls.add(i.target)
                    else:
                        f.labels.add(i.target)
                        work.append(i.target)
                    pc += 8
                    f.labels.add(pc)
                    continue
                if k == CALL:
                    f.calls.add(i.target)
                    pc += 8
                    continue
                if k == JALR:
                    pc += 8
                    continue
                if k == JUMP:
                    if i.target in self.funcs_starts and i.target != start:
                        f.tailcalls.add(i.target)
                    else:
                        f.labels.add(i.target)
                        work.append(i.target)
                    break
                if k == JR:
                    if i.rs != RA:
                        jt = self.find_jumptable(f, i)
                        if jt:
                            f.jumptables[pc] = jt
                            for t in jt[1]:
                                f.labels.add(t)
                                work.append(t)
                        else:
                            f.indirect.add(pc)
                    break
                if k == TRAP:
                    if i.op == "syscall":
                        pc += 4
                        continue
                    break
        f.end = max(f.insns) + 4 if f.insns else start
        return f

    def run(self):
        seeds = self.collect_seeds()
        dropped = set()
        while True:
            self.funcs = {}
            self.seed_origin = {a: o for a, o in seeds.items() if a not in dropped}
            self.funcs_starts = set(self.seed_origin)
            pending = sorted(self.funcs_starts)
            while pending:
                new = []
                for s in pending:
                    if s in self.funcs:
                        continue
                    f = self.funcs[s] = self.build(s)
                    for t in f.calls | f.tailcalls:
                        if self.in_text(t) and t not in self.funcs_starts:
                            self.funcs_starts.add(t)
                            self.seed_origin[t] = "call"
                            new.append(t)
                pending = new
            # Seeds that only came from jump-table words are case labels, not functions.
            bad = {a for a, o in self.seed_origin.items() if o == "data" and a not in dropped
                   and all(r in self.jt_words for r in self.data_refs[a])}
            if not bad:
                return self.funcs
            dropped |= bad

    def coverage(self):
        cov = bytearray((self.hi - self.lo) // 4)
        for f in self.funcs.values():
            for pc in f.insns:
                if self.lo <= pc < self.hi:
                    cov[(pc - self.lo) // 4] = 1
        return cov
