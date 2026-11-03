#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Lay the guests' windows out on this desktop: the two machines' four
screens in a grid on one monitor, no two overlapping.

    python3 tools/drive/vbox_tile.py              # win10 above, win11 below
    python3 tools/drive/vbox_tile.py --monitor HDMI-1 win10 win11

A row a machine, in the order named (FNL_DRIVE_VM's by default); its first
screen on the left and its second right against it, as the guest lays them
out. The windows are scaled (GUI/Scale), and VirtualBox stretches a guest's
screen to whatever its window is, so each window takes its screen's aspect
and every window is as tall as the work area's share less mutter's title
bar allows -- one size for all four, whatever each guest's resolution --
and a row narrower where its two screens would not fit across.

Moved by _NET_MOVERESIZE_WINDOW, the request a pager sends -- no pointer
and no key, so nothing of the guests is touched -- and written to each
machine's GUI/LastScaleWindowPosition<n> too, which the VirtualBox window
reads when it opens: a machine started later opens where this put it. That
key VirtualBox rewrites on every close, so the place is kept under
PLACE_KEY as well, which nothing else writes. Each
screen's measured size is kept on the machine as well (SIZE_KEY), so a
machine that is off -- nothing to measure -- is laid out by what it last
showed and opens unstretched; one never measured is given its row's full
cells. vbox_up.py tiles again once the guests are up, which takes in a
guest whose resolution changed while it was off.

The monitor is the primary unless --monitor names one, by its RandR name.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

from Xlib import X, display
from Xlib.protocol import event

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import environment as B  # noqa: E402
from vbox_driver import VMS  # noqa: E402

# _NET_MOVERESIZE_WINDOW's first word: StaticGravity, so the numbers are the
# client's own rectangle, all four given, and from a pager.
MOVERESIZE_FLAGS = 10 | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (2 << 12)

# The guest's screen <n> as last measured, "WxH": ours, not VirtualBox's.
SIZE_KEY = "fenestal/GuestScreenSize%d"

# Where this put screen <n>'s window, "x,y,w,h". GUI/LastScaleWindowPosition
# is written as well, for the window to open there, but VirtualBox writes
# that one itself whenever the window closes, wherever it then was;
# vbox_up.py holds the windows to this one and copies it back before a start.
PLACE_KEY = "fenestal/WindowPosition%d"


def guest_windows(d):
    """{(machine, screen): window} for every VirtualBox machine window."""
    root = d.screen().root
    name_atom = d.intern_atom("_NET_WM_NAME")
    utf8 = d.intern_atom("UTF8_STRING")
    found = {}

    def walk(w):
        try:
            cls = w.get_wm_class()
            if cls and cls[1] == "VirtualBox Machine":
                n = w.get_full_property(name_atom, utf8)
                title = n.value.decode() if n else ""
                # "win10 [Running] - Oracle VirtualBox : 2"
                if " : " in title:
                    vm = title.split(" [", 1)[0]
                    found[(vm, int(title.rsplit(" : ", 1)[1]))] = w
            for c in w.query_tree().children:
                walk(c)
        except Exception:
            pass

    walk(root)
    return found


def measure(vm, screen):
    """The guest's screen `screen` (1 or 2) as (w, h), or None when the
    machine is not running. Read off a screenshot's PNG header: showvminfo
    reports the first screen's mode only."""
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "s.png")
        got = subprocess.run(["VBoxManage", "controlvm", vm, "screenshotpng",
                              path, str(screen - 1)], capture_output=True)
        if got.returncode != 0 or not os.path.exists(path):
            return None
        with open(path, "rb") as f:
            head = f.read(24)
    w, h = struct.unpack(">II", head[16:24])
    return (w, h) if w and h else None


def guest_size(vm, screen):
    """measure(), kept on the machine under SIZE_KEY; for a machine that is
    off, what was kept, or None."""
    key = SIZE_KEY % screen
    got = measure(vm, screen)
    if got:
        subprocess.run(["VBoxManage", "setextradata", vm, key,
                        "%dx%d" % got], capture_output=True)
        return got
    out = subprocess.run(["VBoxManage", "getextradata", vm, key],
                         capture_output=True, text=True).stdout
    try:
        w, h = out.split(":", 1)[1].strip().split("x")
        return int(w), int(h)
    except (IndexError, ValueError):
        return None


def title_height(d, w):
    ext = w.get_full_property(d.intern_atom("_NET_FRAME_EXTENTS"),
                              X.AnyPropertyType)
    return ext.value[2] if ext else 0


def work_area(d, mx, my, mw, mh):
    """The monitor's part of the work area: mutter's per-monitor list
    (_GTK_WORKAREAS_D0), which knows the dock as well as the top bar, and
    otherwise the desktop's one rectangle cut to the monitor."""
    root = d.screen().root
    per = root.get_full_property(d.intern_atom("_GTK_WORKAREAS_D0"),
                                 X.AnyPropertyType)
    if per:
        v = list(per.value)
        for i in range(0, len(v) - 3, 4):
            x, y, w, h = v[i:i + 4]
            if mx <= x < mx + mw and my <= y < my + mh:
                return x, y, w, h
    wa = root.get_full_property(d.intern_atom("_NET_WORKAREA"),
                                X.AnyPropertyType)
    if not wa:
        return mx, my, mw, mh
    x0, y0 = max(mx, wa.value[0]), max(my, wa.value[1])
    x1 = min(mx + mw, wa.value[0] + wa.value[2])
    y1 = min(my + mh, wa.value[1] + wa.value[3])
    return x0, y0, x1 - x0, y1 - y0


def place(d, w, x, y, cw, ch):
    root = d.screen().root
    ev = event.ClientMessage(
        window=w, client_type=d.intern_atom("_NET_MOVERESIZE_WINDOW"),
        data=(32, [MOVERESIZE_FLAGS, x, y, cw, ch]))
    root.send_event(ev, event_mask=X.SubstructureRedirectMask |
                    X.SubstructureNotifyMask)
    d.flush()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("machines", nargs="*", default=list(VMS))
    ap.add_argument("--monitor", default=None)
    args = ap.parse_args()

    mons = B.monitors(refresh=True)
    mon = next((m for m in mons if m[0] == args.monitor), None) \
        if args.monitor else next((m for m in mons if m[1]), mons[0])
    if not mon:
        sys.exit("no monitor %r here" % args.monitor)
    name, _pri, mx, my, mw, mh = mon
    d = display.Display()
    mx, my, mw, mh = work_area(d, mx, my, mw, mh)

    wins = guest_windows(d)
    rows = len(args.machines)
    cell_w, cell_h = mw // 2, mh // rows
    for r, vm in enumerate(args.machines):
        title = max([title_height(d, w) for (m, _s), w in wins.items()
                     if m == vm] or [37])
        avail_h = cell_h - title
        sizes = [guest_size(vm, s) or (cell_w, avail_h) for s in (1, 2)]
        # Every window as tall as the row, whatever its screen's resolution,
        # so that the four read alike; all narrower together when the two
        # side by side would be wider than the monitor.
        across = sum(w * avail_h / h for w, h in sizes)
        row_h = avail_h * min(1.0, mw / across)
        x = mx
        for s in (1, 2):
            w = wins.get((vm, s))
            gw, gh = sizes[s - 1]
            cw, ch = int(gw * row_h / gh), int(row_h)
            y = my + r * cell_h + title
            # Remembered whether the window is open or not.
            for key in (PLACE_KEY % s, "GUI/LastScaleWindowPosition"
                        + ("" if s == 1 else "1")):
                subprocess.run(["VBoxManage", "setextradata", vm, key,
                                "%d,%d,%d,%d" % (x, y, cw, ch)],
                               capture_output=True)
            if w:
                place(d, w, x, y, cw, ch)
            print("%s screen %d: %dx%d at %d,%d%s" % (
                vm, s, cw, ch, x, y, "" if w else " (not open; remembered)"))
            x += cw
    print("on %s, %d rows" % (name, rows))


if __name__ == "__main__":
    main()
