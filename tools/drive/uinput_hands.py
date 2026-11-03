# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""A keyboard and a mouse that are not there, on the bare machine.

Run *there*, as root, by bare_driver.Bare.uinput_hands() -- never on this
desktop. It makes two uinput devices, which the kernel then offers like any
others: weston reads them through libinput, and the KMS backend reads them
through evdev itself, so what is driven is the whole path a hand takes. It
is the Wayland and the bare display's XTEST, and allowed for XTEST's reason
on that machine: it reaches that machine's input and nothing of this one.

One command a line on stdin, each answered with "ok" once written, so the
harness here knows the event has left before it looks for its echo:

    key CODE 0|1      an evdev key code, up or down
    btn CODE 0|1      a button, BTN_LEFT 272 and on
    rel DX DY         relative motion, whole counts
    abs X Y           a place, in pixels (--absolute alone)
    wheel N           detents, positive away from the user (REL_WHEEL)

`--absolute W H` makes the mouse an absolute one, a pointer at a place as
a virtual machine's tablet is, over a W by H screen: for a compositor that
accelerates a relative pointer and has no setting to stop it (cage). The
axes are 0 to W-1 and 0 to H-1, so libinput's scaling of a count to the
screen is the identity and a place asked for is the place reached.

The devices go when stdin closes. Standard library only: the machine has
python3 and nothing of this tree but this file.
"""

import fcntl
import os
import struct
import sys
import time

# <linux/uinput.h>, by hand: _IOW('U', n, int) and the setup structure.
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_RELBIT = 0x40045566
UI_SET_ABSBIT = 0x40045567
UI_ABS_SETUP = 0x401C5504          # _IOW('U', 4, struct uinput_abs_setup), 28
UI_DEV_SETUP = 0x405C5503          # _IOW('U', 3, struct uinput_setup), 92
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502

EV_SYN, EV_KEY, EV_REL, EV_ABS, EV_REP = 0x00, 0x01, 0x02, 0x03, 0x14
SYN_REPORT = 0
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
ABS_X, ABS_Y = 0x00, 0x01
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
BUS_VIRTUAL = 0x06

# struct input_event on a 64-bit kernel: a timeval the kernel fills in, then
# type, code and value.
EVENT = struct.Struct("llHHi")
# How long a new device takes to be found: udev sets its group, inotify
# tells the KMS backend, and libinput adds it in weston. Nothing says when
# all three are done, so the first command waits this long after creation.
SETTLE = 1.0


def device(name, keys=(), rels=(), absolute=(), repeat=False):
    """`absolute` is (code, largest) per axis, each from 0."""
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    if keys:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
        for k in keys:
            fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    if rels:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
        for r in rels:
            fcntl.ioctl(fd, UI_SET_RELBIT, r)
    if absolute:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_ABS)
        for code, top in absolute:
            fcntl.ioctl(fd, UI_SET_ABSBIT, code)
            # code, then struct input_absinfo: value, minimum, maximum,
            # fuzz, flat, resolution.
            fcntl.ioctl(fd, UI_ABS_SETUP,
                        struct.pack("Hxx6i", code, 0, 0, top, 0, 0, 0))
    if repeat:
        # The kernel's own repeat, which is the only one the bare display
        # has; a compositor drops it and repeats for itself.
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_REP)
    setup = struct.pack("HHHH80sI", BUS_VIRTUAL, 0xF3E5, 0x0001, 1,
                        name.encode(), 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def emit(fd, *events):
    os.write(fd, b"".join(EVENT.pack(0, 0, t, c, v) for t, c, v in events)
             + EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0))


def main(argv):
    kb = device("fenestal-drive keyboard", keys=range(1, 248), repeat=True)
    buttons = (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE)
    if argv[:1] == ["--absolute"]:
        w, h = int(argv[1]), int(argv[2])
        mouse = device("fenestal-drive tablet", keys=buttons,
                       rels=(REL_WHEEL,),
                       absolute=((ABS_X, w - 1), (ABS_Y, h - 1)))
    else:
        mouse = device("fenestal-drive mouse", keys=buttons,
                       rels=(REL_X, REL_Y, REL_WHEEL))
    try:
        time.sleep(SETTLE)
        print("ready", flush=True)
        for line in sys.stdin:
            words = line.split()
            if not words:
                continue
            what, args = words[0], [int(w) for w in words[1:]]
            if what == "key":
                emit(kb, (EV_KEY, args[0], args[1]))
            elif what == "btn":
                emit(mouse, (EV_KEY, args[0], args[1]))
            elif what == "rel":
                # A zero axis says nothing, so it is not said.
                emit(mouse, *[(EV_REL, axis, v) for axis, v in
                              ((REL_X, args[0]), (REL_Y, args[1])) if v])
            elif what == "abs":
                emit(mouse, (EV_ABS, ABS_X, args[0]), (EV_ABS, ABS_Y, args[1]))
            elif what == "wheel":
                emit(mouse, (EV_REL, REL_WHEEL, args[0]))
            else:
                print("unknown %s" % what, flush=True)
                continue
            print("ok", flush=True)
    finally:
        for fd in (kb, mouse):
            fcntl.ioctl(fd, UI_DEV_DESTROY)
            os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
