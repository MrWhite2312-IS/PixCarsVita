#!/usr/bin/env python3
"""Cars (2006) scene resource bundles: <scene>.xbox.res (+ .xbox.str).

.res layout (reverse-engineered, little-endian):
  u32 version = 3
  u32 dir_offset           byte offset of the directory in the .res file
  u32 unknown (320), u32 0, u32 n_chunks
  u16 chunk_compressed_size[n_chunks] ...
  directory @ dir_offset:  u32 0, u32 0, u32 count, u32 unknown
      entry[count]: u32 name_len; char name[name_len]; u32 offset; u32 size
  then, 0x2000-aligned, n zlib streams that decompress to 24 KiB chunks forming one
  virtual blob; entry offsets/sizes address that blob.

Usage:
  scnres.py ls      <scene.res>
  scnres.py extract <scene.res> <out_dir>
"""
import os, struct, sys, zlib


def load(path):
    d = open(path, "rb").read()
    ver, dir_off = struct.unpack_from("<2I", d, 0)
    if ver != 3:
        raise ValueError(f"unsupported .res version {ver}")
    p = dir_off + 8
    count, _ = struct.unpack_from("<2I", d, p)
    p += 8
    entries = []
    for _ in range(count):
        n, = struct.unpack_from("<I", d, p)
        p += 4
        name = d[p:p + n].decode("latin-1")
        p += n
        off, size = struct.unpack_from("<2I", d, p)
        p += 8
        entries.append((name, off, size))
    # zlib chunks follow the directory
    blob = bytearray()
    q = d.find(b"\x78\x9c", p)
    while 0 <= q < len(d):
        o = zlib.decompressobj()
        blob += o.decompress(d[q:])
        q = len(d) - len(o.unused_data)
        nxt = d.find(b"\x78\x9c", q)
        if nxt < 0:
            break
        q = nxt
    return entries, bytes(blob)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    entries, blob = load(a[1])
    if a[0] == "ls":
        print(f"virtual size {len(blob)}")
        for n, o, s in entries:
            ok = "" if o + s <= len(blob) else "  (past end!)"
            print(f"{o:10x} {s:10d} {n}{ok}")
    elif a[0] == "extract":
        for n, o, s in entries:
            dst = os.path.join(a[2], *n.replace("\\", "/").split("/"))
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            open(dst, "wb").write(blob[o:o + s])
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
