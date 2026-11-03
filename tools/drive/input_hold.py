#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Physical input held away from a machine while a round drives it.

A round types and clicks into a desktop, and a hand on the same machine --
a key, a nudge of the mouse, a palm on the touchpad -- lands in the middle
of it: a focus that moves, a drag that ends early, a check that fails for
the hand's reason and not the library's. So for the length of each round
the machine's own keyboards, mice and touchpads are held, and given back
when the round ends, however it ends.

Linux, this desktop and the bare machines: this file, run as root by ssh
(`serve`), grabs each such device exclusively (EVIOCGRAB), so neither the
compositor nor the X server reads it. What the harness sends is never one
of them: on this desktop it enters through the RemoteDesktop portal, into
the compositor; on a bare machine through uinput devices, which are
virtual and passed over; a guest's through VirtualBox. The grabs go with
the process, which ends when its standard input does -- the harness's
pipe, closed at the round's end or by the harness's own end, or the ssh
connection's -- and after a time limit in any case.

Windows and the Mac are asked through their agents, which drop what is not
their own: low-level hooks that let the events SendInput marks injected
through (windows_agent.ps1), and an event tap that lets the agent's marked
events through (mac_agent.m).

Everywhere, Ctrl+Alt+Escape (Control+Option+Escape on the Mac) gives the
input back by hand, and the sweep stops after the round it is in. On
Windows Ctrl+Alt+Delete, which no hook can drop, always reaches Windows' own
screen (Task Manager, sign out). FNL_DRIVE_HOLD=0 holds nothing.

    sudo python3 input_hold.py serve 1800     # by hand: hold, stdin ends it
"""

import os
import select
import shlex
import struct
import subprocess
import sys
import time

# Long past any round; a hold that outlives one has lost its harness.
LIMIT = 1800
EVIOCGRAB = 0x40044590
EV_KEY = 1
KEY_A, BTN_LEFT, BTN_TOUCH = 30, 0x110, 0x14a
# The chord that gives the input back by hand: Control, Alt and Escape,
# by evdev's codes.
KEY_ESC = 1
CTRL = {29, 97}
ALT = {56, 100}
# struct input_event on a 64-bit kernel: a timeval, then type, code, value.
EVENT = struct.Struct("llHHi")


def _bits(path):
    """A sysfs capability bitmap, as one integer: long-sized words, the
    most significant first."""
    try:
        with open(path) as f:
            words = f.read().split()
    except OSError:
        return 0
    v = 0
    for w in words:
        v = (v << 64) | int(w, 16)
    return v


def _wanted(ev):
    """What the device event<N> is, if it is one to hold: a keyboard (it
    has letters), a mouse (relative motion and a button) or a touchpad or
    screen (absolute motion and a touch). A power or lid button, a sound
    card's jack, anything virtual -- uinput's, the compositor's -- is left
    alone."""
    dev = "/sys/class/input/%s/device" % ev
    if "/virtual/" in os.path.realpath(dev):
        return None
    key = _bits(dev + "/capabilities/key")
    rel = _bits(dev + "/capabilities/rel")
    ab = _bits(dev + "/capabilities/abs")
    try:
        with open(dev + "/name") as f:
            name = f.read().strip()
    except OSError:
        name = ev
    if key >> KEY_A & 1:
        return name
    if rel & 1 and key >> BTN_LEFT & 1:
        return name
    if ab & 1 and key >> BTN_TOUCH & 1:
        return name
    return None


def serve(limit):
    """Hold every such device until stdin ends, the limit passes or the
    chord is pressed; say what was held, and why it ended. Exit 3 for the
    chord, which the harness reads as a hand that wants the machine."""
    import fcntl
    import glob
    held = {}
    for path in sorted(glob.glob("/dev/input/event*")):
        name = _wanted(os.path.basename(path))
        if not name:
            continue
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            continue
        try:
            fcntl.ioctl(fd, EVIOCGRAB, 1)
        except OSError:
            os.close(fd)
            continue
        held[fd] = name
    print("held %d: %s" % (len(held), "; ".join(sorted(held.values()))),
          flush=True)
    down = set()
    end = time.monotonic() + limit
    while True:
        left = end - time.monotonic()
        if left <= 0:
            print("released: the time limit", flush=True)
            return 0
        ready, _, _ = select.select([0] + list(held), [], [], min(left, 5))
        for fd in ready:
            if fd == 0:
                if not os.read(0, 4096):
                    print("released: the round is over", flush=True)
                    return 0
                continue
            try:
                data = os.read(fd, EVENT.size * 64)
            except OSError:
                del held[fd]            # unplugged
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, typ, code, value = EVENT.unpack_from(data, off)
                if typ != EV_KEY:
                    continue
                if value:
                    down.add(code)
                else:
                    down.discard(code)
            if KEY_ESC in down and down & CTRL and down & ALT:
                print("released: by hand", flush=True)
                return 3


class Hold:
    """A hold on one machine, begun once its round's program is up and
    ended in the round's finally. `by_hand` says the chord ended it, which
    stops the sweep after the round."""

    # Whether it is begun before the program starts, needing nothing of it:
    # a grab of the devices is, an agent's hook is not.
    EARLY = False

    def __init__(self, where):
        self.where = where
        self.by_hand = False

    def begin(self):
        return False

    def end(self):
        pass


class NoHold(Hold):
    pass


class SshHold(Hold):
    """This file's serve(), as root on a Linux machine by ssh. Begun before
    the program, so that a program that grabs keyboards itself -- the KMS
    backend does, to keep keys off the console -- finds the machine's own
    held, and has the round's uinput keyboard alone."""

    EARLY = True

    def __init__(self, where, ssh):
        super().__init__(where)
        self.ssh = ssh
        self.proc = None

    def begin(self):
        with open(os.path.abspath(__file__)) as f:
            code = f.read()
        self.proc = subprocess.Popen(
            self.ssh + ["python3 -c %s serve %d" % (shlex.quote(code),
                                                    LIMIT)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        ready, _, _ = select.select([self.proc.stdout], [], [], 15)
        line = self.proc.stdout.readline().strip() if ready else ""
        if not line.startswith("held"):
            print("  input not held on %s: %s" % (self.where,
                                                  line or "no answer"),
                  flush=True)
            self.end()
            return False
        print("  input held on %s (%s) -- Ctrl+Alt+Escape gives it back"
              % (self.where, line), flush=True)
        return True

    def end(self):
        if not self.proc:
            return
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.by_hand = self.by_hand or self.proc.returncode == 3
        self.proc = None

    def done(self):
        """Whether the hold has ended already, and by the chord."""
        if self.proc and self.proc.poll() is not None:
            self.by_hand = self.proc.returncode == 3
            return True
        return self.proc is None


class AgentHold(Hold):
    """Asked of a Windows or Mac agent: `hold`, `held` (1 holding, 2 given
    back by hand), `release`. The agent's own end releases it too."""

    def __init__(self, where, env):
        super().__init__(where)
        self.env = env
        self.on = False

    @property
    def desk(self):
        """The agent, which the environment has once its program is up."""
        return self.env.portal

    def begin(self):
        try:
            self.desk.ask("hold")
        except Exception as e:
            print("  input not held on %s: %s" % (self.where, e), flush=True)
            return False
        self.on = True
        print("  input held on %s -- Ctrl+Alt+Escape (Control+Option+"
              "Escape on a Mac) gives it back" % self.where, flush=True)
        return True

    def end(self):
        if not self.on:
            return
        self.on = False
        # Each asked alone: an agent that cannot say how the hold ended may
        # still be holding, and is asked to let go regardless. One that has
        # gone has let go with it.
        try:
            self.by_hand = self.desk.ask("held")[0] == "2"
        except Exception:
            pass
        try:
            self.desk.ask("release")
        except Exception as e:
            print("  input may still be held on %s: %s -- Ctrl+Alt+Escape "
                  "gives it back" % (self.where, e), flush=True)


def wanted():
    """Whether rounds hold input at all: FNL_DRIVE_HOLD=0 says not."""
    return os.environ.get("FNL_DRIVE_HOLD", "1") not in ("0", "")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "serve":
        sys.exit(serve(int(sys.argv[2]) if len(sys.argv) > 2 else LIMIT))
    print(__doc__)
