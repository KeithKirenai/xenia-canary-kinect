"""Offline PE/XEX image analyzer for Kinect Adventures reverse engineering.

The guest image dumped by `DumpGuestImageForDiagnostics` is an in-memory PE
image: every section is mapped at `ImageBase + RVA`, so file offset == RVA.
That lets us parse imports/exports and scan code straight out of the dump
without ever booting the emulator again.

Usage:
    python tools/xex.py guest_nui_dump.bin imports
    python tools/xex.py guest_nui_dump.bin xrefs 0x82D3065C
    python tools/xex.py guest_nui_dump.bin callfrom NuiSkeletonGetNextFrame
    python tools/xex.py guest_nui_dump.bin find "session"
"""

import struct
import sys

IMAGE_BASE = None


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.base, self.sections = self._parse_pe()
        # Import thunk address -> name, and the reverse.
        self.imports = {}
        self.thunks = {}
        self._parse_imports()

    # --- PE plumbing -----------------------------------------------------

    def _parse_pe(self):
        d = self.data
        if d[:2] != b"MZ":
            raise SystemExit("not an MZ image")
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise SystemExit("bad PE signature")
        coff = e_lfanew + 4
        n_sections, = struct.unpack_from("<H", d, coff + 2)
        size_opt, = struct.unpack_from("<H", d, coff + 16)
        opt = coff + 20
        magic, = struct.unpack_from("<H", d, opt)
        if magic == 0x10B:            # PE32
            base, = struct.unpack_from("<I", d, opt + 28)
            n_dirs, = struct.unpack_from("<I", d, opt + 92)
            dir_off = opt + 96
        elif magic == 0x20B:          # PE32+
            base, = struct.unpack_from("<Q", d, opt + 24)
            n_dirs, = struct.unpack_from("<I", d, opt + 108)
            dir_off = opt + 112
        else:
            raise SystemExit("unknown optional header magic %#x" % magic)

        sections = []
        sec_off = opt + size_opt
        for i in range(n_sections):
            o = sec_off + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
            sections.append((name, base + vaddr, vsize, rawsize))
        self._n_dirs = n_dirs
        self._dir_off = dir_off
        return base, sections

    def _dir(self, index):
        """Return (rva, size) for a data directory, or (0, 0)."""
        if index >= self._n_dirs:
            return (0, 0)
        rva, size = struct.unpack_from("<II", self.data, self._dir_off + index * 8)
        return (rva, size)

    # Since the dump is an in-memory image, RVA == file offset.
    def off(self, vaddr):
        return vaddr - self.base

    def read(self, vaddr, n):
        o = self.off(vaddr)
        return self.data[o:o + n]

    def u32(self, vaddr):
        return struct.unpack_from(">I", self.data, self.off(vaddr))[0]

    def u16(self, vaddr):
        return struct.unpack_from(">H", self.data, self.off(vaddr))[0]

    def cstr(self, vaddr, limit=256):
        o = self.off(vaddr)
        end = self.data.find(b"\0", o, o + limit)
        if end < 0:
            end = o + limit
        return self.data[o:end].decode("ascii", "replace")

    # --- imports ---------------------------------------------------------

    def _parse_imports(self):
        # NOTE: a retail XEX keeps its import descriptors encrypted, so this
        # usually yields nothing. The authoritative name->thunk map comes from
        # Xenia's own import dump (see tools/thunks.py); fail soft here.
        imp_rva, imp_size = self._dir(1)
        if not imp_rva or imp_rva + 20 > len(self.data):
            return
        o = imp_rva
        while True:
            if o + 20 > len(self.data):
                return
            oft, ts, fwd, name_rva, ft = struct.unpack_from("<IIIII", self.data, o)
            if not (oft or name_rva or ft):
                break
            # Sanity: a descriptor's fields must be RVAs inside the image.
            if max(oft, name_rva, ft) >= len(self.data):
                return
            dll = self.cstr(self.base + name_rva)
            thunk_rva = oft or ft
            t = thunk_rva
            i = 0
            while True:
                val, = struct.unpack_from("<I", self.data, t + i * 4)
                if val == 0:
                    break
                addr = self.base + ft + i * 4
                if val & 0x80000000:
                    nm = "%s!#%d" % (dll, val & 0xFFFF)
                else:
                    nm = self.cstr(self.base + val + 2)
                self.imports[addr] = (dll, nm)
                self.thunks[nm] = addr
                i += 1
            o += 20

    def exec_sections(self):
        return [s for s in self.sections if s[0] in (".text", "_TEXT", "PSFD00", ".embsec_")]


# --- minimal PPC scanner ------------------------------------------------

def bl_targets(code, vaddr):
    """Yield (site, target) for every PPC branch-with-link (`bl`/`bla`).

    Encoding: opcode 0b010010 in bits 0-5, AA in bit 1, LK in bit 0, and a
    26-bit signed displacement in bits 6-29 already shifted left by 2.
    """
    for o in range(0, len(code) - 4, 4):
        op, = struct.unpack_from(">I", code, o)
        if (op & 0xFC000003) != 0x48000001:      # bl = 0x48000001, bla = 0x48000003
            continue
        imm = op & 0x03FFFFFC
        if imm & 0x02000000:                      # sign-extend the 26-bit field
            imm -= 0x04000000
        yield vaddr + o, vaddr + o + imm


def iter_calls(img):
    """Yield (site, target) for every `bl`/`bla` in executable sections."""
    for name, vaddr, vsize, rawsize in img.exec_sections():
        size = min(vsize, rawsize) if rawsize else vsize
        code = img.read(vaddr, size)
        for site, target in bl_targets(code, vaddr):
            yield site, target


def disasm_region(img, vaddr, count, window=0x800):
    """Disassemble `count` instructions starting exactly at `vaddr`.

    r2 cannot be trusted on a raw XEX image here. With a PE loaded it applies
    its own function/alignment heuristics, and with `-B 0 -m <base>` it still
    mis-maps the offset -- both silently decode the guest's instructions a few
    bytes off (the `bl XamNuiIdentityGetSessionId` really at 0x82D3065C gets
    rendered as `lwz r6, 0x48(r8)`). So carve the window out into a scratch
    file and map *that* at the exact vaddr: no base arithmetic, no heuristics.

    `window` is how far *before* vaddr the scratch window starts; the file
    must extend past vaddr or r2 reads EOF (0xffffffff) instead of code.
    """
    import os
    import subprocess
    import tempfile

    start = (vaddr - window) & ~3
    # Need vaddr..vaddr+4*count inside the file, plus slack for alignment.
    span = (vaddr - start) + 4 * count + 0x40
    chunk = img.read(start, span)
    fd, tmp = tempfile.mkstemp(suffix=".bin")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(chunk)
        cmd = ["r2", "-q", "-a", "ppc", "-b", "32", "-B", "0",
               "-m", hex(start), "-e", "cfg.bigendian=true", "-e", "scr.color=0",
               "-c", "s %#x; pd %d" % (vaddr, count), tmp]
        out = subprocess.run(cmd, capture_output=True, text=True)
    finally:
        os.unlink(tmp)

    res = []
    for l in (out.stdout or "").splitlines():
        if not l.strip():
            continue
        parts = l.split(None, 2)
        # Drop r2's flow-graph gutter characters so the text stays readable.
        if len(parts) >= 2 and len(parts[0]) <= 10 and all(
                c in "0123456789abcdefABCDEFx" for c in parts[0]):
            res.append("%s  %s" % (parts[0], parts[2] if len(parts) > 2 else parts[1]))
        else:
            res.append(l)
    return "\n".join(res)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    path, cmd = argv[1], argv[2]
    img = Image(path)
    img.path = path

    if cmd == "base":
        print("ImageBase %#x" % img.base)
        for s in img.sections:
            print("  %-10s %#010x  vsize %#x  raw %#x" % s)
    elif cmd == "imports":
        pat = argv[3] if len(argv) > 3 else ""
        rows = sorted((a, v) for a, v in img.imports.items() if pat.lower() in v[1].lower())
        for a, (dll, nm) in rows:
            print("%#010x  %-28s %s" % (a, dll, nm))
        print("--- %d imports%s" % (len(rows), (" matching %r" % pat) if pat else ""))
    elif cmd == "callfrom":
        # Which guest call sites call a given import thunk?
        want = argv[3]
        thunks = set(a for a, (d, n) in img.imports.items()
                     if n == want or n.lower().endswith(want.lower()))
        if not thunks:
            cand = [(a, v) for a, v in img.imports.items() if want.lower() in v[1].lower()]
            print("no exact match; candidates:", cand[:10])
            return 1
        hits = []
        for site, target in iter_calls(img):
            if target in thunks:
                hits.append(site)
        for h in hits:
            print("call %#010x -> %s" % (h, want))
        print("--- %d call sites" % len(hits))
    elif cmd == "dis":
        print(disasm_region(img, int(argv[3], 0), int(argv[4]) if len(argv) > 4 else 40))
    elif cmd == "dump":
        a = int(argv[3], 0)
        n = int(argv[4], 0)
        for i in range(0, n, 16):
            chunk = img.read(a + i, 16)
            txt = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
            print("%#010x  %-47s  %s" % (a + i, chunk.hex(" "), txt))
    elif cmd == "find":
        needle = argv[3].encode("ascii")
        start = 0
        while True:
            i = img.data.find(needle, start)
            if i < 0:
                break
            print("%#010x  off %#x" % (img.base + i, i))
            start = i + 1
    else:
        print("unknown command", cmd)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))