#!/usr/bin/env python3
"""Cars (2006, PC) .dxg mesh tool. Pure stdlib.

Format (reverse-engineered, little-endian, version 5):
  char magic[4] = "dxg\\0"; u32 version = 5; u32 node_count
  node[node_count] (224 bytes):
      char name[128]; f32 matrix[16] (row-major, translation in row 3)
      f32 center[3]; f32 extent[3]; f32 radius; i32 parent
  u32 material_count; char material[material_count][64]
  u8  lod_count; u8 lod_flags; u16 pad      (lod_flags != 0 -> bone palettes at end)
  lod[lod_count]:
      f32 max_distance; u32 batch_count
      batch[batch_count]:
          u32 material; u32 vflags
          if vflags & 0x2000 (quantized):
              f32 pos_scale, normal_scale, uv_scale, uv2_scale
              u8  unknown[16]
              u32 n; u16 unknown; f32 unknown[n]
              u8  decl[16]                   vertex declaration, see STREAMS
          else (plain floats, decl = FLOAT_DECL):
              u32 n; u16 unknown; f32 unknown[n]
          u16 vertex_count; u16 index_count
          u16 indices[index_count]           triangle STRIP, degenerate joins
          vertex streams, one after another (struct-of-arrays), see STREAMS
  if lod_flags (and data remains):
      u32 palette_count | 0x80000000 (per-batch map) or | 0x40000000 (one shared palette)
      palette[]: u32 n; u8 node_index[n]
      u8 batch_palette[total batches]    only with 0x80000000

Usage:
  dxg.py info <file.dxg>
  dxg.py obj  <file.dxg> <out.obj>
  dxg.py scan <file.pak>...
"""
import os, struct, sys

# vflags bit -> (stream name, decl slots), in on-disk order.
# Each decl byte describes one slot: high nibble = component count,
# low nibble = component type (see DECL_TYPES).
STREAMS = [
    (0x0001, "position", (0,)),     # usually s16 x3 * pos_scale
    (0x0002, "normal",   (1,)),     # s16 x3 * 1/32768 or s8 x3 * 1/127
    (0x0008, "color",    (2,)),     # u8 x4 (BGRA)
    (0x0004, "uv",       (3,)),     # s16 x2 * uv_scale
    (0x0010, "uv2",      (10,)),    # f32 x2 lightmap coords (unverified)
    (0x0040, "skin_a",   (4,)),     # 1 x 4 bytes, meaning unverified
    (0x1000, "skin_b",   (8, 9)),   # f32x4 + f32x4: likely indices + weights
]
KNOWN_FLAGS = 0x2000 | sum(b for b, _, _ in STREAMS)
# type nibble -> (struct code, bytes)
DECL_TYPES = {0: ("B", 1), 2: ("f", 4), 4: ("b", 1), 5: ("h", 2)}
# Declaration implied for batches without vflags 0x2000 (all-float vertices).
FLOAT_DECL = bytes([0x32, 0x32, 0x40, 0x22, 0x12, 0, 0, 0, 0x42, 0x42, 0x22, 0, 0, 0, 0, 0])


def slot_format(decl, slot):
    """struct format string ('<3h' etc.) for one decl slot."""
    count, typ = decl[slot] >> 4, decl[slot] & 15
    if typ not in DECL_TYPES:
        raise DxgError(f"unknown decl type {decl[slot]:#04x} in slot {slot}")
    return f"<{count}{DECL_TYPES[typ][0]}"


class DxgError(Exception):
    pass


class Reader:
    def __init__(self, buf):
        self.b, self.p = buf, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v

    def raw(self, n):
        if self.p + n > len(self.b):
            raise DxgError(f"read past end at {self.p:#x} (+{n})")
        v = self.b[self.p:self.p + n]
        self.p += n
        return v


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def parse(buf):
    r = Reader(buf)
    magic, ver = r.take("4sI")
    if magic != b"dxg\0" or ver != 5:
        raise DxgError(f"bad header {magic!r} v{ver}")
    if len(buf) == 8:  # empty placeholder file (header only)
        return {"nodes": [], "materials": [], "lods": [], "lod_flags": 0, "palettes": []}
    nnodes, = r.take("I")
    nodes = []
    for _ in range(nnodes):
        name = cstr(r.raw(128))
        m = r.take("16f")
        c, e, rad = r.take("3f"), r.take("3f"), r.take("f")[0]
        parent = r.take("i")[0]
        nodes.append({"name": name, "matrix": m, "center": c, "extent": e,
                      "radius": rad, "parent": parent})
    nmat, = r.take("I")
    materials = [cstr(r.raw(64)) for _ in range(nmat)]
    nlods, lod_flags, _ = r.take("BBH")
    lods, all_batches = [], []
    for _ in range(nlods):
        dist, nb = r.take("fI")
        batches = []
        for _ in range(nb):
            mat, vflags = r.take("II")
            if vflags & ~KNOWN_FLAGS:
                raise DxgError(f"unknown vflags {vflags:#x} at {r.p:#x}")
            if vflags & 0x2000:          # quantized: scales + explicit declaration
                scales = r.take("4f")
                r.raw(16)
                n, _u = r.take("IH")
                r.take(f"{n}f")
                decl = r.raw(16)
            else:                        # plain float vertices, implicit declaration
                scales = (1.0, 1.0, 1.0, 1.0)
                n, _u = r.take("IH")
                r.take(f"{n}f")
                decl = FLOAT_DECL
            vc, ic = r.take("HH")
            idx = r.take(f"{ic}H")
            streams = {}
            for bit, name, slots in STREAMS:
                if vflags & bit:
                    for k, slot in enumerate(slots):
                        key = name if len(slots) == 1 else f"{name}{k}"
                        streams[key] = r.raw(vc * struct.calcsize(slot_format(decl, slot)))
            b = {"material": mat, "vflags": vflags, "scales": scales, "decl": decl,
                 "vertex_count": vc, "strip": idx, "streams": streams}
            if idx and max(idx) >= vc:
                raise DxgError(f"index {max(idx)} >= vertex count {vc}")
            batches.append(b)
            all_batches.append(b)
        lods.append({"distance": dist, "batches": batches})
    palettes = []
    if lod_flags and r.p < len(buf):  # some flagged files simply end here
        npal, = r.take("I")
        for _ in range(npal & 0x3fffffff):
            n, = r.take("I")
            palettes.append(list(r.raw(n)))
        for b in all_batches:
            # 0x80000000: one palette index per batch follows; 0x40000000: shared palette 0
            b["palette"] = r.take("B")[0] if npal & 0x80000000 else 0
    if r.p != len(buf):
        raise DxgError(f"{len(buf) - r.p} trailing bytes at {r.p:#x}")
    return {"nodes": nodes, "materials": materials, "lods": lods,
            "lod_flags": lod_flags, "palettes": palettes}


def strip_to_tris(strip):
    tris = []
    for i in range(len(strip) - 2):
        a, b, c = strip[i], strip[i + 1], strip[i + 2]
        if a == b or b == c or a == c:
            continue
        tris.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return tris


def decode_batch(b):
    """Return positions, normals, uvs as lists of tuples (floats)."""
    ps, ns, us, _ = b["scales"]

    def read(name, slot, scale):
        if name not in b["streams"]:
            return None
        fmt = slot_format(b["decl"], slot)
        if fmt.endswith("f"):
            scale = 1.0  # float components are stored unscaled
        return [tuple(v * scale for v in vals)
                for vals in struct.iter_unpack(fmt, b["streams"][name])]

    return read("position", 0, ps), read("normal", 1, ns), read("uv", 3, us)


def write_obj(mesh, path, lod=0):
    """Export one LOD as Wavefront OBJ. Skinned meshes come out in bind pose."""
    base = 1
    with open(path, "w") as f:
        f.write("# exported by PixCarsVita tools/dxg.py\n")
        for bi, b in enumerate(mesh["lods"][lod]["batches"]):
            pos, nrm, uv = decode_batch(b)
            f.write(f"o batch{bi}_{mesh['materials'][b['material']]}\n")
            f.write(f"usemtl {mesh['materials'][b['material']]}\n")
            for x, y, z in pos:
                f.write(f"v {x:.5f} {y:.5f} {z:.5f}\n")
            for u, v in uv or []:
                f.write(f"vt {u:.5f} {1 - v:.5f}\n")
            for x, y, z in nrm or []:
                f.write(f"vn {x:.5f} {y:.5f} {z:.5f}\n")
            for t in strip_to_tris(b["strip"]):
                ids = [i + base for i in t]
                if uv and nrm:
                    f.write("f " + " ".join(f"{i}/{i}/{i}" for i in ids) + "\n")
                elif nrm:
                    f.write("f " + " ".join(f"{i}//{i}" for i in ids) + "\n")
                else:
                    f.write("f " + " ".join(map(str, ids)) + "\n")
            base += len(pos)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    if a[0] == "info":
        m = parse(open(a[1], "rb").read())
        print(f"nodes={len(m['nodes'])} materials={m['materials']} palettes={len(m['palettes'])}")
        for li, l in enumerate(m["lods"]):
            for b in l["batches"]:
                print(f"  lod{li} mat={m['materials'][b['material']]:24} vflags={b['vflags']:#06x} "
                      f"verts={b['vertex_count']:5d} tris={len(strip_to_tris(b['strip']))}")
    elif a[0] == "obj":
        write_obj(parse(open(a[1], "rb").read()), a[2])
    elif a[0] == "scan":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from pak import read_index
        ok, bad = 0, []
        for pk in a[1:]:
            with open(pk, "rb") as f:
                for n, o, s in read_index(pk):
                    if not n.lower().endswith(".dxg"):
                        continue
                    f.seek(o)
                    try:
                        parse(f.read(s)); ok += 1
                    except Exception as e:  # noqa
                        bad.append((n, repr(e)))
        print(f"ok={ok} bad={len(bad)}")
        for n, e in bad[:25]:
            print("  ", n, e)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
