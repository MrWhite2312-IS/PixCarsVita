#!/usr/bin/env python3
"""Compose a Cars (2006) scene from .toy / .scn placement files.

A .toy is an INI file; [SoultreeToyN] sections place static meshes:
    SLT=<mesh name>            -> <mesh name>.dxg found in the paks (near the toy)
    Position=x,y,z             world position
    LookVector=x,y,z           local +Z axis in world space
    UpVector=x,y,z             local +Y axis in world space
The .scn [ModelN] sections use the same keys (SLT/Position/LookVector/UpVector).

World matrix (row-major, v' = v * M, D3D left-handed):
    row0 = right = normalize(up x look), row1 = up, row2 = look, row3 = position

Usage:
  scene.py list   <toy-or-scn in paks>
  scene.py view   <toy-or-scn in paks> <out.png> ex ey ez tx ty tz [w h fov]
      perspective view from eye (ex,ey,ez) towards (tx,ty,tz); env SCENE_FAR (default 6000)
  scene.py render <toy-or-scn in paks> <out.png> [yaw pitch size]
      (env SCENE_RADIUS: keep instances within this distance of the median, default 3000)
"""
import math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg, dxm

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))


def vec(s):
    return tuple(float(x.strip().rstrip("f")) for x in s.split(",")[:3])


def norm(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / n for c in v)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def placement_matrix(pos, look=(0, 0, 1), up=(0, 1, 0)):
    look = norm(look)
    up = norm(up)
    right = norm(cross(up, look))
    up = cross(look, right)  # re-orthogonalise
    return (right[0], right[1], right[2], 0.0,
            up[0], up[1], up[2], 0.0,
            look[0], look[1], look[2], 0.0,
            pos[0], pos[1], pos[2], 1.0)


def lights(idx, key):
    """The scene's [LightN] and [Fog] (colours 0..1, may be negative): {'ambient': rgb,
    'dir': [(L toward the light, rgb)], 'spot': [(pos, look, inner_deg, outer_deg, att, range, rgb)],
    'fog': (rgb, min, max) or None}. The game lights every object with ambient + 3 directional
    lights (vertex shader DXVAnimSkin4Diffuse / DXVDiffuse)."""
    secs = dxm.parse_ini(idx.read(key).decode("latin-1"))
    out = {"ambient": (0.0, 0.0, 0.0), "dir": [], "spot": [], "fog": None}

    def rgb(kv, k="colorrgb"):
        return tuple(float(x) / 255.0 for x in kv[k][1].split(",")[:3]) if k in kv else (0.0, 0.0, 0.0)

    for sec, kv in secs:
        s = sec.lower()
        if s == "fog" and "colorrgb" in kv:
            out["fog"] = (rgb(kv), float(kv.get("minimumfogdistance", ("", "0"))[1]),
                          float(kv.get("maximumfogdistance", ("", "0"))[1]))
        if not (s.startswith("light") and s[5:].isdigit()):
            continue
        try:
            add_light(out, kv, rgb)
        except ValueError:  # a malformed value in the .scn (cd.scn has "52604'7"): skip it
            pass
    return out


def add_light(out, kv, rgb):
    if True:
        typ = kv.get("type", ("", ""))[1].strip().strip('"').lower()
        if kv.get("initialzone", ("", "1"))[1].strip() != "1":
            return  # zone 1 is the open world; other zones (interiors) have their own lights
        if kv.get("emitslight", ("", "1"))[1].strip().upper() in ("0", "F", "FALSE"):
            return
        if typ == "ambient":
            out["ambient"] = tuple(a + b for a, b in zip(out["ambient"], rgb(kv)))
        elif typ == "directional":
            if "direction" in kv:
                d = norm(vec(kv["direction"][1]))
            else:
                p, t = vec(kv["position"][1]), vec(kv["targetposition"][1])
                d = norm(tuple(b - a for a, b in zip(p, t)))
            out["dir"].append((tuple(-c for c in d), rgb(kv)))
        elif typ in ("spotlight", "objectpoint"):
            att = vec(kv["attenuation"][1]) if "attenuation" in kv else (1.0, 0.0, 0.0)
            look = norm(vec(kv["lookvector"][1])) if "lookvector" in kv else (0.0, -1.0, 0.0)
            out["spot"].append((vec(kv["position"][1]), look,
                                float(kv.get("innerconeangle", ("", "360"))[1]),
                                float(kv.get("outerconeangle", ("", "360"))[1]), att,
                                float(kv.get("range", ("", "0"))[1]), rgb(kv)))


def instances(idx, key):
    """[(mesh_key, world_matrix, name)] for every placed static mesh."""
    secs = dxm.parse_ini(idx.read(key).decode("latin-1"))
    out, missing = [], []
    for sec, kv in secs:
        s = sec.lower()
        if not (s.startswith("soultreetoy") or s.startswith("model")):
            continue
        slt = kv.get("slt", (None, None))[1]
        if not slt or "position" not in kv:
            continue
        mk = idx.find(slt.strip(), ".dxg", key)
        if not mk:
            missing.append(slt.strip())
            continue
        look = vec(kv["lookvector"][1]) if "lookvector" in kv else (0, 0, 1)
        up = vec(kv["upvector"][1]) if "upvector" in kv else (0, 1, 0)
        name = kv.get("name", (None, slt))[1]
        out.append((mk, placement_matrix(vec(kv["position"][1]), look, up), name))
    return out, missing


def transform(p, m):
    x, y, z = p
    return (x * m[0] + y * m[4] + z * m[8] + m[12],
            x * m[1] + y * m[5] + z * m[9] + m[13],
            x * m[2] + y * m[6] + z * m[10] + m[14])


def build(idx, inst):
    """World-space batches for a list of instances: (batches, positions, layers)."""
    import meshview
    batches, positions, layers, cache, meshes = [], [], [], {}, {}
    for mk, m, _ in inst:
        if mk not in meshes:
            meshes[mk] = dxg.parse(idx.read(mk))
        mesh = meshes[mk]
        world = dxg.world_matrices(mesh)
        nm = world[0] if mesh["nodes"] else (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        full = dxg.mat_mul(nm, m)
        for b in mesh["lods"][0]["batches"]:
            pos, _, _ = dxg.decode_batch(b)
            positions.append([transform(p, full) for p in pos])
            batches.append(b)
            mat = mesh["materials"][b["material"]]
            if (mk, mat) not in cache:
                cache[(mk, mat)] = meshview.load_layers(idx, mat, mk)
            layers.append(cache[(mk, mat)])
    return batches, positions, layers


def render_perspective(batches, positions, layers, eye, target, w, h, fov=60.0, far=6000.0,
                       sky=(150, 170, 200)):
    """Pinhole camera, z-buffered, perspective-correct texturing, distance fog."""
    import meshview
    fwd = norm(tuple(t - e for t, e in zip(target, eye)))
    right = norm(cross((0, 1, 0), fwd))
    up = cross(fwd, right)
    f = (h / 2) / math.tan(math.radians(fov) / 2)
    near = 1.0
    img = bytearray(bytes((sky[0], sky[1], sky[2], 255)) * (w * h))
    zbuf = [far] * (w * h)
    light = norm((0.4, 0.8, 0.3))
    # opaque first, alpha-blended (decals, baked shadows) last
    order = sorted(zip(batches, positions, layers), key=lambda x: bool(x[2] and x[2][0].get("blend")))
    for b, pos, lay in order:
        _, _, uv = dxg.decode_batch(b)
        cam = []
        for p in pos:
            d = (p[0] - eye[0], p[1] - eye[1], p[2] - eye[2])
            cam.append((d[0] * right[0] + d[1] * right[1] + d[2] * right[2],
                        d[0] * up[0] + d[1] * up[1] + d[2] * up[2],
                        d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2]))
        for ia, ib, ic in dxg.strip_to_tris(b["strip"]):
            A, B, C = cam[ia], cam[ib], cam[ic]
            if A[2] < near or B[2] < near or C[2] < near:
                continue  # crude near clipping: drop triangles crossing the camera plane
            if A[2] > far and B[2] > far and C[2] > far:
                continue
            P = [(w / 2 + v[0] * f / v[2], h / 2 - v[1] * f / v[2], v[2]) for v in (A, B, C)]
            (ax, ay, az), (bx, by, bz), (cx, cy, cz) = P
            area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
            if abs(area) < 1e-6:
                continue
            minx = max(0, int(min(ax, bx, cx))); maxx = min(w - 1, int(max(ax, bx, cx)) + 1)
            miny = max(0, int(min(ay, by, cy))); maxy = min(h - 1, int(max(ay, by, cy)) + 1)
            if minx > maxx or miny > maxy:
                continue
            # flat lighting from the world-space face normal
            pa, pb, pc = pos[ia], pos[ib], pos[ic]
            u_ = [pb[i] - pa[i] for i in range(3)]; v_ = [pc[i] - pa[i] for i in range(3)]
            n = norm(cross(u_, v_))
            shade = 0.55 + 0.45 * abs(n[0] * light[0] + n[1] * light[1] + n[2] * light[2])
            iz = (1 / az, 1 / bz, 1 / cz)
            uvs = (uv[ia], uv[ib], uv[ic]) if uv else None
            for py in range(miny, maxy + 1):
                for px in range(minx, maxx + 1):
                    w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area
                    w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area
                    w2 = 1 - w0 - w1
                    if w0 < 0 or w1 < 0 or w2 < 0:
                        continue
                    zi = w0 * iz[0] + w1 * iz[1] + w2 * iz[2]
                    z = 1 / zi
                    k = py * w + px
                    if z >= zbuf[k]:
                        continue
                    if lay and uvs:
                        u = (w0 * uvs[0][0] * iz[0] + w1 * uvs[1][0] * iz[1] + w2 * uvs[2][0] * iz[2]) * z
                        v = (w0 * uvs[0][1] * iz[0] + w1 * uvs[1][1] * iz[1] + w2 * uvs[2][1] * iz[2]) * z
                        tex = lay[0]
                        t = meshview.sample(tex, u + tex["offset"][0], v + tex["offset"][1])
                        if t is None:
                            continue
                        if tex["alpha_test"] and not tex["alpha_test"](t[3], tex["ref"]):
                            continue
                        r, g, bl = t[0], t[1], t[2]
                        if tex.get("blend"):  # alpha blend over what is there, no depth write
                            a_ = t[3] / 255.0
                            o = img[k * 4:k * 4 + 3]
                            img[k * 4:k * 4 + 3] = bytes(int(c * shade * a_ + oc * (1 - a_))
                                                         for c, oc in zip((r, g, bl), o))
                            continue
                    else:
                        r = g = bl = 160
                    fog = min(1.0, max(0.0, (z - far * 0.4) / (far * 0.6)))
                    zbuf[k] = z
                    img[k * 4:k * 4 + 4] = bytes((int((r * shade) * (1 - fog) + sky[0] * fog),
                                                  int((g * shade) * (1 - fog) + sky[1] * fog),
                                                  int((bl * shade) * (1 - fog) + sky[2] * fog), 255))
    return w, h, bytes(img)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    idx = dxm.AssetIndex(DATA)
    key = a[1].lower().replace("/", "\\")
    inst, missing = instances(idx, key)
    if a[0] == "list":
        for mk, m, name in inst:
            print(f"{name:32} {mk}  pos=({m[12]:.1f}, {m[13]:.1f}, {m[14]:.1f})")
        print(f"{len(inst)} instances, {len(missing)} missing meshes: {sorted(set(missing))[:10]}")
    elif a[0] in ("render", "view"):
        from dxt import write_png
        if a[0] == "render":
            import meshview
            yaw = float(a[3]) if len(a) > 3 else 30
            pitch = float(a[4]) if len(a) > 4 else -35
            size = int(a[5]) if len(a) > 5 else 640
            # drop far-away pieces (e.g. a cutscene-only terrain parked 10 km away)
            xs = sorted(m[12] for _, m, _ in inst)
            zs = sorted(m[14] for _, m, _ in inst)
            mx, mz = xs[len(xs) // 2], zs[len(zs) // 2]
            radius = float(os.environ.get("SCENE_RADIUS", "3000"))
            inst = [i for i in inst if abs(i[1][12] - mx) < radius and abs(i[1][14] - mz) < radius]
            batches, positions, layers = build(idx, inst)
            combined = {"lods": [{"batches": batches}], "nodes": [], "materials": []}
            print(f"{len(inst)} instances, {len(batches)} batches")
            write_png(a[2], *meshview.render(combined, size, yaw, pitch, layers, positions))
        else:
            eye = tuple(float(x) for x in a[3:6])
            tgt = tuple(float(x) for x in a[6:9])
            w = int(a[9]) if len(a) > 9 else 640
            h = int(a[10]) if len(a) > 10 else 360
            fov = float(a[11]) if len(a) > 11 else 60
            far = float(os.environ.get("SCENE_FAR", "6000"))
            inst = [i for i in inst if math.dist((i[1][12], i[1][14]), (eye[0], eye[2])) < far + 3000]
            batches, positions, layers = build(idx, inst)
            print(f"{len(inst)} instances, {len(batches)} batches")
            write_png(a[2], *render_perspective(batches, positions, layers, eye, tgt, w, h, fov, far))
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
