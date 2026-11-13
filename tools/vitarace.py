#!/usr/bin/env python3
"""Export a race activity (data/act/<name>/<name>.aty + .trk + .rpt) for the Vita.

  vitarace.py <activity> <out_dir>      e.g. vitarace.py rr_1_1/rr_1_1 extracted/vita/act/rr_1_1

Writes out_dir/race.txt, one item per line (positions in game space, feet):
  race <laps> <racers> <boost 0|1>              TotalLaps, NumRacers, !DisableNOX
  grid <per_row> <slot_width> <row_sep> <start_node> <finish_node>   [RaceBoundary]
  bnode <i> px py pz lx ly lz rx ry rz <dist_to_finish> <n_next> next... <n_prev> prev...
  path <i> px py pz lx ly lz rx ry rz <speed_limit> <pace_speed> <boost> <n_next> next...
                                                (AI_LeaderLine; speeds in mph)
  racer <slot> <char> <start_order> <start_delay> <pace_pct> <min_pace_pct> <max_pace_pct>
        <max_offset> <offset_time>              ([VehicleN]; the player is slot 0)
  pace <position> <min_lap_s> <max_lap_s>      (.rpt [Normal]: the AI's lap times by position)
  next <activity>                               (after the race, from story/<name>_complete.as)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dxm  # noqa: E402
from vitaui import ini  # noqa: E402

DATA = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
BS = "\\"


def vec(s):
    return [float(c) for c in s.split(",")]


def ints(kv, count_key, prefix):
    n = int(kv.get(count_key.lower(), 0))
    return [int(kv[f"{prefix}{i}".lower()]) for i in range(n)]


def main():
    a = sys.argv[1:]
    if len(a) != 2:
        print(__doc__)
        sys.exit(1)
    act, out = a[0].lower().replace("/", BS), a[1]
    os.makedirs(out, exist_ok=True)
    idx = dxm.AssetIndex(DATA)
    folder = f"data{BS}act{BS}{act.rsplit(BS, 1)[0]}{BS}"
    aty = ini(idx.read(f"data{BS}act{BS}{act}.aty").decode("latin-1"))
    act_kv = aty["Activity"]
    trk = ini(idx.read(folder + act_kv["track"].lower()).decode("latin-1"))
    lines = [f"race {int(act_kv.get('totallaps', 3))} {int(act_kv.get('numracers', 1))} "
             f"{0 if act_kv.get('disablenox', 'F').upper().startswith('T') else 1}"]

    rb = trk["RaceBoundary"]
    lines.append(f"grid {int(rb.get('vehiclesperrow', 3))} {float(rb.get('vehicleslotwidth', 10.5)):g} "
                 f"{float(rb.get('vehiclerowseparation', 100)):g} {int(rb.get('startlinenode', 0))} "
                 f"{int(rb.get('finishlinenode', 0))}")
    for i in range(int(rb["numboundarynodes"])):
        kv = trk[f"BoundaryNode{i}"]
        nx, pv = ints(kv, "NumNextNodes", "Next"), ints(kv, "NumPrevNodes", "Prev")
        lines.append("bnode %d %s %s %s %.3f %d %s %d %s" % (
            i, " ".join("%.3f" % v for v in vec(kv["position"])), " ".join("%.3f" % v for v in vec(kv["leftedge"])),
            " ".join("%.3f" % v for v in vec(kv["rightedge"])), float(kv["distancetofinish"]),
            len(nx), " ".join(map(str, nx)), len(pv), " ".join(map(str, pv))))

    p = trk["Path0"]
    for i in range(int(p["numnodes"])):
        kv = trk[f"Path0Node{i}"]
        nx = ints(kv, "NumNextNodes", "Next")
        lines.append("path %d %s %s %s %.3f %.3f %d %d %s" % (
            i, " ".join("%.3f" % v for v in vec(kv["position"])), " ".join("%.3f" % v for v in vec(kv["leftedge"])),
            " ".join("%.3f" % v for v in vec(kv["rightedge"])), float(kv.get("speedlimit", 200)),
            float(kv.get("pacespeed", 100)), int(kv.get("boost", 0)), len(nx), " ".join(map(str, nx))))

    slot = 0
    while f"Vehicle{slot}" in aty:
        kv = aty[f"Vehicle{slot}"]
        lines.append("racer %d %s %d %g %g %g %g %g %g" % (
            slot, kv["vehicle"].lower(), int(kv.get("startlineorder", -1)), float(kv.get("startdelay", 0)),
            float(kv.get("pacepercentmodifier", 10)), float(kv.get("minpacepercentmodifier", 10)),
            float(kv.get("maxpacepercentmodifier", 10)), float(kv.get("maxoffsetdist", 0)),
            float(kv.get("timetoswitchoffset", 0))))
        slot += 1

    rpt_key = folder + act.rsplit(BS, 1)[-1] + ".rpt"
    if rpt_key in idx.files:
        rpt = ini(idx.read(rpt_key).decode("latin-1")).get("Normal", {})
        k = 1
        while f"min{k}" in rpt:
            lines.append(f"pace {k} {float(rpt[f'min{k}']):g} {float(rpt[f'max{k}']):g}")
            k += 1

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
