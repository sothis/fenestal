#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Which system a laptop boots, a reboot into it, and waking one that is off.

    python3 tools/drive/power.py where schenker      # stick, windows, macos
    python3 tools/drive/power.py boot schenker windows
    python3 tools/drive/power.py boot aspire stick
    python3 tools/drive/power.py boot schenker macos
    python3 tools/drive/power.py learn thinkpad      # its MACs, for wake
    python3 tools/drive/power.py wake thinkpad

A laptop here runs either its live key (the boot images'
repository's, root by ssh) or its installed Windows (the native driver's
machine, the account by ssh), under one name: root@NAME answering is the
key, NAME answering `ver` is Windows. `boot` sets the firmware's next
boot for one boot only and reboots: from the key, efibootmgr's BootNext
-- Windows Boot Manager, or the entry the key booted from, since the
stick is not first in every machine's order -- and from Windows,
bcdedit's {fwbootmgr} bootsequence for the stick's firmware entry, or a
plain restart where there is none (a firmware that tries USB first, or
schenker's rEFInd, which picks the stick when it is there). It then
waits for the system asked for and says which came up, and into Windows
or macOS waits on until the logon's input is too old for a round to take
for a hand's (SETTLE) -- on the Mac until IOHIDSystem's own idle clock,
the one the agent reads, has passed the window from the desktop's login
on and not from ssh's first answer. Nothing changes the firmware's order
itself.

A machine with a macOS stick (OpenCore and macOS on one USB stick) names
that stick's firmware entry, by its description, in `macos-entry-NAME`
under the state directory; NAME answering `uname -s` with Darwin is macOS,
whose host key known_hosts holds as NAME-mac.local.
The way in is the entry's next boot, from the key or from Windows. The
way out is a plain restart, which the firmware's order takes to the key:
OpenCore routes macOS's boot variables into a space of its own
(RequestBootVarRouting), so macOS cannot name the firmware's next boot,
and Windows is reached through the key. The restart is `sudo -n
shutdown -r now`, which wants a sudoers line for the account (`<user>
ALL=(root) NOPASSWD: /sbin/shutdown`).

`wake` sends a magic packet to each wired port `learn` found with
Wake-on-LAN; the key turns it on at every boot (fenestal-boot ok).
Whether a machine wakes from off is its firmware's, and only a try shows.

A machine named in `cold-hosts` under the state directory, a name a line,
goes into Windows and macOS from off and never by a restart: from the key
the next boot is set as above and the machine powered off, then woken.
One that is in another system goes to the key first, by a restart, and
the way back to the key is a restart too. On schenker a restart from the
key into Windows left the panel dark twice in a day (eDP link training
failing; only a power drain brought it back), where a start from off,
into Windows and into macOS, and a restart from Windows to the key did
not. `boot --cold` or `--warm` chooses for one boot, whatever the list
says; the machine must be in `wol-hosts` either way it goes cold.
"""

import argparse
import os
import socket
import subprocess
import time

STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "fenestal")
WOL_HOSTS = os.path.join(STATE, "wol-hosts")
COLD_HOSTS = os.path.join(STATE, "cold-hosts")
SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=5"]

# A reboot down and up again: Windows with its updates is the slow one.
DOWN_TIMEOUT = 120.0
UP_TIMEOUT = 420.0
# From a machine that stopped answering to the magic packet: nothing
# answering ssh is not yet powered off with its port armed. 10 s was
# enough on schenker, once each into Windows and macOS; a packet lost
# anyway is sent again while the machine stays off (WAKE_AGAIN).
COLD_SETTLE = 10.0
WAKE_AGAIN = 30.0
# How long a round leaves a machine alone when its last input was not the
# round's own: native_driver.BUSY and mac_driver.BUSY, whose variables
# these are and whose default they share. A machine just logged in
# reports the logon as its last input and neither system says whose it
# was, so boot() waits the window out after the systems those drivers
# drive; read and not imported, so power.py stays a file of its own.
SETTLE = {"windows": ("FNL_DRIVE_NATIVE_BUSY", 120.0),
          "macos": ("FNL_DRIVE_MAC_BUSY", 120.0)}
# How long a Mac's desktop login may lag ssh, which Remote Login answers
# at boot: schenker's was 25 s late (2026-11-04), so a wait counted from
# the first answer leaves the login's own input inside the window.
LOGIN_TIMEOUT = 180.0


def ssh(target, command, timeout=60, opts=()):
    # Windows answers in its console's code page and its own language.
    return subprocess.run(["ssh"] + SSH_OPTS + list(opts) + [target, command],
                          capture_output=True, text=True, errors="replace",
                          timeout=timeout)


def going_down(target, command, opts=()):
    """ssh() for the command that restarts the machine, or None when the
    connection hung instead of dropping: a system shutting down does that,
    and it is the restart and no failure."""
    try:
        return ssh(target, command, opts=opts)
    except subprocess.TimeoutExpired:
        return None


def mac_opts(host):
    """macOS shares NAME and its address with the key and Windows but has
    a host key of its own, so known_hosts keeps it under NAME-mac.local;
    under NAME's own alias ssh would refuse it as a changed key."""
    return ["-o", "HostKeyAlias=%s-mac.local" % host]


# The systems `where` can find a machine up in.
UP = ("stick", "windows", "macos")


def where(host):
    """'stick', 'windows', 'macos', 'off', or 'unknown' when a system
    answered the connection and then not in time -- one going down or
    coming up, which is neither."""
    try:
        if ssh("root@" + host, "uname -s", 20).stdout.strip() == "Linux":
            return "stick"
        if "Windows" in ssh(host, "ver", 20).stdout:
            return "windows"
        if ssh(host, "uname -s", 20,
               mac_opts(host)).stdout.strip() == "Darwin":
            return "macos"
    except subprocess.TimeoutExpired:
        return "unknown"
    return "off"


# The stick's firmware entry, by the description the firmware gives it,
# kept from the last time the machine was seen on its stick: Windows lists
# the same description, and a name a USB stick may have could also be an
# internal disk's ("UEFI: <model>").
def stick_file(host):
    return os.path.join(STATE, "stick-entry-" + host)


def remember_stick(host, desc):
    os.makedirs(STATE, exist_ok=True)
    with open(stick_file(host), "w") as f:
        f.write(desc + "\n")


def stick_entry(host):
    try:
        with open(stick_file(host)) as f:
            return f.read().strip() or None
    except FileNotFoundError:
        return None


# The macOS stick's entry is written by hand: nothing boots from it that
# could remember it, and its description must not be guessed, since the
# key's is "UEFI: ..." as well. Leading and inner spaces are the
# firmware's ("UEFI:  PHILIPS, Partition 1"), so only the newline goes.
def macos_entry(host):
    try:
        with open(os.path.join(STATE, "macos-entry-" + host)) as f:
            return f.read().rstrip("\n") or None
    except FileNotFoundError:
        return None


def need_macos_entry(host):
    desc = macos_entry(host)
    if desc is None:
        raise SystemExit("%s: no macOS stick; name its firmware entry in %s"
                         % (host, os.path.join(STATE, "macos-entry-" + host)))
    return desc


def efi_entries(host):
    out = ssh("root@" + host, "efibootmgr").stdout
    current, entries = None, {}
    for line in out.splitlines():
        if line.startswith("BootCurrent:"):
            current = line.split()[1]
        elif line.startswith("Boot") and line[4:8].isalnum() \
                and len(line) > 9 and line[8] in "* ":
            entries[line[4:8]] = line[9:].split("\t")[0].strip()
    return current, entries


def from_stick(host, target, off=False):
    if ssh("root@" + host, "test -d /sys/firmware/efi").returncode != 0:
        if target != "stick":
            raise SystemExit("%s boots by BIOS; its firmware has no next "
                             "boot to set" % host)
        nxt = None
    else:
        current, entries = efi_entries(host)
        # A key OpenCore started sees OpenCore's routed boot variables, not
        # the firmware's: no BootCurrent and no entries.
        if current is None and not entries:
            raise SystemExit("%s: efibootmgr lists no entries -- a key "
                             "started through OpenCore?" % host)
        if current in entries:
            remember_stick(host, entries[current])
        if target == "windows":
            nxt = next((k for k, v in entries.items()
                        if v == "Windows Boot Manager"), None)
            if nxt is None:
                raise SystemExit("%s: no Windows Boot Manager entry" % host)
        elif target == "macos":
            desc = need_macos_entry(host)
            nxt = next((k for k, v in entries.items() if v == desc), None)
            if nxt is None:
                raise SystemExit("%s: no firmware entry %r; is the macOS "
                                 "stick in?" % (host, desc))
        else:
            nxt = current
    cmd = "sync; systemctl " + ("poweroff" if off else "reboot")
    if nxt:
        # Braced: `a && b; c` runs c whether or not a did.
        cmd = "efibootmgr -q -n %s && { %s; }" % (nxt, cmd)
        print("%s: next boot %s (Boot%s)" % (host, target, nxt))
    r = going_down("root@" + host, cmd)
    # 255 is ssh's own, for the connection the restart took away.
    if nxt and r is not None and r.returncode not in (0, 255):
        raise SystemExit("%s: the next boot or the restart failed: %s"
                         % (host, r.stderr.strip()))


# The stick's firmware entry as Windows lists it, in order of preference:
# the names the machines here give the stick itself, then a firmware's
# generic USB entry.
USB_NAMES = ("UEFI:", "USB HDD", "EFI USB Device")


def firmware_apps(out):
    """(identifier, description) of each block of `bcdedit /enum
    firmware`. The field names are the display language's (identifier,
    Bezeichner), so a block's identifier is its first line after the
    dashes under its heading; `description` is not translated."""
    apps, ident, fresh = [], None, False
    for line in out.splitlines():
        stripped = line.strip()
        if stripped and set(stripped) == {"-"}:
            fresh = True
            continue
        parts = stripped.split(None, 1)
        if fresh and len(parts) == 2:
            ident, fresh = parts[1], False
        elif len(parts) == 2 and parts[0] == "description" and ident:
            apps.append((ident, parts[1]))
    return apps


def from_windows(host, target):
    cmd = "shutdown /r /t 0"
    if target == "stick":
        apps = firmware_apps(ssh(host, "bcdedit /enum firmware").stdout)
        known = stick_entry(host)
        # The macOS stick is a USB entry too, and never the key.
        mac = macos_entry(host)
        chosen = next((i for i, d in apps if d == known), None) or \
            next((i for name in USB_NAMES for i, d in apps
                  if d.startswith(name) and d != mac), None)
        if chosen:
            cmd = 'bcdedit /set {fwbootmgr} bootsequence %s && %s' \
                % (chosen, cmd)
            print("%s: next boot stick (%s)" % (host, chosen))
        else:
            print("%s: no USB firmware entry listed; a plain restart, the "
                  "firmware choosing" % host)
    elif target == "macos":
        desc = need_macos_entry(host)
        apps = firmware_apps(ssh(host, "bcdedit /enum firmware").stdout)
        chosen = next((i for i, d in apps if d == desc), None)
        if chosen is None:
            raise SystemExit("%s: no firmware entry %r; is the macOS stick "
                             "in?" % (host, desc))
        cmd = 'bcdedit /set {fwbootmgr} bootsequence %s && %s' % (chosen, cmd)
        print("%s: next boot macos (%s)" % (host, chosen))
    going_down(host, cmd)


def from_macos(host, target):
    """A plain restart, which the firmware's order takes to the key; see
    the module's comment for why macOS cannot choose."""
    if target != "stick":
        raise SystemExit("%s: macOS restarts to the key only" % host)
    # The connection drops as the machine goes down, so only sudo's own
    # refusal is an error.
    r = going_down(host, "sudo -n /sbin/shutdown -r now", mac_opts(host))
    if r is not None and "password" in r.stderr:
        raise SystemExit("%s: sudo asks for a password; the account wants "
                         "a NOPASSWD line for /sbin/shutdown in "
                         "/etc/sudoers.d" % host)
    print("%s: restart from macos" % host)


def wait_for(host, start):
    """Wait for HOST to leave START and come up again. Gone is off, or the
    other system already: ssh to a system shutting down hangs ("unknown")
    rather than refuses, so a fast restart may never be seen off."""
    t0 = time.monotonic()
    now = where(host)
    while now in (start, "unknown"):
        if time.monotonic() - t0 > DOWN_TIMEOUT:
            raise SystemExit("%s did not go down" % host)
        time.sleep(3)
        now = where(host)
    while True:
        if now in UP:
            print("%s: %s, after %.0f s" % (host, now, time.monotonic() - t0))
            return now
        if time.monotonic() - t0 > UP_TIMEOUT:
            raise SystemExit("%s did not come back in %.0f s"
                             % (host, UP_TIMEOUT))
        time.sleep(5)
        now = where(host)


FROM = {"stick": from_stick, "windows": from_windows, "macos": from_macos}


def listed_cold(host):
    try:
        with open(COLD_HOSTS) as f:
            return any(ln.split("#")[0].split()[:1] == [host] for ln in f)
    except FileNotFoundError:
        return False


def wait_off(host):
    t0 = time.monotonic()
    while where(host) != "off":
        if time.monotonic() - t0 > DOWN_TIMEOUT:
            raise SystemExit("%s did not power off" % host)
        time.sleep(3)
    print("%s: off, after %.0f s" % (host, time.monotonic() - t0))


def desktop_login(host):
    """Wait for the Mac's desktop user, whose login restarts the idle
    clock.

    ssh answers as soon as Remote Login is up, before the desktop's own
    login, so a wait counted from the first answer ends before the login's
    input has aged. /dev/console's owner is the desktop's user, root being
    the login screen's, and it changes at the login that matters."""
    t0 = time.monotonic()
    while True:
        r = ssh(host, "stat -f %Su /dev/console", 10, mac_opts(host))
        if r.returncode == 0 and r.stdout.strip() not in ("", "root"):
            return
        if time.monotonic() - t0 > LOGIN_TIMEOUT:
            print("%s: no desktop login after %.0f s; a round will say why"
                  % (host, LOGIN_TIMEOUT))
            return
        time.sleep(5)


def mac_idle(host):
    """Seconds since the Mac's last HID input, IOHIDSystem's own clock.

    The agent reads the same HIDIdleTime (mac_agent.m), so a wait on it
    is the driver's own test and not a guess at how long a login takes to
    finish; None where the machine will not say."""
    r = ssh(host, "ioreg -c IOHIDSystem -r -d 1", 10, mac_opts(host))
    if r.returncode != 0:
        return None
    for line in r.stdout.splitlines():
        if "HIDIdleTime" in line and "=" in line:
            try:
                return int(line.rsplit("=", 1)[1].strip()) / 1e9
            except ValueError:
                return None
    return None


def mac_settle(host, wait):
    """Wait for the login and then the idle clock the round will read."""
    desktop_login(host)
    t0 = time.monotonic()
    while True:
        idle = mac_idle(host)
        if idle is None:
            # A machine that will not say: the wait it is, then.
            print("%s: no idle time from ioreg; waiting %.0f s"
                  % (host, wait))
            time.sleep(wait)
            return
        if idle >= wait:
            return
        if time.monotonic() - t0 > wait + LOGIN_TIMEOUT:
            print("%s: the idle clock still stands at %.0f s" % (host, idle))
            return
        time.sleep(5)


def settle(host, target):
    """Wait out the window in which a round takes a logon's input for a
    hand's.

    A machine that has just logged in reports the logon as its last
    input, and the drivers take input they did not send as somebody's
    hand and skip the round. The logon this tool started is nobody's, so
    the wait belongs here, once, and not in every round after a boot."""
    if target not in SETTLE:
        return
    var, default = SETTLE[target]
    try:
        wait = float(os.environ.get(var, default))
    except ValueError:
        wait = default
    if wait <= 0:
        return
    if target == "macos":
        print("%s: waiting for the desktop login, then %.0f s of idle"
              % (host, wait))
        mac_settle(host, wait)
        return
    print("%s: waiting %.0f s for the logon's input to age" % (host, wait))
    time.sleep(wait)


def boot(host, target, cold=None):
    now = where(host)
    if now not in UP:
        raise SystemExit("%s is %s; wake it first" % (host, now))
    if cold is None:
        cold = listed_cold(host)
    cold = cold and target != "stick"
    if (now == "macos" and target == "windows") or \
            (cold and now != "stick"):
        boot(host, "stick", cold=False)
        now = "stick"
    if cold:
        if not wol_macs(host):
            raise SystemExit("%s: no MAC known to wake it by; `learn` it "
                             "while it is up, or boot it --warm" % host)
        from_stick(host, target, off=True)
        wait_off(host)
        time.sleep(COLD_SETTLE)
        came = wake(host, True)
    else:
        FROM[now](host, target)
        came = wait_for(host, now)
    if came != target:
        raise SystemExit("%s came up in %s, not %s" % (host, came, target))
    settle(host, target)


def learn(host):
    out = ssh("root@" + host, """
        for i in /sys/class/net/en*; do
            n=${i##*/}
            ethtool "$n" 2>/dev/null | grep -q 'Supports Wake-on:.*g' &&
                echo "$n $(cat $i/address)"
        done""").stdout.split()
    macs = out[1::2]
    if not macs:
        raise SystemExit("%s: no wired port with Wake-on-LAN" % host)
    lines = []
    if os.path.exists(WOL_HOSTS):
        with open(WOL_HOSTS) as f:
            lines = [ln for ln in f if ln.split()[:1] != [host]]
    lines.append("%s %s\n" % (host, " ".join(macs)))
    os.makedirs(STATE, exist_ok=True)
    with open(WOL_HOSTS, "w") as f:
        f.writelines(lines)
    print("%s: %s" % (host, " ".join(macs)))


def wol_macs(host):
    macs = []
    if os.path.exists(WOL_HOSTS):
        with open(WOL_HOSTS) as f:
            for ln in f:
                w = ln.split()
                if w[:1] == [host]:
                    macs = w[1:]
    return macs


def magic(macs):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    for mac in macs:
        packet = b"\xff" * 6 + bytes.fromhex(mac.replace(":", "")) * 16
        for _ in range(3):
            s.sendto(packet, ("255.255.255.255", 9))
    s.close()


def wake(host, wait):
    """The magic packet, and with `wait` the system the machine comes up
    in."""
    macs = wol_macs(host)
    if not macs:
        raise SystemExit("%s: no MAC known; `learn` it while it is up"
                         % host)
    magic(macs)
    print("%s: magic packet to %s" % (host, " ".join(macs)))
    if not wait:
        return None
    t0 = sent = time.monotonic()
    while time.monotonic() - t0 < UP_TIMEOUT:
        now = where(host)
        if now in UP:
            print("%s: %s, after %.0f s" % (host, now, time.monotonic() - t0))
            return now
        # Still off: the packet may have come before the port was armed.
        # A machine already starting answers none, so a second costs
        # nothing.
        if now == "off" and time.monotonic() - sent > WAKE_AGAIN:
            magic(macs)
            sent = time.monotonic()
        time.sleep(5)
    raise SystemExit("%s did not wake in %.0f s" % (host, UP_TIMEOUT))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("where").add_argument("host")
    b = sub.add_parser("boot")
    b.add_argument("host")
    b.add_argument("target", choices=list(UP))
    how = b.add_mutually_exclusive_group()
    how.add_argument("--cold", dest="cold", action="store_true",
                     default=None, help="from off, whatever cold-hosts says")
    how.add_argument("--warm", dest="cold", action="store_false",
                     help="by a restart, whatever cold-hosts says")
    sub.add_parser("learn").add_argument("host")
    w = sub.add_parser("wake")
    w.add_argument("host")
    w.add_argument("--no-wait", action="store_true")
    a = ap.parse_args()
    if a.cmd == "where":
        print(where(a.host))
    elif a.cmd == "boot":
        boot(a.host, a.target, a.cold)
    elif a.cmd == "learn":
        learn(a.host)
    else:
        wake(a.host, not a.no_wait)


if __name__ == "__main__":
    main()
