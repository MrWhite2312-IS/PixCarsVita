#!/usr/bin/env python3
"""Export a static Cars (2006) scene (.scn / .toy placements) for the Vita build.

VCS1 format (little-endian, 4-byte aligned, read by vita/src/scene.c):
  char magic[4] = "VCS1"; u32 n_tex, n_mat, n_mesh, n_inst
  f32 start[3]; f32 start_dir[3]            suggested camera start
  texture[n_tex]   same layout as VCM textures (see vitaconv.py)
  material[n_mat]  same layout as VCM materials (only layer 0 used)
  mesh[n_mesh]:    f32 center[3], radius     (local bounding sphere)
                   u32 n_batch
                   batch[n_batch]: u32 material, n_verts, n_idx
                       f32 pos[n*3]; f32 uv[n*2]; u8 rgba[n*4]; u16 strip[n_idx] (+pad)
  instance[n_inst]: u32 mesh; f32 world[16] (row-major, v' = v*M); f32 center[3], radius

Lighting is baked into the vertex colours: prelit meshes keep their authored colours,
the rest get a fixed sun (Lambert + ambient) computed here on the PC.

Usage:
  vitascene.py <toy-or-scn in paks> <out.vcs> [x z radius] [start=x,y,z] [dir=x,y,z]
      optional x z radius: keep only instances within radius of (x, z)
      start/dir: initial camera position and direction
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg, dxm, scene
from vitaconv import texture_blob, pad4

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
SUN = scene.norm((0.45, 0.80, 0.40))


def export(idx, key, keep=None, start_override=None, dir_override=None):
    inst, missing = scene.instances(idx, key)
    if keep:
        x, z, r = keep
        inst = [i for i in inst if math.dist((i[1][12], i[1][14]), (x, z)) < r]
    textures, tex_ids, materials, mat_ids = [], {}, [], {}
    meshes, mesh_ids, instances = [], {}, []

    def tex_index(tk):
        if tk not in tex_ids:
            tex_ids[tk] = len(textures)
            textures.append(texture_blob(idx.read(tk)))
        return tex_ids[tk]

    def material_index(mat, near):
        k = (mat.lower(), near)
        if k in mat_ids:
            return mat_ids[k]
        layer = struct.pack("<iIIi6f2f", -1, 0, 0, -1, *([0.0] * 6), 0.5, 0.5)
        n = 0
        mk = idx.find(mat, ".dxm", near)
        if mk:
            for p in dxm.parse(idx.read(mk).decode("latin-1"))["passes"]:
                st = p.stages.get(1)
                if not st or st["render_target"]:
                    continue
                tk = idx.find(st["texture"], ".dxt", mk)
                if not tk:
                    continue
                flags = (1 if p.alpha_test else 0) \
                    | (2 if st["address_u"] in ("CLAMP", "BORDER") else 0) \
                    | (4 if st["address_v"] in ("CLAMP", "BORDER") else 0)
                layer = struct.pack("<iIIi6f2f", tex_index(tk), flags, p.alpha_ref, -1,
                                    *([0.0] * 6), 0.5, 0.5)
                n = 1
                break
        empty = struct.pack("<iIIi6f2f", -1, 0, 0, -1, *([0.0] * 6), 0.5, 0.5)
        mat_ids[k] = len(materials)
        materials.append(struct.pack("<I", n) + layer + empty)
        return mat_ids[k]

    def mesh_index(mk):
        if mk in mesh_ids:
            return mesh_ids[mk]
        m = dxg.parse(idx.read(mk))
        world = dxg.world_matrices(m)
        nm = world[0] if m["nodes"] else (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        blobs, pts = [], []
        for b in m["lods"][0]["batches"]:
            pos, nrm, uv = dxg.decode_batch(b)
            vc = b["vertex_count"]
            pos = [scene.transform(p, nm) for p in pos]
            pts += pos
            uv = uv or [(0.0, 0.0)] * vc
            col = b["streams"].get("color")
            rgba = bytearray()
            for i in range(vc):
                if col:
                    bb, gg, rr, aa = col[i * 4:i * 4 + 4]
                    rgba += bytes((rr, gg, bb, aa))
                else:
                    n = nrm[i] if nrm else (0.0, 1.0, 0.0)
                    ln = math.sqrt(sum(c * c for c in n)) or 1
                    d = (n[0] * SUN[0] + n[1] * SUN[1] + n[2] * SUN[2]) / ln
                    s = int(255 * min(1.0, 0.5 + 0.6 * max(0.0, d)))
                    rgba += bytes((s, s, s, 255))
            mi = material_index(m["materials"][b["material"]], mk)
            blob = struct.pack("<3I", mi, vc, len(b["strip"]))
            blob += struct.pack(f"<{vc * 3}f", *[c for p in pos for c in p])
            blob += struct.pack(f"<{vc * 2}f", *[c for t in uv for c in t])
            blob += bytes(rgba)
            blob += pad4(struct.pack(f"<{len(b['strip'])}H", *b["strip"]))
            blobs.append(blob)
        if pts:
            c = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
            r = max(math.dist(c, p) for p in pts)
        else:
            c, r = (0.0, 0.0, 0.0), 0.0
        mesh_ids[mk] = len(meshes)
        meshes.append((c, r, struct.pack("<4fI", *c, r, len(blobs)) + b"".join(blobs)))
        return mesh_ids[mk]

    for mk, m, name in inst:
        mi = mesh_index(mk)
        c, r, _ = meshes[mi]
        wc = scene.transform(c, m)
        instances.append(struct.pack("<I16f4f", mi, *m, *wc, r))

    # start: scene default if sensible, else the first instance
    secs = dict((s.lower(), kv) for s, kv in dxm.parse_ini(idx.read(key).decode("latin-1")))
    start = None
    for s in ("sceneinfo",):
        kv = secs.get(s, {})
        if "defaultstartposition" in kv:
            start = scene.vec(kv["defaultstartposition"][1])
    xs = sorted(i[1][12] for i in inst)
    zs = sorted(i[1][14] for i in inst)
    ys = sorted(i[1][13] for i in inst)
    if not start or min(math.dist((start[0], start[2]), (i[1][12], i[1][14])) for i in inst) > 2000:
        start = (xs[len(xs) // 2], ys[len(ys) // 2] + 60, zs[len(zs) // 2])
    if start_override:
        start = start_override
    sdir = scene.norm(dir_override) if dir_override else (1.0, 0.0, 0.0)
    out = b"VCS1" + struct.pack("<4I", len(textures), len(materials), len(meshes), len(instances))
    out += struct.pack("<6f", *start, *sdir)
    out += b"".join(textures) + b"".join(materials) + b"".join(m[2] for m in meshes)
    out += b"".join(instances)
    return out, len(textures), len(materials), len(meshes), len(instances), missing


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); sys.exit(1)
    idx = dxm.AssetIndex(DATA)
    opts = dict(x.split("=", 1) for x in a[2:] if "=" in x)
    pos = [x for x in a[2:] if "=" not in x]
    keep = tuple(float(x) for x in pos[:3]) if len(pos) >= 3 else None
    st = tuple(float(v) for v in opts["start"].split(",")) if "start" in opts else None
    di = tuple(float(v) for v in opts["dir"].split(",")) if "dir" in opts else None
    data, nt, nm, nme, ni, missing = export(idx, a[0].lower().replace("/", "\\"), keep, st, di)
    os.makedirs(os.path.dirname(os.path.abspath(a[1])), exist_ok=True)
    open(a[1], "wb").write(data)
    print(f"{a[1]}: {len(data) / 1e6:.1f} MB  textures={nt} materials={nm} meshes={nme} "
          f"instances={ni} missing={len(set(missing))}")


if __name__ == "__main__":
    main()
