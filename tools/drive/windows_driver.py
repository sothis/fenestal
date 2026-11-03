# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A Windows desktop, as the agent on it answers for one.

Two machines are driven on Windows itself -- a VirtualBox guest
(vbox_driver.py) and a laptop on the network (native_driver.py) -- and they
differ in how the harness gets *in*: where the keys and the pointer come
from, how a program is started, how a file is copied across. What a desktop
can be *asked* is the same on both, because the same agent
(windows_agent.ps1) is on it: the rectangle, the focus, the monitor, where
the pointer is, the layout, the clipboard. That half is here, once.

Desk is what the two share and neither is whole without: a subclass opens
the connection and brings warp(), nudge(), button(), scroll() and keycode()
-- the devices -- and install(), run_argv() and screenshot().
"""

import base64
import os
import socket
import time

from testbed import POLL
from wayland_driver import KEY

# Where the program under test and the agent are copied to, every round: a
# machine that ran last week's binary would report last week's bugs.
DRIVE_DIR = r"C:\fenestal\drive"
POWERSHELL = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
AGENT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "windows_agent.ps1")

# Every program this project builds that opens a window, as Windows names the
# process: the image without its extension.
IMAGES = ("fenestal-testbed", "fenestal-dev")

# The harness names keys by evdev code (wayland_driver.KEY), and Windows
# takes PS/2 set 1. Below 89 the two are the same number, which is where
# evdev's came from. These are the ones that are not: each is an E0 prefix
# and the code beside it.
EXTENDED = {
    96: 0x1c, 97: 0x1d, 98: 0x35, 99: 0x37, 100: 0x38, 102: 0x47, 103: 0x48,
    104: 0x49, 105: 0x4b, 106: 0x4d, 107: 0x4f, 108: 0x50, 109: 0x51,
    110: 0x52, 111: 0x53, 125: 0x5b, 126: 0x5c, 127: 0x5d,
}
PAUSE = 119


class Unreachable(RuntimeError):
    """The machine cannot be driven, which is not a failing check: nothing
    running, no way in, nobody logged in. environment.py reports the round
    skipped."""


class Desk:
    """The agent's half of a Windows machine.

    Shaped like wayland_driver.Portal where the two overlap -- rel(), key(),
    tap() -- so wayland_driver.Driver steers this as it steers that."""

    sock = None

    def _greet(self, sock):
        """Take the connection the agent made, and ask it the first thing."""
        self.sock = sock
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(20.0)
        self.rfile = self.sock.makefile("r", encoding="utf-8", newline="\n")
        self.carry = [0.0, 0.0]
        self.dpi = int(self.ask("hello")[1])

    def _hang_up(self):
        if self.sock:
            try:
                self.sock.sendall(b"quit\n")
            except OSError:
                pass
            # The file first: the socket stays open under close() while a
            # file made from it is.
            rfile = getattr(self, "rfile", None)
            if rfile is not None:
                try:
                    rfile.close()
                except OSError:
                    pass
                self.rfile = None
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def ask(self, *words):
        """One question, one answer: the reply's words after its "ok"."""
        self.sock.sendall((" ".join(str(w) for w in words) + "\n")
                          .encode("utf-8"))
        line = self.rfile.readline()
        if not line:
            raise RuntimeError("the agent on the Windows desktop has gone")
        parts = line.rstrip("\n").split(" ")
        if parts[0] != "ok":
            raise RuntimeError("the agent answered %r to %r"
                               % (line.strip(), words[0]))
        return parts[1:]

    # ---- the window --------------------------------------------------------

    def kill_stale(self):
        """Stop anything an earlier round left up; how many there were."""
        return int(self.ask("kill", *IMAGES)[0])

    def find(self, image, title="Fenestal", timeout=20.0):
        """The window of that program, by its image and its title. Returns
        the process id there, or None if no such window came."""
        end = time.time() + timeout
        while time.time() < end:
            pid = int(self.ask("find", image, title)[0])
            if pid:
                return pid
            time.sleep(0.1)         # poll: an EnumWindows is not free
        return None

    def rect(self):
        return tuple(int(v) for v in self.ask("rect"))

    def monitor(self):
        """(name, x, y, w, h) of the monitor the window is on -- the primary
        before there is a window -- and its work area as (x, y, w, h)."""
        a = self.ask("mon")
        v = [int(x) for x in a[1:]]
        return (a[0], v[0], v[1], v[2], v[3]), tuple(v[4:8])

    def monitors(self):
        """Every monitor of that desktop, as environment.monitors() gives this
        one's: (name, primary, x, y, w, h)."""
        a = self.ask("mons")
        return [(a[i], a[i + 1] == "1") + tuple(int(v) for v in a[i + 2:i + 6])
                for i in range(0, len(a) - 5, 6)]

    def pointer_pos(self, patience=2.0):
        """Where the pointer is, in screen pixels.

        Windows refuses the question while another desktop has the input --
        a consent prompt, the lock screen -- so a refusal is waited out for
        a moment and then raised: nothing sent meanwhile would reach the
        window, and a guessed position is worse than none."""
        end = time.time() + patience
        while True:
            try:
                x, y = self.ask("ptr")
                return (int(x), int(y))
            except RuntimeError:
                if time.time() >= end or not self.sock:
                    raise
                time.sleep(0.05)    # poll

    def foreground(self):
        return self.ask("fg")[0] == "1"

    def focus(self):
        return self.ask("focus")[0] == "1"

    def minimized(self):
        return self.ask("iconic")[0] == "1"

    def move_window_by(self, dx, dy):
        self.ask("moveby", int(dx), int(dy))

    def running(self):
        """Whether the process whose window was found is still there."""
        return self.ask("alive")[0] == "1"

    def cpu_seconds(self):
        return float(self.ask("cpu")[0])

    def has_module(self, name):
        """Whether the program whose window was found has that library
        loaded."""
        return self.ask("module", name)[0] == "1"

    def close_window(self):
        """Ask the window found to close, as its own button would."""
        return self.ask("close")[0] == "1"

    # ---- the layout --------------------------------------------------------

    def layout(self):
        """The layout the window types through, as Windows names one: its
        handle, in hex."""
        return self.ask("layout")[0]

    def layout_load(self, klid):
        """Have a layout loaded in the user's session, by its registry
        name. Returns its handle and whether it was loaded already."""
        hkl, was = self.ask("layoutload", klid)
        return hkl, was == "1"

    def layout_drop(self, hkl):
        self.ask("layoutdrop", hkl)

    def layout_switch(self, hkl, timeout=3.0):
        """Ask the window to type through that layout, and wait until it
        does. Whether it did: the request is the window's to honour."""
        self.ask("layoutask", hkl)
        end = time.time() + timeout
        while time.time() < end:
            if self.layout() == hkl:
                return True
            time.sleep(0.05)        # poll: the request is a posted message
        return False

    # Base64 both ways: the protocol is lines, and a clipboard is not.
    def clipboard_get(self):
        return base64.b64decode(self.ask("clipget")[0])

    def clipboard_put(self, data):
        self.ask("clipput", base64.b64encode(data).decode("ascii"))

    # ---- the pointer and the keys, where they do not depend on the device --

    def rel(self, dx, dy):
        """Move by a delta from wherever the pointer is *now*, by the
        desktop's own account of that: Windows moves it too, when a capture
        ends.

        Absolute underneath, so the delta is what arrives. The fractions a
        drag's steps leave are carried, or twelve steps of 13.3 are 156."""
        px, py = self.pointer_pos()
        self.carry[0] += dx
        self.carry[1] += dy
        ix, iy = int(self.carry[0]), int(self.carry[1])
        self.carry[0] -= ix
        self.carry[1] -= iy
        mx, my, mw, mh = self.desk
        self.warp(min(max(px + ix, mx), mx + mw - 1),
                  min(max(py + iy, my), my + mh - 1))

    def key(self, name, pressed=True):
        self.keycode(KEY[name], pressed)
        (self.keys.add if pressed else self.keys.discard)(name)

    def _keys_up(self):
        """Release every key still down, for close(): nothing else would,
        and a modifier left down is one the desktop goes on reading."""
        for name in list(self.keys):
            try:
                self.key(name, False)
            except Exception:
                pass

    def tap(self, name, hold=0.05):
        self.key(name, True)
        time.sleep(hold)            # the press itself: a key held this long
        self.key(name, False)

    @property
    def desk(self):
        """Every monitor of that desktop together, as (x, y, w, h): what the
        pointer is clamped to. Read once: nothing a round does changes the
        mode."""
        if not getattr(self, "_desk", None):
            self._desk = tuple(int(v) for v in self.ask("desk"))
        return self._desk

    def want_screen(self, w, h, timeout=15.0):
        """Have the screen be at least that large, where the machine is one
        that can be asked. A screen that is what it is does nothing here."""

    def shot_origin(self):
        """Where in a screenshot() the desktop's pixel (0, 0) is."""
        return (0, 0)

    def settle_pointer(self, x, y, timeout=0.3):
        """Wait until the desktop says the pointer is at that pixel, and say
        whether it came to be. A report is queued, not applied: the position
        read back straight after one is often the old one."""
        end = time.time() + timeout
        while time.time() < end:
            px, py = self.pointer_pos()
            if abs(px - x) <= 1 and abs(py - y) <= 1:
                return True
            time.sleep(POLL)
        return False

    # ---- the program under test --------------------------------------------

    def run_argv(self, exe, args=(), env=None):
        """The command line whose process here *is* the program there, as far
        as testbed.py can tell: its stderr is the program's and it ends when the
        program does."""
        raise NotImplementedError

    def run_begin(self):
        """Whatever is still to do once that process has been started."""
