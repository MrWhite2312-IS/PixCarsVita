#!/usr/bin/env python3
"""Convert the game's Bink videos (.bik) to MP4 the Vita plays in hardware (sceAvPlayer):
H.264 Main, yuv420p, 960x544 (4:3 display restored, centre cropped to 16:9), AAC stereo 48 kHz.

Needs an ffmpeg build with the Bink decoders (set FFMPEG or put ffmpeg on PATH).

Usage:
  vitamovies.py <out_dir> [name ...]     default: every .bik in AppStart and Movies
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("CARS_DATA", os.path.join(HERE, "..", "PC data", "Data"))
FFMPEG = os.environ.get("FFMPEG") or shutil.which("ffmpeg") or "ffmpeg"


def sources():
    out = {}
    for d in ("AppStart", "Movies"):
        p = os.path.join(GAME, d)
        for f in sorted(os.listdir(p)) if os.path.isdir(p) else []:
            if f.lower().endswith(".bik") and "old" not in f.lower():
                out[f[:-4].lower()] = os.path.join(p, f)
    return out


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    out = a[0]
    os.makedirs(out, exist_ok=True)
    src = sources()
    names = [n.lower() for n in a[1:]] or list(src)
    for n in names:
        dst = os.path.join(out, n + ".mp4")
        cmd = [FFMPEG, "-v", "error", "-y", "-i", src[n],
               # the 512x512 frames were shown stretched to a 4:3 screen (the films are
               # letterboxed inside): restore 4:3, then crop the centre to the Vita's 16:9
               "-vf", "scale=960:720,crop=960:544,setsar=1",
               "-c:v", "libx264", "-profile:v", "main", "-level", "3.1", "-pix_fmt", "yuv420p",
               "-preset", "medium", "-crf", "21",
               "-c:a", "aac", "-b:a", "160k", "-ar", "48000", "-ac", "2",
               "-movflags", "+faststart", dst]
        subprocess.run(cmd, check=True)
        print(f"{n}.mp4  {os.path.getsize(dst) / 1e6:.1f} MB")
    open(os.path.join(out, "movies.txt"), "w").write("\n".join(names) + "\n")


if __name__ == "__main__":
    main()
