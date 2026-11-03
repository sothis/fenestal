#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The weston round's window-state run on a bare machine's NVK, timed, up
to where the window stops hearing the pointer.

    FNL_DRIVE_BARE=schenker-live tools/probe/nvk_stall.py [--wayland-debug]

Four fullscreen cycles, maximize, fullscreen and back, restore -- each
step timed beside the GPU's runtime PM state -- then the harness's sweep
for the window traced move by move, weston's scene graph, the program's
threads, three moves into the window's body and a key, each beside the
GPU's runtime PM state: what the time to the first answer after an idle
is made of on NVK (DESIGN *The startup inventory*).

--wayland-debug starts the program under WAYLAND_DEBUG=client, so its log
(build/drive/wayland-vulkan-bare@<machine>-release.log) shows what weston
sent; a MARK-INWINDOW line there is where the moves into the window start.
Mesa's WSI dispatches its own queue's events -- its presentation feedback
among them -- only inside its calls on the render thread, and the log
stamps an event when it is dispatched, not when it was sent. The scene
graphs are build/drive/scene-restored.txt and scene-after.txt.
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "drive"))
import environment as B  # noqa: E402
import bare_driver as D  # noqa: E402
from testbed import OUT  # noqa: E402

PCI = os.environ.get("FNL_PROBE_PCI", "0000:01:00.0")


def main():
    env = {"FNL_GPU": os.environ.get("FNL_GPU", "nvidia")}
    if "--wayland-debug" in sys.argv[1:]:
        env["WAYLAND_DEBUG"] = "client"
    b = B.WaylandBare.each()[0]
    b.start("release", env=env)
    t00 = time.time()

    def pm():
        return b.box.sh("cat /sys/bus/pci/devices/%s/power/runtime_status"
                        % PCI, check=False).strip()

    def scene():
        env = " ".join("%s=%s" % kv for kv in b.box.wenv().items())
        return b.box.sh("env %s timeout 3 weston-debug scene-graph 2>&1"
                        % env, check=False)

    def keep_scene(tag):
        with open(os.path.join(OUT, "scene-%s.txt" % tag), "w") as f:
            f.write(scene())

    def step(tag, f):
        t0 = time.time()
        try:
            f()
            res = "ok"
        except Exception as e:
            res = "%s: %s" % (type(e).__name__, e)
        st = b.testbed.state() or {}
        print("%6.1f %-14s %5.1f s  at %s  area %s  GPU %s  %s"
              % (t0 - t00, tag, time.time() - t0, b.drv.at, st.get("area"),
                 pm(), res), flush=True)

    def f11():
        b.press("F11")
        b.quiet(1.0)

    def click(wx, wy):
        b.click(wx, wy)
        b.quiet(1.0)

    try:
        b.refresh()
        for _ in range(8):
            step("F11", f11)
        w = b.refresh()["area"][0]
        step("maximize", lambda: click(w - 58, 17))
        step("F11", f11)
        step("F11", f11)
        w = b.refresh()["area"][0]
        step("restore", lambda: click(w - 58, 17))
        time.sleep(1.0)
        keep_scene("restored")

        # The sweep, each move and each wait for its echo printed.
        t0 = time.time()
        real_rel, real_wait = b.portal.rel, b.testbed.wait_for

        def rel(dx, dy):
            print("%6.2f rel %d %d" % (time.time() - t0, dx, dy), flush=True)
            real_rel(dx, dy)

        def wait_for(kind, m, timeout=1.5):
            ev = real_wait(kind, m, timeout)
            print("%6.2f   wait %s %.2f -> %s"
                  % (time.time() - t0, kind, timeout,
                     ev and ev["parts"][1:3]), flush=True)
            return ev

        b.portal.rel, b.testbed.wait_for = rel, wait_for
        try:
            print("found", b.drv.find_window(timeout=12))
        except RuntimeError as e:
            print("sweep:", e)
        b.portal.rel, b.testbed.wait_for = real_rel, real_wait
        keep_scene("after")
        cur = re.search(r"wl_pointer-cursor.*\n\s*position: "
                        r"\((-?\d+), (-?\d+)\)", scene())
        print("cursor at", cur and cur.groups())
        print(D._text(D._ssh(D.as_root(b.box.dest),
                             "for t in /proc/%d/task/*; do echo $(cat "
                             "$t/comm) $(cat $t/wchan); done"
                             % b.remote_pid)))

        # Into the window's body from the corner: weston has it at (0, 0).
        # The corner in steps, as the sweep goes: one push from another
        # output stays on that output.
        b.testbed.lines.append("MARK-INWINDOW")
        b.drv._corner()
        time.sleep(0.3)
        for k, (dx, dy) in enumerate([(300, 300), (5, 0), (5, 0)]):
            m = b.mark()
            t1 = time.time()
            real_rel(dx, dy)
            ev = real_wait("motion", m, 5)
            print("in the window %d: %s after %d ms, GPU %s"
                  % (k, ev and ev["parts"][1:3], (time.time() - t1) * 1000,
                     pm()), flush=True)
        m = b.mark()
        b.portal.key("F1", True)
        b.portal.key("F1", False)
        print("key echoed:", bool(real_wait("key", m, 4)))
    finally:
        b.stop()
        b.close()


if __name__ == "__main__":
    main()
