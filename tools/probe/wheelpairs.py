#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Pairs of wheel steps from a virtual mouse: wheelpairs.py PAIRS [GAP_MS].

wheelcount.c's other half (docs/wine-wheel-merge.md). Each pair is two
REL_WHEEL events of -1, GAP_MS apart (1 by default), with 250 ms between
pairs, after a nudge of the pointer that gives the window under it the
pointer's focus. Run as root on the machine; the compositor sees a new
mouse, gone when this exits."""
import fcntl
import os
import struct
import sys
import time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x5501, 0x5502
EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y, REL_WHEEL, BTN_LEFT = 0, 1, 8, 0x110
EVENT = struct.Struct("llHHi")

def emit(fd, *events):
    os.write(fd, b"".join(EVENT.pack(0, 0, t, c, v) for t, c, v in events)
             + EVENT.pack(0, 0, EV_SYN, 0, 0))

pairs = int(sys.argv[1])
gap = (float(sys.argv[2]) if len(sys.argv) > 2 else 1.0) / 1000
fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
fcntl.ioctl(fd, UI_SET_KEYBIT, BTN_LEFT)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_REL)
for rel in (REL_X, REL_Y, REL_WHEEL):
    fcntl.ioctl(fd, UI_SET_RELBIT, rel)
fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack("HHHH80sI", 6, 0xF3E5, 2, 1,
                                          b"wheelpairs mouse", 0))
fcntl.ioctl(fd, UI_DEV_CREATE)
try:
    time.sleep(1.0)
    emit(fd, (EV_REL, REL_X, 1))
    emit(fd, (EV_REL, REL_X, -1))
    time.sleep(0.5)
    for _ in range(pairs):
        emit(fd, (EV_REL, REL_WHEEL, -1))
        time.sleep(gap)
        emit(fd, (EV_REL, REL_WHEEL, -1))
        time.sleep(0.25)
    time.sleep(1.0)
    print("sent %d pairs, %d steps" % (pairs, 2 * pairs), flush=True)
finally:
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)
