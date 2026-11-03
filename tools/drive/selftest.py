#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Each checker made to see the one bug it exists for. Silence from a checker
# is worth something only if it was switched on, and a tree can lose its
# instrumentation to a cache, a flag that stopped being passed or a runtime
# that is no longer loaded without anything failing. So every Debug tree
# there is commits its bugs on request (FNL_SELFTEST, src/core/fault.c) and
# this checks that each was reported.
#
#   python3 tools/drive/selftest.py
#
#   asan tree    asan, ubsan, assert
#   tsan tree    tsan, assert
#   gc tree      assert
#   under GL     gl as well: an invalid GL call, which the debug context's
#                output must bring to the log
#   win64 gc     ubsan (a trap: UBSan without a runtime), assert, under wine
#                drawn with Vulkan
#
# A Linux tree is run on both of this desktop's backends, each named by
# FNL_BACKEND as a round names it: the bug is committed in fnl_init(), and
# GL's is committed through a context each backend's bridge made. Every
# program is told its renderer by FNL_RENDERER, as a round's is: each tree
# has every renderer of its system, and GL's bug is committed only where
# OpenGL was chosen.
#
# The program is the testbed, which every tree builds. A tree that is not
# built is skipped and says so. Needs a desktop: the program opens its
# window, and the bug is committed in fnl_init().

import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

EXPECT = {
    'asan':   r'AddressSanitizer: heap-buffer-overflow',
    'ubsan':  r'runtime error: signed integer overflow',
    'tsan':   r'ThreadSanitizer: data race',
    'assert': r'assertion failed: held',
    'gl':     r'GL: .*INVALID_ENUM',
}
# A trap is an illegal instruction, which wine reports by its exception code.
TRAP = r'c000001d|illegal instruction|EXCEPTION_ILLEGAL_INSTRUCTION'

# (tree, backend, renderer, variant, controls); the backend is
# FNL_BACKEND's, and empty for a tree that has one backend alone; the
# renderer is FNL_RENDERER's. The bugs committed in fnl_init() are the same
# whichever renderer follows, so the win64 tree runs them once.
TREES = [
    ('linux', 'x11', 'vulkan', 'asan', ['asan', 'ubsan', 'assert']),
    ('linux', 'x11', 'vulkan', 'tsan', ['tsan', 'assert']),
    ('linux', 'x11', 'vulkan', 'gc', ['assert']),
    ('linux', 'wayland', 'vulkan', 'asan', ['asan', 'ubsan', 'assert']),
    ('linux', 'wayland', 'vulkan', 'tsan', ['tsan', 'assert']),
    ('linux', 'wayland', 'vulkan', 'gc', ['assert']),
    ('win64', '', 'vulkan', 'gc', ['ubsan', 'assert']),
    ('linux', 'x11', 'gl', 'asan', ['asan', 'ubsan', 'assert', 'gl']),
    ('linux', 'x11', 'gl', 'tsan', ['tsan', 'assert', 'gl']),
    ('linux', 'x11', 'gl', 'gc', ['assert', 'gl']),
    ('linux', 'wayland', 'gl', 'asan', ['asan', 'ubsan', 'assert', 'gl']),
    ('linux', 'wayland', 'gl', 'tsan', ['tsan', 'assert', 'gl']),
    ('linux', 'wayland', 'gl', 'gc', ['assert', 'gl']),
]


def run(target, backend, renderer, variant, test):
    tree = os.path.join(ROOT, 'build', target, variant)
    exe = os.path.join(tree, 'fenestal-testbed')
    cmd = [exe]
    if target.startswith('win64'):
        exe += '.exe'
        cmd = ['wine', exe]
    if not os.path.exists(exe):
        return None
    env = dict(os.environ, FNL_SELFTEST=test,
               TSAN_OPTIONS='suppressions=%s' %
               os.path.join(ROOT, 'tsan.supp'))
    env.pop('FNL_BACKEND', None)
    if backend:
        env['FNL_BACKEND'] = backend
    env['FNL_RENDERER'] = renderer
    # The trap is the point, and without this wine answers it by putting its
    # crash debugger's window on the desktop. Disabled, wine reports the
    # exception on stderr and the program ends.
    env['WINEDLLOVERRIDES'] = 'winedbg.exe=d'
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env)
    try:
        _, err = proc.communicate(timeout=6)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            _, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            _, err = proc.communicate()
    log = err.decode('utf-8', 'replace')
    want = EXPECT[test]
    if target.startswith('win64') and test == 'ubsan':
        want = TRAP
    return re.search(want, log, re.I) is not None


def wine_settle():
    """Wait for the wine server to go, as environment.py's wine_clear() does:
    by its process, since the distribution keeps `wineserver` off the PATH,
    and waited for rather than killed."""
    end = time.time() + 20.0
    while time.time() < end:
        up = subprocess.run(['pgrep', '-u', str(os.getuid()), '-x',
                             'wineserver'], capture_output=True).stdout
        if not up.strip():
            return
        time.sleep(0.5)


def main():
    failed = 0
    for target, backend, renderer, variant, tests in TREES:
        for test in tests:
            seen = run(target, backend, renderer, variant, test)
            what = '%s/%s %s%s %s' % (target, variant,
                                      backend + ' ' if backend else '',
                                      renderer, test)
            if seen is None:
                print('%-40s skipped: not built' % what)
            elif seen:
                print('%-40s reported' % what)
            else:
                print('%-40s NOT REPORTED' % what)
                failed += 1
        if target.startswith('win64'):
            wine_settle()
    print('==== %d checker%s silent ====' % (failed,
                                             '' if failed == 1 else 's'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
