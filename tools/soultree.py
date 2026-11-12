#!/usr/bin/env python3
"""Cars (2006) streamed "Soultree" terrain: <scene>.xbox.res + <scene>.xbox.str.

The .res bundle (see scnres.py) holds:
  <scene>.xbox.toc  records of [u32 a][u32 b][char name[256]][fields]:
      sector (356 B): 25 u32 after the name; [1..3] = n_objects, n_geo, n_col,
                      [15..20] = bbox (xmin, xmax, ymin, ymax, zmin, zmax),
                      [21] = byte offset of the sector's data in the .str
      object (280 B): after the name: junk, -1, index, offset, size, type (0 geo, 2 col)
                      offset is relative to the sector's data
  <scene>.xbox.soi  texture pack followed by one "dgs" header per object: a complete
                    .dxg v5 header (node with world bbox, materials, batches with scales,
                    a position offset and counts) without the index/vertex data.
The .str holds each object's data: per batch, the u16 strip (padded to 4 bytes) and the
vertex streams, exactly as in a .dxg.

Usage:
  soultree.py check <scene.xbox.res> <scene.xbox.str>
  soultree.py view  <scene.xbox.res> <scene.xbox.str> <out.png> ex ey ez tx ty tz [w h fov]
"""
import os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxg, scnres

BS = chr(92)
NAME_RE = re.compile(rb"(Sector_\d+|Scenes\x5c[^\x00]+|C_[A-Za-z0-9_]+)\x00")


def load(res_path, str_path):
    entries, blob = scnres.load(res_path)
    files = {n.lower(): blob[o:o + s] for n, o, s in entries}
    toc = next(v for k, v in files.items() if k.endswith(".toc"))
    soi = next(v for k, v in files.items() if k.endswith(".soi"))
    stream = str_path if isinstance(str_path, (bytes, bytearray)) else open(str_path, "rb").read()

    # dgs headers by node name
    headers = {}
    i = 0
    while (i := soi.find(b"dgs\x00\x05\x00\x00\x00", i)) >= 0:
        name = soi[i + 12:i + 140].split(b"\0", 1)[0].decode("latin-1").lower()
        headers.setdefault(name, []).append(i)
        i += 4

    sectors, cur = [], None
    for m in NAME_RE.finditer(toc):
        name = m.group(1).decode("latin-1")
        st = m.start()
        if name.startswith("Sector_"):
            f = struct.unpack_from("<25I", toc, st + 256)
            ff = struct.unpack_from("<25f", toc, st + 256)
            cur = {"name": name, "bbox": ff[15:21], "offset": f[21], "objects": []}
            sectors.append(cur)
        elif cur is not None:
            _, _, index, off, size, typ = struct.unpack_from("<6I", toc, st + 256)
            cur["objects"].append({"name": name, "index": index, "offset": off,
                                   "size": size, "type": typ})
    # materials shipped inside the bundle, and textures embedded in the .soi
    # (records whose embedded .dxt header, at +276, has version 2)
    assets = {"dxm": {k.rsplit(BS, 1)[-1][:-4]: v for k, v in files.items() if k.endswith(".dxm")},
              "dxt": {}}
    tag = b"Scenes" + BS.encode()
    r = 0
    while (r := soi.find(tag, r)) >= 0:
        rec = r - 8
        if rec >= 0 and soi[rec + 276:rec + 280] == b"\x02\x00\x00\x00":
            name = soi[r:r + 256].split(b"\0", 1)[0].decode("latin-1").rsplit(BS, 1)[-1].lower()
            assets["dxt"].setdefault(name, soi[rec + 276:])
        r += 1
    return sectors, headers, soi, stream, assets


def objects(sectors, headers, soi, stream):
    """Yield (sector, object, mesh) for every geometry object that decodes."""
    used = {}
    for sec in sectors:
        for ob in sec["objects"]:
            if ob["type"] != 0:
                continue
            base = ob["name"].replace("\\", "/").rsplit("/", 1)[-1].lower()
            cands = headers.get(base, [])
            k = used.get(base, 0)
            if k >= len(cands):
                yield sec, ob, None
                continue
            used[base] = k + 1
            data = stream[sec["offset"] + ob["offset"]:sec["offset"] + ob["offset"] + ob["size"]]
            try:
                yield sec, ob, dxg.parse(soi[cands[k]:], data)
            except (dxg.DxgError, struct.error) as e:
                yield sec, ob, e


def material_layers(idx, assets, mat, near):
    """Base texture layer of a material: bundle .dxm first, then the paks; textures from the
    paks, then the .soi. shortcut: streamed (.str) textures are not located yet, so those
    materials come out untextured; upgrade once their offsets inside sectors are known."""
    import dxm, meshview
    data = assets["dxm"].get(mat.lower())
    if data is None:
        mk = idx.find(mat, ".dxm", near)
        if not mk:
            return []
        data = idx.read(mk)
    for p in dxm.parse(data.decode("latin-1"))["passes"]:
        st = p.stages.get(1)
        if st and not st["render_target"]:
            tex = meshview._tex_from_stage(idx, st, p, near, assets["dxt"])
            return [tex] if tex else []
    return []


def build(idx, items, assets, near=BS.join(["data", "scenes", "rs", "tex", "x.dxm"])):
    """World-space batches (batches, positions, layers) like scene.build, for streamed objects."""
    import scene
    batches, positions, layers, cache = [], [], [], {}
    for _, _, m in items:
        if not m or isinstance(m, Exception):
            continue
        w = dxg.world_matrices(m)[0]
        for b in m["lods"][0]["batches"]:
            pos, _, _ = dxg.decode_batch(b)
            positions.append([scene.transform(p, w) for p in pos])
            batches.append(b)
            mat = m["materials"][b["material"]]
            if mat not in cache:
                cache[mat] = material_layers(idx, assets, mat, near)
            layers.append(cache[mat])
    return batches, positions, layers


def main():
    a = sys.argv[1:]
    if len(a) < 3 or a[0] not in ("check", "view"):
        print(__doc__); sys.exit(1)
    sectors, headers, soi, stream, assets = load(a[1], a[2])
    if a[0] == "view":
        import dxm, scene
        from dxt import write_png
        idx = dxm.AssetIndex(scene.DATA)
        eye = tuple(float(x) for x in a[4:7])
        tgt = tuple(float(x) for x in a[7:10])
        w = int(a[10]) if len(a) > 10 else 640
        h = int(a[11]) if len(a) > 11 else 360
        fov = float(a[12]) if len(a) > 12 else 60
        far = float(os.environ.get("SCENE_FAR", "4000"))
        items = [it for it in objects(sectors, headers, soi, stream)
                 if it[2] and not isinstance(it[2], Exception)]
        batches, positions, layers = build(idx, items, assets)
        print(f"{len(items)} objects, {len(batches)} batches")
        write_png(a[3], *scene.render_perspective(batches, positions, layers, eye, tgt, w, h, fov, far))
        return
    ok = bad = nohdr = outside = 0
    errors = {}
    for sec, ob, m in objects(sectors, headers, soi, stream):
        if isinstance(m, Exception):
            errors.setdefault(str(m)[:60], []).append(ob["name"])
            continue
        if m is None:
            nohdr += 1
            continue
        if abs(m["data_used"] - ob["size"]) > 3:
            bad += 1
            if bad <= 5:
                print("size mismatch", ob["name"], m["data_used"], ob["size"])
            continue
        n = m["nodes"][0]
        c, e = n["center"], n["extent"]
        for b in m["lods"][0]["batches"]:
            pos, _, _ = dxg.decode_batch(b)
            if any(abs(p[i] - c[i]) > e[i] * 1.02 + 1.0 for p in pos for i in range(3)):
                outside += 1
                break
        ok += 1
    print(f"sectors={len(sectors)} geo objects ok={ok} size_mismatch={bad} no_header={nohdr} "
          f"objects with vertices outside their bbox={outside} errors={sum(map(len, errors.values()))}")
    for e, names in errors.items():
        print(f"  {len(names):4d} x {e}   e.g. {names[0]}")


if __name__ == "__main__":
    main()
