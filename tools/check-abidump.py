#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The public headers against tools/abidump.c, read as text: every struct's
# size and every field, every enum constant and every numeric FNL_ macro the
# headers declare must be named by an S(), F() or E() line there, and every
# such line must name something the headers declare. abidump's lists are
# written out by hand, and a field placed in what was tail padding changes no
# sizeof, so a field it misses is a field outside the ABI baseline and a green
# build.
#
#   python3 tools/check-abidump.py
#
# build.sh runs it after every build that succeeds, beside check-tables.py.
# A text check and not a host tool for the reason that one gives: it must see
# the Direct3D 12 and Vulkan headers on a machine that builds neither.

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = ('fenestal.h', 'fnl_vulkan.h', 'fnl_metal.h', 'fnl_d3d12.h')

# Macros that move with every release, and so are not in a baseline.
UNPINNED = re.compile(r'^FNL_VERSION')


def read(path):
    try:
        with open(os.path.join(ROOT, path), encoding='utf-8') as fh:
            return fh.read()
    except OSError as err:
        sys.exit('check-abidump: cannot read %s: %s' % (path, err.strerror))


def uncomment(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def split_top(body, sep):
    """Split on `sep` outside parentheses and braces."""
    parts, depth, cur = [], 0, ''
    for ch in body:
        if ch in '({':
            depth += 1
        elif ch in ')}':
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


def declarators(decl):
    """[(type, name)] of one `type a, *b[3], (*fn)(...)` declaration."""
    decl = decl.strip()
    if not decl:
        return []
    m = re.match(r'(.*?\(\s*\*\s*)(\w+)(\s*\)\s*\(.*)$', decl, re.S)
    if m:
        return [('fn', m.group(2))]
    parts = split_top(decl, ',')
    first = parts[0].strip()
    # The type is everything before the first declarator's identifier.
    m = re.match(r'(.*?[\s*])(\w+)\s*(\[[^\]]*\])*$', first, re.S)
    if not m:
        return []
    ty = re.sub(r'\b(const|struct|unsigned)\b', '', m.group(1)).replace(
        '*', '').strip()
    out = [(ty, m.group(2))]
    for part in parts[1:]:
        n = re.match(r'[\s*]*(\w+)', part)
        if n:
            out.append((ty, n.group(1)))
    return out


def parse(text):
    """({struct: [(type, field)]}, {enum member: True}, {macro: True},
    {struct: [(member type, member)]} for FnlEvent's union)."""
    text = uncomment(text)
    structs, enums, macros = {}, [], []
    for m in re.finditer(r'typedef\s+(struct|enum)\s*(\w*)\s*\{', text):
        kind, i = m.group(1), m.end()
        depth, j = 1, i
        while depth:
            depth += {'{': 1, '}': -1}.get(text[j], 0)
            j += 1
        body = text[i:j - 1]
        tail = re.match(r'\s*(\w+)\s*;', text[j:])
        if not tail:
            continue  # `typedef struct { ... } *p;` and the like: no name
        name = tail.group(1)
        if kind == 'enum':
            for item in split_top(body, ','):
                n = re.match(r'\s*(FNL_\w+)', item)
                if n:
                    enums.append(n.group(1))
            continue
        # An anonymous union is flattened: its members are the struct's.
        body = re.sub(r'\bunion\s*\{([^{}]*)\}\s*;', r'\1;', body)
        fields = []
        for decl in split_top(body, ';'):
            fields += declarators(decl)
        structs[name] = fields
    for m in re.finditer(r'^#\s*define\s+(FNL_\w+)\s+(\S.*)$', text, re.M):
        if not UNPINNED.match(m.group(1)) and \
           re.match(r'[0-9(]', m.group(2).strip()):
            macros.append(m.group(1))
    return structs, enums, macros


def expand(name, structs, deep):
    """The dotted field names a struct is dumped under."""
    out = []
    for ty, f in structs[name]:
        if deep and ty in structs:
            out += ['%s.%s' % (f, sub) for sub in expand(ty, structs, deep)]
        else:
            out.append(f)
    return out


def main():
    structs, enums, macros = {}, [], []
    for h in HEADERS:
        s, e, m = parse(read('include/fenestal/' + h))
        structs.update(s)
        enums += e
        macros += m

    # FnlEvent's union members are dumped through to their fields, recursively
    # (color.color.space); every other struct's fields are dumped by name.
    want_s, want_f = set(), set()
    for name in structs:
        want_s.add(name)
        if name == 'FnlEvent':
            for ty, f in structs[name]:
                if ty in structs:
                    want_f |= {('FnlEvent', '%s.%s' % (f, sub))
                               for sub in expand(ty, structs, True)}
                else:
                    want_f.add(('FnlEvent', f))
        else:
            want_f |= {(name, f) for _, f in structs[name]}
    want_e = set(enums) | set(macros)

    dump = read('tools/abidump.c')
    have_s = set(re.findall(r'^\s*S\((\w+)\);', dump, re.M))
    have_f = set(re.findall(r'^\s*F\((\w+),\s*([\w.]+)\);', dump, re.M))
    have_e = set(re.findall(r'^\s*E\((\w+)\);', dump, re.M))

    problems = []
    for what, want, have in (('struct', want_s, have_s),
                             ('field', want_f, have_f),
                             ('constant', want_e, have_e)):
        problems += ['tools/abidump.c does not dump the %s %s' % (what, x)
                     for x in sorted(want - have)]
        problems += ['tools/abidump.c dumps the %s %s, which the headers do '
                     'not declare' % (what, x) for x in sorted(have - want)]

    for p in problems:
        print('check-abidump: %s' % (p,), file=sys.stderr)
    if problems:
        return 1
    print('abidump agrees: %d structs, %d fields, %d constants named.' %
          (len(want_s), len(want_f), len(want_e)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
