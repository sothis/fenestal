#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# fenestal.map against the headers, and against the shared object it made.
#
#   python3 tools/check-version-map.py                   # the map and headers
#   python3 tools/check-version-map.py --so libfenestal.so.1.0.0
#
# The map's `global:` list must name every FNL_API function of the public
# headers exactly once, in a node of the release that added it, and nothing
# else. Before 1.0 the map is the one node and may hold the wildcard `fnl_*`,
# which the major-0 rules allow (every minor may break the one before); from
# 1.0 a wildcard is refused, since it would put every later addition in the
# oldest node and a program built against 1.1 could then load under 1.0.
#
# With --so, every `fnl_*` symbol the object exports must carry the node the
# map puts it in, which is what proves the linker applied the script (ELF
# only: Windows exports by the .def and macOS by the export list, and both
# rely on fnl_init()'s header check for the rest, DESIGN *Compatibility*).
# Run by the build after the shared object is made, and by build.sh.

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = ('fenestal.h', 'fnl_vulkan.h', 'fnl_metal.h', 'fnl_d3d12.h')


def read(path):
    try:
        with open(os.path.join(ROOT, path), encoding='utf-8') as fh:
            return fh.read()
    except OSError as err:
        sys.exit('check-version-map: cannot read %s: %s' % (path,
                                                            err.strerror))


def uncomment(text):
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def api_names():
    """Every FNL_API function of the public headers, as CMake reads them."""
    names = []
    for h in HEADERS:
        text = '\n' + uncomment(read('include/fenestal/' + h))
        for proto in re.findall(r'\nFNL_API[^;(]*\(', text):
            m = re.search(r'(fnl_[a-z0-9_]+)\s*\($', proto)
            if m:
                names.append(m.group(1))
    return names


def major():
    m = re.search(r'#define FNL_VERSION_MAJOR (\d+)',
                  read('include/fenestal/fenestal.h'))
    if not m:
        sys.exit('check-version-map: fenestal.h has no FNL_VERSION_MAJOR')
    return int(m.group(1))


def parse_map():
    """({node: [patterns]}, in file order), `local:` ignored."""
    text = uncomment(read('fenestal.map'))
    nodes = {}
    for m in re.finditer(r'(\w+)\s*\{(.*?)\}\s*(?:\w+\s*)?;', text, re.S):
        body = m.group(2)
        g = re.search(r'global:(.*?)(?:local:|$)', body, re.S)
        nodes[m.group(1)] = re.findall(r'[\w*]+', g.group(1)) if g else []
    return nodes


def check_map(problems):
    nodes = parse_map()
    names = api_names()
    owner = {}
    wildcard = None
    for node, pats in nodes.items():
        for p in pats:
            if '*' in p:
                wildcard = node
                if major() >= 1:
                    problems.append('node %s has the wildcard %s; from 1.0 '
                                    'every function is named' % (node, p))
            elif p in owner:
                problems.append('%s is in both %s and %s' % (p, owner[p],
                                                             node))
            else:
                owner[p] = node
    if wildcard is None:
        problems += ['fenestal.map does not export %s' % n
                     for n in names if n not in owner]
        problems += ['fenestal.map exports %s, which no header declares' % p
                     for p in owner if p not in names]
    return nodes, owner, wildcard


def check_so(path, nodes, owner, wildcard, problems):
    out = subprocess.run(['readelf', '--dyn-syms', '--wide', path],
                         capture_output=True, text=True, check=True).stdout
    seen = 0
    for line in out.splitlines():
        f = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(f) < 8 or f[6] == 'UND' or not f[7].startswith('fnl_'):
            continue
        m = re.match(r'(fnl_\w+)@@?(\w+)$', f[7])
        name, node = (m.group(1), m.group(2)) if m else (f[7], None)
        if name not in owner and wildcard is None:
            problems.append('%s is exported and not in the map' % name)
            continue
        want = owner.get(name, wildcard)
        if node != want:
            problems.append('%s carries %s, the map says %s' %
                            (name, node or 'no version', want))
        seen += 1
    if not seen:
        problems.append('%s exports no fnl_* symbol' % path)
    return seen


def main():
    so = None
    if len(sys.argv) == 3 and sys.argv[1] == '--so':
        so = sys.argv[2]
    elif len(sys.argv) != 1:
        sys.exit(__doc__)
    problems = []
    nodes, owner, wildcard = check_map(problems)
    seen = 0
    if so and not problems:
        seen = check_so(so, nodes, owner, wildcard, problems)
    for p in problems:
        print('check-version-map: ' + p, file=sys.stderr)
    if problems:
        return 1
    if so:
        print('version map agrees with %s: %d exports versioned.' % (so, seen))
    else:
        print('version map agrees with the headers: %d nodes, %s.' %
              (len(nodes), 'wildcard before 1.0' if wildcard
               else '%d functions named' % len(owner)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
