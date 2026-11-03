#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Shut the guests down cleanly, and with --reboot bring them back as
vbox_up.py does.

    python3 tools/drive/vbox_down.py              # FNL_DRIVE_VM's machines
    python3 tools/drive/vbox_down.py --reboot win11

Windows is asked from inside first: shutdown.exe, run by guest control as
the guest's user, as the driver runs a program. The ACPI power button is
not enough alone -- win11 left it unanswered on an idle desktop for five
minutes, its button set to do nothing -- and is the fallback for a machine
guest control cannot reach. A machine that is still running after both is
reported and left as it is: never powered off, which is a pulled plug.
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vbox_driver as V  # noqa: E402
from vbox_up import state, window_process  # noqa: E402

# Windows takes 20 to 40 s to power off once asked; the rest is margin.
ASKED_TIMEOUT = 120.0
ACPI_TIMEOUT = 120.0


def stopped(vm, timeout):
    """Powered off and its window's process gone: the state says poweroff a
    few seconds before that process lets the machine's session go, and a
    startvm in between is refused as "already locked"."""
    end = time.time() + timeout
    while time.time() < end:
        if state(vm) in ("poweroff", "aborted", "saved") \
                and not window_process(vm):
            return True
        time.sleep(2)               # poll: the state is all there is
    return False


def down(vm):
    if state(vm) != "running":
        print("%s: %s already" % (vm, state(vm)))
        return True
    V.use(vm)
    t0 = time.time()
    try:
        # No argv[0] after "--": guest control passes what follows as the
        # arguments. Waited on, so a refusal is an exit code and not a 0.
        got = subprocess.run(V.guest_argv(
            "run", "--exe", r"C:\Windows\System32\shutdown.exe",
            "--wait-stdout", "--wait-stderr", "--", "/s", "/t", "0"),
            capture_output=True, text=True, timeout=60)
        why = ("exit %d: %s" % (got.returncode, (got.stdout + got.stderr)
                                .strip()) if got.returncode else "")
    except (OSError, subprocess.TimeoutExpired) as e:
        why = str(e)
    if not why and stopped(vm, ASKED_TIMEOUT):
        print("%s: shut down after %.0f s" % (vm, time.time() - t0))
        return True
    print("%s: shutdown.exe did not stop it (%s); the ACPI button"
          % (vm, why or "still running"))
    V.manage("controlvm", vm, "acpipowerbutton")
    if stopped(vm, ACPI_TIMEOUT):
        print("%s: shut down after %.0f s" % (vm, time.time() - t0))
        return True
    print("%s: still running after %.0f s; left as it is"
          % (vm, time.time() - t0))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("machines", nargs="*", default=list(V.VMS))
    ap.add_argument("--reboot", action="store_true",
                    help="start them again with vbox_up.py")
    args = ap.parse_args()
    ok = all([down(vm) for vm in args.machines])
    if not (ok and args.reboot):
        return 0 if ok else 1
    return subprocess.run([sys.executable,
                           os.path.join(os.path.dirname(
                               os.path.abspath(__file__)), "vbox_up.py")]
                          + args.machines).returncode


if __name__ == "__main__":
    sys.exit(main())
