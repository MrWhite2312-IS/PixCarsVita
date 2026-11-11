#!/usr/bin/env python3
"""Small MIPS (R5900 / PS2 EE subset) disassembler, stdlib only.

Covers the integer ISA (MIPS I-III), COP1 single-precision FPU and the common
EE loads/stores; anything else prints as .word. Good enough to read game logic.

Usage:
  mipsdis.py dis  <elf> <start_hex> [count]       disassemble count instructions
  mipsdis.py find-const <elf> <float>             find lui/ori pairs that build a float
  mipsdis.py refs <elf> <addr_hex>                find lui/addiu|ori|lw address builds
  mipsdis.py str  <elf> <text>                    address of a string
"""
import struct, sys

R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def load_elf(path):
    d = open(path, "rb").read()
    phoff, = struct.unpack_from("<I", d, 0x1C)
    phnum, = struct.unpack_from("<H", d, 0x2C)
    segs = []
    for i in range(phnum):
        t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", d, phoff + 32 * i)
        if t == 1 and fs:
            segs.append((va, d[off:off + fs]))
    return segs


class Mem:
    def __init__(self, segs):
        self.segs = segs

    def word(self, a):
        for va, b in self.segs:
            if va <= a < va + len(b) - 3:
                return struct.unpack_from("<I", b, a - va)[0]
        return None

    def bytes_at(self, a, n):
        for va, b in self.segs:
            if va <= a < va + len(b):
                return b[a - va:a - va + n]
        return b""

    def words(self):
        for va, b in self.segs:
            for i in range(0, len(b) - 3, 4):
                yield va + i, struct.unpack_from("<I", b, i)[0]


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def dis(w, pc):
    op = w >> 26
    rs, rt, rd, sa, fn = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    si = s16(imm)
    tgt = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    br = pc + 4 + (si << 2)
    if w == 0:
        return "nop"
    if op == 0:
        names = {0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or",
                 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x2C: "dadd", 0x2D: "daddu",
                 0x2F: "dsubu", 0x04: "sllv", 0x06: "srlv", 0x07: "srav", 0x0A: "movz", 0x0B: "movn"}
        if fn in names:
            return f"{names[fn]} {R[rd]}, {R[rs]}, {R[rt]}"
        sh = {0x00: "sll", 0x02: "srl", 0x03: "sra", 0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra",
              0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32"}
        if fn in sh:
            return f"{sh[fn]} {R[rd]}, {R[rt]}, {sa}"
        if fn == 0x08:
            return f"jr {R[rs]}"
        if fn == 0x09:
            return f"jalr {R[rd]}, {R[rs]}"
        if fn in (0x18, 0x19, 0x1A, 0x1B):
            return f"{['mult', 'multu', 'div', 'divu'][fn - 0x18]} {R[rs]}, {R[rt]}"
        if fn in (0x10, 0x12):
            return f"{'mfhi' if fn == 0x10 else 'mflo'} {R[rd]}"
        if fn == 0x0F:
            return "sync"
        return f".word 0x{w:08x}"
    if op == 2:
        return f"j 0x{tgt:x}"
    if op == 3:
        return f"jal 0x{tgt:x}"
    if op in (4, 5, 0x14, 0x15):
        n = {4: "beq", 5: "bne", 0x14: "beql", 0x15: "bnel"}[op]
        return f"{n} {R[rs]}, {R[rt]}, 0x{br:x}"
    if op in (6, 7, 0x16, 0x17):
        n = {6: "blez", 7: "bgtz", 0x16: "blezl", 0x17: "bgtzl"}[op]
        return f"{n} {R[rs]}, 0x{br:x}"
    if op == 1:
        n = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 0x11: "bgezal"}.get(rt, f"regimm{rt}")
        return f"{n} {R[rs]}, 0x{br:x}"
    imm_ops = {8: "addi", 9: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
               0x0E: "xori", 0x18: "daddi", 0x19: "daddiu"}
    if op in imm_ops:
        v = imm if op in (0x0C, 0x0D, 0x0E) else si
        return f"{imm_ops[op]} {R[rt]}, {R[rs]}, {v:#x}" if op in (0x0C, 0x0D, 0x0E) else \
            f"{imm_ops[op]} {R[rt]}, {R[rs]}, {v}"
    if op == 0x0F:
        return f"lui {R[rt]}, 0x{imm:x}"
    mem = {0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x27: "lwu", 0x28: "sb",
           0x29: "sh", 0x2B: "sw", 0x37: "ld", 0x3F: "sd", 0x1E: "lq", 0x1F: "sq", 0x1A: "ldl",
           0x1B: "ldr", 0x22: "lwl", 0x26: "lwr", 0x2A: "swl", 0x2E: "swr", 0x2C: "sdl", 0x2D: "sdr"}
    if op in mem:
        return f"{mem[op]} {R[rt]}, {si}({R[rs]})"
    if op == 0x31:
        return f"lwc1 $f{rt}, {si}({R[rs]})"
    if op == 0x39:
        return f"swc1 $f{rt}, {si}({R[rs]})"
    if op == 0x11:  # COP1
        fmt = rs
        ft, fs_, fd = rt, rd, sa
        if fmt == 0:
            return f"mfc1 {R[rt]}, $f{fs_}"
        if fmt == 4:
            return f"mtc1 {R[rt]}, $f{fs_}"
        if fmt == 2:
            return f"cfc1 {R[rt]}, ${fs_}"
        if fmt == 6:
            return f"ctc1 {R[rt]}, ${fs_}"
        if fmt == 8:
            return f"{['bc1f', 'bc1t', 'bc1fl', 'bc1tl'][rt & 3]} 0x{br:x}"
        if fmt == 0x10:  # S
            n = {0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s",
                 7: "neg.s", 0x16: "rsqrt.s", 0x18: "adda.s", 0x19: "suba.s", 0x1A: "mula.s",
                 0x1C: "madd.s", 0x1D: "msub.s", 0x1E: "madda.s", 0x1F: "msuba.s", 0x24: "cvt.w.s",
                 0x28: "max.s", 0x29: "min.s", 0x30: "c.f.s", 0x32: "c.eq.s", 0x34: "c.lt.s",
                 0x36: "c.le.s"}.get(fn)
            if n is None:
                return f".word 0x{w:08x}"
            if n.startswith("c."):
                return f"{n} $f{fs_}, $f{ft}"
            if n in ("sqrt.s", "abs.s", "mov.s", "neg.s", "cvt.w.s"):
                return f"{n} $f{fd}, $f{fs_ if n != 'sqrt.s' else ft}"
            if n in ("adda.s", "suba.s", "mula.s", "madda.s", "msuba.s"):
                return f"{n} $f{fs_}, $f{ft}"
            return f"{n} $f{fd}, $f{fs_}, $f{ft}"
        if fmt == 0x14 and fn == 0x20:
            return f"cvt.s.w $f{fd}, $f{fs_}"
        return f".word 0x{w:08x}"
    if op == 0x1C:
        return f"mmi 0x{w:08x}"
    if op == 0x12:
        return f"cop2 0x{w:08x}"
    return f".word 0x{w:08x}"


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    mem = Mem(load_elf(a[1]))
    if a[0] == "dis":
        start = int(a[2], 16)
        n = int(a[3]) if len(a) > 3 else 64
        for i in range(n):
            pc = start + 4 * i
            w = mem.word(pc)
            if w is None:
                break
            print(f"{pc:08x}: {w:08x}  {dis(w, pc)}")
    elif a[0] == "find-const":
        bits = struct.unpack("<I", struct.pack("<f", float(a[2])))[0]
        hi, lo = bits >> 16, bits & 0xFFFF
        prev = {}
        for pc, w in mem.words():
            op = w >> 26
            if op == 0x0F and (w & 0xFFFF) == hi:
                prev[(w >> 16) & 31] = pc
                if lo == 0:
                    print(f"{pc:08x} lui {R[(w >> 16) & 31]}, 0x{hi:x}")
            elif op == 0x0D and (w & 0xFFFF) == lo and lo:
                r = (w >> 21) & 31
                if r in prev and pc - prev[r] < 64:
                    print(f"{prev[r]:08x}..{pc:08x}  builds {a[2]} in {R[(w >> 16) & 31]}")
    elif a[0] == "refs":
        addr = int(a[2], 16)
        hi = (addr + 0x8000) >> 16
        lo = s16(addr & 0xFFFF)
        last = {}
        for pc, w in mem.words():
            op = w >> 26
            if op == 0x0F and (w & 0xFFFF) == hi:
                last[(w >> 16) & 31] = pc
            elif op in (0x09, 0x23, 0x31, 0x0D) and s16(w & 0xFFFF) == lo:
                r = (w >> 21) & 31
                if r in last and pc - last[r] < 256:
                    print(f"{last[r]:08x}..{pc:08x}: {dis(w, pc)}")
    elif a[0] == "str":
        needle = a[2].encode()
        for va, b in mem.segs:
            i = 0
            while (i := b.find(needle, i)) >= 0:
                print(f"0x{va + i:08x}")
                i += 1
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
