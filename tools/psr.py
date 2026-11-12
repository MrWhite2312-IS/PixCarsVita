#!/usr/bin/env python3
"""Cars (2006, PS2) CARS.PSR archive tool, read straight from the disc image.

Format (little-endian): u32 version (2), 5 * u32 header words, then entries
  { u32 name_len; char name[name_len]; u32 size; u32 sector }
until a zero/absurd name length. Data lives at sector * 2048 from the start of CARS.PSR.

Usage:
  psr.py list    <image.iso> [substring]
  psr.py extract <image.iso> <out_dir> [substring]
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso import Iso, SECTOR


def entries(iso):
    rec = iso.lookup("/CARS.PSR")
    first = iso.read(rec["lba"], 1)
    head_end = struct.unpack_from("<I", first, 16)[0]
    h = iso.read(rec["lba"], head_end // SECTOR + 2)
    p, out = 24, []
    while p < head_end:
        n = struct.unpack_from("<I", h, p)[0]
        if n == 0 or n > 260:
            break
        name = h[p + 4:p + 4 + n].decode("latin-1")
        size, sec = struct.unpack_from("<II", h, p + 4 + n)
        out.append((name, size, sec))
        p += 12 + n
    return rec, out


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    iso = Iso(a[1])
    rec, ents = entries(iso)
    if a[0] == "list":
        flt = a[2].lower() if len(a) > 2 else ""
        for name, size, sec in ents:
            if flt in name.lower():
                print(f"{sec:8d} {size:10d} {name}")
    elif a[0] == "extract":
        flt = a[3].lower() if len(a) > 3 else ""
        for name, size, sec in ents:
            if flt not in name.lower():
                continue
            out = os.path.join(a[2], name.replace("\\", "/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            data = iso.read(rec["lba"] + sec, (size + SECTOR - 1) // SECTOR)[:size]
            with open(out, "wb") as f:
                f.write(data)
            print(out)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
