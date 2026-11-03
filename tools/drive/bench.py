# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Run fenestal-testbed's benchmark mode in the harness's environments and
keep what it records.

    python3 tools/drive/bench.py kms-vulkan-bare@schenker
    python3 tools/drive/bench.py x11-vulkan-bare wayland-vulkan-bare \\
        -- --bench-measure-s=10 --bench-repeats=3 --bench-vsync=off

Each environment's program is started as a round starts it -- its backend
and renderer named, staged or copied to the machine, the server it needs
brought up -- and told `--bench-run --bench-quit --bench-emit` with
whatever follows `--`. The records come back on its stderr, between their
marker lines, which is the one way home every environment has; each is
checked (tools/bench/check.py), written to the state directory's `bench/`
(or --out) and a line goes to the ledger, so status.py shows the latest.
tools/bench/report.py makes the tables.

The program's log is build/drive/bench-<environment>-<variant>.log.

Here are this desk's environments, the bare machines', and the Windows
guests', laptops' and Mac's, whose agents relay the program's stderr home
as a round's start() has it (run_remote()).
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import threading
import time

import environment as B
import ledger
from testbed import OUT, ROOT

sys.path.insert(0, os.path.join(ROOT, "tools", "bench"))
import check  # noqa: E402
import report  # noqa: E402

# The bench's two lines around each measure (bench.c's drive()).
MEASURING = "bench: measuring, "
MEASURED = "bench: measured"
# The hands' pointer motion: a circle this many pixels across, round once
# a second, a step every HAND_STEP -- 100 Hz, a mouse's slowest report.
HAND_RADIUS = 40
HAND_STEP = 0.01

BEGIN = "==== fenestal-bench/1 begin "
END = "==== fenestal-bench/1 end "
FIXED = ["--bench-run", "--bench-quit", "--bench-emit"]


def records(log_path):
    """The documents the program emitted, in order."""
    docs, cur = [], None
    with open(log_path, errors="replace") as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if ln.startswith(BEGIN):
                cur = []
            elif ln.startswith(END):
                if cur is not None:
                    try:
                        docs.append(json.loads("\n".join(cur)))
                    except ValueError as e:
                        print("  a record that does not parse: %s" % e)
                cur = None
            elif cur is not None:
                cur.append(ln)
    return docs


def timeout_for(args):
    """Long enough for every run the arguments ask for, with room for the
    windows to open and a GPU ranking to be measured first."""
    per = 4 + 2 + 20 + 0
    repeats = 1
    for a in args:
        key, _eq, v = a.partition("=")
        try:
            if key == "--bench-repeats":
                repeats = int(v)
            elif key in ("--bench-settle-s", "--bench-warmup-s",
                         "--bench-measure-s", "--bench-cooldown-s"):
                per += int(v) - {"--bench-settle-s": 4,
                                 "--bench-warmup-s": 2,
                                 "--bench-measure-s": 20,
                                 "--bench-cooldown-s": 0}[key]
        except ValueError:
            pass
    return repeats * (per + 30) + 120


class Hands:
    """Pointer motion through every measure the program announces: the
    input the record's latency rows are made from, which a run with
    nothing moving leaves null. Read off the log as it is written, a step
    at a time (step()), so the one who waits for the program also moves
    the pointer and nothing else talks to the machine meanwhile. `lines`
    gives the program's lines said since it was last called; `move` is a
    relative motion in pixels; `aim`, where there is one, puts the pointer
    over the window at each measure's start, for a window the harness has
    to find."""

    def __init__(self, lines, move, aim=None):
        self.lines = lines
        self.move = move
        self.aim = aim
        self.moving = False
        self.phase = 0.0
        self.broken = False

    def step(self):
        """One step, or the lines read for the next measure; how long to
        wait before the next."""
        if self.broken:
            return 0.5
        for ln in self.lines():
            if ln.startswith(MEASURING):
                self.moving = True
                if self.aim:
                    self.aim()
            elif ln.startswith(MEASURED):
                self.moving = False
        if not self.moving:
            return 0.05
        # Along the circle: the difference of two points on it, so the
        # pointer goes round in place and never drifts off.
        nxt = self.phase + 2 * math.pi * HAND_STEP
        dx = HAND_RADIUS * (math.cos(nxt) - math.cos(self.phase))
        dy = HAND_RADIUS * (math.sin(nxt) - math.sin(self.phase))
        self.phase = nxt % (2 * math.pi)
        try:
            self.move(dx, dy)
        except Exception as e:      # whatever it was, the run goes on
            print("  the hands stopped: %s" % e)
            self.broken = True
        return HAND_STEP


def file_lines(path):
    """What a log written as the program speaks has gained since the last
    call, whole lines alone."""
    seen = [0]

    def lines():
        with open(path, "rb") as f:
            f.seek(seen[0])
            chunk = f.read()
        end = chunk.rfind(b"\n") + 1
        seen[0] += end
        return chunk[:end].decode("utf-8", "replace").splitlines()
    return lines


def testbed_lines(tb):
    """The same of a round's Testbed, which keeps them in memory."""
    seen = [0]

    def lines():
        with tb.lock:
            new = tb.lines[seen[0]:]
        seen[0] += len(new)
        return new
    return lines


def hands_thread(h, proc):
    """A bare machine's hands, beside the ssh the program runs in."""
    while proc.poll() is None:
        time.sleep(h.step())


def x11_aim(hands):
    """The pointer onto the program's window, where the X server says that
    is; weston and KMS put it there already (server())."""
    x = getattr(hands, "x", None)
    if x is None or not hasattr(x, "find_window"):
        return None

    def aim():
        try:
            win = x.find_window(pid=None, timeout=20.0)
            wx, wy, ww, wh = x.geom(win)
        except RuntimeError:
            return
        hands.warp(wx + ww // 2, wy + wh // 2)
    return aim


def run_local(cls, variant, args, log_path, with_hands=False):
    """A program on this desk, started as pace.py's measure() starts one.
    No hands here: the portal's go to whatever the desktop has focused,
    which a run that only measures does not check (environment.py's
    keyboard_is_ours() is a round's)."""
    if with_hands:
        return "--hands drives this desk's windows not at all"
    exe = os.path.join(ROOT, "build", cls.TREE or cls.NAME, variant,
                       "fenestal-testbed" + cls.SUFFIX)
    if not os.path.exists(exe):
        return "no %s" % os.path.relpath(exe, ROOT)
    try:
        cls.clear()
        exe = cls.stage(exe, variant)
    except B.Unavailable as e:
        return str(e)
    with open(log_path, "w") as log:
        proc = subprocess.Popen(
            cls.RUNNER + [exe] + FIXED + args, stdout=subprocess.DEVNULL,
            stderr=log, env=cls.environ(
                TSAN_OPTIONS="suppressions=%s"
                % os.path.join(ROOT, "tsan.supp"),
                LSAN_OPTIONS="suppressions=%s"
                % os.path.join(ROOT, "lsan.supp")))
        try:
            proc.wait(timeout=timeout_for(args))
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            return "no end within %d s" % timeout_for(args)
    return None


def run_bare(b, variant, args, log_path, with_hands=False):
    """A program on a bare machine, as pace.py's measure_bare() runs one:
    the server its environment needs brought up, the program copied over,
    its stderr back down the ssh. The records it writes there stay there
    too; the ones that count are the emitted copies."""
    import bare_driver as D
    try:
        box = D.Bare(b.machine, x=b.FORWARD_X)
    except D.Unreachable as e:
        return str(e)
    proc = None
    hands = None
    try:
        b.box = box
        why = box.busy()
        if why:
            return why
        try:
            tree = box.tree(b.TREE)
        except D.Unreachable as e:
            return str(e)
        exe = os.path.join(ROOT, "build", tree, variant, "fenestal-testbed")
        if not os.path.exists(exe):
            return "no %s" % os.path.relpath(exe, ROOT)
        box.kill_stale()
        try:
            b.driver_bind()
        except (D.Unreachable, D.DriverMoveFailed) as e:
            return str(e)
        hands = b.server() if with_hands else None
        if hands is None:
            b.bring_up()
        there = box.install(exe, variant, b.install_dir(tree))
        sent = b.machine_forwarded()
        sent.update(b.program_env())
        sent.update(b.forced())
        # The record is of the device and renderer asked for, under the
        # variant asked for: never where a round's ranking would open, nor
        # a ThreadSanitizer program on NVIDIA's own driver, which the
        # variant left unsaid let gpu_sent() send it to.
        b.variant = variant
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
            proc = subprocess.Popen(
                box.run_argv(there, FIXED + ["--bench-out=/tmp/fenestal-"
                                             "bench"] + args, sent),
                stdout=subprocess.DEVNULL, stderr=log)
            if hands is not None:
                h = Hands(file_lines(log_path), hands.rel, x11_aim(hands))
                threading.Thread(target=hands_thread, args=(h, proc),
                                 daemon=True).start()
            proc.wait(timeout=timeout_for(args))
    # Whatever it was, it is this run's: the next goes on.
    except Exception as e:
        return "%s: %s" % (type(e).__name__, e)
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait()
        if hands is not None:
            hands.close()
        box.close()
        b.box = None
    return None


def run_remote(b, variant, args, log_path, with_hands=False):
    """A program on a Windows guest or laptop, or on the Mac, started as a
    round starts one there (Win32VM.start()): copied over, its stderr
    relayed home by the agent into the round's log, which is copied to
    ours once it has exited. The GPU ranking is left unread, as a round's
    ECHO leaves it: the record is of the device and renderer named."""
    try:
        b.start(variant, args=FIXED + args)
    except B.Unavailable as e:
        return str(e)
    except Exception as e:
        # The program never came up: this run's failure, and the next goes
        # on.
        for stop in (b.stop, b.close):
            try:
                stop()
            except Exception:
                pass
        return "%s: %s" % (type(e).__name__, e)
    h = None
    if with_hands:
        # The agent's relative motion, in the screen's pixels; the pointer
        # put on the window's middle first, each window being a run's.
        def aim():
            # Found again: each run is a window of its own.
            try:
                if not b.portal.find(b.program, timeout=3.0):
                    print("  the hands found no window to aim at")
                    return
                x, y, w, ht = b.rect()
            except (RuntimeError, OSError) as e:
                print("  the hands found no window to aim at: %s" % e)
                return
            b.portal.warp(x + w // 2, y + ht // 2)
        h = Hands(testbed_lines(b.testbed), b.portal.nudge, aim)
    try:
        end = time.time() + timeout_for(args)
        asked = 0.0
        while time.time() < end:
            # Whether it is still there, asked of the agent twice a second
            # and not at every step.
            if time.time() - asked >= 0.5:
                if not b.alive():
                    break
                asked = time.time()
            time.sleep(h.step() if h else 0.5)
        else:
            return "no end within %d s" % timeout_for(args)
    except Exception as e:
        return "%s: %s" % (type(e).__name__, e)
    finally:
        for stop in (b.stop, b.close):
            try:
                stop()
            except Exception:
                pass
    try:
        shutil.copyfile(b.log_path(variant), log_path)
    except OSError as e:
        return "no log from the run: %s" % e
    return None


def keep(doc, place, out_dir):
    """The record written under `out_dir`; its path."""
    run, m = doc["run"], doc["machine"]
    stamp = run["started_utc"].replace("-", "").replace(":", "")[:15] + "Z"
    name = "bench-%s-%s-%s-%s.json" % (
        stamp, m["host"] or "unknown", place.split("@")[0]
        .replace(":", "-"), run["id"])
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name)
    with open(path, "w") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    return path


def summary(doc):
    fr = doc["metrics"]["frame"]
    rate = (fr.get("rate_fps") or {}).get("value")
    iv = fr.get("interval_ms") or {}
    low = (fr.get("low_1pct_fps") or {}).get("value")
    return ("%s, %s fps, p99 %s ms, 1%% low %s"
            % (doc["run"]["status"],
               "-" if rate is None else "%.1f" % rate,
               "-" if "p99" not in iv else "%.2f" % iv["p99"],
               "-" if low is None else "%.0f" % low))


def main():
    argv = sys.argv[1:]
    passed = []
    if "--" in argv:
        passed = argv[argv.index("--") + 1:]
        argv = argv[:argv.index("--")]
    ap = argparse.ArgumentParser(
        description="The testbed's benchmark mode across the environments",
        epilog="Arguments after -- go to the program: --bench-<key>=<v>.")
    ap.add_argument("targets", nargs="*",
                    help="environments, each with as much of "
                         "@machine:device as narrows it; default every one "
                         "here")
    ap.add_argument("--variant", default="release",
                    help="build variant under build/<target> (default "
                         "release)")
    ap.add_argument("--hands", action="store_true",
                    help="move the pointer over the window through each "
                         "measure, for the latency rows (every machine "
                         "but this desk)")
    ap.add_argument("--out", default=report.state_dir(),
                    help="where the records go (default %(default)s)")
    args = ap.parse_args(argv)

    places = [(c.NAME, c, run_local) for c in B.ALL if c.LOCAL]
    for cls in (B.X11Bare, B.WaylandBare, B.KmsBare):
        places += [(b.NAME, b, run_bare) for b in cls.each()
                   if "@" in b.NAME]
    for cls in (B.Win32VM, B.Win32VMD3D12, B.Win32Native,
                B.Win32NativeD3D12, B.CocoaNative, B.CocoaNativeVulkan,
                B.CocoaNativeGL):
        places += [(b.NAME, b, run_remote) for b in cls.each()
                   if "@" in b.NAME]
    wanted = [p for p in places
              if not args.targets
              or any(t in (p[0], p[0].split("@")[0])
                     or p[0].startswith(t + ":") for t in args.targets)]
    if not wanted:
        print("no such target; there is %s"
              % ", ".join(p[0] for p in places))
        return 1

    schema = check.load_schema()
    head, dirty = ledger.commit()
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    for name, what, run in wanted:
        log_path = os.path.join(OUT, "bench-%s-%s.log"
                                % (name.replace(":", "-"), args.variant))
        print("%s:" % name, flush=True)
        began = time.time()
        why = run(what, args.variant, passed, log_path, args.hands)
        if why:
            print("  skipped -- %s" % why)
            ledger.record("bench", ledger.place(name), args.variant,
                          skipped=why, **ledger.bound(what),
                          **ledger.wine(what))
            continue
        docs = records(log_path)
        if not docs:
            print("  no record; see %s" % os.path.relpath(log_path, ROOT))
            bad += 1
            ledger.record("bench", ledger.place(name), args.variant,
                          skipped="no record came back",
                          **ledger.bound(what),
                          **ledger.wine(what))
            continue
        for doc in docs:
            # The program cannot know the commit it was built from; the
            # harness says the tree's, as the ledger's line does.
            doc["run"]["commit"] = head or None
            doc["run"]["dirty"] = dirty if head else None
            found = check.problems(doc, schema)
            if found:
                print("  a record that fails its check: %s" % found[0])
                bad += 1
                continue
            path = keep(doc, name, args.out)
            print("  %s -> %s" % (summary(doc), path))
            fr = doc["metrics"]["frame"]
            ledger.record(
                "bench", ledger.place(name), args.variant,
                asked=getattr(what, "gpu", "") or "",
                device=doc["platform"]["gpu"], file=path,
                status=doc["run"]["status"],
                series=doc["run"]["series"],
                rate_fps=(fr.get("rate_fps") or {}).get("value"),
                p99_ms=(fr.get("interval_ms") or {}).get("p99"),
                vsync=doc["options"].get("vsync", True),
                **ledger.bound(what), **ledger.wine(what))
            bad += doc["run"]["status"] != "complete"
        print("  %.0f s" % (time.time() - began))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
