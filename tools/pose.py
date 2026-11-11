#!/usr/bin/env python3
"""Evaluate a .mot animation on a .dxg skeleton and skin the mesh (CPU, pure Python).

Used by meshview.py to render animated poses. Conventions (see mot.py / dxg.py):
  - node matrices are row-major, v' = v * M, translation in row 3
  - animation translation keys are the node's absolute local translation
  - animation rotation keys are the node's local rotation; QUAT_ORDER says which
    stored component is x, y, z, w
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg, mot

# stored component index for (x, y, z, w): the file stores (x, w, y, z).
# Verified: rest keys decode to identity, wheels spin about X (axle), the Driver
# heading in cutscenes matches its direction of travel.
QUAT_ORDER = (0, 2, 3, 1)

# Rotation tracks ignored when posing. OPEN QUESTION: <Char>_Pelvis rotation keys
# do not decode consistently with any component order (calm poses alternate between
# values 180 degrees apart). Every pose looks right when they are ignored; in game the
# pelvis is probably driven by the suspension. Needs confirming in Cars.exe.
IGNORE_ROTATION = ("_pelvis",)


def quat_to_mat(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    # row-vector convention (D3D): rows are the images of the basis vectors
    return (1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0.0,
            2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0.0,
            2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0.0,
            0.0, 0.0, 0.0, 1.0)


def invert_rigid(m):
    """Inverse of a rotation+translation (row-major, row-vector) matrix."""
    r = [[m[i * 4 + j] for j in range(3)] for i in range(3)]
    t = m[12:15]
    rt = [[r[j][i] for j in range(3)] for i in range(3)]  # transpose
    nt = [-sum(t[k] * rt[k][j] for k in range(3)) for j in range(3)]
    return (rt[0][0], rt[0][1], rt[0][2], 0.0,
            rt[1][0], rt[1][1], rt[1][2], 0.0,
            rt[2][0], rt[2][1], rt[2][2], 0.0,
            nt[0], nt[1], nt[2], 1.0)


def pose_world(mesh, anim, t, order=QUAT_ORDER, ignore=IGNORE_ROTATION):
    """World matrices of every node at time t (nodes without tracks keep bind local)."""
    by_name = {n["name"].lower(): i for i, n in enumerate(mesh["nodes"])}
    local = [list(n["matrix"]) for n in mesh["nodes"]]
    for ch in anim["rotation"]:
        name = anim["tracks"][ch["track"]].lower()
        i = by_name.get(name)
        if i is None or any(name.endswith(sfx) for sfx in ignore):
            continue
        s = mot.sample_channel(ch, t, "r")
        r = quat_to_mat(tuple(s[k] for k in order))
        local[i][0:3], local[i][4:7], local[i][8:11] = r[0:3], r[4:7], r[8:11]
    for ch in anim["translation"]:
        i = by_name.get(anim["tracks"][ch["track"]].lower())
        if i is not None:
            local[i][12:15] = mot.sample_channel(ch, t, "t")
    world = [None] * len(local)

    def get(i):
        if world[i] is None:
            p = mesh["nodes"][i]["parent"]
            world[i] = tuple(local[i]) if p < 0 or p == i else dxg.mat_mul(tuple(local[i]), get(p))
        return world[i]

    for i in range(len(local)):
        get(i)
    return world


def skin_matrices(mesh, world):
    """Per-node skinning matrix: inverse(bind world) * posed world."""
    bind = dxg.world_matrices(mesh)
    return [dxg.mat_mul(invert_rigid(b), w) for b, w in zip(bind, world)]


def skin_batch(mesh, batch, skin):
    """Posed positions of a batch (bind positions if the batch has no skin data)."""
    pos, _, _ = dxg.decode_batch(batch)
    s = batch["streams"]
    if "skin_b0" not in s or not mesh["palettes"]:
        return pos
    pal = mesh["palettes"][batch.get("palette", 0)]
    idx = list(struct.iter_unpack("<4f", s["skin_b0"]))
    wts = list(struct.iter_unpack("<4f", s["skin_b1"]))
    out = []
    for p, bi, bw in zip(pos, idx, wts):
        x = y = z = 0.0
        for j, w in zip(bi, bw):
            if w <= 0:
                continue
            m = skin[pal[int(j)]]
            x += w * (p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12])
            y += w * (p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13])
            z += w * (p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14])
        out.append((x, y, z))
    return out
