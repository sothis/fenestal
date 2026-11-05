# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""One window under test, whichever window server is holding it.

Every backend, one round -- the Win32 one several times over, since the
same binary runs under wine here and on Windows itself, in a virtual machine
and on a laptop. What differs between them is not what a window *does* --
that is the claim this library makes -- but what can be *asked* about it:

    everywhere      what the application was handed, which it echoes itself
                    (see testbed.py) -- the area, the scale, fullscreen,
                    maximized, captured, and every event it received
    X11 and Win32   where the window is on the screen, what monitor that is,
                    what the manager drew around it. A wine window is an X11
                    window on XWayland, so these two are one implementation
    Wayland only    the transparent grab ring, which is the only backend that
                    has one; and a composited picture, through a portal
    the machine     the same questions as an X server answers, put to Windows
                    by an agent on its desktop (windows_driver.py), and a
                    picture of its whole screen. Twice: a virtual machine,
                    and a laptop with a GPU and a panel of its own
    bare X11        the X server's questions again, put to another
                    machine's Xorg with no window manager on it, through a
                    forwarded socket (bare_driver.py); the same machine's
                    weston and its console (KMS, the scanned-out
                    framebuffer its picture, kms_grab.py) ask less
    the Mac         the same questions, put to the window server by an
                    agent there (mac_driver.py), and screencapture's picture

The Wayland and X11 windows are each driven twice on this desktop: drawn
with Vulkan (wayland-vulkan, x11-vulkan) and with OpenGL (wayland-gl,
x11-gl). The window server's half is one class; RENDERER says which
renderer the program draws with. A tree has every backend and every
renderer its system has -- linux, win64, macos -- so an environment is a
tree, the backend the program is told to open (BACKEND, by FNL_BACKEND,
on Linux) and the renderer it is told to draw with (RENDERER, by
FNL_RENDERER).

So a check is written once and declares what it needs. CAN is that
declaration, and a round asks `if "server-rect" in b.CAN`, never
`if b.NAME == "x11-vulkan"`.

Input is the RemoteDesktop portal for everything on this desktop, through
one session whose restore token is saved (wayland_driver.py --consent, once
per machine). Never XTEST: x11_driver.py says what that costs here. The
virtual machine is driven at its own emulated keyboard and mouse, the
laptop and the Mac by the agent on each, the bare X server by XTEST on that
server, and weston and KMS there by uinput; none of them touches anything
on this desktop or asks anybody.

Aiming is one implementation too. move_to() takes a point *in the window*, in
logical points -- the space the library lays its titlebar out in and the
space the echo reports in. The portal offers only relative motion, so the
pointer is nudged and the application's own report of where that landed is
read back until the two agree. The X server is for what a round *reads*: the
rectangle, the monitor, the focus, the properties, the pixels.
"""

import atexit
import os
import re
import shutil
import subprocess
import threading
import time

import testbed
from ledger import IN_USE, LISTED, OPENED, layer as validation_asked
from testbed import HZ, OUT, POLL, QUIET, ROOT, Testbed, cpu_jiffies

# The xclips this process started to offer the clipboard on this desktop,
# which Environment.clipboard_done() ends.
_XCLIPS: list[subprocess.Popen] = []


# ---- the one monitor a round runs on ----------------------------------------
#
# mutter places a new window on the monitor the pointer is on, and a Wayland
# client is never told where its window is. So every round parks the pointer
# inside one named output before the window exists: the window is born under
# it, and find_window() answers from its first probe. The alternative is the
# sweep, which is bounded by the output the echo names but starts from a
# corner of the outputs' union -- with monitors of different heights not a
# rectangle, and corners on no screen at all.
#
# FNL_DRIVE_OUTPUT names the output; the default is the primary. Read from
# `xrandr --listmonitors` for all three backends, because XWayland reports
# the layout the Wayland compositor is using.
OUTPUT_ENV = "FNL_DRIVE_OUTPUT"

# How far one parking step moves the pointer. Small enough that a monitor
# boundary's pointer barrier is crossed by sustained motion and not resisted
# as one jump, large enough that crossing the desktop is a handful of calls.
STEP = 300

_monitors = None


def monitors(refresh=False):
    """Every output, as (name, primary, x, y, w, h). Empty if unreadable.

    Read once and kept: nothing a round does changes the layout, and this is
    asked several times an action. start() asks afresh for each environment."""
    global _monitors
    if _monitors is not None and not refresh:
        return _monitors
    try:
        out = subprocess.run(["xrandr", "--listmonitors"],
                             capture_output=True, text=True).stdout
    except Exception:
        return []
    _monitors = parse_monitors(out)
    return _monitors


def parse_monitors(out):
    """`xrandr --listmonitors` read as monitors() returns it, whichever X
    server said it."""
    found = []
    for line in out.splitlines()[1:]:
        # " 0: +*HDMI-1 3840/880x2160/490+1920+0  HDMI-1" -- the star marks the
        # primary, and the geometry is width/mmxheight/mm+x+y.
        m = re.search(r"^\s*\d+:\s+\+(\*?)(\S+)\s+"
                      r"(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+)", line)
        if m:
            star, name, mw, mh, mx, my = m.groups()
            found.append((name, star == "*", int(mx), int(my),
                          int(mw), int(mh)))
    return found


def target_monitor():
    """(name, x, y, w, h) of the output every round runs on, or None.

    None means the layout could not be read, which is not fatal: the Wayland
    round falls back to its sweep and the X ones to wherever the window
    landed. A name that does not exist stops the run: somebody asked for a
    screen this desktop has not got."""
    mons = monitors()
    if not mons:
        return None
    want = os.environ.get(OUTPUT_ENV)
    if want:
        for name, _pri, x, y, w, h in mons:
            if name == want:
                return (name, x, y, w, h)
        raise SystemExit(
            "no output %r; %s names one of %s"
            % (want, OUTPUT_ENV, ", ".join(m[0] for m in mons)))
    for name, pri, x, y, w, h in mons:
        if pri:
            return (name, x, y, w, h)
    name, _pri, x, y, w, h = mons[0]
    return (name, x, y, w, h)


def root_box():
    """The bounding box of every output together, or None.

    What relative motion is clamped to. With monitors of different heights
    its corners need not be on any of them."""
    mons = monitors()
    if not mons:
        return None
    x0 = min(m[2] for m in mons)
    y0 = min(m[3] for m in mons)
    x1 = max(m[2] + m[4] for m in mons)
    y1 = max(m[3] + m[5] for m in mons)
    return (x0, y0, x1 - x0, y1 - y0)


# How long XWayland is given to report where a move left the pointer. There
# is nothing to wait on; see _aim().
AIM_BEAT = 0.06


class InputRefused(RuntimeError):
    """A key was about to be typed into a window that is not the one under
    test. See _Portal.keyboard_is_ours()."""


class Unavailable(RuntimeError):
    """This environment cannot run here, which is not a failing check.

    No binary built, no display of the right kind, no consent for the portal.
    A round says so and skips rather than reporting a finding about a library
    it never started."""


# The variables an environment sets for its program and never inherits from
# the harness's own environment (Environment.forced()): one left over in a
# shell would name a backend or a renderer the round is not about.
FORCED = ("FNL_BACKEND", "FNL_RENDERER")

# The narrowest window the library's bar allows, in points of window
# geometry: the button strip, the gap and the controls' run with the GPU box
# (deco_min_window(), DESIGN *The size floor*). A window that is its output
# has another, and is never resized.
FLOOR_W = 625
# A point of the bar that is caption where the window is wide enough: left
# of the controls, in the title. Not near the floor, where the controls
# reach past it (caption_x()).
BAR_TITLE_X = 120
# The bar's left margin, DECO_BTN_MARGIN: the floor less it and a point is
# how far from the right the controls' run starts (ctl_run_x()).
BAR_MARGIN = 10
# The resize border inside the window where there is no ring outside it,
# DECO_GRAB: a press there resizes.
BAR_GRAB = 6
# The output box and the gap after it, 101 and 8 at a body advance of 8
# points: under a window server they join the run, left of the rest, where
# the window is FLOOR_W + OUTPUT_ROOM wide or more, and are not in the floor
# (ctl_output_in()).
OUTPUT_ROOM = 109


def caption_x(w=None):
    """A point of the bar of a window `w` points wide that is caption, for
    a drag by the titlebar: BAR_TITLE_X, or nearer the left where the
    controls' run, which keeps its width as the window narrows, starts left
    of that -- a 720-wide window on a 1024x768 screen, whose run starts at
    106 -- the middle of the caption between the resize border and the
    run. A press on either is theirs and drags nothing. BAR_TITLE_X where
    the width is not known."""
    if not w:
        return BAR_TITLE_X
    run_x = w - FLOOR_W + BAR_MARGIN + 1
    if w >= FLOOR_W + OUTPUT_ROOM:
        run_x -= OUTPUT_ROOM
    return min(BAR_TITLE_X, (BAR_GRAB + run_x) // 2)


class Environment:
    """What a round may ask of a window. Every method here is implemented by
    every environment; the ones guarded by CAN raise if asked without it, so
    a round that forgets the guard fails loudly rather than quietly comparing
    None."""

    NAME = "?"
    CAN = frozenset()
    # The renderer the program draws with, as FNL_RENDERER spells it, and
    # the program is told it (forced()) for BACKEND's reason: the Windows
    # tree has two and would choose Direct3D 12 where it can, so a round
    # left to the library's choice would be another renderer's under this
    # one's name -- renderer_check() in stress.py reads the line saying it
    # was named. Every tree is told, the ones with one renderer as well,
    # which take the name of the one they have. Only "vulkan" has the
    # validation layer a checker round puts in front; a checker build
    # drawing with OpenGL reports GL's own debug output through the
    # library's log instead, which the log check reads.
    RENDERER = "vulkan"
    # How the library's own line names each.
    RENDERER_NAMES = {"vulkan": "Vulkan", "gl": "OpenGL",
                      "d3d12": "Direct3D 12", "metal": "Metal"}
    # Which of the tree's programs this drives. Any of them can be: the echo
    # is the library's, and start() asks for it by a variable (ECHO). Set
    # on the instance before start().
    program = "fenestal-testbed"
    # The program's log under OUT, less ".log", for a caller that keeps
    # logs of its own (pace.py's border row); None is log_path()'s name.
    # Set on the instance before start(), as `program` is.
    log_as = None
    # The machine a round is on, where each() makes one a machine; None for
    # the one each() makes with none listed, which is a skipped round.
    machine: str | None = None
    # The GPU the program is put on, by a part of its name, where a place
    # has several and a round is run on each. Empty for the library's own
    # choice.
    gpu = ""
    # What the binary's name ends in, and what runs it.
    SUFFIX = ""
    RUNNER = []
    # The build tree the binary comes out of, where that is not NAME: one
    # tree can be driven in two places.
    TREE: str | None = None
    # The backend a Linux tree is told to open, as FNL_BACKEND spells it.
    # Never left to the library's choice: a round names what it ran, and a
    # choice that went another way would be a round of the wrong backend
    # under this one's name -- backend_check() in stress.py reads the
    # library's line saying it was named. Empty where the build has one
    # backend alone, a Windows or macOS tree, and the variable is then
    # kept from the program: such a build refuses a name it has not got.
    BACKEND = ""
    # How the library's own line names each (backend.h's names).
    BACKEND_NAMES = {"wayland": "Wayland", "x11": "X11", "kms": "KMS"}
    # Whether the program under test is a process on this machine, which is
    # what pace.py needs of one: it starts its own and reads /proc.
    LOCAL = True
    # And no GPU ranking (src/core/rank.c): a round names the GPU it tests,
    # and a ranking kept on the machine would move the window off it.
    ECHO = {"FNL_ECHO_EVENTS": "1", "FNL_GPU_RANK": "off"}
    # What the program is started with and without, over this desktop's
    # environment. environ() is the reader, for pace.py as well, which
    # starts a process of its own.
    ENV = {}
    UNSET = ()
    # Why an environment without "unminimize" has not got it, which is the
    # round's reason for skipping the section there.
    NO_UNMINIMIZE = "the harness cannot bring the window back"
    # Who composes a dead key where it is not the library, said as the
    # round's reason for not asserting the library's rules for one under
    # "known-layout"; empty where the library composes.
    COMPOSED_ELSEWHERE = ""
    # Why a window maximized and then made fullscreen does not come back
    # maximized, where it is something between the library and the screen
    # that loses it: the round's reason for skipping that leg of the
    # maximize section and going on to the restore. Empty everywhere it
    # does.
    MAXIMIZED_LOST_IN_FULLSCREEN = ""
    # Why a window here goes narrower than the library's floor, where
    # something between the library and the screen lets it: the round's
    # reason for skipping the floor check. Empty everywhere it holds.
    FLOOR_UNHELD = ""
    # Why a window here does not open on the output the round pins it to,
    # where something between the library and the screen places it by
    # another rule: the round's reason for not asking which monitor
    # fullscreen covers. Empty everywhere the pin holds.
    PIN_UNHELD = ""
    # The step in pixels the library rounds the floor up to where it sizes
    # its own window, with no manager to read the hint; 0 where the floor
    # is held as it is.
    FLOOR_STEP = 0
    # The renderers, by key, whose GPUs the list holds here but which will
    # not draw, and why: the round's reason for not moving to one. A move
    # there goes back with one line, which is the library right; the cause
    # is the environment's.
    NO_MOVE_TO: dict[str, str] = {}
    # What the library logs here because of where it is, and says nowhere
    # else: patterns, searched for, and a log check takes a line one finds
    # as no complaint. Every other line is one, so a notice this desktop
    # gives in a passing round wants a pattern here.
    EXPECTED_LOG: tuple[str, ...] = ()
    # How long the window server animates a change of window state after
    # the library has said it, during which a key the round sends is the
    # window server's to drop (transition() waits it out). Nothing, where
    # the state comes when the animation is done or there is none.
    WINDOW_ANIMATION = 0.0

    def expected_line(self, line):
        return any(re.search(p, line) for p in self.EXPECTED_LOG)

    def placed(self, still=False):
        """((x, y, w, h), (mx, my, mw, mh)): the window and its monitor on
        the screen, in pixels, or None where nothing can say. The server's
        rectangle where it gives one -- `still` waits for it to settle --
        and an environment that finds the window some other way says so here."""
        if "server-rect" not in self.CAN:
            return None
        mon = self.monitor()
        if not mon:
            return None
        return (self.rect_still() if still else self.rect()), mon

    @classmethod
    def forced(cls):
        """FNL_RENDERER for the program (RENDERER), and FNL_BACKEND where
        it has a choice of backend (BACKEND)."""
        named = {"FNL_RENDERER": cls.RENDERER}
        if cls.BACKEND:
            named["FNL_BACKEND"] = cls.BACKEND
        return named

    @classmethod
    def forwarded(cls, prefixes):
        """This desk's variables that begin with one of `prefixes`, for a
        program on another machine: less the two forced() decides, which
        the program is told whatever this desk's own say."""
        return {k: v for k, v in os.environ.items()
                if prefixes and k.startswith(prefixes) and k not in FORCED}

    @classmethod
    def environ(cls, **more):
        base = {k: v for k, v in os.environ.items()
                if k not in cls.UNSET and k not in FORCED}
        return {**base, **cls.ENV, **cls.forced(), **more}

    @classmethod
    def clear(cls):
        """Whatever has to be over before the program is started; raises
        Unavailable when it will not be. Nothing, for most."""

    @classmethod
    def stage(cls, exe, variant):
        """The program to run for the tree's `exe`: the tree's own, for
        all but an environment that must put files beside it (_Vkd3dProton)."""
        return exe

    @classmethod
    def each(cls):
        """One of these for every place this environment runs, which is
        one unless the environment says otherwise."""
        return [cls()]
    # A sanitizer build, or a GPU that powers off when idle: every wait is
    # stretched. Set by start().
    slow = False
    # How the harness reaches the window, which CAN does not say: CAN is what
    # may be *asked* of a window, this is what does the asking.
    INPUT = "?"

    # ---- lifetime ---------------------------------------------------------
    def log_path(self, variant):
        """Where start() has the program's log written: the environment
        and the variant, and the program where it is not the testbed, so
        that another program's round of an environment and a stress
        round's are two files. The next round there writes it again;
        keep_log() in stress.py copies a failed one beside it."""
        if self.log_as:
            return os.path.join(OUT, self.log_as + ".log")
        name = self.NAME
        if self.program != Environment.program:
            name += "-" + self.program.removeprefix("fenestal-")
        return os.path.join(OUT, "%s-%s.log" % (name, variant))

    def start(self, variant="release", args=(), env=None):
        raise NotImplementedError

    def close(self):
        pass

    def alive(self):
        return self.testbed.alive()

    def pid(self):
        return self.testbed.proc.pid

    def cpu_seconds(self):
        """Processor time the program under test has used so far, user and
        system together. Raises once it has gone."""
        j = cpu_jiffies(self.pid())
        if j is None:
            raise RuntimeError("the process under test has gone")
        return j / HZ

    def stop(self, sig=15):
        return self.testbed.stop(sig)

    def dump(self, n=25):
        return self.testbed.dump(n)

    # ---- what the application says ----------------------------------------
    def state(self):
        return self.testbed.state()

    def mark(self):
        return self.testbed.mark()

    def since(self, mark, kind=None):
        return self.testbed.since(mark, kind)

    def wait_for(self, kind, mark, timeout=1.5):
        return self.testbed.wait_for(kind, mark, timeout)

    def patience(self):
        """What every wait is stretched by: a sanitizer build answers
        several times as slowly, and so does a GPU that powers itself off
        when idle (_OnBare)."""
        return 3 if self.slow else 1

    def device_said(self, timeout=2.0):
        """(name, type) of the device the program says it draws on, the
        type as its inventory prints it ("discrete"), or None if it has
        said neither by `timeout` or before it ended. Its "GPU in use" line
        where it had a choice; where it had none the inventory is one line,
        and it is complete once anything follows it. Read up to OPENED
        alone, as ledger.device() is: past it a "GPU in use" is a move's."""
        end = time.time() + timeout
        while True:
            # Asked before the lines are read: a program gone by then has
            # printed all it will, once the reader has taken what was still
            # in the pipe -- bounded, since wine can hold the pipe open.
            ended = not self.testbed.alive()
            if ended:
                self.testbed.drained(0.5)
            with self.testbed.lock:
                lines = list(self.testbed.lines)
            kinds, last, gap = {}, None, False
            for i, line in enumerate(lines):
                # The other renderers' GPUs are listed ahead of the drawing
                # one's inventory too, where GL draws or the backend shares
                # a driver (gpu_list_early()): only a listing after the
                # inventory ends the opening.
                if OPENED.match(line) and (kinds or
                                           line.startswith("moving to ")):
                    break
                if IN_USE in line:
                    name = line.split(IN_USE, 1)[1].strip()
                    return name, kinds.get(name, "")
                m = LISTED.search(line)
                if m:
                    # A second run is the whole list said after the first
                    # frame, as in ledger.device().
                    if gap:
                        break
                    # "discrete" of "discrete, Vulkan": a program that spans
                    # renderers names each device's after its type.
                    kinds[m.group(1)] = m.group(2).split(",")[0].strip()
                    last = i
                elif kinds:
                    gap = True
            if len(kinds) == 1 and last is not None and last + 1 < len(lines):
                return next(iter(kinds.items()))
            if ended or time.time() >= end:
                return None
            time.sleep(POLL)

    def device_check(self, want):
        """The device said at the top of the round, and Unavailable where
        `want` (FNL_GPU) names one the program is not on: a name the machine
        does not have falls back in the library with a warning, which here
        is a round about the wrong device, so it stops as an absent layer
        does. An index is not checked; the order is the driver's. Returns
        what device_said() did. Four seconds stretched, since the wait ends
        at the line: under TSan with NVIDIA's ICD beside Mesa's,
        vkCreateInstance alone is 5.4 s on schenker. Thirty more in a round
        on a kernel driver, whose program may move the GPU before it picks
        one: NVIDIA's module loaded by a move there and back from nouveau,
        2 to 3 s on the laptops."""
        wait = 4.0 * self.patience()
        if getattr(self, "driver", None):
            wait += 30.0
        said = self.device_said(wait)
        if said:
            print("  %s%s" % (IN_USE, said[0]), flush=True)
        if want and not want.isdigit() and \
                want.lower() not in (said[0] if said else "").lower():
            self.stop()
            self.close()
            raise Unavailable("the round asked for %r and the program on %s "
                              "is on %s" % (want,
                                            self.machine or "this machine",
                                            said[0] if said else
                                            "a device it did not name"))
        return said

    def settle(self, kind, mark, timeout=2.0):
        """wait_for(), with the timeout stretched for a sanitizer build: the
        wait for the library to say a transition has happened."""
        return self.testbed.wait_for(kind, mark, timeout * self.patience())

    def until(self, pred, timeout=1.5):
        """Wait until `pred()` holds, and say whether it came to.

        The wait for something a check is about to assert: what ends it is
        the evidence, and the timeout is only how long its absence is
        believed. A process that has exited will say nothing more."""
        end = time.time() + timeout * self.patience()
        while not pred():
            if time.time() >= end or not self.alive():
                return bool(pred())
            time.sleep(POLL)
        return True

    def quiet(self, pause=QUIET, timeout=2.0):
        """Wait until the echo has said nothing for `pause`: the wait for
        "and nothing else". See Testbed.quiet()."""
        p = self.patience()
        return self.testbed.quiet(pause * p, timeout * p)

    def edge(self, key, mark, which="release"):
        """Whether that edge of that key has been echoed since the mark."""
        return any(e["parts"][1] == key and e["parts"][2] == which
                   for e in self.since(mark, "key"))

    def press(self, key):
        """tap(), and wait for the release to be echoed as well.

        tap() returns on the first sign of the key, which is the press. A
        mark taken then would count this key's release as the next one's."""
        m = self.mark()
        self.tap(key)
        return self.until(lambda: self.edge(key, m))

    def let_go(self, handle):
        """key_up(), and wait for the release to be echoed."""
        m = self.mark()
        self.key_up(handle)
        return self.until(lambda: self.edge(handle, m))

    def refresh(self):
        """The state as of *now*, not as of the last event.

        The newest state on record is whatever the last event stamped, and
        after a click that maximizes the window that is still the press. So
        this makes something happen.

        A captured pointer has no motion events -- that is what capturing it
        means -- so a refresh that waits for one never returns. A nudge by a
        delta arrives as FNL_EVENT_RELATIVE and carries the same stamp."""
        st = self.testbed.state()
        if st and st["cap"]:
            m = self.mark()
            self.move_by(3, 0)
            self.wait_for("relative", m, timeout=1.5)
            return self.testbed.state()
        return self._refresh_free()

    def _refresh_free(self):
        """The same, for a pointer that still has a position."""
        raise NotImplementedError

    # ---- input ------------------------------------------------------------
    def move_to(self, wx, wy):
        raise NotImplementedError

    def move_by(self, dx, dy):
        """By a delta, in points, which is the only way to drive a *captured*
        pointer: there is no position then."""
        raise NotImplementedError

    def click(self, wx, wy, settle=0.35):
        raise NotImplementedError

    def drag(self, x0, y0, x1, y1, steps=10, settle=0.4):
        raise NotImplementedError

    def scrub(self, x0, y0, dx, dy, seconds, steps=10):
        """One press carried out and back for that long: Driver.scrub()."""
        raise NotImplementedError

    def tap(self, key, hold=0.05):
        raise NotImplementedError

    def key_down(self, key):
        raise NotImplementedError

    def key_up(self, handle):
        raise NotImplementedError

    def scroll(self, steps):
        raise NotImplementedError

    def force_output(self, name, state):
        """Have the connector of that name unplugged ("off") or plugged
        back ("detect"); "forced-hotplug" only."""
        raise NotImplementedError

    def focus(self):
        """Take the input focus, as firmly as this window server allows."""

    def make_room(self, room):
        """Leave `room` points free around the window on its monitor, by
        moving it and taking it in, where the harness can find it on the
        screen and the server cannot be asked; True if it was taken in.
        Nothing, for most: a window with "server-rect" is taken in by the
        round itself, and one nothing can locate is left where it is."""
        return False

    def move_window_by(self, dx, dy):
        """Move the window itself by a screen delta, in pixels.

        A delta and not a place, because that is the form all three can do:
        the Wayland backend can neither be told where its window is nor
        asked, and moves by dragging its own titlebar."""
        raise NotImplementedError

    # ---- what only some can answer ("server-rect") ------------------------
    def rect(self):
        raise NotImplementedError

    def monitor(self):
        raise NotImplementedError

    def work_area(self):
        raise NotImplementedError

    def has_focus(self):
        raise NotImplementedError

    def frame_extents(self):
        raise NotImplementedError

    def wm_class(self):
        raise NotImplementedError

    def wm_name(self):
        """The window's title as the window server has it, where "wm-props"
        can say."""
        raise NotImplementedError

    def input_hold(self):
        """The hold on the physical input of the machine this round
        drives (input_hold.py): this desk's own, as root by ssh, for every
        environment whose input comes from here -- the guests' included,
        which VirtualBox feeds from this desk's devices."""
        import input_hold
        return input_hold.SshHold("this desk", [
            "ssh", *root_key_args(), "-o", "BatchMode=yes",
            "root@localhost"])

    def reparented(self):
        raise NotImplementedError

    def pointer_pos(self):
        raise NotImplementedError

    def capture(self, path):
        """A picture, or None. Not in CAN: all three can produce one. Whose
        picture it is differs, and that is "composited"."""
        return None

    def target_monitor(self):
        """(name, x, y, w, h) of the monitor the round runs on, or None. The
        module's own answer for a window on this desktop; an environment
        whose window is on another says which that is."""
        return target_monitor()

    def monitors(self):
        """Every output of the desktop the window is on, as the module's
        monitors() gives this one's. Asked of the environment for the reason
        target_monitor() is."""
        return monitors()

    def fullscreen_px(self, mon):
        """The pixel size a fullscreen window reports on `mon`, as the
        monitor list gives it. The monitor's own size everywhere but a
        Wayland window on this desk, whose compositor mode is its logical
        size at the window's own scale -- see Wayland.fullscreen_px()."""
        return (mon[3], mon[4])

    # ---- the clipboard's other program ------------------------------------
    #
    # A window that pastes what it copied has shown only that it remembers a
    # string, so the far end of every clipboard check is somebody else. On
    # this desktop that is xclip for all three: the compositor bridges the X
    # selection and the Wayland one, so one tool reads what any
    # environment offered and offers what any can read.
    def clipboard_missing(self):
        """Why there is no other program to ask, or "" when there is one."""
        return "" if shutil.which("xclip") else \
            "no xclip to be the other program"

    def clipboard_get(self):
        """What another program reads off the clipboard right now, in
        bytes."""
        try:
            return subprocess.run(
                ["xclip", "-o", "-selection", "clipboard"],
                capture_output=True, timeout=5).stdout
        except subprocess.TimeoutExpired:
            return b"(xclip timed out)"

    def clipboard_put(self, data):
        """Have another program offer `data`."""
        # -quiet keeps xclip in the foreground, serving the selection, so the
        # process kept here is the one clipboard_done() ends: never an xclip
        # of the desktop's own.
        p = subprocess.Popen(["xclip", "-quiet", "-i", "-selection",
                              "clipboard"],
                             stdin=subprocess.PIPE,
                             stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        _XCLIPS.append(p)

        def feed():
            try:
                p.stdin.write(data)
                p.stdin.close()
            except (BrokenPipeError, ValueError):
                pass        # it exited, which clipboard_get() will show

        # Bounded on a thread: the input is past a pipe buffer, and an xclip
        # with no server to reach would stall the sweep.
        t = threading.Thread(target=feed, daemon=True)
        t.start()
        t.join(10)
        if t.is_alive():
            p.kill()
            p.wait()
            raise subprocess.TimeoutExpired(p.args, 10)

    def clipboard_done(self):
        # Each xclip of the round's that is still serving: one that lost the
        # selection to a later offer has exited already.
        while _XCLIPS:
            p = _XCLIPS.pop()
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()


# ---- the way in, which all three share ---------------------------------------

class _Portal(Environment):
    """Input through the RemoteDesktop portal, aimed by the testbed's own echo.

    Everything a round does *to* a window is here, once. A subclass starts
    its process and says what can be asked about the window it got.

    `self.portal` is whatever injects: wayland_driver.Portal for a window on
    this desktop, and vbox_driver.Guest -- the same calls, put to a virtual
    machine's own devices -- for one that is not."""

    INPUT = "portal"

    def open_portal(self):
        import wayland_driver as W
        self.W = W
        try:
            # prompt=False: a round is not the place to meet a consent dialog,
            # which is modal and takes the keyboard. See --consent.
            self.portal = W.Portal(verbose=False, prompt=False).start()
        except W.PortalUnavailable as e:
            raise Unavailable(str(e))

    def _gain(self):
        """Portal units to a window point. See Driver.gain."""
        return 1.0

    def _portal_per_px(self):
        """Portal points per pixel the pointer counts in. 1 here, where the
        portal moves in the same points a window reports."""
        return 1.0

    def _desk_portal(self):
        """Whether the pointer is this desk's RemoteDesktop portal, which
        moves in the compositor's logical points, or a bare machine's own
        hands, which move in its X server's pixels."""
        return isinstance(getattr(self, "portal", None),
                          getattr(getattr(self, "W", None), "Portal", ()))

    def _xwayland_scale(self):
        """How many X pixels a compositor logical point is worth here: the
        X screen's Xft.dpi over 96, which is the scale the library's X11
        backend reads too, and the factor between the monitor list (which
        XWayland reports) and the portal's points. 1 where the pointer is a
        bare machine's hands, which take the X server's own pixels."""
        gx = getattr(self, "_gx", None)
        if gx is None:
            gx = 1.0
            if self._desk_portal():
                try:
                    said = subprocess.run(["xrdb", "-query"],
                                          capture_output=True, text=True,
                                          timeout=5).stdout
                except Exception:
                    said = ""
                m = re.search(r"^Xft\.dpi:\s*(\d+)", said, re.MULTILINE)
                if m:
                    gx = max(1.0, int(m.group(1)) / 96.0)
            self._gx = gx
        return gx

    # ---- whose keyboard it is ---------------------------------------------
    #
    # The portal types into the desktop, not into a window: a key goes to
    # whatever the compositor holds focused, and these rounds paste and press
    # Enter. So no key is pressed without asking first, and one that cannot
    # be shown to be going to the window under test is not pressed at all.
    # The round then fails on the spot, having nothing true left to report.
    def keyboard_is_ours(self):
        """Whether the window under test has the keyboard. None for cannot
        tell, which is refused the same as no."""
        return None

    def _guard(self):
        if self.keyboard_is_ours():
            return
        # Asked for once -- a window just opened or restored is often a
        # moment behind -- and then waited on.
        self.focus()
        end = time.time() + 2.0
        while time.time() < end:
            if self.keyboard_is_ours():
                return
            time.sleep(3 * POLL)
        raise InputRefused(
            "the window under test does not have the keyboard, so the "
            "key was not sent -- it would have gone to another window")

    # ---- whether a key arrived --------------------------------------------
    #
    # Having the keyboard is necessary and not sufficient. The application's
    # echo is the only thing that says a key *arrived*, so each one is waited
    # for there, and one that never shows is sent once more after the focus
    # has been asked for again.
    #
    # Twice at most, and only after silence: a key that arrived and was
    # pressed again would undo every toggle a round presses. Silence is not
    # proof, though -- a first press can arrive after the wait for it ended
    # -- so a key sent twice is counted afterwards (_twice()).
    HEARD = ("key", "text", "compose", "close")

    def _heard_window(self):
        return 6.0 if self.slow else 1.5

    def _heard(self, mark):
        end = time.time() + self._heard_window()
        while time.time() < end:
            if not self.alive():
                return True         # it closed the window, which is an answer
            if any(e["kind"] in self.HEARD for e in self.since(mark)):
                return True
            time.sleep(POLL)
        return False

    def _presses(self, what, mark):
        return sum(1 for e in self.since(mark, "key")
                   if e["parts"][1:3] == [what, "press"])

    def _twice(self, what, first, again):
        """After a resend that was heard: whether the first press arrived
        as well, late, waited for as long as the first wait was. Both
        reaching the program is a toggle undone, and every later toggle of
        the round off by one -- a fullscreen window where the round thinks
        it is not, and failures far from the key. `again` presses it a
        third time, which leaves a toggle where one press would; not for a
        key that typed, which is not a toggle, and is reported alone."""
        end = time.time() + self._heard_window()
        while time.time() < end and self.alive() and \
                self._presses(what, first) < 2:
            time.sleep(POLL)
        if not self.alive() or self._presses(what, first) < 2:
            return
        typed = bool(self.since(first, "text"))
        if again is None or typed:
            print("  (%s arrived twice: the first press was late, not lost)"
                  % what, flush=True)
            return
        print("  (%s arrived twice: the first press was late, not lost; "
              "pressed a third time, so that a toggle ends where one press "
              "leaves it)" % what, flush=True)
        self._guard()
        m = self.mark()
        again()
        if not self._heard(m):
            print("  (%s's third press was not delivered)" % what,
                  flush=True)

    def _deliver(self, what, send, undo=None, again=None):
        """Send a key and see it arrive; False if it did not, twice.
        `again` is how to press it once more where both sends arrived
        (_twice())."""
        first = self.mark()
        for attempt in range(2):
            self._guard()
            m = self.mark()
            send()
            if self._heard(m):
                if attempt:
                    self._twice(what, first, again)
                return True
            if undo:
                undo()
            if attempt == 0:
                # No pause here: the next turn's _guard() waits until the
                # focus is back, or refuses.
                print("  (%s was not delivered; taking the focus and asking "
                      "again)" % what, flush=True)
                self.focus()
        print("  (%s was not delivered a second time)" % what, flush=True)
        return False

    # Between the legs of a blind park. There is nothing to wait on: no
    # window exists yet, and nothing outside the compositor can say where
    # the pointer is. It lets one run of motion land before the next turns.
    PARK_BEAT = 0.15

    def park_pointer(self):
        """Put the pointer well inside the target monitor, blind.

        Blind because the portal gives relative motion only. So the position
        is *established*, not read: relative motion clamps the pointer into a
        corner of the outputs' union, and a corner is the one place whose
        coordinates are known without being told.

        The corner has to be one inside the target: with monitors of
        different heights some corners of the union lie on no screen and
        clamp somewhere unpredictable. If the target has none the pointer is
        left alone and find_window() sweeps.

        Returns the name of the monitor parked on, or None."""
        tgt = target_monitor()
        box = root_box()
        if not tgt or not box:
            return None
        name, mx, my, mw, mh = tgt
        bx, by, bw, bh = box
        # The union's corners, and whether each is inside the target. A corner
        # is the far edge less one pixel; the clamp lands on the last pixel.
        for cx, cy, sx, sy in ((bx, by, -1, -1),
                               (bx + bw - 1, by, +1, -1),
                               (bx, by + bh - 1, -1, +1),
                               (bx + bw - 1, by + bh - 1, +1, +1)):
            if not (mx <= cx < mx + mw and my <= cy < my + mh):
                continue
            # One axis at a time, because a diagonal can enter the gap in a
            # union that is not a rectangle, where each axis alone always has
            # somewhere legal to go. In steps, because GNOME puts a pointer
            # barrier on a monitor boundary and a barrier is beaten by
            # sustained motion, not by one large jump. Twice over, for the
            # layout that wanted the axes in the other order.
            span = max(bw, bh) + 200
            for _ in range(2):
                for _ in range(span // STEP + 1):
                    self.portal.rel(sx * STEP, 0)
                time.sleep(self.PARK_BEAT)
                for _ in range(span // STEP + 1):
                    self.portal.rel(0, sy * STEP)
                time.sleep(self.PARK_BEAT)
            # Then a quarter of the monitor inwards, clear of the panel and
            # of whatever the compositor reserves at the edges.
            p = self._portal_per_px()
            self.portal.rel(-sx * (mw // 4) * p, -sy * (mh // 4) * p)
            time.sleep(self.PARK_BEAT)
            self.parked_on = name
            # Which way "inwards" was, for an environment that goes on to centre
            # its window on this monitor and wants the pointer over it.
            self.parked_dir = (sx, sy, mw, mh)
            return name
        return None

    def close(self):
        if getattr(self, "portal", None):
            self.portal.close()
            self.portal = None

    def _refresh_free(self):
        return self.drv.refresh()

    def move_to(self, wx, wy):
        return self.drv.move_to(wx, wy)

    def move_by(self, dx, dy):
        g = self._gain() * self._portal_per_px()
        self.portal.rel(dx * g, dy * g)
        self.drv.at = None          # the mapping is unknown again

    def click(self, wx, wy, settle=0.35):
        self.drv.click(wx, wy, settle=settle)

    def click_here(self, settle=0.35):
        self.drv.click_here(settle=settle)

    def drag(self, x0, y0, x1, y1, steps=10, settle=0.4):
        self.drv.drag(x0, y0, x1, y1, steps=steps, settle=settle)

    def scrub(self, x0, y0, dx, dy, seconds, steps=10):
        return self.drv.scrub(x0, y0, dx, dy, seconds, steps=steps)

    # ---- the desk's own layout ("desk-layout") ----------------------------
    #
    # A window on this desktop types through the layout the compositor
    # holds, whichever of the four it is: the native Wayland one is sent it,
    # XWayland mirrors it for the two X ones, and wine reads it off either.
    # So a real dead key, from a real layout, is one switch away for all of
    # them -- GNOME's, since that is whose setting it is.
    #
    # German with its dead keys: the key right of the sharp s is a dead
    # acute, a dead grave with Shift, and E and U are where every Latin
    # layout has them.
    DESK_DEAD_LAYOUT = "[('xkb', 'de')]"
    DESK_DEAD_KEY = "Equal"
    _DESK_KEY = ["gsettings", "get", "org.gnome.desktop.input-sources",
                 "sources"]

    def desk_layout_missing(self):
        """Why the desk's layout cannot be switched, or "" when it can."""
        if not shutil.which("gsettings"):
            return "no gsettings to switch the desk's layout with"
        if "GNOME" not in os.environ.get("XDG_CURRENT_DESKTOP", ""):
            return "the desk's layout is switched through GNOME's settings"
        return ""

    def desk_dead_layout(self):
        """Switch the desktop to DESK_DEAD_LAYOUT; returns what undoes it.

        The user's own setting, changed under them for the length of a few
        keys: put back by the caller's `finally`, and at exit as well, for
        a round that is killed between the two."""
        got = subprocess.run(self._DESK_KEY, capture_output=True,
                             text=True, timeout=10)
        was = got.stdout.strip()
        # Never switched without the setting to put back: the user's
        # layouts would be lost for good.
        if got.returncode != 0 or not was:
            raise RuntimeError("the desk's layouts could not be read, so "
                               "they are left as they are: %s"
                               % (got.stderr.strip() or "nothing said"))
        put = ["gsettings", "set"] + self._DESK_KEY[2:]

        def undo():
            subprocess.run(put + [was], timeout=10)
            atexit.unregister(undo)

        atexit.register(undo)
        try:
            subprocess.run(put + [self.DESK_DEAD_LAYOUT], timeout=10)
        except subprocess.TimeoutExpired:
            undo()
            raise
        return undo

    def on_output(self, out, name, home):
        """Whether the output the application names is the monitor `name`,
        which is this desktop's word for it. By the first word: X11 gives
        the RandR connector "HDMI-1" and Wayland wl_output's description
        "HDMI-1 (IVM PL4071UH)", and the connector is the word both begin
        with -- whole, so DP-1 is not DP-10 or DP-1-1. `home` is what the
        application called the monitor it started on."""
        words = out.split()
        return bool(words) and words[0] == name

    def tap(self, key, hold=0.05, confirm=True):
        """Press and release. `confirm=False` is for a key the application is
        never shown -- one an input method takes -- where silence is the
        expected answer and a second press would be a second character."""
        if not confirm:
            self._guard()
            self.portal.tap(key, hold=hold)
            return True
        def press():
            self.portal.tap(key, hold=hold)
        return self._deliver(key, press, again=press)

    def key_down(self, key):
        self._deliver(key, lambda: self.portal.key(key, True),
                      undo=lambda: self.portal.key(key, False))
        return key

    def key_up(self, handle):
        # Never guarded: a key that went down has to come up, wherever the
        # focus has gone since.
        self.portal.key(handle, False)

    def scroll(self, steps):
        self.portal.scroll(steps)


# ---- a window whose place can be asked ---------------------------------------

class _Placed:
    """What follows from "server-rect", for the environments that have it: given
    rect() and _aim(), a window that went quiet is found by asking where it
    is and not by sweeping for it."""

    def _refresh_free(self):
        """A fresh stamp, without going looking for the window.

        The echo's own refresh, hearing nothing, sweeps the desktop -- right
        on Wayland, where nothing else can say where the window went. Here
        the window server can, and the usual reason for silence is a drag that
        shrank the window out from under the pointer. So: nudge, and if that
        is not heard, put the pointer back in the middle of the scene by the
        server's account and nudge again. Only then is it the echo's
        problem, and its forty-second sweep."""
        wait = 0.5 * (3.0 if self.slow else 1.0)
        for attempt in range(2):
            m = self.mark()
            self.portal.rel(3, 0)
            ev = self.wait_for("motion", m, timeout=wait)
            if ev:
                self.drv.at = (float(ev["parts"][1]), float(ev["parts"][2]))
                return self.state()
            if attempt == 0:
                st = self.state() or {}
                w, h = st.get("area", (400, 400))
                try:
                    _x, _y, rw, rh = self.rect()
                    g = self._gain()
                    w, h = rw / g, rh / g
                except Exception:
                    pass
                if not self._aim(w / 2, h / 2):
                    break
        self.drv.at = None
        return self.drv.refresh()

    def rect_still(self, hold=0.2, timeout=2.0):
        """The rectangle once the server has stopped changing it: the same
        answer for `hold` on end. What a move, a drag or a resize is waited
        out by, since the manager acts on each some time after it is asked."""
        p = self.patience()
        was, since = self.rect(), time.time()
        end = since + timeout * p
        while time.time() - since < hold * p and time.time() < end:
            time.sleep(2 * POLL)
            now = self.rect()
            if now != was:
                was, since = now, time.time()
        return was


# ---- X11, and Win32 under wine ---------------------------------------------

class _XEnvironment(_Placed, _Portal):
    """The shared half of the two environments that live on an X server.

    A wine window *is* an X11 window on XWayland, so what separates the two
    below is the binary and the process name. The X server answers what a
    round reads; it is not how a round drives."""

    CAN = frozenset({"server-rect", "edge-resize", "unminimize",
                     "read-pixels", "desk-layout", "resizable"})

    def start(self, variant="release", args=(), env=None):
        import x11_driver as X
        self.X = X
        tree = self.TREE or self.NAME
        exe = os.path.join(ROOT, "build", tree, variant,
                           self.program + self.SUFFIX)
        if not os.path.exists(exe):
            raise Unavailable("no %s -- ./build.sh %s %s"
                              % (os.path.relpath(exe, ROOT), tree, variant))
        X.kill_stale()
        self.clear()
        exe = self.stage(exe, variant)
        self.open_portal()
        monitors(refresh=True)
        # Before the window exists: mutter places a new window on the
        # monitor the pointer is on, X window or not. pin_to_target() below
        # corrects the ones it places elsewhere.
        self.park_pointer()
        self.testbed = Testbed(self.RUNNER + [exe] + list(args),
                         env=self.environ(**{**self.ECHO, **(env or {})}),
                         log=self.log_path(variant))
        self.win = X.find_window(pid=self._window_pid())
        self.slow = variant in ("asan", "tsan")
        self.drv = self.W.Driver(self.portal, self.testbed, slow=self.slow,
                                 gain=self._driver_gain, aim=self._aim,
                                 monitors=self.monitors)
        self.pin_to_target()
        # The answer is not acted on here. The round asks the same question
        # as its first check: a start() that raised would report a focus
        # regression as a skipped environment, which nobody reads as a finding.
        X.focus_settled(self.win)
        self.drv.find_window()
        return self

    def _gain(self):
        # An X window is measured in X pixels. The scale is the one the
        # application reports, which cannot disagree with what it drew.
        st = self.testbed.state() or {"scale": 120}
        return st["scale"] / 120.0

    def _portal_per_px(self):
        return 1.0 / self._xwayland_scale()

    def _driver_gain(self):
        """Portal points per window point: a window's points are the
        library's logical points, so on this desk they are the portal's
        too -- except under wine, whose window counts the X screen's
        pixels, one XWayland scale above them. A bare machine's hands take
        those X pixels, which is what _gain() says."""
        return self._gain() * self._portal_per_px()

    def _aim(self, wx, wy, tries=8):
        """Put the pointer at a window point by asking the X server where both
        are, or return None to leave it to the echo.

        An X environment can read back where a delta left the pointer and knows
        where its window is, so the loop closes without the application
        saying anything -- which under wine it does not over the titlebar,
        that being non-client area.

        Not trusted blind. XWayland only learns where the pointer is while it
        is over an X window, so a position read elsewhere may be a monitor
        stale. A move that leaves the reading where it was is that case, and
        so is one that never converges; both hand over to the echo."""
        try:
            x, y, _w, _h = self.rect()
        except Exception:
            return None
        g = self._gain()
        tx, ty = x + wx * g, y + wy * g
        p = self._portal_per_px()
        moved = False
        for _ in range(tries):
            px, py = self.X.pointer_pos()
            dx, dy = tx - px, ty - py
            if abs(dx) <= 1 and abs(dy) <= 1:
                if moved:
                    return (wx, wy)
                # Already there, by a reading nothing has yet confirmed: step
                # off and let the loop walk back, so that "there" is observed.
                dx = 4 if dx <= 0 else -4
            self.portal.rel(dx * p, dy * p)
            # A fixed beat and not a poll for the first change: the first
            # reading that differs is not reliably the last, and aiming off
            # it lost the window at a resize corner three rounds in ten.
            time.sleep(AIM_BEAT)
            if self.X.pointer_pos() == (px, py):
                return None
            moved = True
        return None

    def pin_to_target(self):
        """Move the window onto the target monitor if it opened elsewhere.

        One round, one monitor, so a geometry figure means the same thing in
        every environment.

        Undecorated and unreparented here, so a plain ConfigureWindow moves
        it; a managed frame would need _NET_MOVERESIZE_WINDOW. Not asserted:
        a manager may refuse, and the round reads the rectangle back."""
        tgt = target_monitor()
        if not tgt:
            return None
        name, mx, my, mw, mh = tgt
        try:
            x, y, w, h = self.rect()
            if (mx <= x + w // 2 < mx + mw and my <= y + h // 2 < my + mh):
                self.parked_on = name
                return name
            # Centred, so that a window wider than the one it opened on still
            # lands inside and the drag checks have room on every side.
            nx, ny = mx + (mw - w) // 2, my + (mh - h) // 2
            self.win.configure(x=nx, y=ny)
            self.X.d.sync()
            self.rect_still()
            # And the pointer follows, for move_window_by()'s reason. It was
            # parked a quarter of the monitor in from a corner; another
            # quarter the same way is the centre, where the window now is.
            pd = getattr(self, "parked_dir", None)
            if pd:
                sx, sy, pw, ph = pd
                p = self._portal_per_px()
                self.portal.rel(-sx * (pw // 4) * p,
                                -sy * (ph // 4) * p)
            # Where that left the pointer is find_window()'s to observe.
            self.drv.at = None
        except Exception:
            return None
        self.parked_on = name
        return name

    def _window_pid(self):
        return self.testbed.proc.pid

    ENV = {}

    def focus(self):
        self.X.focus_settled(self.win)

    def move_window_by(self, dx, dy):
        x, y, w, h = self.rect()
        self.win.configure(x=int(x + dx), y=int(y + dy))
        self.X.d.sync()
        self.rect_still()
        # The pointer has to follow the window: the state a round reads is
        # the last event's stamp, and a window moved out from under the
        # pointer stamps no more. In steps, because a monitor boundary's
        # pointer barrier stops one large jump (see park_pointer).
        n = max(1, int(max(abs(dx), abs(dy)) // STEP) + 1)
        for _ in range(n):
            self.portal.rel(dx / n, dy / n)
        # Where that left it is the next refresh()'s to observe.
        self.drv.at = None

    def rect(self):
        return self.X.geom(self.win)

    def monitor(self):
        """The monitor rectangle the window's centre lies in, else the first
        one, else None."""
        x, y, w, h = self.rect()
        cx, cy = x + w // 2, y + h // 2
        mons = monitors()
        for _name, _pri, mx, my, mw, mh in mons:
            if mx <= cx < mx + mw and my <= cy < my + mh:
                return (mx, my, mw, mh)
        return mons[0][2:] if mons else None

    def work_area(self):
        return self.X.work_area()

    def has_focus(self):
        return self.X.is_focused(self.win)

    def keyboard_is_ours(self):
        # The X server's account and the manager's, both: see
        # x11_driver.is_focused().
        return self.has_focus()

    def minimized(self):
        return self.X.is_hidden(self.win)

    def pointer_pos(self):
        return self.X.pointer_pos()

    def capture(self, path):
        from PIL import Image
        self.X.capture(self.win, path)
        return Image.open(path).convert("RGB")

    def unminimize(self):
        """Activate it, and wait until it is back and has the keyboard.

        Activation and not a MapWindow: under a compositing manager an
        iconified window is still mapped, so mapping it asks for nothing.
        The manager's hidden state is what both builds share; wine's
        WS_MINIMIZE is invisible from this side. Returns whether it came
        back."""
        ok = self.X.focus_settled(self.win, timeout=6.0)
        end = time.time() + 3.0
        while self.minimized() and time.time() < end:
            time.sleep(3 * POLL)
        return ok and not self.minimized()

    def frame_extents(self):
        from Xlib import X as _X
        try:
            p = self.win.get_full_property(
                self.X.d.intern_atom("_NET_FRAME_EXTENTS"), _X.AnyPropertyType)
            return tuple(p.value) if p else (0, 0, 0, 0)
        except Exception:
            return (0, 0, 0, 0)

    def reparented(self):
        """Whether a manager put a frame round it, and what it did instead.

        Two ways of being right: the parent is still the root, meaning no
        frame was ever made -- mutter's way with an undecorated window -- or
        it is a frame exactly the client's own size."""
        parent = self.win.query_tree().parent
        if parent.id == self.X.root.id:
            return False, "not reparented"
        pg = parent.get_geometry()
        me = self.rect()
        same = pg.width == me[2] and pg.height == me[3]
        return (not same), ("frame %dx%d vs client %dx%d"
                            % (pg.width, pg.height, me[2], me[3]))

    def wm_class(self):
        try:
            return self.win.get_wm_class()
        except Exception:
            return None

    def wm_name(self):
        try:
            prop = self.win.get_full_property(
                self.X.d.intern_atom("_NET_WM_NAME"), 0)
            if not prop:
                return None
            v = prop.value
            return v.decode("utf-8") if isinstance(v, bytes) else str(v)
        except Exception:
            return None

    def content_black_pct(self, top=40):
        """How much of the window below the titlebar is black, per cent.

        Read straight off the X server, so it costs no dependency and no
        portal. A handful of rows and not the whole window: a full get_image
        of a 4K window is over the maximum request size.

        What it catches is a window that has stopped *presenting* what it
        draws: a swapchain that never honoured a resize is alive and showing
        an image from three sizes ago. None where nothing could be read,
        which a check skips: 0 would pass for a window that fills."""
        from Xlib import X as _X
        g = self.rect()
        ww, hh = g[2], g[3]
        if ww < 8 or hh < top + 8:
            return 0.0
        black = tot = 0
        for yy in [top + (hh - top) * k // 6 for k in range(1, 6)]:
            try:
                img = self.win.get_image(0, yy, ww, 1, _X.ZPixmap, 0xffffffff)
            except Exception:
                return None
            data = img.data
            if isinstance(data, str):
                data = data.encode("latin-1")
            for i in range(0, len(data) - 3, 4 * 7):
                tot += 1
                if data[i] < 12 and data[i + 1] < 12 and data[i + 2] < 12:
                    black += 1
        return 100.0 * black / tot if tot else None

    def scene_row(self, top=40):
        """One row across the middle of the scene, every pixel, as bytes;
        None where it cannot be read.

        Two of them some frames apart differ while the scene moves -- its
        sweep crosses every row -- and are the same where the window has
        stopped showing what it draws: a frozen frame, which
        content_black_pct() reads as a frame like any other. Every pixel
        and not every seventh, since the sweep is narrower than seven."""
        from Xlib import X as _X
        g = self.rect()
        ww, hh = g[2], g[3]
        if ww < 8 or hh < top + 8:
            return None
        try:
            img = self.win.get_image(0, top + (hh - top) // 2, ww, 1,
                                     _X.ZPixmap, 0xffffffff)
        except Exception:
            return None
        data = img.data
        return data.encode("latin-1") if isinstance(data, str) else data


class X11(_XEnvironment):
    NAME = "x11-vulkan"
    TREE = "linux"
    BACKEND = "x11"
    # "wm-props": the hints, the reparenting and WM_CLASS are this library's
    # own doing here. On the wine-hosted build the window is wine's.
    #
    # "remap-key": the layout this environment types through is the X server's
    # keysym table, which any client may edit, so a key can be made a dead
    # key for the length of a check whatever layout the desk has. Not wine,
    # which has its own idea of the layout, and not Wayland, where the keymap
    # is the compositor's to send.
    CAN = _XEnvironment.CAN | frozenset({"wm-props", "multi-output",
                                         "output-pick", "remap-key"})

    # A keycode with nothing on it in any evdev-derived keymap: 97 is evdev
    # 89, KEY_RO, a key only a Japanese board has.
    SPARE_KEYCODE = 97

    def remap_spare(self, lo, up):
        """Put two keysyms on the spare key; returns what undoes it.

        ChangeKeyboardMapping is core protocol and not input: nothing is
        injected and nothing asks permission. The server tells every client
        its keymap changed, which is half of what the check is for."""
        d = self.X.d
        old = d.get_keyboard_mapping(self.SPARE_KEYCODE, 1)
        width = len(old[0])
        want = [(lo, up) + (0,) * (width - 2)]

        def put(rows):
            # Until the server answers with what it was given. The library
            # is told by an event the server queues at the same moment, so
            # it is ahead of any key pressed after this returns.
            d.change_keyboard_mapping(self.SPARE_KEYCODE, rows)
            d.sync()
            end = time.time() + 2.0
            while time.time() < end:
                got = d.get_keyboard_mapping(self.SPARE_KEYCODE, 1)
                if list(got[0]) == list(rows[0]):
                    return
                time.sleep(POLL)

        put(want)
        return lambda: put(old)

    def tap_spare(self, hold=0.05):
        # Not waited for: the key has no FnlKey, and what it types is what
        # the check that calls this is about to look for.
        self._guard()
        self.portal.keycode(self.SPARE_KEYCODE - 8, True)
        time.sleep(hold)            # the press itself
        self.portal.keycode(self.SPARE_KEYCODE - 8, False)


class X11GL(X11):
    """The same X11 window, drawn with OpenGL through EGL instead of Vulkan.
    Everything a round reads is the X server's and everything it sends is
    the portal's, so the window server's half is X11's whole; only the tree
    and the renderer differ."""
    NAME = "x11-gl"
    TREE = "linux"
    RENDERER = "gl"


# ---- Win32 under wine, on this desktop -------------------------------------

WINE_ENV = "FNL_DRIVE_WINE"
# WineHQ's packages, a directory each and none on the path; a word in
# WINE_ENV names one of them by its branch.
WINE_OPT = "/opt/wine-%s"
# Where a wine WINE_ENV names keeps its prefix, one a wine.
WINE_PREFIXES = os.path.join(testbed.STATE_DIR, "wine-prefix")

# The key root@localhost takes: FNL_DRIVE_ROOT_KEY, or the path in the state
# directory's root-key, beside the machines' lists. Neither leaves it to
# ssh's own configuration.
ROOT_KEY_FILE = os.path.join(testbed.STATE_DIR, "root-key")


def root_key_args():
    key = os.environ.get("FNL_DRIVE_ROOT_KEY")
    if not key:
        try:
            with open(ROOT_KEY_FILE, encoding="utf-8") as f:
                key = f.read().strip()
        except OSError:
            key = ""
    if not key:
        return []
    return ["-i", os.path.expanduser(key), "-o", "IdentitiesOnly=yes"]
_wine_versions: dict[str, str] = {}


def wines(*versions):
    """What `wine --version` says of these releases and their maintenance
    releases (10.0 is 10.0.1 too), as a pattern."""
    return re.compile(r"^wine-(%s)(\.\d+)?(\s|$)"
                      % "|".join(re.escape(v) for v in versions))


# The distribution's, of which every declaration here was first measured.
WINE_10 = wines("10.0")


def desk_wine():
    """(the wine to run, its prefix or ""): WINE_ENV's choice.

    Unset, the `wine` on the path, in whatever prefix this account's own
    environment gives it. A word, WineHQ's package of that branch
    (`stable`, `devel`, `staging`); a path, a wine binary or the directory
    whose bin/ holds one. Every wine named has a prefix of its own, under
    the state directory by the directory it is installed in: a prefix a
    later wine has opened is upgraded past the earlier one's, and two
    wines sharing one would each run in the other's."""
    named = os.environ.get(WINE_ENV, "")
    if not named:
        return "wine", ""
    path = WINE_OPT % named if os.sep not in named else named
    if os.path.isdir(path):
        path = os.path.join(path, "bin", "wine")
    path = os.path.abspath(path)
    home = os.path.dirname(os.path.dirname(path))
    return path, os.path.join(WINE_PREFIXES, os.path.basename(home))


def wine_says(wine):
    """What `wine --version` says, asked once; "" when it says nothing."""
    if wine not in _wine_versions:
        try:
            said = subprocess.run([wine, "--version"], capture_output=True,
                                  text=True, timeout=30).stdout.strip()
        except (OSError, subprocess.TimeoutExpired):
            said = ""
        _wine_versions[wine] = said
    return _wine_versions[wine]


class _DeskWine:
    """What the Windows environments on this desktop have of wine: which
    wine runs the program and in what prefix (desk_wine()), and which of
    their declarations were measured of which wine.

    A declaration a class makes of wine holds for the wines it has been
    seen to hold for, which WINE_HELD names beside the value it takes for
    any other: nothing is declared that has not been seen to hold, so
    another wine is put to the checks, and its round is what says which
    still do."""
    SUFFIX = ".exe"
    # The one Windows tree, with Direct3D 12 and Vulkan in each program:
    # every Windows environment is it, told its renderer.
    TREE: str | None = "win64"
    RUNNER = [desk_wine()[0]]
    # wine's own chatter is not this project's log, and a round that greps
    # its output for trouble would otherwise find somebody else's.
    ENV = {"WINEDEBUG": "-all"}
    # {attribute: (its value where the declaration is lifted, the wines it
    # holds for)}, put on the instance at start().
    WINE_HELD: dict[str, tuple[object, re.Pattern]] = {
        # Measured under 11.0 and 11.21: their own vkd3d draws.
        "NO_MOVE_TO": ({}, WINE_10)}

    @classmethod
    def environ(cls, **more):
        pfx = desk_wine()[1]
        return super().environ(**{**({"WINEPREFIX": pfx} if pfx else {}),
                                  **wine_layer_env(cls.RENDERER), **more})

    @classmethod
    def clear(cls):
        """The last program's wine server waited out, and the prefix made
        if this wine has not got one yet: with no display, so that nothing
        is shown, and without Mono and Gecko, whose installers would ask
        on the desktop. A prefix wineboot did not make is a skipped
        environment."""
        wine, pfx = desk_wine()
        if not shutil.which(wine):
            raise Unavailable("no wine at %s -- %s names one" % (wine,
                                                                 WINE_ENV))
        wine_clear()
        if not pfx or os.path.isfile(os.path.join(pfx, "system.reg")):
            return
        print("making the wine prefix %s" % pfx, flush=True)
        os.makedirs(pfx, mode=0o700, exist_ok=True)
        env = {k: v for k, v in os.environ.items()
               if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update(WINEPREFIX=pfx, WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=")
        try:
            subprocess.run([wine, "wineboot", "-i"], env=env, timeout=300,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise Unavailable("wineboot made no prefix in %s: %s" % (pfx, e))
        # Its server writes the registry out as it exits.
        wine_clear()
        if not os.path.isfile(os.path.join(pfx, "system.reg")):
            raise Unavailable("wineboot made no prefix in %s" % pfx)

    @classmethod
    def wine_version(cls):
        """The wine this environment runs, as it names itself, for the
        ledger: where WINE_ENV named one, and "" where the path's ran."""
        wine, pfx = desk_wine()
        return (wine_says(wine) or wine) if pfx else ""

    def start(self, variant="release", args=(), env=None):
        said = wine_says(self.RUNNER[0])
        for name, (lifted, held) in self.WINE_HELD.items():
            if not held.match(said):
                setattr(self, name, lifted)
        return super().start(variant, args, env)

    def on_output(self, out, name, home):
        # Wine calls its monitors DISPLAY1 and DISPLAY2, and which of this
        # desktop's connectors each is cannot be asked of it from here. What
        # can be said is that it is no longer the one the window began on;
        # the size that follows is the round's next check.
        return out != home


class Win32(_DeskWine, _XEnvironment):
    NAME = "win64-vulkan"
    # Wine's own Direct3D 12, which this environment loads: vkd3d makes
    # the device and lists the GPUs, and refuses every DXIL pipeline state,
    # so the move fails and comes back (tools/vkd3d_proton.py).
    NO_MOVE_TO = {"d3d12": "wine 10.0's own vkd3d runs no DXIL; the d3d12 "
                           "environments load vkd3d-proton"}
    # "multi-output": moved to the second monitor the window says so, in
    # wine's names for the two and not this desktop's. "output-pick": the
    # bar's output list calls fnl_request_output(), which under wine moves a
    # window as it does on X11.
    CAN = _XEnvironment.CAN | frozenset({"multi-output", "output-pick"})


def wine_clear():
    """Wait out the wine server a previous program left behind.

    It outlives its last program by a few seconds, with the desktop process
    that chose a display driver still in it, and a program started meanwhile
    joins that desktop whatever its own environment says: after the other
    driver's round it has no window at all (CreateWindowExW fails, 1400) or,
    the other way about, one on the wrong driver and nothing to say so.
    Waited for and not killed: a server that stays up is somebody's own
    wine program, and that is a skipped environment."""
    # By its process and not by `wineserver -w`: the distribution keeps that
    # binary off the PATH, and a wait that cannot run is no wait.
    end = time.time() + 20.0
    while time.time() < end:
        up = subprocess.run(["pgrep", "-u", str(os.getuid()), "-x",
                             "wineserver"], capture_output=True).stdout
        if not up.strip():
            return
        time.sleep(5 * POLL)
    raise Unavailable("a wine server is still up from another program, and "
                      "its display driver would be this one's")


# What a Debug win64 tree drawing with Direct3D 12 says where Windows'
# Graphics Tools are not installed, and vkd3d-proton has no debug layer
# either: a fact about the machine, said once at open.
D3D12_NO_LAYER = r"^the Direct3D 12 debug layer is not installed$"
# size_await()'s bound in win32.c, reached: a frame at the new size took
# longer than the window waits for one.
SIZE_UNSHOWN = (r"^no frame at the new size in \d+ ms; sizes are not "
                r"waited for until one is shown$")


def wine_layer_env(renderer):
    """A checker round's validation layer for a program under wine.

    The library names the layer to the loader its instance is made by
    (FNL_VK_VALIDATION), and under wine that is Windows' loader, which has
    no layer to give: winevulkan hands the instance to the host's loader,
    which has one but was never asked. So the host's loader is asked for
    it (VK_LOADER_LAYERS_ENABLE) and the library is not, which would only
    log that it found none. Every instance of the process gets it that
    way, which the round declines elsewhere (Zink's, stress.py) and takes
    here for Vulkan, whose program makes no instance but the library's;
    a Direct3D 12 round's vkd3d-proton makes its own, so it runs with no
    layer at all, and the Direct3D 12 debug layer is its checker."""
    if not validation_asked():
        return {}
    env = {"FNL_VK_VALIDATION": "0"}
    if renderer == "vulkan":
        layers = os.environ.get("VK_LOADER_LAYERS_ENABLE", "")
        if "validation" not in layers:
            layers = ",".join(x for x in (layers, "*validation") if x)
        env["VK_LOADER_LAYERS_ENABLE"] = layers
    return env


# "wine" puts the Direct3D 12 environments on wine's own vkd3d in place of
# vkd3d-proton, for a wine whose vkd3d runs DXIL (11.20's does, on the
# thinkpad); "proton", or nothing, is the default. Here and on the bare
# machines alike.
VKD3D_ENV = "FNL_DRIVE_VKD3D"
# What wine's own DXGI answers for frame statistics, which it has not got
# (E_NOTIMPL), under 11.0 and 11.21: said once a swapchain, and the
# library goes on without them. vkd3d-proton's DXVK has them.
WINE_DXGI_NO_STATS = (r"^no frame statistics \(0x80004001\): a frame that "
                      r"answers a size is not waited for until shown$")


class _Vkd3dProton:
    """The win64 tree under wine, told to draw with Direct3D 12, on
    vkd3d-proton and DXVK's DXGI (tools/vkd3d_proton.py says why wine
    10.0's own will not do). The program is run from a directory of its
    own under build/drive/, the tree's .exe and .dll files with the two
    releases' beside them, and wine is told to load those and not its own;
    the tree is left as build.sh made it. The directory is the
    environment's, by its name: the Vulkan environments run the same
    tree's program from the tree itself, where a dxgi.dll of DXVK's beside
    it would be the one their process loads.

    With VKD3D_ENV=wine the program is the tree's, as a Vulkan round's
    is, and wine's own Direct3D 12 draws."""
    RENDERER = "d3d12"
    # vkd3d-proton draws, so every GPU is one to move to.
    NO_MOVE_TO: dict[str, str] = {}
    # Errors only from vkd3d-proton, whose information lines would read as
    # the program's in a log check; nothing from DXVK, which here is only
    # DXGI and says at error level that wine's registry has no EDID. No
    # shader cache: a file in the run directory that outlives the round.
    ENV = {"WINEDEBUG": "-all", "VKD3D_DEBUG": "err",
           "VKD3D_SHADER_DEBUG": "err", "DXVK_LOG_LEVEL": "none",
           "VKD3D_SHADER_CACHE_PATH": "0"}
    EXPECTED_LOG: tuple[str, ...] = (D3D12_NO_LAYER,)

    @classmethod
    def vkd3d(cls):
        """VKD3D_ENV's choice, for the ledger: "wine" where it named wine's
        own, and "" for vkd3d-proton."""
        said = os.environ.get(VKD3D_ENV, "")
        if said not in ("", "proton", "wine"):
            raise Unavailable("%s is proton or wine, not %r"
                              % (VKD3D_ENV, said))
        return "wine" if said == "wine" else ""

    def expected_line(self, line):
        return super().expected_line(line) or bool(
            self.vkd3d() and re.search(WINE_DXGI_NO_STATS, line))

    @classmethod
    def stage(cls, exe, variant):
        # tools/vkd3d_proton.py, which is a directory above this one and
        # not on the path. The overrides are its, and are put into ENV here
        # because every start reads ENV after staging.
        if cls.vkd3d():
            # Its overrides less the DLLs: still no debugger.
            cls.ENV = dict(cls.ENV, WINEDLLOVERRIDES="winedbg.exe=d")
            return exe
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "vkd3d_proton", os.path.join(ROOT, "tools", "vkd3d_proton.py"))
        V = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(V)
        cls.ENV = dict(cls.ENV, WINEDLLOVERRIDES=V.OVERRIDES)
        try:
            extra = V.dlls()
            if not extra:
                V.fetch()
                extra = V.dlls()
        except (OSError, SystemExit) as e:
            raise Unavailable("no vkd3d-proton to run Direct3D 12 under "
                              "wine: %s" % e)
        tree = os.path.dirname(exe)
        run = os.path.join(OUT, "run-%s-%s" % (cls.NAME, variant))
        os.makedirs(run, exist_ok=True)
        srcs = [os.path.join(tree, f) for f in sorted(os.listdir(tree))
                if f.endswith((".exe", ".dll"))] + list(extra or ())
        for src in srcs:
            dst = os.path.join(run, os.path.basename(src))
            st = os.stat(src)
            try:
                same = os.stat(dst)
                if (same.st_size, same.st_mtime_ns) == (st.st_size,
                                                        st.st_mtime_ns):
                    continue
            except FileNotFoundError:
                pass
            # Written beside and renamed over: a sweep on another machine
            # stages the same directory, and must find a whole file.
            tmp = "%s.%d.tmp" % (dst, os.getpid())
            shutil.copy2(src, tmp)
            os.replace(tmp, dst)
        return os.path.join(run, os.path.basename(exe))


class Win32D3D12(_Vkd3dProton, Win32):
    """The win64 binary under wine on X11, told to draw with Direct3D 12:
    Win32's in everything but the renderer and what draws it
    (_Vkd3dProton)."""
    NAME = "win64-d3d12"


# ---- Wayland ----------------------------------------------------------------

class Wayland(_Portal):
    NAME = "wayland-vulkan"
    TREE = "linux"
    BACKEND = "wayland"
    # No "unminimize": xdg-toplevel has set_minimized and no counterpart.
    # No "server-rect": a client is never told where its window is.
    #
    # "input-method": the compositor's own composer, through the text-input
    # protocol. Whether this desktop's is listening is asked by the check.
    CAN = frozenset({"grab-ring", "composited", "multi-output",
                     "input-method", "desk-layout", "resizable"})
    NO_UNMINIMIZE = "a Wayland client cannot ask to be unminimized"
    # The open line naming the globals the compositor lacks: GNOME has no
    # xdg-decoration, weston more than that.
    EXPECTED_LOG: tuple[str, ...] = (
        r"^not bound, so their fallbacks are in use: ",)

    # After the window says it is focused and before the first probe. The
    # compositor's map animation outlasts its last event, and on Wayland
    # there is no server to ask whether it has finished.
    MAP_BEAT = 0.3
    def start(self, variant="release", args=(), env=None):
        tree = self.TREE or self.NAME
        exe = os.path.join(ROOT, "build", tree, variant,
                           self.program + self.SUFFIX)
        if not os.path.exists(exe):
            raise Unavailable("no %s -- ./build.sh %s %s"
                              % (os.path.relpath(exe, ROOT), tree, variant))
        for tool in self.RUNNER[:1]:
            if not shutil.which(tool):
                raise Unavailable("no %s on this machine" % tool)
        import wayland_driver as W
        testbed.kill_stale()
        self.clear()
        exe = self.stage(exe, variant)
        self.open_portal()
        monitors(refresh=True)
        # Before the window exists: mutter places a new window on the monitor
        # the pointer is on, and afterwards a Wayland client can neither be
        # asked where it is nor put anywhere.
        self.park_pointer()
        self.testbed = Testbed(self.RUNNER + [exe] + list(args),
                         env=self.environ(**{**self.ECHO, **(env or {})}),
                         log=self.log_path(variant))
        self.slow = variant in ("asan", "tsan")
        # Until the window says it has the keyboard, which it echoes once it
        # is mapped and activated; the timeout is what a start may take. Then
        # a beat for the compositor's own animation before the first probe.
        end = time.time() + (6.0 if self.slow else 3.5)
        while (not self.keyboard_is_ours() and self.alive()
               and time.time() < end):
            time.sleep(POLL)
        time.sleep(self.MAP_BEAT)
        self.drv = W.Driver(self.portal, self.testbed, slow=self.slow,
                            monitors=self.monitors)
        try:
            self.drv.find_window()
        except Exception:
            # Nothing is left up for the next round to find: the caller has
            # no environment to stop.
            self.stop()
            self.close()
            raise
        return self

    def keyboard_is_ours(self):
        # Nothing outside a Wayland client can say who has the keyboard, so
        # the client is asked: the library echoes every focus event.
        for e in reversed(self.testbed.since(0, "focus")):
            return e["parts"][1] == "1"
        return False

    def fullscreen_px(self, mon):
        """The mode the compositor gives a fullscreen window on `mon`: the
        monitor's logical size at the window's own scale. The monitor list
        is XWayland's screen, one XWayland scale larger, and the window's
        scale is the fractional one it draws at (180ths here); neither is
        the other."""
        if not self._desk_portal():
            return (mon[3], mon[4])
        st = self.testbed.state() or {"scale": 120}
        s = st["scale"] / 120.0 / self._xwayland_scale()
        return (round(mon[3] * s), round(mon[4] * s))

    def move_window_by(self, dx, dy):
        """By its titlebar, which is the only handle there is.

        In points, the space drag() aims in, so the caller's pixel delta is
        divided by the factor between the monitor list and the pointer. On
        this desk that is the XWayland scale -- the monitor list is
        XWayland's screen, the portal moves in the compositor's points --
        and not the window's own scale, which is the fractional scale it
        draws at: dividing by 180ths here lands a drag thirty-three per
        cent too far, halfway off the far monitor. On a bare machine the
        hands move in the X screen's own pixels, which is the window's
        scale.

        More steps than a short drag gets: the compositor moves the window
        per motion event, and a monitor's width in ten jumps is a drag it
        can lose track of.

        By the title and not the middle: the controls are right-aligned and
        do not shrink, so a window taken in (make_room()) has them in its
        middle, and a press there toggles a checkbox and moves nothing --
        caption_x() says where, at the width the window has."""
        st = self.state() or {"scale": 120}
        g = (self._xwayland_scale() if self._desk_portal()
             else st["scale"] / 120.0)
        x = caption_x(st["area"][0] if "area" in st else None)
        self.drv.drag(x, 17, x + dx / g, 17 + dy / g, steps=24, settle=0.7)

    def capture(self, path):
        from PIL import Image
        self.portal.screenshot(path)
        return Image.open(path).convert("RGB")


class WaylandGL(Wayland):
    """The same Wayland window, drawn with OpenGL through EGL instead of
    Vulkan. The echo and the portal are all a round has of a Wayland window,
    so the window server's half is Wayland's whole; only the tree and the
    renderer differ."""
    NAME = "wayland-gl"
    TREE = "linux"
    RENDERER = "gl"


# ---- Win32 under wine, on the compositor itself ------------------------------

class Win32Wayland(_DeskWine, Wayland):
    """The win64 tree's binary under wine, with wine drawing through its
    Wayland driver and not its X11 one.

    Not another build and not Win32 with a flag: the tree is Win32's and the
    program is the same, but the window is a Wayland surface of wine's, so
    everything an X server answers about the other wine window is gone and
    what is left is what the native Wayland environment has -- the echo for the
    focus and the pointer, the portal for a picture.

    The driver is chosen by the launch alone. With no DISPLAY wine has no X
    server to prefer, and winewayland.drv is what it loads; nothing is
    written to the prefix."""

    NAME = "win64-vulkan-wl"
    # Wine's own Direct3D 12, which this environment loads: vkd3d makes
    # the device and lists the GPUs, and refuses every DXIL pipeline state,
    # so the move fails and comes back (tools/vkd3d_proton.py).
    NO_MOVE_TO = {"d3d12": "wine 10.0's own vkd3d runs no DXIL; the d3d12 "
                           "environments load vkd3d-proton"}
    TREE = "win64"     # _DeskWine's, over Wayland's
    BACKEND = ""       # a Windows tree, though Wayland's environment
    UNSET = ("DISPLAY",)
    # Nothing is declared that has not been seen to hold. "edge-resize":
    # the edge is an inner margin, as on every Win32 window, and the drag
    # is wine's to turn into a resize. No "grab-ring": that margin is the
    # Wayland backend's own surface geometry. No "input-method": on Win32
    # the system composes, and here that is wine. No "desk-layout": 10.0's
    # Wayland driver does not type through the desk's layout at all beyond
    # the letters -- under German the accent key types nothing, dead or
    # not, the sharp s a hyphen and the o-umlaut a u-umlaut -- so there is
    # no dead key to switch to. No "multi-output": 10.0's never says the
    # window changed monitor.
    CAN = frozenset({"composited", "edge-resize", "resizable"})
    # Measured: the border carried past the floor leaves the window 119
    # points wide, under the floor the Win32 backend arms after a resize
    # (win32_apply_size_bounds()), which wine's X11 driver holds at 625.
    FLOOR_UNHELD = ("wine's Wayland driver lets the compositor's resize "
                    "take the window below the floor the Win32 backend "
                    "arms after one")
    # Measured on this desktop, DVI-D-1 left of DP-1: the window opened on
    # wine's DISPLAY1 with the pointer parked on DP-1, and fullscreen
    # covered that. mutter's rule is the pointer's monitor; wine's driver
    # opens it by its own idea of the monitors, which the pointer does not
    # steer.
    PIN_UNHELD = ("wine's Wayland driver opens the window on a monitor of "
                  "its own choosing, not the pointer's")
    # The floor and the opening monitor measured again under 11.0, and as
    # 10.0 had them: 119 points wide, and DVI-D-1 for DP-1. 11.0's driver
    # says the window changed monitor, and with a size that is neither
    # monitor's (TODO.md *Wine 11 on this desktop*), so multi-output is
    # lifted and fails.
    WINE_HELD = {"NO_MOVE_TO": ({}, WINE_10),
                 "FLOOR_UNHELD": ("", wines("10.0", "11.0")),
                 "PIN_UNHELD": ("", wines("10.0", "11.0")),
                 "CAN": (CAN | {"multi-output", "desk-layout"}, WINE_10)}

    # The library's bar and its resize margin, in points (DECO_TITLEBAR_H,
    # DECO_GRAB with room to spare), and how far inside either a point is
    # taken to be in the scene.
    BAR = 34
    EDGE = 8
    UNDER = 24

    def start(self, variant="release", args=(), env=None):
        super().start(variant, args, env)
        self.drv.aim = self._aim
        return self

    def clipboard_missing(self):
        """Asked of the driver this wine loaded, and not of its version: one
        that binds no wl_data_device_manager has no clipboard to cross to
        the desktop, in either direction, and a wine whose driver does is
        then put to the checks with nothing here to change. Not being able
        to tell is not a reason to skip."""
        why = super().clipboard_missing()
        if why:
            return why
        try:
            with open("/proc/%d/maps" % self.pid()) as f:
                drivers = {line.split()[-1] for line in f
                           if line.rstrip().endswith("/winewayland.so")}
            for path in drivers:
                with open(path, "rb") as f:
                    if b"wl_data_device" in f.read():
                        return ""
        except OSError:
            return ""
        if drivers:
            return ("this wine's Wayland driver has no clipboard: it binds "
                    "no wl_data_device_manager")
        return ""

    def _in_scene(self):
        """Have the echo say where the pointer is; False when it will not.

        Silence is the pointer over something that reports no motion, and
        the round leaves it in two such places: on the bar, and on the
        corner a resize was dragged by. A step down gets out of the first
        and a step up and left out of the second, which is cheaper than
        the sweep by forty seconds."""
        drv, g = self.drv, self.drv.gain()
        if drv._observe():
            return True
        step = self.BAR + self.UNDER
        self.portal.rel(0, step * g)
        if drv._observe():
            return True
        self.portal.rel(-step * g, -2 * step * g)
        if drv._observe():
            return True
        drv.at = None
        return False

    def _refresh_free(self):
        self._in_scene()
        return super()._refresh_free()

    def _aim(self, wx, wy):
        """Put the pointer at a point on the bar or on a resize edge, by
        reckoning from one inside them; None for anywhere else, which is
        the echo's.

        Under wine the bar and the edges are non-client area and no motion
        over them is reported -- _XEnvironment._aim() has the same reason -- and
        here there is no server to ask instead. What there is: the portal's
        motion is not accelerated, so a delta asked for is the delta the
        echo reports. A point in the scene the echo has confirmed, and one
        move from it, is therefore a known place and not a guess. The
        controls on the bar are client area and do echo; they are reckoned
        like the rest, since which part of the bar a point is on is the
        library's to know."""
        st = self.state()
        if not st:
            return None
        w, h = st["area"][0], st["area"][1] + self.BAR
        if (wy >= self.BAR and wy < h - self.EDGE
                and self.EDGE <= wx < w - self.EDGE):
            return None
        drv = self.drv
        # Where it is now, first: a window that has just moved or grown
        # under the pointer has left `at` describing somewhere else.
        self._in_scene()
        inset = self.EDGE + self.UNDER
        ax = min(max(wx, inset), w - inset)
        ay = min(max(wy, self.BAR + self.UNDER), h - inset)
        drv.aim = None
        try:
            drv.move_to(ax, ay)
        finally:
            drv.aim = self._aim
        m = self.mark()
        g = drv.gain()
        self.portal.rel((wx - ax) * g, (wy - ay) * g)
        # Nothing may answer, so nothing is waited on but the echo going
        # quiet: over a control the move is reported, elsewhere it is not.
        self.testbed.quiet()
        seen = self.since(m, "motion")
        if seen:
            return (float(seen[-1]["parts"][1]), float(seen[-1]["parts"][2]))
        return (wx, wy)


class Win32WaylandD3D12(_Vkd3dProton, Win32Wayland):
    """The win64 binary under wine on Wayland, told to draw with Direct3D
    12, as Win32D3D12 is Win32's."""
    NAME = "win64-d3d12-wl"
    # size_await()'s bound, run past once a round at the maximize: the frame
    # at the new size comes after 250 ms, its present 28 to 63 ms, on the
    # RX 590 (2026-10-31 and 2026-11-02, release and gc). Wine's Direct3D 12
    # resize on its Wayland driver and not the library's: the same window
    # drawn with Vulkan beside it never says it.
    EXPECTED_LOG: tuple[str, ...] = _Vkd3dProton.EXPECTED_LOG + (SIZE_UNSHOWN,)


# ---- Win32 on Windows, in a virtual machine ----------------------------------

class Win32VM(_Placed, _Portal):
    """The win64 tree's binary on a Windows desktop of its own.

    Not another build: the tree is Win32's, and what is different is whose
    Windows it is. Under wine the window is an X11 one and wine answers for
    the window manager; here it is Windows' own, with a foreground lock, a
    modal size loop and raw input that are the real ones.

    What it cannot speak for is the GPU or the frame rate. The guest has no
    Vulkan device, so Mesa's lavapipe draws the frame on the CPU. See
    vbox_driver.py for what the guest needs."""

    NAME = "win64-vulkan-vm"
    TREE = "win64"
    SUFFIX = ".exe"
    LOCAL = False
    INPUT = "vm"
    # A Debug tree on Direct3D 12 where Graphics Tools are not installed,
    # which a Vulkan round reaches too once the GPU box has moved it.
    EXPECTED_LOG: tuple[str, ...] = (D3D12_NO_LAYER,)
    # "composited": the picture is the machine's display, so it is what
    # Windows put on the screen and not what the client drew.
    #
    # "multi-output": Windows shows a client every monitor the machine has.
    # How many that is belongs to the machine's settings, and a guest with
    # one skips the section for that reason. No "wm-props": those are X11's.
    #
    # "dead-layout": Windows composes, so there is no table to edit as
    # "remap-key" edits X11's; what can be done instead is to switch the
    # window to a layout that has a dead key, which every Windows ships.
    CAN = frozenset({"server-rect", "edge-resize", "unminimize",
                     "read-pixels", "composited", "multi-output",
                     "output-pick", "dead-layout", "resizable"})
    # United States-International, by its registry name: the letters stay
    # where any Latin layout has them, and the key right of the semicolon is
    # a dead acute, a dead diaeresis with Shift. Its accent typed alone is
    # the apostrophe, not U+00B4.
    DEAD_LAYOUT = "00020409"
    DEAD_KEY = "Apostrophe"
    DEAD_ALONE = 0x27
    # The least monitor a round has room on, and where on it the window is
    # put: the default window in its corner, the drag that carries it 180 by
    # 119, the taskbar below and some of the 160 by 120 it then grows by --
    # the rest of that it makes itself, by moving back (room_to_grow()).
    # 1600x1200 holds a round and 1280x960 does not.
    SCREEN = (1600, 1000)
    CORNER = 40
    # How many monitors the machine is made to show before a round: two,
    # so that the move between outputs is run and not skipped. 0 for a
    # machine whose monitors are what is plugged into it.
    MONITORS = 2
    # Which of this desk's environment the program there is started with,
    # by prefix. A program on this desktop inherits all of it, which is how
    # a validation layer is put in front of a round; a machine is handed
    # only what it has the other half of -- a Debug tree's fault sites,
    # which the round copies there with the program.
    FORWARD: tuple[str, ...] = ("FNL_FAULT",)

    @classmethod
    def each(cls):
        # One a machine, and named for it: two Windows are two rounds, and a
        # log or a picture called after the environment alone would be the last
        # machine's. A machine that is off is its own skipped line.
        import vbox_driver as V
        made = []
        for vm in V.VMS:
            b = cls()
            b.machine = vm
            b.NAME = "%s@%s" % (cls.NAME, vm)
            made.append(b)
        return made

    def _open(self):
        """The way in to the machine, opened: a windows_driver.Desk. Raises
        its Unreachable when there is none."""
        import vbox_driver as V
        if getattr(self, "machine", None):
            V.use(self.machine)
        return V.Guest()

    def start(self, variant="release", args=(), env=None):
        exe = os.path.join(ROOT, "build", self.TREE, variant,
                           self.program + self.SUFFIX)
        if not os.path.exists(exe):
            raise Unavailable("no %s -- ./build.sh %s %s"
                              % (os.path.relpath(exe, ROOT), self.TREE,
                                 variant))
        import wayland_driver as W
        from windows_driver import Unreachable
        try:
            self.portal = self._open()
        except Unreachable as e:
            raise Unavailable(str(e))
        try:
            self.portal.kill_stale()
            self.portal.want_screen(*self.SCREEN)
            if self.MONITORS:
                self.portal.want_monitors(self.MONITORS)
            there = self.portal.install(exe)
            for f in self.beside(os.path.dirname(exe)):
                self.portal.install(f)
        except Unreachable as e:
            self.close()
            raise Unavailable(str(e))
        except Exception:
            self.close()            # or the agent is left running there
            raise
        # The process here stands for the program there -- the VBoxManage
        # that runs it, or the relay its output comes back to: its stderr
        # is the program's, and it ends when the program does.
        sent = self.forwarded(self.FORWARD)
        sent.update(self.forced())
        if self.gpu:
            sent["FNL_GPU"] = self.gpu
        self.testbed = Testbed(self.portal.run_argv(there, args,
                                              {**sent, **self.ECHO,
                                               **(env or {})}),
                         log=self.log_path(variant))
        try:
            self.portal.run_begin()
            self._meet(W)
        except Exception:
            # Gone before it had a window, as on the Linux machines
            # (WaylandBare.start()): what it said last is why this machine
            # cannot run it -- one with no Vulkan runtime, say.
            gone = not self.alive()
            said = self.dump(3) if gone else []
            # Nothing is left up for the next round to find: the caller has
            # no environment to stop.
            self.stop()
            self.close()
            if gone:
                raise Unavailable("the program exited at start: %s"
                                  % (said[-1] if said else "silently"))
            raise
        return self

    def beside(self, tree):
        """Files of the tree copied to the machine beside the program before
        it starts: a driver the machine lacks. None, for most."""
        return ()

    # Which of that machine's monitors the round is run on, by the name
    # Windows gives it there (DISPLAY2); unset is wherever the window opens,
    # which is the primary. Its own variable: FNL_DRIVE_OUTPUT names an
    # output of this desktop.
    WANT_OUTPUT = "FNL_DRIVE_WIN_OUTPUT"

    def _to_wanted_monitor(self):
        want = os.environ.get(self.WANT_OUTPUT)
        if not want:
            return
        mons = self.portal.monitors()
        there = next((m for m in mons if m[0] == want), None)
        if not there:
            raise Unavailable("no monitor %r on %s; %s names one of %s"
                              % (want, self.machine, self.WANT_OUTPUT,
                                 ", ".join(m[0] for m in mons)))
        here = self.portal.monitor()[0]
        if here[0] == want:
            return
        # Origin to origin: the corner it is then put in is asked of the
        # monitor it has arrived on.
        self.portal.move_window_by(there[2] - here[1], there[3] - here[2])
        self.rect_still()
        print("  on %s, %dx%d at %d,%d" % (want, there[4], there[5],
                                           there[2], there[3]), flush=True)

    def _meet(self, W):
        """From a process started to a window the pointer is over and the
        echo is answering from."""
        if not self.portal.find(self.program):
            raise RuntimeError("no window from %s on that desktop: %r"
                               % (self.program, self.testbed.dump(3)))
        self.drv = W.Driver(self.portal, self.testbed, gain=self._gain,
                            aim=self._aim, monitors=self.monitors)
        # Windows gives a window started this way no foreground: nobody
        # typed to launch it. Asked for and not asserted, for the reason
        # _XEnvironment.start() gives.
        self.focus()
        # Windows opens each new window a step down and right of the last, so
        # where this one is depends on how many came before it. Put where the
        # drags below have room, as pin_to_target() does for an X window.
        self._to_wanted_monitor()
        x, y, w, h = self.rect()
        wx, wy, ww, wh = self.work_area()
        # In from the corner, unless that would put its far edge off the
        # screen: a window that opened at all the room there is goes in the
        # corner itself, where its border can still be reached.
        cx = self.CORNER if w + self.CORNER <= ww else 0
        cy = self.CORNER if h + self.CORNER <= wh else 0
        self.portal.move_window_by(wx + cx - x, wy + cy - y)
        x, y, w, h = self.rect_still()
        # Over the scene, and seen to be: the window can be asked where it
        # is, so there is nothing to sweep for -- a pointer put on it that
        # the application does not report is a fault to say, not to search
        # round. A few points apart each time, because a pointer already
        # there has not moved and is told to nobody.
        for i in range(8):
            m = self.mark()
            at = (x + w // 2 + 4 * i, y + h // 2)
            self.portal.warp(*at)
            landed = self.portal.settle_pointer(*at)
            ev = self.wait_for("motion", m, timeout=0.6)
            if ev:
                self.drv.at = (float(ev["parts"][1]), float(ev["parts"][2]))
                return
        shot = os.path.join(OUT, "%s-unmet.png" % self.NAME)
        try:
            self.portal.screenshot(shot)
        except Exception:
            shot = "no picture"
        raise RuntimeError(
            "the window at %s reports no pointer over it (pointer %s, "
            "arrived %s, foreground %s, alive %s; %s)"
            % ((x, y, w, h), self.portal.pointer_pos(), landed,
               self.portal.foreground(), self.alive(), shot))

    def stop(self, sig=15):
        # A program that has already gone is left to be reported: the
        # VBoxManage here hears of it late, and signalled first it would
        # answer with the signal and not with the program's exit code.
        if self.testbed.alive() and not self.alive():
            try:
                self.testbed.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                pass
        rc = self.testbed.stop(sig)
        # Ending the VBoxManage ends the guest session, and the program with
        # it -- but that is VirtualBox's doing and not asked of it here, so
        # the guest is told as well.
        if getattr(self, "portal", None):
            try:
                self.portal.kill_stale()
            except Exception:
                pass
        return rc

    def alive(self):
        # Asked of the guest as well: the VBoxManage here outlives the
        # program there by a second or more, and a key that closed the
        # window would be waited for and then sent again.
        if not self.testbed.alive():
            return False
        portal = getattr(self, "portal", None)
        if not portal or not getattr(self, "drv", None):
            return True
        try:
            return portal.running()
        except Exception:
            return False

    def cpu_seconds(self):
        # The process on this machine is VBoxManage, which does no work.
        return self.portal.cpu_seconds()

    def _gain(self):
        # Windows hands out physical pixels. See _XEnvironment._gain().
        st = self.testbed.state() or {"scale": 120}
        return st["scale"] / 120.0

    def _aim(self, wx, wy, tries=3):
        """Put the pointer at a window point, or return None to leave it to
        the echo.

        One absolute report, with nothing between the request and the result
        but a queue: the guest is asked where the pointer went only to know
        *when* it got there. A point off the screen never arrives, and that
        is the echo's to deal with."""
        try:
            x, y, _w, _h = self.rect()
        except Exception:
            return None
        g = self._gain()
        tx, ty = int(round(x + wx * g)), int(round(y + wy * g))
        for _ in range(tries):
            self.portal.warp(tx, ty)
            if self.portal.settle_pointer(tx, ty):
                return (wx, wy)
        return None

    def move_by(self, dx, dy):
        # The emulated mouse's relative report, and not _Portal's rel():
        # a captured pointer is read from raw input, which counts device
        # motion and is told nothing by an absolute report.
        g = self._gain()
        self.portal.nudge(dx * g, dy * g)
        self.drv.at = None

    def focus(self):
        self.portal.focus()

    def has_focus(self):
        return self.portal.foreground()

    def keyboard_is_ours(self):
        # A scancode goes to Windows' foreground window, whatever is focused
        # on this desktop.
        try:
            return self.portal.foreground()
        except Exception:
            return None

    def move_window_by(self, dx, dy):
        self.portal.move_window_by(dx, dy)
        self.rect_still()
        # The pointer follows, for _XEnvironment.move_window_by()'s reason.
        self.portal.rel(dx, dy)
        self.drv.at = None

    def rect(self):
        return self.portal.rect()

    def monitor(self):
        return self.portal.monitor()[0][1:]

    def work_area(self):
        return self.portal.monitor()[1]

    def target_monitor(self):
        # The guest's, which nothing on this desktop's xrandr describes.
        return self.portal.monitor()[0]

    def monitors(self):
        return self.portal.monitors()

    def pointer_pos(self):
        return self.portal.pointer_pos()

    def minimized(self):
        return self.portal.minimized()

    def unminimize(self):
        """Restore it and give it the foreground; whether it came back with
        the keyboard."""
        end = time.time() + 6.0
        ok = self.portal.focus()
        while (not ok or self.minimized()) and time.time() < end:
            time.sleep(3 * POLL)
            ok = self.portal.focus()
        return ok and not self.minimized()

    def dead_layout(self):
        """Switch the window to DEAD_LAYOUT; returns what undoes it.

        Whether the window followed is not answered here: a key that is
        not dead afterwards is the round's finding, not this call's."""
        was = self.portal.layout()
        hkl, had = self.portal.layout_load(self.DEAD_LAYOUT)
        self.portal.layout_switch(hkl)

        def undo():
            self.portal.layout_switch(was)
            # Only once the window has left it, and only if it was loaded
            # for this: a layout the user has is not this round's to drop.
            if not had and hkl != was:
                self.portal.layout_drop(hkl)
        return undo

    def capture(self, path):
        from PIL import Image
        self.portal.screenshot(path)
        return Image.open(path).convert("RGB")

    def content_black_pct(self, top=40):
        """_XEnvironment.content_black_pct(), read off the machine's display:
        the same rows of the same rectangle, cut out of a picture of the
        whole screen."""
        im = self.capture(os.path.join(OUT, "%s-pixels.png" % self.NAME))
        x, y, ww, hh = self.rect()
        ox, oy = self.portal.shot_origin()
        x, y = x + ox, y + oy
        if ww < 8 or hh < top + 8:
            return 0.0
        black = tot = 0
        for yy in [top + (hh - top) * k // 6 for k in range(1, 6)]:
            for xx in range(0, ww, 7):
                if not (0 <= x + xx < im.width and 0 <= y + yy < im.height):
                    continue
                tot += 1
                if max(im.getpixel((x + xx, y + yy))) < 12:
                    black += 1
        # None, off every edge of the picture: _XEnvironment's reason.
        return 100.0 * black / tot if tot else None

    def scene_row(self, top=40):
        """_XEnvironment.scene_row(), cut out of a picture of the whole
        screen."""
        im = self.capture(os.path.join(OUT, "%s-row.png" % self.NAME))
        x, y, ww, hh = self.rect()
        ox, oy = self.portal.shot_origin()
        x, y = x + ox, y + oy
        yy = y + top + (hh - top) // 2
        if ww < 8 or hh < top + 8 or not 0 <= yy < im.height:
            return None
        return bytes(c for xx in range(max(x, 0), min(x + ww, im.width))
                     for c in im.getpixel((xx, yy)))

    def clipboard_missing(self):
        return ""

    def clipboard_get(self):
        return self.portal.clipboard_get()

    def clipboard_put(self, data):
        self.portal.clipboard_put(data)

    def clipboard_done(self):
        pass


# ---- Win32 on Windows, on a laptop -------------------------------------------

class Win32Native(Win32VM):
    """The win64 tree's binary on a Windows laptop reached over the network.

    A Windows desktop like the guest's, asked the same questions by the same
    agent, so everything Win32VM does with an answer is inherited. What is
    different is the way in (native_driver.py): there is no machine to
    borrow devices from, so the agent sends the input and starts the
    program, and ssh carries both.

    And what it may be believed about. The guest is the one for Windows'
    input stack below SendInput; this is the one with a GPU, a panel and a
    compositor presenting to it."""

    NAME = "win64-vulkan-native"
    INPUT = "agent"
    # A laptop's monitors are its panel and whatever is plugged in.
    MONITORS = 0
    # Win32VM's, word for word: each is a fact about Windows and the agent,
    # and both are the same here. One that turns out not to hold on real
    # hardware is taken out of this set, not worked round in a check.
    CAN = Win32VM.CAN
    # size_await()'s bound, which NVIDIA's driver reaches on a resize
    # (DESIGN *Measured on Windows itself*); and the debug layer missing,
    # which a Vulkan round says too once the GPU box has moved it onto
    # Direct3D 12.
    EXPECTED_LOG: tuple[str, ...] = (SIZE_UNSHOWN, D3D12_NO_LAYER)
    # The Vulkan loader's and the layers' variables: the laptop has a
    # driver and may have the SDK, so the environment that validates a
    # round on this desktop validates one there. And the library's fault
    # sites, which a Debug tree's round forces there as here.
    FORWARD: tuple[str, ...] = ("VK_", "FNL_GPU", "FNL_FAULT", "FNL_VK_",
                                "FNL_D3D12_DXGI11")

    def input_hold(self):
        """The laptop's own input, held by its agent's hooks
        (windows_agent.ps1), which let what the agent sends through."""
        import input_hold
        if not self.machine:
            return input_hold.NoHold(self.NAME)
        return input_hold.AgentHold(self.machine, self)

    @classmethod
    def each(cls):
        # One a machine and a device, named for both: a laptop's two GPUs
        # are two rounds, and the one nobody remembered to ask for is the
        # one that goes unrun. FNL_GPU in the environment is one device for
        # this run, whatever is written down. With no machine listed there
        # is still one round, so that it says why it was skipped and not
        # nothing at all.
        import native_driver as N
        asked = os.environ.get("FNL_GPU")
        made = []
        for host in N.hosts():
            for gpu in (asked,) if asked else N.devices(host) or ("",):
                b = cls()
                b.machine = host
                b.gpu = gpu
                b.NAME = "%s@%s%s" % (cls.NAME, host,
                                      ":" + gpu if gpu else "")
                made.append(b)
        return made or [cls()]

    def _open(self):
        import native_driver as N
        if not getattr(self, "machine", None):
            raise N.Unreachable(
                "no machine is named -- native_driver.py --setup <name> "
                "writes one down, and FNL_DRIVE_NATIVE names one for a run")
        portal = N.Native(self.machine)
        # Said at the top of the round: every figure from a machine is read
        # against its drivers, its scale and what it is running on.
        for line in portal.facts():
            print("  " + line, flush=True)
        return portal

    VALIDATION = "VkLayer_khronos_validation.dll"

    def start(self, variant="release", args=(), env=None):
        super().start(variant, args, env)
        # A layer the environment asks for and the loader cannot load is a
        # round that validated nothing and says "log is clean". So it is
        # looked for in the program, and its absence stops the round.
        # Vulkan's layer: a Direct3D 12 program never loads it.
        if validation_asked() and self.RENDERER == "vulkan":
            if not self.portal.has_module(self.VALIDATION):
                self.stop()
                self.close()
                raise Unavailable(
                    "the validation layer was asked for and %s is not in "
                    "the program on %s -- the Vulkan SDK, and the Visual "
                    "C++ runtime it needs" % (self.VALIDATION, self.machine))
            print("  validation layer: loaded", flush=True)
        # Two devices, and which one a figure is from decides how it is
        # read.
        self.device_check(self.gpu)
        return self


class Win32VMD3D12(Win32VM):
    """The win64 binary in the Windows guests, told to draw with Direct3D
    12, on WARP: they have no GPU, and Direct3D 12's software adapter is
    part of Windows, so nothing goes in beside the program."""
    NAME = "win64-d3d12-vm"
    RENDERER = "d3d12"


class Win32NativeD3D12(Win32Native):
    """The win64 binary on the Windows laptops, told to draw with Direct3D
    12. FNL_GPU steers it, by a part of the adapter's name, as it steers
    Vulkan."""
    NAME = "win64-d3d12-native"
    RENDERER = "d3d12"


# ---- Cocoa, on a Mac ---------------------------------------------------------

class CocoaNative(Win32VM):
    """The macos tree's binary, drawing with Metal, on a Mac reached over
    ssh: schenker,
    booted from its macOS stick, the HD 630 drawing.

    A desktop asked and driven as the Windows laptop's is, by an agent on it
    (mac_driver.py, mac_agent.m) that answers windows_driver.Desk's
    questions, so everything Win32VM does with an answer is inherited. The
    way in is ssh alone: a program started from an ssh session reaches the
    window server, so the agent and the program are each the last command of
    one, and the program's stderr is the echo.

    The window server speaks points, the library's own unit at the backing
    scale, and the agent's input is posted events, which macOS delivers as
    a hand's -- above the HID drivers, as SendInput is above Windows'."""

    NAME = "macos-metal-native"
    TREE = "macos"
    SUFFIX = ""
    RENDERER = "metal"
    INPUT = "agent"
    # One for each machine's own monitors.
    MONITORS = 0
    # Win32VM's: "dead-layout" too, the agent answering the layout
    # questions with macOS's input sources, a layout named by its source ID.
    CAN = frozenset({"server-rect", "edge-resize", "unminimize",
                     "read-pixels", "composited", "multi-output",
                     "dead-layout", "resizable"})
    # U.S. International - PC, Windows' United States-International on a
    # Mac: the same key a dead acute, a dead diaeresis with Shift, and the
    # apostrophe when typed alone.
    DEAD_LAYOUT = "com.apple.keylayout.USInternational-PC"
    # Metal's own diagnostics and the device's name go along; FNL_GPU steers
    # the program as on every other environment; the library's fault
    # sites, which a Debug tree's round forces there as here; and the
    # loader's and the layer's, with the library's FNL_VK_VALIDATION, for
    # a checker round's loader (beside()) -- every renderer's round, since
    # each lists Vulkan's GPUs and its GPU box walks onto them.
    FORWARD: tuple[str, ...] = ("MTL_", "FNL_GPU", "FNL_FAULT", "VK_",
                                "FNL_VK_")
    # A checker round puts Khronos' loader and validation layer beside the
    # program (tools/vulkan_macos.py), which the library opens before
    # MoltenVK, and names MoltenVK's manifest and the layer's directory to
    # it, relative to the program's, which is where the program runs. A
    # release round has none, as a tree has none: a loader an earlier round
    # left there is taken away.
    LOADER_ENV = {"VK_DRIVER_FILES": "MoltenVK_icd.json",
                  "VK_ADD_LAYER_PATH": "."}
    LOADER_FILES = ("libvulkan.1.dylib",
                    "libVkLayer_khronos_validation.dylib",
                    "VkLayer_khronos_validation.json", "MoltenVK_icd.json",
                    "Vulkan-LICENSE.txt")
    VALIDATION = "VkLayer_khronos_validation"
    # Set by start() for a checker round with no loader built here: the
    # round then has Metal's API validation alone, and the layer check is
    # skipped with this reason (stress.py).
    NO_LAYER = ""
    fronted = False
    NO_LOADER = ("no Vulkan loader for the Mac is built here, to put the "
                 "layer in front of MoltenVK -- python3 "
                 "tools/vulkan_macos.py --build; Metal's API validation "
                 "stands in")
    # Metal's API validation, for a Debug tree's round: an error said on
    # stderr (nslog), where its default stops the program with an
    # assertion and the round learns only that it crashed; and the line
    # Metal says when it is on, which is the proof the round asks for.
    METAL_VALIDATION = {"MTL_DEBUG_LAYER": "1",
                        "MTL_DEBUG_LAYER_ERROR_MODE": "nslog"}
    METAL_VALIDATION_ON = "Metal API Validation Enabled"
    # MoltenVK's log said only for an error, in the Metal round too, which
    # loads it to list Vulkan's GPUs: at its default it lists every
    # extension and device feature, a hundred and fifty lines a log check
    # would take as complaints.
    ECHO = dict(Win32VM.ECHO, MVK_CONFIG_LOG_LEVEL="1")
    WANT_OUTPUT = "FNL_DRIVE_MAC_OUTPUT"
    # AppKit's fullscreen animation: the library says the new state at its
    # will, and the did comes 560 to 670 ms later on schenker; a key pressed
    # between the two is swallowed by the window server -- the press, the
    # release arriving after -- which lost one F11 of eight a round, one
    # round in two (2026-10-31).
    WINDOW_ANIMATION = 0.8
    # AppKit's own line when the input source changes under the window,
    # which "dead-layout" does, on stderr through NSLog: macOS's, not the
    # library's.
    EXPECTED_LOG: tuple[str, ...] = (
        r"TSM AdjustCapsLockLEDForKeyTransitionHandling",
        # And Metal's, through NSLog too, when its API validation is on.
        r"Metal API Validation Enabled$")

    def input_hold(self):
        """The Mac's own input, held by its agent's event tap
        (mac_agent.m), which lets what the agent posts through."""
        import input_hold
        if not self.machine:
            return input_hold.NoHold(self.NAME)
        return input_hold.AgentHold(self.machine, self)

    @classmethod
    def each(cls):
        # FNL_GPU is forwarded to the program there, and named here as
        # Win32Native.each() names it, so that start()'s device_check()
        # holds the program to it: a name the Mac has not got falls back
        # with a warning, which would be a round on the wrong device.
        import mac_driver as M
        asked = os.environ.get("FNL_GPU", "")
        made = []
        for host in M.hosts():
            b = cls()
            b.machine = host
            b.gpu = asked
            b.NAME = "%s@%s%s" % (cls.NAME, host,
                                  ":" + asked if asked else "")
            made.append(b)
        return made or [cls()]

    def _open(self):
        import mac_driver as M
        from windows_driver import Unreachable
        if not getattr(self, "machine", None):
            raise Unreachable(
                "no Mac is named -- mac_driver.py --setup <name> writes one "
                "down, and FNL_DRIVE_MAC names one for a run")
        portal = M.Mac(self.machine)
        for line in portal.facts():
            print("  " + line, flush=True)
        return portal

    @staticmethod
    def _vulkan_macos():
        # tools/vulkan_macos.py, a directory above this one and not on the
        # path.
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "vulkan_macos", os.path.join(ROOT, "tools", "vulkan_macos.py"))
        V = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(V)
        return V

    def beside(self, tree):
        """The loader, the layer and MoltenVK for a checker round
        (LOADER_ENV); for any other, none, and an earlier round's taken
        away there."""
        if not self.fronted:
            self.portal.remove(self.LOADER_FILES)
            return []
        molten = os.path.join(tree, "libMoltenVK.dylib")
        if not os.path.isfile(molten):
            raise Unavailable("no MoltenVK beside the programs of %s -- "
                              "./build.sh puts it there"
                              % os.path.relpath(tree, ROOT))
        return self._vulkan_macos().files() + [molten]

    def start(self, variant="release", args=(), env=None):
        checking = variant != "release"
        self.fronted = checking and self._vulkan_macos().ready()
        self.NO_LAYER = "" if self.fronted or not checking else self.NO_LOADER
        if checking:
            env = dict(self.METAL_VALIDATION, **(env or {}))
        if self.fronted:
            env = dict(self.LOADER_ENV, **env)
        elif checking:
            # MoltenVK opened directly has no layer to give, and an
            # instance that asks for one is refused and says so.
            env = dict(env, FNL_VK_VALIDATION="0")
        super().start(variant, args, env)
        self.device_check(self.gpu)
        # A layer asked for and not in the program is a round that
        # validated nothing and says "log is clean". Vulkan's round alone:
        # the others make their instance only to list its GPUs.
        if self.fronted and validation_asked() and self.RENDERER == "vulkan":
            if not self.until(lambda: self.portal.has_module(
                    self.VALIDATION), timeout=10.0):
                self.stop()
                self.close()
                raise Unavailable(
                    "the validation layer was asked for and %s is not in "
                    "the program on %s" % (self.VALIDATION, self.machine))
            print("  validation layer: loaded", flush=True)
        # Asked and not on is a round that validated nothing and says "log
        # is clean", as a Vulkan layer the loader did not load would be.
        if checking:
            on = self.until(lambda: any(
                self.METAL_VALIDATION_ON in ln
                for ln in self.dump(1 << 30)), timeout=10.0)
            if not on:
                self.stop()
                self.close()
                raise Unavailable("Metal's API validation was asked for "
                                  "(MTL_DEBUG_LAYER) and Metal never said "
                                  "it was on")
            print("  Metal API validation: on", flush=True)
        return self

    def _gain(self):
        # The window server's points are the library's at the backing
        # scale, and the library's own scale may differ from that only by
        # an override.
        st = self.testbed.state() or {"scale": 120}
        return st["scale"] / 120.0 / (self.portal.dpi / 96.0)

    def on_output(self, out, name, home):
        # The agent names a screen as AppKit does, and so does the library.
        return out == name or out.startswith(name)


class CocoaNativeVulkan(CocoaNative):
    """The same program on the same Mac, told the Vulkan renderer: MoltenVK,
    which goes in beside the program as build.sh put it in the tree."""
    NAME = "macos-vulkan-native"
    TREE = "macos"
    RENDERER = "vulkan"
    # MoltenVK's own (CocoaNative.ECHO quiets its log), beside the Mac's.
    FORWARD: tuple[str, ...] = CocoaNative.FORWARD + ("MVK_",)

    def beside(self, tree):
        have = os.path.join(tree, "libMoltenVK.dylib")
        if not os.path.isfile(have):
            raise Unavailable("no MoltenVK beside the programs of %s -- "
                              "./build.sh puts it there"
                              % os.path.relpath(tree, ROOT))
        return sorted(set(super().beside(tree)) | {have})


class CocoaNativeGL(CocoaNative):
    """The same program on the same Mac, told OpenGL: a CGL context, 4.1
    core, the frame copied into the window's Metal layer (src/renderer/gl/
    cgl.m). No validation layer to ask for, and no debug output on 4.1:
    the log check reads the program's own lines alone."""
    NAME = "macos-gl-native"
    TREE = "macos"
    RENDERER = "gl"


# ---- X11 with nobody managing it ---------------------------------------------

class _OnBare:
    """The half of a round on the bare machine that is the same whichever
    server it is on: the machine held open, the program copied there and
    run as the remote shell's last command, its pid there, its processor
    time asked of that machine. bare_driver.py has the machine itself.

    `server` is what the subclass asks the machine for once the program is
    installed, and it returns the hands the round drives with."""

    LOCAL = False
    # The Vulkan loader's and the layers', as a laptop is handed them: a
    # validation layer in front of a round here is one in front there. And
    # Mesa's, since that machine's driver is Mesa's and steered by them --
    # lavapipe's own among them (LP_NUM_THREADS), the one lever on that
    # machine's heat. And FNL_GPU, which puts the program on one device of
    # a machine with two, checked at start as on a laptop. And NVIDIA's
    # (__NV_PRIME_RENDER_OFFLOAD and the rest), for a machine booted on
    # NVIDIA's driver, which is steered by them as Mesa's is by its own.
    # And Mesa's GL ones, since FNL_GPU does not steer OpenGL: DRI_PRIME picks
    # the device, and LIBGL_ALWAYS_SOFTWARE puts llvmpipe where the GPU has
    # no 4.5 -- which a GL round sets itself where it must (_SoftGL). And
    # the library's fault sites, which a Debug tree's round forces there as
    # here.
    FORWARD = ("VK_", "MESA_", "LP_", "FNL_GPU", "FNL_VK_", "__NV_",
               "__GLX_", "LIBGL_", "DRI_PRIME", "FNL_FAULT", "FNL_X11_")
    VALIDATION = "VkLayer_khronos_validation"
    # A ranked round opened on OpenGL: the layer asked for is proven at the
    # walk's first Vulkan entry instead of at the start (_validated()).
    layer_owed = False
    # Whether the X server's socket is forwarded here.
    FORWARD_X = False

    def machine_forwarded(self):
        """forwarded(FORWARD) over what the machine's line in bare-hosts
        sets (bare_driver.machine_env()): this desk's variable wins, so a
        round can still ask for another value."""
        import bare_driver as D
        return {**D.machine_env(self.machine), **self.forwarded(self.FORWARD)}

    def input_hold(self):
        """The machine's own keyboards, mice and touchpads, as root there;
        the uinput devices the round types and points with are virtual and
        not held."""
        import bare_driver as D
        import input_hold
        if not self.machine:
            return input_hold.NoHold(self.NAME)
        # By the machine's name, as Bare() reads it: the hold begins before
        # the round's own connection is made.
        dest = D.listed().get(self.machine, self.machine)
        return input_hold.SshHold(self.machine, [
            "ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
            D.as_root(dest)])

    # The kernel driver the machine's NVIDIA GPU is moved to before the
    # program starts (bare_driver.drivers()), "" for whatever is bound; and
    # whether this environment takes one at all. KMS's alone: under a
    # server the GPU may be the server's, and the boot entry is the way.
    driver = ""
    DRIVERS = False

    @classmethod
    def each(cls):
        # One a machine and a kernel driver, named for both, as a laptop's
        # GPUs are: `x11-vulkan-bare@schenker:nouveau`.
        import bare_driver as D
        made = []
        for name in D.hosts():
            for driver in (D.drivers(name) if cls.DRIVERS else ()) or ("",):
                b = cls()
                b.machine = name
                b.driver = driver
                b.NAME = "%s@%s%s" % (cls.NAME, name,
                                      ":" + driver if driver else "")
                # Drawn on the GPU the driver was moved for, where FNL_GPU
                # steers the renderer: on its own the library picks the
                # integrated GPU over NVK on a GPU before Turing.
                if driver and cls.RENDERER != "gl" and \
                        not os.environ.get("FNL_GPU"):
                    b.gpu = "nvidia"
                made.append(b)
        return made or [cls()]

    def driver_bind(self):
        """The machine's NVIDIA GPU on this round's kernel driver, before
        anything is installed or started there: what the install asks of
        the machine (NVIDIA's ICD kept loaded under ASan) is the driver's.
        Every server stopped first, as the KMS round has them anyway: Xorg
        holds NVIDIA's GPU open through its X driver. NVIDIA's userspace
        is then in place for the round (Bare.gpu_driver()), but under
        ThreadSanitizer, which draws on no row of NVIDIA's own driver: there
        libglvnd still loaded NVIDIA's EGL vendor to list OpenGL's GPUs, the
        GPU on nouveau, and ThreadSanitizer spent 80 minutes in one
        reallocation loop of libnvidia-glcore's. Any other round has the GPU
        and the userspace as the boot does, before its server starts
        (Bare.as_booted())."""
        self.box.moves_driver = self.driver or ""
        if self.driver:
            self.box.bare_display()
            tsan = getattr(self, "variant", "release") == "tsan"
            self.box.gpu_driver(self.driver, userspace=not tsan)
            if tsan:
                self.box.nvidia_userspace_aside()
        else:
            self.box.as_booted()

    # Whether this round opens where the machine's GPU ranking says
    # (gpu_sent()), and names no renderer or GPU.
    ranked = False
    # Whether this environment is the one that runs a round on a kernel
    # driver where the machine keeps a ranking: such a round names no
    # renderer, so another environment of the same tree would run the
    # same round again under another renderer's name (ranked_twin()).
    RANKED_ROUND = True

    def ranked_twin(self, variant):
        """Why this round is not run, or None: a round on a kernel driver,
        on a machine that keeps a ranking, opens where the ranking says and
        walks every renderer on every driver (stress.gpu_every_pair()),
        whichever renderer its environment is named for -- so one
        environment runs it, and the others are skipped as its twin.
        Not under ThreadSanitizer, which keeps no ranking (gpu_sent())."""
        if not self.driver or self.RANKED_ROUND or variant == "tsan" or \
                not self.ranking_kept():
            return None
        return ("%s keeps a GPU ranking, so the round on %s opens on its "
                "first entry and walks every renderer: kms-vulkan-bare@%s:%s "
                "is that round" % (self.machine, self.driver, self.machine,
                                   self.driver))

    def ranking_kept(self):
        """Whether the machine keeps a GPU ranking for the program on KMS
        (src/core/rank.c), in the account's cache."""
        out = self.box.sh("test -s ~/.cache/fenestal/gpu-rank-%s-kms.txt "
                          "&& echo kept" % self.program, check=False)
        return out.strip() == "kept"

    def gpu_sent(self, sent, ranked=True):
        # A round on a kernel driver, on a machine that keeps a ranking,
        # opens where the ranking says, as a start that names nothing does:
        # no renderer and no GPU named, the GPU moved to the first entry's
        # driver first where that is the other. The round's renderer is
        # then the one it opened with (stress.renderer_check()), and the
        # round's driver only where the GPU was before the program. Not
        # under ThreadSanitizer, which NVIDIA's own driver crashes, and not
        # where the caller measures what it named (`ranked` False:
        # bench.py), which the ranking would put elsewhere.
        self.ranked = ranked and bool(self.driver) and \
            getattr(self, "variant", "release") != "tsan" and \
            self.ranking_kept()
        if self.ranked:
            sent.pop("FNL_RENDERER", None)
            sent.pop("FNL_GPU", None)
            self.gpu = ""
        if self.gpu and "FNL_GPU" not in sent:
            sent["FNL_GPU"] = self.gpu
        # A round on a kernel driver offers the move to the other one,
        # which the round's hands take from the GPU box
        # (stress.gpu_driver_moves()); "switch" binds nothing itself, the
        # round having bound this driver already.
        if self.driver and "FNL_GPU_DRIVER" not in sent:
            sent["FNL_GPU_DRIVER"] = "switch"
        # And walks that list ordered by the machine's ranking, where one
        # is kept, as it is used (stress.gpu_every_pair()): opened on its
        # first entry and never measured again in a round ("keep"), or
        # under ThreadSanitizer ordered alone ("order"), the window where
        # FNL_GPU puts it.
        if self.driver:
            sent["FNL_GPU_RANK"] = "keep" if self.ranked else "order"

    # The Vulkan driver a move onto each kernel driver lands on, by its ICD
    # file's name. A machine with NVIDIA's module and none of its userspace
    # has no Vulkan GPU for a move to nvidia to land on; the machine, not
    # the library, is what is missing there. The keys' nouveau boot is not
    # that machine: a round on a driver puts the files back.
    ICDS = {"nvidia": "nvidia_icd", "nouveau": "nouveau_icd"}

    # Whether the device a program said is the NVIDIA GPU on each kernel
    # driver: NVK names itself, NVIDIA's own driver does not. FNL_GPU
    # steers Vulkan onto that GPU and nothing steers OpenGL, which picks
    # for itself and draws on the integrated GPU where the driver's
    # userspace is missing -- a round on a driver about another GPU. Not
    # asked where FNL_GPU, set for the run, names the device itself.
    ON_DRIVER = {"nvidia": lambda n: "NVIDIA" in n and "(NVK " not in n,
                 "nouveau": lambda n: "(NVK " in n}

    def no_vulkan_on(self, driver):
        """Why no Vulkan driver here would list the GPU once it is on
        kernel driver `driver`, or None when one would. A listing that
        cannot be had is None: the move is tried and its checks say."""
        prefix = self.ICDS.get(driver)
        if not prefix:
            return None
        icds = self.box.sh("ls /usr/share/vulkan/icd.d /etc/vulkan/icd.d "
                           "2>/dev/null", check=False).split()
        if not icds or any(f.startswith(prefix) for f in icds):
            return None
        return ("no %s*.json among %s's Vulkan drivers, so none would list "
                "the GPU on %s" % (prefix, self.machine, driver))

    def bring_up(self):
        """The server the program runs on, up on self.box, and nothing to
        drive it with: what pace.py's column needs of a round's start."""
        raise NotImplementedError

    def server(self):
        raise NotImplementedError

    def program_env(self):
        """What the program is started with beside the round's own
        environment, for this place."""
        return {}

    def install_dir(self, tree):
        """The directory the program goes to there, beside the variant
        (Bare.install()): its tree's -- `tree`, the one the machine's
        processor takes (Bare.tree()) -- which every environment of the
        tree shares, since what is copied is the same bytes for each."""
        return tree

    def bare_exe(self, variant):
        """The program this environment sends to self.box, from the tree
        its processor takes; raises Unavailable when that is not built."""
        import bare_driver as D
        try:
            tree = self.box.tree(self.TREE)
        except D.Unreachable as e:
            raise Unavailable(str(e))
        exe = os.path.join(ROOT, "build", tree, variant,
                           self.program + self.SUFFIX)
        if not os.path.exists(exe):
            raise Unavailable("no %s -- ./build.sh %s %s"
                              % (os.path.relpath(exe, ROOT), tree, variant))
        return exe, tree

    def device_found(self, said):
        """Told the device the program said it is on, (name, type) or None,
        once per start: what an environment allows for on that device."""

    def _bare_start(self, variant, args, env):
        """The machine, the program on it, and the program started: what
        every round there does before it looks for its window."""
        import bare_driver as D
        if not getattr(self, "machine", None):
            raise Unavailable(
                "no machine is named -- bare_driver.py --setup <name> "
                "<user@address> writes one down, and FNL_DRIVE_BARE names "
                "one for a run")
        self.box = self.remote_pid = None
        try:
            self.box = D.Bare(self.machine, x=self.FORWARD_X)
            # The tree the machine's processor takes, before anything is
            # done there: an ARM machine has no wine round.
            exe, tree = self.bare_exe(variant)
            busy = self.box.busy()
            if busy:
                raise D.Unreachable(busy)
            same = self.ranked_twin(variant)
            if same:
                raise D.Unreachable(same)
            self.box.kill_stale()
            self.driver_bind()
            for line in self.box.facts():
                print("  " + line, flush=True)
            there = self.box.install(self.stage(exe, variant), variant,
                                     self.install_dir(tree))
            self.portal = self.server()
        except D.Unreachable as e:
            self.close()
            raise Unavailable(str(e))
        except Exception:
            self.close()
            raise
        self.slow = variant in ("asan", "tsan")
        sent = self.machine_forwarded()
        sent.update(self.program_env())
        sent.update(self.forced())
        self.gpu_sent(sent)
        # What gpu_sent() decided over the echo's defaults: a round on a
        # kernel driver orders its list by the ranking.
        self.testbed = Testbed(self.box.run_argv(there, args,
                                           {**self.ECHO, **sent,
                                            **(env or {})}),
                         log=self.log_path(variant))
        try:
            self.remote_pid = self.box.pid_of(self.testbed)
        except Exception as e:
            self.stop()
            self.close()
            if isinstance(e, D.Unreachable):
                raise Unavailable(str(e))
            raise
        # A discrete GPU left to power itself off when idle answers the
        # first input after a pause in about 2 s (1.86 on schenker's NVK),
        # past every wait a round has at release; waited for as a checker
        # tree is. Not held awake for the round: its suspend is what the
        # library has to come through (DESIGN *The startup inventory*).
        said = self.device_check((env or {}).get("FNL_GPU",
                                                 sent.get("FNL_GPU")))
        on = self.ON_DRIVER.get(getattr(self, "driver", None) or "")
        if self.ranked:
            on = None           # where the ranking put it, checked there
        if on and not os.environ.get("FNL_GPU") and not said:
            # OpenGL names its device after Vulkan's inventory and its own:
            # 46 s and more from the start under ASan on aspire, past the
            # wait above.
            said = self.device_said(60.0 * self.patience())
            if said:
                print("  %s%s" % (IN_USE, said[0]), flush=True)
        if on and not os.environ.get("FNL_GPU") and \
                not (said and on(said[0])):
            self.stop()
            self.close()
            raise Unavailable("the round is on the NVIDIA GPU on %s and the "
                              "program on %s is on %s"
                              % (self.driver, self.machine,
                                 said[0] if said else
                                 "a device it did not name"))
        self.device_found(said)
        # NVIDIA's own driver starts its threads through a pthread_create
        # it looks up with dlsym, past ThreadSanitizer's interceptor, and the
        # first allocation on one is a SEGV inside TSan (580.178.04). Not
        # NVK's, which is Mesa's and named so; another device of the same
        # machine runs under TSan with this driver loaded.
        if variant == "tsan" and said and "NVIDIA" in said[0] and \
                "(NVK " not in said[0]:
            self.stop()
            self.close()
            raise Unavailable("ThreadSanitizer cannot follow NVIDIA's own "
                              "driver: it starts its threads past TSan's "
                              "pthread_create; FNL_GPU names another device")
        if said and said[1] == "discrete" and not self.slow and \
                self.box.discrete_sleeps():
            self.slow = True
            print("  the GPU powers off when idle: waits as for a checker",
                  flush=True)

    def known_layout(self):
        """The machine's console layout, (XKBLAYOUT, XKBVARIANT), off
        /etc/default/keyboard: what weston.ini is given, what Xorg's XTEST
        keyboard is set to and what the KMS backend reads for itself from
        xkeyboard-config's files. "known-layout" is this."""
        kb = self.box.console_keyboard()
        return (kb.get("XKBLAYOUT", ""), kb.get("XKBVARIANT", ""))

    def _validated(self):
        """A layer the environment asks for is a layer in the program, or
        the round stops: Win32Native.start()'s reason, asked of /proc
        there since stress.py's layer_check reads this machine's. Vulkan's
        layer only: a GL program never loads it.

        A ranked round draws with what the ranking put first, which its
        environment does not name: opened on OpenGL, the layer is owed
        (layer_owed) and proven at the first Vulkan entry the GPU walk
        lands on (stress.gpu_every_pair())."""
        if not validation_asked():
            return
        renderer = self.RENDERER
        if self.ranked:
            said = [ln for ln in self.dump(1 << 30)
                    if ln.startswith("renderer: ")]
            # "renderer: Vulkan, named by ..." or "renderer: Vulkan; passed
            # over ...": the name is what comes before either.
            drew = (said[0][len("renderer: "):].split(";")[0]
                    .split(",")[0].strip() if said else "")
            renderer = {v: k for k, v in self.RENDERER_NAMES.items()}.get(
                drew, renderer)
            if renderer != "vulkan":
                self.layer_owed = True
                print("  validation layer: owed -- the ranking opened on "
                      "%s; proven at the walk's first Vulkan entry" % drew,
                      flush=True)
                return
        if renderer != "vulkan":
            return
        import bare_driver as D
        if not self.layer_mapped():
            raise D.Unreachable(
                "the validation layer was asked for and is not in the "
                "program on %s -- bare_driver.py --setup installs it"
                % self.machine)
        print("  validation layer: loaded", flush=True)

    def layer_mapped(self):
        """Whether the validation layer is in the program there now."""
        return self.box.has_mapped(self.remote_pid, self.VALIDATION)

    def close(self):
        box = getattr(self, "box", None)
        if box:
            box.close()
            self.box = None
        self.portal = None

    def alive(self):
        # Until the ssh ends, or the shell there says the program has: a
        # wine program's ssh outlives it (Bare.run_argv()).
        import bare_driver as D
        return (self.testbed.alive()
                and D.Bare.exit_said(self.testbed) is None)

    def stop(self, sig=15):
        """The program there signalled, and the ssh here waited out.

        Not the other way round: the ssh here, signalled, answers with the
        signal and not with the program's code, and a program that exits
        cleanly on SIGTERM is what stress.py's Escape check reads. Where the
        shell there says the code, that is the code: the ssh of a wine
        program is still waiting on wine's helpers."""
        import bare_driver as D
        testbed = getattr(self, "testbed", None)
        if testbed is None:
            return None
        box = getattr(self, "box", None)
        pid = getattr(self, "remote_pid", None)
        if box and pid and self.alive():
            try:
                box.signal(pid, sig)
                end = time.time() + 5 * self.patience()
                while self.alive() and time.time() < end:
                    time.sleep(POLL)
            except Exception:
                pass
        def clean():
            if box:
                try:
                    box.kill_stale()
                except Exception:
                    pass

        said = D.Bare.exit_said(testbed)
        if said is not None:
            # The program has gone, and what holds its ssh open is wine's
            # helpers, which go with the wine server.
            clean()
            testbed.stop(sig)
            return said
        rc = testbed.stop(sig)
        clean()
        return rc

    def dump(self, n=25):
        # Without the shell's pid line, which is the harness's and not the
        # program's: every log check reads this.
        import bare_driver as D
        return [l for l in self.testbed.dump(n)
                if not l.startswith((D.PID_TAG, D.EXIT_TAG))]

    def pid(self):
        """The program's pid *there*. Nothing here may read /proc for it;
        cpu_seconds() asks the machine."""
        return self.remote_pid

    def cpu_seconds(self):
        return self.box.cpu_seconds(self.remote_pid)

    def _window_pid(self):
        return self.remote_pid


class X11Bare(_OnBare, X11):
    """The Linux tree's binary on another Linux machine's Xorg, with no
    window manager, desktop or compositor on it, told to open X11.

    Not another build: the tree is the desktop's, and what differs is that
    nothing answers _NET_SUPPORTING_WM_CHECK. So the library moves and sizes
    its own window, does its own fullscreen and maximize arithmetic, and is
    focused by the X server alone -- the paths no environment on this
    desktop reaches. See bare_driver.py for the machine.

    The questions are X11's, put to that server through a forwarded
    socket, and so are the aiming and the spare key's remap. The input is
    XTEST there (Hands), which goes to the server's focus and nowhere
    else."""

    NAME = "x11-vulkan-bare"
    INPUT = "xtest"
    FORWARD_X = True
    # x11.c's MANUAL_WIDTH_STEP: a resize run here moves in 16 pixels, and
    # the floor is kept to the step, rounded up.
    FLOOR_STEP = 16
    # No "unminimize": iconifying is a request to a manager, and with none
    # the window stays where it is. No "desk-layout": the layout switched
    # is GNOME's here, which that server never reads. "multi-output" as on
    # X11 here: RandR shows a client every output, and a machine with one
    # skips the section for that reason. "known-layout": XTEST's keyboard
    # is given the console's layout (Bare.xtest_layout()); left with the
    # server's default it typed every key of the German table's US legend.
    CAN = frozenset({"server-rect", "edge-resize", "read-pixels",
                     "wm-props", "remap-key", "multi-output", "resizable",
                     "known-layout"})
    # Which of the machine's outputs the round runs on, by its RandR name
    # (HDMI-1); unset is the primary, which is the panel. Its own variable:
    # FNL_DRIVE_OUTPUT names an output of this desktop.
    WANT_OUTPUT = "FNL_DRIVE_BARE_OUTPUT"
    NO_UNMINIMIZE = "with no window manager nothing iconifies a window"
    # The library's line for this desktop, which is what it is for, and
    # the nudge Xorg's TearFree swapchains want on resize there.
    EXPECTED_LOG: tuple[str, ...] = (
        r"^no EWMH window manager is running",
        r"^every swapchain at the window's own size is out of date at once "
        r"\(\d+ in a row\); the window's size is nudged$")

    def bring_up(self):
        self.box.x_up()

    def server(self):
        """The X server, its monitors, and XTEST on it. The output a round
        is asked for is looked for before the program is started."""
        import bare_driver as D
        self.X = self.box.server()
        self._mons = parse_monitors(self.box.monitors())
        want = os.environ.get(self.WANT_OUTPUT)
        if want and want not in [m[0] for m in self._mons]:
            raise D.Unreachable("no output %r on %s; %s names one of %s"
                                % (want, self.machine, self.WANT_OUTPUT,
                                   ", ".join(m[0] for m in self._mons)))
        return self.box.hands(self.X)

    def start(self, variant="release", args=(), env=None):
        import bare_driver as D
        import wayland_driver as W
        self.W = W
        self._bare_start(variant, args, env)
        try:
            # In slices, so that a program gone in fnl_init() is said at
            # once and not after the whole search.
            end = time.time() + 20.0
            while True:
                try:
                    self.win = self.X.find_window(pid=self.remote_pid,
                                                  timeout=0.5)
                    break
                except RuntimeError:
                    if not self.alive() or time.time() >= end:
                        raise
            self.drv = W.Driver(self.portal, self.testbed, slow=self.slow,
                                gain=self._gain, aim=self._aim,
                                monitors=self.monitors)
            # And no focus() here: whether the window has the keyboard
            # with nobody to give it one is the round's first question.
            self.pin_to_target()
            self.drv.find_window()
            self._validated()
        except Exception as e:
            # Gone before it had a window, as on weston (WaylandBare.start()):
            # what it said last is why this machine cannot run it.
            gone = not self.alive()
            said = self.dump(3) if gone else []
            self.stop()
            self.close()
            if gone:
                raise Unavailable("the program exited at start: %s"
                                  % (said[-1] if said else "silently"))
            if isinstance(e, D.Unreachable):
                raise Unavailable(str(e))
            raise
        return self

    def target_monitor(self):
        mons = self._mons
        if not mons:
            return None
        want = os.environ.get(self.WANT_OUTPUT)
        name, _pri, x, y, w, h = next(
            (m for m in mons if m[0] == want), None) or next(
            (m for m in mons if m[1]), mons[0])
        return (name, x, y, w, h)

    def monitors(self):
        return self._mons

    def monitor(self):
        x, y, w, h = self.rect()
        cx, cy = x + w // 2, y + h // 2
        for _name, _pri, mx, my, mw, mh in self._mons:
            if mx <= cx < mx + mw and my <= cy < my + mh:
                return (mx, my, mw, mh)
        return self._mons[0][2:] if self._mons else None

    def work_area(self):
        # No manager publishes _NET_WORKAREA, and the library then takes
        # the monitor whole; so does the round.
        return self.X.work_area() or self.monitor()

    def pin_to_target(self):
        """Centred on the machine's monitor, and the pointer over it.

        A window nobody manages opens where it asked to, which is the
        corner; a ConfigureWindow is all it takes to move it, and Hands can
        put the pointer where it is wanted instead of parking it blind."""
        tgt = self.target_monitor()
        if tgt:
            name, mx, my, mw, mh = tgt
            _x, _y, w, h = self.rect()
            self.win.configure(x=mx + max(0, (mw - w) // 2),
                               y=my + max(0, (mh - h) // 2))
            self.X.d.sync()
            self.parked_on = name
        x, y, w, h = self.rect_still()
        self.portal.warp(x + w // 2, y + h // 2)
        self.drv.at = None
        return tgt[0] if tgt else None

    def focus(self):
        """The X server's focus, set on the window itself.

        Here and nowhere else: with no manager nobody answers
        _NET_ACTIVE_WINDOW, and the server's focus is the only account of
        where a key goes. The reason x11_driver.focus() never does this --
        a compositor that sends keys elsewhere -- has nothing to stand on
        here."""
        from Xlib import X as _X
        end = time.time() + 2.0
        while time.time() < end:
            self.win.set_input_focus(_X.RevertToPointerRoot, _X.CurrentTime)
            self.X.d.sync()
            if self.has_focus():
                return
            time.sleep(3 * POLL)

    def has_focus(self):
        """Whether keys go to the window. With nobody managing, the server's
        focus starts as PointerRoot -- the window the pointer is in -- and
        that counts as the window's as much as a focus set on it."""
        from Xlib import X as _X
        try:
            f = self.X.d.get_input_focus().focus
            f = getattr(f, "id", f)
            if f == _X.PointerRoot:
                child = self.X.root.query_pointer().child
                return bool(child) and getattr(child, "id", 0) == self.win.id
        except Exception:
            return False
        return f == self.win.id

    def unminimize(self):
        return not self.minimized()

    def capture(self, path):
        from PIL import Image
        self.box.capture(self.win, path)
        return Image.open(path).convert("RGB")

    def clipboard_missing(self):
        return "" if self.box.has("xclip") else \
            "no xclip on %s -- bare_driver.py --setup installs it" \
            % self.machine

    def clipboard_get(self):
        return self.box.clipboard_get()

    def clipboard_put(self, data):
        self.box.clipboard_put(data)

    def clipboard_done(self):
        self.box.clipboard_done()


# What the library says, through EGL, of a device with no OpenGL it can
# draw with (egl.c): neither a 4.5 nor a 4.1 core context, 4.1 being the
# one it steps down to.
NO_GL41 = ("this device has no OpenGL 4.1",)


class _SoftGL(Environment):
    """An OpenGL round on the bare machine, put on Mesa's llvmpipe where the
    machine's GPU has no 4.1 (the thinkpad's Ironlake, GL 2.1). The program
    says so and exits at start; it does not switch itself, since only
    LIBGL_ALWAYS_SOFTWARE does and a library does not write its caller's
    environment (egl.c). The round sets it and starts once more, and says
    so; the device line then names llvmpipe. Set by hand, it is what the
    round starts with and nothing is retried."""

    def start(self, variant="release", args=(), env=None):
        try:
            return super().start(variant, args, env)
        except Unavailable as e:
            if not any(n in str(e) for n in NO_GL41) or \
                    "LIBGL_ALWAYS_SOFTWARE" in \
                    {**os.environ, **(env or {})}:
                raise
        print("  no OpenGL 4.1 on %s's GPU: started again on llvmpipe "
              "(LIBGL_ALWAYS_SOFTWARE=1)" % (self.machine or "?"),
              flush=True)
        return super().start(variant, args,
                             dict(env or {}, LIBGL_ALWAYS_SOFTWARE="1"))


class X11GLBare(_SoftGL, X11Bare):
    """The linux binary on the same machine's Xorg, told OpenGL: X11Bare's
    in everything but the renderer, as X11GL is X11's."""
    NAME = "x11-gl-bare"
    TREE = "linux"
    RENDERER = "gl"


class WaylandBare(_OnBare, Wayland):
    """The Linux tree's binary on the bare machine's weston, with no
    panel, shell client or desktop around it, told to open Wayland.

    Not another build: the tree is the desktop's, and what differs is what the
    compositor advertises. GNOME offers every global the backend binds, so
    the paths for one that is not there run here or nowhere; the library
    says at open which were left unbound. The round asks the same
    questions as on the native Wayland environment, through the same echo.

    The input is uinput there (bare_driver.UinputHands): devices weston
    finds through libinput like any others, unaccelerated by its config."""

    NAME = "wayland-vulkan-bare"
    INPUT = "uinput"
    # "grab-ring": the margin is the backend's own surface geometry, whoever
    # composites it. "composited": weston-screenshooter, which weston allows
    # under --debug. "multi-output": every output on, where weston's scene
    # graph says it put each (Bare.weston_layout()). No "input-method":
    # weston's composer is text-input v1, which the library does not speak.
    # No "desk-layout": that is GNOME's.
    CAN = frozenset({"grab-ring", "composited", "multi-output",
                     "resizable", "known-layout"})
    NO_UNMINIMIZE = "a Wayland client cannot ask to be unminimized"

    # The library's bar, in points (DECO_TITLEBAR_H), and the pause between
    # pushing the pointer into the output's corner and leaving it.
    BAR = 34
    CORNER_BEAT = 0.15
    # A step left on the way to the corner: under the narrowest output's
    # width, so no step leaves an output for a place no output covers.
    CORNER_STEP = 400
    # The window point make_room() takes the window in by, both axes: the
    # top-left corner of the grab ring, outside the window.
    GRIP = -3

    def _want_output(self):
        """The output the round is on: FNL_DRIVE_BARE_OUTPUT's, or the
        panel, which outputs() lists first."""
        import bare_driver as D
        mons = self.box.outputs()
        want = os.environ.get(X11Bare.WANT_OUTPUT)
        if want and want not in [m[0] for m in mons]:
            raise D.Unreachable("no output %r on %s; %s names one of %s"
                                % (want, self.machine, X11Bare.WANT_OUTPUT,
                                   ", ".join(m[0] for m in mons)))
        name = want or (mons[0][0] if mons else None)
        self._mons = [(n, True, 0, 0, w, h) for n, _p, _x, _y, w, h in mons
                      if n == name]
        return name

    def bring_up(self):
        name = self._want_output()
        self.box.weston_up()
        self._mons = [(n, n == name, x, y, w, h)
                      for n, _p, x, y, w, h in self.box.weston_layout()]

    def server(self):
        """The hands, with the pointer on the round's output: weston opens
        a window on the output the pointer is on."""
        self.bring_up()
        hands = self.box.uinput_hands()
        _name, mx, my, mw, mh = self.target_monitor()
        cx, cy = self._corner(hands)
        hands.rel(mx + mw // 2 - cx, my + mh // 2 - cy)
        return hands

    def _corner(self, hands):
        """The pointer pushed into the top-left output's top-left corner,
        and where that is, in pixels.

        Not one push: weston keeps a motion that ends where no output is
        on the output it began on, so from the second output a push up
        and left stops at that output's corner. Up first, to the outputs'
        shared top edge, then left in steps each landing on an output."""
        mons = self._mons
        span = max(x + w for _n, _p, x, _y, w, _h in mons) - min(
            x for _n, _p, x, _y, _w, _h in mons)
        hands.rel(0, -30000)
        for _ in range(span // self.CORNER_STEP + 1):
            hands.rel(-self.CORNER_STEP, 0)
        hands.rel(-30000, -30000)
        time.sleep(self.CORNER_BEAT)
        left = min(mons, key=lambda m: (m[2], m[3]))
        return left[2], left[3]

    def _origin(self):
        """Where the window's top-left is on the screen, in pixels.

        The pointer pushed into the corner is at a place weston's layout
        names, and the motion here is unaccelerated: a point reached from
        the corner is a known place, and the echo says where in the window
        it is. A few in the round's output are tried, for a window that
        does not cover the middle."""
        _name, mx, my, mw, mh = self.target_monitor()
        s = (self.state() or {"scale": 120})["scale"] / 120.0
        for fx, fy in ((0.5, 0.5), (0.25, 0.25), (0.75, 0.25),
                       (0.25, 0.75), (0.75, 0.75)):
            cx, cy = self._corner(self.portal)
            sx, sy = mx + int(mw * fx), my + int(mh * fy)
            m = self.mark()
            self.portal.rel(sx - cx, sy - cy)
            self.quiet()
            seen = self.since(m, "motion")
            if seen:
                parts = seen[-1]["parts"]
                wx, wy = float(parts[1]), float(parts[2])
                self.drv.at = (wx, wy)
                return (sx - wx * s, sy - wy * s)
        self.drv.at = None
        raise RuntimeError("the window covers none of five points of %dx%d"
                           % (mw, mh))

    def placed(self, still=False):
        """Found by the pointer (_origin()) on the output weston put the
        round on, whose place weston's layout gives."""
        _name, mx, my, mw, mh = self.target_monitor()
        st = self.refresh()
        s = st["scale"] / 120.0
        ox, oy = self._origin()
        return ((ox, oy, st["area"][0] * s, (st["area"][1] + self.BAR) * s),
                (mx, my, mw, mh))

    def make_room(self, room):
        """Into the output's corner by the titlebar, taken in by the grab
        ring's top-left corner, and centred: the order in which every grip
        is on the screen when it is pressed. A 1280x720 window on a
        1366x768 panel has its bottom edge below the screen wherever weston
        opens it, and a press aimed at an edge nobody can reach lands in
        the scene and captures the pointer."""
        # The window and `room` in points, the output and the origin in
        # pixels, as placed() has them; the grip is dragged in points.
        _name, mx, my, mw, mh = self.target_monitor()
        st = self.refresh()
        s = st["scale"] / 120.0
        w, h = st["area"][0], st["area"][1] + self.BAR
        want_w = min(w, int(mw / s) - room[0])
        want_h = min(h, int(mh / s) - room[1])
        ox, oy = self._origin()
        fx = mx + (mw - round(want_w * s)) // 2
        fy = my + (mh - round(want_h * s)) // 2
        if (want_w, want_h) == (w, h) and (round(ox), round(oy)) == (fx, fy):
            return False
        shrink = (want_w, want_h) != (w, h)
        if shrink:
            self.move_window_by(mx + 12 - ox, my + 12 - oy)
            m = self.mark()
            g = self.GRIP
            self.drv.drag(g, g, g + (w - want_w), g + (h - want_h), steps=12)
            self.settle("resize", m, 1.5)
            self.quiet()
            ox, oy = self._origin()
        self.move_window_by(fx - ox, fy - oy)
        self.quiet()
        return shrink

    def start(self, variant="release", args=(), env=None):
        import bare_driver as D
        import wayland_driver as W
        self._bare_start(variant, args, env)
        try:
            # The focus, echoed once the window is mapped and activated: a
            # shell activates what it maps, and with nobody else on the
            # seat there is nothing to activate instead.
            end = time.time() + (12.0 if self.slow else 6.0)
            while (not self.keyboard_is_ours() and self.alive()
                   and time.time() < end):
                time.sleep(POLL)
            if not self.alive():
                # Gone before it had a window: what it said last is the
                # reason this machine cannot run it, and no sweep finds it.
                said = self.dump(3)
                raise D.Unreachable("the program exited at start: %s"
                                    % (said[-1] if said else "silently"))
            self.drv = W.Driver(self.portal, self.testbed, slow=self.slow,
                                monitors=self.monitors)
            self.drv.find_window()
            self._validated()
        except Exception as e:
            self.stop()
            self.close()
            if isinstance(e, D.Unreachable):
                raise Unavailable(str(e))
            raise
        return self

    def target_monitor(self):
        mons = self._mons
        if not mons:
            return None
        name, _pri, x, y, w, h = next((m for m in mons if m[1]), mons[0])
        return (name, x, y, w, h)

    def monitors(self):
        return self._mons

    def capture(self, path):
        from PIL import Image
        self.box.weston_capture(path)
        return Image.open(path).convert("RGB")

    def clipboard_missing(self):
        # Another client can reach weston's selection only with the
        # keyboard focus, which wl-copy takes by mapping a window of its
        # own: the window under test would lose it to the check.
        return ("weston offers no data-control protocol, so another "
                "program reaches the clipboard only by taking the focus")


class WaylandGLBare(_SoftGL, WaylandBare):
    """The linux binary on the same machine's weston, told OpenGL, as
    WaylandGL is Wayland's."""
    NAME = "wayland-gl-bare"
    TREE = "linux"
    RENDERER = "gl"


class KmsBare(WaylandBare):
    """The Linux tree's binary on the bare machine with no window server at
    all, told to open KMS: neither Xorg nor weston up, so the program opens
    the DRM node first and has the panel whole, and reads the kernel's
    devices itself.

    A Wayland round in everything it can ask -- the echo for the focus and
    the pointer, nothing to say where the window is -- less what a window
    server answers: the window is the output, so there is nothing to
    resize, maximize, minimize or move, and nobody to share a clipboard
    with. The hands are the same uinput devices, read by the backend
    through evdev, and the keyboard is grabbed from the console."""
    # A round per kernel driver bare-hosts names (`@schenker:nouveau`).
    DRIVERS = True

    NAME = "kms-vulkan-bare"
    BACKEND = "kms"
    # "composited": the framebuffer the CRTC is scanning out, read by root
    # there (kms_grab.py) -- the screen itself, past the copy path, the
    # mode set, the flips and the cursor plane. "drawn-pointer": nothing
    # but the library puts a pointer there. No "resizable", which is the
    # point of the backend.
    # "display-modes": the window is the output, so its titlebar lists the
    # output's modes and the card's outputs. "forced-hotplug": root there
    # can have a connector unplugged and plugged back (force_output()).
    # "layout-groups": the layout is the program's to name, and it is named
    # with a second group (group_layout()).
    CAN = frozenset({"composited", "drawn-pointer", "known-layout",
                     "display-modes", "forced-hotplug", "layout-groups"})
    # The second group "layout-groups" switches to, and the option that
    # makes Alt+Shift its group key: every PC keyboard has both halves.
    SECOND_GROUP = "us"
    GROUP_OPTION = "grp:alt_shift_toggle"

    def force_output(self, name, state):
        self.box.force_output(name, state)

    def force_output_heard(self, name, state):
        return self.box.force_output_heard(name, state)

    def target_monitor(self):
        # The mode the program opened at, as its open line names it: what
        # sysfs lists first is not always the mode the kernel flags
        # preferred -- schenker's HDMI monitor has listed 1920x1080 first
        # and preferred 3840x2160 -- and the window is the preferred one.
        for ln in self.dump(1 << 30) if getattr(self, "testbed", None) else ():
            m = re.search(r"KMS: (\S+) on \S+, (\d+)x(\d+) at", ln)
            if m:
                return (m.group(1), 0, 0, int(m.group(2)), int(m.group(3)))
        return super().target_monitor()
    NO_UNMINIMIZE = "a bare display has nothing to minimize into"
    # The open line, the mode the titlebar set, and the line for a device
    # that will not take the output (the thinkpad's lavapipe, NVK): the
    # frames go the copy path's way there; and with no headless surface
    # for the device (NVIDIA's own driver), into images of its own.
    EXPECTED_LOG = (r"^KMS: \S+ on /dev/dri/card\d+, \d+x\d+ at [\d.]+ Hz, ",
                    r"^KMS: \S+ at \d+x\d+, [\d.]+ Hz$",
                    r"^KMS: .+; drawing offscreen and copying each frame onto "
                    r"\S+$",
                    r"^KMS: no headless surface for the GPU that will draw; "
                    r"it draws into images of its own$",
                    # Which way OpenGL reaches the output, said once a
                    # device (egl_kms.c): onto the backend's buffers, or
                    # read back where the GPU will not take them.
                    r"^KMS: OpenGL draws onto the output's own buffers, "
                    r"nothing read back$",
                    r"^KMS: OpenGL copies each frame onto the output: .+$",
                    # An output turned off, by the round's forced
                    # unplug or by a move to another (output_dark()).
                    r"^KMS: \S+ dark$",
                    # The round's forced unplug and plug back, as
                    # outputs_rescan() hears them, where the machine has
                    # a second monitor ("forced-hotplug").
                    r"^KMS: \S+ unplugged$",
                    r"^KMS: \S+ plugged in( again)?, \d+ modes?$",
                    # A monitor off the bus for a moment as it sleeps or
                    # wakes (black's Samsung and iiyama), and the move owed
                    # to one while it is away (outputs_rescan(),
                    # kms_set_output()).
                    r"^KMS: \S+ left; an output counts as unplugged once "
                    r"it has been away [\d.]+ s, [\d.]+ s if the window "
                    r"is on it, since a monitor leaves the bus as it "
                    r"sleeps and as it wakes$",
                    r"^KMS: \S+ is away; the window goes there when it "
                    r"is back$",
                    # The display surface remade for a mode the round set
                    # (the renderers' device.c): the display path's line,
                    # which the copy path on lavapipe never says.
                    r"^the surface is made again for the output's new "
                    r"mode$",
                    # The layout applied and where its names came from
                    # (keymap_load()); a layout that failed is a warning
                    # of another shape and stays a complaint.
                    r"^KMS: layout \S.*, (the console's|FNL_KMS_LAYOUT's|"
                    r"the default, with no console keyboard configured)"
                    r"(; left out: .*)?$",
                    # A group key, the round's Alt+Shift
                    # ("layout-groups"), as group_switch() says it.
                    r"^KMS: typing in group \d+ of \d+, \S+$",
                    # The machine's own keyboard, held by the round
                    # (input_hold.py) before the program grabs it: the
                    # program has the round's uinput keyboard alone.
                    r"^KMS: event\d+ is held by another "
                    r"program \(Device or resource busy\)$")

    def bring_up(self):
        self._want_output()
        self.box.bare_display()

    def server(self):
        self.bring_up()
        return self.box.uinput_hands()

    def program_env(self):
        # The output the round is on is the one the program takes.
        want = os.environ.get(X11Bare.WANT_OUTPUT)
        env = {"FNL_KMS_OUTPUT": want} if want else {}
        spec = self.group_layout()
        if spec:
            env["FNL_KMS_LAYOUT"] = spec
        return env

    def group_layout(self):
        """FNL_KMS_LAYOUT for the console's own layout as group 1 and
        SECOND_GROUP as group 2, GROUP_OPTION beside the console's options:
        group 1 is what "known-layout" types through, unchanged. None where
        the console's layout is more than one group already, or is
        SECOND_GROUP, and "layout-groups" then skips with the reason."""
        kb = self.box.console_keyboard()
        layout, variant = kb.get("XKBLAYOUT", ""), kb.get("XKBVARIANT", "")
        if not layout or "," in layout or layout == self.SECOND_GROUP:
            return None
        options = ",".join(o for o in (kb.get("XKBOPTIONS", ""),
                                       self.GROUP_OPTION) if o)
        return "%s%s,%s %s" % (layout, "(%s)" % variant if variant else "",
                               self.SECOND_GROUP, options)

    def make_room(self, room):
        return False

    def capture(self, path):
        # The output the window is on, which the first CRTC lit need not
        # be: the console can still be on another. The last state on
        # record, not refresh(), whose nudge would move the pointer the
        # picture is of.
        from PIL import Image
        st = self.testbed.state() or {}
        out = (st.get("out") or "").split()
        self.box.kms_capture(path, out[0] if out else None)
        return Image.open(path).convert("RGB")

    def clipboard_missing(self):
        return "there is no other program on a bare display to share one"


class KmsGLBare(_SoftGL, KmsBare):
    """The linux binary there, told OpenGL, as KmsBare is told Vulkan:
    OpenGL on Mesa's surfaceless EGL, every frame read back and flipped on
    by the backend, which is the only way OpenGL reaches the output. On a
    kernel driver, on a machine that keeps a ranking, KmsBare's round is
    this one's too, and this one is skipped (ranked_twin())."""
    NAME = "kms-gl-bare"
    TREE = "linux"
    RENDERER = "gl"
    RANKED_ROUND = False


# ---- Win32 under wine, on the bare machine -----------------------------------

# Mesa's loader, under wine on a machine whose GPU NVIDIA's own driver has:
# wine's EGL asks every device, and Mesa has no driver for that one. A fact
# about the machine, said at open.
MESA_NO_DRIVER = (r"^pci id for fd \d+: [0-9a-f]{4}:[0-9a-f]{4}, "
                  r"driver \(null\)$")


class _WineOnBare:
    """What a Windows tree's round on the bare machine has whichever server
    it is on: the .exe, wine to run it (Bare.install() sees the suffix and
    takes the .dll files and the prefix with it), and wine's chatter kept
    out of the log, as Win32.ENV keeps it here."""
    SUFFIX = ".exe"
    # Wine's own Direct3D 12, which this environment loads: vkd3d makes
    # the device and lists the GPUs, and refuses every DXIL pipeline state,
    # so the move fails and comes back (tools/vkd3d_proton.py).
    NO_MOVE_TO = {"d3d12": "wine 10.0's own vkd3d runs no DXIL; the d3d12 "
                           "environments load vkd3d-proton"}
    # First in every wine round's order, ahead of the Linux environment of
    # the server it is on, whose backend a Windows tree has not got.
    BACKEND = ""
    # The Win32 backend's floor, which is not X11's (X11Bare).
    FLOOR_STEP = 0
    ENV = {"WINEDEBUG": "-all"}
    EXPECTED_LOG: tuple[str, ...] = (MESA_NO_DRIVER,)

    def program_env(self):
        # ENV and not the round's variables: _Vkd3dProton.stage() puts
        # its overrides there. Ahead of what is forwarded, so the layer is
        # asked of the machine's loader and not the library (wine_layer_env).
        return {**self.ENV, **wine_layer_env(self.RENDERER)}

    @classmethod
    def wine_version(cls):
        # The machine's own wine runs it, which FNL_DRIVE_WINE does not
        # choose: the desk's answer (_DeskWine), which the Wayland ones
        # inherit, would be about another wine.
        return ""

    def install_dir(self, tree):
        # One a renderer, and not the tree's: what is copied is not the same
        # for the two -- a Direct3D 12 round's comes from the directory
        # _Vkd3dProton stages, with DXVK's dxgi.dll beside the program,
        # which a Vulkan round there must not load. Bare.install() clears a
        # directory's .dll files before each copy as well.
        return "%s-%s" % (tree, self.RENDERER)

    def device_found(self, said):
        # size_await()'s bound, which a frame drawn by the CPU outlasts on
        # a slow machine: the thinkpad's llvmpipe, under vkd3d-proton most
        # of all (DESIGN *Wine on the bare machine*). On a GPU it is still
        # a complaint. Lavapipe calls itself llvmpipe too -- but under wine
        # on the thinkpad's Xorg the program names its device after the X
        # driver ("modesetting", and "hardware" at that), so the machine's
        # own Vulkan devices are asked as well: llvmpipe alone, and every
        # frame there is the CPU's.
        cpu = bool(said) and said[0].startswith("llvmpipe")
        if not cpu:
            vk = [ln.split(": ", 1)[1] for ln in self.box.facts()
                  if ln.startswith("vulkan: ")]
            cpu = bool(vk) and all(d.startswith("llvmpipe") for d in vk)
        self.EXPECTED_LOG = type(self).EXPECTED_LOG + (
            (SIZE_UNSHOWN,) if cpu else ())


class Win32Bare(_WineOnBare, X11Bare):
    """The win64 tree's binary under wine on the bare machine's Xorg: wine
    with no window manager to answer it, which nothing on this desktop
    has. X11Bare's round in what the server answers -- the rectangle, the
    focus, XTEST -- with the window wine's, as Win32 is X11's here.

    Less "wm-props": the hints and WM_CLASS are wine's. Less "remap-key":
    wine reads the keymap into a layout of its own once, and a keysym put
    on a spare key afterwards is no key of that layout."""
    NAME = "win64-vulkan-bare"
    TREE = "win64"
    CAN = X11Bare.CAN - {"wm-props", "remap-key"}
    on_output = Win32.on_output
    COMPOSED_ELSEWHERE = ("wine composes, by Xlib's Compose table: "
                          "an accent and a space is an apostrophe, and an "
                          "accent before a letter it does not go on is "
                          "nothing")

    def start(self, variant="release", args=(), env=None):
        """X11Bare's, and the window then given the X server's focus.

        Wine sets the focus on a window it keeps itself only where a child
        of the window has the Win32 focus (X11DRV_MapNotify asks IsChild),
        so a top-level window that has it never does: keys reach it through
        PointerRoot, but wine clips the pointer only for a process that has
        the X focus (is_current_process_focused()), and a captured pointer
        then neither stays in the window nor moves by raw motion. Given here
        as a manager or a click on one would give it, so that the round is
        about the library; TODO.md *Wine on the bare machine*."""
        X11Bare.start(self, variant, args, env)
        self.focus()
        return self

    def pin_to_target(self):
        """Left where wine put it, and the pointer put over it.

        Not moved: wine's window there is its own to place (Managed=N,
        Bare.WINE_SETUP), and wine reads no position off the server -- a
        window moved behind its back is one wine still thinks is where it
        was, and every point the pointer is at is then off by the move."""
        x, y, w, h = self.rect_still()
        self.portal.warp(x + w // 2, y + h // 2)
        self.drv.at = None
        tgt = self.target_monitor()
        if tgt:
            self.parked_on = tgt[0]
        return tgt[0] if tgt else None

    def move_window_by(self, dx, dy):
        """By its titlebar, as a hand would, and for pin_to_target()'s
        reason: a ConfigureWindow moves a window wine still thinks is where
        it was. In points, by the title and not the middle, as
        Wayland.move_window_by() drags; the pointer goes with the window."""
        s = self._gain()
        st = self.state()
        x = caption_x(st["area"][0] if st and "area" in st else None)
        self.drv.drag(x, 17, x + dx / s, 17 + dy / s, steps=24, settle=0.7)
        self.rect_still()


class Win32D3D12Bare(_Vkd3dProton, Win32Bare):
    """The win64 binary there told to draw with Direct3D 12, on
    vkd3d-proton as Win32D3D12 is here: the run directory
    _Vkd3dProton.stage() makes is what is copied over."""
    NAME = "win64-d3d12-bare"
    EXPECTED_LOG = _Vkd3dProton.EXPECTED_LOG + _WineOnBare.EXPECTED_LOG


class Win32WaylandBare(_WineOnBare, WaylandBare, Win32Wayland):
    """The win64 tree's binary under wine on the bare machine's weston:
    wine's Wayland driver on a compositor that offers less than GNOME's.
    WaylandBare's round in how the window is found and pictured, and
    Win32Wayland's in how a point of the bar or an edge is reached, which
    under wine echoes nothing (_aim()): weston's pointer is unaccelerated,
    which that reckoning needs.

    The same declarations as Win32Wayland's, and for its reasons. Not
    "known-layout" either: weston is given the console's layout, and wine's
    Wayland driver types through a table of its own -- under German, Y is
    y and the o-umlaut key a u-umlaut."""
    NAME = "win64-vulkan-wl-bare"
    TREE = "win64"
    CAN = frozenset({"composited", "edge-resize", "resizable"})
    # Measured with WINEDEBUG=+waylanddrv (wine 11.20, weston 15): leaving
    # fullscreen for maximized, the window is restored and then maximized,
    # and wine sends weston unset_fullscreen and then set_maximized. weston
    # answers the first with a configure of its own, which wine takes for
    # its answer to both: it clears WS_MAXIMIZE, calls a window that covers
    # the monitor and is not maximized fullscreen, and weston is left
    # showing a floating window as wide as the monitor, at the restored
    # window's place. mutter answers the two with one configure, and the
    # round on this desktop passes.
    MAXIMIZED_LOST_IN_FULLSCREEN = (
        "wine's Wayland driver takes weston's answer to unset_fullscreen "
        "for its answer to the set_maximized that followed it, and leaves "
        "the window floating at the monitor's width")
    # The window's own top-left corner: a Win32 window's resize edge is a
    # margin inside it, and there is no ring outside.
    GRIP = 3

    def start(self, variant="release", args=(), env=None):
        WaylandBare.start(self, variant, args, env)
        self.drv.aim = self._aim
        return self


class Win32D3D12WaylandBare(_Vkd3dProton, Win32WaylandBare):
    NAME = "win64-d3d12-wl-bare"
    EXPECTED_LOG = _Vkd3dProton.EXPECTED_LOG + _WineOnBare.EXPECTED_LOG


class Win32CageBare(Win32WaylandBare):
    """The win64 tree's binary under wine on the bare machine with no
    window server but cage: the KMS round of a program that cannot open the
    DRM node itself (bare_driver.py says why cage).

    A kiosk's window is the output: cage sizes it and nothing else may, so
    not "resizable" and not "edge-resize", and nothing minimizes it. Unlike
    on the KMS backend, the library does not know that, and its bar keeps
    every button. The hands are an absolute pointer, since cage's relative
    one is accelerated and the bar's points are reckoned (_aim())."""
    NAME = "win64-vulkan-cage-bare"
    TREE = "win64"
    # "composited": grim, through cage's screencopy. Not "known-layout",
    # for Win32WaylandBare's reason.
    CAN = frozenset({"composited"})
    NO_UNMINIMIZE = "a kiosk has nothing to minimize into"

    def clipboard_missing(self):
        return ("cage offers no data-control protocol, and another program "
                "takes the selection only with a window, which cage would "
                "show in this one's place")

    def bring_up(self):
        self.box.cage_up(self._want_output())

    def server(self):
        self.bring_up()
        _n, _p, _x, _y, w, h = self._mons[0]
        return self.box.uinput_hands(absolute=(w, h))

    def make_room(self, room):
        return False

    def capture(self, path):
        from PIL import Image
        self.box.cage_capture(path)
        return Image.open(path).convert("RGB")


class Win32D3D12CageBare(_Vkd3dProton, Win32CageBare):
    NAME = "win64-d3d12-cage-bare"
    EXPECTED_LOG = _Vkd3dProton.EXPECTED_LOG + _WineOnBare.EXPECTED_LOG


ALL = (Wayland, WaylandGL, X11, X11GL, Win32, Win32D3D12, Win32Wayland,
       Win32WaylandD3D12, Win32VM, Win32VMD3D12, Win32Native,
       Win32NativeD3D12, X11Bare, X11GLBare, WaylandBare, WaylandGLBare,
       KmsBare, KmsGLBare, Win32Bare, Win32D3D12Bare, Win32WaylandBare,
       Win32D3D12WaylandBare, Win32CageBare, Win32D3D12CageBare,
       CocoaNative, CocoaNativeVulkan, CocoaNativeGL)


def by_name(name):
    for cls in ALL:
        if cls.NAME == name:
            return cls
    raise SystemExit("no environment %r; this project builds %s"
                     % (name, ", ".join(c.NAME for c in ALL)))


def select(names):
    """Every place the names stand for, or every place there is.

    A name is an environment's, and as much of `@machine:device` after it as
    narrows it: `win64-vulkan-native` is every laptop on every device,
    `win64-vulkan-native@schenker` that one on each of its devices, and
    `win64-vulkan-native@schenker:intel` one round."""
    if not names:
        return [b for cls in ALL for b in cls.each()]
    made = []
    for name in names:
        cls = by_name(name.split("@")[0])
        places = cls.each()
        got = [b for b in places if name in (cls.NAME, b.NAME)
               or b.NAME.startswith(name + ":")]
        if not got:
            raise SystemExit("nothing is called %r; %s is %s"
                             % (name, cls.NAME,
                                ", ".join(b.NAME for b in places)))
        made += got
    return made
