#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Every layout and variant rules/evdev.lst names, read two ways, and each
key the two readings disagree on.

    tools/probe/xkbdiff.py [--dump ./keymapdump] [--root /usr/share/X11/xkb]
                           [--verbose] [layout[(variant)] ...]

keymapdump (tools/probe/keymapdump.c, built by hand as its header says)
prints the keymap the KMS backend applies for a layout. This runs it on
the files, the way the backend reads them, and on the tool's text --
`xkbcli compile-keymap` where it is installed (Debian's
libxkbcommon-tools, Arch's libxkbcommon), else `setxkbmap -print` through
`xkbcomp -xkb`, which wants a DISPLAY -- and prints one line per pair that
differs, "de(nodeadkeys): 3 keys differ", with each key under --verbose
as code/group/level, the files' keysym against the tool's. Named pairs
alone when any are given. The last line counts the pairs and those that
differ; the exit status is 1 when any does. The data root is
XKB_CONFIG_ROOT, else /usr/share/X11/xkb.
"""
import os
import shutil
import subprocess
import sys


def pairs(lst):
    """(layout, variant) for every layout, and for every variant of one."""
    out, section = [], None
    with open(lst, encoding="utf-8") as f:
        for line in f:
            if line.startswith("!"):
                section = line[1:].strip()
                continue
            words = line.split()
            if not words:
                continue
            if section == "layout":
                out.append((words[0], ""))
            elif section == "variant" and len(words) > 1 \
                    and words[1].endswith(":"):
                out.append((words[1][:-1], words[0]))
    return out


def reference_text(layout, variant, env):
    """The compiled keymap's text from the tool this machine has, xkbcli
    reading the same data root as keymapdump."""
    if shutil.which("xkbcli"):
        cmd = ["xkbcli", "compile-keymap", "--layout", layout]
        if variant:
            cmd += ["--variant", variant]
        return subprocess.run(cmd, capture_output=True, text=True,
                              env=env).stdout
    names = ["setxkbmap", "-print", "-rules", "evdev", "-model", "pc105",
             "-layout", layout]
    if variant:
        names += ["-variant", variant]
    block = subprocess.run(names, capture_output=True, text=True).stdout
    if not block:
        return ""
    return subprocess.run(["xkbcomp", "-xkb", "-", "-"], input=block,
                          capture_output=True, text=True).stdout


def table(lines):
    """(code, group, level) -> (what it types, the keysym), levels that
    type nothing left out: a modifier spelled two ways is no difference."""
    out = {}
    for line in lines.splitlines():
        code, group, level, sym, typed = line.split()
        if int(typed, 16):
            out[(int(code), int(group), int(level))] = (int(typed, 16),
                                                        int(sym, 16))
    return out


def main():
    argv = sys.argv[1:]
    dump, root, verbose, named = "./keymapdump", None, False, []
    while argv:
        a = argv.pop(0)
        if a == "--dump":
            dump = argv.pop(0)
        elif a == "--root":
            root = argv.pop(0)
        elif a == "--verbose":
            verbose = True
        elif a in ("-h", "--help"):
            sys.exit(__doc__)
        else:
            layout, _, variant = a.partition("(")
            named.append((layout, variant.rstrip(")")))
    root = root or os.environ.get("XKB_CONFIG_ROOT") or "/usr/share/X11/xkb"
    env = dict(os.environ, XKB_CONFIG_ROOT=root)
    if not shutil.which("xkbcli") and not (shutil.which("setxkbmap")
                                           and shutil.which("xkbcomp")):
        sys.exit("xkbdiff: neither xkbcli nor setxkbmap with xkbcomp is "
                 "installed; nothing to compare against")

    todo = named or pairs(os.path.join(root, "rules", "evdev.lst"))
    differ = 0
    for layout, variant in todo:
        label = "%s(%s)" % (layout, variant) if variant else layout
        ours = subprocess.run([dump, layout, variant], env=env,
                              capture_output=True, text=True)
        text = reference_text(layout, variant, env)
        theirs = subprocess.run([dump, "--text"], input=text, env=env,
                                capture_output=True, text=True)
        if ours.returncode and theirs.returncode:
            # A layout the data lists and has no file for: neither reads it.
            if verbose:
                print("%s: neither reads it" % label)
            continue
        if ours.returncode or theirs.returncode:
            print("%s: files %s; tool %s" % (
                label, ours.stderr.strip() or "read",
                theirs.stderr.strip() or ("read" if text else "no text")))
            differ += 1
            continue
        a, b = table(ours.stdout), table(theirs.stdout)
        keys = sorted(k for k in set(a) | set(b)
                      if a.get(k, (0,))[0] != b.get(k, (0,))[0])
        if keys:
            differ += 1
            print("%s: %d keys differ" % (label, len(keys)))
            if verbose:
                for k in keys:
                    print("  %3d g%d l%d  files 0x%x  tool 0x%x" % (
                        k[0], k[1] + 1, k[2] + 1, a.get(k, (0, 0))[1],
                        b.get(k, (0, 0))[1]))
    print("%d pairs, %d differ" % (len(todo), differ))
    sys.exit(1 if differ else 0)


if __name__ == "__main__":
    main()
