#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""keytwice.exe under a wine of this desk, typed into through the portal.

    python3 tools/probe/keytwice.py keytwice.exe [--wine devel] [--wayland]
                                    [--raw] [--ime] [--switch]
                                    [--keys A,S,F3]

keytwice.c's other half: the probe started under the wine FNL_DRIVE_WINE's
rules name (--wine; environment.desk_wine(), its prefix too), on wine's X11
driver or with DISPLAY unset on its Wayland one, and each key pressed and
released once through the RemoteDesktop portal, the road the rounds' keys
take. A key is sent only while the probe's last word on the focus is that
it has it -- the evidence the Wayland round types on -- and the run stops
at the first key it cannot say that for. Prints what the window was sent
for each key, and the counts.

--switch types the keys, switches this desktop to the round's German
layout (environment.py's desk_dead_layout(), put back at exit too), types
its dead acute and an E, switches back, and types the keys again: under
wine 11.21's X11 driver the round's keys came once until its first dead
key and twice after it."""
import argparse
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("exe")
    ap.add_argument("--wine", default="")
    ap.add_argument("--wayland", action="store_true")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--ime", action="store_true",
                    help="the probe takes the composition as win32.c does")
    ap.add_argument("--switch", action="store_true",
                    help="type, switch the desk's layout and back, type")
    ap.add_argument("--keys", default="A,S,D,F,F3")
    args = ap.parse_args()
    os.environ["FNL_DRIVE_WINE"] = args.wine
    import environment as B
    import wayland_driver as W
    B.wine_clear()
    wine, pfx = B.desk_wine()
    env = dict(os.environ, WINEDEBUG="-all")
    if pfx:
        env["WINEPREFIX"] = pfx
    if args.wayland:
        env.pop("DISPLAY", None)
    print("%s, %s driver" % (B.wine_says(wine), "Wayland" if args.wayland
                             else "X11"), flush=True)
    portal = W.Portal(verbose=False, prompt=False).start()
    proc = subprocess.Popen([wine, os.path.abspath(args.exe)]
                            + (["raw"] if args.raw else [])
                            + (["ime"] if args.ime else []) + ["40"],
                            stdout=subprocess.PIPE, text=True, env=env)
    lines = []
    reader = threading.Thread(target=lambda: lines.extend(iter(
        proc.stdout.readline, "")), daemon=True)
    reader.start()

    def focused():
        said = [ln for ln in list(lines) if ln.startswith("focus ")]
        return bool(said) and said[-1].strip() == "focus 1"

    try:
        end = time.time() + 10.0
        while not focused() and time.time() < end and proc.poll() is None:
            time.sleep(0.1)
        if not focused():
            print("the window never had the keyboard: no key sent")
            return 1
        time.sleep(0.5)
        keys = args.keys.split(",")
        if args.switch:
            keys = keys + ["switch"] + keys
        for name in keys:
            if name == "switch":
                undo = B.X11().desk_dead_layout()
                try:
                    time.sleep(1.0)
                    for dead in ("Equal", "E"):
                        if not focused():
                            print("the window lost the keyboard: stopped")
                            return 1
                        at = len(lines)
                        portal.tap(dead)
                        time.sleep(0.5)
                        print("  under German, %s:" % dead, flush=True)
                        for ln in lines[at:]:
                            print("    " + ln.rstrip())
                finally:
                    undo()
                time.sleep(1.0)
                print("(the desk's layout back)", flush=True)
                continue
            if not focused():
                print("the window lost the keyboard before %s: stopped"
                      % name)
                return 1
            at = len(lines)
            portal.key(name, True)
            time.sleep(0.05)
            portal.key(name, False)
            time.sleep(0.5)
            got = lines[at:]
            print("%s: %d down, %d up, %d char, %d raw" % (
                name, sum(ln.startswith(("down", "sysdown")) for ln in got),
                sum(ln.startswith(("up", "sysup")) for ln in got),
                sum(ln.startswith("char") for ln in got),
                sum(ln.startswith("raw") for ln in got)), flush=True)
            for ln in got:
                print("    " + ln.rstrip())
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        portal.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
