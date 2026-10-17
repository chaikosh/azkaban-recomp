"""Emotion Engine (MIPS R5900) instruction decoder.

decode(word, pc) -> Insn with the mnemonic, the raw register fields and the
control-flow class the analyzer needs.  Operand semantics are left to the
lifter, which reads the fields it needs from Insn.
"""

# Control-flow classes
BRANCH = "branch"          # conditional, delay slot always executed
BRANCH_L = "branch_likely"  # conditional, delay slot only executed when taken
JUMP = "jump"              # j target
CALL = "call"              # jal target / bal-style branch-and-link
JR = "jr"
JALR = "jalr"
TRAP = "trap"              # syscall/break/eret: ends straight-line flow

SPECIAL = {
    0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr",
    10: "movz", 11: "movn", 12: "syscall", 13: "break", 15: "sync", 16: "mfhi", 17: "mthi",
    18: "mflo", 19: "mtlo", 20: "dsllv", 22: "dsrlv", 23: "dsrav", 24: "mult", 25: "multu",
    26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or",
    38: "xor", 39: "nor", 40: "mfsa", 41: "mtsa", 42: "slt", 43: "sltu", 44: "dadd",
    45: "daddu", 46: "dsub", 47: "dsubu", 48: "tge", 49: "tgeu", 50: "tlt", 51: "tltu",
    52: "teq", 54: "tne", 56: "dsll", 58: "dsrl", 59: "dsra", 60: "dsll32", 62: "dsrl32",
    63: "dsra32",
}
REGIMM = {
    0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 8: "tgei", 9: "tgeiu", 10: "tlti",
    11: "tltiu", 12: "teqi", 14: "tnei", 16: "bltzal", 17: "bgezal", 18: "bltzall",
    19: "bgezall", 24: "mtsab", 25: "mtsah",
}
PRIMARY = {
    2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu",
    10: "slti", 11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui", 20: "beql",
    21: "bnel", 22: "blezl", 23: "bgtzl", 24: "daddi", 25: "daddiu", 26: "ldl", 27: "ldr",
    30: "lq", 31: "sq", 32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu",
    38: "lwr", 39: "lwu", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 44: "sdl", 45: "sdr",
    46: "swr", 47: "cache", 49: "lwc1", 51: "pref", 54: "lqc2", 55: "ld", 57: "swc1",
    62: "sqc2", 63: "sd",
}
MMI = {
    0: "madd", 1: "maddu", 4: "plzcw", 16: "mfhi1", 17: "mthi1", 18: "mflo1", 19: "mtlo1",
    24: "mult1", 25: "multu1", 26: "div1", 27: "divu1", 32: "madd1", 33: "maddu1",
    52: "psllh", 54: "psrlh", 55: "psrah", 60: "psllw", 62: "psrlw", 63: "psraw",
}
MMI0 = {
    0: "paddw", 1: "psubw", 2: "pcgtw", 3: "pmaxw", 4: "paddh", 5: "psubh", 6: "pcgth",
    7: "pmaxh", 8: "paddb", 9: "psubb", 10: "pcgtb", 16: "paddsw", 17: "psubsw",
    18: "pextlw", 19: "ppacw", 20: "paddsh", 21: "psubsh", 22: "pextlh", 23: "ppach",
    24: "paddsb", 25: "psubsb", 26: "pextlb", 27: "ppacb", 30: "pext5", 31: "ppac5",
}
MMI1 = {
    1: "pabsw", 2: "pceqw", 3: "pminw", 4: "padsbh", 5: "pabsh", 6: "pceqh", 7: "pminh",
    10: "pceqb", 16: "padduw", 17: "psubuw", 18: "pextuw", 20: "padduh", 21: "psubuh",
    22: "pextuh", 24: "paddub", 25: "psubub", 26: "pextub", 27: "qfsrv",
}
MMI2 = {
    0: "pmaddw", 2: "psllvw", 3: "psrlvw", 4: "pmsubw", 8: "pmfhi", 9: "pmflo", 10: "pinth",
    12: "pmultw", 13: "pdivw", 14: "pcpyld", 16: "pmaddh", 17: "phmadh", 18: "pand",
    19: "pxor", 20: "pmsubh", 21: "phmsbh", 26: "pexeh", 27: "prevh", 28: "pmulth",
    29: "pdivbw", 30: "pexew", 31: "prot3w",
}
MMI3 = {
    0: "pmadduw", 3: "psravw", 8: "pmthi", 9: "pmtlo", 10: "pinteh", 12: "pmultuw",
    13: "pdivuw", 14: "pcpyud", 18: "por", 19: "pnor", 26: "pexch", 27: "pcpyh", 30: "pexcw",
}
PMFHL = {0: "pmfhl.lw", 1: "pmfhl.uw", 2: "pmfhl.slw", 3: "pmfhl.lh", 4: "pmfhl.sh"}
COP0_C0 = {1: "tlbr", 2: "tlbwi", 6: "tlbwr", 8: "tlbp", 24: "eret", 56: "ei", 57: "di"}
COP1_S = {
    0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s",
    7: "neg.s", 22: "rsqrt.s", 24: "adda.s", 25: "suba.s", 26: "mula.s", 28: "madd.s",
    29: "msub.s", 30: "madda.s", 31: "msuba.s", 36: "cvt.w.s", 40: "max.s", 41: "min.s",
    48: "c.f.s", 50: "c.eq.s", 52: "c.lt.s", 54: "c.le.s",
}
BC = {0: "f", 1: "t", 2: "fl", 3: "tl"}
_XYZW = "xyzw"
VU_SPECIAL1 = {}
for _i, _n in enumerate(("vadd", "vsub", "vmadd", "vmsub", "vmax", "vmini", "vmul")):
    for _b in range(4):
        VU_SPECIAL1[_i * 4 + _b] = _n + _XYZW[_b]
VU_SPECIAL1.update({
    28: "vmulq", 29: "vmaxi", 30: "vmuli", 31: "vminii", 32: "vaddq", 33: "vmaddq",
    34: "vaddi", 35: "vmaddi", 36: "vsubq", 37: "vmsubq", 38: "vsubi", 39: "vmsubi",
    40: "vadd", 41: "vmadd", 42: "vmul", 43: "vmax", 44: "vsub", 45: "vmsub", 46: "vopmsub",
    47: "vmini", 48: "viadd", 49: "visub", 50: "viaddi", 52: "viand", 53: "vior",
    56: "vcallms", 57: "vcallmsr",
})
VU_SPECIAL2 = {}
for _i, _n in enumerate(("vadda", "vsuba", "vmadda", "vmsuba")):
    for _b in range(4):
        VU_SPECIAL2[_i * 4 + _b] = _n + _XYZW[_b]
for _b in range(4):
    VU_SPECIAL2[24 + _b] = "vmula" + _XYZW[_b]
VU_SPECIAL2.update({
    16: "vitof0", 17: "vitof4", 18: "vitof12", 19: "vitof15", 20: "vftoi0", 21: "vftoi4",
    22: "vftoi12", 23: "vftoi15", 28: "vmulaq", 29: "vabs", 30: "vmulai", 31: "vclipw",
    32: "vaddaq", 33: "vmaddaq", 34: "vaddai", 35: "vmaddai", 36: "vsubaq", 37: "vmsubaq",
    38: "vsubai", 39: "vmsubai", 40: "vadda", 41: "vmadda", 42: "vmula", 44: "vsuba",
    45: "vmsuba", 46: "vopmula", 47: "vnop", 48: "vmove", 49: "vmr32", 52: "vlqi",
    53: "vsqi", 54: "vlqd", 55: "vsqd", 56: "vdiv", 57: "vsqrt", 58: "vrsqrt", 59: "vwaitq",
    60: "vmtir", 61: "vmfir", 62: "vilwr", 63: "viswr", 64: "vrnext", 65: "vrget",
    66: "vrinit", 67: "vrxor",
})


class Insn:
    __slots__ = ("pc", "raw", "op", "rs", "rt", "rd", "sa", "funct", "imm", "uimm",
                 "target", "kind", "link", "delay")

    def __init__(self, pc, raw):
        self.pc, self.raw = pc, raw
        self.rs = (raw >> 21) & 31
        self.rt = (raw >> 16) & 31
        self.rd = (raw >> 11) & 31
        self.sa = (raw >> 6) & 31
        self.funct = raw & 63
        self.uimm = raw & 0xFFFF
        self.imm = self.uimm - 0x10000 if self.uimm & 0x8000 else self.uimm
        self.op = None
        self.target = None   # static branch/jump target
        self.kind = None     # control-flow class, None for straight-line
        self.link = False    # writes the return address
        self.delay = False   # has a delay slot

    @property
    def next(self):
        return (self.pc + 4) & 0xFFFFFFFF

    @property
    def fallthrough(self):
        """Address execution continues at when the instruction does not transfer."""
        return (self.pc + 8) & 0xFFFFFFFF if self.delay else self.next

    def __repr__(self):
        return f"{self.pc:08x}: {self.raw:08x} {self.op}"


def _branch(i, likely=False, link=False):
    i.kind = BRANCH_L if likely else BRANCH
    i.target = (i.pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
    i.delay = True
    i.link = link


def decode(raw, pc):
    i = Insn(pc, raw)
    opc = raw >> 26
    if opc == 0:
        f = i.funct
        i.op = SPECIAL.get(f)
        if f == 8:
            i.kind, i.delay = JR, True
        elif f == 9:
            i.kind, i.delay, i.link = JALR, True, True
        elif f in (12, 13):
            i.kind = TRAP
    elif opc == 1:
        i.op = REGIMM.get(i.rt)
        if i.op and i.op[0] == "b":
            _branch(i, likely=i.op.endswith("l"), link="al" in i.op)
            if i.link:
                i.kind = CALL if i.kind == BRANCH and i.rs == 0 and i.op == "bgezal" else i.kind
    elif opc in (2, 3):
        i.op = PRIMARY[opc]
        i.target = ((pc + 4) & 0xF0000000) | ((raw & 0x3FFFFFF) << 2)
        i.delay = True
        if opc == 3:
            i.kind, i.link = CALL, True
        else:
            i.kind = JUMP
    elif opc in (4, 5, 6, 7, 20, 21, 22, 23):
        i.op = PRIMARY[opc]
        _branch(i, likely=opc >= 20)
        # beq $0,$0 is an unconditional branch ("b")
        if opc == 4 and i.rs == 0 and i.rt == 0:
            i.op, i.kind = "b", JUMP
    elif opc == 16:
        if i.rs == 0:
            i.op = "mfc0"
        elif i.rs == 4:
            i.op = "mtc0"
        elif i.rs == 8:
            i.op = "bc0" + BC.get(i.rt, "?")
            _branch(i, likely=i.rt >= 2)
        elif i.rs == 16:
            i.op = COP0_C0.get(i.funct)
            if i.op == "eret":
                i.kind = TRAP
    elif opc == 17:
        rs = i.rs
        if rs in (0, 2, 4, 6):
            i.op = {0: "mfc1", 2: "cfc1", 4: "mtc1", 6: "ctc1"}[rs]
        elif rs == 8:
            i.op = "bc1" + BC.get(i.rt, "?")
            _branch(i, likely=i.rt >= 2)
        elif rs == 16:
            i.op = COP1_S.get(i.funct)
        elif rs == 20 and i.funct == 32:
            i.op = "cvt.s.w"
    elif opc == 18:
        rs = i.rs
        if rs in (1, 2, 5, 6):
            i.op = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2"}[rs]
        elif rs == 8:
            i.op = "bc2" + BC.get(i.rt, "?")
            _branch(i, likely=i.rt >= 2)
        elif rs >= 16:
            if i.funct >= 60:
                i.op = VU_SPECIAL2.get((((raw >> 6) & 31) << 2) | (raw & 3))
            else:
                i.op = VU_SPECIAL1.get(i.funct)
    elif opc == 28:
        f = i.funct
        if f == 8:
            i.op = MMI0.get(i.sa)
        elif f == 40:
            i.op = MMI1.get(i.sa)
        elif f == 9:
            i.op = MMI2.get(i.sa)
        elif f == 41:
            i.op = MMI3.get(i.sa)
        elif f == 48:
            i.op = PMFHL.get(i.sa)
        elif f == 49:
            i.op = "pmthl.lw" if i.sa == 0 else None
        else:
            i.op = MMI.get(f)
    else:
        i.op = PRIMARY.get(opc)
    return i
