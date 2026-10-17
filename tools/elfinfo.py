import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'\x7fELF', 'not ELF'
e_type, e_mach, e_ver, e_entry, e_phoff, e_shoff, e_flags, e_ehsz, e_phes, e_phn, e_shes, e_shn, e_shstr = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
print(f'class={d[4]} mach={e_mach} entry={e_entry:#x} flags={e_flags:#x} phnum={e_phn} shnum={e_shn}')
for i in range(e_phn):
    p_type, p_off, p_va, p_pa, p_fsz, p_msz, p_fl, p_al = struct.unpack_from('<8I', d, e_phoff + i*e_phes)
    print(f' PH type={p_type:#x} off={p_off:#x} va={p_va:#x} filesz={p_fsz:#x} memsz={p_msz:#x} flags={p_fl}')
if e_shn:
    shs = [struct.unpack_from('<10I', d, e_shoff + i*e_shes) for i in range(e_shn)]
    so = shs[e_shstr][4]
    nm = lambda o: d[so+o:d.index(b'\0', so+o)].decode()
    for s in shs:
        print(f' SH {nm(s[0]):24s} type={s[1]:#x} addr={s[3]:#x} off={s[4]:#x} size={s[5]:#x}')
