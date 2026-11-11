#!/usr/bin/env python3
"""Cars (2006, PC) .dxt texture tool. Pure stdlib (no numpy/Pillow needed).

Format (reverse-engineered, little-endian):
  u32 version            always 2
  u32 format             see FORMATS
  u32 palette_count      0, or 256 for paletted formats
  u32 palette[palette_count]   (only if palette_count > 0) BGRA8888
  u32 mip_count
  u32 width, height      size of the top mip
  mip_count * {          stored SMALLEST mip first
      u32 width, height, data_size
      u8  data[data_size]
  }

Usage:
  dxt.py info  <file.dxt>...
  dxt.py png   <file.dxt> <out.png>
  dxt.py scan  <file.pak>...        parse every .dxt inside paks, report problems
  dxt.py export <file.pak> <out_dir> [substring_filter]   decode .dxt in a pak to PNG
"""
import os, struct, sys, zlib

# format code -> (name, bytes per 4x4 block or None, bits per pixel or None)
FORMATS = {
    0x26: ("DXT1", 8, None),
    0x36: ("DXT5", 16, None),
    0x32: ("DXT3", 16, None),
    0x37: ("P8",   None, 8),   # 8-bit paletted, Morton-swizzled
    0x38: ("P8b",  None, 8),   # same layout as P8 (exact difference unknown)
}


class DxtError(Exception):
    pass


def mip_size(fmt, w, h):
    name, block, bpp = FORMATS[fmt]
    if block:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * block
    return w * h * bpp // 8


def parse(buf):
    if len(buf) < 12:
        raise DxtError("too small")
    ver, fmt, npal = struct.unpack_from("<3I", buf, 0)
    if ver != 2:
        raise DxtError(f"version {ver}")
    pos = 12
    palette = None
    if npal:
        palette = [tuple(buf[pos + i*4: pos + i*4 + 4]) for i in range(npal)]  # B,G,R,A
        pos += npal * 4
    mips, w, h = struct.unpack_from("<3I", buf, pos)
    pos += 12
    levels = []
    for _ in range(mips):
        mw, mh, sz = struct.unpack_from("<3I", buf, pos)
        pos += 12
        levels.append((mw, mh, buf[pos:pos + sz]))
        pos += sz
    levels.reverse()  # largest first
    return {"format": fmt, "width": w, "height": h, "palette": palette,
            "levels": levels, "consumed": pos, "total": len(buf)}


# ---------------------------------------------------------------- decoders
def _565(c):
    r = (c >> 11) & 31; g = (c >> 5) & 63; b = c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _color_block(data, o, out, w, h, bx, by, alphas, dxt1):
    c0, c1, bits = struct.unpack_from("<HHI", data, o)
    p0, p1 = _565(c0), _565(c1)
    if c0 > c1 or not dxt1:
        p2 = tuple((2*a + b) // 3 for a, b in zip(p0, p1))
        p3 = tuple((a + 2*b) // 3 for a, b in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
    else:
        p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
        pal = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
    for i in range(16):
        x = bx + (i & 3); y = by + (i >> 2)
        if x < w and y < h:
            r, g, b, a = pal[(bits >> (2*i)) & 3]
            if alphas is not None:
                a = alphas[i]
            k = (y * w + x) * 4
            out[k:k+4] = bytes((r, g, b, a))


def morton(x, y, w, h):
    """Linear index of (x, y) in an Xbox-swizzled w*h (power-of-two) surface."""
    o = b = 0
    bx = by = 1
    while bx < w or by < h:
        if bx < w:
            o |= ((x & bx) != 0) << b; b += 1; bx <<= 1
        if by < h:
            o |= ((y & by) != 0) << b; b += 1; by <<= 1
    return o


def _dxt5_alpha(data, o):
    a0, a1 = data[o], data[o+1]
    bits = int.from_bytes(data[o+2:o+8], "little")
    if a0 > a1:
        tbl = [a0, a1] + [((7-i)*a0 + i*a1) // 7 for i in range(1, 7)]
    else:
        tbl = [a0, a1] + [((5-i)*a0 + i*a1) // 5 for i in range(1, 5)] + [0, 255]
    return [tbl[(bits >> (3*i)) & 7] for i in range(16)]


def _dxt3_alpha(data, o):
    bits = int.from_bytes(data[o:o+8], "little")
    return [((bits >> (4*i)) & 15) * 17 for i in range(16)]


def decode(tex, level=0):
    """Return (w, h, RGBA bytes) of the given mip level."""
    fmt = tex["format"]
    w, h, data = tex["levels"][level]
    out = bytearray(w * h * 4)
    name = FORMATS.get(fmt, ("?",))[0]
    if name in ("DXT1", "DXT3", "DXT5"):
        bw, bh = max(1, (w+3)//4), max(1, (h+3)//4)
        stride = 8 if name == "DXT1" else 16
        o = 0
        for by in range(bh):
            for bx in range(bw):
                if name == "DXT1":
                    _color_block(data, o, out, w, h, bx*4, by*4, None, True)
                else:
                    al = _dxt5_alpha(data, o) if name == "DXT5" else _dxt3_alpha(data, o)
                    _color_block(data, o+8, out, w, h, bx*4, by*4, al, False)
                o += stride
    elif name.startswith("P8"):
        # Paletted textures are stored Xbox-style swizzled (Morton order).
        pal = tex["palette"]
        for y in range(h):
            for x in range(w):
                b, g, r, a = pal[data[morton(x, y, w, h)]]
                k = (y * w + x) * 4
                out[k:k+4] = bytes((r, g, b, a))
    else:
        raise DxtError(f"unknown format 0x{fmt:x}")
    return w, h, bytes(out)


def write_png(path, w, h, rgba):
    raw = b"".join(b"\0" + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def check(tex):
    """Return a list of problems with a parsed texture (empty if consistent)."""
    p = []
    fmt = tex["format"]
    if fmt not in FORMATS:
        return [f"unknown format 0x{fmt:x}"]
    if tex["consumed"] != tex["total"]:
        p.append(f"consumed {tex['consumed']} of {tex['total']} bytes")
    lw, lh = tex["width"], tex["height"]
    for i, (mw, mh, d) in enumerate(tex["levels"]):
        ew, eh = max(1, lw >> i), max(1, lh >> i)
        if (mw, mh) != (ew, eh):
            p.append(f"mip{i} {mw}x{mh} expected {ew}x{eh}")
        if len(d) != mip_size(fmt, mw, mh):
            p.append(f"mip{i} {len(d)} bytes expected {mip_size(fmt, mw, mh)}")
    return p


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    cmd = a[0]
    if cmd == "info":
        for path in a[1:]:
            t = parse(open(path, "rb").read())
            nm = FORMATS.get(t["format"], ("?",))[0]
            print(f"{path}: {nm} {t['width']}x{t['height']} mips={len(t['levels'])} "
                  f"pal={'yes' if t['palette'] else 'no'} problems={check(t) or 'none'}")
    elif cmd == "png":
        t = parse(open(a[1], "rb").read())
        write_png(a[2], *decode(t))
    elif cmd == "export":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from pak import read_index
        flt = a[3].lower() if len(a) > 3 else ""
        n_ok = 0
        with open(a[1], "rb") as f:
            for n, o, s in read_index(a[1]):
                if not n.lower().endswith(".dxt") or flt not in n.lower():
                    continue
                f.seek(o)
                dst = os.path.join(a[2], *n.replace("\\", "/").split("/"))[:-4] + ".png"
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                write_png(dst, *decode(parse(f.read(s))))
                n_ok += 1
        print(f"exported {n_ok} textures")
    elif cmd == "scan":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from pak import read_index
        from collections import Counter
        ok = Counter(); bad = []
        for pk in a[1:]:
            with open(pk, "rb") as f:
                for n, o, s in read_index(pk):
                    if not n.lower().endswith(".dxt"):
                        continue
                    f.seek(o)
                    try:
                        t = parse(f.read(s)); pr = check(t)
                    except Exception as e:  # noqa
                        t, pr = None, [f"parse error: {e}"]
                    if pr:
                        bad.append((n, pr))
                    else:
                        ok[FORMATS[t["format"]][0]] += 1
        print("ok:", dict(ok), "bad:", len(bad))
        for n, pr in bad[:30]:
            print("  ", n, pr)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
