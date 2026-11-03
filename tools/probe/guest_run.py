#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Run one Windows program in the VirtualBox guest and print what it says.

    tools/probe/guest_run.py <exe> <seconds> [--env NAME=VALUE]... [args...]

The program is copied into the guest first and stopped after <seconds>. The
guest relays standard output in delayed chunks, so a run of under about
thirty seconds can print nothing at all.
"""
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))
import vbox_driver as V  # noqa: E402


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        sys.exit(__doc__)
    exe, seconds, rest = argv[0], float(argv[1]), argv[2:]
    env = {"FNL_ECHO_EVENTS": "1"}
    while len(rest) >= 2 and rest[0] == "--env":
        name, _, value = rest[1].partition("=")
        env[name] = value
        rest = rest[2:]

    V.manage(*V.guest_argv("mkdir", "--parents", V.GUEST_DIR)[1:])
    copied = V.manage(*V.guest_argv("copyto", "--target-directory",
                                    V.GUEST_DIR + "\\",
                                    os.path.abspath(exe))[1:], timeout=120)
    if copied.returncode:
        sys.exit("copy failed: " + copied.stderr)

    there = V.GUEST_DIR + "\\" + os.path.basename(exe)
    proc = subprocess.Popen(V.run_argv(there, rest, env),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)

    def pump():
        for line in proc.stdout:
            print(line.rstrip(), flush=True)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    time.sleep(seconds)
    proc.terminate()
    proc.wait(timeout=10)
    reader.join(timeout=3)


if __name__ == "__main__":
    main()
