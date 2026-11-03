# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The program under test, and the event stream it echoes.

Separate from the driver modules and imported by all of them, because none of
it is any window server's: it starts a process and reads the library's event
echo off its stderr -- one line for every FnlEvent delivered, with the window
state stamped on the end of it:

    EV motion 530.0 454.0 | area 1280x720 fs 0 max 0 cap 0 scale 120 \
                            out HDMI-1 3840x2160

(one line; wrapped here only to fit). `out` is the monitor the window is on
and the size of it, which is the only way anything outside the process can see
a window cross between two of them.

The echo's `area` is fnl_content_size_px(): pixels. Everything a round aims
with or compares against is in logical points -- the titlebar's layout, the
motion the echo reports, the default size -- so an event here carries the
area in points as "area", by the library's own rounding, and what was echoed
as "px". At 100% the two are one number, which is how a round can go a long
time without knowing which it has.

That is a *caller's* view, made by the public calls any program written
against this library would make, and it is the same on every backend: the
window servers disagree about almost everything, and the application's
own report of what it was handed is the one thing they cannot.

Nothing here imports dbus, gi or Xlib, so the paths and the process helpers
every runner needs live here too.
"""

import os
import re
import subprocess
import threading
import time

# From this file's own location, so the harness runs from anywhere.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
# Logs and captures go under build/, which is gitignored.
OUT = os.path.join(ROOT, "build", "drive")
os.makedirs(OUT, exist_ok=True)
# Credentials are not build artefacts, so they live under $XDG_STATE_HOME and
# never under build/: `rm -rf build` would orphan the grant behind the
# portal's token and buy a fresh consent dialog.
STATE_DIR = os.path.join(
    os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state"),
    "fenestal")

HZ = os.sysconf("SC_CLK_TCK")

# How long the echo has to say nothing before what it said is taken as all of
# it. Events that belong together -- a key, what it typed, its release; a
# drag's last resize -- arrive within a frame or two of each other.
QUIET = 0.15
# How often a wait loop looks again.
POLL = 0.01

# Every program this project builds that opens a window titled "Fenestal",
# under the name `pkill -x` knows it by. Linux truncates comm to fifteen
# characters: the wine-hosted `fenestal-dev.exe` is `fenestal-dev.ex`, and
# `fenestal-testbed` loses its last letter either way.
# All of them, whichever one a round drives: a window left up by another is
# one the next round finds.
WINDOW_NAMES = ("fenestal-testbe",
                "fenestal-dev", "fenestal-dev.ex")
_COMM = "|".join(n.replace(".", r"\.") for n in WINDOW_NAMES)
# This account's alone: another user's testbed on the machine is not a
# window a round left up, and not one it may kill.
_MINE = ["-u", str(os.getuid())]


def running():
    """The pids of whatever in WINDOW_NAMES is up."""
    return subprocess.run(["pgrep"] + _MINE + ["-x", _COMM],
                          capture_output=True, text=True).stdout.split()


def kill_stale():
    """Kill anything an earlier run left up; False if there was nothing.

    `-x` matches the process name. `pkill -f` matches command lines, the
    driving shell's among them, and kills the driver. Then a check that they
    went: a round that starts beside a leftover window drives that one."""
    if not running():
        return False
    left = []
    for _ in range(10):
        subprocess.run(["pkill"] + _MINE + ["-x", _COMM],
                       stderr=subprocess.DEVNULL)
        end = time.time() + 0.6
        while time.time() < end:
            left = running()
            if not left:
                return True
            time.sleep(5 * POLL)
    raise RuntimeError("a previous window would not die: %r" % left)


def cpu_jiffies(pid):
    """utime + stime for a process, or None once it has gone.

    Fields 14 and 15 of /proc/<pid>/stat. Split after the *last* ')': field
    2 is the executable name in brackets and may contain spaces."""
    try:
        with open("/proc/%d/stat" % pid) as f:
            raw = f.read()
    except OSError:
        return None
    fields = raw[raw.rindex(")") + 2:].split()
    return int(fields[11]) + int(fields[12])


def ssh_control(ctl, op, dest):
    """`ssh -O op` to the connection held at the control socket `ctl`, and
    whether it said yes. Bounded: a wedged master answers nothing, and the
    drivers ask this in loops and in close()."""
    try:
        return subprocess.run(["ssh", "-S", ctl, "-O", op, dest],
                              capture_output=True,
                              timeout=5).returncode == 0
    except subprocess.TimeoutExpired:
        return False


EV_RE = re.compile(
    r"^EV (?P<body>.*?) \| area (?P<aw>\d+)x(?P<ah>\d+) fs (?P<fs>\d) "
    r"max (?P<max>\d) cap (?P<cap>\d) scale (?P<scale>\d+)"
    # Not \S+ for the name: X11 answers the RandR connector, "HDMI-1", and
    # Wayland wl_output's description, "HDMI-1 (IVM PL4071UH)", spaces and
    # all. Anchoring on the trailing WxH reads both.
    r" out (?P<out>.+) (?P<ow>\d+)x(?P<oh>\d+)$")


class Testbed:
    """The running program, and the event stream it echoes.

    stderr is read on a thread and not polled: the steering loop is "inject,
    then wait for the app to say where that landed", and a line that arrives
    while nobody is reading is a move the loop cannot correct from.
    """

    def __init__(self, argv, env=None, log=None):
        self.log_path = log
        # A byte that does not decode is replaced and never raised: raised,
        # it ends the reader, and every event after it is lost unsaid.
        self.proc = subprocess.Popen(
            argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, errors="replace", bufsize=1, env=env)
        self.events = []
        self.lines = []
        # (time.time(), line) for each --frame-times line, as it arrived:
        # what pace.py's border row counts under the press alone.
        self.frame_lines = []
        self.lock = threading.Lock()
        self._t = threading.Thread(target=self._pump, daemon=True)
        self._t.start()
        # stdout is drained too, to nowhere. A pipe nobody reads fills at
        # 64 KB and the *application* then blocks in write(), which stops its
        # message loop -- and a stalled client is one the window manager
        # resizes without waiting for.
        self._t2 = threading.Thread(target=self._drain, daemon=True)
        self._t2.start()

    def _drain(self):
        try:
            for _ in self.proc.stdout:
                pass
        except Exception:
            pass

    def _pump(self):
        for line in self.proc.stderr:
            line = line.rstrip("\n")
            with self.lock:
                self.lines.append(line)
                if (line.startswith("frame ") and " fps," in line
                        or line.startswith("frame at rest: ")):
                    self.frame_lines.append((time.time(), line))
                m = EV_RE.match(line)
                if m:
                    d = m.groupdict()
                    parts = d["body"].split()
                    s120 = int(d["scale"]) or 120
                    px = (int(d["aw"]), int(d["ah"]))
                    self.events.append({
                        "kind": parts[0], "parts": parts,
                        # px_to_pt(), as src/core/app.h rounds it.
                        "area": tuple((v * 120 + s120 // 2) // s120
                                      for v in px),
                        "px": px,
                        "fs": d["fs"] == "1", "max": d["max"] == "1",
                        "cap": d["cap"] == "1", "scale": int(d["scale"]),
                        "out": d["out"],
                        "output": (int(d["ow"]), int(d["oh"])),
                    })
        self._keep()

    def _keep(self):
        """Write the log out: everything the program has said so far.

        From the pump when stderr closes and from stop() as well, because
        closing is not the program's alone to do: wine's server inherits the
        pipe and holds it open past the harness's own exit, and a log
        written only at the end of the pipe is then never written."""
        if not self.log_path:
            return
        with self.lock:
            lines = list(self.lines)
        try:
            with open(self.log_path, "w") as f:
                f.write("\n".join(lines) + "\n")
        except Exception:
            pass

    def alive(self):
        return self.proc.poll() is None

    def mark(self):
        with self.lock:
            return len(self.events)

    def since(self, mark, kind=None):
        with self.lock:
            evs = self.events[mark:]
        return [e for e in evs if kind is None or e["kind"] == kind]

    def wait_for(self, kind, mark, timeout=1.5):
        end = time.time() + timeout
        while time.time() < end:
            got = self.since(mark, kind)
            if got:
                return got[-1]
            time.sleep(POLL)
        return None

    def quiet(self, pause=QUIET, timeout=2.0):
        """Wait until nothing has been echoed for `pause`. False if the echo
        was still talking at the timeout.

        The wait for "and nothing else": after a key that must type nothing,
        or a drag whose resizes have to be over before the window is read."""
        n, last = self.mark(), time.time()
        end = last + timeout
        while time.time() - last < pause:
            if time.time() >= end:
                return False
            time.sleep(POLL)
            k = self.mark()
            if k != n:
                n, last = k, time.time()
        return True

    def state(self):
        """The newest window state any event carried."""
        with self.lock:
            return dict(self.events[-1]) if self.events else None

    def drained(self, timeout):
        """Wait up to `timeout` seconds for the reader to take what an ended
        program left in the pipe: its last lines are a moment behind its
        exit. Bounded, since wine can hold the pipe open past it."""
        self._t.join(timeout=timeout)

    def stop(self, sig=15):
        if self.alive():
            self.proc.send_signal(sig)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        # Its last lines are a moment behind its exit.
        self._t.join(timeout=1.0)
        self._keep()
        return self.proc.returncode

    def dump(self, n=25):
        with self.lock:
            return self.lines[-n:]
