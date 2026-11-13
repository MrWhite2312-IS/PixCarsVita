#!/usr/bin/env python3
"""Export an in-engine cutscene activity (data/act/<scene>/<name>.aty + .as + .toy) for the Vita.

  vitacutscene.py <activity> <out_dir>        e.g. vitacutscene.py ex_rs/cs_intro extracted/vita/act/cs_intro

Writes to out_dir:
  cutscene.txt   the timeline, one item per line, times in seconds from the start:
                   duration <s>
                   scene <scene code>                 (the .aty's Scene, lower case)
                   next <activity> <start>            (NextActivity, NextStart)
                   voice <file>                       (SoundTrackFileName, Spanish, stereo ADPCM .wav)
                   toy <name> <char> px py pz  fx fy fz  ux uy uz   (CarsAnimatedToy: position, look, up)
                   anim <t> <toy> <vca>               (AnimatedToy action: motion from t)
                   cam <t> <track>                    (AnimatedCamera action: cam_<track>.txt from t)
                   caption <t0> <t1> <text>           (Caption: Spanish text, latin-1)
                   fade in <t> <dur> | fade out <t> <dur> <hold>   (VideoFX)
                   particle <t> <emitter> <dur>       (ParticleToy; not drawn yet)
  cam_<track>.txt  the camera motion sampled per key: "fps <f> n <n>", then n lines
                   "px py pz fx fy fz ux uy uz fov" (absolute world position; look and up are
                   +Z and +Y turned by the .mot rotation; fov = horizontal degrees on 4:3)
  <char>_*.vca   the toys' motions (vitaconv's VCA, bound at runtime to <char>/model.vcm)
  voice.wav      the soundtrack

Camera .mot translations in act/ex_rs are absolute: the first key equals the toy's Position.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dxm  # noqa: E402
import mot  # noqa: E402
import vitaconv  # noqa: E402
from vitaui import ini, qrot  # noqa: E402

DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
BS = "\\"
LANG = "spa"


def loc_strings(idx, table):
    """name -> Spanish text for a dialogue table (data/loc/<table>.loc + l_<table>_spa.bin)."""
    loc = idx.read(f"data{BS}loc{BS}{table.lower()}.loc")
    n, size = struct.unpack_from("<2I", loc)
    names = [x.decode("latin-1") for x in loc[8:8 + size].split(b"\0")[:n]]
    b = idx.read(f"data{BS}loc{BS}l_{table.lower()}_{LANG}.bin")
    total = struct.unpack_from("<I", b)[0]
    texts = b[6:4 + total].decode("utf-16-le").split("\0")
    return {nm: texts[i].strip() for i, nm in enumerate(names) if i < len(texts)}


def vec(s):
    return [float(c) for c in s.split(",")]


def main():
    a = sys.argv[1:]
    if len(a) != 2:
        print(__doc__)
        sys.exit(1)
    act, out = a[0].lower().replace("/", BS), a[1]
    os.makedirs(out, exist_ok=True)
    idx = dxm.AssetIndex(DATA)
    folder = f"data{BS}act{BS}{act.rsplit(BS, 1)[0]}{BS}"
    aty = ini(idx.read(f"data{BS}act{BS}{act}.aty").decode("latin-1"))["Activity"]
    script = ini(idx.read(folder + aty["cutscene"].lower()).decode("latin-1"))
    toys = ini(idx.read(folder + aty["cutscene"].lower()[:-3] + ".toy").decode("latin-1"))

    texts = {}
    if "locfilename" in aty:
        for line in idx.read("data" + BS + aty["locfilename"].lower().replace("/", BS)).decode("latin-1").splitlines():
            line = line.split(";", 1)[0].strip()
            if line:
                texts.update(loc_strings(idx, line))

    gen = script["General"]
    lines = [f"duration {(float(gen['endframe']) - float(gen.get('startframe', 0))) / float(gen['fps']):.4f}",
             f"scene {aty['scene'].lower()}",
             f"next {aty.get('nextactivity', '-')} {aty.get('nextstart', '-')}"]
    if "soundtrackfilename" in aty:
        # Dialogue\CUIntro -> audio\dialogue\spa\cu\spa_cuintro.wav (folder = first two letters)
        base = aty["soundtrackfilename"].split(BS)[-1].lower()
        src = f"data{BS}audio{BS}dialogue{BS}{LANG}{BS}{base[:2]}{BS}{LANG}_{base}.wav"
        open(os.path.join(out, "voice.wav"), "wb").write(idx.read(src))
        lines.append("voice voice.wav")
        print("voice.wav", src)

    # the animated cars: char code from the MCF path (Global\Chars\<code>\Anim\...)
    toy_char = {}
    for sec, kv in toys.items():
        if sec.startswith("CarsAnimatedToy") and "name" in kv:
            code = kv["mcfname"].split(BS)[2].lower()
            toy_char[kv["name"].lower()] = code
            lines.append("toy %s %s %s %s %s" % (kv["name"], code, " ".join("%.4f" % v for v in vec(kv["position"])),
                                                " ".join("%.5f" % v for v in vec(kv["lookvector"])),
                                                " ".join("%.5f" % v for v in vec(kv["upvector"]))))

    cams = {}
    for sec, kv in toys.items():
        if sec.startswith("AnimatedCameraToy") and "name" in kv:
            cams[kv["name"].lower()] = kv["motionname"]

    steps = sorted((float(kv["time"]), sec) for sec, kv in script.items()
                   if sec.startswith("Step") and "Action" not in sec)
    anims = set()
    for t, step in steps:
        n = 1
        while f"{step}Action{n}" in script:
            kv = script[f"{step}Action{n}"]
            n += 1
            kind = kv["actiontype"].lower()
            if kind == "animatedtoy":
                code = toy_char.get(kv["toyname"].lower())
                if not code:  # a prop (e.g. CS_1_1Z's cone07_door), not a car: not drawn yet
                    print("skipped prop animation", kv["toyname"], kv["motionname"])
                    continue
                nm = kv["motionname"].lower()
                anims.add((code, nm))
                lines.append(f"anim {t:.4f} {kv['toyname']} {nm}")
            elif kind == "animatedcamera":
                lines.append(f"cam {t:.4f} {kv['motionname'].lower()}")
            elif kind == "caption":
                txt = texts.get(kv["caption"], kv["caption"])
                lines.append(f"caption {t + float(kv['starttime']):.4f} {t + float(kv['endtime']):.4f} {txt}")
            elif kind == "videofx":
                if kv["type"].lower() == "fadein":
                    lines.append(f"fade in {t:.4f} {float(kv['durationin']):.3f}")
                else:
                    lines.append(f"fade out {t:.4f} {float(kv['durationout']):.3f} {float(kv.get('durationhold', 0)):.3f}")
            elif kind == "particletoy":
                lines.append(f"particle {t:.4f} {kv['emittername']} {float(kv['duration']):.3f}")
            else:
                print("unhandled action", kind, step)

    # camera tracks: sample at each key of the translation channel (30 per second here)
    for name, mo in cams.items():
        m = mot.parse(idx.read(folder + mo.lower() + ".mot"))
        n = max(2, m["frames"])
        fps = (n - 1) / m["duration"] if m["duration"] > 0 else 30.0
        rows = [f"fps {fps:.4f} n {n}"]
        for f in range(n):
            t = f / fps
            p = mot.sample_channel(m["translation"][0], t, "t")
            q = mot.sample_channel(m["rotation"][0], t, "r") if m["rotation"] else (0, 0, 0, 1)
            fw, up = qrot(q, (0, 0, 1)), qrot(q, (0, 1, 0))
            sk = m["scalar"][0]["keys"] if m["scalar"] else [(0, 60.0)]
            fov = sk[min(f, len(sk) - 1)][1]
            rows.append(" ".join("%.4f" % v for v in (*p, *fw, *up, fov)))
        open(os.path.join(out, f"cam_{name}.txt"), "w").write("\n".join(rows) + "\n")
    print(f"{len(cams)} camera tracks")

    for code, nm in sorted(anims):
        k = f"data{BS}global{BS}chars{BS}{code}{BS}anim{BS}{nm}.mot"
        d = vitaconv.convert_anim(idx, k)
        open(os.path.join(out, nm + ".vca"), "wb").write(d)
        print(f"{nm}.vca {len(d) // 1024} KB")

    open(os.path.join(out, "cutscene.txt"), "w", encoding="latin-1", newline="\n").write("\n".join(lines) + "\n")
    print("cutscene.txt", len(lines), "lines")


if __name__ == "__main__":
    main()
