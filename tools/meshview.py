#!/usr/bin/env python3
"""Tiny pure-Python software renderer for checking decoded .dxg meshes.

Usage: meshview.py <file.dxg> <out.png> [yaw_deg] [pitch_deg] [size]
Renders LOD 0 with flat Lambert shading, z-buffered, orthographic.
"""
import math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg
from dxt import write_png


def render(mesh, size=512, yaw=35.0, pitch=20.0):
    tris = []
    for b in mesh["lods"][0]["batches"]:
        pos, _, _ = dxg.decode_batch(b)
        tris += [(pos[a], pos[c], pos[d]) for a, c, d in dxg.strip_to_tris(b["strip"])]
    if not tris:
        raise SystemExit("no triangles")
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))

    def view(p):
        x, y, z = p
        x, z = x * cy - z * sy, x * sy + z * cy
        y, z = y * cp - z * sp, y * sp + z * cp
        return x, y, z

    vt = [tuple(view(p) for p in t) for t in tris]
    xs = [p[0] for t in vt for p in t]; ys = [p[1] for t in vt for p in t]
    span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1
    sc = (size * 0.9) / span
    ox = size / 2 - (max(xs) + min(xs)) / 2 * sc
    oy = size / 2 + (max(ys) + min(ys)) / 2 * sc
    zbuf = [1e30] * (size * size)
    img = bytearray(b"\x20\x22\x28\xff" * (size * size))
    light = (0.4, 0.8, -0.45)
    for t in vt:
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = t
        ux, uy, uz = x1 - x0, y1 - y0, z1 - z0
        vx, vy, vz = x2 - x0, y2 - y0, z2 - z0
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        ln = math.sqrt(nx * nx + ny * ny + nz * nz) or 1
        shade = abs(nx * light[0] + ny * light[1] + nz * light[2]) / ln
        col = bytes((int(60 + 180 * shade), int(50 + 140 * shade), int(40 + 90 * shade), 255))
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
                if z < zbuf[k]:
                    zbuf[k] = z
                    img[k * 4:k * 4 + 4] = col
    return size, size, bytes(img)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    yaw = float(sys.argv[3]) if len(sys.argv) > 3 else 35
    pitch = float(sys.argv[4]) if len(sys.argv) > 4 else 20
    size = int(sys.argv[5]) if len(sys.argv) > 5 else 512
    m = dxg.parse(open(sys.argv[1], "rb").read())
    write_png(sys.argv[2], *render(m, size, yaw, pitch))
