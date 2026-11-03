#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Bake X11's keysym names into src/backend/keysym_table.h.
#
#   python3 tools/mkkeysyms.py [/usr/include/X11/keysymdef.h]
#
# Run by hand, and its output is tracked: made during the build, a header
# from x11proto would be a build dependency of the *Wayland* backend. The
# table changes when X.Org adds a keysym, a few times a decade -- and a
# name the header lacks reads as nothing, so generate from the newest
# header to hand (last: xorgproto 2026.1's, from an Arch machine, which
# names SSHARP and rightsingleanglequotemark where Debian's 2025.1 writes
# U1E9E and U203A).
#
# keysymdef.h holds the whole correspondence in its comments, one "U+xxxx" per
# keysym that has one; src/backend/keymap.h says what the table is for.
#
# Three comment shapes carry a code point there, and all three are taken:
#
#     /* U+0041 ... */     the keysym is that character
#     /*<U+0030 ...>*/     a more specific keysym for it -- the keypad's digits
#     /*(U+222B ...)*/     a legacy keysym whose mapping is approximate
#
# The third is deprecated upstream and kept here anyway: a keymap that still
# names one is asking for that character, and the alternative is no character.
#
# Control characters are dropped. BackSpace, Tab and Return are annotated with
# U+0008, U+0009 and U+000D, and none of them is text: they are keys, and
# FNL_EVENT_KEY is where they arrive.

import os
import re
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'src', 'backend', 'keysym_table.h')
NOTICE = os.path.join(ROOT, 'src', 'backend', 'keysym_notice.txt')

DEFINE = re.compile(
    r'^#define\s+XK_(\w+)\s+0x([0-9a-fA-F]+)\s*'
    r'/\*\s*[<(]?\s*U\+([0-9A-Fa-f]{4,6})\b')

# The ranges src/backend/keymap.c tests by number: the function block, which
# prints nothing outside the keypad, and the keypad inside it.
FUNCTION = (0xfd00, 0xffff)
KEYPAD = (0xff80, 0xffbd)

# Where the name table keeps a Unicode keysym: its code point plus the first,
# in sixteen bits, short of the function block.
UNICODE_FLAG = 0x8000
UNICODE_END = FUNCTION[0]


def is_keypad(sym):
    return KEYPAD[0] <= sym <= KEYPAD[1]


# ---- dead keys --------------------------------------------------------------
#
# What a dead key and a letter make is not in keysymdef.h -- X keeps it in a
# Compose file per locale -- but it is exactly Unicode's canonical
# composition, which Python carries. So each dead key is named with its
# combining mark, and every letter that mark composes with becomes a row.
# The Compose key's multi-key sequences have no such derivation and are not
# here.
#
# The third column is what the dead key types on its own -- pressed twice, or
# followed by a space -- where a spacing form of the accent exists.
DEAD = (
    ('dead_grave',              0x0300, 0x0060),
    ('dead_acute',              0x0301, 0x00b4),
    ('dead_circumflex',         0x0302, 0x005e),
    ('dead_tilde',              0x0303, 0x007e),
    ('dead_macron',             0x0304, 0x00af),
    ('dead_breve',              0x0306, 0x02d8),
    ('dead_abovedot',           0x0307, 0x02d9),
    ('dead_diaeresis',          0x0308, 0x00a8),
    ('dead_abovering',          0x030a, 0x02da),
    ('dead_doubleacute',        0x030b, 0x02dd),
    ('dead_caron',              0x030c, 0x02c7),
    ('dead_cedilla',            0x0327, 0x00b8),
    ('dead_ogonek',             0x0328, 0x02db),
    ('dead_iota',               0x0345, 0),
    ('dead_belowdot',           0x0323, 0),
    ('dead_hook',               0x0309, 0),
    ('dead_horn',               0x031b, 0),
    ('dead_abovecomma',         0x0313, 0),
    ('dead_abovereversedcomma', 0x0314, 0),
    ('dead_doublegrave',        0x030f, 0),
    ('dead_belowring',          0x0325, 0),
    ('dead_belowmacron',        0x0331, 0),
    ('dead_belowcircumflex',    0x032d, 0),
    ('dead_belowtilde',         0x0330, 0),
    ('dead_belowbreve',         0x032e, 0),
    ('dead_belowdiaeresis',     0x0324, 0),
    ('dead_invertedbreve',      0x0311, 0),
    ('dead_belowcomma',         0x0326, 0),
)

ANY_DEFINE = re.compile(r'^#define\s+XK_(\w+)\s+0x([0-9a-fA-F]+)')


def compose_rows(text):
    """(dead keysym, base, composed), and (dead keysym, spacing form)."""
    value = {}
    for line in text.splitlines():
        m = ANY_DEFINE.match(line)
        if m:
            value.setdefault(m.group(1), int(m.group(2), 16))

    rows, spacing = [], []
    for name, mark, alone in DEAD:
        if name not in value:
            raise SystemExit('mkkeysyms: keysymdef.h has no %s' % name)
        sym = value[name]
        if alone:
            spacing.append((sym, alone))
        # Letters only, and only the planes a keyboard layout reaches.
        for cp in range(0x20, 0x2000):
            if not unicodedata.category(chr(cp)).startswith('L'):
                continue
            got = unicodedata.normalize('NFC', chr(cp) + chr(mark))
            if len(got) == 1 and ord(got) != cp:
                rows.append((sym, cp, ord(got)))
    return sorted(rows), sorted(spacing)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else '/usr/include/X11/keysymdef.h'
    with open(src, encoding='utf-8') as f:
        text = f.read()

    # The header's own first comment, verbatim, which asks to appear in all
    # copies. In a .txt because tools/mkdist.py strips comments from sources.
    end = text.index('*/') + 2
    notice = text[text.index('/*'):end]

    rows = {}
    for line in text.splitlines():
        m = DEFINE.match(line)
        if not m:
            continue
        name, sym, ucs = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if ucs < 0x20 or 0x7f <= ucs < 0xa0:
            continue
        # First definition wins, which is upstream's own rule for which of
        # two spellings is canonical.
        rows.setdefault(name, (sym, ucs))

    # Wanted by name though they print nothing: a Wayland keymap spells the
    # dead keys, the keypad's other face, BackSpace and Escape, and a name
    # that reads as no keysym is a key the composer or Num Lock never sees.
    # ISO_Level3_Shift is how the KMS backend finds AltGr in the symbols
    # files (level3(ralt_switch)).
    for line in text.splitlines():
        m = ANY_DEFINE.match(line)
        if m and (m.group(1).startswith(('dead_', 'KP_'))
                  or m.group(1) in ('BackSpace', 'Escape',
                                    'ISO_Level3_Shift')):
            rows.setdefault(m.group(1), (int(m.group(2), 16), 0))

    # And every other spelling of a keysym that has a row: the names
    # xorgproto deprecated in 2023 -- masculine for ordmasculine,
    # guillemotleft for guillemetleft -- are the ones xkeyboard-config's
    # files still write, and an alias read as no keysym is a level that
    # types nothing. Their code point is the canonical row's; a name whose
    # keysym has no row prints nothing and is kept only where it fits.
    canonical = {sym: ucs for sym, ucs in rows.values()}
    for line in text.splitlines():
        m = ANY_DEFINE.match(line)
        if not m or m.group(1) in rows:
            continue
        name, sym = m.group(1), int(m.group(2), 16)
        if 0x20 <= sym <= 0x7e or 0xa0 <= sym <= 0xff:
            ucs = sym
        elif sym >= 0x01000000:
            ucs = sym - 0x01000000
        else:
            ucs = canonical.get(sym, 0)
        if sym < 0x20 or 0x7f <= sym < 0xa0 or sym > 0xffff and \
                sym < 0x01000000:
            continue
        if sym >= 0x01000000 and ucs >= UNICODE_END - UNICODE_FLAG:
            continue
        rows[name] = (sym, ucs)

    # keysym_to_ucs() answers three ranges without a table -- Latin-1 and
    # the Unicode keysyms as themselves, the function block outside the
    # keypad as nothing -- so the value table leaves them out, and each is
    # checked against keysymdef.h here. Every field below is sixteen bits.
    by_value = {}
    for name in sorted(rows, key=lambda n: n.encode('ascii')):
        sym, ucs = rows[name]
        if 0x20 <= sym <= 0x7e or 0xa0 <= sym <= 0xff:
            if ucs != sym:
                raise SystemExit(f'mkkeysyms: {name} is in Latin-1 and is '
                                 f'not its own code point')
        elif sym >= 0x01000000:
            if ucs != sym - 0x01000000:
                raise SystemExit(f'mkkeysyms: {name} is a Unicode keysym '
                                 f'for some other code point')
            if ucs >= UNICODE_END - UNICODE_FLAG:
                raise SystemExit(f'mkkeysyms: {name} does not fit the name '
                                 f"table's Unicode rows")
        elif FUNCTION[0] <= sym <= FUNCTION[1] and not is_keypad(sym):
            if ucs:
                raise SystemExit(f'mkkeysyms: {name} prints from the '
                                 f'function block; keysym_to_ucs() skips '
                                 f'that range')
        else:
            if sym > 0xffff or ucs > 0xffff:
                raise SystemExit(f'mkkeysyms: {name} outgrew sixteen bits')
            if by_value.setdefault(sym, (ucs, name))[0] != ucs:
                raise SystemExit(f'mkkeysyms: two names for 0x{sym:04x} '
                                 f'disagree about what it prints')
        if sym < 0x01000000 and UNICODE_FLAG <= sym < UNICODE_END:
            raise SystemExit(f'mkkeysyms: {name} has a value the name table '
                             f'keeps for Unicode keysyms')

    out = []
    out.append('/*')
    out.append(' * SPDX-License-Identifier: MIT')
    out.append(' * SPDX-FileComment: The keysyms, their names and values are '
               'X.Org\'s, from')
    out.append(' * keysymdef.h, and the MIT grant in LICENSE does not cover '
               'them; see')
    out.append(' * keysym_notice.txt and THIRD_PARTY_NOTICES.md.')
    out.append(' *')
    out.append(' * Copyright (c) 2026 Janos Sonntag')
    out.append(' *')
    out.append(' * Keysyms: the code points they stand for, their names, and '
               'what the')
    out.append(' * dead keys compose.')
    out.append(' *')
    out.append(' * Generated by tools/mkkeysyms.py from X11\'s keysymdef.h -- '
               'do not edit;')
    out.append(' * run the tool again. Its header says why this one generated '
               'file is')
    out.append(' * tracked.')
    out.append(' *')
    out.append(' * Two halves, and a translation unit gets one: the names '
               'where')
    out.append(' * KEYSYM_TABLE_NAMES is defined, which is the XKB reader '
               'alone, and the')
    out.append(' * values everywhere else. An X11 build carries no names.')
    out.append(' *')
    out.append(' * The correspondence is keysymdef.h\'s, and travels under '
               'its notice:')
    out.append(' * keysym_notice.txt, beside this file.')
    out.append(' */')
    out.append('')
    out.append('#ifndef FNL_KEYSYM_TABLE_H')
    out.append('#define FNL_KEYSYM_TABLE_H')
    out.append('')
    out.append('#include <stdint.h>')
    out.append('')
    out.append('#ifdef KEYSYM_TABLE_NAMES')
    out.append('')
    # The names go in one pool and a row holds an offset into it: a row
    # holding a `const char *` is a relocation in the shared object per
    # keysym, and a page of table the loader must write to before main().
    order = sorted(rows, key=lambda n: n.encode('ascii'))
    out.append('/* Sorted by name, in strcmp order: keysym_from_name() '
               'bisects it. `sym` is')
    out.append(' * the keysym where that fits, and for a Unicode keysym -- '
               '0x01000000 plus')
    out.append(' * the code point -- the code point plus '
               'KEYSYM_NAME_UNICODE, a range no')
    out.append(' * other keysym has a value in. */')
    out.append(f'#define KEYSYM_NAME_UNICODE     0x{UNICODE_FLAG:04x}u')
    out.append(f'#define KEYSYM_NAME_UNICODE_END 0x{UNICODE_END:04x}u')
    out.append('')
    out.append('typedef struct {')
    out.append('\tuint16_t name;     /* offset into KEYSYM_NAMES */')
    out.append('\tuint16_t sym;')
    out.append('} KeysymName;')
    out.append('')
    out.append('static const char KEYSYM_NAMES[] =')
    offsets, at, line = {}, 0, ''
    for name in order:
        offsets[name] = at
        at += len(name) + 1
        # Three digits, which is as many as an octal escape takes: after
        # a bare "\\0" a name beginning with a digit would be read into it.
        piece = name + '\\000'
        if len(line) + len(piece) > 70:
            out.append(f'\t"{line}"')
            line = ''
        line += piece
    out.append(f'\t"{line}";')
    if at > 0xffff:
        raise SystemExit('mkkeysyms: the name pool outgrew sixteen bits')
    out.append('')
    out.append('static const KeysymName KEYSYM_BY_NAME[] = {')
    for name in order:
        sym = rows[name][0]
        if sym >= 0x01000000:
            sym = sym - 0x01000000 + UNICODE_FLAG
        out.append(f'\t{{ {offsets[name]:5d}, 0x{sym:04x} }}, /* {name} */')
    out.append('};')
    out.append('')
    out.append('#else /* the values */')
    out.append('')
    out.append('/* What a keysym prints, for the ones keysym_to_ucs() cannot '
               'answer from')
    out.append(' * the number alone. Sorted by keysym, which it bisects. */')
    out.append('typedef struct {')
    out.append('\tuint16_t sym;')
    out.append('\tuint16_t ucs;')
    out.append('} KeysymUcs;')
    out.append('')
    out.append('static const KeysymUcs KEYSYM_UCS[] = {')
    values = 0
    for sym in sorted(by_value):
        ucs, name = by_value[sym]
        if ucs:
            out.append(f'\t{{ 0x{sym:04x}, 0x{ucs:04x} }}, /* {name} */')
            values += 1
    out.append('};')
    out.append('')
    crows, spacing = compose_rows(text)
    out.append('/* A dead key and the letter after it, and what the two make. '
               'Unicode\'s')
    out.append(' * canonical compositions, by way of the tool; sorted by the '
               'first two')
    out.append(' * columns, which compose() bisects on. */')
    out.append('typedef struct {')
    out.append('\tuint16_t dead;')
    out.append('\tuint16_t base;')
    out.append('\tuint16_t made;')
    out.append('} ComposeRow;')
    out.append('')
    out.append('static const ComposeRow COMPOSE_TABLE[] = {')
    for sym, base, made in crows:
        if max(sym, base, made) > 0xffff:
            raise SystemExit('mkkeysyms: a compose row outgrew sixteen bits')
        out.append(f'\t{{ 0x{sym:04x}, 0x{base:04x}, 0x{made:04x} }},')
    out.append('};')
    out.append('')
    out.append('/* What a dead key types alone: pressed twice, or before a '
               'space. */')
    out.append('static const uint16_t DEAD_SPACING[][2] = {')
    for sym, alone in spacing:
        if max(sym, alone) > 0xffff:
            raise SystemExit('mkkeysyms: a spacing row outgrew sixteen bits')
        out.append(f'\t{{ 0x{sym:04x}, 0x{alone:04x} }},')
    out.append('};')
    out.append('')
    out.append('#endif /* KEYSYM_TABLE_NAMES */')
    out.append('')
    out.append('#endif /* FNL_KEYSYM_TABLE_H */')

    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('\n'.join(out) + '\n')
    with open(NOTICE, 'w', encoding='utf-8') as f:
        f.write('src/backend/keysym_table.h is derived from the X.Org '
                'Foundation\'s\nkeysymdef.h, which carries this notice:\n\n')
        f.write(notice + '\n')
    print(f'{os.path.relpath(OUT, ROOT)}: {len(rows)} names, '
          f'{values} values, {len(crows)} compositions')


if __name__ == '__main__':
    main()
