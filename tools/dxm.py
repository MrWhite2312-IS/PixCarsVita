#!/usr/bin/env python3
"""Cars (2006, PC) .dxm material tool. Pure stdlib.

A .dxm is an INI file describing a multi-pass Direct3D 9 material:
  [General]            TotalShaderPasses=N
  [ShaderPass_K]       one per pass (K = 1..N):
      VertexShaderName / PixelShaderName   (.xvu / .xpu in misc.pak)
      TextureMap_S=<name>                  texture stage S (1-based); '&Name' = render target
      TextureCoordinateIndex_S, MipMap_S, MAGFILTER_S, MINFILTER_S, MIPFILTER_S,
      ADDRESSU_S, ADDRESSV_S, MIPMAPLODBIAS_S ...
      D3D render states: ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP, ALPHATESTENABLE,
      ALPHAREF, ALPHAFUNC, ZWRITEENABLE, ZFUNC, CULLMODE, BLENDCOLOR, ...
      ShaderColorScale=r,g,b,a (0..1)  /  ShaderRGBA=r,g,b,a (0..255)
      MaterialPassStart / MaterialPassEnd  camera distance range in which the pass draws
      dependentMaterial_S=<path>           material that must be set up first (env map, HDR)
  [ShaderPass_K_Animator_J]  UV scrolling, eye animation, crowd, texture flipbook...
  [Prologue] / [Epilogue] / [FilterSection] ...  render-target setup (shadows, HDR)

Keys are matched case-insensitively and booleans accept TRUE/true/t/1 etc.

Usage:
  dxm.py show <name-or-path-inside-paks>
  dxm.py scan <file.pak>...        parse every .dxm, report problems and missing textures
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BOOL_TRUE = {"true", "t", "1", "yes"}
BOOL_FALSE = {"false", "f", "0", "no", ""}


def parse_ini(text):
    """Return list of (section, {lowercase_key: (OriginalKey, value)}) in file order."""
    sections, cur = [], None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("//"):
            continue
        m = re.match(r"\[(.*)\]$", line)
        if m:
            cur = (m.group(1).strip(), {})
            sections.append(cur)
            continue
        if "=" in line and cur is not None:
            k, v = line.split("=", 1)
            cur[1][k.strip().lower()] = (k.strip(), v.strip())
    return sections


def as_bool(v):
    s = v.strip().lower()
    if s in BOOL_TRUE:
        return True
    if s in BOOL_FALSE:
        return False
    raise ValueError(f"not a boolean: {v!r}")


def as_floats(v):
    return tuple(float(x.rstrip("f")) for x in re.split(r"[,\s]+", v.strip()) if x)


def d3d_enum(v, prefix):
    """'D3DBLEND_SRCALPHA' -> 'SRCALPHA'; tolerant of case and '|' combinations."""
    parts = [p.strip().upper() for p in v.split("|") if p.strip()]
    out = []
    for p in parts:
        for pre in (prefix, "D3D"):
            if p.startswith(pre):
                p = p[len(pre):]
                break
        out.append(p)
    return "|".join(out) if out else None


class Pass:
    def __init__(self, kv):
        g = lambda k, d=None: kv.get(k.lower(), (None, d))[1]
        self.vertex_shader = g("VertexShaderName")
        self.pixel_shader = g("PixelShaderName")
        # texture stages
        self.stages = {}
        for k, (orig, v) in kv.items():
            m = re.match(r"texturemap_(\d+)$", k)
            if m:
                s = int(m.group(1))
                self.stages[s] = {
                    "texture": v,
                    "render_target": v.startswith("&"),
                    "uv_set": int(g(f"TextureCoordinateIndex_{s}", "0") or 0),
                    "mipmap": g(f"MipMap_{s}"),
                    "mag": d3d_enum(g(f"MAGFILTER_{s}", ""), "D3DTEXF_"),
                    "min": d3d_enum(g(f"MINFILTER_{s}", ""), "D3DTEXF_"),
                    "mip": d3d_enum(g(f"MIPFILTER_{s}", ""), "D3DTEXF_"),
                    "address_u": d3d_enum(g(f"ADDRESSU_{s}", ""), "D3DTADDRESS_"),
                    "address_v": d3d_enum(g(f"ADDRESSV_{s}", ""), "D3DTADDRESS_"),
                }
        b = lambda k, d=None: as_bool(g(k)) if g(k) is not None else d
        self.alpha_blend = b("AlphaBlendEnable", False)
        self.src_blend = d3d_enum(g("SrcBlend", ""), "D3DBLEND_") or "ONE"
        self.dst_blend = d3d_enum(g("DestBlend", ""), "D3DBLEND_") or "ZERO"
        self.blend_op = d3d_enum(g("BlendOp", ""), "D3DBLENDOP_") or "ADD"
        self.alpha_test = b("AlphaTestEnable", False)
        self.alpha_ref = int(g("AlphaRef", "0") or 0)
        self.alpha_func = d3d_enum(g("AlphaFunc", ""), "D3DCMP_") or "GREATER"
        self.z_write = b("ZWriteEnable", True)
        self.z_func = d3d_enum(g("ZFunc", ""), "D3DCMP_") or "LESSEQUAL"
        self.cull = d3d_enum(g("CullMode", ""), "D3DCULL_") or "CCW"
        self.fog = b("FogEnable", True)
        cw = g("ColorWriteEnable") or g("D3DCOLORWRITEENABLE")
        self.color_write = d3d_enum(cw, "D3DCOLORWRITEENABLE_") if cw else "ALL"
        bc = g("BlendColor")
        self.blend_color = int(bc, 16) if bc else None
        if g("ShaderColorScale"):
            self.color = as_floats(g("ShaderColorScale"))
        elif g("ShaderRGBA"):
            self.color = tuple(x / 255.0 for x in as_floats(g("ShaderRGBA")))
        else:
            self.color = (1.0, 1.0, 1.0, 1.0)
        start, end = g("MaterialPassStart"), g("MaterialPassEnd")
        self.distance = (float(start) if start else 0.0, float(end) if end else float("inf"))
        self.dependent = [v for k, (_, v) in kv.items() if k.startswith("dependentmaterial_")]
        self.animators = []

    def summary(self):
        tex = ", ".join(f"{s}:{d['texture']}(uv{d['uv_set']})" for s, d in sorted(self.stages.items()))
        blend = f"blend {self.src_blend}/{self.dst_blend}" if self.alpha_blend else "opaque"
        at = f" alphatest {self.alpha_func} {self.alpha_ref}" if self.alpha_test else ""
        anim = f" anim={[a.get('algorithm') for a in self.animators]}" if self.animators else ""
        return (f"vs={self.vertex_shader} ps={self.pixel_shader} tex=[{tex}] {blend}{at} "
                f"zwrite={self.z_write} cull={self.cull} dist={self.distance} color={self.color}{anim}")


def parse(text):
    secs = parse_ini(text)
    by_name = {s.lower(): kv for s, kv in secs}
    n = int(by_name.get("general", {}).get("totalshaderpasses", (None, "0"))[1] or 0)
    passes = []
    for k in range(1, n + 1):
        kv = by_name.get(f"shaderpass_{k}")
        if kv is None:
            if not any(sn.startswith("shaderpass_") for sn in by_name):
                break  # filter-only material (HDR etc.): no regular passes
            raise ValueError(f"missing [ShaderPass_{k}]")
        p = Pass(kv)
        for s, akv in secs:
            m = re.match(rf"shaderpass_{k}_animator_(\d+)$", s.lower())
            if m:
                p.animators.append({ok.lower(): v for ok, v in akv.values()})
        passes.append(p)
    extra = [s for s, _ in secs if not re.match(r"(general|shaderpass_\d+(_animator_\d+)?)$", s.lower())]
    return {"passes": passes, "extra_sections": extra}


# ------------------------------------------------------------- asset lookup
class AssetIndex:
    """Case-insensitive lookup of files across all .pak archives."""

    def __init__(self, data_dir):
        from pak import read_index
        self.files = {}      # full lowercase path -> (pak, offset, size, original)
        self.by_base = {}    # lowercase basename -> [full lowercase paths]
        for name in sorted(os.listdir(data_dir)):
            if not name.lower().endswith(".pak"):
                continue
            pk = os.path.join(data_dir, name)
            for n, o, s in read_index(pk):
                key = n.lower().replace("/", "\\")
                self.files[key] = (pk, o, s, n)
                self.by_base.setdefault(key.rsplit("\\", 1)[-1], []).append(key)

    def read(self, key):
        pk, o, s, _ = self.files[key]
        with open(pk, "rb") as f:
            f.seek(o)
            return f.read(s)

    def find(self, name, ext, near=None):
        """Find '<name><ext>', preferring the directory of `near`, then its parents."""
        base = (name.replace("/", "\\").rsplit("\\", 1)[-1] + ext).lower()
        cands = self.by_base.get(base, [])
        if not cands:
            return None
        if near:
            d = near.lower().rsplit("\\", 1)[0]
            while d:
                for c in cands:
                    if c.rsplit("\\", 1)[0] == d:
                        return c
                # try sibling 'tex' folders while walking up (e.g. Chars\X\Anim -> Chars\X\Tex)
                for c in cands:
                    if c.startswith(d + "\\"):
                        return c
                d = d.rsplit("\\", 1)[0] if "\\" in d else ""
        return cands[0]

    def find_texture(self, name, near=None):
        """Resolve a TextureMap name: .dxt first, then animated .atx; tolerate stray spaces."""
        for n in (name, name.replace(" ", "")):
            for ext in (".dxt", ".atx"):
                hit = self.find(n, ext, near)
                if hit:
                    return hit
        return None


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    here = os.path.dirname(os.path.abspath(__file__))
    data = os.environ.get("CARS_DATA", os.path.join(here, "..", "PC data", "Data"))
    if a[0] == "show":
        idx = AssetIndex(data)
        key = a[1].lower().replace("/", "\\")
        if key not in idx.files:
            key = idx.find(a[1].rsplit(".", 1)[0], ".dxm")
        m = parse(idx.read(key).decode("latin-1"))
        print(key)
        for i, p in enumerate(m["passes"], 1):
            print(f"  pass {i}: {p.summary()}")
            for s, st in sorted(p.stages.items()):
                if not st["render_target"]:
                    print(f"      stage {s} -> {idx.find_texture(st['texture'], key) or 'MISSING'}")
        if m["extra_sections"]:
            print("  extra:", m["extra_sections"])
    elif a[0] == "scan":
        idx = AssetIndex(data)
        ok, bad, missing = 0, [], {}
        for key in sorted(idx.files):
            if not key.endswith(".dxm"):
                continue
            try:
                m = parse(idx.read(key).decode("latin-1"))
                ok += 1
                for p in m["passes"]:
                    for st in p.stages.values():
                        if not st["render_target"] and not idx.find_texture(st["texture"], key):
                            missing.setdefault(st["texture"].lower(), key)
            except Exception as e:  # noqa
                bad.append((key, repr(e)))
        print(f"ok={ok} bad={len(bad)} missing_textures={len(missing)}")
        for k, e in bad[:20]:
            print("  BAD", k, e)
        for t, k in list(missing.items())[:20]:
            print("  MISSING", t, "used by", k)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
