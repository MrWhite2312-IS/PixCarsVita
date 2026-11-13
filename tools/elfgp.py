#!/usr/bin/env python3
"""Find $gp-relative loads/stores of a small-data global in the PS2 ELF (Ghidra does not list
those as references).

  elfgp.py <gp_hex> <global_hex>        every lw/sw/lwc1/swc1/addiu off($gp) hitting the global
  elfgp.py -g <func_hex> <global_hex>   work out $gp from a load of the global inside func
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfstr import ELF, segments, to_off, to_va  # noqa: E402

OPS = {0x23: "lw", 0x2b: "sw", 0x31: "lwc1", 0x39: "swc1", 0x09: "addiu", 0x37: "ld", 0x3f: "sd"}


def insns(data, segs, start, count):
    o = to_off(segs, start)
    for k in range(count):
        w, = struct.unpack_from("<I", data, o + 4 * k)
        yield start + 4 * k, w


def main():
    a = sys.argv[1:]
    data = open(ELF, "rb").read()
    segs = segments(data)
    if a[0] == "-g":
        func, glob = int(a[1], 16), int(a[2], 16)
        for va, w in insns(data, segs, func, 600):
            if (w >> 21) & 31 == 28 and (w >> 26) in OPS:
                imm = struct.unpack("<h", struct.pack("<H", w & 0xffff))[0]
                print(f"0x{va:x} {OPS[w >> 26]} off {imm} -> gp would be 0x{glob - imm:x}")
        return
    gp, glob = int(a[0], 16), int(a[1], 16)
    for vaddr, off, size in segs:
        for k in range(0, size - 3, 4):
            w, = struct.unpack_from("<I", data, off + k)
            if (w >> 21) & 31 == 28 and (w >> 26) in OPS:
                imm = struct.unpack("<h", struct.pack("<H", w & 0xffff))[0]
                if gp + imm == glob:
                    print(f"0x{vaddr + k:x} {OPS[w >> 26]} r{(w >> 16) & 31}")


if __name__ == "__main__":
    main()
