#!/usr/bin/env python3
"""Tiny pure-Python software renderer for checking decoded .dxg meshes.

Usage: meshview.py <mesh> <out.png> [yaw_deg] [pitch_deg] [size] [--flat]
  <mesh> is a .dxg on disk, or a path inside the paks (e.g. data\\Global\\Chars\\McQ\\Anim\\mcq_reg.dxg).
Renders LOD 0, z-buffered, orthographic, Lambert-shaded. When the mesh comes from the
paks, each batch is textured with stage 1 of its material's first textured pass
(.dxm -> .dxt), with alpha test. --flat disables texturing.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg, dxm, dxt
from dxt import write_png

DATA = os.environ.get("CARS_DATA", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                "..", "PC data", "Data"))


def load_texture(idx, material, near):
    """Return (w, h, rgba) for a material's base texture, or None."""
    mk = idx.find(material, ".dxm", near)
    if not mk:
        return None
    for p in dxm.parse(idx.read(mk).decode("latin-1"))["passes"]:
        st = p.stages.get(1)
        if st and not st["render_target"]:
            tk = idx.find(st["texture"], ".dxt", mk)
            if tk:
                return dxt.decode(dxt.parse(idx.read(tk)))
    return None


def render(mesh, size=512, yaw=35.0, pitch=20.0, textures=None):
    """textures: optional list (per batch) of (w, h, rgba) or None."""
    tris = []  # (positions, uvs or None, batch index)
    for bi, b in enumerate(mesh["lods"][0]["batches"]):
        pos, _, uv = dxg.decode_batch(b)
        for a, c, d in dxg.strip_to_tris(b["strip"]):
            tris.append(((pos[a], pos[c], pos[d]), (uv[a], uv[c], uv[d]) if uv else None, bi))
    if not tris:
        raise SystemExit("no triangles")
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))

    def view(p):
        x, y, z = p
        x, z = x * cy - z * sy, x * sy + z * cy
        y, z = y * cp - z * sp, y * sp + z * cp
        return x, y, z

    vt = [tuple(view(p) for p in t[0]) for t in tris]
    xs = [p[0] for t in vt for p in t]; ys = [p[1] for t in vt for p in t]
    span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1
    sc = (size * 0.9) / span
    ox = size / 2 - (max(xs) + min(xs)) / 2 * sc
    oy = size / 2 + (max(ys) + min(ys)) / 2 * sc
    zbuf = [1e30] * (size * size)
    img = bytearray(b"\x20\x22\x28\xff" * (size * size))
    light = (0.4, 0.8, -0.45)
    for t, (_, uvs, bi) in zip(vt, tris):
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
        ux, uy, uz = x1 - x0, y1 - y0, z1 - z0
        vx, vy, vz = x2 - x0, y2 - y0, z2 - z0
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        shade = 0.45 + 0.55 * abs(nx * light[0] + ny * light[1] + nz * light[2]) / ln
        tex = textures[bi] if textures and uvs else None
        flat = bytes((int(60 + 180 * shade), int(50 + 140 * shade), int(40 + 90 * shade), 255))
        P = [(ox + x * sc, oy - y * sc, z) for x, y, z in t]
        minx = max(0, int(min(p[0] for p in P))); maxx = min(size - 1, int(max(p[0] for p in P)) + 1)
        miny = max(0, int(min(p[1] for p in P))); maxy = min(size - 1, int(max(p[1] for p in P)) + 1)
        (ax, ay, az), (bx, by, bz), (cx, cy_, cz) = P
        area = (bx - ax) * (cy_ - ay) - (by - ay) * (cx - ax)
        if abs(area) < 1e-9:
            continue
        for py in range(miny, maxy + 1):
            for px in range(minx, maxx + 1):
                w0 = ((bx - px) * (cy_ - py) - (by - py) * (cx - px)) / area
                w1 = ((cx - px) * (ay - py) - (cy_ - py) * (ax - px)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * az + w1 * bz + w2 * cz
                k = py * size + px
                if z >= zbuf[k]:
                    continue
                if tex:
                    tw, th, td = tex
                    u = w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]
                    v = w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]
                    o = ((int(v * th) % th) * tw + (int(u * tw) % tw)) * 4
                    if td[o + 3] < 32:  # alpha test
                        continue
                    col = bytes((min(255, int(td[o] * shade)), min(255, int(td[o + 1] * shade)),
                                 min(255, int(td[o + 2] * shade)), 255))
                else:
                    col = flat
                zbuf[k] = z
                img[k * 4:k * 4 + 4] = col
    return size, size, bytes(img)


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(args) < 2:
        print(__doc__); sys.exit(1)
    yaw = float(args[2]) if len(args) > 2 else 35
    pitch = float(args[3]) if len(args) > 3 else 20
    size = int(args[4]) if len(args) > 4 else 512
    textures = None
    if os.path.exists(args[0]):
        m = dxg.parse(open(args[0], "rb").read())
    else:
        idx = dxm.AssetIndex(DATA)
        key = args[0].lower().replace("/", "\\")
        m = dxg.parse(idx.read(key))
        if "--flat" not in sys.argv:
            cache = {}
            textures = []
            for b in m["lods"][0]["batches"]:
                mat = m["materials"][b["material"]]
                if mat not in cache:
                    cache[mat] = load_texture(idx, mat, key)
                    print(f"  {mat}: {'ok' if cache[mat] else 'no texture'}")
                textures.append(cache[mat])
    write_png(args[1], *render(m, size, yaw, pitch, textures))


if __name__ == "__main__":
    main()
