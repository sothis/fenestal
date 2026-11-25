#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Join the two macOS trees into one of universal binaries.

    tools/macos_universal.py [--variant release] [--out DIR]

`build/macos/<variant>` and `build/macos-arm64/<variant>` are one
architecture each, because the archive is made from one module of bitcode
(DESIGN *Cross-compiling for macOS*). This copies the x86-64 tree to
`build/macos-universal/<variant>` and replaces every library and program
with the two slices joined by lipo. The static archive is one object in
each: lipo joins the archives where it can, and otherwise the member is
joined and the archive remade.

The tree is for staging and for running on either processor; the harness
still drives `build/macos`, whose x86-64 slice the one Intel Mac here
runs. `lipo` is `FNL_LIPO`, else `llvm-lipo-21`, `llvm-lipo` or `lipo`.
A Mac's `codesign`, where it exists, signs each joined file ad hoc, which
Apple's processors require of all code.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCHS = ("x86_64", "arm64")

# What a Mach-O file starts with: thin and fat, either byte order.
MACHO = {
    b"\xfe\xed\xfa\xce": "thin",
    b"\xce\xfa\xed\xfe": "thin",
    b"\xfe\xed\xfa\xcf": "thin",
    b"\xcf\xfa\xed\xfe": "thin",
    b"\xca\xfe\xba\xbe": "fat",
    b"\xbe\xba\xfe\xca": "fat",
    b"\xca\xfe\xba\xbf": "fat",
    b"\xbf\xba\xfe\xca": "fat",
}
AR_MAGIC = b"!<arch>\n"


def which(names):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def lipo_tool(named):
    if named:
        return named
    env = os.environ.get("FNL_LIPO")
    if env:
        return env
    return which(["llvm-lipo-21", "llvm-lipo", "lipo"])


def ar_tool():
    env = os.environ.get("FNL_AR")
    if env:
        return env
    return which(["llvm-ar-21", "llvm-ar", "ar"])


def is_macho(path):
    try:
        with open(path, "rb") as f:
            head = f.read(4)
    except OSError:
        return False
    return head in MACHO


def is_archive(path):
    try:
        with open(path, "rb") as f:
            return f.read(8) == AR_MAGIC
    except OSError:
        return False


def archs(lipo, path):
    result = subprocess.run([lipo, "-archs", path],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return []
    return result.stdout.split()


def merge_one(lipo, x86, arm, out):
    """One Mach-O file, thin in each tree, joined into the output."""
    # A plain archive's -archs answer is llvm-lipo's to crash on -- a
    # Darwin archive's __.SYMDEF member is read as an object -- so an
    # archive goes straight to the join, which lipo takes whole.
    if not is_archive(x86) and all(a in archs(lipo, x86) for a in ARCHS):
        # Already universal, as MoltenVK is: the x86-64 copy is the one to
        # keep, and lipo would refuse two of the same architectures.
        shutil.copy2(x86, out)
        return True
    tmp = out + ".new"
    result = subprocess.run([lipo, "-create", x86, arm, "-output", tmp],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return False
    got = archs(lipo, tmp)
    if not all(a in got for a in ARCHS):
        os.unlink(tmp)
        return False
    os.replace(tmp, out)
    return True


def merge_archive(lipo, ar, x86, arm, out):
    """A static archive, by joining each member and remaking the archive."""
    with tempfile.TemporaryDirectory() as tmp:
        xdir = os.path.join(tmp, "x86")
        adir = os.path.join(tmp, "arm")
        fdir = os.path.join(tmp, "fat")
        for d in (xdir, adir, fdir):
            os.makedirs(d)
        for archive, there in ((x86, xdir), (arm, adir)):
            result = subprocess.run([ar, "x", archive], cwd=there,
                                    capture_output=True, text=True)
            if result.returncode != 0:
                return False
        names = sorted(set(os.listdir(xdir)) & set(os.listdir(adir)))
        # The symbol table is the archive tool's to make again, not a member
        # to join: BSD names it __.SYMDEF, GNU's ar extracts it as "/".
        members = [n for n in names
                   if n not in ("__.SYMDEF", "SYMDEF", "/", "//") and
                   os.path.isfile(os.path.join(xdir, n)) and
                   not os.path.islink(os.path.join(xdir, n))]
        if not members:
            return False
        for name in members:
            if not merge_one(lipo, os.path.join(xdir, name),
                             os.path.join(adir, name),
                             os.path.join(fdir, name)):
                return False
        # The archive is made at its destination: /tmp and the build tree
        # need not be one filesystem, and os.replace() would fail across.
        tmp_out = out + ".new"
        result = subprocess.run([ar, "rcs", tmp_out] + members, cwd=fdir,
                                capture_output=True, text=True)
        if result.returncode != 0:
            return False
        os.replace(tmp_out, out)
    return True


def merge(lipo, ar, x86, arm, out):
    if is_archive(x86):
        if merge_one(lipo, x86, arm, out):
            return True
        return merge_archive(lipo, ar, x86, arm, out)
    return merge_one(lipo, x86, arm, out)


def deliverables(tree):
    """The files joined, as paths relative to the tree."""
    found = []
    for name in sorted(os.listdir(tree)):
        path = os.path.join(tree, name)
        if os.path.islink(path) or not os.path.isfile(path):
            continue
        if name.endswith(".a") or is_macho(path):
            found.append(name)
    snippets = os.path.join(tree, "snippets")
    if os.path.isdir(snippets):
        for name in sorted(os.listdir(snippets)):
            path = os.path.join(snippets, name)
            if os.path.isfile(path) and not os.path.islink(path) and \
                    is_macho(path):
                found.append(os.path.join("snippets", name))
    return found


def sign(path):
    codesign = shutil.which("codesign")
    if not codesign:
        return
    subprocess.run([codesign, "--force", "--sign", "-", path], check=True,
                   capture_output=True, text=True)


def main(argv):
    parser = argparse.ArgumentParser(
        description="Join the x86-64 and arm64 macOS trees into one of "
                    "universal binaries.")
    parser.add_argument("--variant", default="release",
                        help="the tree variant to join (default: release)")
    parser.add_argument("--x86", default=None,
                        help="the x86-64 tree (default: build/macos/<v>)")
    parser.add_argument("--arm", default=None,
                        help="the arm64 tree (default: build/macos-arm64/<v>)")
    parser.add_argument("--out", default=None,
                        help="the joined tree (default: "
                             "build/macos-universal/<v>)")
    parser.add_argument("--lipo", default=None, help="the lipo to run")
    args = parser.parse_args(argv)

    lipo = lipo_tool(args.lipo)
    if not lipo:
        sys.exit("macos_universal: no lipo (llvm-lipo-21, llvm-lipo, lipo)")
    ar = ar_tool()
    if not ar:
        sys.exit("macos_universal: no ar (llvm-ar-21, llvm-ar, ar)")

    x86 = args.x86 or os.path.join(ROOT, "build", "macos", args.variant)
    arm = args.arm or os.path.join(ROOT, "build", "macos-arm64",
                                   args.variant)
    out = args.out or os.path.join(ROOT, "build", "macos-universal",
                                   args.variant)
    for tree in (x86, arm):
        if not os.path.isdir(tree):
            sys.exit("macos_universal: no tree at %s" % tree)

    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(x86, out, symlinks=True)

    for name in deliverables(x86):
        there = os.path.join(arm, name)
        if not os.path.exists(there):
            sys.exit("macos_universal: %s is not in %s" % (name, arm))
        if not merge(lipo, ar, os.path.join(x86, name), there,
                     os.path.join(out, name)):
            sys.exit("macos_universal: cannot join %s" % name)
        if not name.endswith(".a"):
            sign(os.path.join(out, name))
        got = " ".join(archs(lipo, os.path.join(out, name))) or "archive"
        print("macos_universal: %-24s %s" % (name, got))

    print("macos_universal: %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
