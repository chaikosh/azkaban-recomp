"""VU micro-mode disassembler: python3 vudis.py code.bin start_pair [count]"""
import sys, struct
XYZW = "xyzw"
def dest(d): return "." + "".join(c for k, c in enumerate(XYZW) if d & (8 >> k)) if d else ""
UP1 = {}
for i, n in enumerate(["add", "sub", "madd", "msub", "max", "mini", "mul"]):
    for b in range(4): UP1[i * 4 + b] = n + XYZW[b]
UP1.update({28: "mulq", 29: "maxi", 30: "muli", 31: "minii", 32: "addq", 33: "maddq", 34: "addi", 35: "maddi",
            36: "subq", 37: "msubq", 38: "subi", 39: "msubi", 40: "add", 41: "madd", 42: "mul", 43: "max",
            44: "sub", 45: "msub", 46: "opmsub", 47: "mini"})
UP2 = {}
for i, n in enumerate(["adda", "suba", "madda", "msuba"]):
    for b in range(4): UP2[i * 4 + b] = n + XYZW[b]
for b in range(4): UP2[24 + b] = "mula" + XYZW[b]
UP2.update({16: "itof0", 17: "itof4", 18: "itof12", 19: "itof15", 20: "ftoi0", 21: "ftoi4", 22: "ftoi12", 23: "ftoi15",
            28: "mulaq", 29: "abs", 30: "mulai", 31: "clipw", 32: "addaq", 33: "maddaq", 34: "addai", 35: "maddai",
            36: "subaq", 37: "msubaq", 38: "subai", 39: "msubai", 40: "adda", 41: "madda", 42: "mula", 44: "suba",
            45: "msuba", 46: "opmula", 47: "nop"})
LOW3 = {48: "move", 49: "mr32", 52: "lqi", 53: "sqi", 54: "lqd", 55: "sqd", 56: "div", 57: "sqrt", 58: "rsqrt",
        59: "waitq", 60: "mtir", 61: "mfir", 62: "ilwr", 63: "iswr", 64: "rnext", 65: "rget", 66: "rinit", 67: "rxor",
        100: "mfp", 104: "xtop", 105: "xitop", 108: "xgkick", 112: "esadd", 113: "ersadd", 114: "eleng", 115: "erleng",
        116: "eatanxy", 117: "eatanxz", 118: "esum", 120: "esqrt", 121: "ersqrt", 122: "ercpr", 123: "waitp",
        124: "esin", 125: "eatan", 126: "eexp"}
LOWO = {0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw", 0x08: "iaddiu", 0x09: "isubiu", 0x10: "fceq", 0x11: "fcset",
        0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset", 0x16: "fsand", 0x17: "fsor", 0x18: "fmeq",
        0x1a: "fmand", 0x1b: "fmor", 0x1c: "fcget", 0x20: "b", 0x21: "bal", 0x24: "jr", 0x25: "jalr", 0x28: "ibeq",
        0x29: "ibne", 0x2c: "ibltz", 0x2d: "ibgtz", 0x2e: "iblez", 0x2f: "ibgez"}
def upper(op):
    d, ft, fs, fd = (op >> 21) & 15, (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31
    f = op & 63
    if f < 60:
        n = UP1.get(f, f"?up{f}")
        return f"{n}{dest(d)} vf{fd}, vf{fs}, vf{ft}"
    code = (((op >> 6) & 31) << 2) | (op & 3)
    n = UP2.get(code, f"?up2_{code}")
    if n == "nop": return "nop"
    if n.startswith(("itof", "ftoi", "abs")): return f"{n}{dest(d)} vf{ft}, vf{fs}"
    if n == "clipw": return f"clipw.xyz vf{fs}, vf{ft}w"
    return f"{n}{dest(d)} ACC, vf{fs}, vf{ft}"
def lower(op, pc):
    opc = op >> 25
    d, ft, fs = (op >> 21) & 15, (op >> 16) & 31, (op >> 11) & 31
    imm11 = op & 0x7ff
    if imm11 & 0x400: imm11 -= 0x800
    if opc == 0x40:
        f = op & 63
        id_ = (op >> 6) & 15
        if f == 0x30: return f"iadd vi{id_}, vi{fs&15}, vi{ft&15}"
        if f == 0x31: return f"isub vi{id_}, vi{fs&15}, vi{ft&15}"
        if f == 0x32:
            i5 = (op >> 6) & 31
            if i5 & 16: i5 -= 32
            return f"iaddi vi{ft&15}, vi{fs&15}, {i5}"
        if f == 0x34: return f"iand vi{id_}, vi{fs&15}, vi{ft&15}"
        if f == 0x35: return f"ior vi{id_}, vi{fs&15}, vi{ft&15}"
        if f < 0x3c: return f"?low40_{f:#x}"
        code = (((op >> 6) & 31) << 2) | (op & 3)
        n = LOW3.get(code, f"?low3_{code}")
        if n in ("div", "rsqrt"): return f"{n} Q, vf{fs}{XYZW[(op>>21)&3]}, vf{ft}{XYZW[(op>>23)&3]}"
        if n in ("sqrt",): return f"{n} Q, vf{ft}{XYZW[(op>>23)&3]}"
        if n in ("lqi", "lqd"): return f"{n}{dest(d)} vf{ft}, (vi{fs&15})"
        if n in ("sqi", "sqd"): return f"{n}{dest(d)} vf{fs}, (vi{ft&15})"
        if n in ("xgkick",): return f"xgkick vi{fs&15}"
        if n in ("xtop", "xitop"): return f"{n} vi{ft&15}"
        if n in ("mtir",): return f"mtir vi{ft&15}, vf{fs}{XYZW[(op>>21)&3]}"
        if n in ("mfir",): return f"mfir{dest(d)} vf{ft}, vi{fs&15}"
        if n in ("move", "mr32", "mfp", "rget", "rnext"): return f"{n}{dest(d)} vf{ft}, vf{fs}"
        if n.startswith("e"): return f"{n} P, vf{fs}"
        return n
    n = LOWO.get(opc, f"?low{opc:#x}")
    if n in ("lq",): return f"lq{dest(d)} vf{ft}, {imm11}(vi{fs&15})"
    if n in ("sq",): return f"sq{dest(d)} vf{fs}, {imm11}(vi{ft&15})"
    if n in ("ilw", "isw"): return f"{n}{dest(d)} vi{ft&15}, {imm11}(vi{fs&15})"
    if n in ("iaddiu", "isubiu"):
        imm15 = (op & 0x7ff) | ((op >> 10) & 0x7800)
        return f"{n} vi{ft&15}, vi{fs&15}, {imm15:#x}"
    if n in ("b", "bal"): return f"{n} vi{ft&15}, {pc+1+imm11:#05x}" if n == "bal" else f"b {pc+1+imm11:#05x}"
    if n.startswith("ib"):
        return f"{n} vi{ft&15}, vi{fs&15}, {pc+1+imm11:#05x}"
    if n in ("jr", "jalr"): return f"{n} vi{ft&15}, vi{fs&15}"
    if n.startswith("fc"): return f"{n} {op & 0xffffff:#x}"
    if n.startswith(("fm", "fs")): return f"{n} vi{ft&15}, vi{fs&15} ({op&0x7ff:#x})"
    return n
code = open(sys.argv[1], "rb").read()
start = int(sys.argv[2], 16); count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
for pc in range(start, start + count):
    lo, up = struct.unpack_from("<II", code, pc * 8)
    flags = "".join(c for b, c in ((31, "I"), (30, "E"), (29, "M"), (28, "D"), (27, "T")) if up & (1 << b))
    if up & 0x80000000:
        l = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]:g}"
    else:
        l = lower(lo, pc)
    print(f"{pc:04x} {flags:3s} {upper(up):38s} {l}")
