#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# Pack the sources this tree needs in order to build, and nothing else.
#
#   python3 tools/mkdist.py              -> build/fenestal.zip
#   python3 tools/mkdist.py --dry-run    -> the manifest, and what it dropped
#   python3 tools/mkdist.py --worktree   -> the working tree as it stands
#
# build.sh runs it twice after a successful build: once as above, and once
# with --keep-comments into build/fenestal-commented.zip, the same files
# with every comment in place.
#
# Paths in the archive are the paths in the tree, with no wrapping directory,
# so `unzip -d fenestal` is the habit that keeps it out of the current one.
#
# What goes in is an allowlist (NEEDED), not a filter: a new top-level file or
# dot file is out by default, where a denylist would ship it unnoticed. The
# exclusions the archive promises are then checked against the selected set
# in verify(). Candidates are `git ls-files`, so nothing gitignored is one:
# the index's files by default, and with --worktree every file on disk that
# git does not ignore, tracked or not (`./build.sh dist`).
#
# Comments are stripped, by a scanner per language and never by a regex: this
# tree writes comment delimiters inside string literals (tools/mkfont.c prints
# C comments; an assert message in src/core/app.h holds both /* and */), and
# `#` is a comment in only some of the places the five languages here use it.
# Every stripper's output is then checked -- only_deletions(), idempotence,
# and the language's own parser where there is one -- and a file that fails
# goes in verbatim: a comment left in is cosmetic, a mangled source is not.
# The one comment kept is a file's licence lines, put back on top of what the
# scanner leaves (relicence()).
#
# Entries are sorted, stamped with the zip epoch and given the git index's
# mode -- with --worktree, 644 or 755 by the file's own execute bit, which is
# all the index records of one -- so two archives of one tree are
# byte-identical.

import argparse
import fnmatch
import os
import re
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Everything a build reads, what checks and drives the result, and the notes
# on both. Directory entries end in '/' and take the whole subtree; an entry
# with a * is a glob over the top level alone; everything else is one exact
# path. An entry that matches nothing stops the pack.
NEEDED = (
    'LICENSE',               # the terms everything else here is under
    'CMakeLists.txt',        # the build
    'build.sh',              # the documented entry point to it
    'run_all.sh',            # the release testbed four times at once
    'cmake/',                # the cross toolchain files
    'fenestal.map',          # --version-script, named by CMakeLists
    'tsan.supp',             # build.sh's tsan trees are unreadable without it
    'tsan-lavapipe.supp',    # bare_driver.py's, beside tsan.supp there
    'lsan.supp',             # the asan rounds', for NVIDIA's own driver
    'valgrind.supp',
    'ruff.toml',             # what `ruff check tools` holds tools/ to
    'include/',              # the public headers
    'src/',                  # the library
    'examples/',             # the API reference's snippets, all built
    'testbed/',              # the demo program the rounds drive
    'demos/',                # the programs to copy, each its own build
    'shaders/',              # glslangValidator inputs, library half
    'protocols/',            # wayland-scanner inputs
    'fonts/',                # see below
    'tools/',                # host tools, fuzzers, ABI baseline, harness
    'docs/',
    '*.md',                  # the spec, the changelog and the notes
)

# fonts/ carries COPYRIGHT.txt and LICENCE.txt beside the two .ttf; they are
# build inputs as well as notices, since CMakeLists .incbin's both.

# Comments that are the content and not remarks on it: the public headers'
# are the API reference (tools/mkapidoc.py renders them, and a consumer reads
# them), and each snippet names what it shows in its first comment.
KEEP_COMMENTS = ('include/', 'examples/')

# Objective-C (.m) is C's for comments and literals; its @"..." is a C
# string behind an @ the scanner passes over as any other character.
C_SUFFIXES = ('.c', '.h', '.m', '.vert', '.frag', '.comp', '.geom',
              '.glsl')
PY_SUFFIXES = ('.py',)
CMAKE_SUFFIXES = ('.cmake',)
CMAKE_NAMES = frozenset(('CMakeLists.txt',))
SH_SUFFIXES = ('.sh',)

# Line-oriented # comments and no quoting anywhere in the format.
HASH_SUFFIXES = ('.supp',)

# A stripped file keeps its licence lines (relicence()): its SPDX tags and
# the copyright or dedication under them, which MIT has travel with every
# copy. They open the file, within its first LICENCE_SPAN lines.
LICENCE_SPAN = 12
LICENCE_END = re.compile(r'Copyright \(c\)|Written in \d{4} by')

# 1980-01-01 00:00:00 is the earliest a zip can express, and the conventional
# stamp for a reproducible archive.
EPOCH = (1980, 1, 1, 0, 0, 0)


class StripError(Exception):
    """The scanner reached a state the file cannot legitimately be in."""


class LineBuffer:
    """Output as physical lines, so a scanner can never join two of them.

    Joining is not cosmetic: a comment that ends a `#define` line, folded
    into the next line, puts that line inside the macro. Every branch of
    every stripper writes through add(), which splits on newlines itself."""

    def __init__(self):
        self.lines = []
        self.cur = []

    def add(self, text):
        first, *rest = text.split('\n')
        self.cur.append(first)
        for part in rest:
            self.lines.append(''.join(self.cur))
            self.cur = [part]

    def finish(self):
        self.lines.append(''.join(self.cur))
        return self.lines


def tidy(lines):
    """Trailing whitespace out, runs of blank lines down to one: a
    comment-only line becomes an empty one, and a stripped file is mostly
    those."""
    out = []
    for line in lines:
        line = line.rstrip()
        if line:
            out.append(line)
        elif out and out[-1] != '':
            out.append('')
    while out and out[-1] == '':
        out.pop()
    return '\n'.join(out) + '\n' if out else ''


def only_deletions(original, stripped):
    """Is `stripped` `original` with characters removed and nothing else?

    A subsequence test with whitespace ignored, which is exactly strong
    enough: every way a comment scanner goes wrong ends with source text
    moved, doubled or invented, and none of those is a subsequence."""
    it = iter(re.sub(r'\s+', '', original))
    return all(ch in it for ch in re.sub(r'\s+', '', stripped))


# ---- C, and the GLSL that shares its comment syntax ------------------------

def c_literal_end(src, i):
    """Index one past the literal opening at src[i], or None.

    None means the quote does not close on this logical line, so it is not
    a literal: `#error don't` is the case, and reading that apostrophe as one
    would swallow the rest of the file. A backslash skips two characters, so
    an escaped quote and a continued literal both survive."""
    quote = src[i]
    j, n = i + 1, len(src)
    while j < n:
        c = src[j]
        if c == '\\':
            j += 2
        elif c == '\n':
            return None
        elif c == quote:
            return j + 1
        else:
            j += 1
    return None


def line_comment_end(src, i):
    """Index of the newline ending a // comment.

    A backslash at the end of the line continues the comment onto the next
    one (C99 6.4.9): line splicing happens first."""
    n = len(src)
    while True:
        nl = src.find('\n', i)
        if nl < 0:
            return n
        k = nl - 1
        while k >= i and src[k] in ' \t':
            k -= 1
        if k < i or src[k] != '\\':
            return nl
        i = nl + 1


def strip_c(src):
    """C and GLSL comments out, with literals as states of their own.

    Each comment becomes one space and not nothing: a/**/b is two tokens
    (C11 5.1.1.2p3). A comment spanning n lines leaves n line breaks."""
    out = LineBuffer()
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in '"\'':
            end = c_literal_end(src, i)
            if end is None:
                out.add(c)
                i += 1
            else:
                out.add(src[i:end])
                i = end
        elif src.startswith('/*', i):
            end = src.find('*/', i + 2)
            if end < 0:
                raise StripError('unterminated /* comment')
            end += 2
            out.add(' ' + '\n' * src.count('\n', i, end))
            i = end
        elif src.startswith('//', i):
            end = line_comment_end(src, i)
            out.add(' ' + '\n' * src.count('\n', i, end))
            i = end
        else:
            out.add(c)
            i += 1
    return tidy(out.finish())


# ---- Python ---------------------------------------------------------------

IDENT = re.compile(r'[A-Za-z_]\w*')
CODING = re.compile(r'coding[:=]\s*[-\w.]+')
STR_PREFIXES = frozenset(('r', 'u', 'b', 'f', 'rb', 'br', 'fr', 'rf'))


def py_string_end(src, i):
    """Index one past the string opening at src[i].

    Triple quotes first, so that '''x''' is not read as an empty '' followed
    by code. A backslash skips two characters in raw strings as well: raw
    changes what the escape *means*, never where the token ends."""
    quote = src[i:i + 3] if src[i:i + 3] in ('"""', "'''") else src[i]
    j, n = i + len(quote), len(src)
    while j < n:
        if src[j] == '\\':
            j += 2
        elif src.startswith(quote, j):
            return j + len(quote)
        elif src[j] == '\n' and len(quote) == 1:
            raise StripError('unterminated string literal')
        else:
            j += 1
    raise StripError('unterminated string literal')


def py_keep_comment(src, i, comment):
    """Is this # line a directive rather than prose?

    Two are: the shebang, and an encoding cookie in the first two lines."""
    if i == 0 and comment.startswith('#!'):
        return True
    return bool(CODING.search(comment)) and src.count('\n', 0, i) <= 1


def strip_py(src):
    out = LineBuffer()
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '#':
            end = src.find('\n', i)
            end = n if end < 0 else end
            if py_keep_comment(src, i, src[i:end]):
                out.add(src[i:end])
            i = end
        elif c in '"\'':
            end = py_string_end(src, i)
            out.add(src[i:end])
            i = end
        elif IDENT.match(src, i):
            # Identifiers are consumed whole so that a string prefix is
            # recognised as one and the tail of a name is not: the r ending
            # `for` must not turn the next quote into a raw string. A
            # non-ASCII name falls to the branch below, a character at a
            # time, which is right for everything but a prefix.
            end = IDENT.match(src, i).end()
            word = src[i:end]
            if word.lower() in STR_PREFIXES and src[end:end + 1] in ('"', "'"):
                end = py_string_end(src, end)
            out.add(src[i:end])
            i = end
        else:
            out.add(c)
            i += 1
    return tidy(out.finish())


# ---- CMake ----------------------------------------------------------------

BRACKET = re.compile(r'\[(=*)\[')


def cmake_quoted_end(src, i):
    """Index one past the quoted argument at src[i].

    Quoted arguments span lines here: CMakeLists writes whole assembler
    files with file(WRITE ... "..."), \\" escapes included."""
    j, n = i + 1, len(src)
    while j < n:
        if src[j] == '\\':
            j += 2
        elif src[j] == '"':
            return j + 1
        else:
            j += 1
    raise StripError('unterminated quoted argument')


def bracket_end(src, i, eq):
    close = ']' + '=' * eq + ']'
    end = src.find(close, i)
    if end < 0:
        raise StripError('unterminated bracket ' + close)
    return end + len(close)


def strip_cmake(src):
    """# comments and #[[ ]] bracket comments out; arguments left alone.

    A backslash escapes in an unquoted argument, so \\# is a literal #; a
    quoted argument runs across lines; and #[[ opens a bracket comment that
    ends at ]], with any number of = between the brackets.

    A bracket *argument* -- the same syntax without the # -- is data, and is
    recognised only at an argument boundary, which is CMake's own rule:
    `if(x MATCHES a[[:alpha:]])` has none in it."""
    out = LineBuffer()
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '\\':
            out.add(src[i:i + 2])
            i += 2
        elif c == '"':
            end = cmake_quoted_end(src, i)
            out.add(src[i:end])
            i = end
        elif c == '#':
            m = BRACKET.match(src, i + 1)
            end = (bracket_end(src, m.end(), len(m.group(1))) if m
                   else src.find('\n', i))
            end = n if end < 0 else end
            out.add(' ' + '\n' * src.count('\n', i, end))
            i = end
        elif c == '[' and (i == 0 or src[i - 1] in ' \t\r\n(') \
                and BRACKET.match(src, i):
            m = BRACKET.match(src, i)
            end = bracket_end(src, m.end(), len(m.group(1)))
            out.add(src[i:end])
            i = end
        else:
            out.add(c)
            i += 1
    return tidy(out.finish())


def check_cmake(text, path):
    """Do the parentheses still balance outside quotes and brackets?

    There is no cmake -n, so this re-scans the output for the one property
    every valid CMakeLists has. A scanner that eats a closing quote swallows
    the code up to the next one, and that nearly always unbalances them."""
    depth = 0
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '\\':
            i += 2
        elif c == '"':
            i = cmake_quoted_end(text, i)
        elif c == '[' and (i == 0 or text[i - 1] in ' \t\r\n(') \
                and BRACKET.match(text, i):
            m = BRACKET.match(text, i)
            i = bracket_end(text, m.end(), len(m.group(1)))
        else:
            depth += (c == '(') - (c == ')')
            if depth < 0:
                raise StripError('unbalanced ) after stripping')
            i += 1
    if depth:
        raise StripError(f'{depth} unclosed ( after stripping')


# ---- the shell ------------------------------------------------------------

# A # opens a comment only where a word can start: build.sh has ${0##*/} and
# `[ $# -eq 0 ]`, and cutting at the first # leaves a script that still
# parses and runs.
SH_WORD_BREAK = frozenset(' \t\r\n;|&()<>')


def sh_dquote_end(src, i):
    j, n = i + 1, len(src)
    while j < n:
        if src[j] == '\\':
            j += 2
        elif src[j] == '"':
            return j + 1
        else:
            j += 1
    raise StripError('unterminated "')


def strip_sh(src):
    """# comments out of /bin/sh, with the quoting the word rule needs.

    Comments are read before quotes, so the apostrophes and backticks in
    this tree's prose never open a quote.

    Heredocs are refused: a # in one is data, and no script here has one.
    Refusing gives the file back with its comments; guessing could give it
    back with a line of its data deleted."""
    out = LineBuffer()
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '\\':
            out.add(src[i:i + 2])
            i += 2
        elif c == '#' and (i == 0 or src[i - 1] in SH_WORD_BREAK):
            end = src.find('\n', i)
            end = n if end < 0 else end
            # The shebang is the file's first line and not a remark about it.
            out.add(src[i:end] if i == 0 and src.startswith('#!') else ' ')
            i = end
        elif src.startswith('<<', i):
            raise StripError('heredoc or <<, not handled')
        elif c == "'":
            end = src.find("'", i + 1)
            if end < 0:
                raise StripError("unterminated '")
            out.add(src[i:end + 1])
            i = end + 1
        elif c == '"':
            end = sh_dquote_end(src, i)
            out.add(src[i:end])
            i = end
        elif c == '`':
            end = src.find('`', i + 1)
            if end < 0:
                raise StripError('unterminated `')
            out.add(src[i:end + 1])
            i = end + 1
        else:
            out.add(c)
            i += 1
    return tidy(out.finish())


def check_sh(text, path):
    """`sh -n`, the shell's own parser -- the shell the shebang names, since
    `sh -n` rejects good bash. No such shell installed means no check ran,
    and a file that could not be checked keeps its comments."""
    first = text.split('\n', 1)[0]
    shell = 'bash' if 'bash' in first else 'sh'
    try:
        p = subprocess.run([shell, '-n'], input=text.encode('utf-8'),
                           stderr=subprocess.PIPE)
    except OSError as e:
        raise StripError(f'no {shell} to check with: {e}')
    if p.returncode:
        raise StripError(f'{shell} -n: '
                         f'{p.stderr.decode("utf-8", "replace").strip()}')


def check_py(text, path):
    compile(text, path, 'exec')


def strip_hash(src):
    """# to end of line, for a format with no strings to protect: a TSan
    suppression is `type:pattern`, globbed, with no way to quote one."""
    out = LineBuffer()
    for line in src.split('\n'):
        cut = line.find('#')
        out.add((line if cut < 0 else line[:cut]) + '\n')
    return tidy(out.finish())


def stripper_for(path):
    """The scanner this file gets and the check that has to pass, or None.
    Asked by strip() and by the byte counter in main()."""
    if path.endswith(C_SUFFIXES):
        return strip_c, None
    if path.endswith(PY_SUFFIXES):
        return strip_py, check_py
    if path.endswith(CMAKE_SUFFIXES) or os.path.basename(path) in CMAKE_NAMES:
        return strip_cmake, check_cmake
    if path.endswith(SH_SUFFIXES):
        return strip_sh, check_sh
    if path.endswith(HASH_SUFFIXES):
        return strip_hash, None
    return None, None


def licence_lines(text):
    """The opening comment's licence lines, markers off: from the SPDX tag
    to the copyright or dedication, a FileComment's continuation included."""
    out, inside = [], False
    for line in text.split('\n')[:LICENCE_SPAN]:
        body = re.sub(r'^\s*(?:/\*|\*/?|#(?!!))\s?', '', line).rstrip()
        body = re.sub(r'\s*\*/$', '', body)
        if body.startswith('SPDX-License-Identifier:'):
            inside = True
        if inside and body:
            out.append(body)
        if inside and LICENCE_END.match(body):
            return out
    return []


def relicence(original, stripped, c_syntax):
    """`stripped` with the original's licence lines put back on top, after
    a shebang: the one remark every copy has to keep."""
    lines = licence_lines(original)
    if not lines:
        return stripped
    if c_syntax:
        block = '/*\n' + ''.join(f' * {ln}\n' for ln in lines) + ' */\n'
    else:
        block = ''.join(f'# {ln}\n' for ln in lines)
    if stripped.startswith('#!'):
        first, _, rest = stripped.partition('\n')
        return f'{first}\n{block}{rest}'
    return block + stripped


def strip(path, data):
    """Comments out of one file, or the file back unchanged: every failure
    has that one answer."""
    stripper, check = stripper_for(path)
    if stripper is None or path.startswith(KEEP_COMMENTS):
        return data

    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        print(f'mkdist: {path}: not UTF-8, kept as is', file=sys.stderr)
        return data

    try:
        out = stripper(text)
        if not only_deletions(text, out):
            raise StripError('output is not the input minus comments')
        # Catches a scanner left in the wrong state: a swallowed quote reads
        # as code on the first pass and as a string on the second.
        if stripper(out) != out:
            raise StripError('stripping is not idempotent')
        out = relicence(text, out, stripper is strip_c)
        if check:
            check(out, path)
    except (StripError, SyntaxError, ValueError) as e:
        print(f'mkdist: {path}: {e}; kept with comments', file=sys.stderr)
        return data
    return out.encode('utf-8')


# ---- what goes in ---------------------------------------------------------

def wanted(path, manifest):
    return any(path == m or (m.endswith('/') and path.startswith(m))
               or ('*' in m and '/' not in path and fnmatch.fnmatch(path, m))
               for m in manifest)


def verify(paths):
    """The archive's promises, checked against the selected set: NEEDED is
    written by hand, and hand-written lists drift."""
    for path in sorted(paths):
        parts = path.split('/')
        if any(p.startswith('.') for p in parts):
            raise SystemExit(f'mkdist: {path}: hidden path in the manifest')
        if parts[0] == 'build':
            raise SystemExit(f'mkdist: {path}: build tree in the manifest')


def index_files():
    """The tracked files and the mode git has for them.

    `git ls-files -s` and not the filesystem: it is the set that is not
    gitignored, and its mode bits are a fresh clone's, so build.sh comes out
    executable whatever this working tree's umask says."""
    out = subprocess.run(['git', 'ls-files', '-sz'], cwd=ROOT, check=True,
                         stdout=subprocess.PIPE).stdout.decode('utf-8')
    tracked = {}
    for entry in out.split('\0'):
        if entry:
            meta, path = entry.split('\t', 1)
            mode = int(meta.split()[0], 8)
            # A submodule is a gitlink, a commit of another repository and
            # not a file, and nothing of this tree's.
            if mode == 0o160000:
                continue
            tracked[path] = mode
    return tracked


def worktree_files():
    """Every file on disk that git does not ignore, tracked or not, with
    the mode the index would give it.

    A tracked file deleted on disk is not one; a submodule is a directory
    and not a file, and `ls-files -o` never descends into it, so one
    stays out as it does from the index. An untracked hidden file -- an
    editor's swap file -- is left out with a line rather than failing
    verify(), which is there for the hand-written manifest."""
    out = subprocess.run(['git', 'ls-files', '-coz', '--exclude-standard'],
                         cwd=ROOT, check=True,
                         stdout=subprocess.PIPE).stdout.decode('utf-8')
    tracked = set(index_files())
    files = {}
    for path in sorted(set(p for p in out.split('\0') if p)):
        full = os.path.join(ROOT, path)
        if not os.path.isfile(full):
            continue
        if path not in tracked and any(part.startswith('.')
                                       for part in path.split('/')):
            print(f'mkdist: {path}: hidden and untracked, left out',
                  file=sys.stderr)
            continue
        files[path] = (0o100755 if os.stat(full).st_mode & 0o111
                       else 0o100644)
    return files, tracked


def collect(manifest, worktree=False):
    """The files the manifest names, and the mode each is packed with."""
    if worktree:
        candidates, tracked = worktree_files()
    else:
        candidates = index_files()

    chosen = {p: m for p, m in candidates.items() if wanted(p, manifest)}

    # A stale entry is an archive short of something the tree promises.
    for m in manifest:
        if not any(wanted(p, (m,)) for p in candidates):
            raise SystemExit(f'mkdist: {m}: in the manifest, not in the tree')

    if worktree:
        for p in sorted(set(chosen) - tracked):
            print(f'mkdist: {p}: not tracked, packed as it is on disk',
                  file=sys.stderr)
    else:
        # The index is not the working tree until a commit makes it so,
        # and the build this follows read the working tree. Both
        # differences are said out loud and neither fails the pack.
        for p in sorted(chosen):
            if not os.path.exists(os.path.join(ROOT, p)):
                print(f'mkdist: {p}: tracked but not on disk, left out',
                      file=sys.stderr)
                del chosen[p]
        out = subprocess.run(['git', 'ls-files', '-oz', '--exclude-standard'],
                             cwd=ROOT, check=True,
                             stdout=subprocess.PIPE).stdout.decode('utf-8')
        untracked = sorted(o for o in out.split('\0')
                           if o and wanted(o, manifest))
        for p in untracked:
            print(f'mkdist: {p}: not tracked yet, so not packed',
                  file=sys.stderr)
        if untracked:
            print('mkdist: --worktree (./build.sh dist) packs the working '
                  'tree as it stands', file=sys.stderr)

    verify(chosen)
    return chosen, sorted(set(candidates) - set(chosen))


def main():
    ap = argparse.ArgumentParser(
        description='Zip the sources needed to build, comments stripped.')
    ap.add_argument('-o', '--output',
                    help='archive path (default build/fenestal.zip)')
    ap.add_argument('--keep-comments', action='store_true',
                    help='pack the sources as they are')
    ap.add_argument('--dry-run', action='store_true',
                    help='print what would be packed and what was dropped')
    ap.add_argument('--worktree', action='store_true',
                    help='pack the working tree as it stands: untracked '
                         'files in, deleted ones out, modes from disk')
    args = ap.parse_args()

    chosen, dropped = collect(NEEDED, args.worktree)
    out_path = args.output or os.path.join(ROOT, 'build', 'fenestal.zip')

    if args.dry_run:
        for path in sorted(chosen):
            print(f'  {path}')
        what = 'candidate' if args.worktree else 'tracked'
        print(f'\n{len(chosen)} files in, {len(dropped)} {what} files out:')
        for path in dropped:
            print(f'  - {path}')
        print(f'\nwould write {out_path}')
        return 0

    os.makedirs(os.path.dirname(out_path) or '.', exist_ok=True)

    # Counted over the files the stripper touches: most of the archive by
    # weight is two .ttf files, which would dilute the figure.
    raw = packed = 0
    with zipfile.ZipFile(out_path, 'w', zipfile.ZIP_DEFLATED,
                         compresslevel=9) as zf:
        for path in sorted(chosen):
            with open(os.path.join(ROOT, path), 'rb') as fh:
                data = fh.read()
            counted = stripper_for(path)[0] is not None
            if counted:
                raw += len(data)
            if not args.keep_comments:
                data = strip(path, data)
            if counted:
                packed += len(data)
            info = zipfile.ZipInfo(path, EPOCH)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (chosen[path] | 0o100000) << 16
            zf.writestr(info, data)

    size = os.path.getsize(out_path)
    print(f'{out_path}: {len(chosen)} files, {size / 1024:.1f} KiB')
    if not args.keep_comments and raw:
        print(f'text {raw / 1024:.1f} KiB -> {packed / 1024:.1f} KiB '
              f'({100 * (raw - packed) / raw:.0f}% comment and blank line)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
