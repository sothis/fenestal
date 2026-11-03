#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Run one Windows program on the laptop's desktop and print what it says.

    tools/probe/native_run.py <exe> <seconds> [--env NAME=VALUE]... [args...]

guest_run.py, for the machine with a GPU: the program is copied over,
started on the desktop by the agent there and stopped after <seconds>, or
sooner if it ends by itself. Standard output and stderr both come back. The
laptop is the first FNL_DRIVE_NATIVE names, or else the first
tools/drive/native_driver.py --setup wrote down.

The library's lines come as they are written. A program's own do not have
to: on Windows both streams are buffered when they are pipes, four
kilobytes at a time, and only what is flushed is sent. So a program with a
window is closed at the end and not killed, and what it had still to say
arrives then.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))
import native_driver as N  # noqa: E402


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        sys.exit(__doc__)
    exe, seconds, rest = argv[0], float(argv[1]), argv[2:]
    env = {}
    while len(rest) >= 2 and rest[0] == "--env":
        name, _, value = rest[1].partition("=")
        env[name] = value
        rest = rest[2:]
    if not N.hosts():
        sys.exit("no laptop is named: FNL_DRIVE_NATIVE, or native_driver.py "
                 "--setup <name>")
    try:
        desk = N.Native(N.hosts()[0])
    except N.Unreachable as e:
        sys.exit(str(e))
    try:
        there = desk.install(exe)
        # Straight through to this terminal: the relay's stderr is the
        # program's two streams.
        proc = subprocess.Popen(desk.run_argv(there, rest, env,
                                              everything=True),
                                stdout=subprocess.DEVNULL)
        desk.run_begin()
        try:
            proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            image = os.path.splitext(os.path.basename(exe))[0]
            try:
                if desk.find(image, timeout=1.0) and desk.close_window():
                    proc.wait(timeout=10)
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
    finally:
        # Closing tells the agent to go, and it stops what it started.
        desk.close()


if __name__ == "__main__":
    main()
