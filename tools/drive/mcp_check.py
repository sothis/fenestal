#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""The MCP port's check, against a program that is running with FNL_MCP set.

    python3 tools/drive/mcp_check.py <name> --socket /run/user/1000/f.sock
    python3 tools/drive/mcp_check.py <name> --command "ssh host fenestal-mcp"

`name` is the environment's, as the ledger keeps it (`kms-vulkan-bare`,
`win64-d3d12-native`, `macos-metal-native`). The way in is a Unix socket on
this machine (`--socket`), or a command that speaks the port's framing on
its stdin and stdout (`--command`): `fenestal-mcp` over ssh is how a program
on another machine, Windows and a Mac included, is reached; it is what an MCP
client's `command` is, and the check is its test too.

The program is the testbed, started by whoever set up the machine, with
`FNL_MCP=1` or a path in its environment and `--text-input` (the testbed
leaves text entry off otherwise). The check drives it as an agent would --
state, keys, text, the titlebar's controls found by `controls` and clicked,
a capture and its geometry, the pointer, the window's knobs, the GPUs on
offer -- and says PASS or FAIL for each. Everything it presses it releases.
`--no-gpu` and `--no-fullscreen` leave out what the machine has no second of.

What it is not: the protocol's edge cases (a batch, a line too long, a
client that goes away), which the harness's fuzz round and a hand with socat
meet; this is what a client needs the port to do, run on each environment.
Needs PIL for the capture's size. A line goes to the ledger when it passes,
as `mcp` with the environment's name.
"""
import argparse
import base64
import io
import json
import os
import select
import shlex
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ledger  # noqa: E402


def parse(text):
    """JSON the port sent, or the port's fault: a reply that is not JSON is
    what the check is for, and says what it was."""
    try:
        return json.loads(text)
    except ValueError as e:
        raise ConnectionError("not JSON (%s): %.120s" % (e, text)) from e


class Port:
    """JSON-RPC lines to the port, over a socket or a command's pipes."""

    def __init__(self, socket_path=None, command=None, timeout=25.0):
        self.timeout = timeout
        self.buf = b""
        self.next_id = 0
        self.proc = None
        self.sock = None
        self.to = self.frm = None
        if socket_path:
            self.sock = socket.socket(socket.AF_UNIX)
            self.sock.settimeout(timeout)
            self.sock.connect(socket_path)
        else:
            self.proc = subprocess.Popen(
                command or [], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL)
            self.to, self.frm = self.proc.stdin, self.proc.stdout
            if not self.to or not self.frm:
                raise OSError("no pipes to %s" % command)
        self.rpc("initialize", {"protocolVersion": "2025-06-18",
                                "capabilities": {},
                                "clientInfo": {"name": "mcp_check",
                                               "version": "1"}})

    def _send(self, data):
        if self.sock:
            self.sock.sendall(data)
        elif self.to:
            self.to.write(data)
            self.to.flush()

    def _readline(self):
        end = time.time() + self.timeout
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("the port said nothing in %g s"
                                   % self.timeout)
            if self.sock:
                self.sock.settimeout(left)
                data = self.sock.recv(1 << 20)
            elif self.frm:
                if not select.select([self.frm], [], [], left)[0]:
                    continue
                data = os.read(self.frm.fileno(), 1 << 20)
            else:
                data = b""
            if not data:
                raise ConnectionError("the port closed")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace")

    def rpc(self, method, params=None):
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        if params is not None:
            msg["params"] = params
        self._send((json.dumps(msg) + "\n").encode())
        return parse(self._readline())

    def call(self, name, **args):
        """The tool's content: its text, its image (PNG bytes) and whether it
        failed."""
        reply = self.rpc("tools/call", {"name": name, "arguments": args})
        if "error" in reply:
            return "RPC error: %s" % reply["error"], None, True
        res = reply["result"]
        text = " | ".join(c["text"] for c in res["content"]
                          if c["type"] == "text")
        img = next((base64.b64decode(c["data"]) for c in res["content"]
                    if c["type"] == "image"), None)
        return text, img, bool(res["isError"])

    def text(self, name, **args):
        return self.call(name, **args)[0]

    def state(self):
        return parse(self.text("state"))

    def close(self):
        if self.sock:
            self.sock.close()
        elif self.proc and self.to:
            self.to.close()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Check:
    def __init__(self):
        self.results = []

    def __call__(self, what, ok, info=""):
        self.results.append((what, bool(ok)))
        print("%s %s%s" % ("PASS" if ok else "FAIL", what,
                           "" if ok or not info else "  [%s]" % info[:160]),
              flush=True)


def middle(rect):
    return rect[0] + rect[2] / 2, rect[1] + rect[3] / 2


def run(port, gpu=True, fullscreen=True):
    ok = Check()
    st = port.state
    port.text("events")

    def control(name):
        for c in parse(port.text("controls"))["controls"]:
            if c["name"] == name:
                return c
        return None

    s = st()
    ok("state has what the drop-downs list",
       all(k in s for k in ("backend", "renderer", "gpu", "gpus", "outputs",
                            "modes", "title", "pointer_pt",
                            "grab_margin_px")))

    port.text("key", key="B")
    r = port.text("events", wait_ms=800)
    ok("a key tapped is echoed", "key B press" in r and "key B release" in r,
       r)
    port.text("key", key="Ctrl", action="press")
    port.text("key", key="Ctrl", action="release")
    r = port.text("events", wait_ms=800)
    ok("a modifier pressed and released", "LCtrl press" in r
       and "LCtrl release" in r, r)

    t = port.text("text", text="h\u00e9llo \u2713")
    r = port.text("events", wait_ms=800)
    ok("text typed, one event a character",
       t.startswith("typed 7") and r.count("EV text") == 7, "%s %d" % (
           t, r.count("EV text")))

    box = control("fsaa")
    if box:
        x, y = middle(box["rect_pt"])
        port.text("pointer", action="click", x=x, y=y)
        time.sleep(0.8)
        ok("a click on a control from `controls`",
           st()["antialias"] == "fsaa", st()["antialias"])
        port.text("pointer", action="click", x=x, y=y)
        time.sleep(0.8)
        ok("and back", st()["antialias"] == "none")
    else:
        ok("controls lists the antialiasing box", False)
    port.text("window", antialias="mlaa")
    time.sleep(0.8)
    ok("window sets the antialiasing", st()["antialias"] == "mlaa")
    port.text("window", antialias="none")
    time.sleep(0.5)

    text, png, err = port.call("capture", max_width=0)
    ok("capture", png and not err, text)
    if png:
        from PIL import Image
        im = Image.open(io.BytesIO(png))
        im.load()
        s = st()
        sc, m = s["scale120"] / 120.0, s["grab_margin_px"]
        ok("the frame is the window and its grab ring",
           abs(im.size[0] - (round(s["window_pt"][0] * sc) + 2 * m)) <= 3
           and abs(im.size[1] - (round(s["window_pt"][1] * sc) + 2 * m)) <= 3,
           "%s for %s at %g, ring %d" % (im.size, s["window_pt"], sc, m))
        _, crop, _ = port.call("capture", max_width=0,
                               region=dict(x=0, y=0, w=200, h=60))
        ok("capture crops to a region",
           crop and Image.open(io.BytesIO(crop)).size == (200, 60))

    port.text("pointer", action="move", x=300, y=300)
    time.sleep(0.5)
    p = st()["pointer_pt"]
    ok("the pointer is where it was put", abs(p[0] - 300) < 3
       and abs(p[1] - 300) < 3, str(p))

    port.text("events")
    port.text("pointer", action="click", x=300, y=300)
    port.text("pointer", action="click", x=300, y=300)
    r = port.text("events", wait_ms=800)
    ok("two clicks are not a double-click",
       "(click 2)" not in r and r.count("press") == 2, r)
    port.text("window", release_pointer=True)
    port.text("pointer", action="click", x=300, y=300, count=2)
    r = port.text("events", wait_ms=800)
    ok("count 2 is one", "(click 2)" in r, r)
    port.text("window", release_pointer=True)
    port.text("pointer", action="scroll", x=300, y=300, dy=2)
    ok("a scroll arrives", "scroll" in port.text("events", wait_ms=800))
    text = port.text("pointer", action="drag", x=300, y=300, to_x=500,
                     to_y=400, steps=6)
    ok("a drag runs", text.startswith("drag"), text)
    port.text("window", release_pointer=True)

    port.text("window", title="mcp-check")
    time.sleep(0.5)
    ok("the title is set", st()["title"] == "mcp-check", st()["title"])
    if fullscreen:
        port.text("window", fullscreen=True)
        time.sleep(1.5)
        ok("fullscreen on", st()["fullscreen"])
        port.text("window", fullscreen=False)
        time.sleep(1.5)
        ok("and off", not st()["fullscreen"])
    port.text("window", antialias="taau")
    r = port.text("events", wait_ms=3000, until="antialias taau")
    ok("events waits for a line that holds a text", "antialias taau" in r, r)
    port.text("window", antialias="none")

    if gpu:
        s = st()
        for i, g in enumerate(s["gpus"]):
            if g["current"] or "llvmpipe" in g["name"]:
                continue
            port.text("window", gpu=i)
            time.sleep(4)
            ok("a move to %s" % g["name"][:24], st()["gpu"] == g["name"],
               st()["gpu"])
            _, png, err = port.call("capture", max_width=400)
            ok("and a capture after it", png and not err)
            port.text("window", gpu=next(j for j, h in enumerate(s["gpus"])
                                         if h["current"]))
            time.sleep(4)
    return ok.results


def command_words(command):
    """The words of --command, with a Windows path's backslashes left alone
    (posix=False) and the quotes that kept words together taken off."""
    return [w.strip("\"'") for w in shlex.split(command or "", posix=False)]


def main():
    ap = argparse.ArgumentParser(
        description="the MCP port's check, against a running testbed")
    ap.add_argument("name", help="the environment, as the ledger names it")
    ap.add_argument("--socket")
    ap.add_argument("--command", help="a command that speaks the port on its "
                                      "stdio, e.g. 'ssh host fenestal-mcp'")
    ap.add_argument("--no-gpu", action="store_true")
    ap.add_argument("--no-fullscreen", action="store_true")
    a = ap.parse_args()
    if bool(a.socket) == bool(a.command):
        ap.error("one of --socket and --command")
    port = Port(a.socket, command_words(a.command) or None)
    try:
        results = run(port, not a.no_gpu, not a.no_fullscreen)
    finally:
        port.close()
    passed = sum(1 for _, p in results if p)
    print("%d of %d passed" % (passed, len(results)))
    ledger.record("mcp", a.name, "release", result="pass"
                  if passed == len(results) else "fail", checks=len(results),
                  failed=[w for w, p in results if not p])
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
