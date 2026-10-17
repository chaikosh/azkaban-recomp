"""Minimal ELF32 little-endian loader for the PS2 executable."""
import struct


class Section:
    def __init__(self, name, typ, addr, off, size):
        self.name, self.type, self.addr, self.off, self.size = name, typ, addr, off, size


class ELF:
    def __init__(self, path):
        d = self.data = open(path, "rb").read()
        assert d[:4] == b"\x7fELF" and d[4] == 1 and d[5] == 1, "need ELF32 LE"
        (_, self.machine, _, self.entry, phoff, shoff, _, _, phes, phn, shes, shn,
         shstr) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        self.segments = []
        for i in range(phn):
            p_type, p_off, p_va, _, p_fsz, p_msz, p_fl, _ = struct.unpack_from("<8I", d, phoff + i * phes)
            if p_type == 1:
                self.segments.append((p_va, p_off, p_fsz, p_msz))
        raw = [struct.unpack_from("<10I", d, shoff + i * shes) for i in range(shn)]
        so = raw[shstr][4] if shn else 0
        self.sections = {}
        for s in raw[1:]:
            name = d[so + s[0]:d.index(b"\0", so + s[0])].decode()
            self.sections[name] = Section(name, s[1], s[3], s[4], s[5])
        # Flat image of the loaded segments (BSS zero-filled).
        self.base = min(s[0] for s in self.segments)
        self.end = max(s[0] + s[3] for s in self.segments)
        img = bytearray(self.end - self.base)
        for va, off, fsz, msz in self.segments:
            img[va - self.base:va - self.base + fsz] = d[off:off + fsz]
        self.image = bytes(img)

    def contains(self, va):
        return self.base <= va < self.end

    def u32(self, va):
        return struct.unpack_from("<I", self.image, va - self.base)[0]

    def text_range(self):
        t = self.sections[".text"]
        return t.addr, t.addr + t.size
