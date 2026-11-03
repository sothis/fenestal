#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Whether the testbed's text survives its GPU runtime-suspending, on a bare
machine's weston.

    FNL_DRIVE_BARE=schenker-live tools/probe/nvk_idle.py <tag> [seconds]

A capture, an idle long enough for nouveau to power the GPU off (5 s by
its autosuspend delay; 20 by default here), a frame drawn after the
resume, a capture; then the keybinding panel's pixels compared between the
two, found in each by the green line under the titlebar, since weston
places the window anew each run. 0 is the atlas whole; DESIGN *The startup
inventory* has 5,392 of 61,200 with the atlas in VRAM on schenker's GTX
1050 Ti, and 0 since it is kept out of it.

The device is FNL_GPU's (nvidia by default); FNL_FAULT and the variant
(FNL_PROBE_VARIANT, release by default) reach the program as a round's do,
so `FNL_FAULT=atlas-out-of-vram FNL_PROBE_VARIANT=asan` forces the
fallback. The captures are build/drive/idle-<tag>-0.png and -1.png.
"""
import os
import sys
import time

from PIL import Image, ImageChops

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))
import environment as B  # noqa: E402
from testbed import OUT  # noqa: E402

# schenker's NVIDIA GPU; the one whose runtime PM state is printed.
PCI = os.environ.get("FNL_PROBE_PCI", "0000:01:00.0")


def origin(im):
    """Where the window's green titlebar rule starts: 400 green pixels in
    a row, which nothing else on that desktop has."""
    px = im.load()
    for y in range(im.height):
        run = 0
        for x in range(im.width):
            r, g, b = px[x, y]
            run = run + 1 if g > 150 and r < 120 and b < 150 else 0
            if run == 400:
                return x - 399, y
    raise SystemExit("no window in the capture")


def panel(path):
    """The keybinding panel, which no frame changes, off a capture."""
    im = Image.open(path).convert("RGB")
    x, y = origin(im)
    return im.crop((x + 8, y + 48, x + 248, y + 303))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tag = sys.argv[1]
    idle = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    env = {"FNL_GPU": os.environ.get("FNL_GPU", "nvidia")}
    if os.environ.get("FNL_FAULT"):
        env["FNL_FAULT"] = os.environ["FNL_FAULT"]
    b = B.WaylandBare.each()[0]
    b.start(os.environ.get("FNL_PROBE_VARIANT", "release"), env=env)

    def pm():
        return b.box.sh("cat /sys/bus/pci/devices/%s/power/runtime_status"
                        % PCI, check=False).strip()

    shots = [os.path.join(OUT, "idle-%s-%d.png" % (tag, i)) for i in (0, 1)]
    try:
        for line in b.dump(40):
            if "GPU" in line or "atlas" in line or "FAULT" in line:
                print(" ", line)
        # A frame drawn now: the pointer over the scene and back.
        b.move_to(200, 200)
        b.quiet(0.5)
        b.move_to(400, 300)
        b.quiet(1.0)
        b.capture(shots[0])
        for i in range(idle):
            time.sleep(1)
            if i % 5 == 4:
                print("  %2d s: GPU %s" % (i + 1, pm()), flush=True)
        # And one after the resume, so the text is sampled again.
        b.move_to(200, 200)
        b.quiet(0.5)
        b.move_to(400, 300)
        b.quiet(1.0)
        b.capture(shots[1])
    finally:
        b.stop()
        b.close()
    a, c = panel(shots[0]), panel(shots[1])
    d = ImageChops.difference(a, c)
    n = sum(1 for p in d.get_flattened_data() if max(p) > 24)
    print("%s: keybinding pixels changed after %d s idle: %d of %d"
          % (tag, idle, n, d.width * d.height))
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
