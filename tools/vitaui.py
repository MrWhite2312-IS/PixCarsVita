#!/usr/bin/env python3
"""Export the front-end UI (fonts, strings, layers) for the Vita build.

Output in <out_dir>:
  ui.vcs  a VCS1 file holding only textures (font atlases + UI icons), read by scene.c
  ui.txt  line-based description read by vita/src/ui.c (Latin-1):
    tex <i> <name>
    font <name> <tex> <line_h>                       followed by its glyphs:
    g <code> <u0> <v0> <u1> <v1> <w> <h> <xoff> <yoff>
    str <ID> <text>
    layer <name>                                     followed by its components:
    icon <name> <tex> <x> <y> <w> <h> <u0> <u1> <v0> <v1> <r> <g> <b> <a> <hj> <vj>
    text <name> <font> <ID> <x> <y> <scale> <r> <g> <b> <a> <hj> <vj> [<max width> <max lines>]
    bg <duration> <max_circle> <final_circle> <circle_ratio> <header_speed> <title_x1> <x2> <x3>
                                                     FrontEndMenuBackground [General] (+ code defaults)
    signs <x> <y> <font> <ID> <lx> <ly> <rgba> <selected rgba> <anim_s>   CarsUISignMenu + label
    sign <ID> <tex> <desc ID or -> <r> <g> <b> <a>   one of its items, its tint
    camera <name> <px> <py> <pz> <fx> <fy> <fz> <fov>  front-end animated camera (game space;
                                                     fov: horizontal degrees on 4:3, 0 if none)
  Coordinates are the game's 640x480 UI space; hj/vj: 0 left/top, 1 centre, 2 right/bottom.

.cel font: f32 ?, u32 0, u32 n_glyphs, then per glyph (unaligned):
  u32 code; f32 u0, v0, u1, v1; u16 code; u16 0; u16 w; s16 xoff; s16 yoff; u16 h;
  u16 n_kern; n_kern * (u16 code, s16 adjust)
.bin strings: u32 bytes, UTF-16LE with BOM, NUL-separated, in the order of the .loc names
(u32 count, u32 names_size, names NUL-separated) -- see ALIGN for where they drift apart.

Usage: vitaui.py <out_dir>
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxm, dxt, mot
from vitaconv import texture_blob

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
BS = "\\"
LANG = "spa"
FONTS = ["24pt_franklincond_stroke", "22pt_antique_olive_stroke", "22pt_antique_olive_stroke_title"]
LAYERS = ["TitleScreen", "FrontEndMenuBackground", "MenuDescription", "MainMenuSigns"]
# the main menu picks its description by the selected item's name (PS2 0x2828a0); the PC's
# Quit Game has none
SIGN_DESC = {"Story Mode": "STR_STORY_MODE_DESC", "Arcade Mode": "STR_ARCADE_MODE_DESC",
             "Versus": "STR_VS_MODE_DESC", "Options": "STR_OPTIONS_DESC",
             "Bonus Content": "STR_BONUS_CONTENT_DESC"}
EXTRA_TEX = ["highlight_arrow",  # drawn by the sign menu's HighlightArrows effect
             "boostmeter_cap", "boostmeter_gradient", "white"]  # in-game BoostMeterOverlay
# The Spanish .bin files hold a few strings the shipped .loc name lists lack, so names and
# strings drift apart. (first name index, string offset) pairs checked by hand against the
# text; names past the last checked range are not exported.
# shortcut: only the ranges the front end uses are verified; extend when more text is needed.
ALIGN = {"CarsFrontEndUI": [(0, 0, 72), (582, 5, 589)], "BXCarsFrontEndUI": [(0, 0, 46), (66, 3, 67)]}


def strings(idx, table):
    loc = idx.read(f"data{BS}loc{BS}{table.lower()}.loc")
    n, size = struct.unpack_from("<2I", loc)
    names = [x.decode("latin-1") for x in loc[8:8 + size].split(b"\0")[:n]]
    b = idx.read(f"data{BS}loc{BS}l_{table.lower()}_{LANG}.bin")
    total = struct.unpack_from("<I", b)[0]
    texts = b[6:4 + total].decode("utf-16-le").split("\0")
    out = {}
    for first, off, end in ALIGN[table]:
        for i in range(first, end):
            out[names[i]] = texts[i + off]
    return out


def cel(raw):
    n = struct.unpack_from("<I", raw, 8)[0]
    p, glyphs = 12, []
    for _ in range(n):
        code, u0, v0, u1, v1 = struct.unpack_from("<I4f", raw, p)
        _, _, w, xo, yo, h, nk = struct.unpack_from("<HHHhhHH", raw, p + 20)
        p += 34 + 4 * nk
        glyphs.append((code, u0, v0, u1, v1, w, h, xo, yo))
    assert p == len(raw), (p, len(raw))
    return glyphs


def ini(text):
    """[section] -> {key: value} (last wins), plus repeated keys as lists under key+'*'."""
    secs, cur = {}, None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and "]" in line:
            cur = secs.setdefault(line[1:line.index("]")], {})
        elif cur is not None and "=" in line:
            k, v = (x.strip() for x in line.split("=", 1))
            k = k.lower()
            v = v.strip('"')
            cur[k] = v
            cur.setdefault(k + "*", []).append(v)
    return secs


def qrot(q, v):
    x, y, z, w = q
    tx, ty, tz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + y * tz - z * ty, v[1] + w * ty + z * tx - x * tz, v[2] + w * tz + x * ty - y * tx)


def cameras(idx):
    """The front end's AnimatedCameraToys (act/FrontEndUI/FrontEndUI.toy): toy position plus
    the .mot's translation, looking along +Z turned by its rotation. The shipped motions hold
    one pose for their whole loop, so frame 0 is exported."""
    act = f"data{BS}act{BS}frontendui{BS}"
    out = []
    for sec, kv in ini(idx.read(act + "frontendui.toy").decode("latin-1")).items():
        if not sec.startswith("AnimatedCameraToy") or "motionname" not in kv:
            continue
        pos = [float(c) for c in kv["position"].split(",")]
        m = mot.parse(idx.read(act + kv["motionname"].lower() + ".mot"))
        assert all(len({tuple(k[1]) for k in ch["keys"]}) == 1 for ch in m["rotation"] + m["translation"]),             "animated front-end camera: export more than frame 0"
        for ch in m["translation"]:
            pos = [p + d for p, d in zip(pos, mot.sample_channel(ch, 0, "t"))]
        q = mot.sample_channel(m["rotation"][0], 0, "r") if m["rotation"] else (0, 0, 0, 1)
        f = qrot(q, (0, 0, 1))
        # the .mot's scalar track is the horizontal field of view in degrees (4:3 screen):
        # FE_Wel 31.8 frames the billboard as the PC game shows it
        fov = m["scalar"][0]["keys"][0][1] if m["scalar"] else 0
        out.append("camera %s %.3f %.3f %.3f %.5f %.5f %.5f %.3f" % (kv["name"], *pos, *f, fov))
    return out


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    out = a[0]
    os.makedirs(out, exist_ok=True)
    idx = dxm.AssetIndex(DATA)
    ui = f"data{BS}ui{BS}"
    sty = ini(idx.read(ui + "carsfrontendui.sty").decode("latin-1"))
    style = {k.lower(): v for k, v in sty.items()}

    def color(v, default=(255, 255, 255, 255)):
        if not v:
            return default
        if v.lower().startswith("style::"):
            v = style[v[7:].lower()]["color"]
        c = [int(float(x)) for x in v.split(",")]
        return tuple(c + [255] * (4 - len(c)))

    def font(v):
        return style[v[7:].lower()]["font"].lower() if v.lower().startswith("style::") else v.lower()

    texs, tex_ids = [], {}

    def tex(name, raw=None):
        name = name.lower()
        if name not in tex_ids:
            raw = raw or idx.read(f"{ui}tex{BS}{name}.dxt")
            tex_ids[name] = len(texs)
            texs.append((name, raw))
        return tex_ids[name]

    def material_tex(mat):
        key = idx.find(mat, ".dxm", ui + f"tex{BS}x.dxm")
        for p in dxm.parse(idx.read(key).decode("latin-1"))["passes"]:
            st = p.stages.get(1)
            if st and not st["render_target"]:
                return st["texture"].lower()
        return None

    lines = []
    for f in FONTS:
        raw = idx.read(f"data{BS}font{BS}{LANG}{BS}{f}.dxt")
        t = tex(f, raw)
        glyphs = cel(idx.read(f"data{BS}font{BS}{LANG}{BS}{f}.cel"))
        lines.append(f"font {f} {t} {max(g[6] for g in glyphs)}")
        lines += ["g %d %.6f %.6f %.6f %.6f %d %d %d %d" % g for g in glyphs]

    text = {}
    for table in ALIGN:
        text.update(strings(idx, table))
    used = set()

    for layer in LAYERS:
        base = ui + layer.lower()
        lyr = ini(idx.read(base + ".lyr").decode("latin-1"))
        lyt = ini(idx.read(base + ".lyt").decode("latin-1")) if base + ".lyt" in idx.files else {}
        lines.append(f"layer {layer}")
        gen = lyr.get("General", {})
        if layer == "FrontEndMenuBackground":
            # defaults as the PS2 loader (0x27b080) passes them; TitleX* are not in the .lyr
            g = [float(gen.get(k, d)) for k, d in (("backgroundanimduration", 0.5), ("maximumcirclescale", 2.7),
                 ("finalcirclescale", 2.0), ("circlegrowingratio", 0.8), ("headeranimationspeed", 1.0),
                 ("titlex1", 400), ("titlex2", 20), ("titlex3", 55))]
            lines.append("bg " + " ".join(f"{v:g}" for v in g))
        for comp in lyr["Root"].get("component*", []):
            kv = dict(lyr.get(comp, {}))
            kv.update(lyt.get(comp, {}))
            typ = kv.get("type", "")
            cname = comp.replace(" ", "_")
            x, y = float(kv.get("x", 0)), float(kv.get("y", 0))
            hj = {"left": 0, "center": 1, "right": 2}[kv.get("horizontal_justification", "left").lower()]
            vj = {"top": 0, "center": 1, "bottom": 2}[kv.get("vertical_justification", "top").lower()]
            if typ == "UIIconComponent":
                # a component defined twice (the PC "BX_DONE" block re-adds [FooterMiddle] with
                # footer_bottom_middle) keeps its first Material: the PC game shows a plain red footer
                mt = material_tex(lyr[comp].get("material*", [kv["material"]])[0])
                if not mt:
                    continue
                t = tex(mt)
                # Width/Height default to 64, not the texture size; Scale multiplies them (PS2 0x2d0960)
                sc = float(kv.get("scale", 1))
                w = float(kv.get("width", 64)) * sc
                h = float(kv.get("height", 64)) * sc
                u0, u1, v0, v1 = (float(c) for c in kv.get("texcoords", "0,1,0,1").split(","))
                r, g, b, al = color(kv.get("color"))
                lines.append(f"icon {cname} {t} {x:g} {y:g} {w:g} {h:g} {u0:g} {u1:g} {v0:g} {v1:g} {r} {g} {b} {al} {hj} {vj}")
            elif typ in ("CarsUIText", "CarsUIMultiStateText", "CarsUIMultiLineTextComponent",
                         "UIMultiLineTextComponent"):
                r, g, b, al = color(kv.get("color"))
                used.add(kv["textid"])
                wrap = ""
                if typ.endswith("MultiLineTextComponent") and "maxlinewidth" in kv:
                    wrap = f" {float(kv['maxlinewidth']):g} {int(kv.get('maxnumberoflines', 1))}"
                lines.append(f"text {cname} {font(kv['font'])} {kv['textid']} {x:g} {y:g} {float(kv.get('scale', 1)):g} "
                             f"{r} {g} {b} {al} {hj} {vj}{wrap}")
            elif typ == "CarsUISignMenu":
                lab = lyr[kv["component"]]
                r, g, b, al = color(lab.get("color"))
                sr, sg, sb, sa = color(lab.get("selectedcolor"))
                used.add(lab["textid"])
                lines.append(f"signs {x:g} {y:g} {font(lab['font'])} {lab['textid']} {float(lab.get('x', 0)):g} "
                             f"{float(lab.get('y', 0)):g} {r} {g} {b} {al} {sr} {sg} {sb} {sa} "
                             f"{float(kv.get('animationduration', 1)):g}")  # 1 s: PS2 0x247ae8 default
                for item in kv.get("menuitem*", []):
                    it = lyr[item]
                    used.add(it["textid"])
                    desc = SIGN_DESC.get(item, "-")
                    used.add(desc)
                    # sign tint: its BackgroundColor (PS2 0x24a5f4, default 157,128,17,255 at 0x48db50);
                    # an item with a SubMenuItem is drawn grey 100 instead (0x24a990)
                    sc = (100, 100, 100, 255) if "submenuitem" in it else color(it.get("backgroundcolor"), (157, 128, 17, 255))
                    lines.append(f"sign {it['textid']} {tex(it['iconname'])} {desc} " + " ".join(map(str, sc)))
            else:
                print(f"{layer}: skipped {comp} ({typ})")
    for e in EXTRA_TEX:
        tex(e)
    # the in-game boost meter's bar length per boost level (CarsUIBoostMeterContainer)
    bm = ini(idx.read(ui + "boostmeteroverlay.lyr").decode("latin-1"))
    lines.append("boostlen " + " ".join(x.strip() for x in bm["General"]["boostlevellengths"].split(",")))
    # dynamic texts the code fills in, plus the texts the layers name
    used |= {"STR_START", "STR_MAIN_MENU", "STR_SELECT"}
    for k in sorted(used):
        if k in text and not k.startswith("STR_DYN_"):  # placeholders the code fills
            lines.append(f"str {k} {text[k]}")
    lines = [f"tex {i} {n}" for i, (n, _) in enumerate(texs)] + lines + cameras(idx)

    blobs = [texture_blob(raw) for _, raw in texs]
    vcs = b"VCS1" + struct.pack("<4I", len(blobs), 0, 0, 0) + struct.pack("<6f", 0, 0, 0, 0, 0, 1)
    open(os.path.join(out, "ui.vcs"), "wb").write(vcs + b"".join(blobs))
    open(os.path.join(out, "ui.txt"), "w", encoding="latin-1", newline="\n").write("\n".join(lines) + "\n")
    print(f"ui: {len(texs)} textures, {len(FONTS)} fonts, {sum(l.startswith('str ') for l in lines)} strings")


if __name__ == "__main__":
    main()
