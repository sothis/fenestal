# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A Windows desktop on a machine of its own: a laptop on the network.

The guest (vbox_driver.py) is Windows' window manager and input stack with a
frame rasterised on the CPU. This is the same binary where the compositor,
the kernel's waits and a Vulkan driver that presents to a panel are all real
at once, so it is the one Windows machine a frame rate may be read from.

A guest is entered through its machine: emulated devices, and guest control
to start a program. A laptop has neither, so everything goes through the
agent on its desktop (windows_agent.ps1), and ssh is how the agent gets
there and how it calls home.

  One ssh connection, held for the round, which every copy and command is
  multiplexed over and which forwards two of the laptop's loopback ports
  back to this machine's. So the agent connects to 127.0.0.1 there, nothing
  listens on the network at either end, and neither firewall has a rule for
  it.

  The agent is started on the desktop by a scheduled task, because a program
  started from an ssh session has no desktop to open a window on. The task
  is registered once, by the laptop's user, to run only while they are
  logged on; ssh only asks for it to be run. It runs agent.cmd, which is
  written here each round because the port is new each round.

  Input is the agent's SendInput. That enters above the device drivers:
  Windows marks the events injected and delivers them as it would a hand's,
  raw input included, but a touchpad's driver and a keyboard's own quirks
  are never exercised, which the guest's emulated devices are nearer to.

  The program under test is the agent's child, and its stderr comes back
  down the second forwarded port to native_relay.py, which is the process
  testbed.py holds.

What it needs, once: the OpenSSH server on the laptop, a key of this user's
in it, and somebody logged in to an unlocked desktop --
tools/drive/native-windows.md has each as a command. The rest this file does
over that connection:

    python3 tools/drive/native_driver.py --status <name>   # what is there
    python3 tools/drive/native_driver.py --setup <name>    # and what is not

--setup registers the scheduled task, excludes DRIVE_DIR from Defender's
scanning, stops the machine sleeping on mains, and writes the name down, so
that a round finds the laptop without being told.

The machines are FNL_DRIVE_NATIVE's, as ssh knows them and commas between
them, or else the lines of HOSTS_FILE: the address and the account are
~/.ssh/config's. A machine's GPUs go after its name (listed()), and a round
is then run once on each; a machine named without GPUs takes the ones the
file writes down for it. A round types into that desktop, and a laptop is one
somebody may be sitting at, so it is asked first (BUSY) and a machine with a
hand on it is a skipped environment.
"""

import base64
import ntpath
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from testbed import STATE_DIR, ssh_control
from wayland_driver import BTN_LEFT, BTN_MIDDLE, BTN_RIGHT
from windows_driver import (AGENT, DRIVE_DIR, EXTENDED, PAUSE, POWERSHELL,
                            Desk, Unreachable)

# Beside the portal's token and the guests' password: which machines there
# are is this desk's to know and not the tree's.
HOSTS_FILE = os.path.join(STATE_DIR, "native-hosts")
# How lately a hand may have been on the machine, in seconds, for a round
# to leave it alone. FNL_DRIVE_NATIVE_BUSY=0 is "it is mine, go on".
BUSY = float(os.environ.get("FNL_DRIVE_NATIVE_BUSY", "120"))


def listed(filed=False):
    """{machine: its devices}, in the order written: the environment's, or
    else the file's; the file's alone if `filed`.

    An entry is a machine and then the GPUs a round is run on there, each by
    a part of its name as FNL_GPU takes one -- `schenker intel nvidia` on a
    line, `schenker:intel,schenker:nvidia` between commas. Written down and
    not asked of the machine: what is owed to one has to be known while it
    is off. A machine the environment names and gives no GPUs to takes the
    ones the file writes down for it, so that `FNL_DRIVE_NATIVE=schenker`
    is every round owed to that machine and not one on whichever GPU the
    library picks; one the file gives none either gets one that way."""
    def parse(text):
        found = {}
        for entry in text.split(","):
            words = entry.replace(":", " ").split()
            if not words:
                continue
            devs = found.setdefault(words[0], [])
            devs += [d for d in words[1:] if d not in devs]
        return found

    try:
        with open(HOSTS_FILE) as f:
            written = parse(",".join(ln.split("#")[0] for ln in f))
    except OSError:
        written = {}
    named = None if filed else os.environ.get("FNL_DRIVE_NATIVE")
    if named is None:
        return written
    found = parse(named)
    for name in found:
        if not found[name]:
            found[name] = list(written.get(name, ()))
    return found


def hosts(filed=False):
    """The machines to drive; the file's alone if `filed`."""
    return tuple(listed(filed))


def devices(host):
    """The GPUs a round is run on there, or none for the library's choice."""
    return tuple(listed().get(host, ()))


# The scheduled task that puts the agent on the desktop, and the file it
# runs.
TASK = "fenestal-agent"
LAUNCHER = "agent.cmd"
RELAY = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "native_relay.py")
# BatchMode: a round is not the place to meet a password prompt.
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
BUTTONS = {BTN_LEFT: 1, BTN_RIGHT: 2, BTN_MIDDLE: 3}
VK_PAUSE = 0x13


def _b64(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def powershell(host, script, ctl=None, timeout=120):
    """Run a PowerShell script on the machine; its output's lines. Encoded,
    so that nothing in it is cmd.exe's to quote."""
    enc = base64.b64encode(
        ("$ProgressPreference='SilentlyContinue';" + script)
        .encode("utf-16-le")).decode("ascii")
    got = subprocess.run(
        ["ssh"] + (["-S", ctl] if ctl else []) + SSH_OPTS
        + [host, "powershell -NoProfile -EncodedCommand " + enc],
        stdin=subprocess.DEVNULL, capture_output=True, text=True,
        errors="replace",
        timeout=timeout)
    return [ln.rstrip() for ln in got.stdout.splitlines() if ln.strip()]


def not_windows(host, ctl=None):
    """Why the machine is not one to drive, or None when Windows answers.

    A laptop that boots more than one system (power.py) may be up in
    another under the same name, and there every cmd.exe line is a shell's
    syntax error; asked first, the skip says which system it found."""
    def run(command):
        return subprocess.run(
            ["ssh"] + (["-S", ctl] if ctl else []) + SSH_OPTS
            + [host, command],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            errors="replace", timeout=20)
    try:
        if "Windows" in run("ver").stdout:
            return None
        other = run("uname -s").stdout.strip()
    except subprocess.TimeoutExpired:
        return "%r did not say which system it is up in" % host
    return ("%r is up in %s, not Windows -- `tools/drive/power.py boot %s "
            "windows` where it boots more than one"
            % (host, other or "a system that is not uname's", host))


# What a figure from the machine is read against, a line each: the system,
# every GPU with its driver, the panel's mode, and what it is running on.
FACTS = r"""
$v = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
'Windows ' + $v.DisplayVersion + ' build ' + $v.CurrentBuild + '.' + $v.UBR
Get-CimInstance Win32_VideoController | ForEach-Object {
    $m = ''
    if ($_.CurrentHorizontalResolution) {
        $m = ', {0}x{1} at {2} Hz' -f $_.CurrentHorizontalResolution,
            $_.CurrentVerticalResolution, $_.CurrentRefreshRate
    }
    '{0}, driver {1}{2}' -f $_.Name, $_.DriverVersion, $m
}
$b = Get-CimInstance Win32_Battery
$on = 'mains'
if ($b -and $b.BatteryStatus -eq 1) { $on = 'battery' }
$plan = (powercfg /getactivescheme) -replace '.*\((.*)\).*', '$1'
'on ' + $on + ', power plan ' + $plan
"""


def _free_port():
    """A loopback port nothing has now. Asked for and given back, because
    ssh has to be told the number before the relay that binds it exists."""
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


class Native(Desk):
    """The agent on the laptop's desktop, for what is asked and for what is
    done: windows_driver.Desk's questions, and the input, the program and
    the picture that a guest's machine gives and a laptop's does not."""

    def __init__(self, host):
        self.host = host
        self.sock = None
        self.srv = None
        self.link = None
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
        self.dir = tempfile.mkdtemp(prefix="fnl-native-")
        self.ctl = os.path.join(self.dir, "ssh")
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.relay_port = _free_port()
        fwd = []
        for p in (self.port, self.relay_port):
            fwd += ["-R", "127.0.0.1:%d:127.0.0.1:%d" % (p, p)]
        # To a file and not a pipe: nobody reads it unless the connection
        # fails, and ssh has warnings of its own to fill a pipe with.
        self.said = os.path.join(self.dir, "ssh.err")
        with open(self.said, "w") as err:
            self.link = subprocess.Popen(
                ["ssh", "-M", "-S", self.ctl, "-N"] + SSH_OPTS
                + ["-o", "ExitOnForwardFailure=yes",
                   "-o", "ServerAliveInterval=15"] + fwd + [self.host],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=err)
        # Until the master answers on its socket, which it opens after it
        # has asked for the forwards: a command sent through it then is
        # behind them on the wire, and finds the ports there.
        end = time.time() + 15.0
        while time.time() < end:
            if self.link.poll() is not None:
                raise Unreachable("no ssh to %r: %s"
                                  % (self.host, self._ssh_said()))
            if os.path.exists(self.ctl) and ssh_control(
                    self.ctl, "check", self.host):
                return
            time.sleep(0.05)        # poll: the handshake is a round trip
        raise Unreachable("ssh to %r did not come up" % self.host)

    def _ssh_said(self):
        try:
            with open(self.said) as f:
                # Its last word, past the warnings it opens with.
                lines = [ln.strip() for ln in f if ln.strip()
                         and not ln.startswith("**")]
        except OSError:
            lines = []
        return lines[-1] if lines else "it said nothing"

    def _ssh(self, command, timeout=30):
        """One command line, for the laptop's cmd.exe, over the held
        connection.

        What comes back is in the console's code page, which on a Windows
        that is not English is not UTF-8: a tool's message with an umlaut
        in it must not be what stops a round, so a byte that does not
        decode is replaced and never raised."""
        return subprocess.run(
            ["ssh", "-S", self.ctl] + SSH_OPTS + [self.host, command],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            errors="replace",
            timeout=timeout)

    def _scp(self, src, dst, timeout=120):
        return subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + [src, dst],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            errors="replace",
            timeout=timeout)

    def _there(self, name):
        """A file in DRIVE_DIR as scp names it: forward slashes, and the
        drive letter is not taken for a host."""
        return "%s:%s/%s" % (self.host, DRIVE_DIR.replace("\\", "/"), name)

    def _open_agent(self):
        why = not_windows(self.host, self.ctl)
        if why:
            raise Unreachable(why)
        mk = self._ssh('if not exist "%s" mkdir "%s"' % (DRIVE_DIR,
                                                         DRIVE_DIR))
        if mk.returncode != 0:
            raise Unreachable("could not make %s on %r: %s"
                              % (DRIVE_DIR, self.host, mk.stderr.strip()))
        if self._ssh("schtasks /query /tn %s" % TASK).returncode != 0:
            raise Unreachable(
                "no scheduled task %r on %r to put the agent on its "
                "desktop -- tools/drive/native-windows.md registers one"
                % (TASK, self.host))
        # Headless: the agent is a console program, and a console window on
        # the desktop is one more thing to take the foreground and to be in
        # the picture. `start`, so that the task is over as soon as it has
        # begun and can be asked for again next round.
        launcher = os.path.join(self.dir, LAUNCHER)
        with open(launcher, "w", newline="\r\n") as f:
            f.write('@start "" conhost.exe --headless "%s" -NoProfile '
                    '-ExecutionPolicy Bypass -File "%s\\%s" %d 127.0.0.1\n'
                    % (POWERSHELL, DRIVE_DIR, os.path.basename(AGENT),
                       self.port))
        self.install(AGENT)
        self.install(launcher)
        # Ended first, and no matter if there was nothing to end: a task
        # still counted as running is not started a second time.
        self._ssh("schtasks /end /tn %s" % TASK)
        run = self._ssh("schtasks /run /tn %s" % TASK)
        if run.returncode != 0:
            raise Unreachable("the task %r would not run on %r: %s"
                              % (TASK, self.host,
                                 (run.stderr or run.stdout).strip()))
        self.srv.settimeout(30.0)
        try:
            sock, _ = self.srv.accept()
        except socket.timeout:
            raise Unreachable(
                "the agent never called back from %r -- is its user logged "
                "in to the desktop? The task runs only while they are"
                % self.host)
        self._greet(sock)
        # Asked now and not met mid-round: behind the lock screen the agent
        # runs and answers, and no input it sends arrives anywhere.
        try:
            self.pointer_pos(patience=1.0)
        except RuntimeError:
            raise Unreachable(
                "the desktop of %r is locked, or showing a prompt that has "
                "the input" % self.host)
        # And whether it is somebody's just now. The agent knows its own
        # input from a hand's by the note the last one left.
        idle = int(self.ask("idle", _b64(DRIVE_DIR + "\\last-input"))[0])
        if 0 <= idle < BUSY * 1000:
            raise Unreachable(
                "somebody was at %r %d s ago, and a round types into its "
                "desktop -- FNL_DRIVE_NATIVE_BUSY=0 to drive it anyway"
                % (self.host, idle // 1000))

    def facts(self):
        """What the machine is, for a figure to be read against: a line
        each. The display's scale is the agent's to say."""
        try:
            got = powershell(self.host, FACTS, ctl=self.ctl)
        except subprocess.TimeoutExpired:
            got = []
        return got + ["display at %d%%" % round(self.dpi * 100 / 96)]

    def install(self, path):
        """Copy a file into DRIVE_DIR; returns its path there."""
        name = os.path.basename(path)
        cp = self._scp(os.path.abspath(path), self._there(name))
        if cp.returncode != 0:
            raise Unreachable("could not copy %s to %r: %s"
                              % (name, self.host,
                                 "".join(cp.stderr.strip().splitlines()[-1:])))
        return DRIVE_DIR + "\\" + name

    def close(self):
        if self.sock:
            for btn in list(self.held):
                try:
                    self.button(btn, False)
                except Exception:
                    pass
            self._keys_up()
        # The agent stops whatever it started when it is told to go, and
        # when the connection goes without telling it.
        self._hang_up()
        if self.link:
            ssh_control(self.ctl, "exit", self.host)
            try:
                self.link.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.link.kill()
                self.link.wait()
            self.link = None
        if self.srv:
            self.srv.close()
            self.srv = None
        if self.dir:
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir = None

    # ---- the program under test --------------------------------------------

    def run_argv(self, exe, args=(), env=None, everything=False):
        """`everything` has the program's standard output come back with
        its stderr, for one that is run to be read."""
        # The relay first and the program after it, in run_begin(): what
        # the program says has to have somewhere to go before it says it.
        self.pending = (exe, list(args), dict(env or {}),
                        "runall" if everything else "run")
        return [sys.executable, RELAY, str(self.relay_port)]

    def run_begin(self):
        exe, args, env, verb = self.pending
        self.pending = None
        # One string, quoted as the C runtime there will split it.
        line = subprocess.list2cmdline(args)
        self.ask(verb, self.relay_port, _b64(exe),
                 _b64(line) if line else "-",
                 *[_b64("%s=%s" % kv) for kv in sorted(env.items())])

    def spawn(self, exe, args=(), log=None):
        """Start a program on the desktop and leave it there: its two
        streams go to a log in DRIVE_DIR, and it outlives this connection
        and the agent, for a program that has to go on while the machine
        cannot be reached. Returns the log's path there."""
        # ntpath: exe is the laptop's path, and posixpath would take all of
        # it for the name.
        log = log or DRIVE_DIR + "\\" + ntpath.splitext(
            ntpath.basename(exe))[0] + ".log"
        line = subprocess.list2cmdline(list(args))
        self.ask("spawn", _b64(exe), _b64(line) if line else "-", _b64(log))
        return log

    # ---- the pointer and the keys ------------------------------------------

    def warp(self, x, y):
        """Put the pointer at a screen pixel."""
        self.ask("warp", int(x), int(y))

    def nudge(self, dx, dy):
        """Relative motion, which is what raw input counts and so all a
        captured pointer hears."""
        self.ask("nudge", int(round(dx)), int(round(dy)))

    def button(self, btn=BTN_LEFT, pressed=True):
        self.ask("button", BUTTONS[btn], 1 if pressed else 0)
        (self.held.add if pressed else self.held.discard)(btn)

    def scroll(self, steps):
        self.ask("wheel", int(steps))

    def keycode(self, code, pressed=True):
        """One edge of a key, by evdev code: the scancode, whether it is an
        extended one, and a virtual key for the one key that has to go by
        that."""
        down = 1 if pressed else 0
        if code == PAUSE:
            self.ask("key", 0x45, 0, down, VK_PAUSE)
        elif code in EXTENDED:
            self.ask("key", EXTENDED[code], 1, down, 0)
        elif 0 < code < 89:
            self.ask("key", code, 0, down, 0)
        else:
            raise KeyError("no scancode for evdev %d" % code)

    # ---- a picture ---------------------------------------------------------

    def shot_origin(self):
        # The picture is of every monitor together, and one to the left of
        # the primary puts the desktop's origin inside it.
        x, y, _w, _h = self.desk
        return (-x, -y)

    def screenshot(self, path):
        """Every monitor of the laptop together, as its compositor shows
        them. Taken there as a BMP, which is all the agent can write, and
        kept here as the PNG asked for."""
        from PIL import Image
        name = "shot.bmp"
        self.ask("shot", _b64(DRIVE_DIR + "\\" + name))
        local = os.path.join(self.dir, name)
        got = self._scp(self._there(name), local)
        if got.returncode != 0:
            raise RuntimeError("no screenshot: %s" % got.stderr.strip())
        Image.open(local).convert("RGB").save(path)
        return path


# ---- setting a machine up, and saying how it is set up ----------------------

# Each answers "name: yes" or "name: no ...", so that --status reads as a
# list and --setup knows what is left to do.
STATUS = r"""
function Say($name, $ok, $more) {
    if ($ok) { $name + ': yes' } else { $name + ': no' + $more }
}
$u = [bool](query user 2>$null | Select-String 'console')
Say 'somebody is logged in at the console' $u ''
$w = Get-ItemProperty ('HKLM:\SOFTWARE\Microsoft\Windows NT' +
                      '\CurrentVersion\Winlogon')
Say 'and is logged in again after a restart' ($w.AutoAdminLogon -eq '1') `
    ' -- netplwiz, or Sysinternals Autologon'
$t = Get-ScheduledTask -TaskName '%(task)s' -ErrorAction SilentlyContinue
Say 'the task that starts the agent' ([bool]$t) ' -- --setup registers it'
$x = (Get-MpPreference).ExclusionPath -contains 'C:\fenestal'
Say 'Defender leaves C:\fenestal alone' $x ' -- --setup excludes it'
$p = (Get-NetConnectionProfile | Select-Object -First 1).NetworkCategory
Say 'the network is private' ($p -ne 'Public') ''
$l = Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\Vulkan\ExplicitLayers' `
    -ErrorAction SilentlyContinue
$l = $l -and ($l.PSObject.Properties.Name -match 'khronos_validation')
Say 'the Vulkan validation layer' ([bool]$l) ' -- the Vulkan SDK brings it'
$r = Test-Path 'C:\Windows\System32\vcruntime140.dll'
Say 'and the Visual C++ runtime it will not load without' $r `
    ' -- https://aka.ms/vs/17/release/vc_redist.x64.exe'
"""

# Registered for whoever ssh logs in as, to run only while they are logged
# on: interactive, so the agent is on the desktop and no password is kept;
# limited, so it is no more privileged than what it drives; and allowed on
# battery, where Task Scheduler otherwise neither starts a task nor lets
# one live.
SETUP = r"""
New-Item -ItemType Directory -Force '%(dir)s' | Out-Null
$t = Get-ScheduledTask -TaskName '%(task)s' -ErrorAction SilentlyContinue
if (-not $t) {
    $a = New-ScheduledTaskAction -Execute '%(dir)s\%(launcher)s'
    $p = New-ScheduledTaskPrincipal -LogonType Interactive -RunLevel Limited `
        -UserId "$env:USERDOMAIN\$env:USERNAME"
    $s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName '%(task)s' -Action $a -Principal $p `
        -Settings $s | Out-Null
    'registered the task %(task)s'
}
if (-not ((Get-MpPreference).ExclusionPath -contains 'C:\fenestal')) {
    Add-MpPreference -ExclusionPath 'C:\fenestal'
    'excluded C:\fenestal from Defender'
}
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS LIDACTION 0
powercfg /setactive SCHEME_CURRENT
'on mains: no sleep, no display-off, and the lid does nothing'
"""


def _main(argv):
    if len(argv) != 2 or argv[0] not in ("--status", "--setup"):
        sys.exit(__doc__)
    what, host = argv
    names = {"task": TASK, "dir": DRIVE_DIR, "launcher": LAUNCHER}
    try:
        who = subprocess.run(["ssh"] + SSH_OPTS + [host, "whoami"],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, errors="replace", timeout=20)
    except subprocess.TimeoutExpired:
        who = None
    if not who or who.returncode != 0:
        sys.exit("no ssh to %r without a prompt -- native-windows.md has "
                 "the server and the key" % host)
    print("%s, as %s" % (host, who.stdout.strip()))
    why = not_windows(host)
    if why:
        sys.exit(why)
    if what == "--setup":
        for line in powershell(host, SETUP % names):
            print("  " + line)
        os.makedirs(STATE_DIR, exist_ok=True)
        # The file's own, or FNL_DRIVE_NATIVE set here would have a host
        # written twice, or not at all.
        if host not in hosts(filed=True):
            with open(HOSTS_FILE, "a") as f:
                f.write(host + "\n")
            print("  wrote %s into %s" % (host, HOSTS_FILE))
    for line in powershell(host, FACTS) + powershell(host, STATUS % names):
        print("  " + line)
    listed = host in hosts()
    print("  a round drives it without being told: %s"
          % ("yes" if listed else "no -- --setup writes it into %s"
             % HOSTS_FILE))
    if listed:
        print("  and once on each of: %s"
              % (", ".join(devices(host)) or "no device by name -- write "
                 "them after %r in %s, as FNL_GPU would name them"
                 % (host, HOSTS_FILE)))
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
