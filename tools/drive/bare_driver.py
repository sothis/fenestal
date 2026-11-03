# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A machine of its own, with no desktop: an X server nobody manages, a
compositor nothing surrounds, or no window server at all.

Every other X environment is a window on XWayland under mutter, so every
question this library sends a window manager has had an answer. The X11
backend has a path for the answer never coming -- _NET_SUPPORTING_WM_CHECK
unset, and the titlebar's drag, the edges, fullscreen and maximize done by
the library itself -- and those paths are marked UNVERIFIED in x11.c. This
is where they run: a Linux machine with Xorg on its panel and no window
manager, no desktop and no compositor beside it.

The same machine serves two more rounds. weston, with no panel and no shell
around it, is a compositor that advertises less than GNOME does, so the
Wayland backend's fallbacks for a missing global run there
(wayland-vulkan-bare). And with neither server up the panel belongs to
whoever opens the DRM node first, which is the KMS backend
(kms-vulkan-bare).

The Windows trees run there too, under wine, on the X server
(win64-vulkan-bare) and on weston (win64-vulkan-wl-bare). Wine has no
driver for a display with no server on it, so its KMS round puts the
least server there is between it and the kernel: cage, a kiosk compositor
on the DRM node with the program its one window (win64-vulkan-cage-bare).

    python3 tools/drive/bare_driver.py --status <name>
    python3 tools/drive/bare_driver.py --setup <name> [user@address]
    python3 tools/drive/bare_driver.py --up <name>      # the X server
    python3 tools/drive/bare_driver.py --weston <name>  # weston instead
    python3 tools/drive/bare_driver.py --cage <name>    # or cage
    python3 tools/drive/bare_driver.py --down <name>    # any, and all
    python3 tools/drive/bare_driver.py --driver <name> nouveau|nvidia

--driver moves the machine's NVIDIA GPU to that kernel driver, with no
reboot (tools/gpu-driver.sh, as root), and names whatever holds it open
instead. A KMS round named `kms-vulkan-bare@<name>:nouveau` does the same
before it starts, with the servers stopped, and writing drivers after a
machine's address in bare-hosts (`schenker user@schenker nouveau nvidia`)
makes a KMS round on each. Under Xorg, weston or cage the GPU may be the
server's, so those rounds run on the driver the machine booted.

The machine is reached over ssh with this user's keys, twice:

  As root, for the X server alone. Xorg is started on DISPLAY_NO, on VT,
  as a transient systemd unit (UNIT), so that it outlives the ssh command
  that asked for it and is stopped by name. Root because a server started
  from an ssh session is on no seat, and logind will not hand one the
  devices. It is left running between rounds; --down stops it.

  As the account the program runs as, over one held connection that every
  command, every copy and the program itself are multiplexed over, and
  that forwards a loopback port here to the X server's socket there. So
  the questions a round asks -- the rectangle, the focus, the properties,
  the pixels -- are x11_driver's, put by python-xlib on this machine to
  that server, and nothing listens on the network at either end.

weston is started the way Xorg is, by root and as a transient unit
(WESTON_UNIT), on the same VT, but as a login session of the account's: the
unit opens one through PAM, so logind hands weston the seat's devices and the
program reaches it at WAYLAND_SOCKET in the account's runtime directory. One
of the two runs at a time, since both want the VT and the display, and
asking for either stops the other.

cage is started as weston is, as a login session on the same VT (CAGE_UNIT),
and one of the three runs at a time. A kiosk ends with its program, so the
program cage is given is a shell that writes down the socket cage made and
sleeps; the program under test is a second client, the one cage shows.
cage has no settings: the keyboard's layout is the environment's
(XKB_DEFAULT_*), and the pointer is accelerated with no way to turn that
off, so the hands there are an absolute pointer (AbsUinputHands). It shows
a window across every output it lights, so the outputs but the round's are
had unplugged while it is up (force_output()) and plugged back after.

A Windows program is run by wine, from a prefix in /tmp (WINE_PREFIX): made
the first time a round asks for it after a boot, and in memory, since a
prefix is some 380 MB and a live key's persistent space is not much more.
Its .dll files go with it, into a directory of its renderer's own. Every
round ends with the prefix's wine server killed (kill_stale()): it outlives
its program by seconds with a display driver in it, and the next program
would join it on the last round's server.

Input to weston and to the bare display is uinput (uinput_hands.py), run
there as root: two devices the kernel offers like any others. Input to the
X server is XTEST, through the same forwarded connection. The rule against it
in x11_driver.py is about *this* desktop: XWayland here runs with
-enable-ei-portal, and a fake_input raises a consent dialog. The server this
module speaks to is a plain Xorg on another machine, with no portal to ask
and nobody else's window to type into, so a fake key there goes to the X
server's focus and nowhere else. Hands is built only on a Display this
module opened to that port, and never on one named by $DISPLAY.

The program under test is the remote shell's last command, so the ssh
process here is what testbed.py holds: its stderr is the program's echo, and it
exits with the program's code.

The machines are FNL_DRIVE_BARE's, commas between them, or else the lines of
HOSTS_FILE. An entry is a name for the machine and then the ssh destination
it is reached at, `thinkpad user@thinkpad`; with no destination the
name is one ~/.ssh/config knows.

A machine is Debian (the thinkpad, installed) or Arch (a machine booted from
its stick, from the boot images' repository), told apart by
/etc/os-release:
DISTROS holds what differs between them -- the package manager and its
names, where Xorg and lavapipe are -- and nothing else here names either.

A machine is x86-64 or 64-bit ARM (the Raspberry Pi 4, Arch Linux ARM),
told apart by `uname -m`: TREES says which tree's programs a round sends
to each, and what is not built for a processor is a skipped environment
there -- the Windows trees on ARM, which have no wine to run under.
"""

import contextlib
import importlib.util
import os
import re
import secrets
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

from testbed import ROOT, STATE_DIR, ssh_control

HOSTS_FILE = os.path.join(STATE_DIR, "bare-hosts")
# The helper that moves an NVIDIA GPU between its two kernel drivers, sent
# to a round's machine as it is here, and installed there by --setup for
# the library's own switch (FNL_GPU_DRIVER), which the account reaches
# through systemd's socket in front of it, as a member of its group, or
# else through sudo.
GPU_DRIVER = os.path.join(ROOT, "tools", "gpu-driver.sh")
GPU_DRIVER_THERE = "/usr/local/sbin/fenestal-gpu-driver"
GPU_DRIVER_SOCKET = "/run/fenestal-gpu-driver.sock"
GPU_DRIVER_GROUP = "fenestal-gpu"
GPU_DRIVERS = ("nouveau", "nvidia")
# NVIDIA's Vulkan driver, implicit layer and EGL vendor, which the keys'
# boot puts aside under the stash wherever the NVIDIA GPU starts on
# nouveau (made in the boot images' repository).
# Bare.nvidia_userspace().
NVIDIA_USERSPACE = "/usr/local/bin/fenestal-nvidia-userspace"
NVIDIA_STASH = "/var/lib/fenestal/nvidia-off"
NVIDIA_FILES = ("/usr/share/vulkan/icd.d/nvidia_icd.json",
                "/usr/share/vulkan/implicit_layer.d/nvidia_layers.json",
                "/usr/share/glvnd/egl_vendor.d/10_nvidia.json")
# How lately a hand may have been at the machine's console, in seconds, for
# a round to leave it alone: a round takes the panel to its own VT.
# FNL_DRIVE_BARE_BUSY=0 is "it is mine, go on".
BUSY = float(os.environ.get("FNL_DRIVE_BARE_BUSY", "120"))
# Not :0, which is the one `startx` takes, and not on the VT a getty or a
# root console is sitting on.
DISPLAY_NO = 9
VT = 7
UNIT = "fenestal-xorg"
WESTON_UNIT = "fenestal-weston"
WAYLAND_SOCKET = "wayland-fenestal"
CAGE_UNIT = "fenestal-cage"
# Where the shell cage runs writes the socket name cage chose, in DRIVE_DIR:
# cage has no option to name it.
CAGE_SOCKET_FILE = "cage-socket"
# The outputs had unplugged for cage (cage_up()), one a line, in DRIVE_DIR:
# what cage_down() plugs back, whichever connection started it.
CAGE_FORCED_FILE = "cage-forced"
# The wine prefix there, by the account's uid; WINE_PREFIX's paragraph above
# says why /tmp. Its parent must be the account's own for wine to make it,
# which /tmp is not, so the directory is made first.
WINE_PREFIX = "/tmp/fenestal-wine-%s"
# Under the account's home, so that nothing the round leaves needs root to
# clear: the binaries, the server's cookie, the suppressions file.
DRIVE_DIR = "fenestal-drive"
# What differs between the two distributions a machine may run. `packages`
# is what the machine needs on top of a minimal install with X: Xorg, its
# cookie, xrandr and setxkbmap; the Vulkan loader and Mesa's drivers, lavapipe
# among them; vulkan-tools for --status; xclip, the clipboard's other
# program; the layers a checker round validates under; weston, the Wayland
# round's compositor; xkeyboard-config's files, which the KMS backend reads
# its layout from (Xorg brings them anyway); python3, which
# runs uinput_hands.py and kms_grab.py; wine, which runs the Windows trees;
# libXcomposite, which wine opens at run time and neither distribution's
# wine depends on -- without it winex11 never redirects a child window it
# copies from, so a window presenting into one (every Win32 window after a
# GPU move) shows the frame it left for good; cage, wine's compositor on
# the bare display; and grim, its picture.
# `versions` names the packages the round's facts line quotes, as xorg,
# weston and mesa. `lavapipe` and `nvidia` are the drivers kept loaded under
# ASan (see LAVAPIPE_WHY), the latter NVIDIA's ICD, where its packages put
# it; `dbus` and `egl_mesa` are what is kept in its place in a round that
# moves the GPU between kernel drivers. `debuginfod` is where --setup
# fetches the drivers' and layers' debug info (DEBUGINFO_WHY).
DISTROS = {
    "debian": {
        "packages": ("xserver-xorg-core", "xauth", "x11-xserver-utils",
                     "libvulkan1", "mesa-vulkan-drivers", "vulkan-tools",
                     "vulkan-validationlayers", "xclip", "weston",
                     "xkb-data", "python3", "wine", "wine64",
                     "libxcomposite1", "cage", "grim"),
        "install": "DEBIAN_FRONTEND=noninteractive apt-get install -y "
                   "--no-install-recommends %s",
        "has": "dpkg-query -W -f '${Status}' \"$1\" 2>/dev/null | "
               "grep -q 'ok installed'",
        "version": "dpkg-query -W -f '${Version}' \"$1\" 2>/dev/null",
        "versions": ("xserver-xorg-core", "weston", "mesa-vulkan-drivers"),
        "xorg": "/usr/lib/xorg/Xorg",
        "lavapipe": "/usr/lib/x86_64-linux-gnu/libvulkan_lvp.so",
        "nvidia": "/usr/lib/x86_64-linux-gnu/libGLX_nvidia.so.0",
        "dbus": "/usr/lib/x86_64-linux-gnu/libdbus-1.so.3",
        "egl_mesa": "/usr/lib/x86_64-linux-gnu/libEGL_mesa.so.0",
        "radeon": "/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so",
        "debuginfod": "https://debuginfod.debian.net",
    },
    "arch": {
        "packages": ("xorg-server", "xorg-xauth", "xorg-xrandr",
                     "xorg-setxkbmap", "xorg-xkbcomp", "vulkan-icd-loader",
                     "vulkan-swrast", "vulkan-tools",
                     "vulkan-validation-layers", "xclip", "weston",
                     "xkeyboard-config", "python", "wine", "libxcomposite",
                     "cage", "grim"),
        # -y: a live key has no sync database until it fetches one. Only
        # what is missing is asked for (INSTALL), which on the key is
        # nothing.
        "install": "pacman -Sy --needed --noconfirm %s",
        "has": "pacman -Q \"$1\" >/dev/null 2>&1",
        "version": "pacman -Q \"$1\" 2>/dev/null | cut -d' ' -f2",
        "versions": ("xorg-server", "weston", "mesa"),
        "xorg": "/usr/lib/Xorg",
        "lavapipe": "/usr/lib/libvulkan_lvp.so",
        "nvidia": "/usr/lib/libGLX_nvidia.so.0",
        "dbus": "/usr/lib/libdbus-1.so.3",
        "egl_mesa": "/usr/lib/libEGL_mesa.so.0",
        "radeon": "/usr/lib/libvulkan_radeon.so",
        "debuginfod": "https://debuginfod.archlinux.org",
    },
}
# The trees a processor runs, by the tree an environment names: the Linux
# tree's programs come from linux-arm64 on ARM, built by its cross gcc
# (build.sh), and nothing of win64 runs there -- Arch Linux ARM has no
# wine. ELF's e_machine for each, which install() checks a program by.
TREES = {
    "x86_64": {"linux": "linux", "win64": "win64"},
    "aarch64": {"linux": "linux-arm64"},
}
ELF_MACHINES = {"x86_64": 0x3E, "aarch64": 0xB7}
SANITIZER_RUNTIME = r"lib(?:asan|tsan|ubsan|lsan)\.so\.\d+"
# Packages DISTROS lists that a processor's distribution does not offer,
# or that only wine wants where there is none, left out of what --setup
# installs and --status asks after.
NOT_OFFERED = {"aarch64": ("wine", "wine64", "libxcomposite",
                           "libxcomposite1")}
HANDS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "uinput_hands.py")
GRAB = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "kms_grab.py")
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]
# The names testbed.WINDOW_NAMES gives the programs, for pkill there.
PROGRAMS = ("fenestal-testbe", "fenestal-dev", "fenestal-dev.ex")
# What the remote shell says before it becomes the program: X answers
# _NET_WM_PID with the pid there, and nothing here knows it otherwise.
PID_TAG = "FNL_REMOTE_PID "
# What the remote shell says when a program it waited for has exited, and
# with what code: a wine program's alone (run_argv() says why).
EXIT_TAG = "FNL_REMOTE_EXIT "
# LAVAPIPE_WHY: lavapipe, which every machine of these can draw with, is
# kept loaded under ASan. The loader unloads it at vkDestroyInstance, and the
# objects its globals still point at are then reported as leaked: two
# 56-byte blocks from an "unknown module", gone with this preloaded. Pinned,
# every leak is still looked for, and a frame in the driver can be named.
# NVIDIA's ICD is kept so too, where its kernel driver is up: unloaded, the
# D-Bus connection and xcb replies its globals hold read as 790 bytes leaked
# from an "unknown module" (GTX 1050 Ti, 580.178.04). Not where nouveau has
# the GPU, though the library is installed: a round on NVK is not to carry a
# driver it never loads. Nor in a round whose program moves the GPU between
# kernel drivers (Bare.moves_driver): kept loaded, NVIDIA's userspace keeps
# NVIDIA's device open, and the library refuses every move off its module.
# There libdbus is kept instead, which NVIDIA's driver opens at run time for
# the connection it never closes: kept, its frames are named where that
# connection is reported (schenker, not aspire), which lsan.supp keys on.
# And Mesa's EGL, which NVIDIA's Vulkan driver loads through glvnd to find
# its GPU and the loader unloads with it: its device list, drmGetDevices2's,
# is otherwise lost at every instance (4 KB, aspire). The library keeps
# Mesa's EGL itself after its own spells (static TLS, DESIGN *OpenGL, and
# glvnd closed between its spells*); it holds no device. Where each is, is
# DISTROS'. Where such a round starts on NVIDIA's driver no leak is looked
# for at all: NVIDIA's Vulkan driver, opened and unloaded with nothing of it
# kept, leaves blocks whose every frame is in a module gone by exit (two of
# 56 bytes and one of 16 under its pthread_once, 580.196.07), and Intel's
# hasvk one of 56 below a frame the same. None names a frame of the program,
# so none could be suppressed by one; memory errors are still looked for,
# and the same moves are leak-checked by the round that starts on nouveau.
# What a lavapipe machine's ThreadSanitizer is told beside tsan.supp; the
# file says why.
TSAN_EXTRA = os.path.join(ROOT, "tsan-lavapipe.supp")
# And the Pi's V3D driver, kept loaded under ASan where it is installed, for
# LAVAPIPE_WHY's reason: unloaded at exit, it takes libdrm with it, and a
# leak made inside either reads as an "unknown module". RADV is kept so too
# where the machine has an AMD GPU (DISTROS' "radeon"): this desktop's
# console, where Mesa's display-WSI leak (lsan.supp) is keyed on a
# function that only a loaded driver's frames can name.
ALSO_PINNED = ("/usr/lib/libvulkan_broadcom.so",)
# Under ThreadSanitizer the drivers a report's frames are in are loaded
# before the program starts, in its LD_PRELOAD behind TSan's runtime. GCC's
# sanitizers symbolize through libbacktrace, which reads the list of loaded
# libraries once, at the first frame it names, and never again: a driver
# the loader or EGL opens after that is <null> for the rest of the process,
# its debug info installed or not, and an entry of tsan.supp keyed on one
# of its functions never matches. On schenker's nouveau (2026-10-31, Mesa
# 26.2.6) every innermost NVK frame read so -- a report at the start, in
# libgallium, came first -- while __sanitizer_symbolize_pc() named the same
# addresses whole, the inline chain under the fortified memcpy included,
# in a process that loaded NVK before its first. Matched by name in the
# machine's own library directory: the open drivers whose debug info
# --setup fetches (DEBUGINFO), never NVIDIA's closed one. Not the
# validation layer: it exports the loader's own entry points
# (vkGetInstanceProcAddr, vkEnumerateInstanceExtensionProperties), which
# preloaded take the loader's place for every caller -- Zink's instance
# then failed at its first call -- where a driver exports only the
# vk_icd* names the loader looks up in it alone.
TSAN_PRELOADED = ("libvulkan_*.so", "libgallium*.so", "libEGL_mesa.so.0")


# The prefix made if it is not there, and set up: made with no display, by
# the null driver, so nothing is shown, and without .NET and the browser
# engine, whose installers are dialogs and which no program here loads.
# Then Managed=N, wine's own setting for an X server with no window
# manager: with it wine places, sizes and maximizes its windows itself
# (override-redirect); without it, it asks a manager that is not there,
# and a maximized window stays the size it was. Nothing in it is read on
# Wayland. Says "made" or "there", or what went wrong.
WINE_PREFIX_UP = r"""
export WINEPREFIX=%(pfx)s WINEDEBUG=-all
made=there
if ! test -f "$WINEPREFIX/system.reg"; then
    mkdir -p -m 700 "$WINEPREFIX" || exit 1
    env -u DISPLAY -u WAYLAND_DISPLAY WINEDLLOVERRIDES='mscoree,mshtml=' \
        wineboot -i >/dev/null 2>&1
    wineserver -w
    test -f "$WINEPREFIX/system.reg" || { echo "wineboot made none"; exit 1; }
    made=made
fi
if ! grep -q '^"Managed"="N"' "$WINEPREFIX/user.reg"; then
    env -u DISPLAY -u WAYLAND_DISPLAY wine reg add \
        'HKCU\Software\Wine\X11 Driver' /v Managed /d N /f >/dev/null 2>&1
    wineserver -w
    grep -q '^"Managed"="N"' "$WINEPREFIX/user.reg" ||
        { echo "Managed=N was not written"; exit 1; }
fi
echo $made
"""


class DriverMoveFailed(RuntimeError):
    """The GPU would not go to the kernel driver a round asked for, with
    nothing holding it: the helper's fault or the kernel's, and a failure,
    where a GPU held open is a machine in use and Unreachable."""


class Unreachable(RuntimeError):
    """The machine cannot be driven now: off, no key, no X server."""


def listed():
    """{name: ssh destination}, in the order written.

    A name FNL_DRIVE_BARE gives without a destination takes the one
    HOSTS_FILE has for it, so `FNL_DRIVE_BARE=thinkpad-live` picks that
    machine out of the file rather than reading as an ssh host."""
    try:
        with open(HOSTS_FILE) as f:
            written = ",".join(ln.split("#")[0] for ln in f)
    except OSError:
        written = ""

    def parse(text, known=None):
        found = {}
        for entry in text.split(","):
            words = entry.split()
            if words:
                dest = words[1] if len(words) > 1 and "=" not in words[1] \
                    else None
                found.setdefault(words[0], dest or (known or {}).get(
                    words[0], words[0]))
        return found

    filed = parse(written)
    named = os.environ.get("FNL_DRIVE_BARE")
    return filed if named is None else parse(named, filed)


def hosts():
    return tuple(listed())


def drivers(name):
    """The kernel drivers a round on `name` runs on, one round each: the
    words after its address in HOSTS_FILE, or the one FNL_DRIVE_BARE_DRIVER
    names for every machine (set to nothing, none). () is one round on
    whatever the machine has bound."""
    asked = os.environ.get("FNL_DRIVE_BARE_DRIVER")
    if asked is not None:
        return tuple(w for w in asked.replace(",", " ").split()
                     if w in GPU_DRIVERS)
    for ln in _lines(HOSTS_FILE):
        words = ln.split("#")[0].split()
        if words and words[0] == name:
            return tuple(w for w in words[2:] if w in GPU_DRIVERS)
    return ()


def machine_env(name):
    """{variable: value} for the `KEY=VALUE` words of HOSTS_FILE's line for
    `name`: what that machine's programs start with unless this desk's own
    environment names the variable too. It is how a setting belongs to a
    machine and not to whoever started the round -- `LP_NUM_THREADS=2` on
    the thinkpad, whose lavapipe at four threads has reset it (TODO.md
    *The thinkpad's resets*)."""
    for ln in _lines(HOSTS_FILE):
        words = ln.split("#")[0].split()
        if words and words[0] == name:
            return dict(w.split("=", 1) for w in words[1:]
                        if "=" in w and not w.startswith("="))
    return {}


def as_root(dest):
    """The same machine, as root: the destination with its user swapped."""
    return "root@" + dest.rsplit("@", 1)[-1]


def _ssh(dest, command, timeout=60, ctl=None, data=None):
    argv = ["ssh"] + (["-S", ctl] if ctl else []) + SSH_OPTS + [dest,
                                                                 command]
    return subprocess.run(argv, input=data, capture_output=True,
                          timeout=timeout,
                          stdin=None if data is not None
                          else subprocess.DEVNULL)


def _text(res):
    return res.stdout.decode("utf-8", "replace")


def _free_display():
    """A display number whose TCP port nothing here holds: 6000 + n is the
    port python-xlib connects to for 127.0.0.1:n."""
    import socket
    for n in range(50, 100):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", 6000 + n))
            return n
        except OSError:
            continue
        finally:
            s.close()
    raise Unreachable("no display port free between 6050 and 6099")


def packages(distro, arch):
    """DISTROS' packages less what `arch`'s distribution has not got."""
    return tuple(p for p in distro["packages"]
                 if p not in NOT_OFFERED.get(arch, ()))


def _elf_machine(path):
    """ELF's e_machine of `path`, or None when it is not ELF."""
    try:
        with open(path, "rb") as f:
            head = f.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    return int.from_bytes(head[18:20], "little" if head[5] == 1 else "big")


def distro_of(os_release):
    """DISTROS' key for a machine, from its /etc/os-release: Arch and what
    is built on it, and Debian for everything else, which is what the
    harness assumed before there was a second."""
    ids = re.findall(r'^ID(?:_LIKE)?="?([^"\n]*)"?', os_release, re.M)
    words = " ".join(ids).split()
    return "arch" if "arch" in words else "debian"


# %(version)s is DISTROS' version command, as a shell function's body.
FACTS = r"""
ver() { %(version)s; }
. /etc/os-release; echo "$PRETTY_NAME, kernel $(uname -r)"
lscpu | sed -n 's/^Model name: *//p'
lspci | sed -n 's/^.*\(VGA\|3D\|Display\)[^:]*: //p'
# The kernel driver on each GPU, and NVIDIA's version where it is NVIDIA's:
# the live key boots one machine on nouveau or on NVIDIA's driver.
for d in /sys/bus/pci/devices/*; do
    case $(cat $d/class) in 0x03*) ;; *) continue;; esac
    drv=$(readlink $d/driver || echo none)
    echo "${d##*/}: kernel driver ${drv##*/}"
done
awk '/^NVRM version/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\./) {
    print "nvidia " $i; exit } }' /proc/driver/nvidia/version 2>/dev/null
echo "xorg $(ver %(xorg)s), weston $(ver %(weston)s), mesa $(ver %(mesa)s)"
if command -v vulkaninfo >/dev/null; then
    vulkaninfo --summary 2>/dev/null |
        sed -n 's/^[[:space:]]*deviceName *= */vulkan: /p'
fi
# The hottest zone and the boot: the machine has reset itself once in a round
# and left no trace, so the next one is read beside both.
t=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -1)
echo "${t:+$((t / 1000)) C, }up since $(uptime -s)"
"""


class Bare:
    """One machine, held open for a round: the connection, and whichever
    server the round is on -- the X server's cookie and forwarded display,
    weston's socket, or neither."""

    def __init__(self, name, dest=None, x=True):
        self.name = name
        self.dest = dest or listed().get(name, name)
        self.dir = tempfile.mkdtemp(prefix="fnl-bare-")
        self.ctl = os.path.join(self.dir, "ssh")
        self.xauth = os.path.join(self.dir, "xauth")
        # Whether the X server's socket is forwarded here: only an X round
        # asks it anything.
        self.x = x
        self.link = None
        self.hands_ = None
        self._mons = None
        # What a program there is started with to reach its server; see
        # run_argv(). Set by whichever of x_up(), weston_up() and
        # bare_display() the round asked for.
        self.display_env = None
        # The kernel driver the round starts the GPU on where the program
        # it installs may move the GPU between drivers (FNL_GPU_DRIVER),
        # and "" where not: what install() keeps loaded under ASan, and
        # whether leaks are looked for, depend on it (LAVAPIPE_WHY). Set
        # by the round.
        self.moves_driver = ""
        # The pids of the xclips clipboard_put() started there, which
        # clipboard_done() ends and nothing else does.
        self.xclips = []
        try:
            self._open_link()
        except Exception:
            self.close()
            raise

    # ---- getting in --------------------------------------------------------

    def _open_link(self):
        if not shutil.which("ssh") or (self.x and not shutil.which("xauth")):
            raise Unreachable("no ssh or no xauth on this machine")
        said = os.path.join(self.dir, "ssh.err")
        # Another sweep's link -- one a machine, started together -- may
        # take the port _free_display() found between the look and ssh's
        # bind, which ExitOnForwardFailure reports; the next free one then.
        for attempt in range(3):
            fwd = []
            if self.x:
                self.dno = _free_display()
                fwd = ["-L", "127.0.0.1:%d:/tmp/.X11-unix/X%d"
                       % (6000 + self.dno, DISPLAY_NO)]
            if os.path.exists(self.ctl):
                os.unlink(self.ctl)
            with open(said, "w") as err:
                self.link = subprocess.Popen(
                    ["ssh", "-M", "-S", self.ctl, "-N"] + SSH_OPTS
                    + ["-o", "ExitOnForwardFailure=yes",
                       "-o", "ServerAliveInterval=15"] + fwd + [self.dest],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=err)
            end = time.time() + 15.0
            why = None
            while time.time() < end:
                if self.link.poll() is not None:
                    with open(said) as f:
                        why = f.read().strip().splitlines() or \
                            ["it said nothing"]
                    break
                if os.path.exists(self.ctl) and ssh_control(
                        self.ctl, "check", self.dest):
                    break
                time.sleep(0.05)    # poll: the handshake is a round trip
            else:
                raise Unreachable("ssh to %s did not come up" % self.dest)
            if why is None:
                break
            if "forwarding" in why[-1] and attempt < 2:
                continue
            raise Unreachable("no ssh to %s: %s" % (self.dest, why[-1]))
        # A laptop booted into its other system answers ssh too, in a
        # shell that is not a POSIX one: refused here with what it is,
        # rather than read as a home, a name and a uid.
        system = self.sh("uname -s", check=False).strip()
        if system != "Linux":
            raise Unreachable("%s is not running Linux (uname -s: %s) -- "
                              "tools/drive/power.py boot %s stick"
                              % (self.name, system.splitlines()[-1][:60]
                                 if system else "nothing", self.name))
        home, self.user, self.uid = self.sh(
            "echo $HOME; id -un; id -u").split()
        self.there = "%s/%s" % (home, DRIVE_DIR)
        self.distro = DISTROS[distro_of(
            self.sh("cat /etc/os-release", check=False))]
        self.arch = self.sh("uname -m").strip()

    def tree(self, tree):
        """The tree whose programs run here for an environment of `tree`
        (TREES); raises Unreachable when none is built for this
        processor."""
        got = TREES.get(self.arch, {}).get(tree)
        if not got:
            raise Unreachable("%s is %s, and no %s tree is built for it"
                              % (self.name, self.arch, tree))
        return got

    def sh(self, command, timeout=60, data=None, check=True):
        """A shell command as the account, over the held connection; its
        stdout as text."""
        res = _ssh(self.dest, command, timeout=timeout, ctl=self.ctl,
                   data=data)
        if check and res.returncode != 0:
            raise Unreachable("%s on %s: %s"
                              % (command.split()[0], self.name,
                                 res.stderr.decode("utf-8",
                                                   "replace").strip()))
        return _text(res)

    def close(self):
        if self.hands_:
            self.hands_.close()
            self.hands_ = None
        if self.link and self.link.poll() is None:
            ssh_control(self.ctl, "exit", self.dest)
            try:
                self.link.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.link.kill()
        self.link = None
        if self.dir:
            shutil.rmtree(self.dir, ignore_errors=True)
            self.dir = None

    def busy(self):
        """Why a hand is at the machine, or "" when none has been lately.

        A console login's idle time, as `who -u` reads it off the tty: what
        only a person typing there moves. Not the X server's own, which the
        last round's XTEST moved as well -- so a hand at the X server
        itself, between two rounds, is not seen."""
        if BUSY <= 0:
            return ""
        for ln in self.sh("who -u", check=False).splitlines():
            f = ln.split()
            # VT is the harness's own: weston's login session sits there,
            # and its idle time is the last round's keys.
            if len(f) < 5 or not f[1].startswith("tty") or f[1] == "tty%d" % VT:
                continue
            # The idle time follows the login's HH:MM, after a date that is
            # one field or three by the locale there.
            at = next((i for i in range(2, len(f) - 1)
                       if re.fullmatch(r"\d{1,2}:\d\d", f[i])), None)
            if at is None:
                continue
            idle = f[at + 1]
            secs = (0 if idle == "." else float("inf") if idle == "old"
                    else sum(int(v) * k for v, k in
                             zip(idle.split(":"), (3600, 60), strict=False)))
            if secs < BUSY:
                return ("%s is logged in on %s and was at it %s -- "
                        "FNL_DRIVE_BARE_BUSY=0 to drive it anyway"
                        % (f[0], f[1], "just now" if idle == "."
                           else idle + " ago"))
        return ""

    def gpu_driver(self, driver, userspace=True):
        """The NVIDIA GPU here on `driver`, by tools/gpu-driver.sh as root:
        what a KMS round named `@machine:<driver>` runs on, its servers
        stopped first, with NVIDIA's userspace in place unless
        `userspace` is false (nvidia_userspace()). The script is sent as it
        is in this tree, so a round needs nothing installed for it.
        Anything holding the GPU (the helper's status 2), or no
        connection, is a skipped environment, named; any other refusal is
        DriverMoveFailed."""
        with open(GPU_DRIVER, "rb") as f:
            res = _ssh(as_root(self.dest), "sh -s %s" % shlex.quote(driver),
                       timeout=90, data=f.read())
        said = (_text(res) + res.stderr.decode("utf-8", "replace")).strip()
        if res.returncode == 0:
            print("  " + said.splitlines()[-1].replace(
                "fenestal-gpu-driver: ", "kernel driver: "), flush=True)
            if userspace:
                self.nvidia_userspace(True)
            return
        why = ("the GPU on %s did not move to %s: %s"
               % (self.name, driver, " ".join(said.split())))
        if res.returncode in (2, 255):
            raise Unreachable(why)
        raise DriverMoveFailed(why)

    def boot_driver(self):
        """The kernel driver this boot gives the NVIDIA GPU: NVIDIA's where
        the entry loads nvidia_drm (the keys' nv_cmdline), nouveau's where
        it does not."""
        words = self.sh("cat /proc/cmdline", check=False).split()
        return "nvidia" if "modules-load=nvidia_drm" in words else "nouveau"

    def gpu_drivers_bound(self):
        """{address: driver} for each NVIDIA GPU here, "none" for one with
        nothing bound: tools/gpu-driver.sh's list, sent as gpu_driver()
        sends it. {} on a machine with none."""
        with open(GPU_DRIVER, "rb") as f:
            res = _ssh(as_root(self.dest), "sh -s list", timeout=30,
                       data=f.read())
        if res.returncode != 0:
            raise Unreachable("the NVIDIA GPUs on %s were not listed: %s"
                              % (self.name, res.stderr.decode(
                                  "utf-8", "replace").strip()))
        return dict(ln.split()[:2] for ln in _text(res).splitlines()
                    if len(ln.split()) >= 2)

    def as_booted(self):
        """The NVIDIA GPU back on the driver this boot gave it, and its
        userspace where this boot put it: what a round not on a kernel
        driver runs on, whatever a round on one left. Otherwise the order
        of the rounds decides it -- after a round on NVIDIA's module the
        keys' nouveau boot has the GPU on that module with NVIDIA's
        userspace aside, listed by no Vulkan driver. The servers are
        stopped for a move, as a round on a driver has them: Xorg holds
        NVIDIA's GPU open through its X driver."""
        bound = self.gpu_drivers_bound()
        boot = self.boot_driver()
        if any(d != boot for d in bound.values()):
            self.bare_display()
            self.gpu_driver(boot, userspace=False)
        self.nvidia_userspace(False)

    def nvidia_userspace_aside(self):
        """NVIDIA's userspace under the stash, as the keys' nouveau boot
        puts it, whatever this boot does: for a round that must not load
        it (ThreadSanitizer's, Environment.driver_bind()). as_booted()
        puts it back where the boot has it, for the round after."""
        files = " ".join(NVIDIA_FILES)
        res = _ssh(as_root(self.dest),
                   "for f in %s; do if [ -e $f ]; then mkdir -p %s$(dirname "
                   "$f) && mv $f %s$f || exit 1; fi; done"
                   % (files, NVIDIA_STASH, NVIDIA_STASH), timeout=30)
        if res.returncode != 0:
            raise Unreachable("NVIDIA's userspace on %s was not put aside: "
                              "%s" % (self.name, res.stderr.decode(
                                  "utf-8", "replace").strip()))
        print("  NVIDIA's userspace: aside for the round", flush=True)

    def nvidia_userspace(self, wanted):
        """NVIDIA's userspace in place (`wanted`), or where this boot puts
        it, as root; said where anything moved. A round on a kernel driver
        wants it: on the keys' nouveau boot it is aside, and a move onto
        NVIDIA's module would land on no Vulkan driver, NVIDIA's OpenGL
        not be there at all, and the program would draw on the integrated
        GPU. Every other round asks for the boot's, so one cut short
        leaves it in place for no later round; the boot's unit is that
        rule's own, run again. A machine without the unit has no boot that
        puts it aside, so there it is put back either way: only
        nvidia_userspace_aside() moved it."""
        files = " ".join(NVIDIA_FILES)
        back = ("for f in %s; do if [ -e %s$f ] && [ ! -e $f ]; then "
                "mv %s$f $f; fi; done" % (files, NVIDIA_STASH, NVIDIA_STASH))
        count = "n=0; for f in %s; do [ -e $f ] && n=$((n+1)); done; " \
                "echo $n" % files
        res = _ssh(as_root(self.dest),
                   "%s; if [ -x %s ]; then %s; else %s; fi; %s"
                   % (count, NVIDIA_USERSPACE,
                      back if wanted else NVIDIA_USERSPACE, back, count),
                   timeout=30)
        said = _text(res).split()
        if res.returncode != 0 or len(said) != 2:
            raise Unreachable("NVIDIA's userspace on %s was not %s: %s"
                              % (self.name, "put back" if wanted
                                 else "put where the boot has it",
                                 res.stderr.decode("utf-8",
                                                   "replace").strip()))
        before, after = int(said[0]), int(said[1])
        if after > before:
            print("  NVIDIA's userspace: back from %s for the round"
                  % NVIDIA_STASH, flush=True)
        elif after < before:
            print("  NVIDIA's userspace: aside again, as this boot has it",
                  flush=True)

    def discrete_sleeps(self):
        """Whether a GPU here other than the one the firmware booted on is
        left to runtime-suspend: power/control "auto". A 3D controller,
        NVIDIA's on a hybrid laptop, has no boot_vga at all. NVIDIA's own
        driver says for itself whether it powers the GPU off ("Runtime D3
        status" in /proc/driver/nvidia/gpus/<address>/power), since it
        reads "auto" there on a GPU it never suspends: one before Turing."""
        out = self.sh("for d in /sys/bus/pci/devices/*; do "
                      "case $(cat $d/class) in 0x03*) ;; *) continue;; esac; "
                      "[ \"$(cat $d/boot_vga 2>/dev/null)\" = 1 ] && continue; "
                      "nv=/proc/driver/nvidia/gpus/${d##*/}/power; "
                      "if [ -r $nv ]; then "
                      "grep -q 'Runtime D3 status: *Enabled' $nv && "
                      "echo auto; "
                      "else cat $d/power/control; fi; done", check=False)
        return "auto" in out.split()

    def facts(self):
        d = self.distro
        xorg, weston, mesa = d["versions"]
        return [ln for ln in self.sh(FACTS % {
            "version": d["version"], "xorg": xorg, "weston": weston,
            "mesa": mesa}).splitlines() if ln.strip()]

    # ---- the X server ------------------------------------------------------

    def _cookie(self):
        """The server's cookie, made the first time and kept in the account's
        DRIVE_DIR, where the server reads it at start and the program
        through XAUTHORITY."""
        auth = shlex.quote(self.there + "/xauth")
        got = self.sh("mkdir -p %s && umask 077 && touch %s && "
                      "xauth -f %s list" % (shlex.quote(self.there), auth,
                                            auth))
        m = re.search(r"MIT-MAGIC-COOKIE-1\s+([0-9a-f]{32})", got)
        if m:
            return m.group(1)
        cookie = secrets.token_hex(16)
        self.sh("xauth -f %s add :%d MIT-MAGIC-COOKIE-1 %s"
                % (auth, DISPLAY_NO, cookie))
        return cookie

    def x_running(self):
        # The unit as well as the socket, as weston_running() asks.
        return self.sh("systemctl is-active -q %s && "
                       "test -S /tmp/.X11-unix/X%d && echo up"
                       % (UNIT, DISPLAY_NO), check=False).strip() == "up"

    def x_up(self, timeout=20.0):
        """The X server running, started if it is not. Raises Unreachable
        when it will not start, with what the unit said."""
        cookie = self._cookie()
        if not self.x_running():
            self.weston_down()
            self.cage_down()
            argv = ["systemd-run", "--unit=" + UNIT, "--collect", "--quiet",
                    self.distro["xorg"], ":%d" % DISPLAY_NO, "vt%d" % VT,
                    "-nolisten", "tcp", "-noreset", "-auth",
                    self.there + "/xauth",
                    # No blanking: a round that pauses long enough would read
                    # the pixels of a screen saver.
                    "-s", "0", "-dpms"]
            res = _ssh(as_root(self.dest), " ".join(map(shlex.quote, argv)))
            if res.returncode != 0:
                raise Unreachable("Xorg would not start on %s: %s"
                                  % (self.name, res.stderr.decode(
                                      "utf-8", "replace").strip()))
            end = time.time() + timeout
            while not self.x_running():
                if time.time() > end:
                    log = _text(_ssh(as_root(self.dest),
                                     "journalctl -u %s -n 5 -o cat" % UNIT))
                    raise Unreachable("Xorg on %s opened no socket: %s"
                                      % (self.name, log.strip()))
                time.sleep(0.2)     # poll: nothing tells us it is up
        self.arrange()
        # The local file names the forwarded display by the loopback
        # address, which both libxcb and python-xlib then look up as the
        # host's own name; so does xauth when it writes it.
        if os.path.exists(self.xauth):
            os.remove(self.xauth)
        subprocess.run(["xauth", "-q", "-f", self.xauth, "add",
                        "127.0.0.1:%d" % self.dno, "MIT-MAGIC-COOKIE-1",
                        cookie], check=True, capture_output=True)
        self.display_env = self.xenv()

    def _xrandr(self, *args):
        env = " ".join("%s=%s" % (k, shlex.quote(v))
                       for k, v in self.xenv().items())
        return self.sh("env %s xrandr %s" % (env, " ".join(
            map(shlex.quote, args))), check=False)

    def arrange(self):
        """Every connected output on, left to right from the primary, and
        every disconnected one off: one xrandr, so the server is never
        between two layouts.

        With no desktop nobody decides where a second monitor goes. Xorg
        lights what is connected when it starts and leaves a monitor
        plugged in later dark; left to it, the layout a round reads would
        depend on when the cable went in. So it is set here, each time the
        server is asked for, and a round's monitor list is this order. The
        panel stays primary, which is where a round runs unless told."""
        out = self._xrandr()
        on, off, primary = [], [], None
        for ln in out.splitlines():
            m = re.match(r"^(\S+) (connected|disconnected)( primary)?", ln)
            if not m:
                continue
            if m.group(2) == "connected":
                on.append(m.group(1))
                if m.group(3):
                    primary = m.group(1)
            else:
                off.append(m.group(1))
        if not on:
            return
        primary = primary or on[0]
        order = [primary] + [o for o in on if o != primary]
        args = ["--output", primary, "--auto", "--primary", "--pos", "0x0"]
        for left, o in zip(order, order[1:], strict=False):
            args += ["--output", o, "--auto", "--right-of", left]
        for o in off:
            args += ["--output", o, "--off"]
        self._xrandr(*args)
        self._mons = None

    def x_down(self):
        _ssh(as_root(self.dest), "systemctl stop %s" % UNIT)

    # ---- weston ------------------------------------------------------------

    def _weston_ini(self):
        """weston.ini's text: no panel, no lock, no animation, no blanking
        -- what a round would otherwise read as the window's -- the layout
        the machine's console is configured with (console_keyboard()),
        which its Xorg is given too, and the pointer unaccelerated, which
        is how a drag of so many points moves so many points.

        A repaint window of 10 ms, not weston's 7: its GL repaint of a
        full-window wl_shm buffer takes 7 to 8 ms on the thinkpad's
        Ironlake, so at 7 every frame missed its vblank and the window drew
        at 30 fps; 8 still misses some, 9 holds 60 (DESIGN *KMS: no window
        server*). The round measures the client, not weston's default.

        Every connected output on, laid out by weston and read back by
        weston_layout()."""
        kb = self.console_keyboard()
        lines = ["[core]", "idle-time=0", "require-input=false",
                 "repaint-window=10", "",
                 "[shell]", "panel-position=none", "locking=false",
                 "animation=none", "close-animation=none",
                 "startup-animation=none", "",
                 "[libinput]", "accel-profile=flat", ""]
        keys = [(k, kb.get(v)) for k, v in
                (("keymap_model", "XKBMODEL"), ("keymap_layout", "XKBLAYOUT"),
                 ("keymap_variant", "XKBVARIANT"),
                 ("keymap_options", "XKBOPTIONS"))]
        keys = ["%s=%s" % (k, v) for k, v in keys if v]
        if keys:
            lines += ["[keyboard]"] + keys + [""]
        return "\n".join(lines)

    def console_keyboard(self):
        """The console's keyboard, its XKB* names and their values: what
        weston and Xorg's XTEST keyboard (xtest_layout()) are given, and
        what the KMS backend reads for itself. Read where keymap_rules.c
        reads them, Debian's /etc/default/keyboard and else systemd's
        /etc/vconsole.conf, which is Arch's."""
        conf = self.sh("cat /etc/default/keyboard 2>/dev/null || "
                       "cat /etc/vconsole.conf 2>/dev/null", check=False)
        return dict(re.findall(r'^(XKB\w+)="?([^"\n]*)"?', conf, re.M))

    def _wsock(self):
        return "/run/user/%s/%s" % (self.uid, WAYLAND_SOCKET)

    def weston_running(self):
        # The unit as well as the socket: a weston that crashed leaves its
        # socket behind, and the next round would be refused by nobody.
        return self.sh("systemctl is-active -q %s && test -S %s && echo up"
                       % (WESTON_UNIT, shlex.quote(self._wsock())),
                       check=False).strip() == "up"

    def weston_up(self, timeout=30.0):
        """weston running on VT, started if it is not, as a login session of
        the account's. Raises Unreachable when it will not start.

        PAMName makes the unit a session, which is what has logind hand
        weston the DRM node and the input devices; the session is on a seat
        because the unit owns the VT (TTYPath, StandardInput), and active
        once that VT is the one shown, which the chvt makes it. --debug is
        what lets weston-screenshooter take a picture, and weston_layout()
        read the outputs. A weston started with another weston.ini than
        this round's is stopped and started again."""
        text = self._weston_ini()
        ini = self.there + "/weston.ini"
        was = self.sh("cat %s 2>/dev/null" % shlex.quote(ini), check=False)
        if was != text:
            self.sh("mkdir -p %s && cat > %s" % (shlex.quote(self.there),
                                                 shlex.quote(ini)),
                    data=text.encode())
            if self.weston_running():
                self.weston_down()
        if not self.weston_running():
            self.x_down()
            self.cage_down()
            argv = ["systemd-run", "--unit=" + WESTON_UNIT, "--collect",
                    "--quiet", "-p", "PAMName=login", "-p", "User=" + self.user,
                    "-p", "TTYPath=/dev/tty%d" % VT, "-p", "TTYReset=yes",
                    "-p", "TTYVHangup=yes", "-p", "StandardInput=tty-fail",
                    "-p", "StandardOutput=journal",
                    "-p", "StandardError=journal",
                    "-p", "WorkingDirectory=" + self.there,
                    "-E", "XDG_SESSION_TYPE=wayland",
                    "/usr/bin/weston", "--backend=drm",
                    "--socket=" + WAYLAND_SOCKET, "--idle-time=0", "--debug",
                    "--config=" + ini]
            # A socket a crashed weston left would read as this one's up
            # before it has bound its own.
            res = _ssh(as_root(self.dest), "rm -f %s; " % shlex.quote(
                self._wsock()) + " ".join(map(shlex.quote, argv))
                       + " && chvt %d" % VT)
            if res.returncode != 0:
                raise Unreachable("weston would not start on %s: %s"
                                  % (self.name, res.stderr.decode(
                                      "utf-8", "replace").strip()))
            end = time.time() + timeout
            while not self.weston_running():
                if time.time() > end:
                    log = _text(_ssh(as_root(self.dest),
                                     "journalctl -u %s -n 8 -o cat"
                                     % WESTON_UNIT))
                    raise Unreachable("weston on %s opened no socket: %s"
                                      % (self.name, log.strip()))
                time.sleep(0.2)     # poll: nothing tells us it is up
        self.display_env = self.wenv()

    def weston_layout(self):
        """weston's outputs where weston put them, as outputs() gives them
        -- (name, primary, x, y, w, h), none primary -- off the scene graph
        it prints under --debug. Asked and not assumed: weston places each
        right of the last in the order it enabled them, which is the
        kernel's connector order and not necessarily outputs()'s."""
        env = " ".join("%s=%s" % (k, shlex.quote(v))
                       for k, v in self.wenv().items())
        text = self.sh("env %s timeout 3 weston-debug scene-graph 2>&1"
                       % env, check=False)
        got = re.findall(r"^Output \d+ \(([^)]+)\):\n\s*position: "
                         r"\((-?\d+), (-?\d+)\) -> \((-?\d+), (-?\d+)\)",
                         text, re.M)
        if not got:
            raise Unreachable("weston on %s printed no outputs: %s"
                              % (self.name, text.strip()[:200]))
        return [(n, False, int(x), int(y), int(x2) - int(x), int(y2) - int(y))
                for n, x, y, x2, y2 in got]

    def weston_down(self):
        _ssh(as_root(self.dest), "systemctl stop %s" % WESTON_UNIT)

    def wenv(self):
        """What a Wayland client there needs to reach weston."""
        return {"WAYLAND_DISPLAY": WAYLAND_SOCKET,
                "XDG_RUNTIME_DIR": "/run/user/%s" % self.uid}

    def bare_display(self):
        """No server: the panel is the next program's that opens the DRM
        node, and nothing stands between it and the kernel.

        One unit a command: systemd 262 (Arch's) refuses a stop naming a
        unit that is not loaded -- a server already down, its unit
        collected -- and then stops neither."""
        _ssh(as_root(self.dest), "systemctl stop %s; systemctl stop %s"
             % (UNIT, WESTON_UNIT))
        self.cage_down()
        self.display_env = {}

    # ---- cage --------------------------------------------------------------

    def _cage_env(self, output):
        """The environment cage is started with, as text: the console's
        layout, which cage reads from nowhere else, and the output the
        round is on, so that a cage started for another is started
        again."""
        kb = self.console_keyboard()
        lines = ["XKB_DEFAULT_%s=%s" % (k[3:], kb[k]) for k in
                 ("XKBMODEL", "XKBLAYOUT", "XKBVARIANT", "XKBOPTIONS")
                 if kb.get(k)]
        return "\n".join(lines + ["FNL_CAGE_OUTPUT=%s" % (output or "")])

    def cage_socket(self):
        """The socket cage made, when it is up; "" when it is not."""
        f = shlex.quote("%s/%s" % (self.there, CAGE_SOCKET_FILE))
        return self.sh("systemctl is-active -q %s && s=$(cat %s) && "
                       "test -S /run/user/%s/\"$s\" && echo \"$s\""
                       % (CAGE_UNIT, f, self.uid), check=False).strip()

    def cage_up(self, output=None, timeout=30.0):
        """cage running on VT with no program of its own, started if it is
        not, as a login session of the account's: weston_up()'s reasons,
        and its unit's properties. `output` is the one it lights; the
        others are had unplugged first, since cage fits a window to every
        output it has. Raises Unreachable when it will not start."""
        text = self._cage_env(output)
        conf = "%s/cage.env" % self.there
        was = self.sh("cat %s 2>/dev/null" % shlex.quote(conf), check=False)
        if was != text and self.cage_socket():
            self.cage_down()
        if not self.cage_socket():
            self.x_down()
            self.weston_down()
            self.cage_down()
            self.sh("mkdir -p %s && cat > %s && rm -f %s/%s"
                    % (shlex.quote(self.there), shlex.quote(conf),
                       shlex.quote(self.there), CAGE_SOCKET_FILE),
                    data=text.encode())
            others = [m[0] for m in self.outputs() if output
                      and m[0] != output]
            if others:
                self.sh("printf '%%s\\n' %s > %s/%s"
                        % (" ".join(map(shlex.quote, others)),
                           shlex.quote(self.there), CAGE_FORCED_FILE))
                for name in others:
                    self.force_output(name, "off")
            hold = ('echo "$WAYLAND_DISPLAY" > %s/%s; exec sleep infinity'
                    % (shlex.quote(self.there), CAGE_SOCKET_FILE))
            argv = ["systemd-run", "--unit=" + CAGE_UNIT, "--collect",
                    "--quiet", "-p", "PAMName=login", "-p", "User=" + self.user,
                    "-p", "TTYPath=/dev/tty%d" % VT, "-p", "TTYReset=yes",
                    "-p", "TTYVHangup=yes", "-p", "StandardInput=tty-fail",
                    "-p", "StandardOutput=journal",
                    "-p", "StandardError=journal",
                    "-p", "WorkingDirectory=" + self.there,
                    "-E", "XDG_SESSION_TYPE=wayland"]
            for ln in text.splitlines():
                if ln.startswith("XKB_"):
                    argv += ["-E", ln]
            argv += ["/usr/bin/cage", "--", "/bin/sh", "-c", hold]
            res = _ssh(as_root(self.dest), " ".join(map(shlex.quote, argv))
                       + " && chvt %d" % VT)
            if res.returncode != 0:
                raise Unreachable("cage would not start on %s: %s"
                                  % (self.name, res.stderr.decode(
                                      "utf-8", "replace").strip()))
            end = time.time() + timeout
            while not self.cage_socket():
                if time.time() > end:
                    log = _text(_ssh(as_root(self.dest),
                                     "journalctl -u %s -n 8 -o cat"
                                     % CAGE_UNIT))
                    raise Unreachable("cage on %s opened no socket: %s"
                                      % (self.name, log.strip()))
                time.sleep(0.2)     # poll: nothing tells us it is up
        self.display_env = self.cenv()

    def cage_down(self):
        """cage stopped, and the outputs it had unplugged plugged back."""
        f = "%s/%s" % (shlex.quote(self.there), CAGE_FORCED_FILE)
        _ssh(as_root(self.dest), "systemctl stop %s" % CAGE_UNIT)
        # Each tried whatever the one before did, and the record kept of
        # those still off, for the next cage_down() to plug back.
        failed, why = [], []
        for name in self.sh("cat %s 2>/dev/null" % f, check=False).split():
            try:
                self.force_output(name, "detect")
            except Exception as e:
                failed.append(name)
                why.append(str(e))
        if failed:
            self.sh("printf '%%s\\n' %s > %s"
                    % (" ".join(map(shlex.quote, failed)), f), check=False)
            raise Unreachable("; ".join(why))
        self.sh("rm -f %s" % f, check=False)

    def cenv(self):
        """What a Wayland client there needs to reach cage."""
        return {"WAYLAND_DISPLAY": self.cage_socket(),
                "XDG_RUNTIME_DIR": "/run/user/%s" % self.uid}

    def cage_capture(self, path):
        """cage's own picture of the screen (grim, through the screencopy
        protocol cage offers), made there and brought here."""
        return self._fetch_picture(self.cenv(), "grim shot.png", "grim",
                                   path)

    def outputs(self):
        """The connected outputs, as environment.parse_monitors() gives a
        desktop's -- (name, primary, x, y, w, h) -- read off sysfs, which
        needs no server: each at its preferred mode, the panel first and
        the rest to its right, which is where arrange() puts them for Xorg.
        weston places its own; weston_layout() reads where."""
        out = self.sh(r"""
for c in /sys/class/drm/card*-*; do
    [ "$(cat "$c/status" 2>/dev/null)" = connected ] || continue
    echo "${c##*/} $(head -n1 "$c/modes")"
done""", check=False)
        found = []
        for ln in out.splitlines():
            m = re.match(r"^card\d+-(\S+) (\d+)x(\d+)", ln)
            if m:
                found.append((m.group(1), int(m.group(2)), int(m.group(3))))
        found.sort(key=lambda o: not o[0].startswith(("LVDS", "eDP", "DSI")))
        mons, x = [], 0
        for i, (name, w, h) in enumerate(found):
            mons.append((name, i == 0, x, 0, w, h))
            x += w
        return mons

    def weston_capture(self, path):
        """weston's own picture of the screen, made there and brought here:
        what the compositor did with the frame."""
        return self._fetch_picture(self.wenv(), "weston-screenshooter",
                                   "weston-screenshooter", path)

    def _fetch_picture(self, wenv, command, tool, path):
        """`command` run there in a directory of its own, with `wenv`, and
        the PNG it left brought here to `path`."""
        env = " ".join("%s=%s" % (k, shlex.quote(v))
                       for k, v in wenv.items())
        # The directory goes with the picture once it is brought, and at
        # once where there is none.
        got = self.sh('d=$(mktemp -d) || exit 1; cd "$d" && env %s '
                      '%s >/dev/null 2>&1; '
                      'p=$(ls "$d"/*.png 2>/dev/null | head -n1); '
                      'if [ -n "$p" ]; then echo "$p"; '
                      'elif [ -n "$d" ]; then rm -rf "$d"; fi'
                      % (env, command), check=False).strip()
        if not got:
            raise Unreachable("%s made no picture on %s" % (tool, self.name))
        res = subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + ["%s:%s" % (self.dest, got), path],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60)
        self.sh("rm -rf %s" % shlex.quote(os.path.dirname(got)), check=False)
        if res.returncode != 0:
            raise Unreachable("bringing the picture back from %s: %s"
                              % (self.name, res.stderr.strip()))
        return path

    def force_output(self, name, state):
        """Write `state` -- "off", "on" or "detect" -- to the status of the
        connector of that name, on whichever card has it: the kernel's way
        to have a monitor unplugged and plugged back in without a hand, and
        the hotplug event it sends is the one a cable sends. Root's, there;
        "detect" puts it back to what the cable says."""
        # A connector gone from every card -- unbound, or moved to another
        # driver since it was forced -- has nothing to put back: skipped,
        # or one stale record would fail every bring-up after it.
        skip = '[ -e "$f" ] || continue; ' if state == "detect" else ""
        cmd = ("for f in /sys/class/drm/card*-%s/status; do %secho %s > $f; "
               "done" % (self._connector(name), skip, shlex.quote(state)))
        res = _ssh(as_root(self.dest), cmd, timeout=30)
        if res.returncode != 0:
            raise Unreachable("forcing %s %s on %s: %s"
                              % (name, state, self.name, res.stderr.decode(
                                  "utf-8", "replace").strip()))

    def _connector(self, name):
        """`name`, checked to be a connector's as the kernel spells them
        (HDMI-A-1): it is a path in root's shell there, which a quote would
        not keep inside /sys/class/drm."""
        if not re.fullmatch(r"[A-Za-z0-9-]+", name or ""):
            raise Unreachable("%r is no connector's name" % (name,))
        return name

    def force_output_heard(self, name, state, wait=3.0):
        """force_output(), with the kernel's DRM uevents listened for while
        it is written (`udevadm monitor -k`, as root): whether one came
        within `wait` seconds. A machine whose every connector is
        hotplug-capable leaves DRM's output polling off, and there a forced
        status is queued as a delayed event nothing sends -- the RX 580
        here -- so the program, which hears of an unplug by uevent alone,
        is told nothing, and a round's unplug checks would read a window
        that never moved."""
        tmp = "/tmp/fenestal-uevent.$$"
        cmd = ("timeout %g udevadm monitor -k -s drm > %s 2>&1 & m=$!; "
               "sleep 0.5; for f in /sys/class/drm/card*-%s/status; do "
               "echo %s > $f; done; wait $m; grep -c '^KERNEL\\[' %s; "
               "rm -f %s" % (wait, tmp, self._connector(name),
                             shlex.quote(state), tmp, tmp))
        res = _ssh(as_root(self.dest), cmd, timeout=wait + 30)
        said = _text(res).strip().splitlines()
        if not said or not said[-1].isdigit():
            raise Unreachable("forcing %s %s on %s: %s"
                              % (name, state, self.name, res.stderr.decode(
                                  "utf-8", "replace").strip()))
        return int(said[-1]) > 0

    def kms_capture(self, path, output=None):
        """What the display is scanning out (kms_grab.py, run there by
        root), brought here as a PNG: with no compositor there is nobody
        else to ask for a picture. `output` names the connector to picture;
        without it, the first CRTC lit."""
        from PIL import Image
        res = subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + [GRAB, "%s:%s/" % (self.dest, self.there)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60)
        if res.returncode != 0:
            raise Unreachable("copying the grab to %s: %s"
                              % (self.name, res.stderr.strip()))
        res = _ssh(as_root(self.dest), "python3 %s/%s%s" % (
            shlex.quote(self.there), os.path.basename(GRAB),
            " --output " + shlex.quote(output) if output else ""),
            timeout=60)
        if res.returncode != 0 or not res.stdout.startswith(b"P6"):
            raise Unreachable("no picture of %s's display: %s"
                              % (self.name, res.stderr.decode(
                                  "utf-8", "replace").strip()))
        ppm = path + ".ppm"
        with open(ppm, "wb") as f:
            f.write(res.stdout)
        Image.open(ppm).save(path)
        os.remove(ppm)
        return path

    def uinput_hands(self, absolute=None):
        """A keyboard and a mouse there (uinput_hands.py), driven from here:
        the script copied into DRIVE_DIR and run by root, which is who may
        open /dev/uinput. `absolute`, a screen's (width, height), makes the
        mouse an absolute pointer over it (AbsUinputHands)."""
        res = subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + [HANDS, "%s:%s/" % (self.dest, self.there)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=60)
        if res.returncode != 0:
            raise Unreachable("copying the hands to %s: %s"
                              % (self.name, res.stderr.strip()))
        script = "%s/%s" % (self.there, os.path.basename(HANDS))
        self.hands_ = (AbsUinputHands(self, script, absolute) if absolute
                       else UinputHands(self, script))
        return self.hands_

    @contextlib.contextmanager
    def _as_display(self):
        """$DISPLAY and $XAUTHORITY naming the forwarded server, for as long
        as a connection is being opened to it. Put back after: the X11
        environment on this desktop reads the same two."""
        was = {k: os.environ.get(k) for k in ("DISPLAY", "XAUTHORITY")}
        os.environ["DISPLAY"] = "127.0.0.1:%d" % self.dno
        os.environ["XAUTHORITY"] = self.xauth
        try:
            yield
        finally:
            for k, v in was.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def server(self):
        """x11_driver, a second time, connected to this machine's server.

        A copy of the module and not a change to it: its connection is made
        at import and every function reads it, which is right for the one
        display this desktop has. Loaded under another name, the copy's
        globals are its own, and the environments on this desktop keep
        theirs."""
        from Xlib import error
        from Xlib.protocol import display as xproto
        from Xlib.protocol import event
        self.x_up()
        spec = importlib.util.find_spec("x11_driver")
        mod = importlib.util.module_from_spec(spec)
        # python-xlib keeps a connection's extension opcodes and event and
        # error classes in tables on the class, so a second server writes
        # its own codes over the first's: RandR's events are at another base
        # on that server, and opening it beside this desktop's raised a
        # TypeError. So the connection is made with tables of its own, which
        # are then its instance's, and the class keeps this desktop's.
        fresh = {"extension_major_opcodes": {},
                 "error_classes": error.xerror_class.copy(),
                 "event_classes": event.event_class.copy()}
        was = {k: getattr(xproto.Display, k) for k in fresh}
        for k, v in fresh.items():
            setattr(xproto.Display, k, v)
        try:
            with self._as_display():
                spec.loader.exec_module(mod)
            for k, v in fresh.items():
                setattr(mod.d.display, k, v)
        except Exception as e:
            raise Unreachable("no X connection to %s through the "
                              "forward: %s" % (self.name, e))
        finally:
            for k, v in was.items():
                setattr(xproto.Display, k, v)
        return mod

    def hands(self, xmod):
        self.hands_ = Hands(xmod)
        self.xtest_layout(xmod.d)
        return self.hands_

    def xtest_layout(self, d):
        """The console's layout on XTEST's keyboard, which is the device the
        harness types through, and on the core keyboard: a real keyboard
        there gets it from udev, and XTEST's has the server's default, US.
        The core keyboard takes a device's keymap when that device types,
        so a program running then is shown the change by XKB's own events
        (keymap_stale in x11_keys.c)."""
        from Xlib.ext import xinput
        devs = d.xinput_query_device(xinput.AllDevices).devices
        ids = [v.deviceid for v in devs
               if v.name == "Virtual core XTEST keyboard"]
        if not ids:
            raise Unreachable("no XTEST keyboard on %s's X server"
                              % self.name)
        kb = self.console_keyboard()
        args = ["setxkbmap", "-device", str(ids[0])]
        for flag, key in (("-model", "XKBMODEL"), ("-layout", "XKBLAYOUT"),
                          ("-variant", "XKBVARIANT")):
            args += [flag, kb.get(key, "")]
        # An empty -option first: -option on its own adds to what is set.
        args += ["-option", ""]
        if kb.get("XKBOPTIONS"):
            args += ["-option", kb["XKBOPTIONS"]]
        env = " ".join("%s=%s" % (k, shlex.quote(v))
                       for k, v in self.xenv().items())
        # And the core keyboard's, which is what a program reads at start:
        # it takes XTEST's keymap only once XTEST has typed, and a program
        # started on a fresh server before that read the server's default.
        # The X11 backend reloads at the switch (keymap_stale); wine reads
        # the layout once, and had no AltGr all round.
        core = args[:1] + args[3:]          # without "-device", the id
        out = self.sh("env %s %s 2>&1 && env %s %s 2>&1; echo rc=$?" % (
            env, " ".join(map(shlex.quote, args)),
            env, " ".join(map(shlex.quote, core))), check=False)
        if "rc=0" not in out:
            raise Unreachable("setxkbmap on %s: %s" % (self.name,
                                                       out.strip()))

    def capture(self, win, path):
        with self._as_display():
            env = dict(os.environ)
        subprocess.run(["import", "-window", hex(win.id), path], check=True,
                       stderr=subprocess.DEVNULL, env=env, timeout=30)
        return path

    def xenv(self):
        """What a program there needs to reach the server."""
        return {"DISPLAY": ":%d" % DISPLAY_NO,
                "XAUTHORITY": self.there + "/xauth"}

    def monitors(self):
        """xrandr --listmonitors there, as environment.parse_monitors reads
        it."""
        if self._mons is None:
            self._mons = self._xrandr("--listmonitors")
        return self._mons

    # ---- the program -------------------------------------------------------

    def install(self, exe, variant, tree=None):
        """Copy a tree's program there; its path there. Raises Unreachable
        when the machine has not got a library it links.

        `tree` names the directory it goes to there, beside the variant: a
        tree's programs never land on another's, and the environments that
        share one -- Xorg's, weston's and the bare display's, all run from
        linux -- share its directory, whose copy is the same bytes.
        A Windows program goes to wine (_install_wine()), and its
        environment names a directory for each renderer it is told to draw
        with (environment._WineOnBare.install_dir()), since what goes with
        it is not the same bytes for the three.

        A sanitizer tree's runtime goes with it, this desktop's own copy:
        the instrumentation and the runtime are one compiler's, and the
        machine's distribution ships another's. They ask nothing of glibc
        past what the program does, and run_argv() puts them first.

        Not for a program of another processor: the machine's own runtime
        runs it (_runtimes() says why), and none is sent."""
        self.wine = exe.endswith(".exe")
        if self.wine:
            return self._install_wine(exe, "%s-%s" % (tree, variant))
        want, got = ELF_MACHINES.get(self.arch), _elf_machine(exe)
        if want and got is not None and got != want:
            built = [a for a, m in ELF_MACHINES.items() if m == got]
            raise Unreachable("%s is built for %s and %s is %s: it takes "
                              "the %s tree's" % (os.path.relpath(exe, ROOT),
                                          built[0] if built else
                                          "machine %#x" % got, self.name,
                                          self.arch,
                                          TREES.get(self.arch, {}).get(
                                              "linux", "?")))
        dst = "%s/%s" % (self.there,
                         "%s-%s" % (tree, variant) if tree else variant)
        self.sh("mkdir -p %s" % shlex.quote(dst))
        runtimes = self._runtimes(exe)
        # Sent, and needed: the second is the first where the program is
        # this desktop's processor's, and the machine's own otherwise.
        self.runtimes = [os.path.basename(f) for f in runtimes]
        nv = shlex.quote(self.distro["nvidia"])
        amd = shlex.quote(self.distro["radeon"])
        if self.moves_driver:
            nvidia = "test -e %s && { %s; }" % (nv, "; ".join(
                "test -e %s && echo %s" % (q, q) for q in
                (shlex.quote(self.distro[k]) for k in ("dbus", "egl_mesa"))))
        else:
            nvidia = ("test -e /proc/driver/nvidia/version && test -e %s && "
                      "echo %s" % (nv, nv))
        self.pinned = [self.distro["lavapipe"]] + self.sh(
            nvidia + "".join("; test -e %s && echo %s" % (q, q) for q in
                             map(shlex.quote, ALSO_PINNED))
            + "; grep -qs 0x1002 /sys/bus/pci/devices/*/vendor && "
            "test -e %s && echo %s" % (amd, amd),
            check=False).split()
        libdir = os.path.dirname(self.distro["lavapipe"])
        self.tsan_preloaded = self.sh(
            "ls -1 %s 2>/dev/null" % " ".join(
                shlex.quote(libdir) + "/" + g for g in TSAN_PRELOADED),
            check=False).split()
        files = [exe, self._suppressions(),
                 os.path.join(ROOT, "lsan.supp")] + runtimes
        res = subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + [f for f in files if os.path.exists(f)]
            + ["%s:%s/" % (self.dest, dst)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=120)
        if res.returncode != 0:
            raise Unreachable("copying to %s: %s" % (self.name,
                                                     res.stderr.strip()))
        there = "%s/%s" % (dst, os.path.basename(exe))
        # Asked as the program will be run, the shipped runtimes first.
        lib = ("LD_LIBRARY_PATH=%s " % shlex.quote(dst)
               if self.runtimes else "")
        missing = [ln.split()[0] for ln in
                   self.sh("%sldd %s" % (lib, shlex.quote(there)),
                           check=False).splitlines() if "not found" in ln]
        if missing:
            raise Unreachable("%s has not got %s, which the %s tree's "
                              "program links" % (self.name,
                                                 ", ".join(missing), variant))
        return there

    def wine_prefix(self):
        return WINE_PREFIX % self.uid

    def _install_wine(self, exe, name):
        """The program and every .dll beside it, into the directory `name`
        -- a Direct3D 12 round's copy has DXVK's DXGI beside the program,
        which a Vulkan round must not load, so each has its own, and its
        .dll files are cleared before the copy all the same -- and
        the prefix made if this boot has not got one (WINE_PREFIX_UP)."""
        if not self.has("wine"):
            raise Unreachable("no wine on %s -- bare_driver.py --setup "
                              "installs it" % self.name)
        dst = "%s/%s" % (self.there, name)
        here = os.path.dirname(exe)
        files = [exe] + [os.path.join(here, f) for f in sorted(os.listdir(here))
                         if f.endswith(".dll")]
        self.sh("mkdir -p %s && rm -f %s/*.dll" % (shlex.quote(dst),
                                                   shlex.quote(dst)))
        res = subprocess.run(
            ["scp", "-q", "-o", "ControlPath=" + self.ctl] + SSH_OPTS
            + files + ["%s:%s/" % (self.dest, dst)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=120)
        if res.returncode != 0:
            raise Unreachable("copying to %s: %s" % (self.name,
                                                     res.stderr.strip()))
        pfx = shlex.quote(self.wine_prefix())
        out = self.sh(WINE_PREFIX_UP % {"pfx": pfx}, timeout=300,
                      check=False).strip()
        if out == "made":
            print("  made the wine prefix %s" % self.wine_prefix(), flush=True)
        elif out != "there":
            raise Unreachable("wineboot made no prefix on %s: %s"
                              % (self.name, out))
        self.runtimes = self.sanitizers = []
        return "%s/%s" % (dst, os.path.basename(exe))

    def _suppressions(self):
        """tsan.supp and TSAN_EXTRA as one file, under tsan.supp's name:
        TSAN_OPTIONS takes one, and run_argv() points it there."""
        dst = os.path.join(self.dir, "tsan.supp")
        with open(dst, "w") as out:
            for src in (os.path.join(ROOT, "tsan.supp"), TSAN_EXTRA):
                with open(src) as f:
                    out.write(f.read() + "\n")
        return dst

    def _runtimes(self, exe):
        """This desktop's copies of the sanitizer runtimes `exe` links,
        under their sonames, staged where scp can send them; and every
        runtime it links into self.sanitizers, by soname.

        A program of another processor gets none sent: ldd here cannot
        load it, and the copies beside its cross compiler are not that
        compiler's either -- Ubuntu pairs gcc-15's aarch64 cross with
        gcc-16's runtimes, a snapshot (16-20260322), whose ASan crashed in
        its interceptors' setup on the Pi before main. The machine's own
        runs it (Arch Linux ARM's gcc-libs, 16.1.1), and install()'s ldd
        there says when the machine has not got one."""
        machine = _elf_machine(exe)
        if machine is not None and machine != ELF_MACHINES.get(
                os.uname().machine):
            out = subprocess.run(["readelf", "-d", exe], capture_output=True,
                                 text=True).stdout
            self.sanitizers = re.findall(
                r"\(NEEDED\)\s+Shared library: \[(%s)\]"
                % SANITIZER_RUNTIME, out)
            return []
        out = subprocess.run(["ldd", exe], capture_output=True,
                             text=True).stdout
        found = re.findall(r"^\s*(%s) => (\S+)" % SANITIZER_RUNTIME, out,
                           re.M)
        self.sanitizers = [soname for soname, _ in found]
        staged = []
        for soname, path in found:
            dst = os.path.join(self.dir, soname)
            shutil.copyfile(path, dst)
            staged.append(dst)
        return staged

    def run_argv(self, exe, args=(), env=None):
        """The command line here that runs the program there.

        `-n`: the harness's own stdin stays its own. The shell says its pid
        and then becomes the program, so the pid is the program's, and the
        exit code ssh returns is too. A path into this tree in a variable --
        TSAN_OPTIONS' suppressions -- is turned into the copy there."""
        base = self.xenv() if self.display_env is None else self.display_env
        here = dict(base, **(env or {}))
        shipped = getattr(self, "runtimes", [])
        if shipped:
            here["LD_LIBRARY_PATH"] = os.path.dirname(exe)
        tsan = [r for r in getattr(self, "sanitizers", [])
                if r.startswith("libtsan")]
        if tsan:
            # No gallium threaded context: its driver thread hands each
            # batch over through a futex TSan does not see, and every
            # report of it is on a command buffer of the driver's
            # (tsan.supp, "Not keyed").
            here.setdefault("GALLIUM_THREAD", "0")
            # TSAN_PRELOADED says why; TSan's runtime first, as ASan's is.
            here["LD_PRELOAD"] = ":".join(
                ["%s/%s" % (os.path.dirname(exe), tsan[0])
                 if tsan[0] in shipped else tsan[0]]
                + getattr(self, "tsan_preloaded", []))
        asan = [r for r in getattr(self, "sanitizers", [])
                if r.startswith("libasan")]
        if asan:
            # ASan's runtime first, or it refuses to start: the copy sent,
            # or by soname the machine's own.
            here["LD_PRELOAD"] = ":".join(
                ["%s/%s" % (os.path.dirname(exe), asan[0])
                 if asan[0] in shipped else asan[0]] + self.pinned)
            # NVIDIA's leaks are made inside libxcb, where the fast
            # unwinder stops short of the driver that lsan.supp keys on;
            # where the GPU moves, NVIDIA's driver is not kept and the
            # leaks it leaves are named by their deeper frames or not at
            # all.
            if self.distro["nvidia"] in self.pinned or \
                    self.distro["dbus"] in self.pinned:
                here["ASAN_OPTIONS"] = ":".join(
                    x for x in (here.get("ASAN_OPTIONS"),
                                "fast_unwind_on_malloc=0") if x)
            if self.moves_driver == "nvidia":
                here["ASAN_OPTIONS"] = ":".join(
                    x for x in (here.get("ASAN_OPTIONS"),
                                "detect_leaks=0") if x)
        here = {k: v.replace(ROOT, os.path.dirname(exe)) for k, v in
                here.items()}
        wine = getattr(self, "wine", False)
        if wine:
            here["WINEPREFIX"] = self.wine_prefix()
        command = "env %s %s%s %s" % (
            " ".join("%s=%s" % (k, shlex.quote(v)) for k, v in here.items()),
            "wine " if wine else "", shlex.quote(exe),
            " ".join(map(shlex.quote, args)))
        if wine:
            # Waited for and not exec'd. The wine server and the helpers the
            # program starts (services.exe, explorer.exe and the rest) are
            # handed its stderr, which is this ssh's, and outlive it by the
            # server's few seconds: the ssh ends when they do, and a round
            # reading its end as the program's would see a program that
            # took seconds to exit. So the shell says when the program has
            # gone, and with what code (exit_said()). wine becomes the
            # program, pid and all -- its loader execs the preloader, which
            # loads the .exe -- so $! is the program's.
            line = "%s & p=$!; echo %s$p >&2; wait $p; echo %s$? >&2" % (
                command, PID_TAG, EXIT_TAG)
        else:
            line = "echo %s$$ >&2; exec %s" % (PID_TAG, command)
        return ["ssh", "-n", "-S", self.ctl] + SSH_OPTS + [
            self.dest, "sh -c %s" % shlex.quote(line)]

    def pid_of(self, testbed, timeout=10.0):
        """The program's pid there, as its shell said it."""
        end = time.time() + timeout
        while time.time() < end:
            with testbed.lock:
                for line in testbed.lines:
                    if line.startswith(PID_TAG):
                        return int(line[len(PID_TAG):])
            if not testbed.alive():
                break
            time.sleep(0.01)        # poll: the first line is a round trip
        raise Unreachable("the program on %s never started: %r"
                          % (self.name, testbed.dump(3)))

    @staticmethod
    def exit_said(testbed):
        """The code the remote shell said its program exited with, or None
        while it has said nothing: the last lines only, since nothing but
        the helpers' rare words can follow it."""
        with testbed.lock:
            tail = testbed.lines[-20:]
        for line in reversed(tail):
            if line.startswith(EXIT_TAG):
                return int(line[len(EXIT_TAG):])
        return None

    def signal(self, pid, sig=15):
        self.sh("kill -%d %d" % (sig, pid), check=False, timeout=10)

    def kill_stale(self):
        """Anything an earlier run left up there, gone, and seen to go: the
        programs -- wine names a process after its .exe, so theirs are
        PROGRAMS too -- and the prefix's wine server, killed and waited
        for, which would otherwise hand the next program its display."""
        names = "|".join(PROGRAMS)
        pfx = shlex.quote(self.wine_prefix())
        self.sh("pkill -x '%s'; for i in 1 2 3 4 5 6 7 8 9 10; do "
                "pgrep -x '%s' >/dev/null && { sleep 0.3; continue; }; break; "
                "done; pkill -9 -x '%s'; "
                "if test -d %s && command -v wineserver >/dev/null; then "
                "WINEPREFIX=%s wineserver -k 2>/dev/null; "
                "WINEPREFIX=%s timeout 10 wineserver -w; fi; true"
                % (names, names, names, pfx, pfx, pfx), check=False)

    def cpu_seconds(self, pid):
        raw = self.sh("cat /proc/%d/stat" % pid, check=False)
        if ")" not in raw:
            raise RuntimeError("the process under test has gone")
        fields = raw[raw.rindex(")") + 2:].split()
        hz = int(self.sh("getconf CLK_TCK").strip() or 100)
        return (int(fields[11]) + int(fields[12])) / hz

    def has_mapped(self, pid, name):
        """Whether the process there has `name` in its mappings: the way to
        tell a validation layer asked for from one loaded."""
        return self.sh("grep -c %s /proc/%d/maps" % (shlex.quote(name), pid),
                       check=False).strip() not in ("", "0")

    # ---- the clipboard's other program -------------------------------------

    def has(self, tool):
        return bool(self.sh("command -v %s" % tool, check=False).strip())

    def _xclip(self, *args):
        env = " ".join("%s=%s" % (k, shlex.quote(v))
                       for k, v in self.xenv().items())
        return "env %s xclip %s" % (env, " ".join(args))

    def clipboard_get(self):
        try:
            return _ssh(self.dest, self._xclip("-o", "-selection",
                                               "clipboard"),
                        timeout=5, ctl=self.ctl).stdout
        except subprocess.TimeoutExpired:
            return b"(xclip timed out)"

    def clipboard_put(self, data):
        # In the foreground (-quiet) and in the background of the shell, so
        # that its pid is known and clipboard_done() ends that xclip alone,
        # never one of the account's own. The input goes through a file
        # already open when it is unlinked: a background job's stdin is not
        # the ssh channel's, and nor may its descriptors be, or ssh waits
        # for the xclip that stays behind to serve the selection.
        cmd = ('f=$(mktemp) && cat >"$f" && exec 3<"$f" && rm -f "$f" && '
               '{ %s <&3 >/dev/null 2>&1 3<&- & } && echo $!'
               % self._xclip("-quiet", "-i", "-selection", "clipboard"))
        res = _ssh(self.dest, cmd, timeout=10, ctl=self.ctl, data=data)
        pid = _text(res).strip()
        if pid.isdigit():
            self.xclips.append(int(pid))

    def clipboard_done(self):
        # Each by its pid, and only while that pid is still an xclip: one
        # that lost the selection to a later offer has exited, and its pid
        # may since be another program's.
        if self.xclips:
            self.sh("for p in %s; do [ \"$(cat /proc/$p/comm 2>/dev/null)\""
                    " = xclip ] && kill $p; done; true"
                    % " ".join(str(p) for p in self.xclips), check=False)
            self.xclips = []


class Hands:
    """XTEST, on the forwarded connection: what wayland_driver.Portal offers
    a round -- rel(), button(), key(), keycode(), tap(), scroll() -- and
    warp(), which only a server that says where the pointer is can give.

    Relative motion is made absolute here. XTEST's relative motion goes
    through the server's pointer acceleration, and the portal's does not,
    so a round's arithmetic would be off by the acceleration curve. Read,
    added to and warped to instead, with the fractions carried."""

    BUTTONS = {0x110: 1, 0x111: 3, 0x112: 2}

    def __init__(self, xmod):
        from Xlib import X
        from Xlib.ext import xtest
        self.X = X
        self.xtest = xtest
        self.x = xmod
        self.d = xmod.d
        if not self.d.has_extension("XTEST"):
            raise Unreachable("the X server there has no XTEST")
        scr = self.d.screen()
        self.size = (scr.width_in_pixels, scr.height_in_pixels)
        self.rest = [0.0, 0.0]
        self.held_keys = set()
        self.held_buttons = set()

    def warp(self, x, y):
        w, h = self.size
        self.xtest.fake_input(self.d, self.X.MotionNotify,
                              x=max(0, min(w - 1, int(x))),
                              y=max(0, min(h - 1, int(y))))
        self.d.sync()

    def rel(self, dx, dy):
        self.rest[0] += dx
        self.rest[1] += dy
        ix, iy = round(self.rest[0]), round(self.rest[1])
        self.rest[0] -= ix
        self.rest[1] -= iy
        px, py = self.x.pointer_pos()
        self.warp(px + ix, py + iy)

    def button(self, btn=0x110, pressed=True):
        n = self.BUTTONS[btn]
        self.xtest.fake_input(self.d, self.X.ButtonPress if pressed
                              else self.X.ButtonRelease, detail=n)
        self.d.sync()
        (self.held_buttons.add if pressed else
         self.held_buttons.discard)(btn)

    def key(self, name, pressed=True):
        from wayland_driver import KEY
        self.keycode(KEY[name], pressed)

    def keycode(self, code, pressed=True):
        """By evdev code, as the portal takes one: the X keycode is that
        plus 8 on every evdev server, which Xorg with libinput is."""
        self.xtest.fake_input(self.d, self.X.KeyPress if pressed
                              else self.X.KeyRelease, detail=code + 8)
        self.d.sync()
        (self.held_keys.add if pressed else self.held_keys.discard)(code)

    def tap(self, name, hold=0.05):
        self.key(name, True)
        time.sleep(hold)            # the press itself
        self.key(name, False)

    def scroll(self, steps):
        # Positive is down, as the portal's discrete axis has it: button 5.
        n = 5 if steps > 0 else 4
        for _ in range(abs(int(steps))):
            for t in (self.X.ButtonPress, self.X.ButtonRelease):
                self.xtest.fake_input(self.d, t, detail=n)
        self.d.sync()

    def let_go(self):
        """Whatever is still held, released: a key left down on a server
        nobody is at repeats into the next round."""
        try:
            for code in list(self.held_keys):
                self.keycode(code, False)
            for btn in list(self.held_buttons):
                self.button(btn, False)
        except Exception:
            pass

    def close(self):
        self.let_go()


class UinputHands:
    """uinput_hands.py on the machine, as root, over an ssh of its own: what
    Hands offers a round, without warp(), since neither weston nor the bare
    display says where the pointer is.

    Relative motion is whole counts there, so the fractions are carried
    here, as Hands carries them. The pointer is unaccelerated at the other
    end -- weston's accel-profile=flat, and the KMS backend has no curve."""

    BUTTONS = (0x110, 0x111, 0x112)

    def __init__(self, box, script, timeout=15.0, args=""):
        self.box = box
        self.rest = [0.0, 0.0]
        self.held_keys = set()
        self.held_buttons = set()
        self.proc = subprocess.Popen(
            ["ssh"] + SSH_OPTS + [as_root(box.dest),
                                  "python3 -u %s %s"
                                  % (shlex.quote(script), args)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        if not self._answer(timeout).startswith("ready"):
            said = ""
            if self.proc.poll() is not None:
                said = self.proc.stderr.read().strip().splitlines()
                said = said[-1] if said else ""
            self.close()
            raise Unreachable("no uinput devices on %s%s -- bare_driver.py "
                              "--setup loads the module"
                              % (box.name, ": " + said if said else ""))

    def _answer(self, timeout=5.0):
        import select
        ready, _w, _x = select.select([self.proc.stdout], [], [], timeout)
        return self.proc.stdout.readline().strip() if ready else ""

    def _say(self, line):
        if self.proc.poll() is not None:
            raise Unreachable("the hands on %s have gone" % self.box.name)
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        said = self._answer()
        if not said:
            # Its answer, coming late, would be read as the next line's:
            # these hands are ended, and every later call says they have
            # gone. Killed, not closed, which would ask them to let go.
            self.proc.kill()
            self.proc.wait()
        if said != "ok":
            raise Unreachable("the hands on %s did not answer %r"
                              % (self.box.name, line))

    def rel(self, dx, dy):
        self.rest[0] += dx
        self.rest[1] += dy
        ix, iy = round(self.rest[0]), round(self.rest[1])
        self.rest[0] -= ix
        self.rest[1] -= iy
        if ix or iy:
            self._say("rel %d %d" % (ix, iy))

    def button(self, btn=0x110, pressed=True):
        self._say("btn %d %d" % (btn, 1 if pressed else 0))
        (self.held_buttons.add if pressed else
         self.held_buttons.discard)(btn)

    def key(self, name, pressed=True):
        from wayland_driver import KEY
        self.keycode(KEY[name], pressed)

    def keycode(self, code, pressed=True):
        self._say("key %d %d" % (code, 1 if pressed else 0))
        (self.held_keys.add if pressed else self.held_keys.discard)(code)

    def tap(self, name, hold=0.05):
        self.key(name, True)
        time.sleep(hold)            # the press itself
        self.key(name, False)

    def scroll(self, steps):
        # Positive is down, as the portal's discrete axis has it, and
        # REL_WHEEL's positive is away from the user: one detent a step.
        for _ in range(abs(int(steps))):
            self._say("wheel %d" % (-1 if steps > 0 else 1))

    def let_go(self):
        try:
            for code in list(self.held_keys):
                self.keycode(code, False)
            for btn in list(self.held_buttons):
                self.button(btn, False)
        except Exception:
            pass

    def close(self):
        self.let_go()
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=5)
            except Exception:
                self.proc.kill()
                self.proc.wait()


class AbsUinputHands(UinputHands):
    """UinputHands with an absolute pointer: the mouse is a tablet over the
    screen (uinput_hands.py --absolute), so a place asked for is the place
    the pointer goes, whatever the compositor's acceleration. What the
    round asks for is still motion, so the pointer's place is kept here
    and a motion is a place: the screen's edge holds it as it holds a
    mouse, and warp() is offered as Hands offers it."""

    def __init__(self, box, script, size, timeout=15.0):
        self.size = size
        self.at = [size[0] // 2, size[1] // 2]
        super().__init__(box, script, timeout,
                         args="--absolute %d %d" % size)
        try:
            self._put()
        except Exception:
            self.close()
            raise

    def _put(self):
        self._say("abs %d %d" % tuple(self.at))

    def warp(self, x, y):
        self.at = [max(0, min(self.size[0] - 1, int(x))),
                   max(0, min(self.size[1] - 1, int(y)))]
        self.rest = [0.0, 0.0]
        self._put()

    def rel(self, dx, dy):
        self.rest[0] += dx
        self.rest[1] += dy
        ix, iy = round(self.rest[0]), round(self.rest[1])
        self.rest[0] -= ix
        self.rest[1] -= iy
        if ix or iy:
            rest = self.rest
            self.warp(self.at[0] + ix, self.at[1] + iy)
            self.rest = rest


# ---- the command line ------------------------------------------------------

STATUS = r"""
has() { %(has)s; }
for p in %(packages)s; do
    has "$p" || echo "missing package: $p"
done
if test -S /tmp/.X11-unix/X%(dno)d; then
    echo "X server on :%(dno)d: up"
else
    echo "X server on :%(dno)d: down -- --up starts it, and a round does"
fi
if test -S "/run/user/$(id -u)/%(wsock)s"; then
    echo "weston on VT %(vt)d: up"
else
    echo "weston: down -- --weston starts it, and a Wayland round does"
fi
# cage and the prefix are the wine rounds', which a machine with no wine
# (NOT_OFFERED) never runs.
if %(wine)s; then
    if systemctl is-active -q %(cunit)s; then
        echo "cage on VT %(vt)d: up"
    else
        echo "cage: down -- --cage starts it, and a wine round on KMS does"
    fi
    if test -d %(wpfx)s; then
        echo "wine prefix %(wpfx)s: made"
    else
        echo "wine prefix: not made this boot -- a wine round makes it"
    fi
fi
if grep -qs 0x10de /sys/bus/pci/devices/*/vendor; then
    if ! test -x %(gpudrv)s; then
        echo "no %(gpudrv)s, so the library's FNL_GPU_DRIVER moves" \
             "nothing -- --setup"
    elif ! test -S %(gpusock)s; then
        echo "no %(gpusock)s, so the library's FNL_GPU_DRIVER moves" \
             "through sudo, and a move cut off leaves no driver -- --setup"
    elif ! id -nG | grep -qw %(gpugroup)s; then
        echo "not in group %(gpugroup)s, so the library's FNL_GPU_DRIVER" \
             "moves through sudo -- --setup, and a new login"
    fi
fi
id -nG | grep -qw input ||
    echo "not in group input, so the KMS backend reads no device -- --setup"
test -e /dev/uinput ||
    echo "no /dev/uinput, so a Wayland or KMS round has no hands -- --setup"
command -v weston-screenshooter >/dev/null ||
    echo "no weston-screenshooter, so a Wayland round takes no picture"
if pgrep -x 'xfwm4|openbox|mutter|kwin_x11|i3|fluxbox|twm|xfce4-session' \
        >/dev/null; then
    echo "a window manager is running, which this machine is for having none"
fi
"""


# The packages that are not there, and the package manager asked for those
# alone: one that has everything is not asked at all, so a machine with no
# package database -- the live key -- needs none.
INSTALL = r"""
has() { %(has)s; }
missing=
for p in %(packages)s; do
    has "$p" || missing="$missing $p"
done
if [ -z "$missing" ]; then
    echo "every package is there"
else
    %(install)s
fi
"""


# DEBUGINFO_WHY: the distributions strip their Vulkan drivers, so a
# checker's report names no frame inside one, and an entry in tsan.supp can
# only key on the whole library -- which every Vulkan call of ours passes
# through. The same holds for Mesa's OpenGL (libgallium, which Zink,
# iris and llvmpipe all are, and the EGL vendor library in front of it)
# and for the Vulkan layers, whose own locks a report can name. With the
# debug info where libbacktrace looks for it (/usr/lib/debug/.build-id/),
# GCC's sanitizers name the driver's own functions, and an entry can key
# on one of those. Fetched by build id, so a
# driver updated since is a driver whose info is missing, and --status says
# so; a report from it then fails the round as before, rather than an entry
# quietly matching nothing. Run as root by --setup ("fetch"), as the account
# by --status ("check"); python3, since it is a required package and curl
# is not. NVIDIA's own driver is closed and no server has its info: it is
# named once as such, and an entry for it can key on its library alone.
DEBUGINFO = r"""
import glob, json, os, struct, sys, urllib.request
mode, server = sys.argv[1], sys.argv[2]
CLOSED = ("libGLX_nvidia.", "libEGL_nvidia.")
DIRS = ("/usr/lib", "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu")

def build_id(path):
    with open(path, "rb") as f:
        d = f.read()
    i = d.find(b"GNU\0")
    while i >= 12:
        namesz, descsz, typ = struct.unpack("<III", d[i - 12:i])
        if typ == 3 and namesz == 4:
            return d[i + 4:i + 4 + descsz].hex()
        i = d.find(b"GNU\0", i + 1)
    return None

libs = set()
# A manifest's library: the Vulkan drivers' and layers', and glvnd's EGL
# vendors', each a path or a name the loader looks up.
for pattern, key in (("/usr/share/vulkan/icd.d/*.json", "ICD"),
                     ("/usr/share/vulkan/explicit_layer.d/*.json", "layer"),
                     ("/usr/share/glvnd/egl_vendor.d/*.json", "ICD")):
    for manifest in glob.glob(pattern):
        try:
            lib = json.load(open(manifest))[key]["library_path"]
        except (OSError, ValueError, KeyError, TypeError):
            continue
        for d in ("",) + DIRS:
            p = os.path.join(d, lib) if d else lib
            if os.path.isabs(p) and os.path.exists(p):
                libs.add(os.path.realpath(p))
                break
# Mesa's gallium drivers, one library behind every manifest that names
# none of them.
for d in DIRS:
    libs.update(os.path.realpath(p)
                for p in glob.glob(os.path.join(d, "libgallium*.so")))
for lib in sorted(libs):
    if os.path.basename(lib).startswith(CLOSED):
        if mode == "check":
            print("%s is closed: no debug info anywhere, so a checker names "
                  "no frame in it" % os.path.basename(lib))
        continue
    bid = build_id(lib)
    if not bid:
        continue
    dst = "/usr/lib/debug/.build-id/%s/%s.debug" % (bid[:2], bid[2:])
    name = os.path.basename(lib)
    if os.path.exists(dst):
        continue
    if mode == "check":
        print("no debug info for %s, so a checker names no frame in it "
              "-- --setup" % name)
        continue
    try:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        url = "%s/buildid/%s/debuginfo" % (server, bid)
        with urllib.request.urlopen(url, timeout=600) as r, \
                open(dst + ".part", "wb") as f:
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
        os.rename(dst + ".part", dst)
        print("debug info for %s" % name)
    except OSError as e:
        print("no debug info for %s: %s" % (name, e))
"""


# As root, after the packages: the account into group input, which the KMS
# backend reads the devices as, and uinput loaded now and at every boot,
# which the hands are made with. And the kernel's messages sent to VT 12
# (kbd's setlogcons), now and at every boot: nouveau on aspire's GM107
# logs an MMIO read fault at error level each time the GPU wakes, which
# the console printed over the VT in front -- the one a KMS round draws on
# and a hand reads. The journal keeps them, and Ctrl+Alt+F12 shows them.
# And on a machine with an NVIDIA GPU, nouveau's bus subdev quieted
# (debug=bus=fatal, read when the module is loaded from the root
# filesystem -- the helper's loads -- and not from an initramfs): those
# faults are the graphics engine's init running a VBIOS script that reads
# 0x619444 three times at every resume (gm107_gr_init_bios_2; freedesktop
# bug 111392, harmless, no fix), and one more at every bind, so every line
# the subdev has to say there is this one. nouveau's config=disp=0 was
# tried first and stops none of them: the reader is not the display's.
SETUP_ROOT = r"""
usermod -aG input %(user)s
modprobe uinput
echo uinput > /etc/modules-load.d/fenestal-uinput.conf
if grep -qs 0x10de /sys/bus/pci/devices/*/vendor; then
    cat > /etc/modprobe.d/fenestal-nouveau.conf <<EOF
# nouveau's bus subdev reports MMIO faults; on a GM107 the graphics
# engine's init runs a VBIOS script that reads 0x619444 three times at
# every resume (gm107_gr_init_bios_2; freedesktop bug 111392, harmless,
# no fix). fenestal: tools/drive/bare_driver.py --setup.
options nouveau debug=bus=fatal
EOF
fi
logcons=$(command -v setlogcons)
if [ -n "$logcons" ]; then
    cat > /etc/systemd/system/fenestal-logcons.service <<EOF
[Unit]
Description=fenestal: kernel messages on VT 12, not the VT in front

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$logcons 12

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload &&
        systemctl enable --now -q fenestal-logcons.service
else
    echo "no setlogcons (kbd): the kernel's messages stay on the VT in front"
fi
"""


# As root: tools/gpu-driver.sh, on stdin, installed for the library's
# switch (FNL_GPU_DRIVER), and two ways for the account to run it. systemd's
# socket first: its group's members may connect, the account is put in it,
# and each connection is a service of its own, in which a move outlives
# its caller, and whose OnFailure binds a driver again where a move was
# killed halfway (DESIGN *Another kernel driver, with the window open*).
# Then a sudoers rule that lets the account run it and nothing else, for a
# library that asks through sudo, checked by visudo before it is put in
# place. The harness reaches the helper as root either way.
SETUP_GPU_DRIVER = r"""
there=%(there)s
cat > "$there.new" && chmod 755 "$there.new" && mv "$there.new" "$there"
groupadd -f -r %(group)s && usermod -a -G %(group)s %(user)s || exit 1
units=/etc/systemd/system
cat > "$units/fenestal-gpu-driver.socket" <<EOF
[Unit]
Description=fenestal: an NVIDIA GPU moved between its kernel drivers

[Socket]
ListenStream=%(sock)s
SocketUser=root
SocketGroup=%(group)s
SocketMode=0660
Accept=yes
MaxConnections=8

[Install]
WantedBy=sockets.target
EOF
cat > "$units/fenestal-gpu-driver@.service" <<EOF
[Unit]
Description=fenestal: a move asked for on the GPU driver socket
OnFailure=fenestal-gpu-driver-rescue.service

[Service]
ExecStart=$there serve
StandardInput=socket
StandardOutput=socket
StandardError=journal
TimeoutStopSec=60
EOF
cat > "$units/fenestal-gpu-driver-rescue.service" <<EOF
[Unit]
Description=fenestal: an NVIDIA GPU a killed move left with no driver

[Service]
Type=oneshot
ExecStart=$there rescue
EOF
systemctl daemon-reload &&
    systemctl enable --now -q fenestal-gpu-driver.socket || exit 1
echo "$there installed behind %(sock)s, for group %(group)s," \
     "which %(user)s is in from the next login"
command -v sudo >/dev/null || exit 0
rule=/etc/sudoers.d/fenestal-gpu-driver
echo "%(user)s ALL=(root) NOPASSWD: $there" > "$rule.new"
chmod 440 "$rule.new"
if visudo -cqf "$rule.new"; then
    mv "$rule.new" "$rule"
    echo "and %(user)s may run it through sudo"
else
    rm -f "$rule.new"
    echo "visudo refused the rule for $there" >&2
    exit 1
fi
"""


def _main(argv):
    if len(argv) not in (2, 3) or argv[0] not in ("--status", "--setup",
                                                  "--up", "--weston",
                                                  "--cage", "--down",
                                                  "--driver"):
        sys.exit(__doc__)
    what, name = argv[0], argv[1]
    if what == "--driver":
        if len(argv) != 3 or argv[2] not in GPU_DRIVERS:
            sys.exit(__doc__)
        try:
            box = Bare(name, x=False)
            try:
                box.gpu_driver(argv[2])
            finally:
                box.close()
        except (Unreachable, DriverMoveFailed) as e:
            sys.exit(str(e))
        return 0
    dest = argv[2] if len(argv) == 3 else listed().get(name, name)
    if what == "--setup":
        d = DISTROS[distro_of(_text(_ssh(dest, "cat /etc/os-release")))]
        arch = _text(_ssh(dest, "uname -m")).strip()
        res = _ssh(as_root(dest), INSTALL % {
            "has": d["has"], "packages": " ".join(packages(d, arch)),
            "install": d["install"] % "$missing"}, timeout=900)
        said = _text(res).strip().splitlines()
        if said:
            print(said[-1])
        if res.returncode != 0:
            sys.exit("installing there: %s"
                     % res.stderr.decode("utf-8", "replace").strip())
        user = dest.rsplit("@", 1)[0] if "@" in dest else _text(
            _ssh(dest, "id -un")).strip()
        res = _ssh(as_root(dest), SETUP_ROOT % {"user": shlex.quote(user)})
        if res.returncode != 0:
            sys.exit("setting up input there: %s"
                     % res.stderr.decode("utf-8", "replace").strip())
        with open(GPU_DRIVER, "rb") as f:
            res = _ssh(as_root(dest), SETUP_GPU_DRIVER % {
                "there": GPU_DRIVER_THERE, "user": shlex.quote(user),
                "sock": GPU_DRIVER_SOCKET, "group": GPU_DRIVER_GROUP},
                data=f.read())
        print(_text(res).strip() or res.stderr.decode("utf-8",
                                                      "replace").strip())
        if res.returncode != 0:
            sys.exit("installing %s there failed" % GPU_DRIVER_THERE)
        res = _ssh(as_root(dest), "python3 - fetch %s" % d["debuginfod"],
                   timeout=1800, data=DEBUGINFO.encode())
        for line in _text(res).strip().splitlines():
            print(line)
        if name not in [ln.split("#")[0].split()[0] for ln in
                        _lines(HOSTS_FILE) if ln.split("#")[0].strip()]:
            os.makedirs(STATE_DIR, exist_ok=True)
            with open(HOSTS_FILE, "a") as f:
                f.write("%s %s\n" % (name, dest))
            print("wrote %s into %s" % (name, HOSTS_FILE))
    try:
        box = Bare(name, dest)
    except Unreachable as e:
        sys.exit(str(e))
    try:
        print("%s, as %s" % (name, box.dest))
        if what == "--up":
            box.x_up()
        elif what == "--weston":
            box.weston_up()
        elif what == "--cage":
            box.cage_up()
        elif what == "--down":
            box.x_down()
            box.weston_down()
            box.cage_down()
        for line in box.facts() + box.sh(STATUS % {
                "has": box.distro["has"],
                "packages": " ".join(packages(box.distro, box.arch)),
                "dno": DISPLAY_NO, "wsock": WAYLAND_SOCKET, "vt": VT,
                "cunit": CAGE_UNIT, "wpfx": box.wine_prefix(),
                "wine": "false" if "wine" in NOT_OFFERED.get(box.arch, ())
                        else "true",
                "gpudrv": GPU_DRIVER_THERE, "gpusock": GPU_DRIVER_SOCKET,
                "gpugroup": GPU_DRIVER_GROUP},
                check=False).splitlines() + box.sh(
                "python3 - check %s" % box.distro["debuginfod"],
                data=DEBUGINFO.encode(), check=False).splitlines():
            print("  " + line)
        print("  a round drives it without being told: %s"
              % ("yes" if name in hosts() else
                 "no -- --setup writes it into %s" % HOSTS_FILE))
    except Unreachable as e:
        sys.exit(str(e))
    finally:
        box.close()
    return 0


def _lines(path):
    try:
        with open(path) as f:
            return f.readlines()
    except OSError:
        return []


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
