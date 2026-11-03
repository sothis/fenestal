# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Synthetic input through the RemoteDesktop portal, for all three backends,
and the loop that aims it.

The portal is the sanctioned route and needs no root: one consent dialog,
once, and after that mutter injects the events into its own input stack. What
is driven is therefore the whole path a hand takes -- compositor, window
server, backend -- whichever backend the window belongs to.

Two consequences worth knowing before reading the code.

  Motion is *relative* only. Absolute positioning needs a ScreenCast stream
  to name the coordinate space, which is a second consent dialog. It is not
  needed: the library's event echo prints the window-relative point of every
  motion event, so move_to() steers by the application's own report. That
  loop is self-correcting, which makes it immune to the scale factor, the
  grab ring and where the compositor put the window.

  A Wayland client is never told where its window is, so neither is anything
  outside it. Finding the window means sweeping the pointer until a motion
  event appears.
"""

import json
import os
import subprocess
import sys
import threading
import time
import urllib.parse

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

from testbed import QUIET, STATE_DIR

BUS_NAME = "org.freedesktop.portal.Desktop"
OBJ_PATH = "/org/freedesktop/portal/desktop"
IFACE_RD = "org.freedesktop.portal.RemoteDesktop"
IFACE_SC = "org.freedesktop.portal.Screenshot"
IFACE_RQ = "org.freedesktop.portal.Request"
IFACE_SS = "org.freedesktop.portal.Session"

DEVICE_KEYBOARD = 1
DEVICE_POINTER = 2

# evdev codes, which is what the portal speaks -- not X keysyms and not XKB
# keycodes (those are these plus 8). Every key the testbed can name, under the
# name the echo gives it: src/backend/evdev_keys.h read against KEY_NAMES in
# src/core/echo.c, so a round can tap a key and look for the same word.
KEY = {
    "Escape": 1, "1": 2, "2": 3, "3": 4, "4": 5, "5": 6, "6": 7, "7": 8,
    "8": 9, "9": 10, "0": 11, "Minus": 12, "Equal": 13, "Backspace": 14,
    "Tab": 15, "Q": 16, "W": 17, "E": 18, "R": 19, "T": 20, "Y": 21, "U": 22,
    "I": 23, "O": 24, "P": 25, "LBracket": 26, "RBracket": 27, "Enter": 28,
    "LCtrl": 29, "A": 30, "S": 31, "D": 32, "F": 33, "G": 34, "H": 35, "J": 36,
    "K": 37, "L": 38, "Semicolon": 39, "Apostrophe": 40, "Grave": 41,
    "LShift": 42, "Backslash": 43, "Z": 44, "X": 45, "C": 46, "V": 47, "B": 48,
    "N": 49, "M": 50, "Comma": 51, "Period": 52, "Slash": 53, "RShift": 54,
    "KPMultiply": 55, "LAlt": 56, "Space": 57, "CapsLock": 58, "F1": 59,
    "F2": 60, "F3": 61, "F4": 62, "F5": 63, "F6": 64, "F7": 65, "F8": 66,
    "F9": 67, "F10": 68, "NumLock": 69, "ScrollLock": 70, "KP7": 71, "KP8": 72,
    "KP9": 73, "KPSubtract": 74, "KP4": 75, "KP5": 76, "KP6": 77, "KPAdd": 78,
    "KP1": 79, "KP2": 80, "KP3": 81, "KP0": 82, "KPDecimal": 83,
    "IntlBackslash": 86, "F11": 87, "F12": 88, "KPEnter": 96, "RCtrl": 97,
    "KPDivide": 98, "PrintScreen": 99, "RAlt": 100, "Home": 102, "Up": 103,
    "PageUp": 104, "Left": 105, "Right": 106, "End": 107, "Down": 108,
    "PageDown": 109, "Insert": 110, "Delete": 111, "Pause": 119, "LSuper": 125,
    "RSuper": 126, "Menu": 127,
}
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112

TOKEN_FILE = os.path.join(STATE_DIR, "portal-token.json")


class PortalUnavailable(RuntimeError):
    """No consent, so no input -- which is not a failing check. A type of
    its own so a harness can report the round skipped and not failed."""


class PortalRefused(PortalUnavailable):
    """The portal's answer was no, where PortalUnavailable alone may be
    only its silence."""


class Portal:
    """A started RemoteDesktop session, ready to inject."""

    def __init__(self, verbose=True, prompt=True):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SessionBus()
        obj = self.bus.get_object(BUS_NAME, OBJ_PATH)
        self.rd = dbus.Interface(obj, IFACE_RD)
        self.ss = dbus.Interface(obj, IFACE_SC)
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")
        self.seq = 0
        self.verbose = verbose
        # Whether a consent dialog is an acceptable outcome. A person running
        # this by hand says yes; a timed round says no and is skipped, because
        # the dialog is modal and takes the focus the round asserts on.
        self.prompt = prompt
        self.session = None
        self.restore_token = None

    # ---- request plumbing --------------------------------------------------

    def _request(self, call, *args, **kw):
        """Every portal method answers asynchronously on a Request object whose
        path is derived from our bus name and a token we choose.  Subscribing
        *before* the call is not optional: the reply can beat the return."""
        self.seq += 1
        token = "fnl%d" % self.seq
        path = "/org/freedesktop/portal/desktop/request/%s/%s" % (self.sender,
                                                                  token)
        done = threading.Event()
        box = {}

        def on_response(code, results):
            box["code"] = int(code)
            box["results"] = results
            done.set()

        match = self.bus.add_signal_receiver(
            on_response, signal_name="Response", dbus_interface=IFACE_RQ,
            path=path)

        # Removed however the call ends: one left behind by a call that
        # raised stays on the connection for the process's life.
        try:
            options = dict(kw.pop("options", {}))
            options["handle_token"] = token
            # Popped before the call: `timeout` is the deadline on the loop
            # below, and left in kw it would reach dbus-python as the
            # *call's* timeout.
            wait = float(kw.pop("timeout", 120.0))
            call(*args, options, **kw)

            # The GLib main loop has to run for the signal to arrive, and the
            # portal's own dialog runs on it too, so this is pumped rather
            # than slept through.
            ctx = GLib.MainContext.default()
            deadline = time.time() + wait
            while not done.is_set() and time.time() < deadline:
                ctx.iteration(False)
                time.sleep(0.01)        # poll
        finally:
            match.remove()

        if not done.is_set():
            raise PortalUnavailable(
                "the portal did not answer in %.0f s -- a consent dialog is "
                "probably waiting for a click" % wait)
        if box["code"] != 0:
            raise PortalRefused(
                "the portal refused (code %d) -- the consent dialog was "
                "dismissed, or the session was cancelled" % box["code"])
        return box["results"]

    # ---- session -----------------------------------------------------------

    def start(self):
        saved = self._load_token()
        if not saved and not self.prompt:
            raise PortalUnavailable(
                "no saved restore token, so this would open a consent dialog. "
                "Grant it once, deliberately:\n"
                "    python3 tools/drive/wayland_driver.py --consent")

        # With a token nothing should ask a human anything, so the wait is
        # short: a dialog here means the grant was revoked. Without one, the
        # wait is for a person.
        wait = 120.0 if not saved else 20.0

        res = self._request(self.rd.CreateSession, timeout=wait,
                            options={"session_handle_token": "fnlsess"})
        self.session = res["session_handle"]

        # persist_mode 2 asks the portal to remember this grant; the token it
        # hands back skips the dialog next time.
        opts = {"types": dbus.UInt32(DEVICE_KEYBOARD | DEVICE_POINTER),
                "persist_mode": dbus.UInt32(2)}
        if saved:
            opts["restore_token"] = saved

        if self.verbose and not saved:
            print("  [portal] a consent dialog is about to appear -- allow it "
                  "once and the token is remembered", flush=True)

        # Which of the two calls refuses a token the portal no longer honours
        # is the implementation's business, so the retry brackets the pair.
        try:
            self._request(self.rd.SelectDevices, self.session, timeout=wait,
                          options=opts)
            res = self._request(self.rd.Start, self.session, "", timeout=wait)
        except PortalUnavailable as e:
            # Not left open: the session is what holds a dialog on screen.
            self.close()
            if not saved:
                raise
            # A refusal is the portal's verdict on the token. Its silence may
            # be a dialog or only a slow desktop, and a token dropped for
            # that is a person's consent asked for again.
            if not self.prompt and not isinstance(e, PortalRefused):
                raise
            self._forget_token()
            if not self.prompt:
                raise PortalUnavailable(
                    "the saved restore token was refused, so the grant behind "
                    "it is gone. Grant it once, deliberately:\n"
                    "    python3 tools/drive/wayland_driver.py --consent")
            return self.start()

        if "restore_token" in res:
            self._save_token(str(res["restore_token"]))
        elif not saved:
            print("  [portal] the portal returned no restore token, so the "
                  "next run will ask again -- persist_mode is not being "
                  "honoured here", flush=True)
        self.devices = int(res.get("devices", 0))
        return self

    def close(self):
        if self.session:
            try:
                obj = self.bus.get_object(BUS_NAME, self.session)
                dbus.Interface(obj, IFACE_SS).Close()
            except Exception:
                pass
            self.session = None

    def _load_token(self):
        try:
            with open(TOKEN_FILE) as f:
                return json.load(f).get("restore_token")
        except Exception:
            return None

    def _save_token(self, tok):
        """Atomically, and loudly if it fails: this file is the next run's
        admission, so a half-written one is a dialog and a silent failure to
        write is a dialog every time.

        Written on every Start, because the API lets the portal replace the
        token even though this desktop's hands the same one back."""
        tmp = TOKEN_FILE + ".new"
        try:
            os.makedirs(STATE_DIR, exist_ok=True)
            with open(tmp, "w") as f:
                json.dump({"restore_token": tok}, f)
            os.replace(tmp, TOKEN_FILE)
        except OSError as e:
            print("  [portal] could not save the restore token to %s (%s) -- "
                  "every run will ask for consent until this is fixed"
                  % (TOKEN_FILE, e), flush=True)
            try:
                os.remove(tmp)
            except OSError:
                pass

    def _forget_token(self):
        """This is "ask again", not "revoke": the *grant* stays in the
        portal's permission store, and only the desktop's privacy settings
        take that back."""
        try:
            os.remove(TOKEN_FILE)
        except OSError:
            pass

    # ---- injection ---------------------------------------------------------

    def rel(self, dx, dy):
        self.rd.NotifyPointerMotion(self.session, {}, float(dx), float(dy))

    def button(self, btn=BTN_LEFT, pressed=True):
        self.rd.NotifyPointerButton(self.session, {}, dbus.Int32(btn),
                                    dbus.UInt32(1 if pressed else 0))

    def key(self, name, pressed=True):
        self.keycode(KEY[name], pressed)

    def keycode(self, code, pressed=True):
        """By evdev code, for a key the table above has no name for."""
        self.rd.NotifyKeyboardKeycode(self.session, {}, dbus.Int32(code),
                                      dbus.UInt32(1 if pressed else 0))

    def tap(self, name, hold=0.05):
        self.key(name, True)
        time.sleep(hold)            # the press itself: a key held this long
        self.key(name, False)

    def scroll(self, steps, axis=0):
        self.rd.NotifyPointerAxisDiscrete(self.session, {}, dbus.UInt32(axis),
                                          dbus.Int32(steps))

    # ---- composited capture ------------------------------------------------

    def screenshot(self, dest):
        """A picture of the screen, through the Screenshot portal.

        What the compositor *did with* the frame, where a capture out of
        the swapchain shows what the client produced. Those can disagree: a
        surface with the wrong window geometry, or one under a panel, draws
        a perfect frame nobody sees.

        A second portal, and one with no restore token: whether it asks is
        the desktop's stored permission."""
        res = self._request(self.ss.Screenshot, "",
                            options={"interactive": False,
                                     "modal": False})
        # A URI, so a space or anything past ASCII in the path is escaped.
        uri = str(res["uri"])
        src = (urllib.parse.unquote(uri[len("file://"):])
               if uri.startswith("file://") else uri)
        subprocess.run(["cp", src, dest], check=True)
        try:
            os.remove(src)
        except Exception:
            pass
        return dest


# ---- steering --------------------------------------------------------------

class Driver:
    """Portal injection steered by the testbed's echoed motion events."""

    # Durations that are the gesture itself, not waits for anything. A
    # button is held as long as a finger holds one; a drag rests before it
    # moves and before it lets go, and moves at a hand's pace, because the
    # compositor follows a drag per motion event and loses one made of jumps.
    BUTTON_HOLD = 0.06
    DRAG_HOLD = 0.08
    DRAG_STEP = 0.02
    # Pauses with nothing to wait on, both in the blind sweep: the pointer
    # is pinned in a corner or returned to an edge, where nothing echoes.
    SWEEP_BEAT = 0.4
    ROW_BEAT = 0.05
    # A push left as steps of this, each landing on an output: weston keeps
    # a motion that ends where no output is on the output it began on, so
    # one push of -30000 from the second output stops at that output's
    # edge, and a sweep that had run onto it never came back to the first.
    LEFT_STEP = 1000
    LEFT_STEPS = 30

    def __init__(self, portal, testbed, slow=False, gain=None, aim=None):
        self.p = portal
        self.s = testbed
        self.at = None            # last known window point
        # How many of the portal's units one window point is worth, asked
        # each time because it can change under a round. 1 on Wayland, where
        # the portal moves in logical pixels; scale/120 for an X window, which
        # is measured in X pixels. move_to() would converge without it and
        # drag() would not, because only its start is aimed.
        self.gain = gain or (lambda: 1.0)
        # A second way to aim, for an environment that has one: called with the
        # window point, it returns where the pointer now is, or None. Tried
        # first, with the echo behind it -- a wine window's titlebar is
        # non-client area and echoes no motion.
        self.aim = aim
        # Every wait here is "how long may the app take to echo this", and a
        # sanitizer build takes several times as long.
        self.slow = slow
        self.probe = 0.25 if slow else 0.05
        self.patience = 3.0 if slow else 1.0

    def _observe(self):
        m = self.s.mark()
        self.p.rel(3, 0)                    # see refresh() on why not 1
        ev = self.s.wait_for("motion", m, timeout=0.6 * self.patience)
        if ev:
            self.at = (float(ev["parts"][1]), float(ev["parts"][2]))
        return ev

    def _left_edge(self):
        """The pointer to the layout's left edge, along the row it is on;
        see LEFT_STEP."""
        for _ in range(self.LEFT_STEPS):
            self.p.rel(-self.LEFT_STEP, 0)

    def _corner(self):
        """The pointer to the layout's top-left corner: up, then left in
        steps, which on weston's outputs sharing a top edge is a corner
        reached from any of them (environment.WaylandBare._corner())."""
        self.p.rel(0, -30000)
        self._left_edge()
        self.p.rel(-30000, -30000)

    def _retrace(self, step, made, end):
        """Back along the row a probe at a time, each step its own probe:
        after a late answer the program has just woken, so a step that
        lands on the window is answered at once, and the window is found
        where the late hit came from rather than by a pass made again --
        in which the program, idle while the pointer crosses nothing, may
        power its GPU off again. Counted from before each step, so the
        step's own echo is the answer and no nudge is left behind."""
        for _ in range(made):
            if time.time() > end:
                break
            m = self.s.mark()
            self.p.rel(-step, 0)
            ev = self.s.wait_for("motion", m, timeout=2 * self.probe)
            if ev:
                self.at = (float(ev["parts"][1]), float(ev["parts"][2]))
                return True
        return False

    def find_window(self, timeout=40.0, step=520, rows=10, cols=13):
        """Serpentine sweep until a motion event appears.

        A probe is a move and a wait of at most `probe` for its echo, so a
        miss is cheap and the sweep stays inside its own deadline.
        The step is well under the window's smaller dimension, so a window
        anywhere is crossed. The pointer is pinned to a corner first; where
        that is does not matter, only that it is the same every time."""
        # Ask first: mutter puts a new window under the pointer, and the
        # sweep is the slow and fallible path, not the normal one.
        if self._observe():
            return self.at

        end = time.time() + timeout
        # Three passes with a longer settle each time: a window that has not
        # finished mapping is swept straight past.
        for attempt in range(3):
            self._corner()
            # No evidence to wait on: the pointer is in a corner, over
            # nothing that echoes, and the window may still be mapping.
            time.sleep(self.SWEEP_BEAT * (1 + attempt))

            stale = False
            for _ in range(rows):
                made = 0                    # probes along this row so far
                for _ in range(cols):
                    if time.time() > end:
                        break
                    m = self.s.mark()
                    self.p.rel(step, 0)
                    made += 1
                    got = self.s.wait_for("motion", m, timeout=self.probe)
                    if got:
                        # Not taken on its word: a program whose GPU has
                        # powered off while the pointer was elsewhere
                        # echoes about 2 s late, so this may answer a
                        # step several back. With the rest of them in, a
                        # nudge says where the pointer is now.
                        self.s.quiet(QUIET * self.patience,
                                     0.5 * self.patience)
                        if (self._observe() or
                                self._retrace(step, made, end)):
                            return self.at
                        # Answered from an earlier row: a pass made again
                        # from the corner, the program now awake.
                        stale = True
                        break
                if stale:
                    break
                self._left_edge()                   # back to the left edge
                self.p.rel(0, step)                 # and down one row
                time.sleep(self.ROW_BEAT)
            if time.time() > end:
                break
        raise RuntimeError(
            "swept the screen and found no window (%d events seen, last %r)"
            % (self.s.mark(), self.s.dump(1)))

    def move_to(self, wx, wy, tries=6, tol=1.5):
        """Put the pointer at window point (wx, wy).

        Steered by the app's own report rather than computed, so it needs no
        knowledge of where the window is, what the surface scale is, or how
        wide the grab ring is -- all three cancel out of a difference between
        two points in the same space."""
        if self.aim:
            m = self.s.mark()
            got = self.aim(wx, wy)
            if got:
                self.at = got
                return self.at
            # An aim that gave up has still moved the pointer, so where it
            # was is no longer where it is: take the echo's last word on it.
            self.s.quiet(QUIET * self.patience, 0.5 * self.patience)
            seen = self.s.since(m, "motion")
            if seen:
                self.at = (float(seen[-1]["parts"][1]),
                           float(seen[-1]["parts"][2]))
        if self.at is None:
            self.find_window()
        for _ in range(tries):
            dx, dy = wx - self.at[0], wy - self.at[1]
            if abs(dx) <= tol and abs(dy) <= tol:
                return self.at
            m = self.s.mark()
            g = self.gain()
            self.p.rel(dx * g, dy * g)
            ev = self.s.wait_for("motion", m, timeout=0.8 * self.patience)
            if not ev:
                # Left the surface: step back towards where we came from and
                # let the next iteration re-observe.
                self.p.rel(-dx * g * 0.5, -dy * g * 0.5)
                if not self._observe():
                    raise RuntimeError("lost the window at (%.0f, %.0f)"
                                       % (wx, wy))
                continue
            self.at = (float(ev["parts"][1]), float(ev["parts"][2]))
        return self.at

    def refresh(self, timeout=None):
        """The window state as of *now*, not as of the last event.

        Testbed.state() is whatever the newest event carried, and an event only
        happens when something happens. So this makes one happen: a nudge,
        whose motion event carries a fresh stamp of everything."""
        # Three pixels one way and *not* back: a move followed at once by its
        # opposite nets to zero and mutter delivers nothing for it. The drift
        # does not matter, since move_to() re-aims from the last report.
        # Retried, with longer waits, before concluding the window has gone:
        # the sweep that follows is far dearer than the patience.
        if timeout is None:
            timeout = 1.5 * self.patience
        for attempt in range(3):
            m = self.s.mark()
            self.p.rel(3 + 3 * attempt, 0)
            ev = self.s.wait_for("motion", m, timeout=timeout * (1 + attempt))
            if ev:
                self.at = (float(ev["parts"][1]), float(ev["parts"][2]))
                return self.s.state()
        # Really gone from under the pointer, which a resize or a restore does.
        self.at = None
        self.find_window()
        return self.s.state()

    def _settled(self, settle):
        """After a click or a drag: until the echo has gone quiet. `settle`
        scales how long it may go on talking before that is given up on."""
        self.s.quiet(QUIET * self.patience, 4 * settle * self.patience)

    def click(self, wx, wy, btn=BTN_LEFT, settle=0.35):
        self.move_to(wx, wy)
        self.p.button(btn, True)
        time.sleep(self.BUTTON_HOLD)
        self.p.button(btn, False)
        self._settled(settle)

    def click_here(self, btn=BTN_LEFT, settle=0.35):
        """Press and release wherever the pointer is: for a captured one,
        which has no place to be moved to."""
        self.p.button(btn, True)
        time.sleep(self.BUTTON_HOLD)
        self.p.button(btn, False)
        self._settled(settle)

    def drag(self, x0, y0, x1, y1, steps=10, btn=BTN_LEFT, settle=0.4):
        """Press at one window point and release at another.

        Deliberately *not* steered between the endpoints: a drag that resizes
        or moves the window changes the mapping under the pointer, so the
        intermediate deltas are open-loop and only the start is aimed."""
        self.move_to(x0, y0)
        self.p.button(btn, True)
        time.sleep(self.DRAG_HOLD)
        g = self.gain()
        for i in range(1, steps + 1):
            self.p.rel((x1 - x0) * g / steps, (y1 - y0) * g / steps)
            time.sleep(self.DRAG_STEP)
        time.sleep(self.DRAG_HOLD)
        self.p.button(btn, False)
        self._settled(settle)
        self.at = None            # the mapping moved; re-observe before aiming

    def scrub(self, x0, y0, dx, dy, seconds, steps=10, btn=BTN_LEFT):
        """Press at a window point and carry it by (dx, dy) and back, again
        and again for that long, then let go: a hand that stays on a border.

        One press for the whole of it, which is the difference from drags
        back to back: on Win32 the window is inside the manager's own loop
        from the press to the release. Every leg out is followed by its leg
        back before the time is looked at, so it ends where it began, and
        the legs are open-loop for drag()'s reason. Returns how many."""
        self.move_to(x0, y0)
        self.p.button(btn, True)
        time.sleep(self.DRAG_HOLD)
        g = self.gain()
        end = time.time() + seconds
        legs = 0
        while time.time() < end:
            for way in (1, -1):
                for _ in range(steps):
                    self.p.rel(way * dx * g / steps, way * dy * g / steps)
                    time.sleep(self.DRAG_STEP)
                legs += 1
        time.sleep(self.DRAG_HOLD)
        self.p.button(btn, False)
        self._settled(0.4)
        self.at = None
        return legs


# ---- the consent, taken deliberately ---------------------------------------
#
# `python3 tools/drive/wayland_driver.py --consent`, once per machine, so
# that the click happens when a person is looking and not in the middle of a
# timed check: the dialog is modal and takes the keyboard, so a round that
# meets one has already lost.

def _main(argv):
    import argparse

    ap = argparse.ArgumentParser(
        prog="wayland_driver.py",
        description="The RemoteDesktop grant every input round needs.")
    ap.add_argument("--consent", action="store_true",
                    help="take the dialog now and remember the token")
    ap.add_argument("--status", action="store_true",
                    help="say whether a token is held, and where")
    ap.add_argument("--forget", action="store_true",
                    help="drop the token so the next run asks again; the "
                         "grant itself is the desktop's to revoke")
    args = ap.parse_args(argv)

    if args.forget:
        Portal(verbose=False)._forget_token()
        print("token dropped: %s" % TOKEN_FILE)
        print("the grant behind it is still in the portal's permission store; "
              "the desktop's privacy settings are what revoke that.")
        return 0

    if args.status or not (args.consent or args.forget):
        held = os.path.exists(TOKEN_FILE)
        print("restore token: %s" % ("held" if held else "none"))
        print("               %s" % TOKEN_FILE)
        if not held:
            print("\nevery input round will skip itself until this is "
                  "granted:\n    python3 tools/drive/wayland_driver.py "
                  "--consent")
        return 0 if held else 1

    try:
        p = Portal(verbose=True, prompt=True).start()
    except PortalUnavailable as e:
        print("consent not granted: %s" % e)
        return 1
    p.close()
    print("granted; token saved to %s" % TOKEN_FILE)
    print("it survives `rm -rf build`, which is the whole point of it not "
          "living there.")
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
