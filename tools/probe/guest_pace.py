#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""pace.py's sweep, put to the VirtualBox guest.

    tools/probe/guest_pace.py [--rates 0,5,15,...]

fenestal-dev.exe is copied in and run in its pace mode to its own exit once
per rate, and each cell is the presents a second while it drove, off the
count it prints as it stops -- pace.py's DRIVE_LINE, read as pace.py reads
it, and never the frame lines, which are the settle's (pace.frame_rate()).
Beside it the mode's last word: the requests made and the present mode it
ended on. There is no CPU figure: nothing on this side can read the
guest's.

The guest draws with lavapipe, whose present waits for no display, and the
library leaves its hold and spacing off on a software device. So this says
how fast an empty scene is drawn and resized there, never whether pacing
holds the display rate -- which is why pace.py leaves the guest out. The
figures also move with the size of the guest's desktop; see DESIGN.md.

The program is the win64 tree's, told to draw with Vulkan (FNL_RENDERER) as
the win64-vulkan-vm round's is.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "drive"))
import vbox_driver as V  # noqa: E402
from pace import DRIVE_LINE  # noqa: E402

EXE = os.path.join(ROOT, "build", "win64", "release", "fenestal-dev.exe")
ENV = {"FNL_RENDERER": "vulkan"}
RATES = (0, 5, 15, 30, 45, 60, 250)
SECONDS = 12
SETTLE = 4


def run_rate(there, rate):
    """One cell: the text to print for it."""
    try:
        p = subprocess.run(
            V.run_argv(there, ["pace", str(rate), str(SECONDS),
                               "--settle", str(SETTLE)], ENV),
            capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "timed out"
    out = (p.stdout + p.stderr).splitlines()
    drove = [m for m in map(DRIVE_LINE.match, out)
             if m and float(m.group(2)) > 0]
    if not drove:
        return "no drive count (rc %s)" % p.returncode
    m = drove[-1]
    asked = [line for line in out
             if line.startswith("pace: ") and " requests in " in line]
    return "%5.1f presents/s  %s" % (
        int(m.group(1)) / float(m.group(2)),
        asked[-1][len("pace: "):] if asked else "no request count")


def main():
    rates = RATES
    argv = sys.argv[1:]
    if len(argv) == 2 and argv[0] == "--rates":
        rates = tuple(int(r) for r in argv[1].split(","))
    elif argv:
        sys.exit(__doc__)
    if not os.path.exists(EXE):
        sys.exit("no %s; build win64 first" % os.path.relpath(EXE, ROOT))

    V.manage(*V.guest_argv("mkdir", "--parents", V.GUEST_DIR)[1:])
    copied = V.manage(*V.guest_argv("copyto", "--target-directory",
                                    V.GUEST_DIR + "\\", EXE)[1:], timeout=120)
    if copied.returncode:
        sys.exit("copy failed: " + copied.stderr)
    there = V.GUEST_DIR + "\\" + os.path.basename(EXE)

    print("%-8s%s" % ("rate", "win64-vulkan-vm"))
    for rate in rates:
        print("%-8s%s" % ("idle" if rate == 0 else rate,
                          run_rate(there, rate)), flush=True)


if __name__ == "__main__":
    main()
