#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Bring the guests up as a round wants them: running in their windows,
logged in, two monitors each, and the four windows tiled on this desktop.

    python3 tools/drive/vbox_up.py            # FNL_DRIVE_VM's machines
    python3 tools/drive/vbox_up.py win11

A machine that is off is started with its window (`startvm --type gui`, not
headless) and waited for until its user is logged on and the desktop has
had a moment. Then Windows is asked how many monitors it shows; one is
answered with the machine's screen layout (vbox_driver.want_monitors()),
and the count before and after is printed, so a machine that came up on one
says so. GUI/LastVisibilityStatusForGuestScreen1 is set on each first: it is
what the VirtualBox window reads to open the second screen's window at all,
and a machine that once had it hidden keeps it hidden.

Then every screen's window on this desktop is looked at, because Windows
showing two monitors is not the window showing the second: after a cold
start VirtualBox can leave that window withdrawn while the guest has the
screen on. One not shown is brought back by rebooting the machine
(shown()), which starts VirtualBox's window afresh and says so. Last,
vbox_tile.py lays the windows out.
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vbox_driver as V  # noqa: E402
import vbox_tile as T  # noqa: E402
from Xlib import X, display  # noqa: E402

# The logon property can say 1 before the desktop is there; a moment more
# is what the agent's first connection wants.
DESKTOP_SETTLE = 20.0
BOOT_TIMEOUT = 400.0


def state(vm):
    for ln in V.manage("showvminfo", vm, "--machinereadable").stdout \
            .splitlines():
        if ln.startswith("VMState="):
            return ln.split("=", 1)[1].strip('"')
    return "unknown"


def logged_in(vm):
    r = V.manage("guestproperty", "get", vm,
                 "/VirtualBox/GuestInfo/OS/LoggedInUsers")
    return r.stdout.strip().lower().endswith(": 1")


def places(vm):
    """{screen: (x, y, w, h)} where the tiler last placed the machine's
    windows: its own key (vbox_tile.PLACE_KEY), and VirtualBox's only for a
    machine never tiled since. VirtualBox's key is whatever the window was
    at its last close -- a window left out of place one round was held
    there the next."""
    got = {}
    for s in (1, 2):
        for key in (T.PLACE_KEY % s,
                    "GUI/LastScaleWindowPosition" + ("" if s == 1 else "1")):
            out = V.manage("getextradata", vm, key).stdout
            try:
                got[s] = tuple(int(v)
                               for v in out.split(":", 1)[1].split(","))
                break
            except (IndexError, ValueError):
                pass
    return got


def open_at(vm, want):
    """VirtualBox's own keys set to `want`, from places(), for the windows
    it opens next."""
    for s, xywh in want.items():
        V.manage("setextradata", vm,
                 "GUI/LastScaleWindowPosition" + ("" if s == 1 else "1"),
                 "%d,%d,%d,%d" % xywh)


def park(d, vm, want, seen):
    """Each of the machine's windows put where the tiler last placed it
    (`want`, from places()), as soon as it is somewhere else.

    VirtualBox opens a window there itself. But Windows turns its second
    screen off while it boots, VirtualBox hides that window and moves it
    while hidden, and at logon shows it again where it moved it, some 150 px
    above its place, for as long as nothing moves it back. So a hidden
    window that has been shown before (`seen`) is moved back before it is
    shown again, and a shown one is moved the moment it is out of place. A
    window not yet shown is left alone: moved while VirtualBox is still
    setting it up, it opened 3000 px left of its place."""
    for (m, s), w in T.guest_windows(d).items():
        if m != vm or s not in want:
            continue
        x, y, cw, ch = want[s]
        try:
            mapped = w.get_attributes().map_state == X.IsViewable
            g = w.get_geometry()
            at = d.screen().root.translate_coords(w, 0, 0)
        except Exception:
            continue                # the window went
        if (at.x, at.y, g.width, g.height) == (x, y, cw, ch):
            if mapped:
                seen.add(s)
            continue
        if mapped:
            seen.add(s)
            T.place(d, w, x, y, cw, ch)
        elif s in seen:
            w.configure(x=x, y=y, width=cw, height=ch)
            d.flush()


def up(vm):
    V.manage("setextradata", vm, "GUI/LastVisibilityStatusForGuestScreen1",
             "true")
    was = state(vm)
    want = places(vm)
    if was not in ("running",):
        open_at(vm, want)
        # Left from the last session when Windows was shut down: read as a
        # logon at once, and the monitors were asked for mid-boot.
        V.manage("guestproperty", "delete", vm,
                 "/VirtualBox/GuestInfo/OS/LoggedInUsers")
        got = V.manage("startvm", vm, "--type", "gui")
        if got.returncode != 0:
            print("%s: would not start: %s" % (vm, got.stderr.strip()))
            return False
    t0 = time.time()
    d = display.Display()
    seen = set()
    try:
        while not logged_in(vm):
            if time.time() - t0 > BOOT_TIMEOUT:
                print("%s: nobody logged on after %.0f s"
                      % (vm, BOOT_TIMEOUT))
                return False
            for _ in range(5):
                park(d, vm, want, seen)
                time.sleep(0.2)     # poll: a window out of place is seen
        if was != "running":
            end = time.time() + DESKTOP_SETTLE
            while time.time() < end:
                park(d, vm, want, seen)
                time.sleep(0.2)     # poll: the window comes back at logon
            print("%s: started, logged on after %.0f s"
                  % (vm, time.time() - t0))
    finally:
        d.close()
    V.use(vm)
    try:
        g = V.Guest()
    except V.Unreachable as e:
        print("%s: %s" % (vm, e))
        return False
    try:
        before = len(g.monitors())
        if before < 2:
            try:
                g.want_monitors(2)
            except V.Unreachable as e:
                print("%s: %s" % (vm, e))
        print("%s: %d monitor(s) at logon, %d now" % (vm, before,
                                                       len(g.monitors())))
    finally:
        g.close()
    return True


def drawn(w):
    """Mapped, and something other than black in it: a window mapped from
    outside VirtualBox's own wish to show it is a frame nothing draws in."""
    if w.get_attributes().map_state != X.IsViewable:
        return False
    g = w.get_geometry()
    try:
        img = w.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
    except Exception:
        return False
    return any(img.data)


def window_process(vm):
    """Whether the machine's VirtualBox window process is running. Anchored
    at the start of the command line, or a shell whose own command names
    the pattern matches itself."""
    return subprocess.run(
        ["pgrep", "-f", r"^\S*VirtualBoxVM --comment %s " % vm],
        capture_output=True).returncode == 0


def windows_drawn(d, vm, n, timeout, want=None):
    """Each of the machine's first `n` windows mapped and drawn within
    `timeout`; the screens still not, as a list. With `want`, from places(),
    each is held there meanwhile (park())."""
    end = time.time() + timeout
    seen = set()
    while True:
        if want:
            park(d, vm, want, seen)
        wins = T.guest_windows(d)
        bad = [s for s in range(1, n + 1)
               if not (wins.get((vm, s)) and drawn(wins[(vm, s)]))]
        if not bad or time.time() >= end:
            return bad
        time.sleep(0.5)             # poll: a window just shown is black


def reboot(vm):
    """The machine shut down by vbox_down.py's way and started again by
    up(): a window process started afresh reads
    GUI/LastVisibilityStatusForGuestScreen1 and opens every window the guest
    has a screen for, and up() puts the windows' places back first.

    Not the state saved and restored, which opens the windows as well with
    no boot: a win11 restored that way refused the foreground to every
    program started on it afterwards -- each round's window opened with its
    taskbar button flashing, and stopped at its first focus check -- until
    it was rebooted. Imported here, since vbox_down imports this module."""
    import vbox_down
    return vbox_down.down(vm) and up(vm)


def shown(vm):
    """Every screen the guest has, with its window on this desktop mapped
    and drawn. One that is not is brought back by reboot(): VirtualBox has
    left the second screen's window withdrawn while Windows showed both
    screens, and neither the layout sent again nor the screen turned off and
    on shows it, nor mapping it from here, after which mutter holds it
    minimised in a frame it never fills. False when the reboot did not bring
    it either."""
    V.use(vm)
    try:
        g = V.Guest()
    except V.Unreachable as e:
        print("%s: %s" % (vm, e))
        return False
    try:
        n = len(g.monitors())
    finally:
        g.close()
    d = display.Display()
    try:
        bad = windows_drawn(d, vm, n, 10)
        if not bad:
            return True
        print("%s: screen(s) %s not shown here; the machine rebooted"
              % (vm, ",".join(map(str, bad))))
        want = places(vm)
        if not reboot(vm):
            return False
        bad = windows_drawn(d, vm, n, 60, want)
        if bad:
            print("%s: screen(s) %s still not shown"
                  % (vm, ",".join(map(str, bad))))
        return not bad
    finally:
        d.close()


def main():
    # As vbox_down.py parses: an option is never a machine's name, and
    # VBoxManage was handed `--help` to start.
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("machines", nargs="*", default=list(V.VMS))
    vms = ap.parse_args().machines
    ok = all([up(vm) for vm in vms])
    time.sleep(2.0)                 # the second screen's window, mapped
    ok = all([shown(vm) for vm in vms]) and ok
    subprocess.run([sys.executable,
                    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "vbox_tile.py")] + list(V.VMS))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
