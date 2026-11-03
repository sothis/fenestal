#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# The API reference, made from the public headers (HEADERS) and nothing
# else, so that what it says is what the header says.
#
#   python3 tools/mkapidoc.py                 -> build/api/index.html
#   python3 tools/mkapidoc.py --quiet         -> the same, saying what is
#                                                wrong, or one line where
#                                                all is well
#   python3 tools/mkapidoc.py --check         -> the completeness check alone
#
# The header's own comments are the text: a section comment ("---- name ----")
# opens a section, a comment directly above a declaration documents it, and
# the [main] / [any] / [overlay] tag beside a prototype is its thread.
# examples/*.c are the examples, and *.m for Metal, which is
# Objective-C: each is a complete program built in every tree that has its
# API, so an example that stops compiling fails the build rather than the
# reader, and each names the functions it shows in its first comment
# ("Shows: fnl_poll_event fnl_set_text_input").
#
# --quiet is what build.sh runs after every build, and it fails on what
# --check fails on: every FNL_API function has a comment and a thread tag,
# and every one is called somewhere a build compiles -- an example, a
# snippet, or a tool that drives the library -- so none is documented and
# never exercised. A name in a comment is not a call. NOT_CALLED is the
# short list that may be, each with its reason.

import argparse
import functools
import html
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADERS = ('include/fenestal/fenestal.h', 'include/fenestal/fnl_vulkan.h',
           'include/fenestal/fnl_gl.h', 'include/fenestal/fnl_d3d12.h',
           'include/fenestal/fnl_metal.h')
# What a program is written in here: C, and Objective-C where Metal is.
SOURCES = ('.c', '.m')
SNIPPETS = 'examples'
# Where a public function counts as called: every program the build makes.
CALLERS = ('examples', 'testbed', 'demos', 'tools/drive/pace.c',
           'tools/drive/lifecycle.c', 'tools/headercheck.c')

# Functions no program calls, and why that is acceptable.
NOT_CALLED: dict[str, str] = {}

THREAD = re.compile(r'\[(main|any|overlay)\]|\bon_record\b')

# A file's licence lines: the SPDX tags and the copyright or dedication.
LICENCE_LINE = re.compile(r'SPDX-[\w-]+:|Copyright \(c\)|Written in \d{4} by')


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as fh:
        return fh.read()


def comments(text):
    """Every comment as (start, end, body), the body without its delimiters
    and leading stars."""
    out = []
    for m in re.finditer(r'/\*.*?\*/', text, re.S):
        lines = [re.sub(r'^\s*\* ?', '', ln)
                 for ln in m.group(0)[2:-2].split('\n')]
        out.append((m.start(), m.end(), '\n'.join(lines).strip()))
    return out


def declarations(blank):
    """The prototypes and typedefs, in order, as (start, end, name, kind),
    found in the header with its comments blanked out. Only at the start of
    a line, so a #define naming FNL_API is not one; braces are counted, not
    matched by a pattern, because FnlEvent's union holds structs."""
    out = []
    for m in re.finditer(r'^(FNL_API\b|typedef\b)', blank, re.M):
        start = m.start()
        if blank.startswith('FNL_API', start):
            end = blank.index(';', start) + 1
            name = re.search(r'(fnl_\w+)\s*\(', blank[start:end]).group(1)
            out.append((start, end, name, 'function'))
            continue
        brace, semi = blank.find('{', start), blank.find(';', start)
        if brace != -1 and brace < semi:
            depth, j = 0, brace
            while True:
                depth += {'{': 1, '}': -1}.get(blank[j], 0)
                if depth == 0:
                    break
                j += 1
            end = blank.index(';', j) + 1
            name = re.search(r'\}\s*(\w+)\s*;$', blank[start:end]).group(1)
        else:
            end = semi + 1
            fp = re.search(r'\(\s*\*\s*(\w+)\s*\)', blank[start:end])
            name = fp.group(1) if fp else \
                re.search(r'(\w+)\s*;$', blank[start:end]).group(1)
        out.append((start, end, name, 'type'))
    return out


SECTION = re.compile(r'\s*-{2,}\s*(.+?)\s*-{2,}\s*\n?(.*)', re.S)


def parse(path):
    """Sections, each a title, its prose and its items. An item is a
    declaration with the comment directly above it, any comment on its last
    line, and for a function its thread."""
    text = read(path)
    cms = comments(text)
    blank = list(text)
    for a, b, _ in cms:
        for k in range(a, b):
            if blank[k] != '\n':
                blank[k] = ' '
    blank = ''.join(blank)

    events = [(a, 'comment', (a, b, body)) for a, b, body in cms] + \
             [(d[0], 'decl', d) for d in declarations(blank)]
    events.sort(key=lambda e: e[0])

    sections = [{'title': os.path.basename(path), 'prose': '', 'items': []}]
    last_end = 0
    for _, kind, e in events:
        if kind == 'comment':
            a, b, body = e
            m = SECTION.match(body)
            if m:
                sections.append({'title': m.group(1), 'prose': m.group(2),
                                 'items': []})
            continue
        start, end, name, k = e
        doc = ''
        for a, b, body in cms:
            # Directly above: nothing but whitespace between, and not a
            # section heading, whose prose is the section's.
            if b <= start and b >= last_end and \
                    not text[b:start].strip() and not SECTION.match(body):
                doc = body
        line_end = text.find('\n', end)
        tail = ' '.join(body for a, b, body in cms
                        if end <= a < (line_end if line_end != -1
                                       else len(text)))
        # As written, comments and all: a struct's field notes are its
        # documentation as much as the comment above it.
        sig = text[start:end].strip()
        sections[-1]['items'].append({'name': name, 'kind': k, 'sig': sig,
                                      'doc': doc, 'tail': tail,
                                      'file': path})
        last_end = end
    for sec in sections:
        last_doc = ''
        for it in sec['items']:
            if it['kind'] != 'function':
                continue
            if it['doc']:
                last_doc = it['doc']
            else:
                it['shared'] = True
            m = THREAD.search(it['tail']) or THREAD.search(it['doc'] or
                                                           last_doc)
            it['thread'] = m.group(0).strip('[]') if m else ''
    return [s for s in sections if s['items'] or s['prose'].strip()]


def snippets():
    out = []
    d = os.path.join(ROOT, SNIPPETS)
    if not os.path.isdir(d):
        return out
    for f in sorted(os.listdir(d)):
        if not f.endswith(SOURCES):
            continue
        src = read(os.path.join(SNIPPETS, f))
        # The list wraps over lines of its own, so it runs to a blank
        # comment line or the comment's end, not to the first newline.
        m = re.search(r'Shows:(.*?)(?:\n[ \t]*\*?[ \t]*\n|\*/)', src, re.S)
        # The title is the first comment's first line after the licence
        # lines every file opens with.
        cms = comments(src)
        lines = [ln.strip() for ln in (cms[0][2] if cms else '').split('\n')
                 if ln.strip() and not LICENCE_LINE.match(ln.strip())]
        out.append({'file': f, 'src': src,
                    'shows': re.findall(r'\bfnl_\w+', m.group(1)) if m
                    else [],
                    'title': lines[0] if lines else f})
    return out


# A string or character literal is matched so that a "/*" or "//" inside
# one is not taken for a comment; only the comments are blanked.
CODE_SKIP = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
                       r'|/\*.*?\*/|//[^\n]*', re.S)


@functools.cache
def code_of(path):
    """A source's text with its comments blanked."""
    with open(path, encoding='utf-8') as fh:
        text = fh.read()
    return CODE_SKIP.sub(
        lambda m: m.group(0) if m.group(0)[0] in '"\'' else ' ', text)


def called_somewhere(name):
    pat = re.compile(r'\b%s\s*\(' % re.escape(name))
    for c in CALLERS:
        p = os.path.join(ROOT, c)
        paths = [p] if os.path.isfile(p) else [
            os.path.join(dp, f) for dp, _, fs in os.walk(p) for f in fs
            if f.endswith(SOURCES)]
        for q in paths:
            if pat.search(code_of(q)):
                return True
    return False


def check(sections):
    problems = []
    for sec in sections:
        for it in sec['items']:
            if it['kind'] != 'function':
                continue
            n = it['name']
            if not it['thread']:
                problems.append('%s has no thread tag' % n)
            if not (it['doc'] or it.get('shared') or it.get('tail')):
                problems.append('%s has no comment' % n)
            if n not in NOT_CALLED and not called_somewhere(n):
                problems.append('%s is called by no example, snippet or '
                                'tool' % n)
    return problems


# ---- rendering ---------------------------------------------------------------

def prose(text, names):
    """The header's comment text as HTML: blank lines are paragraphs, an
    indented run is preformatted, `code` and *emphasis* and **strong** are
    marked, and an fnl_ name that is documented here is a link."""
    out = []
    for block in re.split(r'\n\s*\n', text.strip('\n')):
        lines = block.split('\n')
        if all(ln.startswith('  ') or not ln.strip() for ln in lines):
            out.append('<pre>%s</pre>' % html.escape(
                '\n'.join(ln[2:] if ln.startswith('  ') else ln
                          for ln in lines)))
            continue
        h = html.escape(' '.join(ln.strip() for ln in lines))
        h = re.sub(r'`([^`]+)`', r'<code>\1</code>', h)
        h = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', h)
        h = re.sub(r'(?<![\w*])\*([^*\s][^*]*?)\*(?![\w*])', r'<em>\1</em>',
                   h)
        h = re.sub(r'\b(fnl_\w+|Fnl\w+)\b',
                   lambda m: '<a href="#%s">%s</a>' % (m.group(1),
                                                       m.group(1))
                   if m.group(1) in names else m.group(1), h)
        out.append('<p>%s</p>' % h)
    return '\n'.join(out)


CSS = """
:root { --bg:#fbfaf7; --fg:#1d1d1b; --mute:#6b6862; --rule:#e2ded6;
        --code:#f1eee7; --link:#2a5d8f; --tag:#e8eef4; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#16171a; --fg:#e7e5e0; --mute:#9a978f; --rule:#2c2e33;
          --code:#202227; --link:#8fb8e0; --tag:#22303d; } }
body { background:var(--bg); color:var(--fg); margin:0;
       font:16px/1.55 system-ui, sans-serif; }
nav { position:fixed; top:0; bottom:0; width:16rem; overflow:auto;
      padding:1.5rem 1rem; border-right:1px solid var(--rule);
      font-size:14px; }
nav a { display:block; color:var(--fg); text-decoration:none; padding:1px 0; }
nav .sec { margin-top:.9rem; font-weight:600; }
nav .fn { padding-left:.8rem; color:var(--mute); font-family:ui-monospace,
          monospace; font-size:13px; }
main { margin-left:18rem; max-width:52rem; padding:1.5rem 2rem 6rem; }
h1 { font-size:1.6rem; } h2 { margin-top:3rem; border-bottom:1px solid
     var(--rule); padding-bottom:.3rem; }
.item { margin:1.8rem 0; } .item h3 { font:600 15px ui-monospace,monospace;
     margin:0 0 .4rem; }
pre, code { font-family:ui-monospace, monospace; font-size:13.5px;
            background:var(--code); border-radius:4px; }
pre { padding:.7rem .9rem; overflow:auto; tab-size:4; }
code { padding:0 .25rem; }
a { color:var(--link); }
.tag { font:12px ui-monospace,monospace; background:var(--tag);
       border-radius:3px; padding:0 .35rem; margin-left:.5rem; }
.mute { color:var(--mute); font-size:14px; }
@media (max-width: 800px) { nav { display:none; }
  main { margin-left:0; padding:1rem; } }
"""


def render(all_sections, snips, version):
    names = {it['name'] for secs in all_sections for s in secs
             for it in s['items']}
    nav, body = [], []
    body.append('<h1>Fenestal %s API reference</h1>' % version)
    body.append('<p class="mute">Generated from <code>fenestal.h</code>, '
                '<code>fnl_vulkan.h</code>, <code>fnl_gl.h</code>, '
                '<code>fnl_d3d12.h</code> and <code>fnl_metal.h</code> by '
                '<code>tools/mkapidoc.py</code>;'
                ' the headers are the authority. Every example below is a '
                'program the build compiles.</p>')
    for secs in all_sections:
        for i, sec in enumerate(secs):
            sid = 'sec-%s' % re.sub(r'\W+', '-', sec['title']).strip('-')
            nav.append('<a class="sec" href="#%s">%s</a>' % (
                sid, html.escape(sec['title'])))
            body.append('<h2 id="%s">%s</h2>' % (sid,
                                                 html.escape(sec['title'])))
            if sec['prose'].strip():
                body.append(prose(sec['prose'], names))
            for it in sec['items']:
                nav.append('<a class="fn" href="#%s">%s</a>' % (it['name'],
                                                                it['name']))
                tag = ('<span class="tag">%s</span>' % it['thread']
                       if it.get('thread') else '')
                body.append('<div class="item" id="%s"><h3>%s%s</h3>' % (
                    it['name'], it['name'], tag))
                body.append('<pre>%s</pre>' % html.escape(it['sig']))
                if it['doc']:
                    body.append(prose(it['doc'], names))
                elif it.get('shared'):
                    body.append('<p class="mute">Described by the text '
                                'above it.</p>')
                if it.get('tail'):
                    body.append('<p class="mute">%s</p>' %
                                html.escape(it['tail']))
                for sn in snips:
                    if it['name'] in sn['shows']:
                        body.append('<p class="mute">Example: <a href='
                                    '"#ex-%s">%s</a></p>' % (sn['file'],
                                                             sn['file']))
                body.append('</div>')
    if snips:
        nav.append('<a class="sec" href="#examples">Examples</a>')
        body.append('<h2 id="examples">Examples</h2>')
        for sn in snips:
            nav.append('<a class="fn" href="#ex-%s">%s</a>' % (sn['file'],
                                                               sn['file']))
            body.append('<div class="item" id="ex-%s"><h3>%s</h3>'
                        '<p>%s</p><pre>%s</pre></div>' % (
                            sn['file'], sn['file'],
                            html.escape(sn['title']), html.escape(sn['src'])))
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,'
            'initial-scale=1"><title>Fenestal API</title><style>%s</style>'
            '</head><body><nav>%s</nav><main>%s</main></body></html>\n' % (
                CSS, '\n'.join(nav), '\n'.join(body)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--quiet', action='store_true',
                    help='generate, and say what is wrong, or in one '
                         'line where all is well')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'api'))
    args = ap.parse_args()

    all_sections = [parse(h) for h in HEADERS]
    problems = check([s for secs in all_sections for s in secs])
    for p in problems:
        print('mkapidoc: ' + p, file=sys.stderr)
    if args.check:
        if problems:
            return 1
        n = sum(1 for secs in all_sections for s in secs
                for it in s['items'] if it['kind'] == 'function')
        print('api documented: %d functions, each with a comment, a thread '
              'and a caller.' % n)
        return 0

    h = read(HEADERS[0])
    version = '.'.join(re.search(r'FNL_VERSION_%s (\d+)' % k, h).group(1)
                       for k in ('MAJOR', 'MINOR', 'PATCH'))
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, 'index.html')
    with open(path, 'w', encoding='utf-8') as fh:
        fh.write(render(all_sections, snippets(), version))
    if not args.quiet:
        print(path)
    elif not problems:
        print('api reference: %s' % os.path.relpath(path, ROOT))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
