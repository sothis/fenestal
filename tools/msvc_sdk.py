#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The MSVC CRT and the Windows SDK the win64-msvc tree cross-compiles on
Linux against.

    python3 tools/msvc_sdk.py --fetch     # download, check, splat
    python3 tools/msvc_sdk.py --path      # the root, fetched first if need be
    python3 tools/msvc_sdk.py --status    # which, or that it is not

The MSVC tree is built by cl, and cl is Microsoft's; on a Linux machine the
same tree is built by clang-cl with lld, which needs the CRT's and the
Windows SDK's headers and libraries and nothing else of Visual Studio. No
distribution packages either half, and Microsoft's licence reaches them
through their own installer only, so one tool that reads their manifest --
xwin, pinned here by its archive's SHA-256, as dxc.py pins Microsoft's
shader compiler -- downloads and lays them out under ~/.local/state/
fenestal, which survives `rm -rf build`.

The layout is xwin's `splat`: crt/ (the CRT's headers and lib/x64) and sdk/
(the Windows SDK's include/ and lib/), which is what clang-cl's
-vctoolsdir and -winsdkdir name, and what cmake/windows-msvc-x86_64.cmake
points the compiler at. The debug CRT is not redistributable and so is not
in it: that tree links the release runtime in every configuration, and the
toolchain file says so.
"""

import argparse
import hashlib
import os
import subprocess
import sys
import tarfile
import urllib.request

VERSION = "0.10.0"
ARCHIVE = "xwin-%s-x86_64-unknown-linux-musl.tar.gz" % VERSION
SHA256 = ("d870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597"
          "fc88")
URL = ("https://github.com/Jake-Shadle/xwin/releases/download/%s/%s"
       % (VERSION, ARCHIVE))
# The manifest generation xwin reads: VS 2022's, which is what the tree's
# CRT and SDK are pinned to by that.
MANIFEST = "17"

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "msvc-sdk")
ROOT = os.path.join(HOME, "splat")
CACHE = os.path.join(HOME, "cache")
XWIN = os.path.join(HOME, "xwin")
# What says the splat is whole: a header from each half, and a library from
# each of the three lib directories the link reads.
MARKERS = ("crt/include/vcruntime.h", "sdk/include/um/windows.h",
           "sdk/include/ucrt/stdio.h", "crt/lib/x64/libcmt.lib",
           "sdk/lib/ucrt/x64/ucrt.lib", "sdk/lib/um/x64/d3d12.lib")


def ready():
    return all(os.path.isfile(os.path.join(ROOT, m)) for m in MARKERS)


def _sha256(path):
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
    except OSError as e:
        sys.exit("msvc_sdk: %s: %s" % (path, e))
    return h.hexdigest()


def _get(url, path, sha256):
    """Fetch `url` to `path` unless it is there whole, and refuse it
    otherwise: a download that does not match its pin is removed."""
    if os.path.isfile(path) and _sha256(path) == sha256:
        return
    print("msvc_sdk: downloading %s" % url, file=sys.stderr, flush=True)
    part = path + ".part"
    try:
        with urllib.request.urlopen(url, timeout=600) as r, \
                open(part, "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
    except OSError as e:
        if os.path.exists(part):
            os.remove(part)
        sys.exit("msvc_sdk: %s: %s" % (url, e))
    got = _sha256(part)
    if got != sha256:
        try:
            os.remove(part)
        except OSError as e:
            sys.exit("msvc_sdk: %s: %s" % (part, e))
        sys.exit("msvc_sdk: %s has SHA-256 %s, not the pinned %s"
                 % (url, got, sha256))
    try:
        os.replace(part, path)
    except OSError as e:
        sys.exit("msvc_sdk: %s: %s" % (path, e))


def _unpack(archive):
    """The xwin binary and its licences, and nothing outside HOME: the
    archive is data."""
    root = os.path.realpath(HOME)
    try:
        with tarfile.open(archive) as t:
            for m in t.getmembers():
                name = os.path.basename(m.name)
                if not m.isfile() or not (name == "xwin" or
                                          name.startswith("LICENSE")):
                    continue
                dest = os.path.realpath(os.path.join(HOME, name))
                if os.path.commonpath([root, dest]) != root:
                    sys.exit("msvc_sdk: %s names a path outside %s"
                             % (m.name, HOME))
                src = t.extractfile(m)
                assert src is not None
                with src, open(dest, "wb") as out:
                    out.write(src.read())
                os.chmod(dest, m.mode | (0o111 if name == "xwin" else 0))
    except OSError as e:
        sys.exit("msvc_sdk: %s: %s" % (archive, e))


def fetch():
    try:
        os.makedirs(HOME, exist_ok=True)
        os.makedirs(CACHE, exist_ok=True)
    except OSError as e:
        sys.exit("msvc_sdk: %s: %s" % (HOME, e))
    archive = os.path.join(CACHE, ARCHIVE)
    _get(URL, archive, SHA256)
    if not os.path.isfile(XWIN):
        _unpack(archive)
    # --preserve-ms-arch-notation so that the layout is cl.exe's: lib/x64
    # and lib/ucrt/x64, which is what clang-cl's -vctoolsdir derives its
    # paths from, and not LLVM's x86_64. --accept-license because this is
    # asked once, by a person, from a command they type: the prompt xwin
    # would show is not answerable from a build. Nothing is installed; the
    # files are unpacked under HOME.
    cmd = [XWIN, "--accept-license", "--cache-dir", CACHE,
           "--manifest-version", MANIFEST,
           "--arch", "x86_64", "--variant", "desktop",
           "splat", "--output", ROOT, "--preserve-ms-arch-notation"]
    print("msvc_sdk: splatting the CRT and the Windows SDK", file=sys.stderr,
          flush=True)
    rc = subprocess.run(cmd).returncode
    if rc != 0:
        sys.exit("msvc_sdk: xwin splat failed (%d)" % rc)
    if not ready():
        sys.exit("msvc_sdk: %s is not whole after splatting" % ROOT)


def main():
    ap = argparse.ArgumentParser(
        description="The MSVC CRT and Windows SDK the win64-msvc tree "
                    "cross-compiles against")
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--path", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()
    if args.fetch or (args.path and not ready()):
        fetch()
    if args.path:
        print(ROOT)
        return 0
    print("xwin %s: %s" % (VERSION,
                           ROOT if ready() else "not fetched -- --fetch"))
    return 0 if ready() else 1


if __name__ == "__main__":
    sys.exit(main())
