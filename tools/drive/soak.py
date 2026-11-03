#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# A long run, for what no round is long enough to show: memory that grows a
# little with every resize. fenestal-dev pace asks for sizes from a thread of
# its own for the whole run, and the resident set is sampled throughout; after a
# warm-up -- a driver allocates as it goes, and a pipeline cache fills -- it
# must stop growing.
#
# The resident set is honest only in a release build: under asan, freed
# memory waits in a quarantine of up to 256 MiB before it is reused, and the
# line climbs until that is full -- 116 to 384 MiB over ten minutes on X11,
# where release held at 67,100 to 67,116 KiB over the same ten. So under a
# checker the growth is printed and not judged, and the checker's own
# reports, the leak check at exit among them, are what fail the run.
#
#   python3 tools/drive/soak.py                         # x11, release, an hour
#   python3 tools/drive/soak.py x11-vulkan --variant asan --minutes 30
#
# The target is one of this desktop's environments (environment.py), which
# says the tree, what runs the program, the backend it is told to open and
# the renderer it is told to draw with -- win64-vulkan and win64-d3d12 are the
# one win64 tree, the second run from the copy it stages beside
# vkd3d-proton.
#
# Opt-in: nothing runs it, and it holds a window on the desktop for as long as
# it is told. It sends no input. What it leaves is a line in the ledger.

import argparse
import os
import subprocess
import sys
import time

import environment as B
import ledger

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

REPORTS = ('ERROR: AddressSanitizer', 'ERROR: LeakSanitizer',
           'WARNING: ThreadSanitizer', 'runtime error:', 'assertion failed')

# What the resident set may still grow by after the warm-up, as a fraction
# of where it stood then: allocator noise and a driver's late arenas, not a
# leak, which grows with the resizes and so with the run.
SLACK = 0.05


def rss_kib(pid):
    with open('/proc/%d/status' % pid) as fh:
        for line in fh:
            if line.startswith('VmRSS:'):
                return int(line.split()[1])
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', nargs='?', default='x11-vulkan')
    ap.add_argument('--variant', default='release')
    ap.add_argument('--minutes', type=float, default=60.0)
    ap.add_argument('--warmup', type=float, default=2.0,
                    help='minutes before the baseline is taken')
    ap.add_argument('--rate', type=int, default=15,
                    help='size requests a second')
    args = ap.parse_args()

    cls = next((c for c in B.ALL if c.NAME == args.target and c.LOCAL),
               None)
    if cls is None:
        sys.exit('soak: %s is not an environment of this desktop: %s'
                 % (args.target, ', '.join(c.NAME for c in B.ALL
                                           if c.LOCAL)))
    exe = os.path.join(ROOT, 'build', cls.TREE or cls.NAME, args.variant,
                       'fenestal-dev' + cls.SUFFIX)
    runner = list(cls.RUNNER)
    if not os.path.exists(exe):
        sys.exit('soak: no %s; build it first' % exe)
    try:
        cls.clear()
        exe = cls.stage(exe, args.variant)
    except B.Unavailable as e:
        sys.exit('soak: %s' % e)

    seconds = args.minutes * 60.0
    env = cls.environ(TSAN_OPTIONS='suppressions=%s' %
                      os.path.join(ROOT, 'tsan.supp'),
                      LSAN_OPTIONS='suppressions=%s' %
                      os.path.join(ROOT, 'lsan.supp'))
    log = os.path.join(ROOT, 'build', 'drive', 'soak-%s-%s.log'
                       % (args.target, args.variant))
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, 'w') as out:
        proc = subprocess.Popen(runner + [exe, 'pace', str(args.rate),
                                          str(int(seconds) + 5)],
                                stdout=out, stderr=subprocess.STDOUT,
                                env=env)
        start = time.time()
        base = None
        samples = []
        while proc.poll() is None and time.time() - start < seconds:
            time.sleep(10.0)
            try:
                kib = rss_kib(proc.pid)
            except OSError:
                break
            t = (time.time() - start) / 60.0
            samples.append((t, kib))
            if base is None and t >= args.warmup:
                base = kib
            print('%6.1f min  %8d KiB%s' % (
                t, kib, '' if base is None else
                '  %+.1f%%' % (100.0 * (kib - base) / base)), flush=True)
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    with open(log, errors='replace') as fh:
        text = fh.read()
    reports = [r for r in REPORTS if r in text]
    end = samples[-1][1] if samples else 0
    grew = (end - base) / base if base else 0.0
    failed = []
    if base is None:
        failed.append('ran %.1f min, short of the warm-up'
                      % (samples[-1][0] if samples else 0))
    elif grew > SLACK and args.variant in ('release', 'gc'):
        failed.append('resident set grew %.1f%% after the warm-up'
                      % (100 * grew))
    if reports:
        failed.append('the log has %s' % ', '.join(reports))

    print('==== %s/%s: %.0f min at %d sizes/s, %d -> %d KiB after the '
          'warm-up: %s ====' % (args.target, args.variant, args.minutes,
                                args.rate, base or 0, end,
                                '; '.join(failed) or 'held'))
    ledger.record('soak', ledger.place(args.target), args.variant,
                  checks=2, failed=len(failed), fails=failed,
                  minutes=args.minutes, rss_base=base, rss_end=end,
                  **ledger.wine(cls))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
