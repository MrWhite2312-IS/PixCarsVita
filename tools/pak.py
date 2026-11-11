#!/usr/bin/env python3
"""Cars (2006, PC) .pak archive tool.

Format (reverse-engineered, little-endian):
  u32 count
  count * { char name[100] (NUL-terminated, 0xCC padded); u32 offset; u32 size }
  data blob; offset is relative to the end of the table.

Usage:
  pak.py list    <file.pak>
  pak.py stats   <file.pak>
  pak.py extract <file.pak> <out_dir> [substring_filter]
"""
import os, struct, sys
from collections import Counter

ENTRY = 108

def read_index(path):
    with open(path, "rb") as f:
        (count,) = struct.unpack("<I", f.read(4))
        table = f.read(count * ENTRY)
    base = 4 + count * ENTRY
    entries = []
    for i in range(count):
        raw = table[i*ENTRY:(i+1)*ENTRY]
        name = raw[:100].split(b"\0", 1)[0].decode("latin-1")
        off, size = struct.unpack_from("<II", raw, 100)
        entries.append((name, base + off, size))
    return entries

def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    cmd, path = sys.argv[1], sys.argv[2]
    entries = read_index(path)
    total = os.path.getsize(path)
    if cmd == "list":
        for n, o, s in entries:
            print(f"{o:10d} {s:10d} {n}")
    elif cmd == "stats":
        end = max(o + s for _, o, s in entries) if entries else 4
        print(f"files={len(entries)} archive={total} last_byte={end} ok={end <= total}")
        c = Counter(os.path.splitext(n)[1].lower() for n, _, _ in entries)
        sz = Counter()
        for n, _, s in entries: sz[os.path.splitext(n)[1].lower()] += s
        for ext, k in c.most_common():
            print(f"  {ext or '(none)':12} {k:6d} files {sz[ext]/1e6:9.1f} MB")
    elif cmd == "extract":
        out, flt = sys.argv[3], (sys.argv[4] if len(sys.argv) > 4 else "")
        with open(path, "rb") as f:
            for n, o, s in entries:
                if flt and flt.lower() not in n.lower():
                    continue
                dst = os.path.join(out, *n.replace("\\", "/").split("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                f.seek(o)
                with open(dst, "wb") as w:
                    w.write(f.read(s))
    else:
        print(__doc__); sys.exit(1)

if __name__ == "__main__":
    main()
