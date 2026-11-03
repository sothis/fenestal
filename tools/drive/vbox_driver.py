# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A Windows desktop in a VirtualBox machine: its keyboard and mouse, and what
it can be asked about a window.

The win64 build under wine is an X11 window, driven and questioned like one.
This is the same binary on Windows itself -- its window manager, its input
stack, its clipboard -- which is the half wine can only resemble. It is *not*
a second opinion on the GPU: a VirtualBox guest has no Vulkan device, so the
frame is rasterised on the CPU by Mesa's lavapipe, and a frame rate read
there says nothing about this library.

Three ways in, each for what the others cannot do.

  The machine's own devices, through VirtualBox's API. A key is a scancode at
  the emulated keyboard and a move is a report from the emulated mouse, so
  what is driven is everything from the Windows input stack upwards. Nothing
  passes through this desktop: no portal, no consent, and no key that can go
  to the wrong window *here*. It can still go to the wrong window *there*,
  so environment.py asks before each one, as it does everywhere.

  Guest control, through VBoxManage, which starts a program on the guest's
  desktop and hands its stderr back down a pipe. The program under test is
  therefore an ordinary subprocess to testbed.py: the VBoxManage that runs it.

  An agent on the guest (windows_agent.ps1) for what only that desktop
  knows: the rectangle, the focus, the monitor, where the pointer is, the
  clipboard. It connects out to a socket here and answers a line a question.
  The asking is windows_driver.Desk, which a laptop's driver shares.

The pointer is moved by *absolute* reports wherever it has a position, which
the Guest Additions' mouse integration offers and the portal never did: no
acceleration between the request and the result. A captured pointer has no
position, and the library reads it from raw input, which counts only relative
motion -- so nudge() is the emulated mouse's relative report, the one a mouse
on a desk sends.

What it needs, once: the guest's user logged in to a desktop, the Guest
Additions, a Vulkan loader in the guest and lavapipe where LAVAPIPE below
says (it needs no registry entry), that user's password in
~/.local/state/fenestal/vbox-guest (mode 600), and the machine's NAT told
to show the guest this host's loopback, which a new machine's does not:
VBoxManage modifyvm <vm> --nat-localhostreachable1 on, with it powered off.
FNL_DRIVE_VM names the machines, commas between them, and FNL_DRIVE_VM_USER
the account; the defaults are "win10,win11" and whoever is running this. A
round drives each in turn (environment.Win32VM.each()), and a machine with a
password of its own keeps it in vbox-guest-<vm> beside the shared file.
"""

import atexit
import getpass
import os
import shutil
import socket
import subprocess
import time

from testbed import STATE_DIR
from wayland_driver import BTN_LEFT, BTN_MIDDLE, BTN_RIGHT
from windows_driver import (AGENT, DRIVE_DIR, EXTENDED, PAUSE, POWERSHELL,
                            Desk, Unreachable)

VMS = tuple(v for v in os.environ.get("FNL_DRIVE_VM", "win10,win11").split(",")
            if v)
# The machine being driven now. One at a time: a round is one window, and
# use() is how the next round names the next machine. Read where it is used
# and never copied at import, or the copy is the first machine for good.
VM = VMS[0] if VMS else "win10"
USER = os.environ.get("FNL_DRIVE_VM_USER") or getpass.getuser()
# A credential, so it lives beside the portal's token and not under build/.
PASSWORD_FILE = os.path.join(STATE_DIR, "vbox-guest")


def use(vm):
    """Name the machine everything below is put to from here on."""
    global VM
    VM = vm


def password_file():
    """The machine's own file where it has one, and the shared one where it
    does not: two guests set up alike have one account and one password."""
    own = "%s-%s" % (PASSWORD_FILE, VM)
    return own if os.path.exists(own) else PASSWORD_FILE

# Named from here by the probes as well (tools/probe/guest_run.py).
GUEST_DIR = DRIVE_DIR
# The guest's Vulkan driver, named to the loader at every launch. The registry
# is where a driver is usually found, but the only key the loader reads for
# one is under HKLM and writing it takes an administrator, which guest control
# is not. The loader ignores this variable in an elevated process; nothing
# here starts one.
LAVAPIPE = {"VK_DRIVER_FILES": r"C:\fenestal\lavapipe\lvp_icd.x86_64.json"}

BUTTON_BITS = {BTN_LEFT: 0x01, BTN_RIGHT: 0x02, BTN_MIDDLE: 0x04}


def scancodes(code, pressed):
    """One edge of a key, by evdev code, as set 1 bytes."""
    up = 0 if pressed else 0x80
    if code == PAUSE:
        # The one key with no break code: the whole sequence is sent on the
        # press and nothing on the release.
        return [0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5] if pressed else []
    if code in EXTENDED:
        return [0xe0, EXTENDED[code] | up]
    if 0 < code < 89:
        return [code | up]
    raise KeyError("no scancode for evdev %d" % code)


def manage(*args, timeout=60):
    return subprocess.run(["VBoxManage"] + list(args), capture_output=True,
                          text=True, timeout=timeout)


def guest_argv(verb, *args):
    """A VBoxManage guestcontrol command line, logged in as the guest's
    user. The password goes by file, so it is on no command line."""
    return ["VBoxManage", "guestcontrol", VM, verb, "--username", USER,
            "--passwordfile", password_file()] + list(args)


def run_argv(exe, args=(), env=None):
    """The command line whose process *is* a program on the guest's desktop:
    it lives as long as the program does, its stderr is the program's, and
    killing it ends the program."""
    put = []
    for k, v in sorted(dict(LAVAPIPE, **(env or {})).items()):
        put += ["--putenv", "%s=%s" % (k, v)]
    return guest_argv("run", *put, "--exe", exe, "--wait-stdout",
                      "--wait-stderr", "--", *args)


_mgr = None


def _manager(cls):
    """The API's manager, one a process and kept for the length of it.

    It shuts the API down when it is collected, and every object got from it
    then answers "unexpected error" -- so it is not a local. Shut down by
    hand at exit, because left to the interpreter's teardown it runs after
    the import machinery it needs has gone."""
    global _mgr
    if _mgr is None:
        _mgr = cls(None, None)
        atexit.register(_mgr.deinit)
    return _mgr


class Guest(Desk):
    """The machine's devices and the agent on its desktop, together: what
    windows_driver.Desk asks, and the emulated mouse and keyboard under
    it."""

    def __init__(self):
        self.session = None
        self.sock = None
        self.agent = None
        self.buttons = 0
        self.keys = set()
        try:
            self._open_machine()
            self._open_agent()
        except Exception:
            self.close()
            raise

    # ---- getting in --------------------------------------------------------

    def _open_machine(self):
        if not shutil.which("VBoxManage"):
            raise Unreachable("no VBoxManage on this machine")
        try:
            from vboxapi import VirtualBoxManager
        except ImportError:
            raise Unreachable("no vboxapi for this python -- it comes with "
                              "the VirtualBox SDK")
        try:
            if os.path.getsize(password_file()) == 0:
                raise OSError
        except OSError:
            raise Unreachable(
                "no password for the guest's %r in %s -- one line, mode 600"
                % (USER, password_file()))
        mgr = _manager(VirtualBoxManager)
        try:
            machine = mgr.getVirtualBox().findMachine(VM)
        except Exception:
            raise Unreachable("no VirtualBox machine %r; FNL_DRIVE_VM names "
                              "one" % VM)
        if machine.state != mgr.constants.MachineState_Running:
            raise Unreachable("the machine %r is not running" % VM)
        # Asked here and not left to the agent's silence: without it the
        # agent's connection is dropped, not refused, and its own timeout
        # is as long as the wait for it.
        nic = machine.getNetworkAdapter(0)
        if (nic.attachmentType == mgr.constants.NetworkAttachmentType_NAT
                and not nic.NATEngine.localhostReachable):
            raise Unreachable(
                "the machine %r hides this host's loopback from its guest, "
                "so the agent cannot call back -- power it off and "
                "VBoxManage modifyvm %s --nat-localhostreachable1 on"
                % (VM, VM))
        # Shared: the machine belongs to the window it is running in, and
        # this only borrows its console.
        self.session = mgr.getSessionObject()
        machine.lockMachine(self.session, mgr.constants.LockType_Shared)
        console = self.session.console
        self.mouse = console.mouse
        self.keyboard = console.keyboard
        if not self.mouse.absoluteSupported:
            raise Unreachable(
                "the guest takes no absolute pointer -- the Guest Additions' "
                "mouse integration is what offers one")

    def _open_agent(self):
        mk = manage(*guest_argv("mkdir", "--parents", GUEST_DIR)[1:])
        if mk.returncode != 0 and "not able to logon" in mk.stderr:
            raise Unreachable(
                "the guest refused %r with the password in %s"
                % (USER, password_file()))
        self.install(AGENT)
        srv = socket.socket()
        try:
            # The guest reaches this as 10.0.2.2: VirtualBox's NAT shows it
            # the host's loopback, where the machine's settings allow that,
            # and nothing else of the host.
            srv.bind(("127.0.0.1", 0))
            srv.listen(1)
            srv.settimeout(30.0)
            self.agent = subprocess.Popen(
                run_argv(POWERSHELL, [
                    "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-WindowStyle", "Hidden", "-File",
                    GUEST_DIR + "\\" + os.path.basename(AGENT),
                    str(srv.getsockname()[1])]),
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            try:
                sock, _ = srv.accept()
            except socket.timeout:
                self.agent.kill()
                said = self.agent.communicate()[1].strip().splitlines()
                raise Unreachable(
                    "the agent never called back from the guest -- is %r "
                    "logged in to its desktop, and can the guest reach "
                    "10.0.2.2? (%s)"
                    % (USER, said[-1] if said else "it said nothing"))
        finally:
            srv.close()
        self._greet(sock)

    def run_argv(self, exe, args=(), env=None):
        return run_argv(exe, args, env)

    def install(self, path):
        """Copy a file into GUEST_DIR; returns its path there."""
        # The trailing separator is what says "into", and the source has to
        # be absolute: it is resolved by VirtualBox's service, not from here.
        cp = manage(*guest_argv("copyto", "--target-directory",
                                GUEST_DIR + "\\", os.path.abspath(path))[1:],
                    timeout=120)
        if cp.returncode != 0:
            raise Unreachable("could not copy %s into the guest: %s"
                              % (os.path.basename(path),
                                 "".join(cp.stderr.strip().splitlines()[-1:])))
        return GUEST_DIR + "\\" + os.path.basename(path)

    def close(self):
        if self.buttons and self.sock:
            self.buttons = 0
            try:
                self._here()
            except Exception:
                pass
        if self.session:
            self._keys_up()
            # And whatever the machine's keyboard still holds that this
            # did not press or could not release.
            try:
                self.keyboard.releaseKeys()
            except Exception:
                pass
        self._hang_up()
        if self.agent:
            try:
                self.agent.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.agent.kill()
                self.agent.wait()
            self.agent.stderr.close()
            self.agent = None
        if self.session:
            try:
                self.session.unlockMachine()
            except Exception:
                pass
            self.session = None

    # ---- the machine's mouse and keyboard ----------------------------------

    def warp(self, x, y):
        """Put the pointer at a screen pixel. The device counts from one."""
        self.mouse.putMouseEventAbsolute(int(x) + 1, int(y) + 1, 0, 0,
                                         self.buttons)

    def nudge(self, dx, dy):
        """Relative motion from the emulated mouse itself, which is what raw
        input counts and so all a captured pointer hears."""
        self.mouse.putMouseEvent(int(round(dx)), int(round(dy)), 0, 0,
                                 self.buttons)

    # A button or a wheel step goes out as an absolute report at the place
    # the pointer already is. The relative device takes them too, and the
    # guest then holds the press back until the next absolute report: a
    # click arrives as a release, and its press on the following move.
    def _here(self, dz=0):
        x, y = self.pointer_pos()
        self.mouse.putMouseEventAbsolute(x + 1, y + 1, dz, 0, self.buttons)

    def button(self, btn=BTN_LEFT, pressed=True):
        bit = BUTTON_BITS[btn]
        self.buttons = (self.buttons | bit) if pressed \
            else (self.buttons & ~bit)
        self._here()

    def scroll(self, steps):
        self._here(int(steps))

    def keycode(self, code, pressed=True):
        seq = scancodes(code, pressed)
        if seq:
            self.keyboard.putScancodes(seq)

    def want_screen(self, w, h, timeout=15.0):
        """Have the guest's screen be at least that large, by asking the
        machine for a video mode if it is not.

        A round drags and grows a 1280x754 window, and a guest's screen is
        whatever size its window on this desktop was last left at. A corner
        dragged off the screen is one no pointer can be put on.

        The screen is the monitor the window will open on, not the desktop:
        two monitors of 1280x960 side by side are 2560 across and no room at
        all. A machine whose window does not resize its guest keeps the mode
        it has, and is then a skipped environment that says which."""
        def screen():
            self._desk = None       # the pointer's clamp moves with the mode
            return self.monitor()[0][3:5]

        dw, dh = screen()
        if dw >= w and dh >= h:
            return
        manage("controlvm", VM, "setvideomodehint", str(w), str(h), "32")
        end = time.time() + timeout
        while time.time() < end:
            dw, dh = screen()
            if dw >= w and dh >= h:
                return
            time.sleep(0.2)         # poll: a mode change takes a second or so
        raise Unreachable("the screen of %r is %dx%d and would not be made "
                          "%dx%d -- with auto-resize off for its window, "
                          "that is the guest's own display setting"
                          % (VM, dw, dh, w, h))

    def want_monitors(self, n=2, second=(1600, 1200), timeout=15.0):
        """Have the guest show `n` monitors, by asking the machine to connect
        the second screen if it has not.

        The machine is set for two (--monitorcount 2), and Windows still
        comes up on one more often than not, which skips the round's move
        between outputs without a word. What brings the second back is the
        machine's screen layout -- every screen, the first as it is, the
        second right of it at `second` -- which is what the window's
        "Virtual Screen 2" menu does. Not setvideomodehint with "enabled",
        and not DisplaySwitch /extend from inside: both were tried against
        a guest on one screen and changed nothing, and on Windows 10 the
        second leaves its Project pane holding the foreground, so that no
        window started after it is given the keyboard. A machine that still
        shows fewer is a skipped environment that says so."""
        mons = self.monitors()
        if len(mons) >= n:
            return
        _name, _pri, _x, _y, w, h = mons[0]
        sw, sh = second
        got = manage("controlvm", VM, "setscreenlayout",
                     "0", "primary", "0", "0", str(w), str(h), "32",
                     "1", "on", str(w), "0", str(sw), str(sh), "32")
        end = time.time() + timeout
        while got.returncode == 0 and time.time() < end:
            if len(self.monitors()) >= n:
                self._desk = None   # the pointer's clamp is the union
                return
            time.sleep(0.2)         # poll: the guest answers in a second
        raise Unreachable("%r shows %d monitor(s) and would not be given %d"
                          " -- %s" % (VM, len(self.monitors()), n,
                                      got.stderr.strip() or "the layout was "
                                      "accepted and not shown"))

    # ---- a picture ---------------------------------------------------------

    def screenshot(self, path):
        """The guest's whole screen, as the machine's display holds it: what
        Windows composed, taken from outside Windows."""
        got = manage("controlvm", VM, "screenshotpng", path)
        if got.returncode != 0:
            raise RuntimeError("no screenshot: %s" % got.stderr.strip())
        return path
