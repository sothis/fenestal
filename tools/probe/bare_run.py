#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Run one Linux program on the bare X server's machine and print what it says.

    tools/probe/bare_run.py <exe> <seconds> [--kms | --weston]
                            [--driver nouveau|nvidia]
                            [--env NAME=VALUE]... [args...]

native_run.py, for the machine with nobody managing its X server: the
program is copied over, started on that server -- which is started first if
it is down -- and sent SIGTERM after <seconds>, or left to end sooner by
itself. Its output comes back here as it is written, stdout and stderr,
and its exit code is this script's. The machine is the first
FNL_DRIVE_BARE names, or else the first tools/drive/bare_driver.py --setup
wrote down.

--kms stops every server there instead and tells the program to open KMS,
as a KMS round does; --weston starts weston there instead of X, as a
weston round does, and gives the program its socket -- the way
tools/probe/heldpresent.c is run; --driver moves the machine's NVIDIA GPU
to that kernel driver first (tools/gpu-driver.sh, as root), as
`kms-vulkan-bare@m:nouveau` does -- the way tools/probe/drivermove.c is
run.

A sanitizer tree's program takes its runtime with it, as in a round, and its
tree and variant are read off the path: build/<target>/<variant>/<program>.
A tree another processor takes the place of there is swapped for that one
(bare_driver.TREES): build/linux/... names linux-arm64's program on a Pi.
The server started is X, so a Linux program is told to open X11
(FNL_BACKEND=x11) unless --env names another, as a round's would be;
under --weston, Wayland.
"""
import os
import shlex
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))
import bare_driver as D  # noqa: E402


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        sys.exit(__doc__)
    exe, seconds, rest = argv[0], float(argv[1]), argv[2:]
    env = {}
    kms = weston = False
    driver = None
    while rest and rest[0] in ("--env", "--kms", "--weston", "--driver"):
        if rest[0] in ("--kms", "--weston"):
            kms = kms or rest[0] == "--kms"
            weston = weston or rest[0] == "--weston"
            rest = rest[1:]
            continue
        if len(rest) < 2:
            sys.exit(__doc__)
        if rest[0] == "--driver":
            driver = rest[1]
        else:
            name, _, value = rest[1].partition("=")
            env[name] = value
        rest = rest[2:]
    if not D.hosts():
        sys.exit("no machine is named: FNL_DRIVE_BARE, or bare_driver.py "
                 "--setup <name> <user@address>")
    try:
        box = D.Bare(D.hosts()[0], x=not kms and not weston)
    except D.Unreachable as e:
        sys.exit(str(e))
    built = os.path.dirname(os.path.abspath(exe))
    variant = os.path.basename(built)
    tree = os.path.basename(os.path.dirname(built))
    try:
        mine = box.tree(tree)
    except D.Unreachable:
        mine = tree     # a tree TREES does not name goes as it is
    if mine != tree:
        tree = mine
        exe = os.path.join(os.path.dirname(os.path.dirname(built)), tree,
                           variant, os.path.basename(exe))
        if not os.path.exists(exe):
            box.close()
            sys.exit("no %s -- ./build.sh %s %s"
                     % (os.path.relpath(exe), tree, variant))
    if kms and weston:
        box.close()
        sys.exit("--kms and --weston are two servers' ways; one of them")
    for name, value in D.machine_env(D.hosts()[0]).items():
        env.setdefault(name, value)
    if not exe.endswith(".exe"):
        env.setdefault("FNL_BACKEND", "kms" if kms
                       else "wayland" if weston else "x11")
    try:
        if kms:
            box.bare_display()
            if driver:
                box.gpu_driver(driver)
        elif weston:
            box.weston_up()
            for name, value in box.wenv().items():
                env.setdefault(name, value)
        else:
            box.x_up()
        there = box.install(exe, variant, tree)
        # Straight through to this terminal, both streams, as native_run.py
        # passes them: heldpresent's log is its stdout.
        proc = subprocess.Popen(box.run_argv(there, rest, env))
        try:
            return proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            # By its own name, as pkill knows it: fifteen characters.
            name = os.path.basename(exe)[:15]
            box.sh("pkill -x %s" % shlex.quote(name), check=False)
            try:
                return proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                # The ssh, then, which a program that ignored the signal
                # keeps open.
                proc.kill()
                return proc.wait()
    except (D.Unreachable, D.DriverMoveFailed) as e:
        sys.exit(str(e))
    finally:
        box.close()


if __name__ == "__main__":
    sys.exit(main())
