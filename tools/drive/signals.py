#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Terminating signals at random moments, many times: during startup, while
# frames run, and once teardown has begun. The handler fenestal installs
# (FnlInitDesc::install_signal_handlers) turns a signal into the ordinary
# shutdown, and teardown waits out a handler still on its way to the App
# before freeing it; this is the round that would catch either going wrong.
#
#   python3 tools/drive/signals.py [target] [--variant tsan] [--runs 40]
#
# The target is one of this desktop's Linux environments (environment.py),
# which says the tree, the backend the program is told to open and the
# renderer it is told to draw with; a program under wine is signalled
# through wine, which is not this round.
#
# A run passes when the program exits within its bound -- with status 0 when
# the handler had been installed, or killed by the signal when it arrived
# before -- and its log holds no sanitizer report and no failed assertion.
# Needs a desktop, as the stress round does, but sends no input: no portal,
# no focus, nothing typed.

import argparse
import os
import random
import signal
import subprocess
import sys
import time

import environment as B

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

REPORTS = ('ERROR: AddressSanitizer', 'WARNING: ThreadSanitizer',
           'runtime error:', 'assertion failed', 'LeakSanitizer')


def one(exe, sig, delay, second, env):
    proc = subprocess.Popen([exe], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env)
    time.sleep(delay)
    proc.send_signal(sig)
    if second is not None:
        # A second signal, into a teardown the first one began.
        time.sleep(second)
        if proc.poll() is None:
            proc.send_signal(sig)
    try:
        _, err = proc.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        _, err = proc.communicate()
        return 'hung', err.decode('utf-8', 'replace')
    log = err.decode('utf-8', 'replace')
    if any(r in log for r in REPORTS):
        return 'report', log
    if proc.returncode not in (0, -sig):
        return 'status %d' % proc.returncode, log
    return None, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', nargs='?', default='x11-vulkan')
    ap.add_argument('--variant', default='tsan')
    ap.add_argument('--runs', type=int, default=40)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()

    cls = next((c for c in B.ALL if c.NAME == args.target and c.LOCAL
                and c.BACKEND), None)
    if cls is None:
        sys.exit('signals: %s is not a Linux environment of this desktop: '
                 '%s' % (args.target, ', '.join(c.NAME for c in B.ALL
                                                if c.LOCAL and c.BACKEND)))
    exe = os.path.join(ROOT, 'build', cls.TREE or cls.NAME, args.variant,
                       'fenestal-testbed')
    if not os.path.exists(exe):
        sys.exit('signals: no %s; build it first' % exe)
    env = cls.environ(TSAN_OPTIONS='suppressions=%s'
                      % os.path.join(ROOT, 'tsan.supp'),
                      LSAN_OPTIONS='suppressions=%s'
                      % os.path.join(ROOT, 'lsan.supp'))

    rng = random.Random(args.seed)
    sigs = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    failed = 0
    for i in range(args.runs):
        sig = rng.choice(sigs)
        # Mostly while frames run; one in four before the window is up, when
        # the handler may not be installed yet.
        delay = rng.uniform(0.0, 0.3) if rng.random() < 0.25 else \
            rng.uniform(0.5, 3.0)
        second = rng.uniform(0.0, 0.05) if rng.random() < 0.3 else None
        why, log = one(exe, sig, delay, second, env)
        line = '%3d %-8s at %.2f s%s' % (
            i + 1, signal.Signals(sig).name, delay,
            ' +%.3f s again' % second if second is not None else '')
        print('%-40s %s' % (line, why or 'ok'), flush=True)
        if why:
            failed += 1
            print('\n'.join(log.splitlines()[-30:]))
    print('==== %s/%s: %d runs, %d failed ====' % (
        args.target, args.variant, args.runs, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
