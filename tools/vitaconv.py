#!/usr/bin/env python3
"""Convert Cars (2006, PC) assets into the simple runtime formats used by the Vita build.

Formats (little-endian, everything 4-byte aligned, read sequentially by vita/src/vcm.c):

VCM (character model):
  char magic[4] = "VCM1"; u32 n_nodes, n_tex, n_mat, n_batch
  node[n_nodes]:    char name[32]; i32 parent; f32 local[16]; f32 inv_bind_world[16]
  texture[n_tex]:   u32 format (0 RGBA8, 1 DXT1, 2 DXT3, 3 DXT5); u32 w, h, levels
                    level[levels]: u32 size; u8 data[size] (padded to 4)
  material[n_mat]:  u32 n_layers (1 or 2)
                    layer[2]: i32 tex; u32 flags (1 alpha test, 2 clamp U, 4 clamp V);
                              u32 alpha_ref; i32 eye_node (-1 if not a CarsEye layer);
                              f32 affine[6] (bind-space x,y -> u,v of the eye surface);
                              f32 center[2]
  batch[n_batch]:   u32 material, n_verts, n_idx
                    f32 pos[n_verts*3]; f32 nrm[n_verts*3]; f32 uv[n_verts*2]
                    u8 bone[n_verts*4] (node indices); u8 weight[n_verts*4] (sum 255)
                    u16 strip[n_idx] (+pad)   triangle strip with degenerate joins

VCA (animation, resampled to a fixed rate so the runtime only lerps between frames):
  char magic[4] = "VCA1"; u32 n_frames; f32 fps; u32 n_tracks
  track[n_tracks]:  char name[32]; u32 flags (1 rotation, 2 translation)
                    [f32 quat_xyzw[n_frames*4]] [f32 trans[n_frames*3]]
                    the rotation is the node's absolute local rotation

Usage:
  vitaconv.py char <code> <out_dir> [anim_substring ...]
      e.g. vitaconv.py char McQ out/mcq bef_kch bef_won bef_breath beft_idle
      (also copies <code>.phy, the game's physics, read by the driving code, and
      rscamera.ini, the follow camera presets, and fpcam.ini, the first-person camera)
  vitaconv.py model <dxg-in-paks> <out.vcm>
  vitaconv.py anim  <mot-in-paks> <out.vca>
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg, dxm, dxt, mot, pose, meshview

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
FPS = 30.0
TEX_FORMAT = {"DXT1": 1, "DXT3": 2, "DXT5": 3}


def name32(s):
    b = s.encode("latin-1")[:31]
    return b + b"\0" * (32 - len(b))


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


# ------------------------------------------------------------------ textures
def dxt1_to_dxt5(data):
    """Re-encode DXT1 blocks as DXT5 (DXT5 uploads are proven on the Vita, DXT1 crashed).

    Opaque 4-colour blocks are exact. 3-colour blocks (c0 <= c1) keep their transparent
    texels exactly via the alpha block; their midpoint colour becomes the 2/3 blend,
    a barely visible shift.
    """
    out = bytearray()
    for o in range(0, len(data), 8):
        c0, c1, bits = struct.unpack_from("<HHI", data, o)
        three = c0 <= c1
        abits = 0
        cbits = bits
        if three:
            cbits = 0
            for i in range(16):
                ix = (bits >> (2 * i)) & 3
                if ix == 3:
                    abits |= 1 << (3 * i)  # alpha index 1 -> a1 = 0 (transparent)
                    ix = 0
                cbits |= ix << (2 * i)
        # DXT5 alpha block: a0 = 255, a1 = 0, 16 x 3-bit indices (0 -> 255, 1 -> 0)
        out += bytes((255, 0)) + abits.to_bytes(6, "little")
        out += struct.pack("<HHI", c0, c1, cbits)
    return bytes(out)


def texture_blob(raw_dxt):
    """DXT3/DXT5 kept as-is, DXT1 re-encoded as DXT5, paletted -> RGBA8."""
    t = dxt.parse(raw_dxt)
    name = dxt.FORMATS[t["format"]][0]
    if min(t["width"], t["height"]) < 16:
        # vitaGL crashes allocating tiny compressed textures (core dump: memcpy inside
        # gpu_alloc_compressed_texture for an 8x8 DXT5); keep them as RGBA8, top level only
        w, h, rgba = dxt.decode(t)
        out = struct.pack("<4I", 0, w, h, 1) + struct.pack("<I", len(rgba)) + pad4(bytes(rgba))
        return out
    if name == "DXT1":
        levels = [dxt1_to_dxt5(lv[2]) for lv in t["levels"]]
        fmt = TEX_FORMAT["DXT5"]
    elif name in TEX_FORMAT:
        levels = [lv[2] for lv in t["levels"]]
        fmt = TEX_FORMAT[name]
    else:
        w, h, rgba = dxt.decode(t)  # top level only
        levels, fmt = [rgba], 0
    out = struct.pack("<4I", fmt, t["width"], t["height"], len(levels))
    for lv in levels:
        out += struct.pack("<I", len(lv)) + pad4(bytes(lv))
    return out


def fit_affine(batch):
    """Least squares u,v = A*(x,y)+b over the batch's bind-pose vertices."""
    pos, _, uv = dxg.decode_batch(batch)
    rows = [(p[0], p[1], 1.0) for p in pos]

    def solve(target):
        # normal equations 3x3
        ata = [[sum(r[i] * r[j] for r in rows) for j in range(3)] for i in range(3)]
        atb = [sum(r[i] * t for r, t in zip(rows, target)) for i in range(3)]
        m = [ata[i] + [atb[i]] for i in range(3)]
        for c in range(3):
            piv = max(range(c, 3), key=lambda r: abs(m[r][c]))
            m[c], m[piv] = m[piv], m[c]
            for r in range(3):
                if r != c and m[c][c]:
                    f = m[r][c] / m[c][c]
                    m[r] = [a - f * b for a, b in zip(m[r], m[c])]
        return [m[i][3] / m[i][i] if m[i][i] else 0.0 for i in range(3)]

    a = solve([t[0] for t in uv])
    b = solve([t[1] for t in uv])
    return a + b


# ------------------------------------------------------------------ model
def convert_model(idx, key):
    m = dxg.parse(idx.read(key))
    bind = dxg.world_matrices(m)
    lod = m["lods"][0]
    out = [b"VCM1"]
    textures, tex_ids = [], {}
    materials, mat_ids = [], {}

    def tex_index(tex_key):
        if tex_key not in tex_ids:
            tex_ids[tex_key] = len(textures)
            textures.append(texture_blob(idx.read(tex_key)))
        return tex_ids[tex_key]

    def material_index(mat_name, batch):
        if mat_name in mat_ids:
            return mat_ids[mat_name]
        layers = []
        mk = idx.find(mat_name, ".dxm", key)
        if mk:
            passes = dxm.parse(idx.read(mk).decode("latin-1"))["passes"]
            eye = [p for p in passes if any(a.get("algorithm", "").lower() == "carseye" for a in p.animators)]
            use = eye if eye else passes
            for p in use:
                st = p.stages.get(1)
                if not st or st["render_target"]:
                    continue
                tk = idx.find(st["texture"], ".dxt", mk)
                if not tk:
                    continue
                flags = (1 if p.alpha_test else 0) \
                    | (2 if st["address_u"] in ("CLAMP", "BORDER") else 0) \
                    | (4 if st["address_v"] in ("CLAMP", "BORDER") else 0)
                eye_node, affine, center = -1, [0.0] * 6, [0.5, 0.5]
                if eye:
                    an = next(a for a in p.animators if a.get("algorithm", "").lower() == "carseye")
                    found = dxg.find_node(m, "_eye_r" if "right" in an.get("name", "").lower() else "_eye_l")
                    eye_node = -1 if found is None else found
                    affine = fit_affine(batch)
                    center = [float(an.get("ucenter", ".5")), float(an.get("vcenter", ".5"))]
                layers.append(struct.pack("<iIIi6f2f", tex_index(tk), flags, p.alpha_ref, eye_node,
                                          *affine, *center))
                if not eye:
                    break  # base pass only (env-map / specular passes come later)
        n = len(layers)
        while len(layers) < 2:
            layers.append(struct.pack("<iIIi6f2f", -1, 0, 0, -1, *([0.0] * 6), 0.5, 0.5))
        mat_ids[mat_name] = len(materials)
        materials.append(struct.pack("<I", n) + b"".join(layers))
        return mat_ids[mat_name]

    batches = []
    for b in lod["batches"]:
        mi = material_index(m["materials"][b["material"]], b)
        pos, nrm, uv = dxg.decode_batch(b)
        vc = b["vertex_count"]
        nrm = nrm or [(0.0, 1.0, 0.0)] * vc
        uv = uv or [(0.0, 0.0)] * vc
        s = b["streams"]
        if "skin_b0" in s and m["palettes"]:
            pal = m["palettes"][b.get("palette", 0)]
            bi = list(struct.iter_unpack("<4f", s["skin_b0"]))
            bw = list(struct.iter_unpack("<4f", s["skin_b1"]))
        else:
            pal, bi, bw = [0], [(0, 0, 0, 0)] * vc, [(1, 0, 0, 0)] * vc
        bones, weights = bytearray(), bytearray()
        for ii, ww in zip(bi, bw):
            q = [max(0, int(round(w * 255))) for w in ww]
            q[0] += 255 - sum(q)  # exact sum
            bones += bytes(pal[int(j)] if w > 0 else pal[int(ii[0])] for j, w in zip(ii, ww))
            weights += bytes(q)
        blob = struct.pack("<3I", mi, vc, len(b["strip"]))
        blob += struct.pack(f"<{vc * 3}f", *[c for p in pos for c in p])
        blob += struct.pack(f"<{vc * 3}f", *[c for p in nrm for c in p])
        blob += struct.pack(f"<{vc * 2}f", *[c for p in uv for c in p])
        blob += bytes(bones) + bytes(weights)
        blob += pad4(struct.pack(f"<{len(b['strip'])}H", *b["strip"]))
        batches.append(blob)

    nodes = b""
    for n, bw in zip(m["nodes"], bind):
        nodes += name32(n["name"]) + struct.pack("<i", n["parent"])
        nodes += struct.pack("<16f", *n["matrix"]) + struct.pack("<16f", *pose.invert_rigid(bw))
    out.append(struct.pack("<4I", len(m["nodes"]), len(textures), len(materials), len(batches)))
    out += [nodes] + textures + materials + batches
    return b"".join(out)


# ------------------------------------------------------------------ animation
def convert_anim(idx, key, ignore=pose.IGNORE_ROTATION):
    ignore = tuple(ignore)
    a = mot.parse(idx.read(key))
    nf = max(1, int(round(a["duration"] * FPS)) + 1)
    rot = {a["tracks"][c["track"]]: c for c in a["rotation"]}
    tra = {a["tracks"][c["track"]]: c for c in a["translation"]}
    names = [n for n in a["tracks"] if (n in rot and not n.lower().endswith(ignore)) or n in tra]
    out = [b"VCA1", struct.pack("<IfI", nf, FPS, len(names))]
    for n in names:
        has_r = n in rot and not n.lower().endswith(ignore)
        has_t = n in tra
        out.append(name32(n) + struct.pack("<I", (1 if has_r else 0) | (2 if has_t else 0)))
        if has_r:
            qs = []
            prev = None
            for f in range(nf):
                s = mot.sample_channel(rot[n], f / FPS, "r")
                q = list(s)
                if prev and sum(x * y for x, y in zip(q, prev)) < 0:
                    q = [-x for x in q]  # keep hemisphere continuity for runtime lerp
                prev = q
                qs += q
            out.append(struct.pack(f"<{len(qs)}f", *qs))
        if has_t:
            ts = [c for f in range(nf) for c in mot.sample_channel(tra[n], f / FPS, "t")]
            out.append(struct.pack(f"<{len(ts)}f", *ts))
    return b"".join(out)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    idx = dxm.AssetIndex(DATA)
    if a[0] == "model":
        open(a[2], "wb").write(convert_model(idx, a[1].lower().replace("/", "\\")))
    elif a[0] == "anim":
        open(a[2], "wb").write(convert_anim(idx, a[1].lower().replace("/", "\\")))
    elif a[0] == "char":
        code, out_dir, filters = a[1].lower(), a[2], [f.lower() for f in a[3:]]
        os.makedirs(out_dir, exist_ok=True)
        base = f"data\\global\\chars\\{code}\\anim\\"
        model = base + f"{code}_reg.dxg"
        data = convert_model(idx, model)
        open(os.path.join(out_dir, "model.vcm"), "wb").write(data)
        print(f"model.vcm  {len(data) / 1024:.0f} KB")
        names = []
        for k in sorted(idx.files):
            if k.startswith(base) and k.endswith(".mot") and (not filters or any(f in k for f in filters)):
                nm = k.rsplit("\\", 1)[-1][:-4]
                d = convert_anim(idx, k)
                open(os.path.join(out_dir, nm + ".vca"), "wb").write(d)
                names.append(nm)
                print(f"{nm}.vca  {len(d) / 1024:.0f} KB")
        open(os.path.join(out_dir, "anims.txt"), "w").write("\n".join(names) + "\n")
        # the character's blob shadow texture (tex\<code>_shadow, drawn by the game with
        # DXVDiffuse under the car): a one-texture VCS
        sh = base.rsplit("anim\\", 1)[0] + f"tex\\{code}_shadow.dxt"
        if sh in idx.files:
            blob = texture_blob(idx.read(sh))
            vcs = b"VCS1" + struct.pack("<4I", 1, 0, 0, 0) + struct.pack("<6f", 0, 0, 0, 0, 0, 1)
            open(os.path.join(out_dir, "shadow.vcs"), "wb").write(vcs + blob)
            print("shadow.vcs")
        phy = base.rsplit("anim\\", 1)[0] + f"{code}.phy"  # the game's physics for this car
        if phy in idx.files:
            open(os.path.join(out_dir, code + ".phy"), "wb").write(idx.read(phy))
            print(f"{code}.phy")
        # the follow camera's presets ([Preset<n>] + [<Code><n>]), read by vita/src/camera.c
        open(os.path.join(out_dir, "rscamera.ini"), "wb").write(idx.read(f"data{chr(92)}rscamera.ini"))
        print("rscamera.ini")
        # the first-person camera ([1Player] FOV, OffsetFromBodyPoint, LookFilter)
        open(os.path.join(out_dir, "fpcam.ini"), "wb").write(idx.read(f"data{chr(92)}fpcam.ini"))
        print("fpcam.ini")
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
