#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Suspend the laptop, have it wake itself, and watch it come back.

    tools/probe/native_nap.py [--testbed [<exe>] [--gpu <name>]] <nap.exe>
                              <seconds> [<hold>]

nap.c, started on the desktop by the agent there and not over ssh: a
program ssh starts is in the services session, where a request to keep the
display on is accepted and ignored, and a window across a suspend wants a
display to present to. The agent leaves it running (Native.spawn), because
the link is gone as soon as the machine sleeps; its log is read back over
plain ssh, a connection a time, until it says it is done. The laptop is the
first FNL_DRIVE_NATIVE names, or else the first native_driver.py --setup
wrote down.

Refused unless wake timers are allowed on what the machine is running on:
suspended without them it stays asleep until a hand wakes it. Once the hold
is said to have begun, `powercfg /requests` is asked whether it names
nap.exe under SYSTEM and under DISPLAY, and the run says which it does not.

--testbed has a window drawing across the suspend: fenestal-testbed, the release
cross build's unless another is named, started the same way with
--frame-times, so that its log has a line a second while it presents. It
draws with the renderer the library chooses, Direct3D 12 where it can:
the agent's spawn hands a program no environment to name one in. Its
lines are read back beside nap's -- the frame lines counted, anything else
in full -- for LINGER seconds past the wake; then the agent is asked
whether the window is still there, the laptop's screens are captured into
build/drive/, and the testbed is stopped. --gpu puts it on that device, by a
part of its name as the testbed's own --gpu= takes one: --gpu intel.
"""
import ntpath
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))
import native_driver as N  # noqa: E402

# Which source the machine is on, and the wake-timer index for it. The
# setting's two values are matched by their form and not by their words,
# which are the display language's.
WAKE = r"""
$b = Get-CimInstance Win32_Battery
$k = 0; $src = 'mains'
if ($b -and $b.BatteryStatus -eq 1) { $k = 1; $src = 'battery' }
$i = @(powercfg /query SCHEME_CURRENT SUB_SLEEP RTCWAKE |
       Select-String ': 0x([0-9a-fA-F]+)\s*$' |
       ForEach-Object { [Convert]::ToInt32($_.Matches[0].Groups[1].Value, 16) })
$src + ' ' + $i[$k]
"""
# The lines after which nap.c has nothing more to say.
DONE = ("released", "no timer", "this machine will not", "would not suspend",
        "could not hold")
POLL = 5.0
# How long a window is watched past the wake, in seconds.
LINGER = 20.0
HERE = os.path.dirname(os.path.abspath(__file__))
TESTBED = os.path.join(HERE, "..", "..", "build", "win64", "release",
                       "fenestal-testbed.exe")
SHOT = os.path.join(HERE, "..", "..", "build", "drive", "nap-testbed.png")


def say(text):
    print("%s %s" % (time.strftime("%H:%M:%S"), text), flush=True)


def ssh_lines(host, command):
    """A command's output lines, or None when the machine is not reached --
    ssh's own failure, which is 255, and not the command's."""
    try:
        got = subprocess.run(["ssh"] + N.SSH_OPTS + [host, command],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        return None
    if got.returncode == 255:
        return None
    return [ln.rstrip() for ln in got.stdout.splitlines()]


def held(host):
    """The sections of `powercfg /requests` that name nap.exe."""
    lines = ssh_lines(host, "powercfg /requests")
    if lines is None:
        return None
    found, section = set(), None
    for ln in lines:
        if ln and not ln[0].isspace() and ln.endswith(":"):
            section = ln[:-1]
        elif section and "nap.exe" in ln.lower():
            found.add(section)
    return found


class Tail:
    """A log on the laptop, read again from the start each time and said
    from where it was left."""

    def __init__(self, host, path, name=None):
        self.host, self.path, self.name = host, path, name
        self.seen = 0

    def read(self):
        """Every line so far, or None when the machine is not reached;
        says the new ones, frame lines counted for a log with a name."""
        lines = ssh_lines(self.host, 'type "%s"' % self.path)
        if lines is None:
            return None
        new = [ln.strip() for ln in lines[self.seen:] if ln.strip()]
        self.seen = len(lines)
        if self.name is None:
            for ln in new:
                say(ln)
            return lines
        frames = [ln for ln in new if " frame " in ln or
                  ln.startswith("frame ")]
        for ln in new:
            if ln not in frames:
                say("%s: %s" % (self.name, ln))
        if frames:
            fps = frames[-1].split(" fps")[0].rsplit(" ", 1)[-1]
            say("%s: %d frame lines, the last at %s fps"
                % (self.name, len(frames), fps))
        return lines


def look(host, image):
    """After the wake: whether the window is there, and the screens."""
    N.BUSY = 0          # the last input there is nap's, not a hand's
    try:
        desk = N.Native(host)
    except N.Unreachable as e:
        say("the agent is not reached: %s" % e)
        return
    try:
        pid = desk.find(image, timeout=5.0)
        say("the window is %s" % ("there" if pid else "gone"))
        try:
            os.makedirs(os.path.dirname(SHOT), exist_ok=True)
            say("the screens: %s" % os.path.normpath(desk.screenshot(SHOT)))
        except (RuntimeError, ImportError) as e:
            say("no screenshot: %s" % e)
        desk.ask("kill", image)
    finally:
        desk.close()


def main():
    argv = sys.argv[1:]
    testbed, gpu = None, None
    while argv[:1] in (["--testbed"], ["--gpu"]):
        if argv[0] == "--gpu" and len(argv) > 1:
            gpu, argv = argv[1], argv[2:]
            continue
        argv = argv[1:]
        testbed = TESTBED
        if argv and argv[0].endswith(".exe") and len(argv) > 2 \
                and argv[1].endswith(".exe"):
            testbed, argv = argv[0], argv[1:]
    if len(argv) < 2:
        sys.exit(__doc__)
    exe, seconds = argv[0], float(argv[1])
    hold = float(argv[2]) if len(argv) > 2 else 600.0
    if testbed and not os.path.exists(testbed):
        sys.exit("no %s to run -- ./build.sh win64" % testbed)
    if not N.hosts():
        sys.exit("no laptop is named: FNL_DRIVE_NATIVE, or native_driver.py "
                 "--setup <name>")
    host = N.hosts()[0]
    image = ntpath.splitext(os.path.basename(testbed or ""))[0]
    try:
        desk = N.Native(host)
    except N.Unreachable as e:
        sys.exit(str(e))
    try:
        said = N.powershell(host, WAKE, ctl=desk.ctl)
        src, index = said[-1].split() if said else ("?", "0")
        # 2 is "important wake timers only", and an application's is not.
        if index != "1":
            sys.exit("wake timers are not allowed on %s on %s: nothing would "
                     "wake it -- powercfg /set%svalueindex SCHEME_CURRENT "
                     "SUB_SLEEP RTCWAKE 1, then /setactive SCHEME_CURRENT"
                     % (src, host, "ac" if src == "mains" else "dc"))
        tail = None
        if testbed:
            desk.kill_stale()
            log = desk.spawn(desk.install(testbed), ["--frame-times"] +
                             (["--gpu=" + gpu] if gpu else []))
            if not desk.find(image, timeout=20.0):
                desk.ask("kill", image)
                sys.exit("%s opened no window on %s" % (image, host))
            tail = Tail(host, log, "testbed")
            say("%s is drawing on %s; its log is %s" % (image, host, log))
        log = desk.spawn(desk.install(exe), ["%g" % seconds, "%g" % hold])
    finally:
        desk.close()
    say("nap started on the desktop of %s, on %s; the log is %s"
        % (host, src, log))

    end = time.time() + seconds + hold + 300
    nap, reached, checked, over = Tail(host, log), True, False, None
    done = DONE + (("input sent", "no input sent") if hold <= 0 else ())
    while time.time() < end and (over is None or time.time() < over):
        lines = nap.read()
        if lines is None:
            if reached:
                say("not reached")
            reached = False
            time.sleep(POLL)    # poll: the machine says nothing by itself
            continue
        if not reached:
            say("reached again")
        reached = True
        if tail:
            tail.read()
        if not checked and any(ln.startswith("holding") for ln in lines):
            checked = True
            sections = held(host)
            if sections is None:
                say("powercfg /requests: not reached")
            else:
                missing = {"SYSTEM", "DISPLAY"} - sections
                if missing:
                    say("not held: " + ", ".join(sorted(missing)))
                else:
                    say("held: DISPLAY, SYSTEM")
        if over is None and any(ln.startswith(done) for ln in lines):
            if not tail:
                return 0
            over = time.time() + LINGER
        time.sleep(POLL)        # poll: the machine says nothing by itself
    if over is not None:
        look(host, image)
        return 0
    say("nap said nothing to end on in %.0f s" % (seconds + hold + 300))
    if tail:
        look(host, image)
    return 1


if __name__ == "__main__":
    sys.exit(main())
