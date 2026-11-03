#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The tables that have to agree and that no compiler compares, read as text:
#
#   - every FnlKey has a row in evdev_keys.h (Wayland, X11, KMS), in win32.c's
#     SC_PLAIN or SC_EXTENDED, in cocoa.m's MAC_KEYS, and a name in echo.c's
#     KEY_NAMES -- or a key is unknown on one backend, or "unknown" to the
#     harness;
#   - every FnlEventType and FnlButton is named in echo.c, and every FnlCursor
#     is mapped by each backend;
#   - every FnlDrawState field is compared in fnl_draw_state_eq(), or a frame
#     never re-records when that field alone changes. The size assert beside
#     it catches a field that grows the struct and not one that fills
#     padding; this catches both, by name.
#
#   python3 tools/check-tables.py
#
# build.sh runs it after every build that succeeds, beside the layering
# check. Text and not a host tool because win32.c's and cocoa.m's tables
# never compile for the machine that builds the others, and a cross build
# runs nothing.

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as fh:
        return fh.read()


def uncomment(text):
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def enum(header, name):
    """The members of `typedef enum { ... } name;`, less a *_COUNT."""
    end = header.index('} %s;' % name)
    start = header.rindex('typedef enum {', 0, end)
    body = uncomment(header[start:end])
    return [m for m in re.findall(r'\bFNL_[A-Z0-9_]+\b', body)
            if not m.endswith('_COUNT')]


def block(path, start):
    """The FNL_ names inside the initialiser that follows `start`."""
    text = uncomment(read(path))
    i = text.index(start)
    return set(re.findall(r'\bFNL_[A-Z0-9_]+\b', text[i:text.index('};', i)]))


def names(path):
    return set(re.findall(r'\bFNL_[A-Z0-9_]+\b', uncomment(read(path))))


def draw_state_fields(app_h):
    end = app_h.index('} FnlDrawState;')
    start = app_h.rindex('typedef struct {', 0, end)
    fields = []
    body = uncomment(app_h[start + len('typedef struct {'):end])
    for decl in body.split(';'):
        decl = re.sub(r'\[[^]]*\]', '', decl).strip()
        if decl:
            # `int a, b` declares two: the last word of each comma part.
            fields += [part.split()[-1].lstrip('*')
                       for part in decl.split(',')]
    return fields


def main():
    header = read('include/fenestal/fenestal.h')
    problems = []

    keys = [k for k in enum(header, 'FnlKey') if k != 'FNL_KEY_UNKNOWN']
    win32 = (block('src/backend/win32/win32.c', 'SC_PLAIN[]') |
             block('src/backend/win32/win32.c', 'SC_EXTENDED[]'))
    for where, have in (
            ('src/backend/evdev_keys.h EVDEV_KEYS',
             block('src/backend/evdev_keys.h', 'EVDEV_KEYS[')),
            ('src/backend/win32/win32.c SC_PLAIN/SC_EXTENDED', win32),
            ('src/backend/cocoa/cocoa.m MAC_KEYS',
             block('src/backend/cocoa/cocoa.m', 'MAC_KEYS[')),
            ('src/core/echo.c KEY_NAMES',
             block('src/core/echo.c', 'KEY_NAMES[]'))):
        problems += ['%s has no row for %s' % (where, k)
                     for k in keys if k not in have]

    echo = names('src/core/echo.c')
    for kind in ('FnlEventType', 'FnlButton'):
        problems += ['src/core/echo.c does not name %s %s' % (kind, v)
                     for v in enum(header, kind) if v not in echo]
    for backend in ('src/backend/wayland/wayland.c',
                    'src/backend/x11/x11.c',
                    'src/backend/win32/win32.c',
                    'src/backend/kms/kms.c',
                    'src/backend/cocoa/cocoa.m'):
        have = names(backend)
        problems += ['%s does not map FnlCursor %s' % (backend, v)
                     for v in enum(header, 'FnlCursor') if v not in have]

    app_h = read('src/core/app.h')
    i = app_h.index('static inline bool fnl_draw_state_eq(')
    eq = app_h[i:app_h.index('\n}', i)]
    for field in draw_state_fields(app_h):
        if not re.search(r'x->%s\b' % field, eq) or \
           not re.search(r'y->%s\b' % field, eq):
            problems.append('fnl_draw_state_eq() does not compare '
                            'FnlDrawState::%s' % field)

    for p in problems:
        print('check-tables: ' + p, file=sys.stderr)
    if problems:
        return 1
    print('tables agree: %d keys on five backends, every event, button, '
          'cursor and draw-state field accounted for.' % len(keys))
    return 0


if __name__ == '__main__':
    sys.exit(main())
