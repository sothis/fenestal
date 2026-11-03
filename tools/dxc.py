#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Microsoft's DirectX shader compiler for Linux, for the win64 tree.

    python3 tools/dxc.py --fetch     # download, check, unpack
    python3 tools/dxc.py --path      # the dxc to run, fetched first if need be
    python3 tools/dxc.py --status    # where it is, or that it is not

Direct3D 12 loads DXIL, and only DXIL a validator has signed unless the
machine is in developer mode. The Linux release carries the validator
(lib/libdxil.so), so what dxc compiles here comes out signed: measured,
the rectangle shader's container passed `dxv` with a non-zero hash. No
distribution packages it, so one release is fetched, pinned by its
archive's SHA-256 -- a download that does not match is refused -- and
kept under ~/.local/state/fenestal, which survives `rm -rf build`. The
build runs it as a tool, beside glslangValidator and spirv-cross, and
links nothing of it. A new release is a new VERSION, URL and SHA256 here.
"""

import argparse
import hashlib
import os
import sys
import tarfile
import urllib.request

VERSION = "1.9.2609"
ARCHIVE = "linux_dxc_2026_09_28.x86_x64.tar.gz"
URL = ("https://github.com/microsoft/DirectXShaderCompiler/releases/download/"
       "v%s/%s" % (VERSION, ARCHIVE))
SHA256 = "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1"

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "dxc", VERSION)
UNPACKED = os.path.join(HOME, "x")
DXC = os.path.join(UNPACKED, "bin", "dxc")
LIBS = os.path.join(UNPACKED, "lib")


def ready():
    return (os.access(DXC, os.X_OK) and
            os.path.isfile(os.path.join(LIBS, "libdxil.so")))


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch():
    os.makedirs(UNPACKED, exist_ok=True)
    archive = os.path.join(HOME, ARCHIVE)
    if not os.path.isfile(archive) or _sha256(archive) != SHA256:
        print("dxc: downloading %s" % URL, file=sys.stderr, flush=True)
        part = archive + ".part"
        with urllib.request.urlopen(URL, timeout=600) as r, \
                open(part, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
        os.replace(part, archive)
    got = _sha256(archive)
    if got != SHA256:
        os.remove(archive)
        sys.exit("dxc: %s has SHA-256 %s, not the pinned %s; removed"
                 % (ARCHIVE, got, SHA256))
    # Only bin/ and lib/, and nothing that would land outside the directory:
    # the archive is data.
    with tarfile.open(archive) as t:
        members = [m for m in t.getmembers()
                   if m.name.lstrip("./").split("/")[0] in ("bin", "lib")
                   and (m.isfile() or m.isdir())]
        t.extractall(UNPACKED, members=members, filter="data")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--path", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()
    if args.fetch or (args.path and not ready()):
        fetch()
    if args.path:
        print(DXC)
        return 0
    print("dxc %s: %s" % (VERSION, UNPACKED if ready() else
                          "not fetched -- --fetch"))
    return 0 if ready() else 1


if __name__ == "__main__":
    sys.exit(main())
