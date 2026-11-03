# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""One stress round, run against every environment that will run here.

    python3 tools/drive/stress.py                    # every environment
    python3 tools/drive/stress.py x11-vulkan         # one of them
    python3 tools/drive/stress.py x11-gl             # the same, drawn by GL
    python3 tools/drive/stress.py wayland-gl         # Wayland, drawn by GL
    python3 tools/drive/stress.py win64-vulkan-vm    # on Windows, in a VM
    python3 tools/drive/stress.py win64-vulkan-native    # ... on a laptop
    python3 tools/drive/stress.py win64-vulkan-native@schenker:intel
    python3 tools/drive/stress.py win64-vulkan-wl    # wine, on Wayland
    python3 tools/drive/stress.py x11-vulkan-bare    # X11, no manager
    python3 tools/drive/stress.py wayland-vulkan-bare    # weston, no desktop
    python3 tools/drive/stress.py kms-vulkan-bare    # no window server
    python3 tools/drive/stress.py win64-vulkan-cage-bare  # wine there, on cage
    python3 tools/drive/stress.py --variant tsan     # under a checker
    python3 tools/drive/stress.py --capture          # opt in to pictures

The claim this library makes is that a caller sees one window, so the round
that checks it is one round: an environment covered less well than its
neighbours is a gap, and a file per environment is how a gap hides.

---- how a check knows where it applies ------------------------------------

Not by name. `if "server-rect" in b.CAN`, never `if b.NAME == "x11-vulkan"` --
see environment.py, which is where the differences actually are. Each is a fact
about the window server and not about this library:

    server-rect   where the window is on screen, and what monitor that is. A
                  Wayland client is never told, by design
    wm-props      _NET_FRAME_EXTENTS, the reparenting, WM_CLASS -- X11's own
    edge-resize   an edge the harness itself can drag
    read-pixels   the window's own pixels off the X server, no portal involved
    grab-ring     a transparent resize margin outside the window, which only
                  the Wayland backend has
    multi-output  the window server shows this client more than one monitor,
                  so a window can be moved between them and the move observed
    unminimize    a window can be brought back by the harness. A Wayland client
                  cannot ask to be unminimized and nothing outside it can be
                  asked to either, so that environment skips the section
    composited    a picture of what the *compositor* did, not of what the
                  client produced
    remap-key     the keymap is a table the harness may edit, so a dead key
                  can be made for the length of a check
    dead-layout   the system composes, and the window can be switched to a
                  layout of its own that has a dead key
    desk-layout   the window types through this desktop's layout, which the
                  harness can switch to one with a dead key and back
    input-method  the compositor has a composer of its own to type through
    resizable     the window can be a size other than its output's: opened at
                  one, maximized and restored. On a bare display the window
                  is the output, and the sections that size it are skipped
    known-layout  the window types through a layout the harness can read --
                  a bare machine's console layout, which its Xorg, weston
                  and KMS are given -- so what each key types is asserted,
                  dead keys and AltGr included, where LAYOUT_TYPES has it
    drawn-pointer the library puts the pointer on the screen itself, at
                  window point times scale from the output's origin: on a
                  bare display nothing else draws one
    display-modes the window is an output in one of its display modes, and
                  the titlebar has a list of those modes and one of the
                  card's outputs to choose from (fnl_request_mode(),
                  fnl_request_output()). Its titlebar has no minimize or
                  maximize, so the buttons are aimed at by BTN_DX_OUTPUT;
                  a window a kiosk holds at its output's size has them
    forced-hotplug a connector can be had unplugged and plugged back from
                  here (b.force_output()), as a cable would, and the
                  window has to follow
    layout-groups the layout is the program's to name, and it is given a
                  second group with a key to switch between the two

Everything else is asked of every environment, through the application's own
report -- see testbed.py.

---- and what it costs to run ----------------------------------------------

A desktop, and a one-time portal consent
(`python3 tools/drive/wayland_driver.py --consent`), which every environment
needs because the portal is the one way in. Beyond that wayland-vulkan wants
a compositor, x11-vulkan an X server and win64-vulkan wine. win64-vulkan-vm
wants none of those and a VirtualBox machine instead, and is a round for each
machine FNL_DRIVE_VM lists: see vbox_driver.py. win64-vulkan-native wants a
Windows laptop that ssh reaches, and is a round for each FNL_DRIVE_NATIVE
lists: see native_driver.py. x11-vulkan-bare wants a Linux machine with
Xorg and no window manager, and is a round for each FNL_DRIVE_BARE lists:
see bare_driver.py. An environment that cannot run here is *skipped* with
the reason, not failed: a round that never started has not found a bug.

`--capture` turns on the pictures. Off by default for what it *may* cost: the
Screenshot portal has no restore token, so whether a picture opens a modal
dialog is the desktop's stored permission and nothing the harness can
arrange. See tools/drive/README.md *Driving a window*.
"""

import argparse
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

import environment as B
import input_hold
import ledger
from testbed import OUT, QUIET, ROOT

# The window the testbed opens, in logical points: 1280x720 of presentable area
# plus the 34-point titlebar this library draws. Asserted, because every
# round-trip check below compares against it.
AREA0 = (1280, 720)
# Whether the round was started with the compositor's input method left
# unbound (wayland.c), which a program on this desktop inherits: the round
# is then the one that types through Wayland's fallback composer.
UNBOUND = os.environ.get("FNL_NO_INPUT_METHOD", "") not in ("", "0")
# The room a round wants around the window it starts from, in points: the
# drag that carries it 180 by 119, the 160 by 120 it then grows by, and the
# corner it is put in.
ROOM = (300, 220)
# What the testbed's F4 puts on the clipboard: TESTBED_CLIPBOARD_TEXT in
# testbed/testbed.h.
COPIED = "fenestal-testbed-copied-this"
BAR_H = 34
FLOOR_W = B.FLOOR_W

# How long the echo has to stay quiet after a transition before the window is
# read or pictured: the resize that goes with a state change is reported
# after it in some environments, and the compositor's animation may outlast
# both. Longer than testbed.QUIET because of that animation, which nothing
# reports the end of. The one figure to raise if a geometry or picture check
# turns flaky.
PAUSE = 0.3

# ---- durations that are the thing exercised, not waits ----------------------
# A key held long enough to be a hold and not a tap, with no repeat wanted.
HOLD = 0.25
# A key held until it has repeated several times, on a build of that speed.
REPEAT_HOLD = 1.5
REPEAT_HOLD_SLOW = 3.0
# Between two clicks on one spot that must not be read as a double-click,
# which on a titlebar is a maximize.
NOT_A_DOUBLE_CLICK = 0.6
# A still window before each timed key: the latency check is about an event
# arriving on a frame nothing else caused.
STILL = 0.5
# Between the two pictures that show a moved window is drawn: the scene
# moves through the GPU sections, so two rows this far apart differ on a
# live window and match on a frozen one.
LIVE_APART = 0.5
# The lead into a CPU sample, so that the sample is all one regime: the frame
# rate once the scene moves, and the off-screen skip after the window went.
MOTION_LEAD = 1.2
HIDDEN_LEAD = 1.5
# The frames a second that count as a window drawing: under the slowest
# measured drawing every frame it could (30, docs/benchmarks.md's draw
# row) and over a window at rest's none. Counted from the library's
# --frame-times lines, which the round starts the testbed with, and not
# read off the CPU: Windows charges a process's time in whole clock
# ticks of 15.6 ms, and Direct3D 12 on schenker's HD 630 draws for 2% of
# a core, four ticks in three seconds -- two read as a window at rest.
DRAWING_FPS = 5.0
# The share of the visible rate a restored window draws at: halfway
# between one back at its rate and one drawing every other refresh.
RESTORED_SHARE = 0.75
# The most of a core, in percent, that a window drawing nothing was
# measured to take. A minimized window at or under it has stopped
# drawing, however cheap its drawing was; a fifth of the visible figure
# alone asks for 0.32 of the HD 530's 1.6, under what rest costs.
AT_REST = 0.8
# The minimized window's CPU sample, seconds: long enough that a tick of
# Windows' 15.6 ms is a fifth of AT_REST. Over 4 s, three ticks charged
# for a window drawing nothing -- aspire's HD 530 under Direct3D 12,
# three times in 30 rounds -- read 1.2% and failed it.
HIDDEN_SAMPLE = 8.0
# Frames a second a minimized window may still draw and be drawing
# nothing: one strays in at the edge of a sample, a skip that stopped
# working draws at the rate.
HIDDEN_FPS = 1.0
# After another program has taken the X selection, for the compositor to
# carry it across to the Wayland one. Nothing reports that it has.
SELECTION_BRIDGE = 0.3

# The button strip, right-to-left from the window's right edge, in points.
# src/core/app.h's figures rather than a guess at them: DECO_BTN_W 28,
# DECO_BTN_GAP 6, DECO_BTN_MARGIN 10, so the pitch is 34 and the first centre
# is 24 in from the right, with the row centred at 17 in a 34-point bar.
BTN_ROW_Y = 17
BTN_DX = {"close": 24, "max": 58, "min": 92, "full": 132}
# Where the window is the output there is no minimize or maximize, and
# fullscreen keeps the 12-point group gap to close (deco_button_rects()).
BTN_DX_OUTPUT = {"close": 24, "full": 64}
# And the two lists left of fullscreen there, aimed at by their middles: the
# mode box ends 90 in from the right and is 189 wide, the output box ends 8
# before it and is 101 wide (controls.c, at a body advance of 8 points). A
# list's rows are 18 points from 27 down.
MODE_BOX_DX = 116
OUTPUT_BOX_DX = 337
# The GPU box, once the renderer lists its GPUs (one included): before the
# size box, 8 points of gap and 61 wide ("GPU 8"), so its middle is 353 in from
# the right; where the window is the output, between the mode box and the
# output box, at 317, and the output box GPU_BOX_SPAN further left.
GPU_BOX_DX = 353
GPU_BOX_DX_OUTPUT = 317
GPU_BOX_SPAN = 69
LIST_ROW0_Y = 36
LIST_ROW_H = 18

# What a layout types, by key: (strokes, the characters they type), each
# stroke (held modifiers, key). For "known-layout", by the layout's XKB name
# and variant. Only what the layout's own legend says, and the library's
# rule for a letter an accent does not go on (keymap_type(): the letter).
LAYOUT_TYPES = {
    ("de", ""): {
        "keys": [
            ([((), "Y")], [0x7a]), ([((), "Z")], [0x79]),
            ([((), "Minus")], [0xdf]), ([((), "Semicolon")], [0xf6]),
            ([((), "Apostrophe")], [0xe4]), ([((), "LBracket")], [0xfc]),
            ([(("LShift",), "Semicolon")], [0xd6]),
            ([((), "IntlBackslash")], [0x3c]),
            ([(("LShift",), "IntlBackslash")], [0x3e]),
            ([(("RAlt",), "IntlBackslash")], [0x7c]),
            ([(("RAlt",), "Q")], [0x40]), ([(("RAlt",), "E")], [0x20ac]),
            ([(("RAlt",), "7")], [0x7b]), ([(("RAlt",), "Minus")], [0x5c]),
            ([(("LShift",), "3")], [0xa7]), ([(("LShift",), "7")], [0x2f]),
            ([(("LShift",), "Grave")], [0xb0]),
        ],
        "dead": [
            ([((), "Equal"), ((), "E")], [0xe9]),
            ([(("LShift",), "Equal"), ((), "A")], [0xe0]),
            ([((), "Grave"), ((), "O")], [0xf4]),
            ([((), "Equal"), ((), "Space")], [0xb4]),
            ([((), "Grave"), ((), "Grave")], [0x5e]),
            ([((), "Grave"), ((), "X")], [0x78]),
        ],
    },
    # The same keyboard with the accents as spacing characters: the acute
    # and grave key types them at once, and the circumflex key its sign.
    ("de", "nodeadkeys"): {
        "keys": [
            ([((), "Y")], [0x7a]), ([((), "Z")], [0x79]),
            ([((), "Minus")], [0xdf]), ([((), "Semicolon")], [0xf6]),
            ([((), "Apostrophe")], [0xe4]), ([((), "LBracket")], [0xfc]),
            ([(("LShift",), "Semicolon")], [0xd6]),
            ([((), "IntlBackslash")], [0x3c]),
            ([(("LShift",), "IntlBackslash")], [0x3e]),
            ([(("RAlt",), "IntlBackslash")], [0x7c]),
            ([(("RAlt",), "Q")], [0x40]), ([(("RAlt",), "E")], [0x20ac]),
            ([(("RAlt",), "7")], [0x7b]), ([(("RAlt",), "Minus")], [0x5c]),
            ([(("LShift",), "3")], [0xa7]), ([(("LShift",), "7")], [0x2f]),
            ([(("LShift",), "Grave")], [0xb0]),
            ([((), "Grave")], [0x5e]), ([((), "Equal")], [0xb4]),
            ([(("LShift",), "Equal")], [0x60]),
            ([((), "Equal"), ((), "E")], [0xb4, 0x65]),
        ],
        "dead": [],
    },
}


class Round:
    """One environment's run, and the checks it accumulated."""

    def __init__(self, b, capture):
        self.b = b
        self.capture = capture
        self.oks, self.fails, self.skips = [], [], []
        self.shot_n = 0
        self.shots_ok = True
        # What every check's name ends in while a section is run again
        # somewhere else -- " on HDMI-A-1" (gpu_on_other_outputs()) -- so
        # the second run's checks are told from the first's.
        self.where = ""

    # ---- reporting --------------------------------------------------------
    def check(self, name, cond, detail=""):
        name += self.where
        line = "%-52s %s  %s" % (name, "ok " if cond else "FAIL", detail)
        print(line, flush=True)
        (self.oks if cond else self.fails).append(name if cond
                                                  else line.strip())
        return bool(cond)

    def skip(self, name, why):
        name += self.where
        print("%-52s skip %s" % (name, why), flush=True)
        self.skips.append(name)
        return None

    def can(self, what):
        return what in self.b.CAN

    # ---- pictures ---------------------------------------------------------
    def shot(self, tag):
        if not self.capture or not self.shots_ok:
            return None
        self.shot_n += 1
        path = os.path.join(OUT, "%s-%02d-%s.png"
                            % (self.b.NAME, self.shot_n, tag))
        try:
            return self.b.capture(path)
        except Exception as e:
            print("      (capture unavailable: %s)" % e, flush=True)
            self.shots_ok = False
            return None

    # ---- helpers ----------------------------------------------------------
    def area(self):
        st = self.b.refresh()
        return st["area"] if st else None

    def btn(self, which, width_pt):
        dx = BTN_DX_OUTPUT if self.can("display-modes") else BTN_DX
        return (width_pt - dx[which], BTN_ROW_Y)

    def cpu_percent(self, secs=4.0):
        a, t0 = self.b.cpu_seconds(), time.monotonic()
        time.sleep(secs)            # the window the sample is taken over
        z, t1 = self.b.cpu_seconds(), time.monotonic()
        return (z - a) / (t1 - t0) * 100.0

    def frames_mark(self):
        """Where the program's log stands, for frame_rate()."""
        with self.b.testbed.lock:
            return len(self.b.testbed.lines)

    def frame_rate(self, mark):
        """Frames drawn a second in the --frame-times lines since `mark`, or
        None where none came. Each line is a window of the frame loop, so
        the first can reach back before the mark by one window -- or by
        far more where the loop rested with cost sampling on, which keeps
        the window open through the rest (frame.c, rest_owed()): a window
        restored from minimized closes it at its first frame back, one
        frame over the whole time away. So a first line spanning more than
        GAP_SPAN is time nothing was drawn in, and left out: on the Mac it
        read 12 fps where 60 were drawn (2026-10-31)."""
        with self.b.testbed.lock:
            lines = self.b.testbed.lines[mark:]
        frames, spent = 0, 0.0
        first = True
        for ln in lines:
            m = FRAME_COUNT.match(ln)
            if m:
                n = int(m.group(2))
                span = float(m.group(1)) * n / 1e3
                if first and span > GAP_SPAN:
                    first = False
                    continue
                first = False
                frames += n
                spent += span
            elif FRAME_AT_REST.match(ln):
                frames += int(FRAME_AT_REST.match(ln).group(1) or 0)
                spent += FRAME_TIMES_WINDOW
        return frames / spent if spent > 0 else None

    def transition(self, act, kind="window", timeout=4.0):
        """Do `act`, wait until the library says the window changed, and
        then until it has stopped saying things.

        The "window" event is the library's own report that fullscreen or
        maximized moved, and "capture" that the pointer was taken or let
        go. A transition that never happens costs the timeout, and the
        check after it then fails on the state, as it should."""
        b = self.b
        m = b.mark()
        act()
        got = b.settle(kind, m, timeout)
        if kind == "window" and got:
            time.sleep(b.WINDOW_ANIMATION)
        b.quiet(PAUSE)
        return got


def check_bar(r, name, st, present=True):
    """Assert the compositor is (or is not) showing this window's titlebar.

    The portal hands back the whole desktop, so the bar has to be *found*
    and not sampled at a fixed offset. Two things pin it down: the colour is
    sRGB here, so the bar authored at 0.16 reads 41; and the run has to be as
    wide as *this* window, or some other window's dark strip answers."""
    import numpy as np
    # The area's width in pixels, less the one-pixel window outline the
    # decorations draw down each side.
    want = st["px"][0] - 2

    def find(im):
        a = np.asarray(im.convert("RGB")).astype(int)
        for shade, _focused in ((41, True), (31, False)):
            m = ((np.abs(a[:, :, 0] - shade) <= 2) &
                 (np.abs(a[:, :, 1] - shade) <= 2) &
                 (np.abs(a[:, :, 2] - shade) <= 6))
            for y in np.where(m.sum(axis=1) >= want - 8)[0]:
                xs = np.where(m[y])[0]
                if abs(int(xs.max() - xs.min() + 1) - want) <= 8:
                    return (int(y), int(xs.min()))
        return None

    # Looked at again when the picture disagrees, twice at most: the
    # compositor animates a window into and out of fullscreen for longer
    # than the client is told about, and a picture from inside that is of a
    # window some other width.
    for _ in range(3):
        im = r.shot("bar")
        if im is None:
            return r.skip(name, "--capture is off" if not r.capture
                                else "no composited capture")
        hit = find(im)
        if (hit is not None) == present:
            break
        time.sleep(0.4)             # the animation, which nothing reports
    return r.check(name, (hit is not None) == present,
                   "%s (a %dpx run is one)" % (hit, want))


def arrived(b, there, timeout=3.0):
    """refresh() until the application's own report says the window is
    `there`, and the last report either way."""
    end = time.time() + timeout * b.patience()
    st = b.refresh()
    while st and not there(st) and time.time() < end:
        b.quiet()
        st = b.refresh()
    return st


def cross_monitors(r):
    """Take the window to the other monitor and back.

    The one section that leaves the output the round is pinned to. What has
    to follow the window is `fnl_output_name()` and `fnl_output_size_pt()`, the
    pair a caller filters its size list against: a window that still names
    the screen it came from offers 4K modes on a 1200p monitor.

    Nothing here needs "server-rect": the whole exchange is in the
    application's own report, so Wayland answers it as the other two do.
    No fullscreen or maximize on the far monitor; that is the compositor's
    arithmetic. What is checked is that the library noticed."""
    b, check = r.b, r.check
    if not r.can("multi-output"):
        return r.skip("the window follows a move to another monitor",
                      "the window server shows this client one output")
    mons = b.monitors()
    if len(mons) < 2:
        return r.skip("the window follows a move to another monitor",
                      "one monitor on this desktop")
    tgt = b.target_monitor()
    if not tgt:
        return r.skip("the window follows a move to another monitor",
                      "the monitor it starts on is not known")
    other = next((m for m in mons if m[0] != tgt[0]), None)
    if not other:
        return r.skip("the window follows a move to another monitor",
                      "no second output to move to")
    oname, _pri, ox, oy, ow, oh = other

    st = b.refresh()
    home, home_out = st["out"], st["output"]
    # Centre to centre, so the window is wholly on the far screen: which
    # output a straddling window belongs to is the compositor's to decide.
    dx = (ox + ow // 2) - (tgt[1] + tgt[3] // 2)
    dy = (oy + oh // 2) - (tgt[2] + tgt[4] // 2)
    m = b.mark()
    b.move_window_by(dx, dy)
    st = arrived(b, lambda t: b.on_output(t["out"], oname, home))
    # In the environment's own words for a monitor; see on_output().
    check("the window follows a move to another monitor",
          b.on_output(st["out"], oname, home),
          "%s -> %s, wanted %s" % (home, st["out"], oname))
    # "output W H pt NAME": the event a caller is told by, not only the
    # state each line carries.
    told = [" ".join(e["parts"][4:]) for e in b.since(m, "output")]
    check("and FNL_EVENT_OUTPUT says so",
          any(b.on_output(n, oname, home) for n in told),
          "told %s" % (told or "nothing"))
    # Fits inside the new screen and is not still the old one's -- not
    # equality with anything. fnl_output_size_pt() is the room a window has
    # there: the mode, or the mode less what the shell reserves, depending on
    # whether the compositor has sent bounds for this output yet. Two
    # monitors of one size cannot tell the old figure from the new, and the
    # name above is then the whole answer.
    got = st["output"]
    same = (ow, oh) == (tgt[3], tgt[4])
    check("and the output size follows with it",
          0 < got[0] <= ow and 0 < got[1] <= oh and
          (same or got != home_out),
          "%s on %s %dx%d, was %s" % (got, oname, ow, oh, home_out))

    b.move_window_by(-dx, -dy)
    st = arrived(b, lambda t: t["out"] == home)
    got = st["output"]
    check("and both come back",
          st["out"] == home and 0 < got[0] <= tgt[3] and 0 < got[1] <= tgt[4],
          "%s %s, was %s %s" % (st["out"], got, home, home_out))


def drawn_pointer(r, name="the pointer is drawn where it is"):
    """The pointer is on the screen where the echo says it is.

    Where the library draws it -- KMS, on the cursor plane -- nothing else
    would: a pointer that moved without its plane is invisible, and a
    picture of the scanned-out screen (kms_grab.py blends the plane in) is
    the one place that shows it. The arrow is white within a black outline,
    each exactly 255 and 0, which the scene beneath does not draw in a
    glyph's box; two pictures with the pointer at either of two points
    tell the arrow from anything that happens to be there."""
    b, check = r.b, r.check
    if not r.can("drawn-pointer"):
        return r.skip(name, "the window server draws the pointer")
    if not r.capture:
        return r.skip(name, "--capture is off")
    import numpy as np
    st = b.refresh()
    s = st["scale"] / 120.0
    y = BAR_H + st["area"][1] // 2
    spots = [(st["area"][0] // 3, y), (2 * st["area"][0] // 3, y)]

    def arrow(im, at):
        """White and black pixels in the arrow's box at window point `at`:
        the glyph is 12x19 with 69 of one and 50 of the other."""
        a = np.asarray(im.convert("RGB")).astype(int)
        x0, y0 = int(at[0] * s), int(at[1] * s)
        box = a[y0:y0 + 19, x0:x0 + 12]
        return (int((box == 255).all(axis=2).sum()),
                int((box == 0).all(axis=2).sum()))

    seen = []
    for at in spots:
        b.move_to(*at)
        b.quiet(PAUSE)
        im = r.shot("pointer")
        if im is None:
            return r.skip(name, "no composited capture")
        seen.append([arrow(im, p) for p in spots])
    # Each picture: the arrow whole where the pointer is, and not where it
    # is not.
    here = [seen[i][i] for i in range(2)]
    there = [seen[i][1 - i] for i in range(2)]
    check(name,
          all(w >= 65 and k >= 45 for w, k in here)
          and all(w < 20 for w, _k in there),
          "white/black at the pointer %s, at the other spot %s"
          % (here, there))


def layout_types(r, typed):
    """Every key of LAYOUT_TYPES' table for the layout the window types
    through, under text entry: what the legend says, AltGr and the 102nd
    key's included, and the dead keys composed by the library's table --
    where the library composes them (Environment.COMPOSED_ELSEWHERE).

    One check for the keys and one for the dead keys, each naming every
    stroke that typed something else. A dead key left held by a failure is
    let go with Escape, which takes an accent back."""
    b, check = r.b, r.check
    layout = b.known_layout()
    table = LAYOUT_TYPES.get(layout)
    if table is None:
        return r.skip("the machine's layout types what its keys say",
                      "no table for layout %r variant %r" % layout)

    def stroke(strokes):
        m = b.mark()
        for mods, key in strokes:
            held = [b.key_down(k) for k in mods]
            b.press(key)
            for h in reversed(held):
                b.let_go(h)
        b.quiet()
        return typed(m)

    def name(strokes):
        return " ".join("+".join(mods + (key,)) for mods, key in strokes)

    def show(cs):
        return " ".join("U+%04X" % c for c in cs) or "none"

    label = "%s%s" % (layout[0], "(%s)" % layout[1] if layout[1] else "")
    for what, title in (("keys", "the %s layout types what its keys say"),
                        ("dead", "and its dead keys compose")):
        if what == "dead" and not table[what]:
            r.skip(title, "the layout has no dead keys")
            continue
        if what == "dead" and b.COMPOSED_ELSEWHERE:
            r.skip(title, b.COMPOSED_ELSEWHERE)
            continue
        wrong = []
        for strokes, want in table[what]:
            got = stroke(strokes)
            if got != want:
                wrong.append("%s typed %s, wanted %s"
                             % (name(strokes), show(got), show(want)))
                b.press("Escape")
                b.quiet()
        check(title % label if "%s" in title else title, not wrong,
              "; ".join(wrong) or "%d sequences" % len(table[what]))


def layout_groups(r, typed):
    """A group key, under text entry: the program started with the
    console's layout and a second group (KmsBare.group_layout()), Alt+Shift
    between them. The Y key typed in each -- y in US, and whatever the
    console's layout puts there, which is all this asks of it -- and the
    line the backend says at each switch."""
    b, check = r.b, r.check
    second = getattr(b, "SECOND_GROUP", "us")
    named = [ln for ln in b.dump(1 << 30) if ln.startswith("KMS: layout ")]
    if not named or not re.match(r"^KMS: layout \S+,%s " % second,
                                 named[0]):
        r.skip("a group key switches the layout",
               "the program was given no second group: the console's "
               "layout has more than one, or is %s" % second)
        r.skip("and switches it back", "the same")
        return

    def y():
        m = b.mark()
        b.press("Y")
        b.quiet()
        return typed(m)

    def switch():
        alt = b.key_down("LAlt")
        b.press("LShift")
        b.let_go(alt)
        b.quiet()

    def said(group):
        return any(ln.startswith("KMS: typing in group %d of 2, " % group)
                   for ln in b.dump(1 << 30))

    first = y()
    switch()
    other = y()
    check("a group key switches the layout",
          other == [0x79] and said(2),
          "Y typed %s, then after Alt+Shift %s; logged: %s"
          % (first, other, said(2)))
    switch()
    back = y()
    check("and switches it back", back == first and said(1),
          "Y typed %s again, %s before; logged: %s" % (back, first, said(1)))


def room_to_grow(r, dx, dy, spare=44):
    """How far the bottom-right corner may be dragged, in points: (dx, dy)
    where the screen has that much beyond it, and less where it has not.

    A corner dragged off the screen is one no pointer can follow, and a
    panel at 125% is smaller than the window this round has just dragged
    down and right plus the growth it is about to ask for. So the window is
    first moved back towards its monitor's origin by what is missing, and
    the drag is then cut to the room that leaves. `spare` is kept beyond
    it for the back-to-back drags that follow, each forty points.

    Only where the window can be found on the screen (Environment.placed());
    elsewhere the compositor placed it and the answer is what was asked."""
    b = r.b
    got = b.placed()
    if not got:
        return dx, dy
    (x, y, w, h), mon = got
    g = b.refresh()["scale"] / 120.0
    far_x, far_y = mon[0] + mon[2], mon[1] + mon[3]
    back_x = min(max(0, x + w + (dx + spare) * g - far_x), max(0, x - mon[0]))
    back_y = min(max(0, y + h + (dy + spare) * g - far_y), max(0, y - mon[1]))
    if back_x or back_y:
        b.move_window_by(-int(back_x), -int(back_y))
        (x, y, w, h), mon = b.placed(still=True)
    return (max(0, min(dx, int((far_x - x - w) / g) - spare)),
            max(0, min(dy, int((far_y - y - h) / g) - spare)))


def near(a, b, tol=2):
    return abs(a - b) <= tol


def rect_eq(a, b, tol=2):
    # Lengths first: zip() stops at the shorter, and a rectangle missing a
    # field would otherwise compare equal on the fields it has.
    return len(a) == len(b) and all(near(x, y, tol)
                                    for x, y in zip(a, b, strict=True))


# ---- the round --------------------------------------------------------------

def pick_row(r, box_dx, row):
    """Open a titlebar list and click one of its rows; the mark taken
    before the click."""
    b = r.b
    w = b.refresh()["area"][0]
    b.click(w - box_dx, BTN_ROW_Y)
    time.sleep(NOT_A_DOUBLE_CLICK)
    m = b.mark()
    b.click(w - box_dx, LIST_ROW0_Y + row * LIST_ROW_H)
    return m


# A renderer as the inventory names it, and as FNL_RENDERER and the echo
# key it.
RENDERER_KEYS = {"Vulkan": "vulkan", "OpenGL": "gl", "Direct3D 12": "d3d12",
                 "Metal": "metal"}
# The line the library logs when it has listed another renderer's GPUs
# (gpu_listed() in src/core/app.c).
OTHER_LISTED = re.compile(r"(.+) lists \d+ GPUs? here, in \d+ ms$")


# The library's line when it stops offering a move to another kernel
# driver (app_gpu_drivers_pin()): the move entries leave the list, which
# it then says again with its new numbers.
DRIVERS_PINNED = "GPU driver: no move offered from now on: "


def drivers_pinned(lines):
    """The library's reason for offering no more moves to another kernel
    driver, if one of `lines` gives it."""
    said = [ln for ln in lines if ln.startswith(DRIVERS_PINNED)]
    return said[-1][len(DRIVERS_PINNED):] if said else None


def gpu_rows(b):
    """Every row of the GPU box, {number: (name, renderer key)}, off the
    inventory the library logs at every pick and the whole list it logs
    once the other renderers are listed (`GPU 2: llvmpipe (LLVM ...)
    (software, Vulkan)`): the numbers are the list's, from 1, and the last
    line for a number wins. The renderer is the kind's last word where the
    list spans several, and the round's own where it does not. A move to
    another kernel driver is a row too, named `<GPU>, on <driver>`; each
    row is one renderer on one GPU on one driver for the window's life,
    until the library stops offering the moves, after which only the list
    it says then counts (DRIVERS_PINNED)."""
    got = {}
    lines = b.dump(1 << 30)
    pins = [i for i, ln in enumerate(lines) if ln.startswith(DRIVERS_PINNED)]
    for ln in lines[pins[-1] + 1:] if pins else lines:
        m = re.match(r"GPU (\d+): (.*) \(([^()]*)\)$", ln)
        if not m:
            continue
        # "Vulkan via D3D11" is a Vulkan row, presented another way
        # (fnl_gpu_via_at()).
        last = m.group(3).split(", ")[-1].split(" via ")[0]
        got[int(m.group(1))] = (m.group(2),
                                RENDERER_KEYS.get(last, b.RENDERER))
    return got


def gpus_listed(b):
    """The GPUs of the GPU box, as gpu_rows(), without the moves to another
    kernel driver."""
    return {n: v for n, v in gpu_rows(b).items() if ", on " not in v[0]}


def gpu_via_rows(b):
    """The numbers of the GPU box's rows that present another way, `(...,
    Vulkan via D3D11)` (fnl_gpu_via_at())."""
    return {int(m.group(1)) for m in
            (re.match(r"GPU (\d+): .* \(([^()]*)\)$", ln)
             for ln in b.dump(1 << 30))
            if m and " via " in m.group(2)}


def gpu_in_use(b, listed):
    """The number of the GPU the last pick took, off `GPU in use: <name>`
    -- Direct3D 12 and Metal put the renderer behind the name, so the
    longest listed name it starts with is the one -- among the round's own
    renderer's, which drew the window when it opened. A device listed
    twice is told by the way: `(via D3D11)` behind the name is its second
    row."""
    said = [ln[len("GPU in use: "):] for ln in b.dump(1 << 30)
            if ln.startswith("GPU in use: ")]
    own = [n for n, (_, rnd) in listed.items() if rnd == b.RENDERER]
    # A renderer that lists one GPU names none as in use: the list is the
    # answer, as ledger.device() reads it.
    if not said:
        return own[0] if len(own) == 1 else None
    hits = [n for n, (name, rnd) in listed.items() if rnd == b.RENDERER
            and (said[-1] == name or said[-1].startswith(name + " ("))]
    via = said[-1].endswith(" (via D3D11)")
    rows = gpu_via_rows(b)
    hits = [n for n in hits if (n in rows) == via] or hits
    return max(hits, key=lambda n: len(listed[n][0])) if hits else None


# How long a move the program has begun and not ended is waited for past a
# section's own wait: under ThreadSanitizer OpenGL's instance alone took
# 80 s on Intel, 98 s on llvmpipe and 204 s on Zink over NVK (schenker,
# measured with the validation layer in Zink's instance as in ours),
# where a release build takes under one.
MOVE_UNDER_WAY_S = 600.0
# The library's lines for a move begun (frame.c, drivers.c) and for one
# that ended without landing (app.c, frame.c, drivers.c).
MOVE_BEGUN = "moving to "
MOVE_FAILED = (" would not draw here", " back from ", "the GPU did not move")


def await_move(b, logged, done, timeout):
    """Wait for `done()` as b.until() does, and on past `timeout` while the
    program lives and says, since line `logged`, that a move has begun and
    not failed -- up to MOVE_UNDER_WAY_S more. Whether it came to."""
    if b.until(done, timeout=timeout):
        return True
    end = time.time() + MOVE_UNDER_WAY_S
    while b.alive() and time.time() < end:
        said = b.dump(1 << 30)[logged:]
        begun = [i for i, ln in enumerate(said) if ln.startswith(MOVE_BEGUN)]
        if not begun or any(f in ln for ln in said[begun[-1]:]
                            for f in MOVE_FAILED):
            break
        if b.until(done, timeout=5.0):
            return True
    return bool(done())


def title_names(b, parts):
    """Whether the title names the renderer and the GPU an `EV gpu` line
    says the window is on, as the testbed puts them: the renderer's name
    and the GPU's short name, which starts the long one."""
    title = b.wm_name() or ""
    if len(parts) < 4:
        return False
    rnd = {v: k for k, v in RENDERER_KEYS.items()}.get(parts[2], parts[2])
    # fnl_gpu_short_name()'s cut: "Mesa " and the vendor's marks out, and
    # the driver's detail behind the first " (" or "/" left off.
    gpu = " ".join(parts[3:])
    gpu = gpu[5:] if gpu.startswith("Mesa ") else gpu
    gpu = re.split(r" \(|/", gpu.replace("(R)", "").replace("(TM)", ""))[0]
    return rnd in title and " ".join(gpu.split()) in title


def unmovable(b, name, renderer):
    """Why the round does not move to a GPU it lists, or None: a renderer
    that will not draw here (Environment.NO_MOVE_TO), or NVIDIA's own
    driver under ThreadSanitizer, which a round does not start on either
    (_OnBare.start() says why) -- NVK names itself, and is Mesa's."""
    if renderer in b.NO_MOVE_TO:
        return b.NO_MOVE_TO[renderer]
    if getattr(b, "variant", "") == "tsan" and "NVIDIA" in name and \
            "(NVK " not in name:
        return "ThreadSanitizer cannot follow NVIDIA's own driver"
    return None


def gpu_moves(r):
    """Where the list holds two GPUs or more: the titlebar's GPU box
    moves the window onto each other one and back to the one it opened on
    (fnl_request_gpu()). Each move is a rebuild of the device there that the
    caller is told of -- FNL_EVENT_GPU, with the GPU's place in the list and
    the renderer -- and that the log says as a move, never as a loss. A GPU
    of another renderer is a move to that renderer, the window drawn with
    it, which the event names."""
    b, check = r.b, r.check
    # The other renderer's GPUs are listed beside the first frame: 40 to
    # 300 ms at release, 23 to 29 s under ASan where NVIDIA's userspace is
    # not kept loaded (a round on a kernel driver, the laptops). Counted
    # before its line, the round moves to fewer GPUs than there are.
    b.until(lambda: any(
        m and RENDERER_KEYS.get(m.group(1)) != b.RENDERER
        for m in (OTHER_LISTED.match(ln) for ln in b.dump(1 << 30))),
        timeout=30.0)
    listed = gpus_listed(b)
    if len(listed) < 2:
        r.skip("the GPU box moves the window to another GPU",
               "%d GPU listed" % len(listed))
        return
    home = gpu_in_use(b, listed)
    if not check("the inventory names the GPU in use", home is not None,
                 "%d listed" % len(listed)):
        return
    dx = GPU_BOX_DX_OUTPUT if r.can("display-modes") else GPU_BOX_DX
    area0 = b.refresh()["area"]
    sized = away = False
    for n in [n for n in sorted(listed) if n != home] + [home]:
        back = n == home
        # Every other GPU declined leaves the window where it opened, and a
        # pick of that GPU is no move: the library says nothing of it.
        if back and not away:
            r.skip("and back to the GPU it opened on",
                   "no move away was made")
            continue
        why = unmovable(b, *listed[n])
        if why:
            r.skip("the GPU box moves the window to GPU %d, drawn with %s"
                   % (n, listed[n][1]), why)
            continue
        logged = len(b.dump(1 << 30))
        mk = pick_row(r, dx, n - 1)
        away = away or not back
        await_move(b, logged, lambda: b.since(mk, "gpu"),
                   20.0 * b.patience())
        got = b.since(mk, "gpu")
        parts = got[-1]["parts"] if got else []
        name = "and back to the GPU it opened on" if back \
            else "the GPU box moves the window to GPU %d" % n
        if not back and listed[n][1] != b.RENDERER:
            name += ", drawn with %s" % listed[n][1]
        check(name, parts[1:3] == [str(n - 1), listed[n][1]],
              " ".join(parts) if parts else "no EV gpu: %s" % listed[n][0])
        time.sleep(2.0 * b.patience())
        said = b.dump(1 << 30)[logged:]
        moved = [ln for ln in said if ln.startswith("moving to GPU %d: " % n)]
        lost = [ln for ln in said if "lost -- rebuilding" in ln]
        check("and says it as a move, not a loss" if not back
              else "and says that as a move too", bool(moved) and not lost,
              (lost or moved or ["nothing logged"])[-1])
        # refresh() has no state to give once the program is gone, which
        # is this check failing and not the round.
        st = b.refresh()
        now = st["area"] if st else None
        check("and is drawn there at its size" if not back
              else "and is drawn there again", b.alive() and now == area0,
              "%s, was %s" % (now if st else "no state: the program is "
                              + ("running" if b.alive() else "gone"), area0))
        # Drawn and shown are two things: under wine on a bare Xorg with
        # no Xcomposite every frame after a move is drawn and the window
        # keeps the one the move left, which has no black in it. So the
        # picture is read twice, and a moving scene must have moved.
        if r.can("read-pixels"):
            first = b.scene_row()
            time.sleep(LIVE_APART)
            second = b.scene_row()
            if first is None or second is None:
                r.skip("and what it draws there is shown" if not back
                       else "and shown again", "no picture of the window")
            else:
                check("and what it draws there is shown" if not back
                      else "and shown again", first != second,
                      "the picture moves" if first != second else
                      "the same picture %.1f s apart while the scene "
                      "moves: a frozen frame" % LIVE_APART)
        # The testbed names its renderer and GPU in the title again at
        # every FNL_EVENT_GPU: what the window server shows is the
        # caller's string, and the bar draws the same one.
        if r.can("wm-props"):
            title = b.until(lambda: title_names(b, parts), timeout=2.0)
            check("and the title names the renderer and the GPU",
                  bool(title), b.wm_name() or "no title")
        # Where the library draws the pointer, the move may have set the
        # output's mode the other way under it (kms.c,
        # kms_present_window_renew()).
        if r.can("drawn-pointer"):
            drawn_pointer(r, "and the pointer is drawn after it" if not back
                          else "and after the way back")
        time.sleep(NOT_A_DOUBLE_CLICK)
        if not back and not sized:
            area0 = sizes_after_move(r, area0)
            sized = True


# How many times a round moves the GPU to its other kernel driver and back:
# what a move leaves behind -- a module's use count, a device node held, a
# renderer's state not freed -- shows on a later move and not on the first.
# FNL_DRIVE_GPU_CYCLES sets it for a soak.
GPU_DRIVER_CYCLES = int(os.environ.get("FNL_DRIVE_GPU_CYCLES", "3"))


def gpu_driver_moves(r):
    """Where FNL_GPU_DRIVER offers the NVIDIA GPU a move to its other
    kernel driver -- a KMS round on a driver (`@machine:nouveau`), which
    _OnBare.gpu_sent() sets it for -- the GPU box's row `<GPU>, on
    <driver>` moves the window there and then back, GPU_DRIVER_CYCLES
    times, the checks of each cycle after the first named for it. The
    list has such a row for each renderer it spans, after every GPU
    listed; a round names one renderer, so there is one, the round's.
    While the GPU is on the other driver its row and the move entry trade
    places, so the move back is the GPU's own row of the list it opened
    with. Each move is a rebuild
    with the round's renderer on that GPU under the name its new driver
    gives it, said as a move and never as a loss, the window drawn across
    on another GPU meanwhile -- on Vulkan, whatever it lands on -- where
    it can be or the frames said to pause (DESIGN *Drawn across the
    move*), and the helper's line says how long the kernel took. A
    landing is a GPU named as the new driver names it -- NVK's "(NVK
    ...)" on nouveau, Zink's for OpenGL, and nowhere else -- and never one
    the library then sent back, which with the window first on the NVIDIA
    GPU is a GPU called NVIDIA too."""
    b, check = r.b, r.check
    drawn = {v: k for k, v in RENDERER_KEYS.items()}.get(b.RENDERER,
                                                          b.RENDERER)
    offered = [ln for ln in b.dump(1 << 30) if ln.startswith("GPU driver: ")
               and ", and can be moved to " in ln]
    m = re.match(r"GPU driver: (\S+) is on (\S+), and can be moved to (\S+)$",
                 offered[-1]) if offered else None
    if not m:
        r.skip("the GPU box moves the GPU to its other kernel driver",
               "no move offered (FNL_GPU_DRIVER)")
        return
    pci, bound, other = m.groups()
    pinned = drivers_pinned(b.dump(1 << 30))
    if pinned:
        r.skip("the GPU box moves the GPU to its other kernel driver",
               "the library offers no move now: %s" % pinned)
        return
    # Where the GPU is now, which is not where the line found it when a
    # ranking opened the window on its first entry on the other driver
    # (rank.c's open_on()): the list's move entry of the round's renderer
    # names the driver it would go to.
    rows0 = gpu_rows(b)
    for name, rnd in rows0.values():
        if rnd == b.RENDERER and name.endswith(", on " + bound):
            bound, other = other, bound
            break
    # The library offers the move by the kernel driver alone and learns of
    # a missing Vulkan driver by trying; the round asks the machine first.
    # The move only runs on a bare machine, which answers no_vulkan_on().
    # Each cycle lands on both drivers, so a driver the round does not move
    # to (NVIDIA's own under ThreadSanitizer) is the whole section's.
    why = next((w for w in (row_unmovable(b, "GPU, on " + d, b.RENDERER)
                            for d in (bound, other)) if w), None)
    if why:
        r.skip("the GPU box moves the GPU to its other kernel driver", why)
        return
    missing = b.no_vulkan_on(other)
    if missing:
        r.skip("the GPU box moves the GPU to its other kernel driver",
               missing)
        return
    dx = GPU_BOX_DX_OUTPUT if r.can("display-modes") else GPU_BOX_DX
    area0 = b.refresh()["area"]
    # Each renderer on each driver keeps its row for the window's life,
    # wherever the list puts it -- after every GPU listed, or where a
    # ranking puts it: the move there is the row of the round's renderer
    # named `on <other>` as the list opened, and the move back the GPU's
    # own row then, which the move entry takes while the GPU is away
    # (app_gpu_driver_entries()).
    own = sorted(n for n, (name, rnd) in rows0.items()
                 if rnd == b.RENDERER and "NVIDIA" in name
                 and ", on " not in name)
    move = sorted(n for n, (name, rnd) in rows0.items()
                  if rnd == b.RENDERER and name.endswith(", on " + other))
    if not own or not move:
        r.skip("the GPU box moves the GPU to its other kernel driver",
               "no row of %s's on the NVIDIA GPU, or no move: %s"
               % (b.RENDERER, sorted(rows0.items())))
        return
    there, home = move[0] - 1, own[0] - 1
    moves = ((bound, other, False), (other, bound, True)) * GPU_DRIVER_CYCLES
    for i, (frm, to, back) in enumerate(moves):
        nth = " (cycle %d)" % (i // 2 + 1) if i >= 2 else ""
        row = home if back else there
        logged = len(b.dump(1 << 30))
        mk = pick_row(r, dx, row)

        def landed():
            return [e for e in b.since(mk, "gpu")
                    if e["parts"][2:3] == [b.RENDERER]
                    and "NVIDIA" in " ".join(e["parts"][3:])
                    and ("(NVK " in " ".join(e["parts"][3:]))
                    == (to == "nouveau")]
        await_move(b, logged, lambda: bool(landed()), 30.0 * b.patience())
        got = landed()
        parts = got[-1]["parts"] if got else []
        said = b.dump(1 << 30)[logged:]
        rebound = [ln for ln in said if ln.startswith(
            "GPU driver: fenestal-gpu-driver: %s: %s to %s in "
            % (pci, frm, to))]
        undone = [ln for ln in said if ln.startswith(
            "GPU driver: %s back from " % pci)]
        name = ("and back to %s" if back
                else "the GPU box moves the GPU to %s") % to + nth
        check(name, bool(parts) and bool(rebound) and not undone,
              undone[-1] if undone else
              " ".join(parts[3:]) + ("; " + rebound[-1].split(": ", 2)[-1]
                                     if rebound else "; the kernel driver "
                                     "said nothing") if parts
              else "no EV gpu on an NVIDIA GPU with %s on %s" % (drawn, to))
        time.sleep(2.0 * b.patience())
        said = b.dump(1 << 30)[logged:]
        moved = [ln for ln in said if ln.startswith("moving to ")
                 and ", on %s, the renderer built again on it" % to in ln]
        lost = [ln for ln in said if "lost -- rebuilding" in ln]
        check(("and says it as a move, not a loss" if not back
               else "and says that as a move too") + nth,
              bool(moved) and not lost,
              (lost or moved or ["nothing logged"])[-1])
        across = [ln for ln in said if ln.startswith("GPU driver: drawn on ")
                  and " while %s moves to %s" % (pci, to) in ln]
        paused = [ln for ln in said if "the frames pause for this move" in ln]
        built = [ln for ln in said if ln.startswith("rebuilt on ")
                 and ", drawn with %s in " % drawn in ln]
        check("and is drawn across it, or says the frames pause" + nth,
              bool(across or paused),
              (across or paused or ["neither drawn across nor paused"])[-1]
              + ("; " + built[-1].split(", drawn with ")[-1] if built else ""))
        st = b.refresh()     # None once the program is gone, as above
        now = st["area"] if st else None
        check(("and is drawn there at its size" if not back
               else "and is drawn there again") + nth,
              b.alive() and now == area0,
              "%s, was %s" % (now if st else "no state: the program is "
                              + ("running" if b.alive() else "gone"), area0))
        if r.can("drawn-pointer"):
            drawn_pointer(r, ("and the pointer is drawn after it"
                              if not back else "and after the way back")
                          + nth)
        # The moves withdrawn, the rows the rest would click are gone and
        # the ones behind them renumbered: each would fail for this one.
        pinned = drivers_pinned(said)
        if pinned:
            r.skip("the moves to another kernel driver after this one",
                   "the library offers no move now: %s" % pinned)
            return
        time.sleep(NOT_A_DOUBLE_CLICK)


def every_pair(nodes):
    """A walk over `nodes`, in their order, that takes every ordered pair
    of them once -- n(n-1) steps -- row by row, as a hand would: the first
    to each row after the next and back (1 3 1 4 1 ... 8 1), then on to
    the next (2), which does the same, and so on; the step from each to
    its next is the last of its turn, and the steps back down are the
    walk's end (8 7 6 ... 1), which leaves it where it began."""
    v = list(nodes)
    walk = [v[0]]
    for i in range(len(v) - 1):
        for j in range(i + 2, len(v)):
            walk += [v[j], v[i]]
        walk.append(v[i + 1])
    walk += v[-2::-1]
    return walk


def row_unmovable(b, name, renderer):
    """unmovable() for a row of gpu_rows(): a move entry by the driver it
    names, which is NVIDIA's own where it says `on nvidia`."""
    if ", on " not in name:
        return unmovable(b, name, renderer)
    if renderer in b.NO_MOVE_TO:
        return b.NO_MOVE_TO[renderer]
    if getattr(b, "variant", "") == "tsan" and name.endswith(", on nvidia"):
        return "ThreadSanitizer cannot follow NVIDIA's own driver"
    return None


def gpu_every_pair(r):
    """Where FNL_GPU_DRIVER offers a move: from every row of the GPU box to
    every other -- each renderer on each GPU on each kernel driver, eight
    rows and 56 switches on schenker -- in one walk through the list as
    numbered, ordered by the machine's ranking (FNL_GPU_RANK=order), that
    takes each ordered pair once row by row (every_pair()), so that a
    switch between renderers, between GPUs and between drivers comes as
    the list has it; in the one process, the window never closed, and
    from the first row, a step to it first where the window is elsewhere.
    Each switch is a click on the row, by the number it keeps for
    the window's life, and lands when FNL_EVENT_GPU names that row and its
    renderer; never sent back, never said as a loss. A switch that moved
    the GPU to another driver waits for the whole list to be logged again,
    which the other renderer's GPUs join once listed on the new driver: a
    click before that is on an entry the list then remakes."""
    b, check = r.b, r.check
    title = "every row of the GPU box to every other"
    rows = gpu_rows(b)
    pinned = drivers_pinned(b.dump(1 << 30))
    if pinned:
        r.skip(title, "the library offers no move to another kernel driver "
               "now: %s" % pinned)
        return
    if not any(", on " in name for name, _ in rows.values()):
        r.skip(title, "no move to another kernel driver offered")
        return
    out = {n: why for n, (name, rnd) in rows.items()
           if (why := row_unmovable(b, name, rnd))}
    for n, why in sorted(out.items()):
        r.skip("%s: GPU %d" % (title, n), why)
    nodes = sorted(n for n in rows if n not in out)
    evs = b.since(0, "gpu")
    cur = int(evs[-1]["parts"][1]) + 1 if evs else \
        gpu_in_use(b, gpus_listed(b))
    if cur not in nodes or len(nodes) < 2:
        r.skip(title, "the window is on GPU %s, %d rows to move between"
               % (cur, len(nodes)))
        return
    walk = every_pair(nodes)
    lead = [cur] if cur != walk[0] else []
    dx = GPU_BOX_DX_OUTPUT if r.can("display-modes") else GPU_BOX_DX
    failed = {n: [] for n in nodes}
    steps = dict.fromkeys(nodes, 0)
    driver_moves = 0
    stopped = None
    path = lead + walk
    print("  every pair: %s" % " ".join(map(str, path)), flush=True)
    for frm, to in zip(path, path[1:], strict=False):
        logged = len(b.dump(1 << 30))
        steps[frm] += 1
        mk = pick_row(r, dx, to - 1)

        def landed(to=to):
            return [e for e in b.since(mk, "gpu")
                    if e["parts"][1:3] == [str(to - 1), rows[to][1]]]
        await_move(b, logged, lambda: bool(landed()), 30.0 * b.patience())
        said = b.dump(1 << 30)[logged:]
        moved = any(ln.startswith("GPU driver: moving ") for ln in said)
        if moved:
            driver_moves += 1
            # The whole list again, the other renderer's GPUs listed on
            # the new driver: as many `GPU n:` lines as rows.
            b.until(lambda: sum(
                1 for ln in b.dump(1 << 30)[logged:]
                if re.match(r"GPU \d+: ", ln)) >= len(rows),
                timeout=30.0 * b.patience())
            said = b.dump(1 << 30)[logged:]
        bad = [ln for ln in said if " back from " in ln
               or "would not draw here" in ln or "lost -- rebuilding" in ln]
        # The moves withdrawn, the list is renumbered under the walk, and
        # every later step would click a row that is now another.
        pinned = drivers_pinned(said)
        if pinned:
            stopped = "%d->%d: the library offers no move now: %s" % (
                frm, to, pinned)
            failed[frm].append(stopped)
            break
        # A ranked round opened on OpenGL owes the layer's proof to the
        # first Vulkan entry it lands on (_OnBare._validated()).
        if getattr(b, "layer_owed", False) and rows[to][1] == "vulkan" \
                and landed() and not bad and b.alive():
            b.layer_owed = False
            mapped = b.layer_mapped()
            check("the validation layer is in the program, at the first "
                  "Vulkan entry (GPU %d)" % to, mapped,
                  "mapped" if mapped else "asked for and not mapped")
        if not landed() or bad or not b.alive():
            got = b.since(mk, "gpu")
            failed[frm].append("%d->%d: %s" % (
                frm, to, bad[-1] if bad else " ".join(got[-1]["parts"])
                if got else "no EV gpu" if b.alive() else "the program "
                "is gone"))
            if not b.alive():
                break
        time.sleep(NOT_A_DOUBLE_CLICK)
    if getattr(b, "layer_owed", False):
        r.skip("the validation layer is in the program",
               "the ranking opened on OpenGL and the walk landed on no "
               "Vulkan entry")
    for n in nodes:
        name, rnd = rows[n]
        if stopped and not steps[n]:
            r.skip("%s: from GPU %d (%s, %s)" % (title, n, rnd, name),
                   "the walk stopped before it, at %s" % stopped)
            continue
        check("%s: from GPU %d (%s, %s)" % (title, n, rnd, name),
              not failed[n], "; ".join(failed[n][:3])
              or "%d switches" % steps[n])
    switches = len(walk) - 1
    lost = sum(len(f) for f in failed.values())
    check("%s: %d switches%s, %d of them to another kernel driver, in one "
          "process" % (title, switches, " after one to GPU %d" % walk[0]
                       if lead else "", driver_moves),
          lost == 0 and b.alive(), "the walk stopped at %s" % stopped
          if stopped else "%d of %d did not land" % (lost, switches)
          if lost else "every one landed")


def outputs_offered(b):
    """The output the window opened on and the others connected, off the
    KMS backend's open line; (None, []) where it said none."""
    said = "\n".join(b.dump(1 << 30))
    m = re.search(r"KMS: (\S+) on \S+, \d+x\d+ at [\d.]+ Hz, \d+ modes? "
                  r"on offer; other outputs: (.*)", said)
    if not m:
        return None, []
    others = m.group(2).strip()
    return m.group(1), [] if others == "none" else others.split(", ")


def is_output(out, name):
    """Whether the window's output `out` is the one named: by its whole
    first word, so DP-1 is not eDP-1 or DP-10, and a Wayland description
    ("HDMI-1 (IVM PL4071UH)") is its connector's."""
    return out.split()[0] == name if out.split() else False


def to_output(r, name, rows):
    """The window moved to output `name` from the titlebar's output list,
    whose `rows` rows are in the card's order, which nothing says: tried
    row by row until the window is there. Whether it got there."""
    b = r.b
    out_dx = OUTPUT_BOX_DX + (GPU_BOX_SPAN if gpus_listed(b) else 0)
    for row in range(rows):
        if is_output(b.refresh()["out"], name):
            return True
        mk = pick_row(r, out_dx, row)
        b.until(lambda: b.since(mk, "output"), timeout=10.0)
        time.sleep(NOT_A_DOUBLE_CLICK)
    b.until(lambda: is_output(b.refresh()["out"], name), timeout=5.0)
    return is_output(b.refresh()["out"], name)


def gpu_on_other_outputs(r):
    """Where the window is an output and another is connected: the GPU
    sections again on each other one -- the GPU walk, the moves to the
    other kernel driver and from every row to every other -- in the same
    process, the window taken there from the titlebar's output list and
    back after. Another output is another path for most rows: schenker's
    HDMI is on Intel's card, so every other GPU's frames are read back and
    copied onto it, at 3840x2160 and 30 Hz there; and a ranking orders the
    list for each display, so the walk meets the pairs in another order.
    The checks there are named for the output."""
    b, check = r.b, r.check
    if not r.can("display-modes"):
        return
    home, others = outputs_offered(b)
    if not home or not others:
        r.skip("the GPU sections on another output",
               "one output connected")
        return
    rows = 1 + len(others)
    for name in others:
        logged = len(b.dump(1 << 30))
        if not check("the window is moved to %s for the GPU sections" % name,
                     to_output(r, name, rows), b.refresh()["out"]):
            continue
        # A ranking orders the list for this display and says it again
        # (rank.c): the rows are read from that.
        if getattr(b, "ranked", False):
            b.until(lambda: sum(1 for ln in b.dump(1 << 30)[logged:]
                                if re.match(r"GPU \d+: ", ln))
                    >= len(gpu_rows(b)), timeout=10.0 * b.patience())
        time.sleep(2.0 * b.patience())
        r.where = " on %s" % name
        try:
            gpu_moves(r)
            gpu_driver_moves(r)
            gpu_every_pair(r)
        finally:
            r.where = ""
        check("and the window goes back to %s" % home,
              to_output(r, home, rows), b.refresh()["out"])
        time.sleep(2.0 * b.patience())


def sizes_after_move(r, area):
    """The sizes again, on the GPU the window moved to: fullscreen and
    back, maximize and restore, and the border carried out with the frame
    looked at. On Win32 the frames go into a child window from the first
    move on (win32_present_hwnd()), which the round's size sections, run
    before it, never sized. The area left is returned, for the moves after
    to be drawn at."""
    b, check = r.b, r.check
    f11 = lambda: b.tap("F11")
    r.transition(f11)
    st = b.refresh()
    check("after the move, F11 goes fullscreen", st["fs"] is True,
          str(st["area"]))
    r.transition(f11)
    st = b.refresh()
    check("and leaves it at the area it had",
          st["fs"] is False and st["area"] == area,
          "%s -> %s" % (area, st["area"]))
    if r.can("resizable"):
        r.transition(lambda: b.click(*r.btn("max", b.refresh()["area"][0])))
        st = b.refresh()
        check("after the move, the maximize button maximizes",
              st["max"] is True, str(st["area"]))
        r.transition(lambda: b.click(*r.btn("max", b.refresh()["area"][0])))
        st = b.refresh()
        check("and restore returns to the area it had",
              st["max"] is False and st["area"] == area,
              "%s -> %s" % (area, st["area"]))
    if r.can("edge-resize"):
        gx, gy = room_to_grow(r, 120, 80)
        w, h = area[0], area[1] + BAR_H
        m = b.mark()
        b.drag(w - 2, h - 2, w - 2 + gx, h - 2 + gy, steps=12)
        b.settle("resize", m, 1.5)
        b.quiet(PAUSE)
        grown = b.refresh()["area"]
        # Grown both ways, by however much the screen had room for.
        check("after the move, the bottom-right edge resizes",
              grown[0] > area[0] and grown[1] > area[1],
              "%dx%d -> %dx%d" % (area + grown))
        if r.can("read-pixels"):
            black = b.content_black_pct()
            if black is None:
                r.skip("and the frame fills the window",
                       "no picture of the window")
            else:
                check("and the frame fills the window", black < 40.0,
                      "%.0f%% black" % black)
        area = grown
    return area


def display_modes(r):
    """Where the window is an output: another of its modes from the
    titlebar's list and back, and another output and back. The open line
    says how many modes the output has and which other outputs there are;
    the lists are largest first, so the first row is the largest mode."""
    b, check = r.b, r.check
    said = "\n".join(b.dump(1 << 30))
    m = re.search(r"KMS: (\S+) on \S+, \d+x\d+ at [\d.]+ Hz, (\d+) "
                  r"modes? on offer; other outputs: (.*)", said)
    if not check("the open line names the modes and the outputs", bool(m),
                 m.group(0) if m else "none"):
        return
    home, modes, others = m.group(1), int(m.group(2)), m.group(3).strip()
    area0 = b.refresh()["area"]
    if modes < 2:
        r.skip("a display mode picked from the titlebar", "one mode")
    else:
        # Read after the pick and not off the log's tail, which the lines
        # after the switch may have run past by then.
        before = len(b.dump(1 << 30))
        pick_row(r, MODE_BOX_DX, 1)
        b.until(lambda: any("KMS: %s at" % home in ln
                            for ln in b.dump(1 << 30)[before:]),
                timeout=10.0)
        time.sleep(2.0 * b.patience())
        st = b.refresh()
        set_to = [ln for ln in b.dump(1 << 30)[before:]
                  if ("KMS: %s at" % home) in ln]
        check("the second row sets that mode", bool(set_to),
              set_to[-1] if set_to else "nothing logged")
        check("and the window is still the output's",
              is_output(st["out"], home) and b.alive(),
              "%s on %s" % (st["area"], st["out"]))
        # The first row is the largest, which is the one the window opened
        # at only where that is the output's preferred mode: the size is
        # held against the mode the library says it set.
        before = len(b.dump(1 << 30))
        pick_row(r, MODE_BOX_DX, 0)
        b.until(lambda: any("KMS: %s at" % home in ln
                            for ln in b.dump(1 << 30)[before:]),
                timeout=10.0)
        time.sleep(2.0 * b.patience())
        set_to = [re.search(r"at (\d+)x(\d+),", ln)
                  for ln in b.dump(1 << 30)[before:]
                  if ("KMS: %s at" % home) in ln]
        st = b.refresh()
        mw, mh = (int(set_to[-1].group(1)), int(set_to[-1].group(2))) \
            if set_to and set_to[-1] else (0, 0)
        check("the first row sets the largest",
              st["area"][0] == mw and 0 < mh - st["area"][1] <= BAR_H + 8,
              "%s for %dx%d" % (st["area"], mw, mh))
    if others == "none":
        r.skip("the window moved to another output", "one output connected")
        return
    # The rows are the card's order, which the line does not give: the row
    # that is not the one the window is on is found by trying.
    moved = None
    out_dx = OUTPUT_BOX_DX + (GPU_BOX_SPAN if gpus_listed(b) else 0)
    for row in (0, 1):
        mk = pick_row(r, out_dx, row)
        b.until(lambda: b.since(mk, "output"), timeout=10.0)
        got = b.since(mk, "output")
        if got and not is_output(b.refresh()["out"], home):
            moved = row
            break
        # Home's row: the pick shut the list and moved nothing.
        time.sleep(NOT_A_DOUBLE_CLICK)
    st = b.refresh()
    check("the output list moves the window", moved is not None,
          "%s on %s" % (st["area"], st["out"]))
    if moved is None:
        return
    time.sleep(2.0 * b.patience())
    check("and it is drawn there", b.alive())
    pick_row(r, out_dx, 1 - moved)
    b.until(lambda: is_output(b.refresh()["out"], home), timeout=10.0)
    time.sleep(2.0 * b.patience())
    st = b.refresh()
    check("and goes back", is_output(st["out"], home) and
          st["area"] == area0, "%s on %s" % (st["area"], st["out"]))
    if r.can("forced-hotplug"):
        hotplug(r, home, others.split(",")[0].strip(), area0)
    else:
        r.skip("the window leaves an output unplugged",
               "nothing here can unplug one")


def hotplug(r, home, other, area0):
    """The window's output unplugged: the window moves to the other, and the
    output plugged back is listed again without the window moving; then the
    other unplugged, and it comes home. Each connector put back to what its
    cable says whatever happens."""
    b, check = r.b, r.check

    def moved_to(name, mark):
        b.until(lambda: any(is_output(e["parts"][-1], name)
                            for e in b.since(mark, "output")),
                timeout=10.0)
        time.sleep(2.0 * b.patience())
        return is_output(b.refresh()["out"], name)

    titles = ("an unplugged output sends the window to another",
              "and plugged back it is listed again, as the one it was",
              "without the window moving",
              "the other unplugged, it comes home")
    try:
        m = b.mark()
        # The force first, listened for: where the kernel sends nothing for
        # it, nothing tells the program either, and the section is the
        # kernel's to fail, not the library's (Bare.force_output_heard()).
        if not b.force_output_heard(home, "off"):
            for title in titles:
                r.skip(title, "a forced unplug sends no hotplug uevent on "
                       "this machine's kernel, so nothing tells the program")
            return
        check("an unplugged output sends the window to another",
              moved_to(other, m) and b.alive(),
              "%s on %s" % (b.refresh()["area"], b.refresh()["out"]))
        before = len(b.dump(1 << 30))
        b.force_output(home, "detect")
        said = "KMS: %s plugged in" % home
        b.until(lambda: any(said in ln for ln in b.dump(1 << 30)[before:]),
                timeout=10.0)
        back = [ln for ln in b.dump(1 << 30)[before:] if said in ln]
        check("and plugged back it is listed again, as the one it was",
              bool(back) and "again" in back[-1],
              back[-1] if back else "nothing logged")
        check("without the window moving",
              is_output(b.refresh()["out"], other))
        m = b.mark()
        b.force_output(other, "off")
        came = moved_to(home, m)
        st = b.refresh()
        check("the other unplugged, it comes home",
              came and st["area"] == area0,
              "%s on %s" % (st["area"], st["out"]))
    finally:
        # Each put back whatever the one before did.
        failed = None
        for name in (home, other):
            try:
                b.force_output(name, "detect")
            except Exception as e:
                failed = failed or e
        if failed:
            raise failed
    time.sleep(2.0 * b.patience())
    check("and draws on after it all", b.alive())


def maximize_and_restore(r, st, area0, base, mon, f11):
    """The maximize button, fullscreen on top of it, and the way back: what
    a window that can be another size than its output's does with one."""
    b, check = r.b, r.check
    w = st["area"][0]
    r.transition(lambda: b.click(*r.btn("max", w)))
    st = b.refresh()
    check("the maximize button maximizes", st["max"] is True, str(st["area"]))
    maxarea = st["area"]
    if mon:
        mx = b.rect()
        check("maximized fits inside its monitor",
              mx[0] >= mon[0] - 2 and mx[1] >= mon[1] - 2 and
              mx[0] + mx[2] <= mon[0] + mon[2] + 2 and
              mx[1] + mx[3] <= mon[1] + mon[3] + 2, "%s in %s" % (mx, mon))
        check("maximized fills all but the panels",
              mx[2] * mx[3] > 0.85 * mon[2] * mon[3], str(mx))
        check("maximized is larger than the restored size",
              mx[2] > base[2], str(mx))
    r.shot("maximized")

    if b.MAXIMIZED_LOST_IN_FULLSCREEN:
        for name in ("fullscreen from maximized goes fullscreen",
                     "leaving fullscreen returns to maximized"):
            r.skip(name, b.MAXIMIZED_LOST_IN_FULLSCREEN)
    else:
        r.transition(f11)
        st = b.refresh()
        check("fullscreen from maximized goes fullscreen", st["fs"] is True,
              str(st["area"]))
        r.transition(f11)
        st = b.refresh()
        check("leaving fullscreen returns to maximized",
              st["max"] is True and st["area"] == maxarea,
              "%s -> %s" % (maxarea, st["area"]))

    w = st["area"][0]
    r.transition(lambda: b.click(*r.btn("max", w)))
    st = b.refresh()
    check("restore returns to the original area", st["area"] == area0,
          "%s -> %s" % (area0, st["area"]))


def run(r):
    b, check = r.b, r.check

    check("the testbed started", b.alive())
    st = b.refresh()
    check("events reach the harness", st is not None,
          str(st["area"]) if st else "none")
    if not st:
        return
    area0 = st["area"]
    if r.can("resizable"):
        # The default, or what of it the screen has room for: a window opens
        # no larger than its monitor's work area, and at 150% a 1920x1080
        # panel is 1280x720 points before the taskbar.
        want = AREA0
        if r.can("server-rect"):
            g = st["scale"] / 120.0
            work = b.work_area()
            want = (min(AREA0[0], int(work[2] / g)),
                    min(AREA0[1], int(work[3] / g) - BAR_H))
        check("opens at the default area", near(area0[0], want[0])
              and near(area0[1], want[1]), "%s, wanted %s" % (area0, want))
    else:
        # The output whole, less the bar, and maximized from the start: the
        # one size there is.
        want = (st["output"][0], st["output"][1] - BAR_H)
        check("opens at the size of its output", area0 == want,
              "%s, wanted %s" % (area0, want))
        check("and is maximized from the start", st["max"] is True)
    r.shot("base")

    # ---- what the window server made of it --------------------------------
    if r.can("server-rect"):
        base = b.rect()
        mon = b.monitor()
        print("  rect %s  monitor %s" % (base, mon), flush=True)
        # The rectangle is the server's, in pixels; the area and the bar
        # are points.
        g = st["scale"] / 120.0
        check("opens at the size the area implies",
              near(base[2], area0[0] * g)
              and near(base[3], (area0[1] + BAR_H) * g), str(base))
        if not check("the window took the input focus", b.has_focus()):
            # A window without the focus is handed nothing, so the rest of
            # the round would be one fact about the desktop restated as a
            # dozen plausible findings about window management.
            print("  (stopping here -- the window never took the focus, so "
                  "nothing below would be about the library)", flush=True)
            return
        # A window that opened at all the room there is has none to be
        # dragged, grown or maximized into, and the rest of the round is
        # those. Taken in by its own border first, which is a check too.
        work = b.work_area()
        short = (ROOM[0] - (work[2] / g - area0[0]),
                 ROOM[1] - (work[3] / g - area0[1] - BAR_H))
        if short[0] > 0 or short[1] > 0:
            w, h = area0[0], area0[1] + BAR_H
            m = b.mark()
            b.drag(w - 2, h - 2, w - 2 - max(0, int(short[0])),
                   h - 2 - max(0, int(short[1])), steps=12)
            b.settle("resize", m, 1.5)
            b.quiet(PAUSE)
            st = b.refresh()
            check("a window with no room is taken in by its border",
                  st["area"][0] <= area0[0] and st["area"][1] <= area0[1]
                  and st["area"] != area0,
                  "%dx%d -> %dx%d" % (area0 + st["area"]))
            area0 = st["area"]
            base = b.rect_still()
            print("  taken in to %s, rect %s" % (area0, base), flush=True)
    else:
        base = mon = None
        r.skip("the window's rectangle on screen",
               "a Wayland client is never told where it is")
        # The same room, made by a harness that can find the window on the
        # screen without a server to ask (Environment.make_room()).
        if r.can("resizable") and b.make_room(ROOM):
            b.quiet(PAUSE)
            st = b.refresh()
            check("a window with no room is taken in by its border",
                  st["area"][0] <= area0[0] and st["area"][1] <= area0[1]
                  and st["area"] != area0,
                  "%dx%d -> %dx%d" % (area0 + st["area"]))
            area0 = st["area"]

    if r.can("wm-props"):
        ext = b.frame_extents()
        check("no frame extents are published", not any(ext), str(ext))
        drew, why = b.reparented()
        check("the manager drew no frame", not drew, why)
        cls = b.wm_class()
        check("WM_CLASS is set", cls == ("fenestal", "Fenestal"), str(cls))

    # ---- fullscreen -------------------------------------------------------
    if r.can("composited"):
        # What the *compositor* made of the frame, which a client cannot
        # answer for itself: a surface with the wrong window geometry, or one
        # under a panel, draws a perfect frame nobody sees.
        check_bar(r, "the compositor shows the titlebar", st)

    f11 = lambda: b.tap("F11")
    r.transition(f11)
    st = b.refresh()
    check("F11 goes fullscreen", st["fs"] is True, str(st["area"]))
    # Which monitor the round is on, asked of the application: a fullscreen
    # window *is* its output, so the area it reports is that monitor's size.
    # The only way the Wayland environment can answer it at all.
    tgt = b.target_monitor()
    if tgt and b.PIN_UNHELD:
        r.skip("fullscreen is the monitor the round is pinned to",
               b.PIN_UNHELD)
    elif tgt:
        px = st["px"]
        check("fullscreen is the monitor the round is pinned to",
              px == (tgt[3], tgt[4]),
              "%s wanted %s %dx%d" % (px, tgt[0], tgt[3], tgt[4]))
    if mon:
        fs = b.rect()
        check("fullscreen covers the monitor", rect_eq(fs, mon), str(fs))
    if r.can("composited"):
        check_bar(r, "no titlebar on screen while fullscreen", st,
                  present=False)

    r.transition(f11)
    st = b.refresh()
    check("F11 leaves fullscreen", st["fs"] is False, str(st["area"]))
    check("and the area comes back exactly", st["area"] == area0,
          "%s -> %s" % (area0, st["area"]))
    if r.can("composited"):
        check_bar(r, "the titlebar is back after fullscreen", st)

    # Three more cycles. On Win32 the bug this catches grew the window by a
    # frame each time; elsewhere the manager or compositor owns the restore, so
    # it is asking whether *that* drifts -- a different mechanism, the same
    # thing going wrong if it does.
    for _ in range(3):
        r.transition(f11)
        r.transition(f11)
    st = b.refresh()
    check("four fullscreen cycles do not drift", st["area"] == area0,
          "%s -> %s" % (area0, st["area"]))

    # ---- the titlebar's own buttons ---------------------------------------
    if r.can("resizable"):
        maximize_and_restore(r, st, area0, base, mon, f11)
    else:
        r.skip("maximize and restore", "the window is its output")

    # ---- the caption drags the window -------------------------------------
    #
    # Two halves, and only the first is the server's to answer: where the
    # window went. That it is still the size it was is the application's own
    # report, so every environment is asked that one.
    was = b.rect() if r.can("server-rect") else None
    m = b.mark()
    # By the title's end of the bar: its middle is the controls' at the
    # default width, the output box and the checkboxes left of it, and in a
    # window taken in, narrower, the rest of the run, which is
    # right-aligned and does not shrink -- a press there is theirs and
    # drags nothing.
    grip = B.caption_x(area0[0])
    b.drag(grip, BTN_ROW_Y, grip + 180, BTN_ROW_Y + 120, steps=14)
    if was:
        moved = b.rect_still()
        check("the titlebar drags the window",
              abs(moved[0] - was[0]) > 60 and abs(moved[1] - was[1]) > 40,
              "%s -> %s" % (was[:2], moved[:2]))
    b.quiet(PAUSE)
    st = b.refresh()
    sizes = sorted({tuple(e["parts"][1:3]) for e in b.since(m, "resize")})
    check("dragging does not resize",
          st["area"] == area0 and not sizes,
          "%dx%d -> %dx%d%s" % (area0 + st["area"]
                               + (", through %s" % sizes if sizes else "",)))
    r.shot("dragged")

    # ---- resizing ---------------------------------------------------------
    if r.can("edge-resize"):
        # The window's own bottom-right corner, one point inside it, which is
        # the resize edge the manager owns.
        #
        # Read off the area the application says it was handed and not off
        # the server's rectangle. The claim is that the library was told
        # and rebuilt, which the echo is the witness to and a rectangle is
        # not; and an environment with no server to ask is then asked the same.
        def sized(mark, pause=PAUSE):
            """The area once a drag's resizes are over. The manager acts
            on a drag some time after it ends, so the first of them is
            waited for before the silence that follows the last."""
            b.settle("resize", mark, 1.5)
            b.quiet(pause)
            return b.refresh()["area"]

        gx, gy = room_to_grow(r, 160, 120)
        st = b.refresh()
        before = st["area"]
        w, h = before[0], before[1] + BAR_H
        m = b.mark()
        b.drag(w - 2, h - 2, w - 2 + gx, h - 2 + gy, steps=12)
        grown = sized(m)
        check("the bottom-right edge resizes",
              grown[0] > before[0] + 60 and grown[1] > before[1] + 40,
              "%dx%d -> %dx%d" % (before + grown))

        # Eight back to back, which is what a swapchain rebuild per
        # configure is stressed by, and then a look at what is on the screen:
        # "alive" is also true of a window showing an image from three sizes
        # ago.
        area = grown
        for i in range(8):
            w, h = area[0], area[1] + BAR_H
            d = 40 if i % 2 == 0 else -40
            m = b.mark()
            b.drag(w - 2, h - 2, w - 2 + d, h - 2 + d, steps=6, settle=0.25)
            # The next one aims at the corner the application reports now.
            area = sized(m, QUIET)
        # The frames for the last one presented.
        b.quiet(PAUSE)
        check("alive after eight drag-resizes", b.alive(),
              "%dx%d" % area)
        if r.can("read-pixels"):
            black = b.content_black_pct()
            if black is None:
                r.skip("the frame still fills the window after a resize",
                       "no picture of the window")
            else:
                check("the frame still fills the window after a resize",
                      black < 40.0, "%.0f%% black" % black)

        # The floor from below: the corner carried to the window's own left
        # edge, inside the window so that no screen has to have room for
        # it, and the width the window stops at. Then back.
        if b.FLOOR_UNHELD:
            r.skip("the window goes no narrower than its floor",
                   b.FLOOR_UNHELD)
        else:
            # The corner on the screen first: the drags above may have left
            # it past the monitor's edge, where no pointer can take it, or
            # under a taskbar, which the default spare clears.
            room_to_grow(r, 0, 0)
            before = b.refresh()["area"]
            w, h = before[0], before[1] + BAR_H
            m = b.mark()
            b.drag(w - 2, h - 2, 40, h - 2, steps=12)
            least = sized(m)
            # Where the library sizes its own window the floor is rounded
            # up to the step it resizes in (Environment.FLOOR_STEP).
            floor = FLOOR_W
            if b.FLOOR_STEP:
                g = b.refresh()["scale"] / 120.0
                px = -(-round(FLOOR_W * g) // b.FLOOR_STEP) * b.FLOOR_STEP
                floor = round(px / g)
            check("the window goes no narrower than its floor",
                  near(least[0], floor), "%d wide, the floor %d" % (
                      least[0], floor))
            m = b.mark()
            b.drag(least[0] - 2, least[1] + BAR_H - 2, w - 2, h - 2,
                   steps=12)
            back = sized(m)
            check("and grows back from it", near(back[0], before[0], 4),
                  "%dx%d -> %dx%d" % (before + back))

    if r.can("grab-ring"):
        # The transparent margin outside the window, which is the Wayland
        # backend's own way of offering a resize edge -- see deco_margin().
        # Negative window points are inside it.
        st = b.refresh()
        before = st["area"]
        b.drag(-3, st["area"][1] // 2, -160, st["area"][1] // 2, steps=10)
        b.quiet(PAUSE)
        st = b.refresh()
        check("dragging the left grab ring resizes",
              st["area"][0] > before[0] + 60,
              "%s -> %s" % (before, st["area"]))
        before = st["area"]
        b.drag(st["area"][0] // 2, st["area"][1] + BAR_H + 3,
               st["area"][0] // 2, st["area"][1] + BAR_H + 130, steps=10)
        b.quiet(PAUSE)
        st = b.refresh()
        check("dragging the bottom grab ring resizes",
              st["area"][1] > before[1] + 40,
              "%s -> %s" % (before, st["area"]))

    # Back to a known size for everything below, whatever the drags left.
    st = b.refresh()

    cross_monitors(r)
    drawn_pointer(r)

    # ---- pointer capture --------------------------------------------------
    st = b.refresh()
    mid = (st["area"][0] // 2, BAR_H + st["area"][1] // 2)
    r.transition(lambda: b.click(*mid), "capture", timeout=2.0)
    st = b.refresh()
    captured = check("a click in the scene captures the pointer",
                     st["cap"] is True, str(st["cap"]))

    if captured:
        # By a delta and not to a place, which is the only thing that means
        # anything to a captured pointer, and the only thing the portal
        # offers in any case.
        m = b.mark()
        b.move_by(40, 30)
        b.until(lambda: b.since(m, "relative"))
        rel = b.since(m, "relative")
        check("captured motion arrives as relative", bool(rel),
              " ".join(rel[-1]["parts"][:3]) if rel else "none")

        # A captured pointer's click is the scene's wherever its hidden
        # cursor has drifted. Pushed to the top-left -- on Win32, where it is
        # confined to the client area, that is the bar and a resize corner --
        # a press must still reach the caller and move nothing: a window
        # manager handed it would start a move or a resize instead.
        before = b.rect() if r.can("server-rect") else None
        b.move_by(-4000, -4000)
        m = b.mark()
        b.click_here()
        b.until(lambda: b.since(m, "button"), timeout=2.0)
        got = b.since(m, "button")
        st = b.refresh()
        moved = before is not None and b.rect() != before
        check("a captured click at the bar's corner is the scene's",
              bool(got) and st["cap"] is True and not moved,
              "%d button events, captured %s%s" % (
                  len(got), st["cap"], ", the window moved" if moved else ""))

        r.transition(f11)
        st = b.refresh()
        check("still captured across fullscreen", st["cap"] is True,
              str(st["cap"]))
        r.transition(f11)
        st = b.refresh()
        check("still captured coming back", st["cap"] is True, str(st["cap"]))

        r.transition(lambda: b.tap("F9"), "capture", timeout=2.0)
        st = b.refresh()
        check("F9 releases the pointer", st["cap"] is False, str(st["cap"]))

        if r.can("server-rect"):
            px, py = b.pointer_pos()
            wx, wy, ww, wh = b.rect()
            check("the released cursor is inside the window",
                  wx <= px < wx + ww and wy <= py < wy + wh,
                  "pointer %s window %s" % ((px, py), (wx, wy, ww, wh)))
        r.shot("released")
    else:
        # Reported and not dropped: a block that simply vanishes shrinks the
        # round's total, which reads as a smaller round and not a broken one.
        for name in ("captured motion arrives as relative",
                     "a captured click at the bar's corner is the scene's",
                     "still captured across fullscreen",
                     "still captured coming back",
                     "F9 releases the pointer"):
            r.skip(name, "the pointer was never captured")
        if r.can("server-rect"):
            r.skip("the released cursor is inside the window",
                   "the pointer was never captured")

    # ---- keys the library binds, and keys it does not ---------------------
    # Twice, so the scene clock behind F2 ends where it started: the
    # minimize section's CPU figure means something only from a known one.
    m = b.mark()
    b.press("F2")
    b.press("F2")
    got = b.since(m, "key")
    check("F2 reaches the caller", len(got) >= 2, "%d events" % len(got))

    # F5, a key beyond the first sixteen. What is checked is the *name*: a
    # key that arrives as "unknown" has reached the caller and still
    # failed. Twice, as F2: it toggles the library's metrics panel, which
    # keeps a window at rest drawing while it is shown, and the sections
    # after measure that window.
    m = b.mark()
    b.press("F5")
    b.press("F5")
    got = [e["parts"][1] for e in b.since(m, "key")]
    check("a key beyond the first sixteen is named", got.count("F5") >= 4,
          " ".join(got) or "none")

    m = b.mark()
    h = b.key_down("W")
    time.sleep(HOLD)
    b.let_go(h)
    edges = [e["parts"][2] for e in b.since(m, "key")]
    check("a held key delivers both edges",
          "press" in edges and "release" in edges, " ".join(edges))

    # Four keys held in turn, as a hand flies a camera. The testbed drains
    # at the top of on_record and again in on_overlay, so each edge is
    # polled by one of the two, and none may be dropped between them or
    # taken twice. No repeats: text entry is off.
    flight = ("W", "D", "S", "A")
    m = b.mark()
    for key in flight:
        h = b.key_down(key)
        time.sleep(HOLD)
        b.let_go(h)
    b.quiet()
    edges = [e["parts"][2] for e in b.since(m, "key")
             if e["parts"][1] in flight]
    check("keys held in turn are each polled once, by one of two drains",
          edges.count("press") == len(flight)
          and edges.count("release") == len(flight)
          and "repeat" not in edges, " ".join(edges))

    st = b.refresh()
    b.move_to(st["area"][0] // 2, BAR_H + st["area"][1] // 2)
    m = b.mark()
    b.scroll(-2)

    def notches():
        return sum(float(e["parts"][2]) for e in b.since(m, "scroll"))

    # Until both steps are in, not the first event: a window at rest draws
    # the frame that echoes the first as soon as it arrives, and the second
    # of a pair the window server split comes a frame later.
    b.until(lambda: abs(notches() + 2.0) < 0.01)
    got = b.since(m, "scroll")
    check("scroll reaches the caller", bool(got),
          " ".join(got[-1]["parts"][:3]) if got else "none")
    # In notches on every platform (fenestal.h): two wheel steps up are -2,
    # summed over however many events the window server split them into.
    dy = notches()
    check("and it is counted in notches", abs(dy + 2.0) < 0.01,
          "dy %.2f for two steps" % dy)

    # ---- the titlebar's own controls --------------------------------------
    #
    # Three checkboxes and a size drop-down, left of the window buttons: from
    # the fullscreen button's centre, 12 the group gap, and 40 more puts this
    # inside the drop-down. What is being checked is the library's half -- that
    # a press there is delivered to the caller as well as acted on, rather than
    # swallowed or treated as a window drag.
    st = b.refresh()
    before = st["area"]
    w = before[0]
    combo_x = r.btn("full", w)[0] - 12 - 40
    m = b.mark()
    b.click(combo_x, BTN_ROW_Y)
    b.until(lambda: b.since(m, "button"))
    got = b.since(m, "button")
    check("a press on the titlebar controls reaches the caller", bool(got),
          " ".join(got[-1]["parts"][:4]) if got else "none")
    st = b.refresh()
    check("and did not resize the window", st["area"] == before,
          "%s -> %s" % (before, st["area"]))

    # And shut the list again: while it is open ctl_modal() makes *every*
    # press the controls', and the minimize click below would land on a
    # drop-down and not on a button.
    time.sleep(NOT_A_DOUBLE_CLICK)
    b.click(combo_x, BTN_ROW_Y)
    b.quiet()

    # ---- the output's modes, and its outputs ------------------------------
    if r.can("display-modes"):
        display_modes(r)
    else:
        r.skip("a display mode picked from the titlebar",
               "a window server sizes and places the window")

    # ---- another GPU, from the titlebar ------------------------------------
    # Every switch with the scene moving (F2), as the window is used: a
    # rebuild under a frame in flight, not under a still one. Pressed again
    # after, so the clock ends where it started.
    m = b.mark()
    b.press("F2")
    check("the scene moves for the switches (F2)", bool(b.since(m, "key")))
    gpu_moves(r)
    gpu_driver_moves(r)
    gpu_every_pair(r)
    gpu_on_other_outputs(r)
    b.press("F2")

    # ---- how long an unbound event takes to arrive ------------------------
    #
    # on_overlay is where an application drains the queue, and it runs only
    # from a re-record. So an event that moves no field of FnlDrawState can
    # only be delivered on a frame something else moved, and on a still
    # window there is nothing else. F1, because the library binds nothing to
    # it. Delivered through the gate the median is some 15 ms; without it,
    # over a second.
    #
    # Pressed and released by hand and not through tap(), whose hold would
    # be inside the figure.
    def timed_f1(lead):
        lat = []
        for _ in range(8):
            time.sleep(lead)
            m = b.mark()
            t0 = time.monotonic()
            h = b.key_down("F1")
            got = b.wait_for("key", m, timeout=1.5)
            if got:
                lat.append((time.monotonic() - t0) * 1e3)
            b.key_up(h)
        return lat

    def delivered(lat):
        return "%d/8 delivered, median %s ms" % (
            len(lat), "%.0f" % statistics.median(lat) if lat else "n/a")

    lat = timed_f1(STILL)
    check("the event queue is drained every frame", len(lat) >= 7,
          delivered(lat))

    # And on a window that is drawing, where a still one now draws nothing
    # and is woken by the key itself: the frame loop's own pacing is then in
    # the figure, which is the one to compare between environments.
    b.press("F2")
    lat = timed_f1(HOLD)
    b.press("F2")
    b.quiet()
    check("and while the scene moves", len(lat) >= 7, delivered(lat))

    # ---- text entry ---------------------------------------------------------
    #
    # F3 is the testbed's switch for it. What is asserted is what holds under
    # any layout, because a round does not get to choose the desk it runs on:
    # that a key types one character, that Shift types the other case of the
    # same one, that Space is a space, that a shortcut types nothing. Which
    # letter the A position prints is the layout's business and is only
    # reported.
    def typed(mark):
        return [int(e["parts"][1][2:], 16) for e in b.since(mark, "text")]

    def chord(mod, key):
        # No pauses between the edges: each is echoed before the next is
        # sent, and the order they are delivered in is the order sent.
        h = b.key_down(mod)
        b.press(key)
        b.let_go(h)
        b.quiet()

    def type_key(key):
        """Both edges of a key, and then whatever it typed or did not."""
        b.press(key)
        b.quiet()

    b.press("F3")
    m = b.mark()
    type_key("A")
    low = typed(m)
    kinds = [e["kind"] for e in b.since(m) if e["kind"] in ("key", "text")]
    check("a key types one character under text entry", len(low) == 1,
          " ".join("U+%04X" % c for c in low) or "none")
    check("and the key arrives before what it typed",
          kinds[:2] == ["key", "text"], " ".join(kinds))

    m = b.mark()
    chord("LShift", "A")
    up = typed(m)
    # The other case of the same letter, whichever way round: with Caps Lock
    # on, Shift is what types the lower one.
    check("Shift types the other case of it",
          len(low) == 1 and len(up) == 1 and up != low and
          chr(up[0]).lower() == chr(low[0]).lower(),
          "%s -> %s" % (" ".join("U+%04X" % c for c in low),
                        " ".join("U+%04X" % c for c in up)))

    m = b.mark()
    type_key("Space")
    got = typed(m)
    check("Space types a space", got == [0x20],
          " ".join("U+%04X" % c for c in got) or "none")

    m = b.mark()
    chord("LCtrl", "A")
    got = typed(m)
    check("a key under Ctrl types nothing", got == [] and
          len(b.since(m, "key")) >= 4,
          "%d typed, %d key events" % (len(got), len(b.since(m, "key"))))

    # A held key, which is three different mechanisms: the server's repeat on
    # X11, the system's on Win32, and a timer of the backend's own on Wayland
    # because a compositor sends none. Each repeat is a key event and the
    # character again, so the two counts differ by the first press.
    m = b.mark()
    h = b.key_down("A")
    time.sleep(REPEAT_HOLD_SLOW if b.slow else REPEAT_HOLD)
    released = b.let_go(h)
    b.quiet()
    reps = [e for e in b.since(m, "key") if e["parts"][2] == "repeat"]
    got = typed(m)
    # What else the hold was shown, when it was anything but the two edges:
    # the counts alone cannot tell a server that sent no repeats from a hold
    # something interrupted -- the focus leaving, a release ahead of the
    # key-up, another key.
    rest = [" ".join(e["parts"][:3]) for e in b.since(m)
            if e["kind"] != "text" and e not in reps]
    odd = [] if rest == ["key A press", "key A release"] else rest
    if not released:
        odd.append("no release after the key-up")
    check("a held key repeats, and types each time",
          len(reps) >= 3 and len(got) == len(reps) + 1,
          "%d repeats, %d typed" % (len(reps), len(got))
          + ("; with " + ", ".join(odd) if odd else ""))

    m = b.mark()
    # That it did *not* close the window has no event to wait for; what
    # there is to wait for is the key, and the echo going quiet after it.
    b.press("Escape")
    b.quiet(PAUSE)
    check("Escape is only a key while text entry is on",
          b.alive() and len(b.since(m, "key")) >= 2,
          "alive=%s, %d key events" % (b.alive(), len(b.since(m, "key"))))

    # ---- composing ---------------------------------------------------------
    #
    # Still under text entry. Three different composers, and each environment
    # is asked about the one it has.
    def composing(mark):
        return [" ".join(e["parts"][2:]) for e in b.since(mark, "compose")]

    if r.can("remap-key"):
        # The library's own: an accent key held, then joined to the letter
        # after it. No layout here has a dead key, so one is put on a spare
        # keycode for the duration -- which also asks whether the environment
        # notices its keymap being changed under it, since an answer read
        # from the layout it started with is no answer.
        undo = b.remap_spare(0xfe51, 0xfe57)    # dead_acute, dead_diaeresis
        try:
            m = b.mark()
            b.tap_spare()
            b.until(lambda: composing(m))
            b.quiet()
            held = composing(m)
            check("a dead key types nothing and is reported as held",
                  typed(m) == [] and held == ["\u00b4"],
                  "typed %s, composing %r" % (typed(m), held))
            type_key("E")
            got = typed(m)
            check("and the letter after it arrives accented", got == [0xe9],
                  " ".join("U+%04X" % c for c in got) or "none")
            check("and the composition is over", composing(m)[-1:] == [""],
                  repr(composing(m)))

            m = b.mark()
            h = b.key_down("LShift")
            b.tap_spare()
            b.let_go(h)
            type_key("U")
            got = typed(m)
            check("Shift picks the dead key's other accent", got == [0xfc],
                  " ".join("U+%04X" % c for c in got) or "none")

            m = b.mark()
            b.tap_spare()
            type_key("Space")
            got = typed(m)
            check("a dead key and a space type the accent itself",
                  got == [0xb4],
                  " ".join("U+%04X" % c for c in got) or "none")

            m = b.mark()
            b.tap_spare()
            type_key("X")
            got = typed(m)
            check("a letter the accent does not go on arrives bare",
                  got == [ord("x")] or got == [ord("X")],
                  " ".join("U+%04X" % c for c in got) or "none")
        finally:
            undo()

    if r.can("dead-layout"):
        # The system's. Nothing is reported while an accent is held and the
        # rules are the layout's, not the library's: an accent alone is
        # whatever that layout says, and a letter it does not go on arrives
        # behind it instead of bare. What is this environment's is to pass the
        # result on -- and to go on typing through whatever layout the
        # window is switched to, which is asked both ways.
        m = b.mark()
        type_key(b.DEAD_KEY)
        plain = typed(m)
        if not plain:
            # Held, then: let go of, before anything else is typed.
            type_key("Space")
            r.skip("a layout switched under the window is typed through",
                   "the layout it starts with has a dead key there already")
        else:
            undo = b.dead_layout()
            try:
                m = b.mark()
                type_key(b.DEAD_KEY)
                check("a layout switched under the window is typed through",
                      typed(m) == [],
                      "typed %s before and %s after, composing %r"
                      % (plain, typed(m), composing(m)))
                type_key("E")
                got = typed(m)
                check("and the letter after a dead key arrives accented",
                      got == [0xe9],
                      " ".join("U+%04X" % c for c in got) or "none")

                m = b.mark()
                chord("LShift", b.DEAD_KEY)
                type_key("U")
                got = typed(m)
                check("Shift picks the dead key's other accent",
                      got == [0xfc],
                      " ".join("U+%04X" % c for c in got) or "none")

                m = b.mark()
                type_key(b.DEAD_KEY)
                type_key("Space")
                got = typed(m)
                check("a dead key and a space type the accent itself",
                      got == [b.DEAD_ALONE],
                      " ".join("U+%04X" % c for c in got) or "none")

                m = b.mark()
                type_key(b.DEAD_KEY)
                type_key("X")
                got = typed(m)
                check("a letter the accent does not go on arrives after it",
                      got in ([b.DEAD_ALONE, ord("x")],
                              [b.DEAD_ALONE, ord("X")]),
                      " ".join("U+%04X" % c for c in got) or "none")
            finally:
                undo()
            m = b.mark()
            type_key(b.DEAD_KEY)
            got = typed(m)
            check("and the key types as it did once the layout is back",
                  got == plain,
                  " ".join("U+%04X" % c for c in got) or "none")

    if r.can("desk-layout"):
        # A real dead key from a real layout, on whatever composer this
        # environment has: the desktop is switched to a layout with one, under
        # the running window, and switched back. Only what every composer
        # agrees on is asserted -- nothing typed while the accent is held,
        # the accented letter after it, Shift's other accent -- and what an
        # accent and a space make is theirs to differ on: the library's
        # composer and Windows type the accent, a Compose table an
        # apostrophe.
        #
        # No key here is waited for in the echo: an input method takes the
        # dead key and the letter after it, and a key resent after silence
        # is a second character.
        def strike(key, mark):
            b.tap(key, confirm=False)
            b.quiet(PAUSE)
            return typed(mark)

        why = b.desk_layout_missing()
        m = b.mark()
        plain = [] if why else strike(b.DESK_DEAD_KEY, m)
        if why or not plain:
            r.skip("a dead key from the desk's own layout",
                   why or "the desk's layout has a dead key there already")
            if not why:
                strike("Space", b.mark())       # let go of the one held
        else:
            # A desk whose layouts cannot be read is left as it is, and
            # the section is the desk's to skip, not the program's to fail.
            try:
                undo = b.desk_dead_layout()
            except (RuntimeError, OSError,
                    subprocess.TimeoutExpired) as e:
                undo = None
                r.skip("a layout switched on the desk has its dead key",
                       str(e))
        if not why and plain and undo is not None:
            try:
                # The keymap's arrival is echoed by nothing, so the key
                # is asked until it has stopped typing: that is the
                # evidence the switch has reached the window.
                b.until(lambda: strike(b.DESK_DEAD_KEY, b.mark()) == [],
                        timeout=4.0)
                strike("Space", b.mark())
                m = b.mark()
                dead = strike(b.DESK_DEAD_KEY, m)
                check("a layout switched on the desk has its dead key",
                      dead == [], "typed %s before and %s after, composing %r"
                      % (plain, dead, composing(m)))
                got = strike("E", m)
                check("and the letter after it arrives accented",
                      got == [0xe9],
                      " ".join("U+%04X" % c for c in got) or "none")

                m = b.mark()
                h = b.key_down("LShift")
                b.tap(b.DESK_DEAD_KEY, confirm=False)
                b.let_go(h)
                got = strike("U", m)
                check("Shift picks the dead key's other accent",
                      got == [0xf9],
                      " ".join("U+%04X" % c for c in got) or "none")

                m = b.mark()
                strike(b.DESK_DEAD_KEY, m)
                got = strike("Space", m)
                check("a dead key and a space type one accent",
                      got in ([0xb4], [0x27]),
                      " ".join("U+%04X" % c for c in got) or "none")
                # With the compositor's input method left unbound, what
                # answered is the library's composer in a window, and the
                # accent is how the two are told apart.
                if r.can("input-method") and UNBOUND:
                    check("and it is the library's composer that typed it",
                          got == [0xb4],
                          " ".join("U+%04X" % c for c in got) or "none")
            finally:
                undo()
            # Not merely typing again: the switched layout's dead key types
            # too, every second strike, with the accent.
            b.until(lambda: strike(b.DESK_DEAD_KEY, b.mark()) == plain,
                    timeout=4.0)
            got = strike(b.DESK_DEAD_KEY, b.mark())
            check("and the key types as it did once the layout is back",
                  got == plain,
                  " ".join("U+%04X" % c for c in got) or "none")

    if r.can("input-method"):
        # The compositor's. Ctrl+Shift+U is the Unicode entry every IBus
        # engine carries, and it is the one sequence that needs no particular
        # layout and no candidate window: U, the hex digits, Space. What is
        # asserted is the protocol's whole shape -- the keys are swallowed,
        # what they have built so far is reported as it grows, the result is
        # typed once, and the composition ends.
        m = b.mark()
        c = b.key_down("LCtrl")
        try:
            s = b.key_down("LShift")
            try:
                b.tap("U")
            finally:
                b.key_up(s)
        finally:
            b.key_up(c)
        # The timeout is how long a desktop with no input method is given
        # to say nothing.
        if not b.until(lambda: composing(m), timeout=1.0):
            r.skip("the input method composes a character",
                   "FNL_NO_INPUT_METHOD left it unbound" if UNBOUND else
                   "no input method answered Ctrl+Shift+U on this desktop")
            b.press("Escape")
            b.quiet()
        else:
            # Not press(): the input method takes these keys, and what the
            # application is shown is the composition growing.
            b.tap("E")
            b.tap("9")
            b.until(lambda: len(composing(m)) >= 3)
            b.quiet()
            grown = composing(m)
            check("the input method reports what is being composed",
                  len(grown) >= 3 and typed(m) == [],
                  "%r, typed %s" % (grown, typed(m)))
            b.tap("Space")
            b.until(lambda: typed(m))
            b.quiet()
            got = typed(m)
            check("the input method composes a character", got == [0xe9],
                  " ".join("U+%04X" % c for c in got) or "none")
            check("and its composition ends with the commit",
                  composing(m)[-1:] == [""], repr(composing(m)[-2:]))
            m = b.mark()
            type_key("A")
            check("and plain keys type through the layout again",
                  len(typed(m)) == 1, str(typed(m)))

    if r.can("known-layout"):
        layout_types(r, typed)
    if r.can("layout-groups"):
        layout_groups(r, typed)

    b.press("F3")
    m = b.mark()
    type_key("A")
    got, keys = typed(m), b.since(m, "key")
    check("and nothing is typed once it is off", got == [] and len(keys) >= 2,
          "%d typed, %d key events" % (len(got), len(keys)))

    # ---- the clipboard ------------------------------------------------------
    #
    # Against another program, because a window that pastes what it copied
    # has shown only that it remembers a string -- and on Wayland that is
    # literally how it is answered. Which program that is, is the environment's
    # to say: it has to be on the desktop the window is on. F4 copies a known
    # word, F6 asks for whatever is there.
    why = b.clipboard_missing()
    if why:
        r.skip("the clipboard round trip", why)
    else:
        held = b.clipboard_get

        def holds(data, timeout=3.0):
            """Wait until the clipboard reads back as `data`; what it read
            last, either way."""
            end = time.time() + timeout * b.patience()
            seen = held()
            while seen != data and time.time() < end:
                time.sleep(0.05)        # poll: each look is a process
                seen = held()
            return seen

        def offer(data):
            b.clipboard_put(data)
            holds(data)
            time.sleep(SELECTION_BRIDGE)
            # Another client has just taken the selection, and on this desktop
            # that can cost the window the keyboard for a moment. Asked for
            # back rather than assumed: the key that follows is the check.
            b.focus()

        def pasted(mark, wait=3.0):
            ev = b.wait_for("clipboard", mark, timeout=wait)
            return (int(ev["parts"][1]), " ".join(ev["parts"][2:])) if ev \
                else None

        try:
            b.press("F4")
            seen = holds(COPIED.encode("utf-8")).decode("utf-8", "replace")
            check("a copy reaches another program", seen == COPIED, repr(seen))
            b.focus()

            word = "fenestal-pasted-\u00e9"
            offer(word.encode("utf-8"))
            m = b.mark()
            b.tap("F6")
            got = pasted(m)
            # In bytes, which is what the event counts: the last letter is two.
            check("a paste arrives from another program",
                  got == (len(word.encode("utf-8")), word), repr(got))

            # Past what one X property or one pipe buffer holds, so the
            # incremental transfer on X11 and the read loop on Wayland are
            # both what carries it. The length is the whole of the check: the
            # echo prints only the head.
            big = b"0123456789abcdef" * 40000
            offer(big)
            m = b.mark()
            b.tap("F6")
            got = pasted(m, wait=8.0)
            check("and a long one arrives whole",
                  got is not None and got[0] == len(big),
                  "%s of %d bytes" % (got[0] if got else None, len(big)))
        finally:
            b.clipboard_done()

    # ---- minimize, and the off-screen skip --------------------------------
    if r.can("unminimize"):
        # A window with nothing moving draws nothing (DESIGN *At rest*), so
        # the scene's clock is started: what is measured is a window
        # drawing every refresh, and what minimizing it takes away.
        b.press("F2")
        time.sleep(MOTION_LEAD)
        fm = r.frames_mark()
        visible = r.cpu_percent()
        visible_fps = r.frame_rate(fm)
        drawing = visible_fps is not None and visible_fps >= DRAWING_FPS
        st = b.refresh()
        # Waited for on the manager's own word, and clicked once more if that
        # word never comes: a wine titlebar is non-client area and echoes no
        # press, so nothing else says whether the click landed -- and a click
        # that did not leaves a visible window to be measured as a hidden one.
        for attempt in range(2):
            b.click(*r.btn("min", st["area"][0]))
            end = time.time() + 3.0
            while not b.minimized() and time.time() < end:
                time.sleep(0.05)        # poll
            if b.minimized():
                break
            print("  (the minimize click did not land; aiming again)",
                  flush=True)
            st = b.refresh()
        check("the minimize button minimizes", b.minimized())
        time.sleep(HIDDEN_LEAD)
        check("alive while minimized", b.alive())
        # The regression guard for the off-screen skip, which is what
        # FnlBackendVtbl::window_on_screen is for: by its frames, which no
        # resolution of the CPU's decides, and by what it costs. A parked
        # loop writes no frame line at all, which is no frame drawn.
        fm = r.frames_mark()
        idle = r.cpu_percent(HIDDEN_SAMPLE)
        hidden_fps = r.frame_rate(fm) or 0.0
        check("a minimized window draws nothing",
              drawing and hidden_fps < HIDDEN_FPS,
              "%s minimized, %s visible"
              % (fps_said(hidden_fps), fps_said(visible_fps)))
        # Both halves, because the drop is the claim and a drop from nothing
        # is not one: with the F2 above never having arrived, `idle < 5`
        # holds without anything having been measured. So the visible
        # window is shown drawing by its frames, and the drop is the ratio
        # -- a fast GPU at 60 Hz leaves a few per cent of a core, lavapipe
        # most of one -- or rest's own figure where drawing is so cheap
        # that the ratio asks for less.
        check("a minimized window costs no CPU",
              drawing and idle < 5.0
              and (idle < visible / 5.0 or idle <= AT_REST),
              "visible %.1f%% at %s -> minimized %.1f%%"
              % (visible, fps_said(visible_fps), idle))
        check("and it comes back when activated", b.unminimize())
        check("alive after restore from minimize", b.alive())
        # Guarded on its own capability: asking the server who holds the
        # focus is a "server-rect" question.
        if r.can("server-rect"):
            check("keyboard focus returns after a restore", b.has_focus())
        fm = r.frames_mark()
        time.sleep(3.0)             # the window the rate is taken over
        back = r.frame_rate(fm)
        # Against what it was, for the same reason: full rate is this
        # device's and no figure of ours.
        check("and goes back to full rate when restored",
              drawing and back is not None
              and back >= visible_fps * RESTORED_SHARE,
              "%s, %s before" % (fps_said(back), fps_said(visible_fps)))
        b.press("F2")           # the clock stopped again
    else:
        r.skip("minimize and restore", b.NO_UNMINIMIZE)

    # ---- and the way out --------------------------------------------------
    b.tap("Escape")
    end = time.time() + 3.0
    while b.alive() and time.time() < end:
        time.sleep(0.05)            # poll
    # Whether it went *on its own*, asked before anything is sent to make it
    # go. The exit code cannot say: the library turns a terminating signal
    # into the ordinary shutdown path, so the stop() below answers 0 whether
    # Escape closed the window or nothing ever reached it.
    gone = not b.alive()
    rc = b.stop()
    check("exits on Escape", gone and rc == 0, "gone=%s rc=%s" % (gone, rc))

    bad = complaints(b)
    check("log is clean", not bad, "; ".join(bad[:3]))


def complaints(b):
    """Every line of the program's log that is on no list: the testbed's log
    is the library's at every level with none named, so a warning is told
    from a notice only by that. The table LeakSanitizer prints at exit when
    a suppression matched is no complaint; a leak it did not suppress is a
    report above it."""
    lines = b.dump(1 << 30)
    table = set()
    for i, l in enumerate(lines):
        if (l != "Suppressions used:" or i == 0
                or not DASHES.match(lines[i - 1])):
            continue
        j = i + 1
        while j < len(lines) and (lines[j] == "  count      bytes template"
                                  or re.match(r"\s+\d+\s+\d+\s+\S+$",
                                              lines[j])):
            j += 1
        if j < len(lines) and DASHES.match(lines[j]):
            table.update(range(i - 1, j + 1))
    return [l for i, l in enumerate(lines)
            if l.strip() and i not in table and not expected(l)
            and not b.expected_line(l)]


DASHES = re.compile(r"-{20,}$")
# frame_times_log()'s line, whole.
FRAME_TIMES = re.compile(
    r"frame [\d.]+ ms: pace [\d.]+ fence [\d.]+ acquire [\d.]+ "
    r"record [\d.]+ "
    r"submit [\d.]+ present [\d.]+ rebuild [\d.]+ other [\d.]+ "
    r"\(draw [\d.]+, "
    r"hold [\d.]+, \d+ fps, gpu idle \d+/\d+, miss \d+/\d+, "
    r"period [\d.]+\)$")
# The same lines read for a rate (Round.frame_rate()): a window's mean
# frame time and its frame count, and frame.c's line for a window at rest,
# written once a FRAME_TIMES_WINDOW (0.5 s there) with nothing drawn, or
# with only the frames that showed the figures (rest_owed()).
FRAME_COUNT = re.compile(r"frame ([\d.]+) ms: .* gpu idle \d+/(\d+), ")
FRAME_AT_REST = re.compile(
    r"frame at rest: (?:0 fps, nothing drawn|(\d+) drawn to show the "
    r"figures, none for a change)$")
FRAME_TIMES_WINDOW = 0.5
# A --frame-times line spanning more than this holds time the window drew
# nothing in: four of its windows, past any device's frame (frame_rate()).
GAP_SPAN = 4 * FRAME_TIMES_WINDOW


def fps_said(rate):
    return "%.0f fps" % rate if rate is not None else "no frame lines"


# events_latency_publish()'s, beside it: fnl_input_latency_ms()'s figure.
INPUT_LATENCY = re.compile(
    r"input [\d.]+ ms over \d+ inputs, \d+ stamped, longest [\d.]+$")


def expected(line):
    """A line any environment prints in a round that went right; what one
    prints because of where it is is that environment's EXPECTED_LOG.

    `EV lost` is *not* filtered: it says the ring overflowed, which means
    the application stopped draining. The startup inventories are the
    library saying which backend it opened, which renderer it draws with,
    which devices it found, how it ranked them and which it took; the frame
    lines are its statistics, which the round turns on."""
    return (line.startswith("EV ") and not line.startswith("EV lost")
            or line.startswith("backend: ")
            or line.startswith("renderer: ")
            or line.startswith("Vulkan on")
            or re.match(r"EGL \d+\.\d+, .+$", line) is not None
            or re.match(r"GPU \d+: .* \(.*\)$", line) is not None
            or line.startswith("GPU in use: ")
            # The GL renderer on a device with no 4.5, drawing through its
            # 4.1 profile (DESIGN *OpenGL 4.1*): which profile, as the line
            # above says which device.
            or re.match(r"no OpenGL 4\.5 core context here \(\S+\); "
                        r"drawing with 4\.1 core$", line) is not None
            # And OpenGL's way onto a Mac's window (cgl.m), said once.
            or re.match(r"OpenGL draws into an IOSurface, which Metal "
                        r"copies onto .+$", line) is not None
            or re.match(r"moving to GPU \d+: ", line) is not None
            # A move to another kernel driver, as the library and the
            # helper narrate one that goes right (gpu_driver_moves()); a
            # refusal, a hold or a fallback is said otherwise and stays a
            # complaint.
            or re.match(r"GPU driver: \S+ is on \S+(, and can be moved to "
                        r"\S+)?$", line) is not None
            or re.match(r"GPU driver: drawn on .*, built in \d+ ms, while "
                        r"\S+ moves to \S+$", line) is not None
            or re.match(r"GPU driver: moving \S+ from \S+ to \S+$", line)
            is not None
            # NVIDIA's module loaded at the start by a move there and back
            # (app_gpu_drivers_start()), on a machine booted on nouveau.
            or line.startswith("GPU driver: NVIDIA's module is not loaded, "
                               "which NVIDIA's userspace then tries ")
            or re.match(r"GPU driver: fenestal-gpu-driver: \S+: \S+ to \S+ "
                        r"in \d+ ms \(let go \d+, bound \d+, nodes \d+\)$",
                        line) is not None
            or re.match(r"moving to .*, on \S+, the renderer built again on "
                        r"it \([^()]+\)$", line) is not None
            # The machine's GPU ranking read, and the entry opened on
            # (src/core/rank.c).
            or re.match(r"GPU rank: \d+ figures kept, from \S+; the window "
                        r"on \S+ \d+x\d+@\d+(, the list ordered alone)?$",
                        line)
            is not None
            or re.match(r"GPU rank: opening on rank \d+, \S+\|.+$",
                        line) is not None
            # The renderer drawn across such a move taking the copy path,
            # which is what keeps the output from going dark (DESIGN *The
            # veil*).
            or re.match(r"KMS: drawn across the move by the copy path onto "
                        r"\S+, so the output is not handed over for the "
                        r"landing$", line) is not None
            # Mesa's loader, shown a card it has no driver for -- NVIDIA's
            # own module has it -- whenever EGL's devices are listed.
            or re.match(r"pci id for fd \d+: [0-9a-f]{4}:[0-9a-f]{4}, "
                        r"driver \(null\)$", line) is not None
            or line.startswith("rebuilt on ")
            # FNL_X11_PRESENT_LOG's diagnostic, which a round asked for.
            or line.startswith("X11 present: ")
            # Vulkan on Win32 saying its frames go through DXGI
            # (present_dxgi.c).
            or line == ("the frames go through a DXGI swapchain, copied by "
                        "Direct3D 11")
            # Direct3D 12 on Win32 taking its frames through a Direct3D 11
            # swapchain where DWM composes its own, and back where that
            # is composed too (present_d3d11.c).
            or line in ("the frames go through a Direct3D 11 swapchain, "
                        "copied there",
                        "DWM composes this window's frames; they go through "
                        "a Direct3D 11 swapchain instead",
                        "DWM composes the Direct3D 11 swapchain's frames "
                        "too; back to this device's own")
            # The other renderers' GPUs, listed after the first frame.
            or re.match(r"\S.* lists \d+ GPUs? here, in \d+ ms$", line)
            is not None
            # A GPU whose VRAM its runtime suspend loses (NVK before
            # Turing), and the GPUs OpenGL left out: no 4.5, or a software
            # device whose display Mesa opens on the hardware one.
            or line.startswith("the glyph atlas is in system memory: ")
            or re.match(r"GPU left out: .*, which makes no OpenGL 4\.5 "
                        r"core context$", line) is not None
            or re.match(r"GPU left out: .*, whose display for this window "
                        r"EGL opens on another device$", line) is not None
            or line.startswith("EGL's own GPU makes no OpenGL 4.5 core "
                               "context; drawing on ")
            or re.match(r".+ is ranked below an integrated one: ",
                        line) is not None
            or FRAME_TIMES.match(line) is not None
            or INPUT_LATENCY.match(line) is not None
            or re.match(r"raw input: \d+ inputs matched to their reports, "
                        r"\d+ messages left to the tick$", line) is not None
            or FRAME_AT_REST.match(line) is not None
            # The display period put right where the swap did not block at
            # first (frame.c's PERIOD_RELEARN_RUN): OpenGL on a bare Xorg.
            or re.match(r"display period learnt again: [\d.]+ ms, where "
                        r"[\d.]+ stood -- ", line) is not None
            or "MESA" in line
            or "screenshot" in line.lower())


def lavapipe_lib():
    """Lavapipe's library as its ICD file names it here, or None."""
    import glob
    import json
    for icd in glob.glob("/usr/share/vulkan/icd.d/lvp_icd*.json"):
        with open(icd) as f:
            lib = json.load(f).get("ICD", {}).get("library_path")
        if lib:
            return lib
    return None


def lavapipe_pinned():
    """LD_PRELOAD for an asan round here: ASan's runtime, first as it
    must be, and lavapipe, kept loaded for bare_driver.py's LAVAPIPE_WHY.
    A move to another renderer destroys the Vulkan instance, the loader
    unloads lavapipe with it, and two 56-byte blocks its globals held read
    as leaked from an unknown module. A bare round puts its own (Bare.
    run_argv()); nothing where this desktop has no lavapipe."""
    import subprocess
    lib = lavapipe_lib()
    try:
        runtime = subprocess.run(["gcc", "-print-file-name=libasan.so"],
                                 capture_output=True, text=True,
                                 timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return {}
    # A name alone is the linker's to find, as the loader finds it.
    if not lib or not os.path.isabs(runtime):
        return {}
    return {"LD_PRELOAD": ":".join((os.path.realpath(runtime), lib))}


def lavapipe_supp():
    """tsan.supp and tsan-lavapipe.supp as one file, as Bare.install()
    sends a lavapipe machine both: ThreadSanitizer reads one."""
    path = os.path.join(OUT, "tsan-desk.supp")
    os.makedirs(OUT, exist_ok=True)
    with open(path, "w") as out:
        for src in ("tsan.supp", "tsan-lavapipe.supp"):
            with open(os.path.join(ROOT, src)) as f:
                out.write(f.read())
    return path


def given_back(hold):
    """Whether a hand took the machine's input back during the round: it
    wants the machine, so no round runs after this one."""
    if hold.by_hand:
        print("\ninput given back by hand on %s: the rounds stop here"
              % hold.where, flush=True)
    return hold.by_hand


def run_rounds(run, description, program=None, pictures=False,
               program_args=()):
    """Parse the command line, run `run` against each environment asked for,
    print the totals, and return the exit status.

    `program` names what to drive when it is not the testbed, and
    `program_args` are what it is started with. `pictures` adds --capture,
    for a round that has checks to spend one on."""
    ap = argparse.ArgumentParser(description=description)
    ap.add_argument("targets", nargs="*",
                    help="environments to drive, each with as much of "
                         "@machine:device as narrows it; default is every "
                         "one")
    ap.add_argument("--variant", default="release",
                    help="build variant under build/<target> (default release)")
    if pictures:
        ap.add_argument("--capture", action="store_true",
                        help="take pictures; on Wayland whether one costs a "
                             "consent dialog is the desktop's stored "
                             "permission and the harness cannot persist it, "
                             "which is why it is off")
    ap.add_argument("--validation", choices=("auto", "on", "off"),
                    default="auto",
                    help="put the Vulkan validation layer in front, with "
                         "sync and best-practices checks; auto is on for "
                         "the checker variants and off for release")
    args = ap.parse_args()

    wanted = B.select(args.targets)

    # A checker round is a claim about correctness, and the layer is the
    # half of it the sanitizers cannot see; so it is in front of every one
    # unless declined, and the round proves it loaded (layer_check below)
    # rather than trusting the variable.
    validate = args.validation == "on" or (
        args.validation == "auto" and args.variant in ("asan", "tsan", "gc"))
    # The library's instance alone (FNL_VK_VALIDATION), not the loader's
    # every instance: Zink's and vkd3d-proton's are Mesa's and wine's, and
    # the layer's own lock order in Zink's failed rounds that were ours.
    if validate:
        if not ledger.layer():
            os.environ["FNL_VK_VALIDATION"] = "1"
        os.environ.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
        os.environ.setdefault("VK_LAYER_VALIDATE_BEST_PRACTICES", "1")

    # `env` goes to every environment, and a bare machine turns its paths
    # into the copies it was sent; `desk` only to this desk's own, for
    # lavapipe here as Bare.install() and Bare.run_argv() have it there.
    env, desk = {}, {}
    if args.variant == "tsan":
        env["TSAN_OPTIONS"] = "suppressions=%s/tsan.supp" % ROOT
        # A move to another renderer tears a lavapipe device down with its
        # instance (tsan-lavapipe.supp says why).
        if lavapipe_lib():
            desk["TSAN_OPTIONS"] = "suppressions=%s" % lavapipe_supp()
    if args.variant == "asan":
        env["LSAN_OPTIONS"] = "suppressions=%s/lsan.supp" % ROOT
        desk.update(lavapipe_pinned())

    # Named for the file that ran, which is what tells one round's line in
    # the ledger from another's.
    instrument = os.path.splitext(os.path.basename(sys.argv[0]))[0]

    rounds, skipped = [], []
    for b in wanted:
        print("\n======== %s / %s ========" % (b.NAME, args.variant),
              flush=True)
        if program:
            b.program = program
        b.variant = args.variant
        # The machine's own input held for the round, and given back in the
        # finally below however the round ends (input_hold.py). Before the
        # program where the hold needs nothing of it, so that what the
        # program opens of the machine's devices is already the hold's.
        hold = b.input_hold() if input_hold.wanted() else \
            input_hold.NoHold(b.NAME)
        if hold.EARLY:
            hold.begin()
        try:
            b.start(args.variant, args=program_args,
                    env=env if isinstance(b, B._OnBare) else
                    dict(env, **desk))
        except B.Unavailable as e:
            hold.end()
            print("skipped -- %s" % e, flush=True)
            skipped.append((b.NAME, str(e)))
            ledger.record(instrument, ledger.place(b.NAME), args.variant,
                          asked=b.gpu, skipped=str(e),
                          layer=ledger.layer() and b.RENDERER == "vulkan",
                          **ledger.bound(b), **ledger.wine(b))
            if given_back(hold):
                break
            continue
        except Exception as e:
            # The program never came up -- it exited at fnl_init(), or its
            # window never appeared. A failed round, said with what the
            # program said, and the next environment's round goes on.
            r = Round(b, False)
            try:
                said = [ln for ln in b.dump(12) if not ln.startswith("EV ")]
            except Exception:
                said = []
            r.check("the program started", False, "%s: %s%s"
                    % (type(e).__name__, e,
                       "".join("\n      " + ln for ln in said)))
            for stop in (hold.end, b.stop, b.close):
                try:
                    stop()
                except Exception:
                    pass
            rounds.append(r)
            ledger.record(instrument, ledger.place(b.NAME), args.variant,
                          asked=b.gpu, layer=False, checks=1, failed=1,
                          na=0, fails=["the program started"],
                          **kept_log(b, r), **ledger.bound(b),
                          **ledger.wine(b))
            if given_back(hold):
                break
            continue
        except BaseException:
            # Ctrl+C while it starts: what was begun is ended, as the
            # finally below ends it once the program is up -- a program on
            # another machine outlives its ssh, and a hold its pipe only
            # once this process has gone.
            for stop in (hold.end, b.stop, b.close):
                try:
                    stop()
                except Exception:
                    pass
            raise
        # The program is up from here: whatever raises, the finally below
        # stops it and gives the machine's input back.
        r = Round(b, pictures and args.capture)
        layered = False
        try:
            # Asked of the environment once it has started: a machine's
            # monitor is its own, and this desktop's would be named for
            # every round.
            tgt = b.target_monitor()
            print("monitor: %s" % ("%s %dx%d+%d+%d"
                                   % (tgt[0], tgt[3], tgt[4], tgt[1], tgt[2])
                                   if tgt
                                   else "unknown -- xrandr said nothing"),
                  flush=True)
            if not hold.EARLY:
                hold.begin()
            backend_check(r, b)
            renderer_check(r, b)
            # The layer is Vulkan's; a GL program is checked by GL's debug
            # output, which a Debug tree logs and the log check reads.
            layered = ledger.layer() and b.RENDERER == "vulkan"
            if layered and getattr(b, "NO_LAYER", ""):
                r.skip("the validation layer is in the program", b.NO_LAYER)
            elif layered:
                layer_check(r, b)
            elif ledger.layer():
                r.skip("the validation layer is in the program",
                       "a Vulkan layer, and this program draws with %s"
                       % b.RENDERER)
            run(r)
        except Exception as e:
            r.check("the round ran to the end", False, "%s: %s"
                    % (type(e).__name__, e))
        finally:
            try:
                hold.end()
            finally:
                try:
                    b.stop()
                finally:
                    b.close()
        rounds.append(r)
        try:
            said = b.dump(1 << 30)
        except Exception:
            said = []           # a program that never started said nothing
        # Where a monitor was named: the default is no fact about the round.
        ledger.record(
            instrument, ledger.place(b.NAME), args.variant, asked=b.gpu,
            layer=layered, **ledger.bound(b), **ledger.wine(b),
            checks=len(r.oks) + len(r.fails), failed=len(r.fails),
            na=len(r.skips),
            fails=[f.split(" FAIL")[0].strip() for f in r.fails[:8]],
            capture=bool(r.capture), device=ledger.device(said),
            output=os.environ.get(getattr(b, "WANT_OUTPUT", B.OUTPUT_ENV),
                                  ""),
            **kept_log(b, r))
        if given_back(hold):
            break

    print()
    failed = 0
    for r in rounds:
        n = len(r.oks) + len(r.fails)
        print("==== %s/%s ====" % (r.b.NAME, args.variant))
        print("%d checks, %d failed, %d not applicable"
              % (n, len(r.fails), len(r.skips)))
        for f in r.fails:
            print("  " + f)
        failed += len(r.fails)
    for name, why in skipped:
        print("==== %s ====\nskipped -- %s" % (name, why))

    if not rounds:
        # Nothing ran at all, which is neither a pass nor a finding. 77 is
        # what automake and meson read as skipped.
        return 77
    return 1 if failed else 0


def keep_log(b):
    """A failed round's program log, copied beside it under a name of its
    own, and its path from the tree's root; None where there was none.
    The next round of the environment writes the same file, so a failure
    that comes once in twenty rounds is otherwise gone before anyone reads
    it. After stop(), which writes the log out (Testbed._keep())."""
    src = getattr(getattr(b, "testbed", None), "log_path", None)
    if not src or not os.path.exists(src):
        return None
    dst = "%s.failed-%s.log" % (os.path.splitext(src)[0],
                                time.strftime("%Y%m%d-%H%M%S"))
    try:
        shutil.copyfile(src, dst)
    except OSError as e:
        print("  (the program log was not kept: %s)" % e, flush=True)
        return None
    rel = os.path.relpath(dst, ROOT)
    print("  program log kept: %s" % rel, flush=True)
    return rel


def kept_log(b, r):
    """The ledger's `log` field: the kept log of a round that failed, and
    nothing at all for one that did not."""
    kept = keep_log(b) if r.fails else None
    return {"log": kept} if kept else {}


def backend_check(r, b):
    """The backend the round names is the one the program opened: a Linux
    tree has three, and a round of one under another's name would pass for
    it. Read off the library's line at open, which start() has waited past
    by the time the window is found; nothing to ask of a tree that has one
    backend alone (Environment.BACKEND)."""
    if not b.BACKEND:
        return
    want = "backend: %s, named by FNL_BACKEND" % b.BACKEND_NAMES.get(
        b.BACKEND, b.BACKEND)
    said = [ln for ln in b.dump(1 << 30) if ln.startswith("backend: ")]
    r.check("the program opened the backend it was named", want in said,
            "said %s" % (said[0] if said else "nothing"))


def renderer_check(r, b):
    """The renderer the round names is the one the program draws with: the
    Windows tree has two and would choose Direct3D 12 where it could, and
    a round of one under another's name would pass for it. Read off the
    library's line at open, as backend_check() reads its own, and asked of
    every program: each is told its renderer (Environment.RENDERER), the
    trees with one renderer alone as well."""
    said = [ln for ln in b.dump(1 << 30) if ln.startswith("renderer: ")]
    if getattr(b, "ranked", False):
        # Named nothing, the round opened where the machine's ranking says
        # (Environment.gpu_sent()): its renderer is the one it opened with
        # from here on, and the check is that it opened on the ranked
        # first entry -- the library's line, and that entry's renderer.
        opening = [ln for ln in b.dump(1 << 30)
                   if ln.startswith("GPU rank: opening on rank 1, ")]
        key = opening[0].split(", ", 1)[1].split("|")[0] if opening else ""
        drew = said[0][len("renderer: "):].split(";")[0] if said else ""
        b.RENDERER = RENDERER_KEYS.get(drew, b.RENDERER)
        r.check("the program opened on the ranked first entry",
                bool(opening) and key == b.RENDERER,
                "%s; %s" % (opening[0] if opening else "no ranking said",
                            said[0] if said else "no renderer said"))
        return
    want = "renderer: %s, named by FNL_RENDERER" % b.RENDERER_NAMES.get(
        b.RENDERER, b.RENDERER)
    r.check("the program drew with the renderer it was named", want in said,
            "said %s" % (said[0] if said else "nothing"))


def layer_check(r, b):
    """The validation layer asked for is the layer in the program: a loader
    that cannot find it says nothing, and a round run without it would pass
    for one run with it. Read from the process's own mappings; the laptop
    and the guest are asked by their own environments."""
    if not getattr(b, "LOCAL", False):
        return
    try:
        with open("/proc/%d/maps" % b.pid()) as fh:
            loaded = "VkLayer_khronos_validation" in fh.read()
    except OSError as e:
        r.check("the validation layer is in the program", False, str(e))
        return
    r.check("the validation layer is in the program", loaded,
            "" if loaded else "asked for and not mapped; is it installed?")


def main():
    # --frame-times for the rate a restored window draws at, which is
    # counted and not read off the CPU (DRAWING_FPS).
    return run_rounds(
        run, "One stress round, against every environment that runs here.",
        pictures=True, program_args=("--frame-times",))


if __name__ == "__main__":
    sys.exit(main())
