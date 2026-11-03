#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""MoltenVK, Vulkan on Metal, beside the programs of a macos tree.

    python3 tools/moltenvk.py --fetch           # download, check, unpack
    python3 tools/moltenvk.py --into <tree>     # and put it in a tree
    python3 tools/moltenvk.py --status          # where it is, or that it is not

A Mac has no Vulkan of its own. MoltenVK implements it on Metal, with no
loader between: the library opens it from beside the program
(@executable_path, src/renderer/vulkan/vk_load.c) and hands it to a
program's own Vulkan scene (FnlVulkanGpu::get_instance_proc_addr); a
program that links it instead does so through the synthesised stub, which
names @rpath/libMoltenVK.dylib (CMakeLists.txt) -- so the one dylib beside
the programs is all there is to ship. No loader in a tree: Khronos builds
none for a Mac and Homebrew no longer does for an Intel one. The loader and
the validation layer a checker round wants are tools/vulkan_macos.py's,
built here and put beside the program by the harness alone; vk_load.c
opens a loader there before MoltenVK. build.sh puts the dylib and its
licence into every macos tree with --into.

One release, pinned by its archive's SHA-256: a download that does not
match is refused, not unpacked. Kept under ~/.local/state/fenestal, so it
survives `rm -rf build`. A new release is a new VERSION and SHA256 here.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request

VERSION = "1.4.2"
ARCHIVE = "MoltenVK-macos.tar"
URL = ("https://github.com/KhronosGroup/MoltenVK/releases/download/v%s/%s"
       % (VERSION, ARCHIVE))
SHA256 = "f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e"
# The dynamic library, universal, and the licence it travels under.
MEMBERS = ("MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib",
           "MoltenVK/LICENSE")

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "moltenvk", VERSION)
UNPACKED = os.path.join(HOME, "x")
DYLIB = os.path.join(UNPACKED, MEMBERS[0])
LICENCE = os.path.join(UNPACKED, MEMBERS[1])


def ready():
    return os.path.isfile(DYLIB) and os.path.isfile(LICENCE)


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
        print("moltenvk: downloading %s" % URL, file=sys.stderr, flush=True)
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
        sys.exit("moltenvk: %s has SHA-256 %s, not the pinned %s; removed"
                 % (ARCHIVE, got, SHA256))
    # By name, into a directory of its own: the archive is data, and tar
    # writes only the two members asked for.
    subprocess.run(["tar", "-C", UNPACKED, "-xf", archive] + list(MEMBERS),
                   check=True)
    print("MoltenVK %s: %s" % (VERSION, DYLIB))


def into(tree):
    """The dylib and its licence into `tree`, fetched first where they are
    not here yet. Copied when the tree's copy differs, so a rebuild leaves
    them as they are."""
    if not ready():
        fetch()
    for src, name in ((DYLIB, "libMoltenVK.dylib"),
                      (LICENCE, "MoltenVK-LICENSE.txt")):
        dst = os.path.join(tree, name)
        if not os.path.isfile(dst) or _sha256(dst) != _sha256(src):
            shutil.copyfile(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--into", metavar="TREE")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()
    if args.into:
        into(args.into)
        return 0
    if args.fetch:
        fetch()
        return 0
    print("MoltenVK %s: %s" % (VERSION, DYLIB if ready() else
                                "not fetched -- --fetch"))
    return 0 if ready() else 1


if __name__ == "__main__":
    sys.exit(main())
