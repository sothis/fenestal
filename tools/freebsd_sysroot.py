#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The FreeBSD sysroot the freebsd tree is cross-compiled against.

    python3 tools/freebsd_sysroot.py --fetch    # download and unpack
    python3 tools/freebsd_sysroot.py --path     # where it is, for build.sh
    python3 tools/freebsd_sysroot.py --status   # which, or that it is not

The FreeBSD tree is built on Linux by clang, as the macOS one is: the base
system's headers and libraries from the release's base.txz, and the ports
the build reads headers and pkg-config files from, fetched from the
release's own package repository. Neither is code this project ships, and
nothing here runs them: the libraries are linked against a sysroot and the
tree is never executed. Kept under ~/.local/state/fenestal, which survives
`rm -rf build`.

Everything is pinned, the base tarball and the repository's index alike: a
repository that has moved is a new pin, in the commit that builds against
it, and not a silently different sysroot. Each package is checked against
the hash the index gives it, and the two indexes against theirs, so a
download that does not match is refused rather than unpacked.

The base needs no unpacking of the whole tarball: only what a compiler and
a linker read -- the headers, the libraries, the C++ standard library's
and the compiler's own -- is taken out of it.

A failure of the disk or the network is one line and a stop, as every tool
here answers: the sysroot is either whole or not there, and a half one
would fail the build in a file the compiler would name instead.
"""

import argparse
import hashlib
import json
import os
import sys
import tarfile
import urllib.request

VERSION = "15.1"
ARCH = "amd64"
ABI = "FreeBSD:15:%s" % ARCH

RELEASE = "https://download.freebsd.org/releases/%s/%s-RELEASE" % (ARCH,
                                                                  VERSION)
BASE = "base.txz"
BASE_SHA256 = ("3768988b151c20f965679062b065c63a977d6bbb9f47fd83695ec2"
               "c40790c18f")
# The package repository, its index, and the index's hash: the whole of it
# is pinned by that one, packages and dependencies together.
REPO = "https://pkg.freebsd.org/%s/release_1" % ABI
INDEX = "packagesite.pkg"
INDEX_SHA256 = ("cfbd9c50689ffd3f70971634e85017ae5aa268ba8cac361014ad1c"
                "fdd955b9e2")
# What the build reads: the window servers', the Vulkan headers' and the
# DRM's, each a port's. Every dependency of these comes with them.
PACKAGES = ("wayland", "libxcb", "libdrm", "vulkan-headers")
# Of the base tarball, the directories a compile and a link read: the
# headers, the libraries, the compiler's own (crt objects, libclang_rt and
# compiler-rt's headers) and the C++ standard library. Everything else in
# base.txz -- /usr/bin, the kernel, the man pages -- is four fifths of the
# download and nothing this build opens.
BASE_PATHS = ("usr/include", "usr/lib", "lib", "usr/libdata")
# What says the sysroot is whole.
MARKERS = ("usr/include/sys/event.h", "usr/lib/crt1.o", "usr/lib/libc.so",
           "usr/local/include/wayland-client.h",
           "usr/local/libdata/pkgconfig/xcb.pc")

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
HOME = os.path.join(STATE, "freebsd-sysroot", VERSION)
ROOT = os.path.join(HOME, "root")
CACHE = os.path.join(HOME, "cache")


def ready():
    return all(os.path.exists(os.path.join(ROOT, m)) for m in MARKERS)


def _sha256(path):
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
    except OSError as e:
        sys.exit("freebsd_sysroot: %s: %s" % (path, e))
    return h.hexdigest()


def _get(url, path, sha256):
    """Fetch `url` to `path` unless it is there whole, and refuse it
    otherwise: a download that does not match its pin is removed."""
    if os.path.isfile(path) and _sha256(path) == sha256:
        return
    print("freebsd_sysroot: downloading %s" % url, file=sys.stderr,
          flush=True)
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
        sys.exit("freebsd_sysroot: %s: %s" % (url, e))
    got = _sha256(part)
    if got != sha256:
        try:
            os.remove(part)
        except OSError as e:
            sys.exit("freebsd_sysroot: %s: %s" % (part, e))
        sys.exit("freebsd_sysroot: %s has SHA-256 %s, not the pinned %s"
                 % (url, got, sha256))
    try:
        os.replace(part, path)
    except OSError as e:
        sys.exit("freebsd_sysroot: %s: %s" % (path, e))


def _dest(name):
    """Where a member's path lands under the sysroot, refused if it would
    land outside: an archive is data, and one naming ../.. is not followed."""
    root = os.path.realpath(ROOT)
    try:
        dest = os.path.realpath(os.path.join(root, name.lstrip("./")
                                             .lstrip("/")))
    except OSError as e:
        sys.exit("freebsd_sysroot: %s: %s" % (name, e))
    if os.path.commonpath([root, dest]) != root:
        sys.exit("freebsd_sysroot: %s names a path outside %s"
                 % (name, root))
    return dest


def _place(m, t):
    """One member of an archive at its place under the sysroot: a file, a
    directory or a link."""
    dest = _dest(m.name)
    try:
        if m.issym() or m.islnk():
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.lexists(dest):
                os.remove(dest)
            os.symlink(m.linkname, dest)
        elif m.isdir():
            os.makedirs(dest, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            src = t.extractfile(m)
            assert src is not None
            with src, open(dest, "wb") as out:
                out.write(src.read())
            os.chmod(dest, m.mode)
    except OSError as e:
        sys.exit("freebsd_sysroot: %s: %s" % (dest, e))


def _unpack_base(archive):
    with tarfile.open(archive) as t:
        for m in t.getmembers():
            name = m.name.lstrip("./")
            if not any(name == p or name.startswith(p + "/")
                       for p in BASE_PATHS):
                continue
            if not (m.isfile() or m.issym() or m.islnk() or m.isdir()):
                continue
            _place(m, t)


def _closure(index):
    """Every package `PACKAGES` needs, by name, from the repository's index."""
    with tarfile.open(index, "r:zst") as t:
        manifest = t.extractfile("packagesite.yaml")
        assert manifest is not None
        text = manifest.read().decode()
    known = {}
    try:
        for line in text.splitlines():
            d = json.loads(line)
            known[d["name"]] = d
    except ValueError as e:
        sys.exit("freebsd_sysroot: the index is not readable: %s" % e)
    seen, stack = {}, list(PACKAGES)
    while stack:
        n = stack.pop()
        if n in seen:
            continue
        if n not in known:
            sys.exit("freebsd_sysroot: the repository has no %s" % n)
        seen[n] = known[n]
        stack += list(known[n].get("deps", {}))
    return seen


def _unpack_package(archive):
    """A package's files, at the target's paths under the sysroot. Its
    metadata -- the +MANIFEST and the like -- is not a file of the tree."""
    with tarfile.open(archive, "r:zst") as t:
        for m in t.getmembers():
            if not m.isfile() or m.name.lstrip("./").startswith("+"):
                continue
            _place(m, t)


def fetch():
    try:
        os.makedirs(CACHE, exist_ok=True)
        os.makedirs(ROOT, exist_ok=True)
    except OSError as e:
        sys.exit("freebsd_sysroot: %s: %s" % (HOME, e))
    base = os.path.join(CACHE, BASE)
    _get("%s/%s" % (RELEASE, BASE), base, BASE_SHA256)
    index = os.path.join(CACHE, INDEX)
    _get("%s/%s" % (REPO, INDEX), index, INDEX_SHA256)
    _unpack_base(base)
    for name, d in sorted(_closure(index).items()):
        archive = os.path.join(CACHE, os.path.basename(d["path"]))
        _get("%s/%s" % (REPO, d["path"]), archive, d["sum"])
        print("freebsd_sysroot: %s %s" % (name, d["version"]),
              file=sys.stderr, flush=True)
        _unpack_package(archive)
    if not ready():
        sys.exit("freebsd_sysroot: %s is not whole after unpacking" % ROOT)


def main():
    ap = argparse.ArgumentParser(
        description="The FreeBSD sysroot the freebsd tree is built against")
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--path", action="store_true")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args()
    if args.fetch or (args.path and not ready()):
        fetch()
    if args.path:
        print(ROOT)
        return 0
    print("FreeBSD %s %s: %s" % (VERSION, ABI,
                                 ROOT if ready() else "not fetched -- --fetch"))
    return 0 if ready() else 1


if __name__ == "__main__":
    sys.exit(main())
