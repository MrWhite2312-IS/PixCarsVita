#!/usr/bin/env python3
"""Read data from the PS2 ELF by virtual address (for reading decompiled Ghidra output).

  elfstr.py <hexaddr> [...]        the C string at each address
  elfstr.py -f <hexaddr> [count]   32-bit floats
  elfstr.py -i <hexaddr> [count]   32-bit ints
  elfstr.py -x <text>              addresses of a C string
"""
import os
import struct
import sys

ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "extracted", "SLUS_211.51")


def segments(data):
    phoff, = struct.unpack_from("<I", data, 0x1c)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2a)
    out = []
    for i in range(phnum):
        _, off, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        out.append((vaddr, off, filesz))
    return out


def to_off(segs, va):
    for vaddr, off, size in segs:
        if vaddr <= va < vaddr + size:
            return off + va - vaddr
    raise ValueError(f"0x{va:x} not in a file-backed segment")


def to_va(segs, off):
    for vaddr, o, size in segs:
        if o <= off < o + size:
            return vaddr + off - o
    return None


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(1)
    data = open(ELF, "rb").read()
    segs = segments(data)
    if a[0] in ("-f", "-i"):
        va, n = int(a[1], 16), int(a[2]) if len(a) > 2 else 1
        o = to_off(segs, va)
        fmt = "<f" if a[0] == "-f" else "<i"
        for k in range(n):
            print(f"0x{va + 4 * k:x}: {struct.unpack_from(fmt, data, o + 4 * k)[0]}")
    elif a[0] == "-x":
        needle = a[1].encode("latin-1") + b"\0"
        p = data.find(needle)
        while p >= 0:
            va = to_va(segs, p)
            print(f"0x{va:x}" if va is not None else f"file 0x{p:x}")
            p = data.find(needle, p + 1)
    else:
        for h in a:
            va = int(h, 16)
            o = to_off(segs, va)
            print(f"0x{va:x}: {data[o:data.index(b'\0', o)].decode('latin-1')!r}")


if __name__ == "__main__":
    main()
