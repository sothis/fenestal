#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Capture tools/probe/dissolve's window on X11 through a move and print
what each capture shows: the mean red and blue of the presentable area,
the brightest column of the walking bar, and whether the frame is black.
A dissolve reads as red giving way to blue over the transition's length
with the bar still walking; a cut as one step; a gap as black or missing
frames.

    FNL_BACKEND=x11 ./dissolve 2 1000 &
    python3 tools/probe/dissolve.py [seconds]

Captures with `import -window`, which XWayland allows and the harness uses
for a look at a frame; a capture can lag a frame, so read the trend, not
one row (CLAUDE.md, *Running and driving*)."""
import io
import re
import subprocess
import sys
import time

from PIL import Image, ImageStat


def window_id(name):
    out = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True,
                         text=True, check=True).stdout
    for line in out.splitlines():
        if '"%s"' % name in line:
            m = re.match(r'\s*(0x[0-9a-f]+)', line)
            if m:
                return m.group(1)
    return None


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
    wid = None
    for _ in range(100):
        wid = window_id("dissolve")
        if wid:
            break
        time.sleep(0.1)
    if not wid:
        sys.exit("no window named dissolve")
    t0 = time.monotonic()
    print("   t     red   green    blue   bar  note")
    last = None
    n_rows = 0
    misses = 0
    while time.monotonic() - t0 < seconds:
        t = time.monotonic() - t0
        try:
            r = subprocess.run(["import", "-window", wid, "png:-"],
                               capture_output=True, timeout=2.0)
        except subprocess.TimeoutExpired:
            r = None
        if r is None or r.returncode != 0 or not r.stdout:
            print("%6.3f  (no capture)" % t)
            misses += 1
            if misses >= 3 and not window_id("dissolve"):
                break
            continue
        misses = 0
        im = Image.open(io.BytesIO(r.stdout)).convert("RGB")
        w, h = im.size
        # The presentable area is under the titlebar: the lower three
        # quarters, inset from the edges, so the bar's row and the text are
        # in it and the frame's outline is not.
        area = im.crop((8, h // 4, w - 8, h - 8))
        red, green, blue = ImageStat.Stat(area).mean
        # Where the walking bar is: the brightest column of the area's
        # middle band. A capture that repeats it is a stale one.
        band = area.crop((0, area.height // 2 - 4, area.width,
                          area.height // 2 + 4)).convert("L")
        cols = [sum(band.getpixel((x, y)) for y in range(band.height))
                for x in range(band.width)]
        bar = cols.index(max(cols))
        note = ""
        if red + green + blue < 12:
            note = "BLACK"
        elif last is not None and abs(red - last[0]) + abs(blue - last[2]) > 3:
            note = "changing"
        print("%6.3f  %6.1f  %6.1f  %6.1f  %4d  %s"
              % (t, red, green, blue, bar, note), flush=True)
        last = (red, green, blue)
        n_rows += 1
    print("%d captures in %.1f s" % (n_rows, time.monotonic() - t0))


if __name__ == "__main__":
    main()
