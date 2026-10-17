"""Recompiler driver: python3 recomp/main.py <elf> <outdir> [--analyze-only]"""
import sys, os, time, pickle, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.setrecursionlimit(10000)
from elf import ELF
from analyze import Analyzer


def main():
    path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    elf = ELF(path)
    an = Analyzer(elf)
    print(f"[analyze] {os.path.basename(path)} entry={elf.entry:#x} text={an.lo:#x}..{an.hi:#x}")
    funcs = an.run()
    ninsn = sum(len(f.insns) for f in funcs.values())
    print(f"[analyze] {len(funcs)} functions, {ninsn} instructions, {time.time() - t0:.1f}s")
    cov = an.coverage()
    zeros = sum(1 for k, c in enumerate(cov) if not c and elf.u32(an.lo + 4 * k) == 0)
    print(f"[coverage] code {cov.count(1) / len(cov):.1%}  zero words {zeros / len(cov):.1%}  "
          f"unknown {(len(cov) - cov.count(1) - zeros) / len(cov):.1%}")
    jt = sum(len(f.jumptables) for f in funcs.values())
    ij = sum(len(f.indirect) for f in funcs.values())
    tc = sum(len(f.tailcalls) for f in funcs.values())
    print(f"[cfg] jump tables {jt}, unresolved jr {ij}, tailcalls {tc}")
    print(f"[seeds] {dict(collections.Counter(an.seed_origin[a] for a in funcs))}")
    mn = collections.Counter(i.op for f in funcs.values() for i in f.insns.values())
    with open(os.path.join(out, "mnemonics.txt"), "w") as fh:
        for k, v in mn.most_common():
            fh.write(f"{v:8d} {k}\n")
    # Uncovered runs, to look for missed functions.
    runs, k = [], 0
    while k < len(cov):
        if cov[k] or elf.u32(an.lo + 4 * k) == 0:
            k += 1
            continue
        j = k
        while j < len(cov) and not cov[j]:
            j += 1
        runs.append((an.lo + 4 * k, j - k))
        k = j
    with open(os.path.join(out, "uncovered.txt"), "w") as fh:
        for va, n in sorted(runs, key=lambda r: -r[1]):
            fh.write(f"{va:08x} {n}\n")
    print(f"[uncovered] {len(runs)} runs, largest {sorted((n for _, n in runs), reverse=True)[:8]}")
    with open(os.path.join(out, "analysis.pkl"), "wb") as fh:
        pickle.dump({"funcs": funcs, "seed_origin": an.seed_origin}, fh)
    if "--analyze-only" in sys.argv:
        return
    from lift import emit_all
    emit_all(elf, an, out)
    print(f"[done] {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
