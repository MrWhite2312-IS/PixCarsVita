#!/usr/bin/env python3
"""Tiny pure-Python software renderer for checking decoded .dxg meshes.

Usage: meshview.py <mesh> <out.png> [yaw_deg] [pitch_deg] [size] [--flat]
                   [--anim=<path.mot in paks> --time=<seconds>]
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


ALPHA_FUNCS = {
    "NEVER": lambda a, r: False, "LESS": lambda a, r: a < r, "EQUAL": lambda a, r: a == r,
    "LESSEQUAL": lambda a, r: a <= r, "GREATER": lambda a, r: a > r,
    "NOTEQUAL": lambda a, r: a != r, "GREATEREQUAL": lambda a, r: a >= r,
    "ALWAYS": lambda a, r: True,
}


def _tex_from_stage(idx, st, p, mk, extra=None):
    """extra: optional {lowercase texture name: raw .dxt bytes} used when the paks lack it."""
    tk = idx.find(st["texture"], ".dxt", mk)
    raw = idx.read(tk) if tk else (extra or {}).get(st["texture"].lower())
    if not raw:
        return None
    w, h, rgba = dxt.decode(dxt.parse(raw))
    return {
        "w": w, "h": h, "rgba": rgba,
        "clamp_u": st["address_u"] in ("CLAMP", "BORDER"),
        "clamp_v": st["address_v"] in ("CLAMP", "BORDER"),
        "border_u": st["address_u"] == "BORDER",
        "border_v": st["address_v"] == "BORDER",
        # alpha only cuts pixels when the material enables alpha testing;
        # otherwise it is a mask for other passes (reflection strength etc.)
        "alpha_test": ALPHA_FUNCS.get(p.alpha_func) if p.alpha_test else None,
        "blend": p.alpha_blend,
        "ref": p.alpha_ref,
        "offset": (0.0, 0.0),
    }


def uv_at_point(batch, x, y):
    """UV of the batch surface under (x, y), projecting along Z (bind pose)."""
    pos, _, uv = dxg.decode_batch(batch)
    best = None
    for a, b, c in dxg.strip_to_tris(batch["strip"]):
        (ax, ay, _), (bx, by, _), (cx, cy, _) = pos[a], pos[b], pos[c]
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if abs(area) < 1e-12:
            continue
        w0 = ((bx - x) * (cy - y) - (by - y) * (cx - x)) / area
        w1 = ((cx - x) * (ay - y) - (cy - y) * (ax - x)) / area
        w2 = 1 - w0 - w1
        if min(w0, w1, w2) >= -1e-6:
            return (w0 * uv[a][0] + w1 * uv[b][0] + w2 * uv[c][0],
                    w0 * uv[a][1] + w1 * uv[b][1] + w2 * uv[c][1])
    # fallback: nearest vertex
    for i, (px, py, _) in enumerate(pos):
        d = (px - x) ** 2 + (py - y) ** 2
        if best is None or d < best[0]:
            best = (d, uv[i])
    return best[1]


def load_layers(idx, material, near, mesh=None, batch=None, world=None, skin=None):
    """Texture layers to draw for a material, in pass order.

    Normally just the first textured pass (later passes are env-map/specular effects).
    CarsEye materials (the windshield eyes) draw every pass: each pass shifts the UVs
    so the iris texture is centred on the eye node it animates (<Char>_Eye_R / _Eye_L).
    """
    mk = idx.find(material, ".dxm", near)
    if not mk:
        return []
    passes = dxm.parse(idx.read(mk).decode("latin-1"))["passes"]
    eye_passes = [p for p in passes if any(a.get("algorithm", "").lower() == "carseye" for a in p.animators)]
    if eye_passes and mesh is not None:
        bind_world = dxg.world_matrices(mesh)
        layers = []
        for p in eye_passes:
            st = p.stages.get(1)
            tex = st and not st["render_target"] and _tex_from_stage(idx, st, p, mk)
            if not tex:
                continue
            name = next(a.get("name", "") for a in p.animators if a.get("algorithm", "").lower() == "carseye")
            node = dxg.find_node(mesh, "_eye_r" if "right" in name.lower() else "_eye_l")
            if node is not None:
                if world is None:
                    x, y = bind_world[node][12], bind_world[node][13]
                else:
                    # animated gaze: bring the posed eye node back into the bind space of
                    # the eye surface (skinned to the eye node's parent) and look up UVs there
                    import pose
                    par = mesh["nodes"][node]["parent"]
                    inv = pose.invert_rigid(skin[par])
                    ep = world[node][12:15]
                    x = ep[0] * inv[0] + ep[1] * inv[4] + ep[2] * inv[8] + inv[12]
                    y = ep[0] * inv[1] + ep[1] * inv[5] + ep[2] * inv[9] + inv[13]
                u, v = uv_at_point(batch, x, y)
                cu = float(next(a.get("ucenter", ".5") for a in p.animators))
                cv = float(next(a.get("vcenter", ".5") for a in p.animators))
                tex["offset"] = (cu - u, cv - v)
            layers.append(tex)
        return layers
    for p in passes:
        st = p.stages.get(1)
        if st and not st["render_target"]:
            tex = _tex_from_stage(idx, st, p, mk)
            if tex:
                return [tex]
    return []


def sample(tex, u, v):
    """Point-sample with D3D addressing; returns (r, g, b, a) or None for border."""
    w, h = tex["w"], tex["h"]
    x, y = int(u * w), int(v * h)
    if tex["clamp_u"]:
        if tex["border_u"] and not 0 <= x < w:
            return None
        x = min(max(x, 0), w - 1)
    else:
        x %= w
    if tex["clamp_v"]:
        if tex["border_v"] and not 0 <= y < h:
            return None
        y = min(max(y, 0), h - 1)
    else:
        y %= h
    o = (y * w + x) * 4
    return tex["rgba"][o:o + 4]


def render(mesh, size=512, yaw=35.0, pitch=20.0, textures=None, positions=None, frame=None):
    """textures: optional per-batch layer lists. positions: optional per-batch posed
    vertex positions (from pose.skin_batch). frame: optional (cx, cy, span) to keep the
    camera fixed across several renders."""
    tris = []  # (positions, uvs or None, batch index)
    for bi, b in enumerate(mesh["lods"][0]["batches"]):
        pos, _, uv = dxg.decode_batch(b)
        if positions is not None:
            pos = positions[bi]
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
    if frame:
        cxm, cym, span = frame
    else:
        cxm, cym = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
        span = max(max(xs) - min(xs), max(ys) - min(ys)) or 1
    sc = (size * 0.9) / span
    ox = size / 2 - cxm * sc
    oy = size / 2 + cym * sc
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
        layers = textures[bi] if textures and uvs else None
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
                if layers:
                    u = w0 * uvs[0][0] + w1 * uvs[1][0] + w2 * uvs[2][0]
                    v = w0 * uvs[0][1] + w1 * uvs[1][1] + w2 * uvs[2][1]
                    texel = None
                    for tex in layers:  # each pass replaces where it draws (blend ONE/ZERO)
                        t = sample(tex, u + tex["offset"][0], v + tex["offset"][1])
                        if t is None:  # border color (transparent black)
                            t = b"\0\0\0\0"
                        if tex["alpha_test"] and not tex["alpha_test"](t[3], tex["ref"]):
                            continue
                        texel = t
                    if texel is None:
                        continue
                    col = bytes((min(255, int(texel[0] * shade)), min(255, int(texel[1] * shade)),
                                 min(255, int(texel[2] * shade)), 255))
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
    opts = dict(x[2:].split("=", 1) for x in sys.argv[1:] if x.startswith("--") and "=" in x)
    textures = positions = world = skin = None
    idx = None
    if os.path.exists(args[0]):
        m = dxg.parse(open(args[0], "rb").read())
    else:
        idx = dxm.AssetIndex(DATA)
        key = args[0].lower().replace("/", "\\")
        m = dxg.parse(idx.read(key))
    if "anim" in opts:
        import mot, pose
        idx = idx or dxm.AssetIndex(DATA)
        a = mot.parse(idx.read(opts["anim"].lower().replace("/", "\\")))
        world = pose.pose_world(m, a, float(opts.get("time", "0")))
        skin = pose.skin_matrices(m, world)
        positions = [pose.skin_batch(m, b, skin) for b in m["lods"][0]["batches"]]
    if not os.path.exists(args[0]) and "--flat" not in sys.argv:
        cache = {}
        textures = []
        for b in m["lods"][0]["batches"]:
            mat = m["materials"][b["material"]]
            if mat not in cache:
                cache[mat] = load_layers(idx, mat, key, m, b, world, skin)
                print(f"  {mat}: {len(cache[mat])} layer(s)")
            textures.append(cache[mat])
    write_png(args[1], *render(m, size, yaw, pitch, textures, positions))


if __name__ == "__main__":
    main()
