#!/usr/bin/env python3
"""Cars (2006, PC) .mot animation tool. Pure stdlib.

Format (reverse-engineered, little-endian, version -7):
  i32 version = -7  (-3: no unknown_flag, and only the first 4 channel-count fields)
  u32 unknown_flag            0 or 1
  u32 frame_count             at 30 fps
  u32 track_count
  track[track_count]:  u32 len; char name[len] (NUL-terminated) -- skeleton node names
  f32 duration                seconds = (frame_count - 1) / 30
  u32 rot16                   1: rotation keys are s16x4, 0: s8x4
  u32 trans_tangents          1: translation keys carry in/out tangents (Hermite)
  u32 trans_channels, rot_channels, scalar_channels, unused (always 0)
  trans_channel[trans_channels]:
      u32 key_count; u32 track; u32 unknown
      key[key_count]: (f32 time unless first key, time 0)
                      f32 pos[3] [+ f32 in_tangent[3], out_tangent[3]]
  rot_channel[rot_channels]:
      u32 key_count; u32 track; u32 unknown
      key[key_count]: (f32 time unless first key) + quaternion x,y,z,w
                      as s16 (/32767) or s8 (/127)
  scalar_channel[scalar_channels]:     camera field of view in degrees
      u32 key_count; u32 track
      key[key_count]: (f32 time unless first key) + f32 value
  (unknown_flag is 0 exactly in the files that animate a camera)

Usage:
  mot.py info <path-in-paks or file.mot>
  mot.py scan                    parse every .mot in the paks
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class MotError(Exception):
    pass


def parse(buf):
    p = 0

    def take(fmt):
        nonlocal p
        if p + struct.calcsize("<" + fmt) > len(buf):
            raise MotError(f"read past end at {p:#x}")
        v = struct.unpack_from("<" + fmt, buf, p)
        p += struct.calcsize("<" + fmt)
        return v

    ver, = take("i")
    if ver == -7:
        flag, frames, ntracks = take("III")
    elif ver == -3:  # older layout without the flag (5 legacy files, old skeletons)
        flag = 1
        frames, ntracks = take("II")
    else:
        raise MotError(f"unsupported version {ver}")
    tracks = []
    for _ in range(ntracks):
        n, = take("I")
        tracks.append(buf[p:p + n].split(b"\0", 1)[0].decode("latin-1"))
        p += n
    duration, = take("f")
    if ver == -7:
        rot16, tangents, ntrans, nrot, nscalar, n5 = take("6I")
    else:
        rot16, tangents, ntrans, nrot = take("4I")
        nscalar = n5 = 0
    if n5:
        raise MotError(f"unknown channel kind in use: {n5}")

    def channel(value_fmt, scale):
        count, track, unk = take("3I")
        if track >= ntracks:
            raise MotError(f"track {track} out of range at {p:#x}")
        keys = []
        for k in range(count):
            t = take("f")[0] if k else 0.0
            v = take(value_fmt)
            keys.append((t, tuple(x * scale for x in v) if scale != 1 else v))
        return {"track": track, "unknown": unk, "keys": keys}

    tfmt = "9f" if tangents else "3f"
    trans = [channel(tfmt, 1) for _ in range(ntrans)]
    rfmt, rscale = ("4h", 1 / 32767) if rot16 else ("4b", 1 / 127)
    rots = [channel(rfmt, rscale) for _ in range(nrot)]
    scalars = []
    for _ in range(nscalar):  # camera field of view (degrees)
        count, track = take("2I")
        if track >= ntracks:
            raise MotError(f"track {track} out of range at {p:#x}")
        scalars.append({"track": track, "keys": [((take("f")[0] if k else 0.0), take("f")[0])
                                                 for k in range(count)]})
    if p != len(buf):
        raise MotError(f"{len(buf) - p} trailing bytes at {p:#x}")
    return {"flag": flag, "frames": frames, "duration": duration, "tracks": tracks,
            "tangents": bool(tangents), "translation": trans, "rotation": rots,
            "scalar": scalars}


def sample_channel(ch, t, kind):
    """Value of a channel at time t: linear for translation (Hermite if tangents),
    normalized lerp for rotation quaternions (shortest path)."""
    keys = ch["keys"]
    if t <= keys[0][0] or len(keys) == 1:
        v = keys[0][1]
        return v[:3] if kind == "t" else v
    if t >= keys[-1][0]:
        v = keys[-1][1]
        return v[:3] if kind == "t" else v
    lo, hi = 0, len(keys) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if keys[mid][0] <= t:
            lo = mid
        else:
            hi = mid
    (t0, a), (t1, b) = keys[lo], keys[hi]
    f = (t - t0) / (t1 - t0) if t1 > t0 else 0.0
    if kind == "t":
        if len(a) == 9:  # Hermite: p0, out-tangent of a, in-tangent of b, p1
            h00 = 2 * f ** 3 - 3 * f ** 2 + 1; h10 = f ** 3 - 2 * f ** 2 + f
            h01 = -2 * f ** 3 + 3 * f ** 2;    h11 = f ** 3 - f ** 2
            dt = t1 - t0
            return tuple(h00 * a[i] + h10 * a[6 + i] * dt + h01 * b[i] + h11 * b[3 + i] * dt
                         for i in range(3))
        return tuple(a[i] + (b[i] - a[i]) * f for i in range(3))
    if sum(x * y for x, y in zip(a, b)) < 0:
        b = tuple(-x for x in b)
    q = tuple(x + (y - x) * f for x, y in zip(a, b))
    n = sum(x * x for x in q) ** 0.5 or 1
    return tuple(x / n for x in q)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    import dxm
    here = os.path.dirname(os.path.abspath(__file__))
    data = os.environ.get("CARS_DATA", os.path.join(here, "..", "PC data", "Data"))
    if a[0] == "info":
        if os.path.exists(a[1]):
            buf = open(a[1], "rb").read()
        else:
            buf = dxm.AssetIndex(data).read(a[1].lower().replace("/", "\\"))
        m = parse(buf)
        print(f"frames={m['frames']} duration={m['duration']:.3f}s tracks={len(m['tracks'])} "
              f"tangents={m['tangents']} trans_ch={len(m['translation'])} rot_ch={len(m['rotation'])}")
        for kind in ("translation", "rotation"):
            for ch in m[kind]:
                print(f"  {kind[:5]} {m['tracks'][ch['track']]:28} keys={len(ch['keys'])}")
    elif a[0] == "scan":
        idx = dxm.AssetIndex(data)
        ok, bad = 0, []
        for k in sorted(idx.files):
            if k.endswith(".mot"):
                try:
                    parse(idx.read(k)); ok += 1
                except Exception as e:  # noqa
                    bad.append((k, repr(e)))
        print(f"ok={ok} bad={len(bad)}")
        for k, e in bad[:25]:
            print("  ", k, e)
    else:
        print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
