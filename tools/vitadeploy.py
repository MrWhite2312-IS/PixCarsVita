"""Deploy PixCarsVita to a Vita over the network (vitacompanion or VitaShell FTP).

  python tools/vitadeploy.py                 # upload eboot.bin, launch, wait, fetch log
  python tools/vitadeploy.py --data          # also sync extracted/vita/* -> ux0:data/pixcars/
  python tools/vitadeploy.py --log           # only fetch log.txt
  python tools/vitadeploy.py --dumps         # fetch new crash dumps from ux0:data/

Vita IP comes from --ip or the VITA_IP environment variable.
Launch needs vitacompanion's command port (1338); without it you start the game by hand.
"""
import argparse
import ftplib
import os
import socket
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TITLE_ID = "PXCV00001"
EBOOT = os.path.join(ROOT, "vita", "build", "eboot.bin")
DATA_SRC = os.path.join(ROOT, "extracted", "vita")
DATA_DST = "/ux0:/data/pixcars"  # VitaShell needs the leading slash
LOG_REMOTE = DATA_DST + "/log.txt"
OUT_DIR = os.path.join(ROOT, "vita", "build", "device")


def command(ip, text):
    """Send one vitacompanion command; False when the command port is not there."""
    try:
        with socket.create_connection((ip, 1338), timeout=5) as s:
            s.sendall((text + "\n").encode())
            try:
                s.recv(256)
            except socket.timeout:
                pass
        return True
    except OSError:
        return False


def connect(ip):
    ftp = ftplib.FTP()
    ftp.connect(ip, 1337, timeout=20)
    ftp.login()
    return ftp


def remote_sizes(ftp, path):
    """name -> size (files) or None (dirs), parsed from LIST."""
    out = {}
    lines = []
    try:  # VitaShell's server rejects LIST <path>: change directory first
        ftp.cwd(path + "/")
        ftp.retrlines("LIST", lines.append)
    except ftplib.error_perm:
        return None
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9:
            continue
        out[parts[8]] = None if line.startswith("d") else int(parts[4])
    return out


def upload(ftp, local, remote):
    with open(local, "rb") as f:
        ftp.storbinary("STOR " + remote, f, blocksize=64 * 1024)


def sync_data(ftp):
    sent = 0
    for dirpath, _, files in os.walk(DATA_SRC):
        rel = os.path.relpath(dirpath, DATA_SRC).replace("\\", "/")
        rdir = DATA_DST if rel == "." else DATA_DST + "/" + rel
        have = remote_sizes(ftp, rdir)
        if have is None:
            ftp.mkd(rdir)
            have = {}
        for name in sorted(files):
            local = os.path.join(dirpath, name)
            if have.get(name) == os.path.getsize(local):
                continue
            print("  data", rdir + "/" + name)
            upload(ftp, local, rdir + "/" + name)
            sent += 1
    print("data: %d file(s) uploaded" % sent)


def fetch_log(ftp):
    os.makedirs(OUT_DIR, exist_ok=True)
    dst = os.path.join(OUT_DIR, "log.txt")
    try:
        with open(dst, "wb") as f:
            ftp.retrbinary("RETR " + LOG_REMOTE, f.write)
    except ftplib.error_perm:
        print("no log.txt on the Vita")
        return
    print("log ->", dst)
    with open(dst, encoding="utf-8", errors="replace") as f:
        sys.stdout.write(f.read())


def fetch_dumps(ftp):
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, size in (remote_sizes(ftp, "/ux0:/data") or {}).items():
        if size is None or not name.endswith(".psp2dmp"):
            continue
        dst = os.path.join(OUT_DIR, name)
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            continue
        with open(dst, "wb") as f:
            ftp.retrbinary("RETR /ux0:/data/" + name, f.write)
        print("dump ->", dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=os.environ.get("VITA_IP"))
    ap.add_argument("--data", action="store_true", help="sync extracted/vita data too")
    ap.add_argument("--log", action="store_true", help="only fetch log.txt")
    ap.add_argument("--dumps", action="store_true", help="only fetch crash dumps")
    ap.add_argument("--wait", type=float, default=15, help="seconds to run before fetching the log")
    a = ap.parse_args()
    if not a.ip:
        sys.exit("set --ip or VITA_IP")

    if a.log or a.dumps:
        ftp = connect(a.ip)
        if a.log:
            fetch_log(ftp)
        if a.dumps:
            fetch_dumps(ftp)
        ftp.quit()
        return

    has_cmd = command(a.ip, "destroy")  # close the running game so eboot.bin can be replaced
    if has_cmd:
        time.sleep(2)
    ftp = connect(a.ip)
    if a.data:
        sync_data(ftp)
    upload(ftp, EBOOT, "/ux0:/app/%s/eboot.bin" % TITLE_ID)
    print("eboot.bin uploaded")
    ftp.quit()

    if not has_cmd:
        print("no vitacompanion command port (1338): start the game on the Vita, then run --log")
        return
    command(a.ip, "launch " + TITLE_ID)
    print("launched, waiting %gs" % a.wait)
    time.sleep(a.wait)
    ftp = connect(a.ip)
    fetch_log(ftp)
    fetch_dumps(ftp)
    ftp.quit()


if __name__ == "__main__":
    main()
