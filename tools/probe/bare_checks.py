#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Run a tree's own checks on a bare machine: verifykeymap and the fuzz
harnesses, which a native build runs as it builds and a cross build cannot.

    tools/probe/bare_checks.py [build/<target>/<variant>] [machine]

The tree is build/linux-arm64/release by default, and the machine the first
FNL_DRIVE_BARE names, or else the first bare-hosts line. A tree another
processor takes the place of there is swapped for that one, as in a round
(bare_driver.TREES). The commands are the tree's own generated/checks.txt,
written by CMake with the arguments and counts the build runs them with;
the corpus goes with them, under the directory they are run from.

A check fails when it exits with anything but 0, or when a checker in it
says so -- UBSan's `runtime error:` among them, which reports and goes on:
the build's runs exit 0 past one, so this is the stricter reading. The
reason to run them on another processor at all is what differs there:
plain `char` is unsigned on 64-bit ARM, and the layout's parser reads text
a byte at a time. Each run is a ledger line (instrument `checks`).
"""
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "drive"))
import bare_driver as D  # noqa: E402
import ledger  # noqa: E402
from testbed import ROOT  # noqa: E402

CORPUS = os.path.join(ROOT, "tools", "fuzz", "corpus")
# What a checker prints that is a failure whatever the exit code says.
SAID = re.compile(r"runtime error:|ERROR: AddressSanitizer|"
                  r"ERROR: LeakSanitizer|WARNING: ThreadSanitizer")


def main():
    args = sys.argv[1:]
    built = os.path.abspath(args[0] if args else
                            os.path.join(ROOT, "build", "linux-arm64",
                                         "release"))
    names = D.hosts()
    machine = args[1] if len(args) > 1 else (names[0] if names else None)
    if not machine:
        sys.exit("no machine is named: FNL_DRIVE_BARE, or bare_driver.py "
                 "--setup <name> <user@address>")
    variant = os.path.basename(built)
    tree = os.path.basename(os.path.dirname(built))
    try:
        box = D.Bare(machine, x=False)
    except D.Unreachable as e:
        sys.exit(str(e))
    try:
        try:
            mine = box.tree(tree)
        except D.Unreachable:
            mine = tree
        built = os.path.join(os.path.dirname(os.path.dirname(built)), mine,
                             variant)
        listed = os.path.join(built, "generated", "checks.txt")
        if not os.path.exists(listed):
            sys.exit("no %s -- ./build.sh %s %s"
                     % (os.path.relpath(listed, ROOT), mine, variant))
        with open(listed) as f:
            commands = [ln.split() for ln in f if ln.strip()]
        print("%s, %s: %s/%s, %d checks"
              % (machine, box.arch, mine, variant, len(commands)),
              flush=True)

        there = None
        for argv in commands:
            there = box.install(os.path.join(built, argv[0]), variant,
                                "%s-checks" % mine)
        where = os.path.dirname(there)
        res = subprocess.run(
            ["scp", "-q", "-r", "-o", "ControlPath=" + box.ctl] + D.SSH_OPTS
            + [CORPUS, "%s:%s/" % (box.dest, where)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=120)
        if res.returncode != 0:
            sys.exit("copying the corpus: %s" % res.stderr.strip())

        env = ["LSAN_OPTIONS=suppressions=%s/lsan.supp" % where,
               "TSAN_OPTIONS=suppressions=%s/tsan.supp" % where]
        if box.runtimes:
            env.append("LD_LIBRARY_PATH=%s" % where)
        failed = []
        for argv in commands:
            command = "cd %s && env %s ./%s" % (
                shlex.quote(where), " ".join(map(shlex.quote, env)),
                " ".join(map(shlex.quote, argv)))
            run = D._ssh(box.dest, command, timeout=1800, ctl=box.ctl)
            out = (D._text(run) + run.stderr.decode("utf-8", "replace"))
            said = SAID.search(out)
            ok = run.returncode == 0 and not said
            last = out.strip().splitlines()[-1:] or ["(nothing)"]
            print("%-40s %s  %s" % (" ".join(argv)[:40],
                                    "ok  " if ok else "FAIL", last[0][:60]),
                  flush=True)
            if not ok:
                failed.append(argv[0])
                print("\n".join("    " + ln for ln in
                                out.strip().splitlines()[-30:]), flush=True)
        print("%d checks, %s" % (len(commands), "%d failed" % len(failed)
                                 if failed else "none failed"), flush=True)
        ledger.record("checks", "checks-bare@%s" % machine, variant,
                      checks=len(commands), failed=len(failed), tree=mine)
        return 1 if failed else 0
    except D.Unreachable as e:
        sys.exit(str(e))
    finally:
        box.close()


if __name__ == "__main__":
    sys.exit(main())
