# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Run tools/drive/pace.c across every environment that will run here, and put
the answers in one table.

    python3 tools/drive/pace.py                 # every target, release
    python3 tools/drive/pace.py x11-vulkan      # just that one
    python3 tools/drive/pace.py --variant tsan  # under a checker
    python3 tools/drive/pace.py --rates 0,15,60 --seconds 8

What it measures, per (target, rate): frames actually presented, and per cent
of one core. The rate is *size requests a second*, asked for by the program's
own thread through fnl_request_size_pt(), and 0 is a window nobody touches --
the floor every other row has to be read against. That window draws nothing
once it has settled (DESIGN *At rest*), so its row reads 0 fps, and its
CPU is what resting costs. A rate is the presents the program counts while
it drives (fnl_frame_stats()), never the --frame-times lines, which a
window resting between sizes does not print.

One table across them all, because a pacing figure on one environment alone
reads as "this client is fast": an environment that paces differently from its
neighbours shows reading across a row and not down a column.

    rate      wayland-vulkan   x11-vulkan       win64-vulkan
    idle        0 fps   0.3%      0 fps   0.3%      0 fps   0.7%
    5          20 fps   1.5%     20 fps   3.2%     19 fps   4.2%
    15         47 fps   3.0%     48 fps   7.8%     46 fps  10.5%

---- what it needs ---------------------------------------------------------

A desktop, like everything else in this directory. `wayland-vulkan` wants a
compositor, `x11-vulkan` an X server (XWayland counts), and `win64-vulkan` wine.
The first two are one tree, linux, told which backend to open
(FNL_BACKEND), as a round's program is; `win64-vulkan` and `win64-d3d12`
are the one Windows tree, win64, told which renderer to draw with
(FNL_RENDERER), and every column's program is told its renderer so. A target
whose binary is missing is skipped and said so; build it first:

    ./build.sh                 # or ./build.sh linux tsan, etc.

---- and what it cannot claim ----------------------------------------------

The CPU figure is utime+stime of the launched process over a fixed window,
read out of /proc. For `win64-vulkan` that process is wine, so the figure
carries wine's overhead too: comparable across *rates* on that target and
only roughly with the other two.

`win64-vulkan-native` is a column for each Windows laptop the harness knows
and each GPU written down for it (native_driver.py): the same program
started on its desktop by the agent there, and the CPU figure the agent's
account of that process. It is the one Windows column with a GPU and a
panel under it. `win64-d3d12-native` is the same program told the other
renderer, titled `d3d12@schenker`. The guest has neither and stays out.

`metal@schenker-mac`, `vulkan@schenker-mac` and `gl@schenker-mac` are the
Mac's (mac_driver.py), taken the same way: the macos tree's program, told
each renderer, started over ssh on its desktop, its CPU the agent's account of
it.

`bare@thinkpad` is a column for each bare X server's machine
(bare_driver.py): the Linux tree's program run on that server as X11, its
output back down the ssh that runs it, its CPU read out of /proc there.
lavapipe draws there, so it is no GPU's figure; it is where a loop nothing
throttles shows. Run it with MESA_VK_WSI_DEBUG=noshm, or the 250 row
exhausts the machine's shared-memory segments (DESIGN *X11*).

`weston@thinkpad` and `kms@thinkpad` are the same machine's other two
rounds: the same program on weston there, told to open Wayland, and told
to open KMS on the bare display, which on lavapipe is the copy path.
`bare-gl@`, `weston-gl@` and `kms-gl@` are the three drawn with OpenGL,
columns only when named (`wayland-gl-bare@pi4`), so a bare sweep keeps
the columns it had; a GPU with no OpenGL 4.1 there wants
LIBGL_ALWAYS_SOFTWARE=1 in front, which a round sets for itself. The
KMS window is its output and refuses every size, so its rows are what
asking costs a window that cannot change, and none of them draws for the
asking.

`wine-bare@`, `wine-weston@` and `wine-cage@` are the Windows tree's
program under wine there, on Xorg, weston and cage, and `d3d12-bare@`,
`d3d12-weston@` and `d3d12-cage@` the same told Direct3D 12, on
vkd3d-proton: copied and started as a round's (DESIGN *Wine on the bare
machine*), their CPU the program's and the wine server's
(measure_bare()). Columns only when named (`win64-vulkan-bare`), as
OpenGL's are. Wine places its window itself, so FNL_DRIVE_BARE_OUTPUT is
not taken there.

A cell is a handful of seconds on a desktop doing other things, so one sweep
is an indication and two sweeps agreeing is a measurement.

---- the two rows after the rates ------------------------------------------

`draw` asks for no sizes and for every frame instead (`fenestal-dev pace
--draw`): each is recorded and presented, so the row is what a frame costs
where the rates are what a size costs. It is the one row the KMS column
draws in -- that window refuses every size -- and so the one that times its
copy path: the readback, the fence it waits on, and the flip.

`border` is the one row the program does not drive itself: the harness
holds a window's bottom-right corner and carries it in and out for the
cell's seconds. That is a hand on the border -- the window server's own
resize, and on Win32 the manager's loop from the press to the release --
where every row above it is the caller asking for sizes. The window is
fenestal-testbed's and not fenestal-dev's: the harness finds a window by the
events it echoes, and a program with no callbacks drains none. So the row
has the testbed's scene in it and is read across, not against the rows above.
It goes through the portal or the agent, so what it times includes them,
and an environment whose border the harness cannot drag has no cell. A bare
sweep has the row; one given --rates has it with --border.
"""

import argparse
import os
import re
import subprocess
import sys
import time

import environment as B
import ledger
from stress import BAR_H, FRAME_AT_REST
from testbed import HZ, OUT, ROOT, cpu_jiffies

# Zero is the floor every other row is read against; the display rate and
# above are where the spacing binds, and the rates between are what an
# interactive drag passes through.
DEFAULT_RATES = [0, 5, 15, 30, 45, 60, 250]

# No GPU ranking, as a round has none (environment.Environment.ECHO): a
# machine with no figures kept calibrates in the first cell's settle and
# drive, which is then the ranking's frames and CPU, and one whose
# calibration outlasts a cell -- weston's five entries, 17.5 s -- is killed
# before it keeps any, so every cell there is the ranking's.
UNRANKED = {"FNL_GPU_RANK": "off"}


def measure(cls, variant, rate, seconds, settle, window, extra):
    """One cell: launch, sample, wait, read the log back.

    The CPU window is taken inside the driven stretch and not over the whole
    run, which also contains the settle.
    """
    exe = os.path.join(ROOT, "build", cls.TREE or cls.NAME, variant,
                       "fenestal-dev" + cls.SUFFIX)
    if not os.path.exists(exe):
        return None, "no %s" % os.path.relpath(exe, ROOT)

    try:
        cls.clear()
        exe = cls.stage(exe, variant)
    except B.Unavailable as e:
        return None, str(e)

    log_path = os.path.join(OUT, "pace-%s-%s-%s.log"
                            % (cls.NAME, variant, rate))
    argv = cls.RUNNER + [exe] + pace_argv(rate, seconds, settle, extra)
    with open(log_path, "w") as log:
        proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT,
                                env=cls.environ(
                                    **UNRANKED, TSAN_OPTIONS="suppressions=%s"
                                    % os.path.join(ROOT, "tsan.supp"),
                                    LSAN_OPTIONS="suppressions=%s"
                                    % os.path.join(ROOT, "lsan.supp")))

        # Both sleeps are the measurement and not waits: into the driven
        # stretch, then the fixed window the CPU is sampled over.
        time.sleep(settle + 1.0)
        j0, t0 = cpu_jiffies(proc.pid), time.time()
        time.sleep(window)
        j1, t1 = cpu_jiffies(proc.pid), time.time()
        try:
            rc = proc.wait(timeout=settle + seconds + 30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            return None, "no exit in %d s, see %s" % (
                settle + seconds + 30, os.path.relpath(log_path, ROOT))

    cpu = None
    if j0 is not None and j1 is not None and t1 > t0:
        cpu = (j1 - j0) / HZ / (t1 - t0) * 100.0

    fps = frame_rate(log_path)
    if rc != 0:
        return None, "exit %d, see %s" % (rc, os.path.relpath(log_path, ROOT))
    if fps is None:
        return None, "no drive count in %s" % os.path.relpath(log_path, ROOT)
    return (fps, cpu), None


def pace_argv(rate, seconds, settle, extra):
    """The program's arguments for one cell, its mode first: a rate of
    sizes, or "draw"."""
    if rate == "draw":
        return ["pace", "0", str(seconds), "--settle", str(settle),
                "--draw"] + extra
    return ["pace", str(rate), str(seconds), "--settle", str(settle)] + extra


# The pace mode's count of what it drove: the presents between the settle's
# end and the last size, and the seconds between.
DRIVE_LINE = re.compile(r"^pace: drive \d+ frames, (\d+) presents, .* "
                        r"in ([\d.]+) s")


def frame_rate(log_path):
    """The presents a second while the program drove, from the count it
    prints when it stops. Not the --frame-times lines: a window that rests
    between two sizes restarts their window at every wake (DESIGN *At
    rest*) and prints none while it is driven, and the lines left are the
    settle's -- which read as 20 fps where the window drew 53."""
    with open(log_path, errors="replace") as f:
        for line in f:
            m = DRIVE_LINE.match(line)
            if m and float(m.group(2)) > 0:
                return int(m.group(1)) / float(m.group(2))
    return None


def pace_device(place, variant, rate):
    """The GPU a column's program was on, off the log of one of its cells:
    both kinds of cell name theirs the same way."""
    try:
        with open(os.path.join(OUT, "pace-%s-%s-%s.log"
                               % (place, variant, rate)),
                  errors="replace") as f:
            return ledger.device(f)
    except OSError:
        return ""


# A table cell: as wide as the longest title, "d3d12@schenker:nvidia".
CELL = "%-23s"


def measure_native(b, variant, rate, seconds, settle, window, extra):
    """measure(), for a program on a laptop: started by the agent on its
    desktop, with its output relayed here and its processor time asked of
    the agent, since /proc has only the relay. `b` is the round's own
    account of the place -- the machine, and the device if it names one --
    and its class says which renderer the Windows tree is told, or which of a
    Mac's trees it is, which is asked the same questions by an agent of its
    own."""
    from windows_driver import Unreachable
    if isinstance(b, B.CocoaNative):
        from mac_driver import Mac as Agent
    else:
        from native_driver import Native as Agent
    cls, host = type(b), b.machine
    exe = os.path.join(ROOT, "build", cls.TREE, variant,
                       "fenestal-dev" + cls.SUFFIX)
    if not os.path.exists(exe):
        return None, "no %s" % os.path.relpath(exe, ROOT)
    log_path = os.path.join(OUT, "pace-%s-%s-%s.log"
                            % (b.NAME, variant, rate))
    try:
        desk = Agent(host)
    except Unreachable as e:
        return None, str(e)
    proc = None
    try:
        desk.kill_stale()
        there = desk.install(exe)
        argv = pace_argv(rate, seconds, settle, extra)
        # What a round on that machine is handed, the renderer it is told
        # among it, and FNL_GPU is the part that matters here: the column
        # is one device's.
        sent = b.forwarded(cls.FORWARD)
        sent.update(UNRANKED)
        sent.update(b.forced())
        if b.gpu:
            sent["FNL_GPU"] = b.gpu
        with open(log_path, "w") as log:
            proc = subprocess.Popen(
                desk.run_argv(there, argv, sent, everything=True),
                stdout=subprocess.DEVNULL, stderr=log)
            desk.run_begin()
            if not desk.find("fenestal-dev"):
                return None, "no window from fenestal-dev pace on %s" % host
            # The monitor asked for, as a round has it.
            want = os.environ.get(cls.WANT_OUTPUT)
            if want:
                there = next((m for m in desk.monitors() if m[0] == want),
                             None)
                if not there:
                    return None, "no monitor %r on %s" % (want, host)
                here = desk.monitor()[0]
                desk.move_window_by(there[2] - here[1], there[3] - here[2])
            # The same two sleeps as measure(), and for the same reason.
            time.sleep(settle + 1.0)
            c0, t0 = desk.cpu_seconds(), time.time()
            time.sleep(window)
            c1, t1 = desk.cpu_seconds(), time.time()
            rc = proc.wait(timeout=seconds + 30)
    except (Unreachable, RuntimeError, subprocess.TimeoutExpired) as e:
        return None, "%s: %s" % (type(e).__name__, e)
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait()
        desk.close()
    fps = frame_rate(log_path)
    if rc != 0:
        return None, "exit %d, see %s" % (rc, os.path.relpath(log_path, ROOT))
    if fps is None:
        return None, "no drive count in %s" % os.path.relpath(log_path, ROOT)
    return (fps, (c1 - c0) / (t1 - t0) * 100.0), None


def measure_bare(b, variant, rate, seconds, settle, window, extra):
    """measure(), for the bare machine: the program copied over and run on
    the server its round has there -- Xorg, weston, or none -- its output
    coming back down the ssh that runs it, and its processor time read out
    of /proc there. An X window opens at the root's corner, with nobody to
    place it, and is moved onto the output FNL_DRIVE_BARE_OUTPUT names;
    weston is shown that output alone, and the KMS program takes it."""
    import bare_driver as D
    log_path = os.path.join(OUT, "pace-%s-%s-%s.log"
                            % (b.NAME, variant, rate))
    try:
        box = D.Bare(b.machine, x=b.FORWARD_X)
    except D.Unreachable as e:
        return None, str(e)
    proc = None
    wine = isinstance(b, B._WineOnBare)
    try:
        b.box = box
        # The tree the machine's processor takes (Bare.tree()).
        try:
            tree = box.tree(b.TREE)
        except D.Unreachable as e:
            return None, str(e)
        exe = os.path.join(ROOT, "build", tree, variant,
                           "fenestal-dev" + b.SUFFIX)
        if not os.path.exists(exe):
            return None, "no %s" % os.path.relpath(exe, ROOT)
        box.kill_stale()
        try:
            b.driver_bind()
        except (D.Unreachable, D.DriverMoveFailed) as e:
            return None, str(e)
        b.bring_up()
        # What a round copies: the tree's program, or for Direct3D 12
        # under wine the directory staged with vkd3d-proton beside it,
        # whose overrides program_env() then sends.
        try:
            exe = b.stage(exe, variant)
        except B.Unavailable as e:
            return None, str(e)
        there = box.install(exe, variant, b.install_dir(tree))
        argv = pace_argv(rate, seconds, settle, extra)
        sent = b.machine_forwarded()
        sent.update(b.program_env())
        sent.update(UNRANKED)
        sent.update(b.forced())
        # A column measures the renderer and GPU it names: a ranked round
        # would drop both and open on the ranking's first entry, which on
        # a ranked machine made `kms-gl` Vulkan's column.
        b.gpu_sent(sent, ranked=False)
        # The checkers' known reports, as a round names them; run_argv()
        # points each at the copy sent there.
        if variant == "tsan":
            sent["TSAN_OPTIONS"] = "suppressions=%s" \
                % os.path.join(ROOT, "tsan.supp")
        if variant == "asan":
            sent["LSAN_OPTIONS"] = "suppressions=%s" \
                % os.path.join(ROOT, "lsan.supp")
        with open(log_path, "w") as log:
            proc = subprocess.Popen(box.run_argv(there, argv, sent),
                                    stdout=log, stderr=subprocess.STDOUT)
            begun = time.time()
            pid = None
            while pid is None and time.time() < begun + settle + 1.0:
                time.sleep(0.2)     # poll: the pid comes down the ssh
                with open(log_path, errors="replace") as f:
                    for ln in f:
                        if ln.startswith(D.PID_TAG):
                            pid = int(ln[len(D.PID_TAG):])
            if pid is None:
                return None, "the program on %s never said its pid" \
                    % b.machine
            # The output asked for, as a round has it: the window opens at
            # the root's corner with nobody to place it, which is whichever
            # output arrange() put there, so it is moved before the settle
            # is out and the cell is that output's.
            want = os.environ.get(B.X11Bare.WANT_OUTPUT)
            if want and wine:
                # Wine reads no position off the server, so a window moved
                # behind its back is not where wine thinks it is
                # (Win32Bare.pin_to_target()).
                return None, ("wine places its own window; %s is not taken "
                              "under wine" % B.X11Bare.WANT_OUTPUT)
            if want and b.FORWARD_X:
                mon = next((m for m in B.parse_monitors(box.monitors())
                            if m[0] == want), None)
                if not mon:
                    return None, "no output %r on %s" % (want, b.machine)
                X = box.server()
                win = X.find_window("Fenestal pace", pid=pid)
                win.configure(x=mon[2], y=mon[3])
                X.d.sync()
            time.sleep(max(0.0, begun + settle + 1.0 - time.time()))
            # Under wine the program and the wine server: the server does
            # the Windows kernel's half of the program's calls -- each
            # message, wait and window change -- so its time is the
            # program's, some 5% beside 75 at 60 sizes a second on the
            # thinkpad. Not the prefix's helpers (explorer.exe,
            # services.exe, winedevice.exe and the rest), other Windows
            # processes, busy at the prefix's start and at 0 to 2% while
            # the program drives, as a laptop's column leaves out its
            # DWM and Explorer.
            pids = [pid]
            if wine:
                pids += [int(p) for p in box.sh(
                    "pgrep -u %s -x 'wineserver(64)?'" % box.uid,
                    check=False).split()]

            def cpu():
                return sum(box.cpu_seconds(p) for p in pids)
            c0, t0 = cpu(), time.time()
            time.sleep(window)
            c1, t1 = cpu(), time.time()
            rc = proc.wait(timeout=seconds + 30)
    except (D.Unreachable, RuntimeError, subprocess.TimeoutExpired) as e:
        return None, "%s: %s" % (type(e).__name__, e)
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait()
        box.close()
        b.box = None
    if wine:
        # ssh's code is the shell's, which waited for the program and
        # said its code (Bare.run_argv()).
        with open(log_path, errors="replace") as f:
            said = [ln[len(D.EXIT_TAG):].strip() for ln in f
                    if ln.startswith(D.EXIT_TAG)]
        rc = int(said[-1]) if said and said[-1].isdigit() else -1
    fps = frame_rate(log_path)
    if rc != 0:
        return None, "exit %d, see %s" % (rc, os.path.relpath(log_path, ROOT))
    if fps is None:
        return None, "no drive count in %s" % os.path.relpath(log_path, ROOT)
    return (fps, (c1 - c0) / (t1 - t0) * 100.0), None


# How far the corner is carried, in points, and in ten steps a leg at the
# driver's pace: a third of a second each way. Inwards, so the window is
# never larger than it opened and no leg runs off a screen the pointer is
# clamped to -- which would shorten the leg back and walk the window away.
BORDER_REACH = (-200, -150)
# Seconds the window is left alone before the press: past the sixty presents
# every estimator in frame.c waits for, twice over.
BORDER_REST = 3.0


FRAME_N = re.compile(r"gpu idle \d+/(\d+),")


def frames_between(b, t0, t1):
    """Frames drawn and the seconds they span, from the --frame-times lines
    that arrived after `t0` and by `t1`: each says how many frames its
    window drew. A window begins where a rest ends, so the first line under
    a press counts from the wake; a rest in the middle prints "at rest"
    lines, which count the frames that only showed the figures, if any, as
    stress.frame_rate() does, and otherwise time with no frames. The line
    that ends a window still open at `t1` comes after it and is left out,
    so the span ends at the last line counted, not at `t1`."""
    with b.testbed.lock:
        timed = [(t, ln) for t, ln in b.testbed.frame_lines
                 if t0 < t <= t1]
    if not timed:
        return 0, 0.0
    frames = 0
    for _, ln in timed:
        m = FRAME_N.search(ln) or FRAME_AT_REST.match(ln)
        frames += int(m.group(1) or 0) if m else 0
    return frames, timed[-1][0] - t0


def measure_border(b, variant, seconds):
    """The border row's cell: the testbed at rest, and a hand on its corner.

    `b` is an environment as a round has one, because this needs what a round
    needs -- the window found, the pointer aimed, the echo read. The rate
    is the frames the --frame-times lines counted under the press, over the
    time they span (frames_between()); the CPU is over the whole gesture,
    the aim and the release with it."""
    if "edge-resize" not in b.CAN:
        return None, "no border the harness can drag"
    # Named as this script's other cells are, and not the round's log,
    # which it would write over: the testbed in that environment is what a
    # stress round starts too.
    b.log_as = "pace-%s-%s-border" % (b.NAME, variant)
    try:
        b.start(variant, args=["--frame-times"])
    except B.Unavailable as e:
        b.log_as = None
        return None, str(e)
    except Exception as e:
        # The program never came up: this cell's failure, and the sweep
        # goes on to the next with the ledger it has.
        for stop in (b.stop, b.close):
            try:
                stop()
            except Exception:
                pass
        b.log_as = None
        return None, "%s: %s" % (type(e).__name__, e)
    try:
        # A hand finds a window that has been there a while, and the row is
        # that window's: one pressed in its first second has no late-latch
        # hold yet, and on Intel draws a frame between every two rebuilds.
        time.sleep(BORDER_REST)
        st = b.refresh()
        w, h = st["area"][0], st["area"][1] + BAR_H
        cpu = b.cpu_seconds
        c0, t0 = cpu(), time.time()
        legs = b.scrub(w - 2, h - 2, BORDER_REACH[0], BORDER_REACH[1],
                       seconds)
        c1, t1 = cpu(), time.time()
        frames, span = frames_between(b, t0, t1)
        device = ledger.device(b.testbed.lines)
    except Exception as e:
        return None, "%s: %s" % (type(e).__name__, e)
    finally:
        for stop in (b.stop, b.close):
            try:
                stop()
            except Exception:
                pass
        b.log_as = None
    # Half the drag at least, or the figure is a few lines' and not the
    # gesture's: a window that rests through most of a press is a finding,
    # and says so here rather than as a rate.
    if not legs or span < seconds / 2:
        return None, ("frame lines over %.1f s of a %.0f s drag"
                      % (span, seconds))
    return (frames / span, (c1 - c0) / (t1 - t0) * 100.0, device), None


def main():
    ap = argparse.ArgumentParser(
        description="Frame pacing across every Fenestal environment that "
                    "runs here")
    ap.add_argument("targets", nargs="*",
                    help="targets to run; default is every one that is built")
    ap.add_argument("--variant", default="release",
                    help="build variant under build/<target> (default release)")
    ap.add_argument("--rates", default=None,
                    help="comma-separated size-request rates; 0 means idle")
    ap.add_argument("--seconds", type=float, default=12.0,
                    help="how long to drive each cell for (default 12)")
    ap.add_argument("--settle", type=float, default=4.0,
                    help="quiet seconds before driving starts (default 4)")
    ap.add_argument("--window", type=float, default=6.0,
                    help="CPU sampling window, inside the drive (default 6)")
    ap.add_argument("--no-late-latch", action="store_true",
                    help="the floor the late-latch numbers are read against")
    ap.add_argument("--border", action="store_true",
                    help="the border row as well, in a sweep given --rates")
    ap.add_argument("--draw", action="store_true",
                    help="the draw row as well, in a sweep given --rates")
    ap.add_argument("--no-vsync", action="store_true",
                    help="vsync off: the draw row alone, the frame rate "
                         "flat out (a rate row with vsync off rests between "
                         "its sizes and reads as nothing)")
    args = ap.parse_args()

    rates = list(DEFAULT_RATES)
    if args.rates:
        rates = [int(r) for r in args.rates.split(",")]
    if args.no_vsync:
        rates = []
    # A row of the same kind as the rates -- a program that drives itself,
    # each column's own cell -- so it is measured among them.
    if args.draw or not args.rates or args.no_vsync:
        rates.append("draw")

    # What runs on this machine, where a cell is a process started here and
    # sampled out of /proc, and each laptop, where it is one the agent
    # starts and samples. Not the guest: a frame rate from a machine that
    # rasterises on its CPU is no figure about the library.
    import functools
    # (the place as a round names it, the column's title, the cell). A
    # laptop's column is titled from the @ on: a machine and a device are
    # already as wide as a cell.
    columns = [(c.NAME, c.NAME, functools.partial(measure, c))
               for c in B.ALL if c.LOCAL]
    # One column for each renderer on each laptop, the renderer ahead of
    # the @ where it is not Vulkan's, whose columns kept the bare name.
    native = []
    for cls, renderer in ((B.Win32Native, ""),
                          (B.Win32NativeD3D12, "d3d12"),
                          (B.CocoaNative, "metal"),
                          (B.CocoaNativeVulkan, "vulkan"),
                          (B.CocoaNativeGL, "gl")):
        here = [b for b in cls.each() if b.gpu or "@" in b.NAME]
        native += here
        columns += [(b.NAME, renderer + b.NAME[b.NAME.index("@"):],
                     functools.partial(measure_native, b)) for b in here]
    # The bare machines, a column for each server a round runs there, titled
    # by it as well as the machine: "@thinkpad" alone would read as one more
    # laptop.
    bare = []
    for cls, title in ((B.X11Bare, "bare"), (B.WaylandBare, "weston"),
                       (B.KmsBare, "kms"), (B.X11GLBare, "bare-gl"),
                       (B.WaylandGLBare, "weston-gl"),
                       (B.KmsGLBare, "kms-gl"),
                       (B.Win32Bare, "wine-bare"),
                       (B.Win32WaylandBare, "wine-weston"),
                       (B.Win32CageBare, "wine-cage"),
                       (B.Win32D3D12Bare, "d3d12-bare"),
                       (B.Win32D3D12WaylandBare, "d3d12-weston"),
                       (B.Win32D3D12CageBare, "d3d12-cage")):
        here = [b for b in cls.each() if "@" in b.NAME]
        bare += here
        columns += [(b.NAME, title + b.NAME[b.NAME.index("@"):],
                     functools.partial(measure_bare, b)) for b in here]
    # OpenGL's bare columns only when named: a default sweep keeps the
    # columns it had. Where the GPU has no OpenGL 4.1 the program says so
    # and exits; LIBGL_ALWAYS_SOFTWARE=1 in front puts it on llvmpipe, as
    # a round does by itself (_SoftGL).
    # Wine's too: six columns more for each machine, more than doubling a
    # bare sweep, and figures about wine's translation, which the
    # library's own columns there are read without.
    named_only = {b.NAME for c in (B.X11GLBare, B.WaylandGLBare,
                                   B.KmsGLBare, B.Win32Bare,
                                   B.Win32WaylandBare, B.Win32CageBare,
                                   B.Win32D3D12Bare, B.Win32D3D12WaylandBare,
                                   B.Win32D3D12CageBare) for b in c.each()}
    # The same places as a round has them, for the row a round's hands make.
    hands = {b.NAME: b for c in B.ALL if c.LOCAL for b in c.each()}
    hands.update((b.NAME, b) for b in native)
    hands.update((b.NAME, b) for b in bare)
    # Chosen as a round's are: an environment, and as much of @machine:device
    # as narrows it.
    wanted = [(title, fn) for place, title, fn in columns
              if (not args.targets and place not in named_only)
              or any(t in (place, place.split("@")[0])
                     or place.startswith(t + ":") for t in args.targets)]
    places = {title: place for place, title, _f in columns}
    if not wanted:
        print("no such target; there is %s"
              % ", ".join(place for place, _t, _f in columns))
        return 1

    extra = (["--no-late-latch"] if args.no_late_latch else []) + \
        (["--no-vsync"] if args.no_vsync else [])

    print("build/<target>/%s, %.0f s a cell, CPU over %.0f s of it%s%s"
          % (args.variant, args.seconds, args.window,
             ", late latching off" if args.no_late_latch else "",
             ", vsync off" if args.no_vsync else ""))
    if os.environ.get("FNL_GPU"):
        print("FNL_GPU=%s, which is then a laptop's one column"
              % os.environ["FNL_GPU"])
    print()
    print("%-8s%s" % ("rate", "".join(CELL % t for t, _f in wanted)))

    skipped, cells = {}, {title: {} for title, _f in wanted}
    for rate in rates:
        row = ["%-8s" % ("idle" if rate == 0 else rate)]
        for target, measure_cell in wanted:
            if target in skipped:
                row.append(CELL % "-")
                continue
            cell, why = measure_cell(args.variant, rate, args.seconds,
                                     args.settle, args.window, extra)
            if cell is None:
                # Skipped for the whole sweep and not retried each rate: the
                # reason does not change between rates.
                skipped[target] = why
                row.append(CELL % "-")
                continue
            fps, cpu = cell
            cells[target][str(rate)] = [
                round(fps, 1), None if cpu is None else round(cpu, 1)]
            row.append(CELL % ("%3.0f fps %5s%%"
                               % (fps, "?" if cpu is None else "%.1f" % cpu)))
        print("".join(row).rstrip())

    # The border row, which a sweep with either option has no column for:
    # the program is the testbed, started as a round starts one, with no
    # options of its own.
    unheld, devices = {}, {}
    if (args.border or not args.rates) and not extra:
        row = ["%-8s" % "border"]
        for target, _f in wanted:
            cell, why = (None, None) if target in skipped else \
                measure_border(hands[places[target]], args.variant,
                               args.seconds)
            if cell is None:
                if why:
                    unheld[target] = why
                row.append(CELL % "-")
                continue
            fps, cpu, devices[target] = cell
            cells[target]["border"] = [round(fps, 1), round(cpu, 1)]
            row.append(CELL % ("%3.0f fps %5.1f%%" % (fps, cpu)))
        print("".join(row).rstrip())

    # A line a column, with what it got before it stopped if it stopped.
    for target, _f in wanted:
        what = {"skipped": skipped[target]} if target in skipped else {}
        if cells[target]:
            # Off a rate cell's log, or the draw row's, which names its
            # device the same way: a vsync-off sweep has nothing else.
            rated = [r for r in cells[target] if r != "border"]
            what.update(cells=cells[target],
                        late_latch=not args.no_late_latch,
                        vsync=not args.no_vsync,
                        device=pace_device(places[target], args.variant,
                                           rated[-1])
                        if rated else devices.get(target, ""),
                        output=os.environ.get(
                            B.CocoaNative.WANT_OUTPUT
                            if places[target].startswith("macos-")
                            else B.Win32Native.WANT_OUTPUT
                            if "-native@" in places[target]
                            else B.X11Bare.WANT_OUTPUT
                            if "-bare@" in places[target]
                            else B.OUTPUT_ENV, ""))
        b = hands.get(places[target])
        ledger.record("pace", ledger.place(places[target]), args.variant,
                      asked=b.gpu if b else places[target].partition(":")[2],
                      **ledger.bound(b), **ledger.wine(b), **what)

    if skipped or unheld:
        print()
        for target, why in skipped.items():
            print("%s: skipped -- %s" % (target, why))
        for target, why in unheld.items():
            print("%s: no border row -- %s" % (target, why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
