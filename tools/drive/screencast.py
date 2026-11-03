# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The desktop as the compositor shows it, recorded: a ScreenCast portal
session and GStreamer reading its PipeWire stream into a file.

    python3 tools/drive/screencast.py --consent       # the dialog, once
    python3 tools/drive/screencast.py --status        # whether a token is held
    python3 tools/drive/screencast.py --record 10 out.mkv

What it is for: a window's transient states -- mid-resize, mid-flip -- which
neither the Screenshot portal (two seconds a picture here, so every picture
is of a settled window) nor the X server's picture of an X window (its own
pixels, not what the compositor composes) can catch. A recording is every
frame the compositor drew, at its rate.

A portal of its own, with a consent of its own: the RemoteDesktop grant
(wayland_driver.py) is for input and names no screen. Its restore token is
kept beside that one, in STATE_DIR, and survives `rm -rf build`; with it no
dialog is shown again unless the grant is revoked. Every monitor the dialog
was given is recorded as a stream of its own; record() takes the one asked
for by its position on the desktop, or the first.
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

from testbed import STATE_DIR

BUS_NAME = "org.freedesktop.portal.Desktop"
OBJ_PATH = "/org/freedesktop/portal/desktop"
IFACE_CAST = "org.freedesktop.portal.ScreenCast"
IFACE_RQ = "org.freedesktop.portal.Request"
IFACE_SS = "org.freedesktop.portal.Session"

SOURCE_MONITOR = 1
CURSOR_EMBEDDED = 2         # drawn into the picture, where the drag is
PERSIST_UNTIL_REVOKED = 2

TOKEN_FILE = os.path.join(STATE_DIR, "screencast-token.json")


class CastUnavailable(RuntimeError):
    """No grant, so no recording: a probe that wanted one is skipped."""


class CastRefused(CastUnavailable):
    """The portal answered no -- the one answer that says a saved token is
    no longer honoured. A portal that did not answer in time says nothing
    about the token, which is then kept."""


class Screencast:
    """A started ScreenCast session: its streams, and a way to record one."""

    def __init__(self, prompt=False):
        dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SessionBus()
        self.cast = dbus.Interface(self.bus.get_object(BUS_NAME, OBJ_PATH),
                                   IFACE_CAST)
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")
        self.seq = 0
        # A dialog is acceptable when a person asked for one (--consent);
        # a probe with no token is skipped instead of putting one up.
        self.prompt = prompt
        self.session = None
        self.streams = []           # (node id, (x, y), (w, h))
        self.token_saved = False

    def _request(self, call, *args, timeout=20.0, options=None):
        """As wayland_driver.Portal._request(): subscribed before the call,
        the main loop pumped until the Response."""
        self.seq += 1
        token = "fnlcast%d" % self.seq
        path = "/org/freedesktop/portal/desktop/request/%s/%s" % (self.sender,
                                                                  token)
        done = threading.Event()
        box = {}

        def on_response(code, results):
            box["code"], box["results"] = int(code), results
            done.set()

        match = self.bus.add_signal_receiver(
            on_response, signal_name="Response", dbus_interface=IFACE_RQ,
            path=path)
        opts = dict(options or {})
        opts["handle_token"] = token
        call(*args, opts)
        ctx = GLib.MainContext.default()
        deadline = time.time() + timeout
        while not done.is_set() and time.time() < deadline:
            ctx.iteration(False)
            time.sleep(0.01)        # poll
        match.remove()
        if not done.is_set():
            raise CastUnavailable("the portal did not answer in %.0f s -- a "
                                  "consent dialog is probably waiting"
                                  % timeout)
        if box["code"] != 0:
            raise CastRefused("the portal refused (code %d)" % box["code"])
        return box["results"]

    def start(self):
        saved = _load_token()
        if not saved and not self.prompt:
            raise CastUnavailable(
                "no saved ScreenCast token, so this would open a consent "
                "dialog. Grant it once:\n"
                "    python3 tools/drive/screencast.py --consent")
        wait = 120.0 if not saved else 20.0
        res = self._request(self.cast.CreateSession, timeout=wait,
                            options={"session_handle_token": "fnlcastsess"})
        self.session = res["session_handle"]
        opts = {"types": dbus.UInt32(SOURCE_MONITOR),
                "multiple": True,
                "cursor_mode": dbus.UInt32(CURSOR_EMBEDDED),
                "persist_mode": dbus.UInt32(PERSIST_UNTIL_REVOKED)}
        if saved:
            opts["restore_token"] = saved
        elif self.prompt:
            print("  [screencast] a dialog is about to appear -- choose every "
                  "monitor and allow it; the choice is remembered",
                  flush=True)
        try:
            self._request(self.cast.SelectSources, self.session, timeout=wait,
                          options=opts)
            res = self._request(self.cast.Start, self.session, "",
                                timeout=wait)
        except CastUnavailable as e:
            # Not left open: the session is what holds a dialog on screen.
            self.close()
            if not saved or not isinstance(e, CastRefused):
                raise
            _forget_token()
            if not self.prompt:
                raise CastUnavailable(
                    "the saved ScreenCast token was refused; grant it again:\n"
                    "    python3 tools/drive/screencast.py --consent")
            return self.start()
        self.token_saved = ("restore_token" in res and
                            _save_token(str(res["restore_token"])))
        self.streams = [(int(node), tuple(props.get("position", (0, 0))),
                         tuple(props.get("size", (0, 0))))
                        for node, props in res.get("streams", [])]
        if not self.streams:
            self.close()
            raise CastUnavailable("the portal started a session with no "
                                  "stream")
        return self

    def close(self):
        if self.session:
            try:
                dbus.Interface(self.bus.get_object(BUS_NAME, self.session),
                               IFACE_SS).Close()
            except dbus.DBusException:
                pass
            self.session = None

    def stream_at(self, x, y):
        """The stream whose monitor holds the desktop point (x, y), or the
        first."""
        for s in self.streams:
            (sx, sy), (sw, sh) = s[1], s[2]
            if sx <= x < sx + sw and sy <= y < sy + sh:
                return s
        return self.streams[0]

    def record(self, path, seconds=None, stream=None):
        """Record `stream` (the first by default) into `path`, a Matroska
        file of near-lossless H.264 at the compositor's own rate. With
        `seconds`, for that long; without, until the returned process is
        stopped -- stop() below -- so a probe can record across its own
        work. Returns the process; what gst-launch said is in `path` +
        ".log", a file rather than a pipe that nobody reads while it
        records."""
        node = (stream or self.streams[0])[0]
        fd = self.cast.OpenPipeWireRemote(self.session,
                                          dbus.Dictionary({}, signature="sv"))
        fd = fd.take()
        argv = ["gst-launch-1.0", "-e", "-q",
                "pipewiresrc", "fd=%d" % fd, "path=%d" % node,
                "do-timestamp=true", "always-copy=true",
                "!", "videoconvert",
                "!", "x264enc", "speed-preset=ultrafast", "tune=zerolatency",
                "qp-min=0", "qp-max=8",
                "!", "matroskamux", "!", "filesink",
                "location=%s" % _gst_quote(path)]
        with open(path + ".log", "wb") as log:
            proc = subprocess.Popen(argv, pass_fds=(fd,),
                                    stdout=subprocess.DEVNULL, stderr=log)
        os.close(fd)
        if seconds is not None:
            time.sleep(seconds)
            stop(proc)
        return proc


def _gst_quote(value):
    """A property value as gst_parse_launch reads one: gst-launch joins its
    arguments into one description, so a path with a space or a `!` in it
    is quoted, and a quote or a backslash in it escaped."""
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')


def stop(proc, timeout=10.0):
    """End a recording so the file is whole: -e turns SIGINT into an
    end-of-stream, which the muxer writes out before exiting."""
    if proc.poll() is None:
        proc.send_signal(2)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return proc.returncode


def _load_token():
    try:
        with open(TOKEN_FILE) as f:
            return json.load(f).get("restore_token")
    except (OSError, ValueError):
        return None


def _save_token(tok):
    """Whether the token reached the disk."""
    tmp = TOKEN_FILE + ".new"
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(tmp, "w") as f:
            json.dump({"restore_token": tok}, f)
        os.replace(tmp, TOKEN_FILE)
        return True
    except OSError as e:
        print("  [screencast] could not save the token to %s (%s)"
              % (TOKEN_FILE, e), flush=True)
        return False


def _forget_token():
    try:
        os.remove(TOKEN_FILE)
    except OSError:
        pass


def _main(argv):
    ap = argparse.ArgumentParser(prog="screencast.py",
                                 description=__doc__.split("\n")[0])
    ap.add_argument("--consent", action="store_true",
                    help="take the dialog now and remember the token")
    ap.add_argument("--status", action="store_true",
                    help="say whether a token is held")
    ap.add_argument("--record", nargs=2, metavar=("SECONDS", "FILE"),
                    help="record the first monitor for that long")
    args = ap.parse_args(argv)
    if args.record:
        try:
            cast = Screencast().start()
        except CastUnavailable as e:
            print("no recording: %s" % e)
            return 1
        try:
            for node, pos, size in cast.streams:
                print("stream %d: %dx%d at %d,%d" % (node, size[0], size[1],
                                                     pos[0], pos[1]))
            proc = cast.record(args.record[1], float(args.record[0]))
            with open(args.record[1] + ".log", errors="replace") as f:
                err = f.read().strip()
            if proc.returncode != 0:
                print("gst-launch ended %s: %s" % (proc.returncode, err))
                return 1
            if err:
                print("gst-launch said: %s" % err)
        finally:
            cast.close()
        return 0
    if args.consent:
        try:
            cast = Screencast(prompt=True).start()
        except CastUnavailable as e:
            print("not granted: %s" % e)
            return 1
        cast.close()
        if not cast.token_saved:
            print("granted for this run only: no restore token was saved, "
                  "so the next run will ask again")
            return 1
        print("granted: %d stream(s); token saved to %s"
              % (len(cast.streams), TOKEN_FILE))
        return 0
    held = os.path.exists(TOKEN_FILE)
    print("ScreenCast token: %s\n                  %s"
          % ("held" if held else "none", TOKEN_FILE))
    return 0 if held else 1


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
