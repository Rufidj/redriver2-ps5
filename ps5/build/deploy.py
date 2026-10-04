#!/usr/bin/env python3
"""Uploads the built title to the console through the ftpsrv payload.

  deploy.py              eboot.bin only (the fast loop; the game must be closed or the console answers "Text file busy")
  deploy.py --config     eboot.bin + assets/config.ini
  deploy.py --all        the whole title folder (sce_sys, sce_module, assets, eboot.bin) - first install
  deploy.py --fmv DIR    also uploads the intro/cut-scene videos from DIR (DRIVER2/FMV of the game data)

Environment: NAB, PS5_IP, PS5_FTP_PORT (default 2121), TITLE_ID (default PPSA00056).
ftpsrv wants a `cwd` per folder: absolute paths do not work with its MLSD/STOR, so everything is relative.
"""
import os, sys
from ftplib import FTP

NAB = os.environ["NAB"]
IP = os.environ["PS5_IP"]
PORT = int(os.environ.get("PS5_FTP_PORT", "2121"))
TITLE = os.environ.get("TITLE_ID", "PPSA00056")
DIST = os.path.join(NAB, "dist", TITLE)
REMOTE = "/data/homebrew/" + TITLE

ftp = FTP()
ftp.connect(IP, PORT, timeout=60)
ftp.login()

def cd(path):
    ftp.cwd("/")
    for p in path.split("/"):
        if p:
            try:
                ftp.cwd(p)
            except Exception:
                ftp.mkd(p)
                ftp.cwd(p)

def put(local, remote_dir):
    cd(remote_dir)
    with open(local, "rb") as f:
        ftp.storbinary("STOR " + os.path.basename(local), f, blocksize=1 << 20)

def put_tree(local_root, remote_root):
    for base, _dirs, files in os.walk(local_root):
        rel = os.path.relpath(base, local_root)
        rdir = remote_root if rel == "." else remote_root + "/" + rel.replace(os.sep, "/")
        for name in files:
            put(os.path.join(base, name), rdir)
            print("  ", rdir + "/" + name)

if "--all" in sys.argv:
    for sub in ("sce_sys", "sce_module", "assets"):
        if os.path.isdir(os.path.join(DIST, sub)):
            put_tree(os.path.join(DIST, sub), REMOTE + "/" + sub)
for i, a in enumerate(sys.argv):
    if a == "--fmv":
        put_tree(sys.argv[i + 1], REMOTE + "/assets/DRIVER2/FMV")
if "--config" in sys.argv:
    put(os.path.join(DIST, "assets", "config.ini"), REMOTE + "/assets")
    print("config ok")
try:
    put(os.path.join(DIST, "eboot.bin"), REMOTE)
    print("eboot ok")
except Exception as e:
    print("eboot FAIL (is the game open?)", e)
    sys.exit(1)
