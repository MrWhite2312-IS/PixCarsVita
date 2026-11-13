#!/usr/bin/env python3
"""Find 32-bit words in the PS2 ELF equal to an address (vtables, pointer tables), and dump
the words around each hit.

  elfref.py <hexaddr> [before] [after]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfstr import ELF, segments, to_va


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(1)
    target = int(a[0], 16)
    before = int(a[1]) if len(a) > 1 else 4
    after = int(a[2]) if len(a) > 2 else 4
    data = open(ELF, "rb").read()
    segs = segments(data)
    needle = struct.pack("<I", target)
    p = data.find(needle)
    while p >= 0:
        if p % 4 == 0:
            va = to_va(segs, p)
            print(f"hit at 0x{va:x}" if va is not None else f"hit at file 0x{p:x}")
            for k in range(-before, after + 1):
                q = p + 4 * k
                w, = struct.unpack_from("<I", data, q)
                v = to_va(segs, q)
                print(f"  {'>' if k == 0 else ' '} 0x{v:x} [{k:+d}] 0x{w:08x}")
        p = data.find(needle, p + 1)


if __name__ == "__main__":
    main()
