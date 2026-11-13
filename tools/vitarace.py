#!/usr/bin/env python3
"""Export a race activity (data/act/<name>/<name>.aty + .trk + .rpt) for the Vita.

  vitarace.py <activity> <out_dir>      e.g. vitarace.py rr_1_1/rr_1_1 extracted/vita/act/rr_1_1

Writes out_dir/race.txt with the values as the files hold them; vita/src/race.c applies the
game's parsing (defaults and unit conversions from the PS2 ELF). One item per line, positions in
game space (feet):
  act <Key> <value>                             [Activity] keys of the .aty
  grid <per_row> <slot_width> <row_sep> <start_node> <finish_node>   [RaceBoundary]
  bnode <i> px py pz lx ly lz rx ry rz <dist_to_finish> <zone_id> <branch_id> <no_reset> <progress>
        <n_next> next... <n_prev> prev...       (ZoneID default 1; NoReset/ProgressNode: key present)
  path <p> <num_nodes> <num_segments> <linear> <start_node> <times_computed>
                                                [Path<p>] (RacePath loader 0x38c990)
  seg <p> <i> <start> <finish> <section_id> <has_time> <segment_time> <branch_count>
      then per branch: <branch> <n_times> <times_to_finish...>
  pnode <p> <i> px py pz lx ly lz rx ry rz <speed_limit> <pace_speed> <flags> <dist_to_finish>
        <remaining_segment_time> <segment_id> <n_next> next...
        (speeds in mph as written; missing keys: the node constructor's 0 (0x38c700); flags as
        the loader sets them: Boost 1, PowerSlide 2, Jump 8, NoAI 0x10, Corner 0x20,
        DriveBackward 0x40, ForceTargetSpeed 0x80; no NumNextNodes: next = i+1, the last -> 0)
  racer <slot> <char>                           [Vehicle<slot>]; the player is slot 0
  rkey <slot> <Key> <value>                     every key of [Vehicle<slot>], as written
  rpt <section> <i> <min> <max>                 .rpt [<section>] Min<i>/Max<i> (0x3301d0)
  next <activity>                               (after the race, from story/<name>_complete.as)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dxm  # noqa: E402

DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
BS = "\\"
FLAGS = (("boost", 1), ("powerslide", 2), ("jump", 8), ("noai", 0x10), ("corner", 0x20),
         ("drivebackward", 0x40), ("forcetargetspeed", 0x80))


def sections(text):
    """[section] -> [(Key, value)] in file order, keys with their case (comments stripped)."""
    secs, cur = {}, None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and "]" in line:
            cur = secs.setdefault(line[1:line.index("]")], [])
        elif cur is not None and "=" in line:
            k, v = (x.strip() for x in line.split("=", 1))
            cur.append((k, v.strip('"')))
    return secs


def kv(sec):
    """lower-case key -> value (last wins)."""
    return {k.lower(): v for k, v in sec}


def vec(s):
    return [float(c) for c in s.split(",")]


def num(s, default=0.0):
    try:
        return float(s.rstrip("f;"))
    except (TypeError, ValueError):
        return default


def boolv(s):
    """the game's boolean read (FUN_001638d0): a number, or T/F/TRUE/FALSE."""
    s = (s or "").strip().upper()
    if s.startswith("T"):
        return 1
    if s.startswith("F") or not s:
        return 0
    return int(num(s))


def main():
    a = sys.argv[1:]
    if len(a) != 2:
        print(__doc__)
        sys.exit(1)
    act, out = a[0].lower().replace("/", BS), a[1]
    os.makedirs(out, exist_ok=True)
    idx = dxm.AssetIndex(DATA)
    folder = f"data{BS}act{BS}{act.rsplit(BS, 1)[0]}{BS}"
    aty = sections(idx.read(f"data{BS}act{BS}{act}.aty").decode("latin-1"))
    lines = [f"act {k} {v}" for k, v in aty["Activity"] if " " not in k]
    trk = sections(idx.read(folder + kv(aty["Activity"])["track"].lower()).decode("latin-1"))

    rb = kv(trk["RaceBoundary"])
    lines.append(f"grid {int(rb.get('vehiclesperrow', 3))} {num(rb.get('vehicleslotwidth'), 10.5):g} "
                 f"{num(rb.get('vehiclerowseparation'), 100):g} {int(rb.get('startlinenode', 0))} "
                 f"{int(rb.get('finishlinenode', 0))}")
    for i in range(int(rb["numboundarynodes"])):
        n = kv(trk[f"BoundaryNode{i}"])
        nx = [int(n[f"next{k}"]) for k in range(int(n.get("numnextnodes", 0)))]
        pv = [int(n[f"prev{k}"]) for k in range(int(n.get("numprevnodes", 0)))]
        lines.append("bnode %d %s %s %s %.3f %d %d %d %d %d %s %d %s" % (
            i, " ".join("%.3f" % v for v in vec(n["position"])), " ".join("%.3f" % v for v in vec(n["leftedge"])),
            " ".join("%.3f" % v for v in vec(n["rightedge"])), num(n.get("distancetofinish")),
            int(num(n.get("zoneid"), 1)), int(num(n.get("branchid"))), int("noreset" in n), int("progressnode" in n),
            len(nx), " ".join(map(str, nx)), len(pv), " ".join(map(str, pv))))

    for p in range(int(kv(trk.get("RaceTrack", [])).get("numaipaths", 1))):
        pk = kv(trk[f"Path{p}"])
        nn, ns = int(pk["numnodes"]), int(pk.get("numsegments", 0))
        lines.append(f"path {p} {nn} {ns} {int(pk.get('lineartrack', 0))} {int(pk.get('startnode', 0))} "
                     f"{int(pk.get('timescomputed', 0))}")
        for s in range(ns):
            sk = kv(trk[f"Path{p}Segment{s}"])
            nb = int(sk.get("branchcount", 1))
            row = [f"seg {p} {s} {int(sk.get('startnode', 0))} {int(sk.get('finishnode', 0))} "
                   f"{int(sk.get('sectionid', 0))} {int('segmenttime' in sk)} {num(sk.get('segmenttime')):g} {nb}"]
            for b in range(nb):
                nt = int(sk.get(f"numberoflaptimes{b}", 0))
                times = [num(sk.get(f"timestofinish{b}{t}")) for t in range(max(nt, 0))]
                row.append(f"{int(sk.get(f'branch{b}', 0))} {max(nt, 0)} " + " ".join(f"{t:g}" for t in times))
            lines.append(" ".join(r.strip() for r in row))
        for i in range(nn):
            n = kv(trk[f"Path{p}Node{i}"])
            flags = sum(bit for key, bit in FLAGS if boolv(n.get(key)))
            cnt = int(n.get("numnextnodes", 0))
            nx = [int(n[f"next{k}"]) for k in range(cnt)] if cnt >= 1 else [0 if i == nn - 1 else i + 1]
            zero = "0,0,0"
            lines.append("pnode %d %d %s %s %s %g %g %d %g %g %d %d %s" % (
                p, i, " ".join("%.3f" % v for v in vec(n.get("position", zero))),
                " ".join("%.3f" % v for v in vec(n.get("leftedge", zero))),
                " ".join("%.3f" % v for v in vec(n.get("rightedge", zero))),
                num(n.get("speedlimit")), num(n.get("pacespeed")), flags, num(n.get("distancetofinish")),
                num(n.get("remainingsegmenttime")), int(num(n.get("segmentid"))), len(nx), " ".join(map(str, nx))))

    slot = 0
    while f"Vehicle{slot}" in aty:
        sec = aty[f"Vehicle{slot}"]
        lines.append(f"racer {slot} {kv(sec)['vehicle'].lower()}")
        lines += [f"rkey {slot} {k} {v}" for k, v in sec if " " not in k and v]
        slot += 1

    rpt_key = folder + act.rsplit(BS, 1)[-1] + ".rpt"
    if rpt_key in idx.files:
        for name, sec in sections(idx.read(rpt_key).decode("latin-1")).items():
            r = kv(sec)
            for i in range(0, 16):
                if f"max{i}" in r:
                    lines.append(f"rpt {name} {i} {num(r.get(f'min{i}')):g} {num(r[f'max{i}']):g}")

    # what the story does after the race: story/<name>_complete.as switches to the activity
    # (RR_1_1 -> RR_1_1\CS_1_1Z the first time)
    comp = f"data{BS}story{BS}{act.rsplit(BS, 1)[-1]}_complete.as"
    if comp in idx.files:
        for line in idx.read(comp).decode("latin-1").splitlines():
            if line.strip().lower().startswith("newactivityname"):
                lines.append("next " + line.split("=", 1)[1].strip())
                break

    open(os.path.join(out, "race.txt"), "w", newline="\n").write("\n".join(lines) + "\n")
    print("race.txt", len(lines), "lines")


if __name__ == "__main__":
    main()
