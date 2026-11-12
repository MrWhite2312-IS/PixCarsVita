#!/usr/bin/env python3
"""Export Cars (2006) audio for the Vita build (data stays the user's own copy).

The game's .wav files are RIFF with format tag 2 but hold headerless IMA ADPCM:
stereo music = one byte per frame (low nibble left, high nibble right), mono engine loops =
two samples per byte (low nibble first). They are copied unchanged; the Vita decodes them.

Writes into <out_dir>:
  music/<name>.wav + music/playlist.txt        background tracks, in play order
  engine/<file>.wav + engine/engine.txt         a character's engine profile:
      gear <speed_lo> <speed_hi> <rpm_lo> <rpm_hi>     <char>EngineAudio.ini (MapSpeedToRPM)
      loop <file> <base_rate> <rpm>:<vol>:<rate> ...  carengine.aud [<Char>LoopN]
  (rate = Hz added to the loop's original sample rate, vol in percent)

Usage:
  vitaaudio.py <out_dir> [char=Mcq] [music=score_rs_day_1,score_rs_day_2,licensed1,...]
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxm

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
BS = "\\"
DEFAULT_MUSIC = ["score_rs_day_1", "score_rs_day_2"] + [f"licensed{i}" for i in (1, 2, 3, 4, 5, 7, 8, 9, 10, 11)]


def ini_sections(text):
    return {s.lower(): {k: v[1] for k, v in kv.items()} for s, kv in dxm.parse_ini(text)}


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    out = a[0]
    opts = dict(x.split("=", 1) for x in a[1:] if "=" in x)
    char = opts.get("char", "Mcq")
    music = opts["music"].split(",") if "music" in opts else DEFAULT_MUSIC
    idx = dxm.AssetIndex(DATA)

    os.makedirs(os.path.join(out, "music"), exist_ok=True)
    names = []
    for m in music:
        k = BS.join(["data", "audio", "music", m.lower() + ".wav"])
        if k not in idx.files:
            print("missing", k)
            continue
        open(os.path.join(out, "music", m.lower() + ".wav"), "wb").write(idx.read(k))
        names.append(m.lower())
    open(os.path.join(out, "music", "playlist.txt"), "w").write("\n".join(names) + "\n")
    print(f"music: {len(names)} tracks")

    eng = os.path.join(out, "engine")
    os.makedirs(eng, exist_ok=True)
    secs = ini_sections(idx.read(BS.join(["data", "audio", "engine", "carengine.aud"])).decode("latin-1"))
    prof = secs[char.lower()]
    base_rate = int(float(prof.get("originalsamplerateps2", "32000")))
    lines = []
    gears = ini_sections(idx.read(BS.join(["data", "audio", "engine", char.lower() + "engineaudio.ini"])).decode("latin-1"))
    for g in range(1, int(gears["general"]["numberofgears"]) + 1):
        s = gears[f"gear{g}"]
        lines.append(f"gear {s['rpmkey1']} {s['rpmkey2']} {s['minrpm']} {s['maxrpm']}")
    for n in range(1, int(prof["numberofengineloops"]) + 1):
        lp = secs[f"{char.lower()}loop{n}"]
        src = BS.join(["data", "audio", lp["filename"].lower().replace("/", BS) + ".wav"])
        fn = src.rsplit(BS, 1)[-1]
        open(os.path.join(eng, fn), "wb").write(idx.read(src))
        keys = " ".join(f"{lp[f'key{i}rpm']}:{lp[f'key{i}vol']}:{lp[f'key{i}rate']}"
                        for i in range(1, int(lp["numberofkeys"]) + 1))
        lines.append(f"loop {fn} {base_rate} {keys}")
    open(os.path.join(eng, "engine.txt"), "w").write("\n".join(lines) + "\n")
    print(f"engine: {char}, {int(prof['numberofengineloops'])} loops")

    # driving effects from sfx\fx.aud [<char>Burnout|Powerslide|Brake|Suspension]; impacts from
    # sfx\impact. sfx.txt: name file [slip_min slip_max freq_min freq_max vol_min vol_max fade]
    # (suspension lines carry the velocity tier start in slip_min)
    sfxd = os.path.join(out, "sfx")
    os.makedirs(sfxd, exist_ok=True)
    fx = ini_sections(idx.read(BS.join(["data", "audio", "sfx", "fx.aud"])).decode("latin-1"))
    rows = []

    def take(name, ref, params=""):
        src = BS.join(["data", "audio", ref.lower().replace("/", BS) + ".wav"])
        if src not in idx.files:
            print("missing", src)
            return
        fn = src.rsplit(BS, 1)[-1]
        open(os.path.join(sfxd, fn), "wb").write(idx.read(src))
        rows.append(f"{name} {fn} {params}".rstrip())

    for role in ("burnout", "powerslide"):
        s = fx[f"{char.lower()}{role}"]
        take(role, s["filename"], " ".join(s[k] for k in ("slipmin", "slipmax", "frequencymodifiermin",
                                                          "frequencymodifiermax", "volumemin", "volumemax",
                                                          "fadeduration")))
    take("brake", fx[f"{char.lower()}brake"]["filename"])
    sus = fx[f"{char.lower()}suspension"]
    for i in range(1, int(sus["numsounds"]) + 1):
        take("suspension", sus[f"sound{i}"], f"{sus[f'sound{i}velocitytierstart']} {sus['maxlevelvelocity']} "
             f"0 0 {sus['minlevelattenuation']} {sus['maxlevelattenuation']} {sus['minlevelvelocity']}")
    for i in (1, 2, 3):
        take("impact", f"SFX\\Impact\\imp_veh_hard_0{i}")
    # front end: the UIResponseEmitter bank (sfx/ui_fe.aud) and the drive-in's crickets loop
    # (scenes/fe/fe.aud [SoultreeSound1]); named after their files, e.g. fe_select_continue
    ui = ini_sections(idx.read(BS.join(["data", "audio", "sfx", "ui_fe.aud"])).decode("latin-1"))["uiresponseemitter"]
    for i in range(1, int(ui[f"numsounds"]) + 1):
        take(ui[f"sound{i}"].rsplit(BS, 1)[-1].lower(), ui[f"sound{i}"])
    fe = ini_sections(idx.read(BS.join(["data", "scenes", "fe", "fe.aud"])).decode("latin-1"))["soultreesound1"]
    take(fe["refname"].lower(), fe["soundresourcefile"])
    open(os.path.join(sfxd, "sfx.txt"), "w").write("\n".join(rows) + "\n")
    print(f"sfx: {len(rows)} sounds")


if __name__ == "__main__":
    main()
