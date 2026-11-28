#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A round driven from inside, through the MCP port, on this machine.

    python3 tools/drive/mcp_round.py [--variant release] [x11-vulkan ...]

What stress.py asks of a window server and a desktop's portal -- keys typed
into whatever has the focus, a held input device, a picture off the
compositor -- this asks of the library, which was written to be asked
(src/core/mcp.c): `controls` has the titlebar's own rectangles, `window` the
knobs, `key`, `pointer` and `text` put input where the backend's would
arrive, `capture` is the frame read back from the GPU, and in a Debug tree
`fault` arms a site while the program runs. So a round needs no consent
dialog, takes no input from the user, finds nothing by a constant that the
layout owns, and does not start the program again for each fault.

What it cannot say is what the window server does with the window: a drag
on the border, the manager's minimize, a paste from another program, the
compositor's picture. Those stay in stress.py, which is the round for the
server; this is the round for the library, and about as fast as the program
draws. Layering: the program is the testbed, started here with FNL_MCP
naming a socket under build/drive/.

    x11-vulkan  wayland-vulkan  kms-vulkan   the backend and the renderer
                                              FNL_BACKEND and FNL_RENDERER
                                              name, as a stress round does

A checker variant (`asan`, `tsan`, `gc`) has the validation layer put in
front, proved mapped, and its tree's `fault` tool, which the release tree
does not have: the fault section runs there alone.
"""
import argparse
import io
import os
import re
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import environment  # noqa: E402
import ledger  # noqa: E402
import stress  # noqa: E402
from mcp_check import Port, middle, parse  # noqa: E402
from testbed import OUT, ROOT  # noqa: E402

# Every window of a program opens a port of its own, so FNL_MCP=1 is the
# whole configuration: the first window's socket is fenestal-<pid>.sock and
# the ones after it -2, -3, in the runtime directory. A client drives the
# window it names by connecting to that window's port, and reaches across
# from another's nowhere.
SOCKET_DIR = (os.environ.get("XDG_RUNTIME_DIR") or os.environ.get("TMPDIR")
              or "/tmp")
# The names the antialiasing box gives its rows, by what the echo calls them.
AA_LABELS = {"No AA": "none", "FSAA": "fsaa", "MLAA": "mlaa", "TAAU": "taau"}
# What a checker prints when it found something. A line of these is a failure
# wherever in the log it is, fault chatter or none.
SANITIZER = re.compile(
    r"(ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error:|"
    r"ERROR: LeakSanitizer|Validation (Error|Warning)|VUID-)"
)
TARGETS = {
    "x11-vulkan": ("x11", "vulkan"),
    "wayland-vulkan": ("wayland", "vulkan"),
}
CHECKERS = ("asan", "tsan", "gc")


class Session:
    """The testbed, started with the port open, its stderr kept."""

    def __init__(self, backend, renderer, variant, args=()):
        self.backend, self.renderer, self.variant = backend, renderer, variant
        self.lines = []
        self.lock = threading.Lock()
        self.rc = None
        # Log lines [from, to) a forced fault wrote, which are no complaint.
        self.forced = (0, 0)
        exe = os.path.join(ROOT, "build", "linux", self.variant,
                           "fenestal-testbed")
        if not os.path.exists(exe):
            raise FileNotFoundError("no %s" % os.path.relpath(exe, ROOT))
        try:
            os.makedirs(OUT, exist_ok=True)
        except OSError as e:
            raise RuntimeError("no %s: %s" % (OUT, e)) from e
        env = dict(os.environ, FNL_MCP="1", FNL_BACKEND=self.backend,
                   FNL_RENDERER=self.renderer, FNL_ECHO_EVENTS="1",
                   FNL_GPU_RANK="off")
        if self.variant in CHECKERS:
            env["FNL_VK_VALIDATION"] = "1"
            env.setdefault("VK_LAYER_VALIDATE_SYNC", "1")
            env.setdefault("VK_LAYER_VALIDATE_BEST_PRACTICES", "1")
        if self.variant == "tsan":
            env["TSAN_OPTIONS"] = "suppressions=%s/tsan.supp" % ROOT
            if stress.lavapipe_lib():
                env["TSAN_OPTIONS"] = "suppressions=%s" % stress.lavapipe_supp()
        if self.variant == "asan":
            env["LSAN_OPTIONS"] = "suppressions=%s/lsan.supp" % ROOT
            env.update(stress.lavapipe_pinned())
        self.proc = subprocess.Popen(
            [exe, "--frame-times"] + list(args), env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
            errors="replace")
        threading.Thread(target=self._read, args=(self.proc.stderr,),
                         daemon=True).start()
        path = self.socket()
        for _ in range(300):
            if os.path.exists(path):
                break
            if self.proc.poll() is not None:
                raise RuntimeError("the program ended at once: %s"
                                   % " | ".join(self.tail(3)))
            time.sleep(0.1)
        if not os.path.exists(path):
            raise RuntimeError("no port under %s for pid %d"
                               % (SOCKET_DIR, self.proc.pid))
        time.sleep(0.5)
        self.port = Port(socket_path=path)

    def socket(self, n=1):
        """The nth window's port: the first is the pid's, and the ones after
        it take -2, -3 (mcp_open())."""
        base = os.path.join(SOCKET_DIR, "fenestal-%d" % self.proc.pid)
        return base + ".sock" if n == 1 else "%s-%d.sock" % (base, n)

    def attach(self, n):
        """A port on the nth window of this program: a connection of its
        own, so what it serves is that window and nothing else."""
        path = self.socket(n)
        for _ in range(200):
            if os.path.exists(path):
                return Port(socket_path=path)
            time.sleep(0.05)
        raise RuntimeError("no port for window %d under %s" % (n, SOCKET_DIR))

    def _read(self, pipe):
        for ln in pipe:
            with self.lock:
                self.lines.append(ln.rstrip("\n"))

    def tail(self, n):
        with self.lock:
            return self.lines[-n:]

    def log(self):
        with self.lock:
            return list(self.lines)

    def wait_line(self, pattern, since=0, timeout=5.0):
        """The first line from `since` on that matches, or None."""
        end = time.time() + timeout * (3 if self.variant in CHECKERS else 1)
        while time.time() < end:
            for ln in self.log()[since:]:
                if re.search(pattern, ln):
                    return ln
            if self.proc.poll() is not None:
                break
            time.sleep(0.05)
        return None

    def alive(self):
        return self.proc.poll() is None

    def stop(self, timeout=20.0):
        if self.port:
            try:
                self.port.text("window", close=True)
            except (OSError, ConnectionError, TimeoutError):
                pass
        try:
            self.rc = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.rc = self.proc.wait()
        if self.port:
            self.port.close()
        time.sleep(0.2)
        return self.rc

    # ---- the port -------------------------------------------------------
    def text(self, name, **args):
        return self.port.text(name, **args)

    def state(self, live=False):
        if live:
            return parse(self.text("state", live=True))
        return self.port.state()

    def events(self, wait_ms=0, until=None):
        a = {"wait_ms": wait_ms}
        if until:
            a["until"] = until
        return self.text("events", **a)

    def collect(self, want, timeout=3.0):
        """Every event line until `want(text)` holds, or the timeout: a
        checker's frame is long enough that one batch of events can end
        between a click's press and its release."""
        text = ""
        end = time.time() + timeout * (3 if self.variant in CHECKERS else 1)
        while time.time() < end:
            text += self.events(wait_ms=250)
            if want(text):
                break
        return text

    def controls(self):
        return parse(self.text("controls"))["controls"]

    def control(self, name, label=None):
        for c in self.controls():
            if c["name"] == name and (label is None or c.get("label") == label):
                return c
        return None

    def click(self, c):
        x, y = middle(c["rect_pt"])
        self.text("pointer", action="click", x=x, y=y)

    def tap(self, key):
        self.text("key", key=key)

    def settle(self, what, cond, timeout=5.0):
        """Whether `cond()` holds within the timeout; the wait stretched for
        a checker, as stress.py's patience is."""
        end = time.time() + timeout * (3 if self.variant in CHECKERS else 1)
        while time.time() < end:
            try:
                if cond():
                    return True
            except (KeyError, ValueError):
                pass
            time.sleep(0.05)
        return False

    def capture(self, **a):
        from PIL import Image

        t, png, err = self.port.call("capture", **a)
        if err or not png:
            return None
        im = Image.open(io.BytesIO(png))
        im.load()
        return im


class Round:
    def __init__(self, name, variant):
        self.name, self.variant = name, variant
        self.results = []
        self.sections = []
        self.t = time.time()

    def check(self, what, ok, info=""):
        self.results.append((what, bool(ok)))
        print("%-58s %s%s" % (what, "ok  " if ok else "FAIL",
                              " " + str(info)[:800] if info or not ok else ""),
              flush=True)
        return bool(ok)

    def section(self, name):
        now = time.time()
        if self.sections:
            self.sections[-1][1] = now - self.t
        self.sections.append([name, 0.0])
        self.t = now

    def end(self):
        self.sections[-1][1] = time.time() - self.t


# ---- the sections --------------------------------------------------------


def opening(s, r):
    r.section("opening")
    lines = s.log()
    want = {"x11": "X11", "wayland": "Wayland", "kms": "KMS"}[s.backend]
    r.check("the backend named is the one opened",
            any(ln.startswith("backend: %s" % want) for ln in lines),
            next((ln for ln in lines if ln.startswith("backend:")), "none"))
    rn = {"vulkan": "Vulkan"}[s.renderer]
    r.check("and the renderer",
            any(ln.startswith("renderer: %s" % rn) for ln in lines),
            next((ln for ln in lines if ln.startswith("renderer:")), "none"))
    st = s.state()
    r.check("state names both", st["backend"] == want and st["renderer"] == rn)
    r.check("the window has a size and a content rectangle",
            st["window_pt"][0] > 100 and st["content_rect_pt"][3] > 100,
            "%s %s" % (st["window_pt"], st["content_rect_pt"]))
    if s.variant in CHECKERS:
        # Asked of the library, which knows its own instance: the process's
        # mappings are a question only a client on this machine can ask.
        r.check("the validation layer is in the program",
                s.state().get("validation") is True)
    live = s.state(live=True)
    time.sleep(1.0)
    live = s.state(live=True)
    r.check("the window draws", live["fps"] > 0, "%s fps" % live["fps"])
    # The complaints so far, before anything is forced.
    bad = complaints(s)
    r.check("log is clean at the open", not bad, "; ".join(bad[:3]))


def server_logs(s):
    """What the window server's environment says in a round that went right
    (environment.py's EXPECTED_LOG for the backend)."""
    cls = {"x11": environment.X11,
           "wayland": environment.Wayland}.get(s.backend)
    return cls.EXPECTED_LOG if cls else ()


def complaints(s, since=0):
    """The log's lines that are on no list (stress.expected()), less the
    table LeakSanitizer prints when a suppression matched."""
    out = []
    lines = s.log()[since:]
    skip = set()
    for i, ln in enumerate(lines):
        if (ln == "Suppressions used:" and i
                and stress.DASHES.match(lines[i - 1])):
            j = i + 1
            while j < len(lines) and not stress.DASHES.match(lines[j]):
                j += 1
            skip.update(range(i - 1, j + 1))
    lo, hi = s.forced
    for i, ln in enumerate(lines):
        if lo <= i + since < hi:
            continue
        # The port says who connected to it: not the program's complaint.
        if (ln.strip() and i not in skip and not stress.expected(ln)
                and not ln.startswith("mcp: ")
                and not any(re.search(p, ln) for p in server_logs(s))
                and not stress.FAULT_LINE.match(ln)):
            out.append(ln)
    return out


def keys(s, r):
    r.section("keys")
    s.events()
    s.tap("B")
    ev = s.events(wait_ms=800, until="key B release")
    r.check("a key tapped is echoed",
            "key B press" in ev and "key B release" in ev, ev)
    m = len(s.log())
    s.tap("F2")
    on = s.wait_line(r"^scene motion: on$", m)
    r.check("F2 is the library's: the scene's clock runs", on is not None)
    m = len(s.log())
    s.tap("F2")
    off = s.wait_line(r"^scene motion: off$", m)
    r.check("and stops", off is not None)
    # Text entry is a mode, and Escape is live only while it is off.
    s.text("window", text_input=True)
    s.settle("ti", lambda: s.state()["text_input"] is True)
    s.events()
    t = s.text("text", text="h\u00e9llo \u2713")
    ev = s.events(wait_ms=800)
    r.check("text typed, one event a character",
            t.startswith("typed 7") and ev.count("EV text") == 7,
            "%s, %d events" % (t, ev.count("EV text")))
    s.text("window", text_input=False)
    s.settle("ti", lambda: s.state()["text_input"] is False)


def titlebar(s, r):
    r.section("titlebar")
    ctl = {c["name"] for c in s.controls()}
    r.check("controls lists the library's own", {"vsync", "aa", "size"} <= ctl,
            " ".join(sorted(ctl)))
    # The checkboxes flip what they name.
    for name, read in (("vsync", lambda: s.state()["vsync"]),):
        was = read()
        s.click(s.control(name))
        r.check("a click on %s flips it" % name,
                s.settle(name, lambda: read() != was))
        s.click(s.control(name))
        r.check("and back", s.settle(name, lambda: read() == was))
    fs = s.control("fs_upsample")
    if fs:
        was = fs["on"]
        s.click(fs)
        r.check("Fs Upsample's tick flips",
                s.settle("fs", lambda: s.control("fs_upsample")["on"] != was))
        s.click(s.control("fs_upsample"))
        s.settle("fs", lambda: s.control("fs_upsample")["on"] == was)
    # The antialiasing box: a pick, not a toggle; every row, then the way out.
    box = s.control("aa")
    s.click(box)
    rows = [c for c in s.controls() if c["name"] == "aa_row"]
    labels = [c["label"] for c in rows]
    r.check("the antialiasing box lists its modes",
            labels[:1] == ["No AA"] and len(labels) >= 3, " ".join(labels))
    seen = []
    first = True
    for label in labels[1:] + labels[:1]:
        want = AA_LABELS.get(label, label.lower())
        ok = False
        for attempt in range(2):
            if not first or attempt:
                s.click(s.control("aa"))
            first = False
            s.click(s.control("aa_row", label))
            ok = s.settle("aa", lambda: s.state()["antialias"] == want)
            if ok:
                break
        seen.append(want if ok else "!%s" % want)
    r.check("each row is a pick the state follows",
            not any(x.startswith("!") for x in seen), " ".join(seen))
    # The size box is derived from the output and the floor.
    s.click(s.control("size"))
    sizes = [c for c in s.controls() if c["name"] == "size_row"]
    r.check("the size box lists sizes", len(sizes) >= 2, "%d rows" % len(sizes))
    if sizes:
        label = sizes[0]["label"]
        dims = re.match(r"(\d+)x(\d+)", label)
        if not dims:
            return r.check("a size row names its size", False, label)
        w, h = int(dims.group(1)), int(dims.group(2))
        s.click(sizes[0])
        ok = s.settle("size", lambda: abs(s.state()["content_px"][0] - w) <= 2
                      and abs(s.state()["content_px"][1] - h) <= 2, 8.0)
        r.check("the first row sets that content size", ok,
                "%s for %s" % (s.state()["content_px"], label))
        s.text("window", size={"w": 1280, "h": 720 + 34})
        s.settle("size", lambda: s.state()["content_px"][0] == 1280, 8.0)


def knobs(s, r):
    r.section("window")
    s.text("window", title="mcp-round")
    r.check("the title is set",
            s.settle("t", lambda: s.state()["title"] == "mcp-round"))
    s.text("window", antialias="mlaa")
    r.check("antialias is set",
            s.settle("aa", lambda: s.state()["antialias"] == "mlaa"))
    s.text("window", antialias="none")
    s.text("window", vsync=True)
    r.check("vsync is set", s.settle("v", lambda: s.state()["vsync"] is True))
    s.text("window", vsync=False)
    s.settle("v", lambda: s.state()["vsync"] is False)
    s.text("window", fullscreen=True)
    r.check("fullscreen on",
            s.settle("fs", lambda: s.state()["fullscreen"], 8.0))
    s.text("window", fullscreen=False)
    r.check("and off", s.settle("fs", lambda: not s.state()["fullscreen"], 8.0))
    s.text("window", maximized=True)
    r.check("maximized on", s.settle("mx", lambda: s.state()["maximized"], 8.0))
    s.text("window", maximized=False)
    r.check("and off", s.settle("mx", lambda: not s.state()["maximized"], 8.0))
    live = s.state(live=True)
    r.check("the window still draws", live["fps"] > 0)


def pointer(s, r):
    r.section("pointer")
    s.text("window", release_pointer=True)
    s.settle("cap", lambda: s.state()["pointer_captured"] is False, 8.0)
    st = s.state()
    cx, cy, cw, ch = st["content_rect_pt"]
    x, y = cx + cw // 2, cy + ch // 2

    def presses(text):
        return [ln for ln in text.splitlines() if "button left press" in ln]

    s.events()
    s.text("pointer", action="click", x=x, y=y)
    ev = s.collect(lambda t: "capture 1" in t, 8.0)
    r.check("a click in the scene captures the pointer", "capture 1" in ev,
            ev.splitlines()[-1][:120] if ev else "none")
    s.events()
    s.text("pointer", action="relative", dx=40, dy=30)
    ev = s.collect(lambda t: "relative 40.0 30.0" in t, 5.0)
    r.check("captured motion arrives as relative", "relative 40.0 30.0" in ev,
            ev.splitlines()[-1][:120] if ev else "none")
    s.tap("F9")
    r.check("F9 releases it",
            s.settle("cap", lambda: s.state()["pointer_captured"] is False,
                     8.0))
    s.events()
    s.text("pointer", action="scroll", x=x, y=y, dy=2)
    ev = s.collect(lambda t: "scroll" in t)
    got = sum(float(m) for m in re.findall(r"EV scroll \S+ (\S+)", ev))
    r.check("a scroll of two notches is two notches",
            abs(abs(got) - 2.0) < 0.01, "dy %.2f" % got)

    # Two clicks of their own: the second waits out the double-click
    # interval, so neither carries the run's count.
    s.collect(lambda t: False, 0.4)
    s.events()
    time.sleep(stress.NOT_A_DOUBLE_CLICK)
    s.text("pointer", action="click", x=x, y=y)
    s.text("pointer", action="click", x=x, y=y)
    ev = s.collect(lambda t: len(presses(t)) >= 2)
    got = presses(ev)
    r.check("two clicks apart are not a double-click",
            len(got) == 2 and not any("(click 2)" in ln for ln in got),
            " / ".join(got)[:300] or "no press")
    s.text("window", release_pointer=True)
    time.sleep(stress.NOT_A_DOUBLE_CLICK)
    s.events()
    s.text("pointer", action="click", x=x, y=y, count=2)
    ev = s.collect(lambda t: "(click 2)" in t)
    r.check("count 2 is one", "(click 2)" in ev,
            presses(ev)[-1][:120] if presses(ev) else "no press")
    s.text("window", release_pointer=True)
    s.settle("cap", lambda: s.state()["pointer_captured"] is False, 8.0)


def frames(s, r):
    r.section("capture")
    im = s.capture(max_width=0)
    r.check("capture reads the frame back", im is not None)
    if im is None:
        return
    st = s.state()
    sc, m = st["scale120"] / 120.0, st["grab_margin_px"]
    r.check("it is the window and its grab ring",
            abs(im.size[0] - (round(st["window_pt"][0] * sc) + 2 * m)) <= 3
            and abs(im.size[1] - (round(st["window_pt"][1] * sc) + 2 * m)) <= 3,
            "%s for %s" % (im.size, st["window_pt"]))
    colours = im.convert("RGB").resize((160, 90)).getcolors(1 << 20) or []
    r.check("the scene is drawn: more than a flat colour", len(colours) > 40,
            "%d colours" % len(colours))
    crop = s.capture(max_width=0, region=dict(x=0, y=0, w=200, h=60))
    r.check("capture crops to a region",
            crop is not None and crop.size == (200, 60))


def gpus(s, r):
    r.section("gpus")
    st = s.state()
    listed = st["gpus"]
    home = next((i for i, g in enumerate(listed) if g["current"]), 0)
    others = [i for i, g in enumerate(listed) if i != home
              and not (s.variant == "tsan" and g["kind"] == "software")]
    if not others:
        return r.check("another GPU to move to", True, "one listed")
    for i in others:
        name = listed[i]["name"]
        gb = s.control("gpu")
        s.click(gb)
        row = next((c for c in s.controls() if c["name"] == "gpu_row"
                    and c.get("label", "").startswith("%d " % (i + 1))), None)
        if row is None:
            r.check("the GPU box has a row for %s" % name[:24], False)
            s.click(s.control("gpu"))
            continue
        m = len(s.log())
        s.click(row)
        ok = s.settle("gpu", lambda: s.state()["gpu"] == name, 20.0)
        r.check("the GPU box moves the window to %s" % name[:28], ok,
                s.state()["gpu"])
        r.check("and says it as a move, not a loss",
                s.wait_line(r"^moving to GPU %d" % (i + 1), m, 2.0) is not None
                and not any("device lost" in ln for ln in s.log()[m:]))
        r.check("and a frame reads back from it",
                s.capture(max_width=200) is not None)
        s.text("window", gpu=home)
        s.settle("gpu", lambda: s.state()["gpu"] == listed[home]["name"], 20.0)


def faults(s, r):
    r.section("faults")
    start = len(s.log())
    try:
        s.text("fault", spec="-frame-record")
    except Exception:
        pass
    probe = s.text("fault", spec="-frame-record")
    if "applied" not in probe:
        return r.check("the fault tool is there", False, probe[:100])

    def fire(site, pattern, name, expect=None, wait=10.0):
        m = len(s.log())
        s.text("fault", spec=site)
        names = [x.split("@")[0].split("*")[0] for x in site.split(",")]
        hit = s.wait_line(r"FNL_FAULT: '%s' fired" % names[0], m, wait)
        s.text("fault", spec=",".join("-" + x for x in names))
        ok = hit is not None
        if ok and expect:
            ok = s.wait_line(expect, m, wait) is not None
        r.check(name, ok, hit or "the site never fired")
        s.settle("fps", lambda: s.state(live=True)["fps"] > 0, 10.0)
        r.check("%s: the window draws on" % site.split(",")[0],
                s.alive() and s.state(live=True)["fps"] > 0)
        r.check("%s: and a frame reads back" % site.split(",")[0],
                s.capture(max_width=200) is not None)
        return m

    fire("frame-record", "", "a recording that fails is dropped, not fatal")
    fire("frame-submit", "", "a refused submit is dropped, not fatal")
    fire("device-lost", "", "a lost device is rebuilt", r"rebuilt on")
    fire("surface-lost", "", "a lost surface is rebuilt", r"rebuilt on")
    fire("surface-lost,create-surface*3", "",
         "a surface that will not come back is tried again", r"rebuilt on",
         20.0)
    s.forced = (start, len(s.log()))
    bad = [ln for ln in s.log() if SANITIZER.search(ln)]
    r.check("no checker or validation line through the faults", not bad,
            bad[0] if bad else "")


def second(s, r):
    """The second window, driven through the port of its own: every window of
    a program opens one, so what a client asks is that window and never
    reaches across from the first's."""
    r.section("second window")
    st = s.state()
    r.check("the second window is open", st["windows"] == 2,
            "%d" % st["windows"])
    port = s.attach(2)
    try:
        two = port.state()
        r.check("and has a port of its own",
                two["window_pt"] != st["window_pt"],
                "first %s, second %s" % (st["window_pt"], two["window_pt"]))
        r.check("the second window's state names its own size",
                two["content_rect_pt"][2] > 100 and two["title"] != st["title"],
                "%s %r" % (two["content_rect_pt"], two["title"]))
        # A click through the second's port is the second's capture, and
        # leaves the first's pointer alone.
        cx, cy, cw, ch = two["content_rect_pt"]
        port.text("events")
        port.text("pointer", action="click", x=cx + cw // 2, y=cy + ch // 2)
        got = port.text("events", wait_ms=1500, until="capture 1")
        r.check("a click through the second's port captures there",
                "capture 1" in got, got.splitlines()[-1][:100] if got else "")
        r.check("and the first window's pointer is not captured",
                s.state()["pointer_captured"] is False)
        port.text("window", release_pointer=True)
        # A frame read back through the second's port is the second window.
        _, png, err = port.call("capture", max_width=0)
        if png:
            from PIL import Image
            im = Image.open(io.BytesIO(png))
            im.load()
            sc = two["scale120"] / 120.0
            m = two["grab_margin_px"]
            r.check("a frame through it is the second window",
                    abs(im.size[0] - (round(two["window_pt"][0] * sc) + 2 * m))
                    <= 3, "%s for %s" % (im.size, two["window_pt"]))
        else:
            r.check("a frame through it is the second window", False, "none")
    finally:
        port.close()
    # Hiding it through the first's port frees the window, and its port with
    # it: shown again, the window and its port come back.
    s.tap("F10")
    r.check("F10 hides it", s.settle("w", lambda: s.state()["windows"] == 1))
    s.tap("F10")
    r.check("and shows it again",
            s.settle("w", lambda: s.state()["windows"] == 2))
    port = s.attach(2)
    try:
        r.check("and the shown window has its port again",
                port.state()["windows"] == 2)
        m = len(s.log())
        s.tap("F12")
        ev = s.wait_line(r"^scene view: ", m, 5.0)
        r.check("F12 names the view", ev is not None, ev or "nothing said")
    finally:
        port.close()


def leaving(s, r, base):
    r.section("leaving")
    s.tap("Escape")
    try:
        s.rc = s.proc.wait(timeout=20 * (3 if s.variant in CHECKERS else 1))
    except subprocess.TimeoutExpired:
        s.rc = None
    r.check("exits on Escape", s.rc == 0, "rc=%s" % s.rc)
    if s.rc is None:
        s.stop()
    bad = [ln for ln in s.log() if SANITIZER.search(ln)]
    r.check("no checker or validation line in the whole log", not bad,
            bad[0] if bad else "")
    said = complaints(s, base)
    r.check("log is clean", not said, "; ".join(said[:3]))


# ---- the run ------------------------------------------------------------


def one(name, variant):
    backend, renderer = TARGETS[name]
    r = Round(name, variant)
    try:
        s = Session(backend, renderer, variant)
    except (FileNotFoundError, RuntimeError, OSError) as e:
        print("skipped: %s" % e)
        ledger.record("mcp_round", name, variant, skipped=str(e))
        return None
    base = 0
    try:
        opening(s, r)
        keys(s, r)
        titlebar(s, r)
        knobs(s, r)
        pointer(s, r)
        frames(s, r)
        gpus(s, r)
        if variant in CHECKERS:
            faults(s, r)
        leaving(s, r, base)
        r.end()
        # The second window is a program of its own (--second-window).
        s2 = Session(backend, renderer, variant, ["--second-window"])
        try:
            time.sleep(1.5)
            second(s2, r)
            r.section("leaving")
            s2.tap("Escape")
            try:
                s2.proc.wait(timeout=20 * (3 if variant in CHECKERS else 1))
            except subprocess.TimeoutExpired:
                s2.stop()
            bad = [ln for ln in s2.log() if SANITIZER.search(ln)]
            r.check("the two-window program ends clean",
                    s2.proc.returncode == 0 and not bad,
                    "rc=%s %s" % (s2.proc.returncode, bad[:1]))
        finally:
            if s2.proc and s2.proc.poll() is None:
                s2.stop()
        r.end()
    except Exception as e:  # a round that dies says where, and still reports
        r.check("the round ran to the end", False,
                "%s: %s" % (type(e).__name__, e))
    finally:
        if s.proc and s.proc.poll() is None:
            s.stop()
        path = os.path.join(OUT, "mcp-round-%s-%s.log" % (name, variant))
        with open(path, "w") as f:
            f.write("\n".join(s.log()) + "\n")
    return r


def main():
    ap = argparse.ArgumentParser(description="a round through the MCP port")
    ap.add_argument("targets", nargs="*", help="x11-vulkan, wayland-vulkan "
                    "(default: the ones this desktop has)")
    ap.add_argument("--variant", default="release")
    a = ap.parse_args()
    have = {"x11": "DISPLAY", "wayland": "WAYLAND_DISPLAY"}
    names = a.targets or [n for n, (b, _) in TARGETS.items()
                          if os.environ.get(have[b])]
    bad = 0
    for name in names:
        if name not in TARGETS:
            print("no target %r (%s)" % (name, ", ".join(TARGETS)))
            return 2
        print("\n======== %s / %s (mcp) ========" % (name, a.variant),
              flush=True)
        t0 = time.time()
        r = one(name, a.variant)
        if r is None:
            continue
        failed = [w for w, ok in r.results if not ok]
        print("\n==== %s/%s ====\n%d checks, %d failed, %.1f s" % (
            name, a.variant, len(r.results), len(failed), time.time() - t0))
        print("  " + "  ".join("%s %.1f s" % (n, t) for n, t in r.sections))
        for w in failed:
            print("  FAIL  %s" % w)
        ledger.record("mcp_round", name, a.variant, checks=len(r.results),
                      failed=len(failed), fails=failed,
                      seconds=round(time.time() - t0, 1))
        bad += bool(failed)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
