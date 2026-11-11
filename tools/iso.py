#!/usr/bin/env python3
"""Minimal ISO 9660 reader (stdlib only) for PS2 discs.

Usage:
  iso.py ls      <image.iso> [dir]          list a directory (default: root)
  iso.py find    <image.iso> [substring]    recursive listing
  iso.py extract <image.iso> <path-in-iso> <out-file>
"""
import os, struct, sys

SECTOR = 2048


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16, 1)
        if pvd[1:6] != b"CD001":
            raise ValueError("not an ISO 9660 image")
        root = pvd[156:190]
        self.root = self._record(root)

    def read(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    @staticmethod
    def _record(b):
        ln = b[0]
        lba = struct.unpack_from("<I", b, 2)[0]
        size = struct.unpack_from("<I", b, 10)[0]
        flags = b[25]
        nlen = b[32]
        name = b[33:33 + nlen].decode("latin-1")
        return {"lba": lba, "size": size, "dir": bool(flags & 2), "name": name, "len": ln}

    def listdir(self, rec):
        data = self.read(rec["lba"], (rec["size"] + SECTOR - 1) // SECTOR)
        out, p = [], 0
        while p < len(data):
            ln = data[p]
            if ln == 0:  # records don't cross sectors: skip to next sector
                p = (p // SECTOR + 1) * SECTOR
                continue
            r = self._record(data[p:p + ln])
            p += ln
            if r["name"] in ("\x00", "\x01"):
                continue
            r["name"] = r["name"].split(";")[0]
            out.append(r)
        return out

    def lookup(self, path):
        rec = self.root
        for part in [x for x in path.replace("\\", "/").split("/") if x]:
            for r in self.listdir(rec):
                if r["name"].upper() == part.upper():
                    rec = r
                    break
            else:
                raise FileNotFoundError(path)
        return rec

    def walk(self, rec=None, prefix=""):
        rec = rec or self.root
        for r in self.listdir(rec):
            p = prefix + "/" + r["name"]
            yield p, r
            if r["dir"]:
                yield from self.walk(r, p)

    def extract(self, rec, out):
        left, lba = rec["size"], rec["lba"]
        with open(out, "wb") as w:
            while left > 0:
                n = min(left, 64 * SECTOR)
                w.write(self.read(lba, (n + SECTOR - 1) // SECTOR)[:n])
                left -= n
                lba += n // SECTOR


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    iso = Iso(a[1])
    if a[0] == "ls":
        for r in iso.listdir(iso.lookup(a[2]) if len(a) > 2 else iso.root):
            print(f"{'d' if r['dir'] else '-'} {r['size']:12d} {r['name']}")
    elif a[0] == "find":
        flt = a[2].lower() if len(a) > 2 else ""
        for p, r in iso.walk():
            if flt in p.lower():
                print(f"{'d' if r['dir'] else '-'} {r['size']:12d} {p}")
    elif a[0] == "extract":
        os.makedirs(os.path.dirname(os.path.abspath(a[3])), exist_ok=True)
        iso.extract(iso.lookup(a[2]), a[3])
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
