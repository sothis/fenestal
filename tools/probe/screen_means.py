#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""On a bare machine, as root: what its display is scanning out, sampled
as fast as tools/drive/kms_grab.py (beside this file, or in tools/drive/
when run from the tree) can read it, each
capture one line -- the time, the mean red, green and blue of the frame
and the brightest column of its middle band -- for a look at what the
screen shows across a move (DESIGN *The veil*). A black frame says so.
Standard library only, like kms_grab.py: the machine has nothing of this
tree but the two files.

    python3 screen_means.py <seconds> [--output eDP-1]
"""
import os
import subprocess
import sys
import time


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
    here = os.path.dirname(os.path.abspath(__file__))
    grab_py = os.path.join(here, 'kms_grab.py')
    if not os.path.exists(grab_py):
        # From the tree, beside this script is not where it lives. On the
        # machine both files are copied together, and it is.
        grab_py = os.path.join(here, '..', 'drive', 'kms_grab.py')
    if not os.path.exists(grab_py):
        sys.exit('screen_means.py: no kms_grab.py beside this script or in '
                 'tools/drive')
    grab = [sys.executable, grab_py] + sys.argv[2:]
    t0 = time.monotonic()
    print('   t     red   green    blue   bar  note', flush=True)
    last = None
    decoded = 0
    while time.monotonic() - t0 < seconds:
        t = time.monotonic() - t0
        r = subprocess.run(grab, capture_output=True)
        if r.returncode != 0 or not r.stdout.startswith(b'P6'):
            print('%6.3f  (no capture: %s)'
                  % (t, r.stderr.decode(errors='replace').strip()[-60:]),
                  flush=True)
            continue
        # The PPM header kms_grab.py writes, a line each: P6, width height,
        # maxval; then the bytes. Split by line and not by whitespace, which
        # would take a first byte of 9-13 or 32 for part of the header.
        parts = r.stdout.split(b'\n', 3)
        w, h = map(int, parts[1].split())
        px = parts[3][:w * h * 3]
        # Every 8th pixel of every 8th row, which is plenty for a mean.
        rs = gs = bs = 0
        n = 0
        for y in range(0, h, 8):
            row = px[y * w * 3:(y + 1) * w * 3]
            rs += sum(row[0::24])
            gs += sum(row[1::24])
            bs += sum(row[2::24])
            n += len(row[0::24])
        red, green, blue = rs / n, gs / n, bs / n
        # The brightest column of the middle band, in steps of 8 columns.
        mid = px[(h // 2) * w * 3:(h // 2 + 1) * w * 3]
        cols = [mid[x * 3] + mid[x * 3 + 1] + mid[x * 3 + 2]
                for x in range(0, w, 8)]
        bar = cols.index(max(cols)) * 8
        note = ''
        if red + green + blue < 12:
            note = 'BLACK'
        elif last and abs(red - last[0]) + abs(blue - last[2]) > 3:
            note = 'changing'
        print('%6.3f  %6.1f  %6.1f  %6.1f  %4d  %s'
              % (t, red, green, blue, bar, note), flush=True)
        last = (red, green, blue)
        decoded += 1
    # Nothing read is not a run that saw a black screen: it is no run at all,
    # and says so to whoever started it.
    if not decoded:
        sys.exit('screen_means.py: no capture was read')


if __name__ == '__main__':
    main()
