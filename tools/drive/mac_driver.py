# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A Mac's desktop, reached over ssh: schenker booted from its macOS stick.

The Windows laptop's way in (native_driver.py), shaped for macOS. What a
desktop can be asked is windows_driver.Desk's, and the agent on it
(mac_agent.m) answers the same questions with macOS's own calls. What is
different is getting in:

  A program started from an ssh session as the logged-in user reaches the
  window server, so there is no scheduled task and no forwarded port: the
  agent is the last command of one ssh session, its stdin and stdout are
  the line, and the program under test is the last command of another,
  whose stderr is the program's and so the echo. One master connection is
  held for the round, which both, and every copy, are multiplexed over.

  The agent is compiled here, by the toolchain that cross-compiles the
  library (cmake/macos-x86_64.cmake's clang and Apple's SDK), into
  build/drive, and copied over with the program each round: a machine that
  ran last week's agent answers last week's questions.

What it needs, once, and only by a hand on the Mac -- macOS gives these to
nobody who asks remotely: Remote Login on, this desk's key in the account's
authorized_keys, and two privacy grants to /usr/libexec/sshd-keygen-wrapper,
which is what an ssh session is to the privacy database. Screen Recording,
without which a window's title is withheld and a picture is the wallpaper;
and Accessibility, without which a posted event is dropped without a word.
--setup asks for the second, which puts the entry in System Settings and a
dialog on the Mac's screen.

    python3 tools/drive/mac_driver.py --status <name>   # what is there
    python3 tools/drive/mac_driver.py --setup <name>    # and asks for the rest

The machines are FNL_DRIVE_MAC's, as ssh knows them and commas between them,
or else the lines of HOSTS_FILE, which --setup writes. A Mac booted into
another system answers ssh with that system's host key, which is a skipped
round and not a finding: tools/drive/power.py boots schenker into macOS.
"""

import base64
import os
import re
import select
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

from testbed import OUT, ROOT, STATE_DIR, ssh_control
from wayland_driver import BTN_LEFT, BTN_MIDDLE, BTN_RIGHT
from windows_driver import Desk, Unreachable

HOSTS_FILE = os.path.join(STATE_DIR, "mac-hosts")
# How lately a hand may have been on the machine, in seconds, for a round
# to leave it alone, as native_driver.BUSY. FNL_DRIVE_MAC_BUSY=0 is "it is
# mine, go on".
BUSY = float(os.environ.get("FNL_DRIVE_MAC_BUSY", "120"))
# Under the account's home there, which is where ssh starts.
DRIVE_DIR = "fenestal/drive"
AGENT_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "mac_agent.m")
AGENT = os.path.join(OUT, "mac-agent")
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
# How long one question to the agent may take, a capture included.
ASK_S = 30
BUTTONS = {BTN_LEFT: 1, BTN_RIGHT: 2, BTN_MIDDLE: 3}


def hosts(filed=False):
    """The Macs to drive: the environment's, or else the file's; the file's
    alone if `filed`."""
    named = None if filed else os.environ.get("FNL_DRIVE_MAC")
    if named is None:
        try:
            with open(HOSTS_FILE) as f:
                named = ",".join(ln.split("#")[0] for ln in f)
        except OSError:
            named = ""
    return tuple(h.strip() for h in named.split(",") if h.strip())


def _b64(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


# ---- keys -------------------------------------------------------------------
#
# The harness names a key by its evdev code and the Mac by its virtual key
# code. Both are tables of the library's own -- evdev_keys.h for the Linux
# backends and MAC_KEYS in cocoa.m -- keyed by FnlKey, so the one is read
# through the other here rather than written out a third time to drift.

def _fnl_table(path, start):
    """{code: FnlKey name} from a `[code] = FNL_KEY_X` initialiser."""
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        text = f.read()
    body = text[text.index(start):]
    body = body[:body.index("};")]
    return {int(c, 0): k for c, k in
            re.findall(r"\[\s*(0x[0-9a-fA-F]+|\d+)\s*\]\s*=\s*(FNL_KEY_\w+)",
                       body)}


def _evdev_to_mac():
    evdev = _fnl_table("src/backend/evdev_keys.h", "EVDEV_KEYS[")
    mac = {k: c for c, k in
           _fnl_table("src/backend/cocoa/cocoa.m", "MAC_KEYS[").items()}
    return {e: mac[k] for e, k in evdev.items() if k in mac}


EVDEV_TO_MAC = _evdev_to_mac()
# The two codes an ISO board swaps, as cocoa.m's iso_swap swaps them back.
KC_ISO_SECTION, KC_GRAVE = 0x0a, 0x32


# ---- the agent, built here ---------------------------------------------------

def build_agent():
    """The agent, compiled for macOS where it is older than its source.
    Raises Unreachable when there is no toolchain or SDK to do it with."""
    if os.path.exists(AGENT) and \
            os.path.getmtime(AGENT) >= os.path.getmtime(AGENT_SRC):
        return AGENT
    sdk = subprocess.run([sys.executable, os.path.join(ROOT, "tools",
                                                       "macos_sdk.py"),
                          "--path"], capture_output=True, text=True)
    clang = os.environ.get("FNL_CLANG") or "clang-21"
    if sdk.returncode != 0 or not shutil.which(clang):
        raise Unreachable("no macOS SDK or %s to build the agent with -- "
                          "python3 tools/macos_sdk.py --fetch" % clang)
    os.makedirs(OUT, exist_ok=True)
    cc = subprocess.run(
        [clang, "--target=x86_64-apple-macos13", "-isysroot",
         sdk.stdout.strip(), "-O2", "-fobjc-arc", "-Wall", "-Wextra",
         "-fuse-ld=lld", AGENT_SRC, "-framework", "AppKit", "-framework",
         "ApplicationServices", "-framework", "Carbon", "-framework",
         "IOKit", "-framework", "ImageIO", "-o", AGENT],
        capture_output=True, text=True)
    if cc.returncode != 0:
        raise RuntimeError("the Mac agent did not build:\n" + cc.stderr)
    return AGENT


# ---- the machine -------------------------------------------------------------

class Mac(Desk):
    """The agent on the Mac's desktop, for what is asked and what is done."""

    def __init__(self, host):
        self.host = host
        self.link = None
        self.agent = None
        self.dir = None
        self.held = set()
        self.keys = set()
        self.pending = None
        try:
            self._open_link()
            self._open_agent()
        except Exception:
            self.close()
            raise

    # ---- getting in --------------------------------------------------------

    def _open_link(self):
        if not shutil.which("ssh") or not shutil.which("scp"):
            raise Unreachable("no ssh on this machine")
        self.dir = tempfile.mkdtemp(prefix="fnl-mac-")
        self.ctl = os.path.join(self.dir, "ssh")
        self.said = os.path.join(self.dir, "ssh.err")
        with open(self.said, "w") as err:
            self.link = subprocess.Popen(
                ["ssh", "-M", "-S", self.ctl, "-N"] + SSH_OPTS
                + ["-o", "ServerAliveInterval=15", self.host],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=err)
        end = time.time() + 15.0
        while time.time() < end:
            if self.link.poll() is not None:
                raise Unreachable("no ssh to %r: %s -- is it booted into "
                                  "macOS? tools/drive/power.py boot <name> "
                                  "macos" % (self.host, self._ssh_said()))
            if os.path.exists(self.ctl) and ssh_control(
                    self.ctl, "check", self.host):
                return
            time.sleep(0.05)        # poll: the handshake is a round trip
        raise Unreachable("ssh to %r did not come up" % self.host)

    def _ssh_said(self):
        try:
            with open(self.said) as f:
                lines = [ln.strip() for ln in f if ln.strip()
                         and not ln.startswith("@")]
        except OSError:
            lines = []
        return lines[-1] if lines else "it said nothing"

    def _ssh_argv(self, command):
        return ["ssh", "-S", self.ctl] + SSH_OPTS + [self.host, command]

    def _ssh(self, command, timeout=30):
        return subprocess.run(self._ssh_argv(command),
                              stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, errors="replace", timeout=timeout)

    def _scp(self, src, dst, timeout=120):
        return subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + [src, dst], stdin=subprocess.DEVNULL, capture_output=True,
            text=True, errors="replace", timeout=timeout)

    def _open_agent(self):
        mk = self._ssh("mkdir -p %s" % DRIVE_DIR)
        if mk.returncode != 0:
            raise Unreachable("could not make ~/%s on %r: %s"
                              % (DRIVE_DIR, self.host, mk.stderr.strip()))
        there = self.install(build_agent())
        self.agent = subprocess.Popen(
            self._ssh_argv("exec ./" + there), stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            bufsize=1)
        self.carry = [0.0, 0.0]
        self.dpi = int(self.ask("hello")[1])
        # Asked now and not met mid-round, as the laptop's lock screen is:
        # without it every key and click a round sends goes nowhere.
        if self.ask("trusted")[0] != "1":
            raise Unreachable(
                "%r has not granted Accessibility to what an ssh session is "
                "-- python3 tools/drive/mac_driver.py --setup %s asks for it"
                % (self.host, self.host))
        if self.ask("screen")[0] != "1":
            raise Unreachable(
                "%r has not granted Screen Recording to what an ssh session "
                "is, or wants it confirmed again -- by a hand on the Mac, "
                "in System Settings (mac_driver.py --status %s)"
                % (self.host, self.host))
        idle = int(self.ask("idle", _b64(DRIVE_DIR + "/last-input"))[0])
        if 0 <= idle < BUSY * 1000:
            raise Unreachable(
                "somebody was at %r %d s ago, and a round types into its "
                "desktop -- FNL_DRIVE_MAC_BUSY=0 to drive it anyway"
                % (self.host, idle // 1000))
        self.iso = self.ask("iso")[0] == "1"

    def ask(self, *words):
        """One question, one answer: the reply's words after its "ok"."""
        if not self.agent or self.agent.poll() is not None:
            raise RuntimeError("the agent on the Mac has gone")
        self.agent.stdin.write(" ".join(str(w) for w in words) + "\n")
        self.agent.stdin.flush()
        # The Windows agent's deadline (windows_driver.Desk): an agent that
        # stops answering ends the round rather than holding it, and the
        # Mac's input, for good.
        ready, _, _ = select.select([self.agent.stdout], [], [], ASK_S)
        if not ready:
            self.agent.kill()
            raise RuntimeError("the agent on the Mac did not answer %r in "
                               "%d s" % (words[0], ASK_S))
        line = self.agent.stdout.readline()
        if not line:
            raise RuntimeError("the agent on the Mac has gone")
        parts = line.rstrip("\n").split(" ")
        if parts[0] != "ok":
            raise RuntimeError("the agent answered %r to %r"
                               % (line.strip(), words[0]))
        return parts[1:]

    def facts(self):
        """What the machine is, a line each, for a figure to be read
        against. The display's alone where the rest take too long, as
        native_driver's: a round is not lost for want of them, and the
        connection _open() made would be left behind."""
        try:
            got = self._ssh(
                "sw_vers -productVersion; sw_vers -buildVersion; "
                "system_profiler SPDisplaysDataType | grep -E "
                "'Chipset Model|Resolution:|Metal Support'; "
                "pmset -g batt | head -1").stdout
        except subprocess.TimeoutExpired:
            got = ""
        lines = [ln.strip() for ln in got.splitlines() if ln.strip()]
        if len(lines) >= 2:
            lines[:2] = ["macOS %s (%s)" % (lines[0], lines[1])]
        return lines + ["display at %d%%" % round(self.dpi * 100 / 96)]

    def install(self, path):
        """Copy a file into ~/DRIVE_DIR; returns its path there, from the
        home directory, where every ssh command starts."""
        name = os.path.basename(path)
        cp = self._scp(os.path.abspath(path),
                       "%s:%s/%s" % (self.host, DRIVE_DIR, name))
        if cp.returncode != 0:
            raise Unreachable("could not copy %s to %r: %s"
                              % (name, self.host,
                                 "".join(cp.stderr.strip()
                                         .splitlines()[-1:])))
        return "%s/%s" % (DRIVE_DIR, name)

    def remove(self, names):
        """Take files out of ~/DRIVE_DIR, by name; one not there is
        nothing."""
        rm = self._ssh("cd %s && rm -f %s" % (
            DRIVE_DIR, " ".join(shlex.quote(n) for n in names)))
        if rm.returncode != 0:
            raise Unreachable("could not clear %s on %r: %s"
                              % (DRIVE_DIR, self.host, rm.stderr.strip()))

    def close(self):
        if self.agent:
            for btn in list(self.held):
                try:
                    self.button(btn, False)
                except Exception:
                    pass
            self._keys_up()
            try:
                self.agent.stdin.write("quit\n")
                self.agent.stdin.close()
                self.agent.wait(timeout=5)
            except Exception:
                self.agent.kill()
            self.agent = None
        if self.link:
            ssh_control(self.ctl, "exit", self.host)
            try:
                self.link.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.link.kill()
                self.link.wait()
            self.link = None
        if self.dir:
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir = None

    # ---- names, which the agent sends as base64 --------------------------

    def monitor(self):
        a = self.ask("mon")
        v = [int(x) for x in a[1:]]
        return ((base64.b64decode(a[0]).decode("utf-8"),
                 v[0], v[1], v[2], v[3]), tuple(v[4:8]))

    def monitors(self):
        a = self.ask("mons")
        return [(base64.b64decode(a[i]).decode("utf-8"), a[i + 1] == "1")
                + tuple(int(v) for v in a[i + 2:i + 6])
                for i in range(0, len(a) - 5, 6)]

    # ---- the program under test --------------------------------------------

    def run_argv(self, exe, args=(), env=None, everything=False):
        """The ssh session whose last command is the program: its stderr is
        the program's, and it ends when the program does. `everything` has
        its standard output come back on stderr too, for one that is run
        to be read."""
        sets = " ".join(shlex.quote("%s=%s" % kv)
                        for kv in sorted((env or {}).items()))
        line = "cd %s && exec env %s ./%s %s%s" % (
            shlex.quote(os.path.dirname(exe)), sets,
            shlex.quote(os.path.basename(exe)),
            " ".join(shlex.quote(a) for a in args),
            " 1>&2" if everything else "")
        return self._ssh_argv(line)

    def run_begin(self):
        pass

    # ---- the pointer and the keys ------------------------------------------

    def warp(self, x, y):
        self.ask("warp", int(x), int(y))

    def nudge(self, dx, dy):
        self.ask("nudge", int(round(dx)), int(round(dy)))

    def button(self, btn=BTN_LEFT, pressed=True):
        self.ask("button", BUTTONS[btn], 1 if pressed else 0)
        (self.held.add if pressed else self.held.discard)(btn)

    def scroll(self, steps):
        self.ask("wheel", int(steps))

    def keycode(self, code, pressed=True):
        """One edge of a key, by evdev code, sent by the Mac's code for the
        same place on the board."""
        mac = EVDEV_TO_MAC.get(code)
        if mac is None:
            raise KeyError("no Mac key code for evdev %d" % code)
        if self.iso and mac in (KC_ISO_SECTION, KC_GRAVE):
            mac = KC_GRAVE if mac == KC_ISO_SECTION else KC_ISO_SECTION
        self.ask("key", mac, 1 if pressed else 0)

    # ---- a picture ---------------------------------------------------------

    def _not_covered(self):
        over = self.ask("over")
        if over:
            raise RuntimeError(
                "the window is covered on %r by %s -- a dialog of macOS's "
                "wants a hand there (Screen Recording's asks to be "
                "confirmed again from time to time)"
                % (self.host, ", ".join(over)))

    def screenshot(self, path):
        """The main display as the window server shows it, in sRGB: matched
        on the Mac, by ColorSync (shot_srgb() in mac_agent.m), since matched
        here through the profile screencapture writes the darks came back
        wrong."""
        name = "shot.png"
        # A dialog of macOS's own over the window -- Screen Recording's
        # asking to be confirmed again, which comes from time to time -- is
        # said here, and never read as the frame (over_window() in the
        # agent). Before and after, so one that comes up between is seen.
        self._not_covered()
        self.ask("shot", _b64("%s/%s" % (DRIVE_DIR, name)))
        self._not_covered()
        got = self._scp("%s:%s/%s" % (self.host, DRIVE_DIR, name), path)
        if got.returncode != 0:
            raise RuntimeError("no screenshot: %s" % got.stderr.strip())
        return path


# ---- setting a machine up, and saying how it is set up ----------------------

# Read off the privacy database, which root may read where the ssh session
# has Full Disk Access (Remote Login's "Allow full disk access for remote
# users") and nobody may write while SIP guards it.
TCC = ("sudo -n sqlite3 '/Library/Application Support/com.apple.TCC/TCC.db' "
       "\"select service, auth_value from access where client = "
       "'/usr/libexec/sshd-keygen-wrapper'\"")
GRANTS = {"kTCCServiceScreenCapture": "Screen Recording",
          "kTCCServiceAccessibility": "Accessibility",
          "kTCCServiceSystemPolicyAllFiles": "Full Disk Access"}


def _main(argv):
    if len(argv) != 2 or argv[0] not in ("--status", "--setup"):
        sys.exit(__doc__)
    what, host = argv
    try:
        who = subprocess.run(["ssh"] + SSH_OPTS + [host, "whoami"],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        who = None
    if not who or who.returncode != 0:
        sys.exit("no ssh to %r without a prompt -- is it booted into macOS, "
                 "with Remote Login on and this desk's key in "
                 "authorized_keys?" % host)
    print("%s, as %s" % (host, who.stdout.strip()))
    try:
        tcc = subprocess.run(["ssh"] + SSH_OPTS + [host, TCC],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=20)
    except subprocess.TimeoutExpired:
        sys.exit("%s did not answer what it has granted within 20 s" % host)
    have = dict(ln.split("|") for ln in tcc.stdout.split() if "|" in ln)
    for service, name in GRANTS.items():
        print("  %s for ssh sessions: %s" % (name, "yes" if
                                             have.get(service) == "2" else
                                             "no"))
    if what == "--setup":
        agent = build_agent()
        # Bounded, as every ssh here is: a Mac that went to sleep or off
        # between the look and the copy would hold --setup for good.
        try:
            subprocess.run(["ssh"] + SSH_OPTS
                           + [host, "mkdir -p " + DRIVE_DIR],
                           stdin=subprocess.DEVNULL, check=True, timeout=20)
            subprocess.run(["scp", "-q"] + SSH_OPTS
                           + [agent, "%s:%s/" % (host, DRIVE_DIR)],
                           stdin=subprocess.DEVNULL, check=True,
                           timeout=120)
            asked = subprocess.run(
                ["ssh"] + SSH_OPTS + [host, "%s/mac-agent" % DRIVE_DIR],
                input="trusted prompt\nquit\n", capture_output=True,
                text=True, timeout=20)
        except subprocess.TimeoutExpired as e:
            sys.exit("%s did not finish within %d s: %s"
                     % (host, e.timeout, " ".join(e.cmd[-2:])))
        if asked.stdout.strip() != "ok 1":
            print("  asked for Accessibility: the Mac shows where to switch "
                  "it on, in System Settings -> Privacy & Security -> "
                  "Accessibility (sshd-keygen-wrapper)")
        if have.get("kTCCServiceScreenCapture") != "2":
            print("  Screen Recording is switched on by hand, in the same "
                  "pane's Screen & System Audio Recording: + "
                  "/usr/libexec/sshd-keygen-wrapper")
        os.makedirs(STATE_DIR, exist_ok=True)
        # The file's own, or FNL_DRIVE_MAC set here would have a host
        # written twice, or not at all.
        if host not in hosts(filed=True):
            with open(HOSTS_FILE, "a") as f:
                f.write(host + "\n")
            print("  wrote %s into %s" % (host, HOSTS_FILE))
    print("  a round drives it without being told: %s"
          % ("yes" if host in hosts() else
             "no -- --setup writes it into %s" % HOSTS_FILE))
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
