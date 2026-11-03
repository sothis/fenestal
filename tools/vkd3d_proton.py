#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""vkd3d-proton and DXVK's DXGI, for the win64 tree's Direct3D 12 under wine.

    python3 tools/vkd3d_proton.py --fetch     # download, check, unpack
    python3 tools/vkd3d_proton.py --dir       # where the DLLs are, fetched
                                              # first if need be
    python3 tools/vkd3d_proton.py --status    # where, or that they are not
    python3 tools/vkd3d_proton.py --run build/win64/release/fenestal-testbed.exe
                                              # a program of a tree run on
                                              # them, by hand, args after it

The tree's shaders are DXIL (tools/dxc.py), and the vkd3d wine 10.0 bundles
in wined3d.dll will not run DXIL: it reads the container and answers "not
implemented" (VKD3D_ERROR_NOT_IMPLEMENTED; DXIL is on by default only from
vkd3d 1.16). vkd3d-proton runs it. Its d3d12.dll needs DXVK's dxgi.dll beside
it: wine's own dxgi makes a D3D12 swapchain through wined3d and its vkd3d,
and given vkd3d-proton's device instead it reads through a null pointer.

For the harness's wine environments (environment.py stages them beside a
copy of the program, under build/drive/), and for a program run by hand
under wine (--run, which does the same into build/drive/run-by-hand-<tree>-
<variant>/ and starts it there with wine's own Direct3D 12 overridden; the
renderer is the program's to choose, or FNL_RENDERER's). A later wine needs
none of it: 11.20's own vkd3d draws the tree's DXIL. Never put into a
tree: copied to
Windows with the program, a d3d12.dll beside it would be loaded in place of
the system's. Two releases, each pinned by its archive's SHA-256 -- a
download that does not match is refused, not unpacked -- and kept under
~/.local/state/fenestal, which survives `rm -rf build`. A new release is a
new version and SHA-256 in RELEASES.

A fetch, by --fetch or by --dir and --run with nothing fetched yet, wants
Python 3.14 or later: vkd3d-proton's archive is .tar.zst, which tarfile
reads from 3.14 (compression.zstd) and not before.
"""

import argparse
import hashlib
import os
import sys
import tarfile
import urllib.request

# name, version, archive, URL, SHA-256, the x64 DLLs taken from it
RELEASES = (
    ("vkd3d-proton", "3.0.1", "vkd3d-proton-3.0.1.tar.zst",
     "https://github.com/HansKristian-Work/vkd3d-proton/releases/download/"
     "v3.0.1/vkd3d-proton-3.0.1.tar.zst",
     "3cf2315522af5e43605ef6d3c41dad91387040bf97199934f3f7ab76caaa2f0c",
     ("d3d12.dll", "d3d12core.dll")),
    ("dxvk", "3.1.1", "dxvk-3.1.1.tar.gz",
     "https://github.com/doitsujin/dxvk/releases/download/v3.1.1/"
     "dxvk-3.1.1.tar.gz",
     "40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608",
     ("dxgi.dll",)),
)

# What wine is told to load from the program's directory and not its own:
# native only, so a missing file is a failure rather than wine's vkd3d. And
# no debugger: a crash under it is a dialog on the desktop and a process
# that never exits, where the round wants an exit status.
OVERRIDES = "d3d12,d3d12core,dxgi=n;winedbg.exe=d"

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "vkd3d-proton",
                    "-".join(r[1] for r in RELEASES))
UNPACKED = os.path.join(HOME, "x64")


def dlls():
    """The DLLs to put beside the program, or None when not fetched."""
    paths = [os.path.join(UNPACKED, f) for r in RELEASES for f in r[5]]
    return paths if all(os.path.isfile(p) for p in paths) else None


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def _download(url, archive, sha):
    if os.path.isfile(archive) and _sha256(archive) == sha:
        return
    print("vkd3d_proton: downloading %s" % url, file=sys.stderr, flush=True)
    part = archive + ".part"
    with urllib.request.urlopen(url, timeout=600) as r, open(part, "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
    os.replace(part, archive)
    got = _sha256(archive)
    if got != sha:
        os.remove(archive)
        sys.exit("vkd3d_proton: %s has SHA-256 %s, not the pinned %s; "
                 "removed" % (os.path.basename(archive), got, sha))


def fetch():
    os.makedirs(UNPACKED, exist_ok=True)
    for name, version, file, url, sha, want in RELEASES:
        archive = os.path.join(HOME, file)
        _download(url, archive, sha)
        # By name and flattened, into a directory of the DLLs' own: the
        # archive is data, and only the members asked for are written.
        top = "%s-%s/x64/" % (name, version)
        with tarfile.open(archive) as t:
            for f in want:
                m = t.getmember(top + f)
                m.name = f
                t.extract(m, UNPACKED, filter="data")
    print("vkd3d-proton and DXVK's DXGI: %s" % UNPACKED, file=sys.stderr)


def run(exe, args):
    """`exe` of a tree started under wine from a directory of its own, the
    tree's .exe and .dll files and the DLLs beside it: the harness's
    staging (environment.py, _Vkd3dProton), for a hand. Returns wine's exit
    status."""
    import shutil
    import subprocess
    exe = os.path.abspath(exe)
    tree = os.path.dirname(exe)
    if not os.path.isfile(exe):
        sys.exit("vkd3d_proton: no %s -- ./build.sh win64" % exe)
    if not dlls():
        fetch()
    root = os.path.dirname(os.path.dirname(os.path.dirname(tree)))
    stage = os.path.join(root, "build", "drive", "run-by-hand-%s-%s"
                         % (os.path.basename(os.path.dirname(tree)),
                            os.path.basename(tree)))
    os.makedirs(stage, exist_ok=True)
    for src in [os.path.join(tree, f) for f in sorted(os.listdir(tree))
                if f.endswith((".exe", ".dll"))] + dlls():
        shutil.copy2(src, os.path.join(stage, os.path.basename(src)))
    env = dict(os.environ, WINEDLLOVERRIDES=OVERRIDES)
    print("vkd3d_proton: %s from %s" % (os.path.basename(exe), stage),
          file=sys.stderr, flush=True)
    proc = subprocess.Popen(["wine", os.path.basename(exe)] + list(args),
                            cwd=stage, env=env)
    try:
        return proc.wait()
    except KeyboardInterrupt:
        # Ctrl+C reaches wine too, which ends the program its own way.
        return proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--dir", action="store_true")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--run", metavar="EXE",
                    help="run a program of the win64 tree on them, by hand")
    ap.add_argument("args", nargs=argparse.REMAINDER,
                    help="the program's arguments, after --run EXE")
    args = ap.parse_args()
    if args.run:
        return run(args.run, args.args)
    if args.fetch or (args.dir and not dlls()):
        fetch()
    if args.dir:
        print(UNPACKED)
        return 0
    have = dlls()
    print("%s: %s" % (", ".join("%s %s" % r[:2] for r in RELEASES),
                      UNPACKED if have else "not fetched -- --fetch"))
    return 0 if have else 1


if __name__ == "__main__":
    sys.exit(main())
