/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * XKB's text keymaps -- as a Wayland compositor sends one, every include
 * resolved, and as xkeyboard-config keeps them, in files that include one
 * another -- and the keysym names they are written in. Apart from keymap.c
 * so that the names are linked only where a keymap arrives as text.
 */

#include "core/app.h"

#include "backend/keymap.h"

#define KEYSYM_TABLE_NAMES
#include "backend/keysym_table.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEYSYM_NAME_ROWS (sizeof KEYSYM_BY_NAME / sizeof KEYSYM_BY_NAME[0])

/* ---- keysym names -------------------------------------------------------- */

static int name_cmp(const char *s, size_t n, const char *name)
{
	/* strncmp stops at the row's terminator, so it never reads past `n`
	 * bytes of a token that has none of its own. */
	const int c = strncmp(s, name, n);
	if (c)
		return c;
	return name[n] ? -1 : 0;
}

static bool hex_value(const char *s, size_t n, uint32_t *out)
{
	uint32_t v = 0;
	if (n == 0 || n > 8)
		return false;
	for (size_t i = 0; i < n; i++) {
		const char c = s[i];
		uint32_t d;
		if (c >= '0' && c <= '9')
			d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (uint32_t)(c - 'a') + 10u;
		else if (c >= 'A' && c <= 'F')
			d = (uint32_t)(c - 'A') + 10u;
		else
			return false;
		v = (v << 4) | d;
	}
	*out = v;
	return true;
}

uint32_t keysym_from_name(const char *name, size_t len)
{
	uint32_t v;

	if (len == 0)
		return 0;
	/* A keysym with no name is written as its number. */
	if (len > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
		return hex_value(name + 2, len - 2, &v) ? v : 0;

	size_t lo = 0, hi = KEYSYM_NAME_ROWS;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const int c = name_cmp(name, len,
							   KEYSYM_NAMES + KEYSYM_BY_NAME[mid].name);
		if (c == 0) {
			const uint32_t row = KEYSYM_BY_NAME[mid].sym;
			if (row >= KEYSYM_NAME_UNICODE && row < KEYSYM_NAME_UNICODE_END)
				return KEYSYM_UNICODE | (row - KEYSYM_NAME_UNICODE);
			return row;
		}
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}

	/* "U20AC", "U458" or xkbcomp's "U0001F12F": a code point with no name
	 * of its own, in as many digits as the writer gave it, which is
	 * libxkbcommon's reading. After the table and not before it: a name is
	 * a name first, and Udiaeresis begins with a U as well. */
	if (name[0] == 'U' && len >= 2 && len <= 9 &&
		hex_value(name + 1, len - 1, &v) && v >= 0x20 && v <= 0x10ffff)
		return v < 0x100 ? v : (KEYSYM_UNICODE | v);

	/* The two spellings of a level filled with nothing, which is not the
	 * same as a level left out: `none` over a base's symbol covers it. So
	 * does a media or vendor key -- XF86AudioMute, XF86ClearGrab -- whose
	 * names are another header's (XF86keysym.h) and type nothing anyway. */
	if ((len == 4 && !memcmp(name, "none", 4)) ||
		(len == 10 && !memcmp(name, "VoidSymbol", 10)) ||
		(len > 4 && !memcmp(name, "XF86", 4)))
		return KEYSYM_VOID;

	/* NoSymbol, any, and every name the table has no row for. */
	return 0;
}

/* ---- XKB's text format ----------------------------------------------------
 *
 * Four kinds of section, of which two matter here:
 *
 *     xkb_keycodes "..." { <AD01> = 24; alias <LatQ> = <AD01>; ... };
 *     xkb_symbols  "..." { key <AD01> { [ q, Q ] };
 *                          key <AE02> { type= "FOUR_LEVEL",
 *                                       symbols[Group1]= [ 2, at, onehalf ],
 *                                       symbols[Group2]= [ ... ] }; ... };
 *
 * Everything else in a key's braces is skipped by counting brackets.
 *
 * A compositor sends one file, every include already resolved.
 * xkeyboard-config keeps the same syntax in many: a section says
 * `include "latin(type4)"` for another file's section, whose keys land
 * first so that the including section's own land over them -- `override`,
 * what `+` between the pieces of a chain means and what every chain the
 * rules write uses. `|` is `augment`, which fills only what is empty.
 * A key's NoSymbol leaves the level it was given alone under either, which
 * is how `eurosign(e)` adds a third level and keeps the two below it. `:2`
 * on a piece puts its keys in the second group. Of a key's type only its
 * level count is read, and only to cut the levels a shorter type drops;
 * actions and virtual modifiers are skipped as they are in a compositor's
 * text, and the four levels are taken as plain, Shift, AltGr and
 * AltGr+Shift.
 *
 * The text is another process's, or root's files read as if they were
 * not: nothing below reads past `end`, no token is assumed terminated, a
 * file that is not what it claims to be produces an empty map rather than
 * a wrong one, and what one parse may read is bounded in files, bytes and
 * include depth, so a chain that names itself ends.
 */

typedef enum { TOK_END, TOK_IDENT, TOK_KEYNAME, TOK_STRING, TOK_PUNCT } TokKind;

typedef struct {
	const char *p, *end;
	TokKind     kind;
	const char *s;          /* IDENT, KEYNAME and STRING: not terminated */
	size_t      n;
	char        c;          /* PUNCT */
} Lex;

static bool ident_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		   (c >= '0' && c <= '9') || c == '_';
}

static void lex_next(Lex *l)
{
	for (;;) {
		while (l->p < l->end && (*l->p == ' ' || *l->p == '\t' ||
								 *l->p == '\n' || *l->p == '\r'))
			l->p++;
		if (l->p >= l->end || *l->p == '\0') {
			l->kind = TOK_END;
			return;
		}
		const bool slashes = *l->p == '/' && l->end - l->p > 1 &&
							 l->p[1] == '/';
		if (*l->p != '#' && !slashes)
			break;
		while (l->p < l->end && *l->p != '\n')
			l->p++;
	}

	const char c = *l->p;
	if (ident_char(c)) {
		l->kind = TOK_IDENT;
		l->s = l->p;
		while (l->p < l->end && ident_char(*l->p))
			l->p++;
		l->n = (size_t)(l->p - l->s);
		return;
	}
	if (c == '<') {
		const char *q = l->p + 1;
		while (q < l->end && *q != '>' && *q != '\n')
			q++;
		if (q < l->end && *q == '>') {
			l->kind = TOK_KEYNAME;
			l->s = l->p + 1;
			l->n = (size_t)(q - l->s);
			l->p = q + 1;
			return;
		}
	}
	if (c == '"') {
		const char *q = l->p + 1;
		while (q < l->end && *q != '"') {
			if (*q == '\\' && l->end - q > 1)
				q++;
			q++;
		}
		l->kind = TOK_STRING;
		l->s = l->p + 1;
		l->n = (size_t)(q - l->s);
		l->p = q < l->end ? q + 1 : l->end;
		return;
	}
	l->kind = TOK_PUNCT;
	l->c = c;
	l->p++;
}

static bool tok_is(const Lex *l, char c)
{
	return l->kind == TOK_PUNCT && l->c == c;
}

static bool tok_word(const Lex *l, const char *w)
{
	const size_t n = strlen(w);
	return l->kind == TOK_IDENT && l->n == n && !memcmp(l->s, w, n);
}

/* Run forward to `stop` outside any bracket, or to the brace that closes the
 * block this was called in, and stop *on* it so the caller sees which. A stray
 * closing bracket of another kind is stepped over. */
static void lex_skip(Lex *l, char stop)
{
	int depth = 0;
	while (l->kind != TOK_END) {
		if (l->kind == TOK_PUNCT) {
			const char c = l->c;
			if (depth == 0 && (c == stop || c == '}'))
				return;
			if (c == '{' || c == '[' || c == '(')
				depth++;
			else if ((c == '}' || c == ']' || c == ')') && depth > 0)
				depth--;
		}
		lex_next(l);
	}
}

/* The keys by name, for as long as the parse lasts. keycodes/evdev defines
 * some 500 and its aliases file 80 more. */
#define XKB_NAME_MAX  24
#define XKB_NAMES_MAX 768

typedef struct {
	char    name[XKB_NAME_MAX];
	uint8_t len;
	uint8_t code;
	bool    alias;
} XkbName;

typedef struct {
	XkbName *v;
	size_t   n;
} XkbNames;

static int names_find(const XkbNames *names, const char *s, size_t n)
{
	for (size_t i = 0; i < names->n; i++)
		if (names->v[i].len == n && !memcmp(names->v[i].name, s, n))
			return names->v[i].code;
	return -1;
}

/* A name given again takes its new code, as the later of two definitions
 * does in libxkbcommon, unless the second came in by augment, which only
 * fills what is empty: `evdev+moved` moves a key and `evdev|moved` does
 * not. An alias never moves a key's own name -- libxkbcommon drops such an
 * alias -- and a key's own name takes over one an alias had. */
static void names_add(XkbNames *names, const char *s, size_t n, unsigned code,
					  bool augment, bool alias)
{
	if (n == 0 || n >= XKB_NAME_MAX)
		return;
	for (size_t i = 0; i < names->n; i++) {
		XkbName *e = &names->v[i];
		if (e->len != n || memcmp(e->name, s, n))
			continue;
		if (!augment && (!alias || e->alias)) {
			e->code = (uint8_t)code;
			e->alias = alias;
		}
		return;
	}
	if (names->n >= XKB_NAMES_MAX)
		return;
	XkbName *e = &names->v[names->n++];
	memcpy(e->name, s, n);
	e->len = (uint8_t)n;
	e->code = (uint8_t)code;
	e->alias = alias;
}

static bool decimal_value(const char *s, size_t n, unsigned *out)
{
	unsigned v = 0;
	if (n == 0 || n > 5)
		return false;
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10u + (unsigned)(s[i] - '0');
	}
	*out = v;
	return true;
}

/* ---- the files an include names ------------------------------------------ */

/* One file, and everything one parse may read: a layout's chain touches a
 * dozen files of some 300 KB in all, the largest of them 120 KB. */
#define XKB_FILE_MAX      ((size_t)1 << 20)
#define XKB_BYTES_MAX     ((size_t)4 << 20)
/* libxkbcommon's own bound on nesting. A cycle is a depth that runs out --
 * and a section that includes itself twice would double at every level, so
 * the pieces one parse may take are counted as well; a layout's chain is
 * some fifteen. */
#define XKB_INCLUDE_DEPTH 15
#define XKB_PIECES_MAX    256

typedef enum { XKB_KEYCODES, XKB_SYMBOLS } XkbKind;
/* Which keys of the included section win: the including section's own,
 * written after it (OVERRIDE, `+`); the included ones (AUGMENT, `|`); or
 * the key as a whole, in place of what it had (REPLACE, a `replace key`). */
typedef enum { MERGE_OVERRIDE, MERGE_AUGMENT, MERGE_REPLACE } Merge;

static const char *const KIND_DIR[]  = { "keycodes", "symbols" };
static const char *const KIND_WORD[] = { "xkb_keycodes", "xkb_symbols" };

typedef struct {
	XkbSource *src;         /* NULL: a compositor's text */
	XkbNames  *names;
	Keymap    *km;
	int        depth;       /* includes open */
} Parse;

void xkb_source_lend(XkbSource *src, const char *path, const char *text,
					 size_t len)
{
	if (src->file_count >= XKB_SOURCE_FILES)
		return;
	XkbFile *f = &src->files[src->file_count];
	f->path = strdup(path);
	if (!f->path)
		return;
	f->text = (char *)text;
	f->len = len;
	f->lent = true;
	src->file_count++;
}

void xkb_source_free(XkbSource *src)
{
	for (size_t i = 0; i < src->file_count; i++) {
		free(src->files[i].path);
		if (!src->files[i].lent)
			free(src->files[i].text);
	}
	src->file_count = 0;
	src->bytes = 0;
	src->pieces = 0;
}

static void note_missing(Parse *p, XkbKind kind, const char *file,
						 size_t flen, const char *sect, size_t slen,
						 const char *why)
{
	char line[sizeof p->src->missing];
	snprintf(line, sizeof line, "%s/%.*s%s%.*s%s: %s", KIND_DIR[kind],
			 (int)flen, file, slen ? "(" : "", (int)slen, sect ? sect : "",
			 slen ? ")" : "", why);
	if (!p->src->missing[0])
		memcpy(p->src->missing, line, sizeof line);
	if (p->depth == 0 && !p->src->top_miss[0])
		memcpy(p->src->top_miss, line, sizeof line);
}

/* A name that stays under the root: no leading slash and no `..` step. The
 * files are root's, but the name may be the user's own (FNL_KMS_LAYOUT). */
static bool file_name_ok(const char *s, size_t n)
{
	if (n == 0 || n > 64 || s[0] == '/')
		return false;
	for (size_t i = 0; i < n; i++) {
		const char c = s[i];
		if (c == '.' && i + 1 < n && s[i + 1] == '.' &&
			(i == 0 || s[i - 1] == '/') && (i + 2 == n || s[i + 2] == '/'))
			return false;
		if (!ident_char(c) && c != '-' && c != '.' && c != '/')
			return false;
	}
	return true;
}

char *xkb_read_file(const char *path, size_t budget, size_t *len)
{
	/* Non-blocking: a FIFO by that name would hold the pump in open()
	 * until the S_ISREG test below could refuse it. */
	const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return NULL;
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
		(size_t)st.st_size > budget) {
		close(fd);
		return NULL;
	}
	const size_t size = (size_t)st.st_size;
	char *buf = malloc(size + 1);
	size_t got = 0;
	while (buf && got < size) {
		const ssize_t n = read(fd, buf + got, size - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	/* Short of the size fstat() gave is not the whole file. */
	if (!buf || got < size) {
		free(buf);
		return NULL;
	}
	buf[got] = '\0';
	*len = got;
	return buf;
}

/* The file `dir/name`, from the ones already held or read from the root,
 * or NULL. */
static const XkbFile *source_file(XkbSource *src, XkbKind kind,
								  const char *name, size_t n)
{
	char path[96];
	if (!file_name_ok(name, n))
		return NULL;
	snprintf(path, sizeof path, "%s/%.*s", KIND_DIR[kind], (int)n, name);
	for (size_t i = 0; i < src->file_count; i++)
		if (!strcmp(src->files[i].path, path))
			return &src->files[i];
	if (!src->root || src->file_count >= XKB_SOURCE_FILES ||
		src->bytes >= XKB_BYTES_MAX)
		return NULL;

	char full[1024];
	snprintf(full, sizeof full, "%s/%s", src->root, path);
	size_t len = 0;
	size_t budget = XKB_BYTES_MAX - src->bytes;
	if (budget > XKB_FILE_MAX)
		budget = XKB_FILE_MAX;
	char *text = xkb_read_file(full, budget, &len);
	if (!text)
		return NULL;
	XkbFile *f = &src->files[src->file_count];
	f->path = strdup(path);
	if (!f->path) {
		free(text);
		return NULL;
	}
	f->text = text;
	f->len = len;
	f->lent = false;
	src->file_count++;
	src->bytes += len;
	return f;
}

/* Leave `l` just inside the brace of the section asked for -- the one named,
 * or with no name the one flagged `default`, else the first of the kind --
 * or return false. The other sections are stepped over by their brackets. */
static bool find_section(Lex *l, XkbKind kind, const char *name, size_t n)
{
	Lex first = *l;
	bool have_first = false, flagged = false;

	lex_next(l);
	while (l->kind != TOK_END) {
		if (tok_word(l, "default")) {
			flagged = true;
			lex_next(l);
			continue;
		}
		if (!tok_word(l, KIND_WORD[kind])) {
			/* partial, hidden, alphanumeric_keys, a brace of the keymap
			 * around the sections, or another kind's section: that one is
			 * skipped whole, so its body cannot pass for ours. */
			const bool other = tok_word(l, "xkb_keycodes") ||
							   tok_word(l, "xkb_symbols") ||
							   tok_word(l, "xkb_types") ||
							   tok_word(l, "xkb_compatibility") ||
							   tok_word(l, "xkb_compat") ||
							   tok_word(l, "xkb_geometry");
			lex_next(l);
			if (other) {
				while (l->kind != TOK_END && !tok_is(l, '{'))
					lex_next(l);
				if (tok_is(l, '{')) {
					lex_next(l);
					lex_skip(l, '}');
					if (tok_is(l, '}'))
						lex_next(l);
				}
				flagged = false;
			}
			continue;
		}
		lex_next(l);
		const char *sname = NULL;
		size_t slen = 0;
		if (l->kind == TOK_STRING) {
			sname = l->s;
			slen = l->n;
		}
		while (l->kind != TOK_END && !tok_is(l, '{'))
			lex_next(l);
		if (l->kind == TOK_END)
			break;
		lex_next(l);
		const bool match = n ? (slen == n && sname && !memcmp(sname, name, n))
							 : flagged;
		if (match)
			return true;
		if (n == 0 && !have_first) {
			first = *l;
			have_first = true;
		}
		lex_skip(l, '}');
		if (tok_is(l, '}'))
			lex_next(l);
		flagged = false;
	}
	if (have_first) {
		*l = first;
		return true;
	}
	return false;
}

static void parse_keycodes(Lex *l, Parse *p, Merge merge);
static void parse_symbols(Lex *l, Parse *p, unsigned base, Merge merge);

/* One piece of a chain, "file(section):N": its section parsed into the
 * same map, with its keys in group N. */
static void include_piece(Parse *p, XkbKind kind, const char *piece,
						  size_t n, unsigned base, Merge merge)
{
	const char *file = piece, *sect = NULL;
	size_t flen = n, slen = 0;
	for (size_t i = 0; i < n; i++) {
		if (piece[i] == '(' && !sect) {
			flen = i;
			sect = piece + i + 1;
			slen = 0;
			for (size_t j = i + 1; j < n && piece[j] != ')'; j++)
				slen++;
		} else if (piece[i] == ':') {
			unsigned g;
			if (!sect)
				flen = i;
			if (decimal_value(piece + i + 1, n - i - 1, &g) && g >= 1)
				base = g - 1;
			break;
		}
	}
	if (p->depth >= XKB_INCLUDE_DEPTH || p->src->pieces >= XKB_PIECES_MAX) {
		note_missing(p, kind, file, flen, sect, slen,
					 p->depth >= XKB_INCLUDE_DEPTH ? "included too deep"
												   : "past the includes one "
													 "keymap may have");
		return;
	}
	p->src->pieces++;
	const XkbFile *f = source_file(p->src, kind, file, flen);
	if (!f) {
		note_missing(p, kind, file, flen, sect, slen, "not found");
		return;
	}
	Lex fl = { .p = f->text, .end = f->text + f->len };
	if (!find_section(&fl, kind, sect, slen)) {
		note_missing(p, kind, file, flen, sect, slen,
					 "has no such section");
		return;
	}
	p->depth++;
	if (kind == XKB_KEYCODES)
		parse_keycodes(&fl, p, merge);
	else
		parse_symbols(&fl, p, base, merge);
	p->depth--;
}

/* An include's string, "pc+de(nodeadkeys)+inet(evdev)": each piece in
 * turn, the sign before it the merge of its keys. */
static void include_chain(Parse *p, XkbKind kind, const char *s, size_t n,
						  unsigned base, Merge merge)
{
	size_t i = 0;
	while (i < n) {
		if (s[i] == '+' || s[i] == '|') {
			merge = s[i] == '|' ? MERGE_AUGMENT : MERGE_OVERRIDE;
			i++;
			continue;
		}
		size_t start = i;
		while (i < n && s[i] != '+' && s[i] != '|')
			i++;
		size_t len = i - start;
		while (len && s[start] == ' ') {
			start++;
			len--;
		}
		while (len && s[start + len - 1] == ' ')
			len--;
		if (len)
			include_piece(p, kind, s + start, len, base, merge);
	}
}

/* Entered just inside the section's brace; returns on the one that closes it.
 * A code past 255 is a multimedia key, which types nothing. Merged as
 * parse_symbols() merges, a name standing for a key: `replace` is an
 * override here, there being no levels to clear. */
static void parse_keycodes(Lex *l, Parse *p, Merge merge)
{
	Merge next = merge;
	while (l->kind != TOK_END && !tok_is(l, '}')) {
		if (tok_word(l, "replace") || tok_word(l, "override") ||
			tok_word(l, "augment")) {
			next = tok_word(l, "augment") ? MERGE_AUGMENT : MERGE_OVERRIDE;
			lex_next(l);
			continue;
		}
		const bool augment = next == MERGE_AUGMENT;
		next = merge;
		if (tok_word(l, "include")) {
			lex_next(l);
			if (l->kind == TOK_STRING) {
				if (p->src)
					include_chain(p, XKB_KEYCODES, l->s, l->n, 0,
								  augment ? MERGE_AUGMENT : MERGE_OVERRIDE);
				lex_next(l);
			}
			/* No semicolon follows an include. */
			continue;
		}
		if (l->kind == TOK_KEYNAME) {
			const char *s = l->s;
			const size_t n = l->n;
			unsigned code;
			lex_next(l);
			if (tok_is(l, '=')) {
				lex_next(l);
				if (l->kind == TOK_IDENT &&
					decimal_value(l->s, l->n, &code) && code < KEYMAP_CODES)
					names_add(p->names, s, n, code, augment, false);
			}
		} else if (tok_word(l, "alias")) {
			lex_next(l);
			if (l->kind == TOK_KEYNAME) {
				const char *s = l->s;
				const size_t n = l->n;
				lex_next(l);
				if (tok_is(l, '='))
					lex_next(l);
				if (l->kind == TOK_KEYNAME) {
					const int code = names_find(p->names, l->s, l->n);
					if (code >= 0)
						names_add(p->names, s, n, (unsigned)code, augment,
								  true);
				}
			}
		}
		lex_skip(l, ';');
		if (tok_is(l, ';'))
			lex_next(l);
	}
}

/* "Group2" and "2" both mean the second group, by the whole trailing
 * number, so Group12 is not Group2. One past the groups kept stands for
 * any group beyond them, which every caller drops; a name with no number,
 * or with 0, is none. */
static int group_index(const char *s, size_t n)
{
	size_t d = n;
	while (d > 0 && s[d - 1] >= '0' && s[d - 1] <= '9')
		d--;
	if (d == n)
		return -1;
	unsigned g;
	if (!decimal_value(s + d, n - d, &g))
		return KEYMAP_GROUPS;
	if (g < 1)
		return -1;
	return g > KEYMAP_GROUPS ? KEYMAP_GROUPS : (int)g - 1;
}

/* How many levels a key's type gives it, for the types with fewer than
 * four: the compiler cuts a key's merged levels to its type's, so a layout
 * that writes `type[Group1]="TWO_LEVEL"` over a four-level base loses the
 * base's third and fourth, and the key here must too. A type not listed
 * has four or more, which is as many as are kept. */
static unsigned type_levels(const char *s, size_t n)
{
	static const struct { const char *name; unsigned levels; } TYPES[] = {
		{ "ONE_LEVEL", 1 },
		{ "TWO_LEVEL", 2 }, { "ALPHABETIC", 2 }, { "KEYPAD", 2 },
		{ "PC_SUPER_LEVEL2", 2 }, { "PC_CONTROL_LEVEL2", 2 },
		{ "PC_ALT_LEVEL2", 2 }, { "SHIFT+ALT", 2 },
		{ "THREE_LEVEL", 3 },
	};
	for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
		if (strlen(TYPES[i].name) == n && !memcmp(TYPES[i].name, s, n))
			return TYPES[i].levels;
	return KEYMAP_LEVELS;
}

/* Which modifier selects a type's second level (keymap.h, KeySelect): the
 * three PC_*_LEVEL2 types group options put ISO_Next_Group under, and Shift
 * for every other. */
static KeySelect type_select(const char *s, size_t n)
{
	static const struct { const char *name; KeySelect sel; } TYPES[] = {
		{ "PC_ALT_LEVEL2", KEY_SELECT_ALT },
		{ "PC_CONTROL_LEVEL2", KEY_SELECT_CTRL },
		{ "PC_SUPER_LEVEL2", KEY_SELECT_SUPER },
	};
	for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++)
		if (strlen(TYPES[i].name) == n && !memcmp(TYPES[i].name, s, n))
			return TYPES[i].sel;
	return KEY_SELECT_SHIFT;
}

/* A type: the levels it keeps, and the modifier that selects its second. */
typedef struct {
	unsigned  levels;       /* KEYMAP_LEVELS: no cut */
	KeySelect select;
} KeyType;

/* The types a key is given, by group counted from its piece's first. A
 * type named for a group is that group's, and the unindexed one is every
 * other group's, in whichever order they are written -- as the compiler
 * reads them, so `type[Group1]="TWO_LEVEL", type[Group2]="FOUR_LEVEL"` is
 * two types and not the last. With none at all it is Shift's with no cut,
 * since the compiler then gives the key one of the Shift types. */
typedef struct {
	KeyType   any;
	KeyType   group[KEYMAP_GROUPS];
	unsigned  named;        /* bit g: group[g] was named */
} KeyTypes;

/* `type[group] = "name"` into `t`: group -1 is the unindexed form, and one
 * past the groups kept (group_index()) is dropped. */
static void types_set(KeyTypes *t, int group, const char *s, size_t n)
{
	const KeyType k = { type_levels(s, n), type_select(s, n) };
	if (group < 0) {
		t->any = k;
	} else if (group < KEYMAP_GROUPS) {
		t->group[group] = k;
		t->named |= 1u << group;
	}
}

/* From the piece's first group on, and never below it: a piece included as
 * `:2` holds the second group, and the first is another piece's. */
static void key_cut(Keymap *km, int code, unsigned base, const KeyTypes *t,
					uint32_t keep)
{
	if (code < 0)
		return;
	for (unsigned g = base; g < KEYMAP_GROUPS; g++) {
		if ((keep >> g) & 1u)
			continue;
		const unsigned r = g - base;
		const KeyType k = (t->named >> r) & 1u ? t->group[r] : t->any;
		km->select[code][g] = (uint8_t)k.select;
		for (unsigned level = k.levels; level < KEYMAP_LEVELS; level++)
			km->sym[code][g][level] = 0;
	}
}

/* One level of one key. NoSymbol is no change: in a chain of files it is
 * how a later piece adds a level without taking the ones below, and in a
 * compositor's text a key is written once, so the slot is empty anyway. */
static void level_set(Keymap *km, int code, unsigned group, unsigned level,
					  uint32_t sym, Merge merge)
{
	if (code < 0 || group >= KEYMAP_GROUPS || level >= KEYMAP_LEVELS ||
		sym == 0)
		return;
	uint32_t *slot = &km->sym[code][group][level];
	if (merge == MERGE_AUGMENT && *slot)
		return;
	*slot = sym;
}

/* One bracketed list of levels, entered just inside the bracket and left
 * just past the one that closes it. */
static void parse_levels(Lex *l, Keymap *km, int code, unsigned group,
						 Merge merge)
{
	unsigned level = 0;
	/* A `[` never closed stops at the key's `}`, which parse_key() takes,
	 * rather than reading the next keys' symbols as this one's levels. */
	while (l->kind != TOK_END && !tok_is(l, ']') && !tok_is(l, '}')) {
		if (l->kind == TOK_IDENT) {
			level_set(km, code, group, level, keysym_from_name(l->s, l->n),
					  merge);
			level++;
			lex_next(l);
		} else if (tok_is(l, '{')) {
			/* Several keysyms on one level: a key that types a sequence. It
			 * still occupies its level, and types nothing here. */
			lex_next(l);
			lex_skip(l, '}');
			if (tok_is(l, '}'))
				lex_next(l);
			level++;
		} else {
			lex_next(l);
		}
	}
	if (tok_is(l, ']'))
		lex_next(l);
}

/* The inside of one key's braces, left on the brace that closes them. A bare
 * list is the next group in order from `base`; a named one says which. The
 * key's types, group by group its own or else the section's default, cut
 * the levels after. */
static void parse_key(Lex *l, Keymap *km, int code, unsigned base,
					  Merge merge, const KeyTypes *deflt)
{
	unsigned implicit = 0;
	KeyTypes types = *deflt;
	/* An augment fills in what is empty and replaces nothing: a group the
	 * key already had -- symbols, or a selector of its own -- keeps the type
	 * it was given, its levels and its selector (key_cut()). */
	uint32_t keep = 0;
	if (merge == MERGE_AUGMENT && code >= 0)
		for (unsigned g = 0; g < KEYMAP_GROUPS; g++) {
			bool had = km->select[code][g] != KEY_SELECT_SHIFT;
			for (unsigned v = 0; v < KEYMAP_LEVELS && !had; v++)
				had = km->sym[code][g][v] != 0;
			if (had)
				keep |= 1u << g;
		}
	while (l->kind != TOK_END && !tok_is(l, '}')) {
		if (tok_is(l, '[')) {
			lex_next(l);
			parse_levels(l, km, code, base + implicit++, merge);
		} else if (l->kind == TOK_IDENT) {
			const bool symbols = tok_word(l, "symbols");
			const bool typed = tok_word(l, "type");
			int group = -1;
			lex_next(l);
			if (tok_is(l, '[')) {
				lex_next(l);
				if (l->kind == TOK_IDENT)
					group = group_index(l->s, l->n);
				lex_skip(l, ']');
				if (tok_is(l, ']'))
					lex_next(l);
			}
			if (tok_is(l, '='))
				lex_next(l);
			if (symbols && tok_is(l, '[')) {
				lex_next(l);
				parse_levels(l, km, code,
							 base + (group >= 0 ? (unsigned)group
												: implicit++), merge);
			} else {
				if (typed && l->kind == TOK_STRING)
					types_set(&types, group, l->s, l->n);
				lex_skip(l, ',');
			}
		} else if (!tok_is(l, ',')) {
			lex_next(l);
		}
		if (tok_is(l, ','))
			lex_next(l);
	}
	key_cut(km, code, base, &types, keep);
}

static void parse_symbols(Lex *l, Parse *p, unsigned base, Merge merge)
{
	/* A merge word stands in front of the one statement it is for. */
	Merge next = merge;
	/* `key.type[Group1] = "THREE_LEVEL";` is the type of every key the
	 * section defines after it, in each group the key names none of its own
	 * for. The section's alone: an included file starts with none. */
	KeyTypes deflt = { .any = { KEYMAP_LEVELS, KEY_SELECT_SHIFT } };
	while (l->kind != TOK_END && !tok_is(l, '}')) {
		if (tok_word(l, "replace") || tok_word(l, "override") ||
			tok_word(l, "augment")) {
			next = tok_word(l, "replace") ? MERGE_REPLACE
				 : tok_word(l, "augment") ? MERGE_AUGMENT : MERGE_OVERRIDE;
			lex_next(l);
			continue;
		}
		if (tok_word(l, "include")) {
			lex_next(l);
			if (l->kind == TOK_STRING) {
				if (p->src)
					include_chain(p, XKB_SYMBOLS, l->s, l->n, base,
								  next == MERGE_AUGMENT ? MERGE_AUGMENT
														: MERGE_OVERRIDE);
				lex_next(l);
			}
			next = merge;
			continue;
		}
		if (tok_word(l, "key")) {
			int code = -1;
			lex_next(l);
			if (tok_is(l, '.')) {
				lex_next(l);
				if (tok_word(l, "type")) {
					int group = -1;
					lex_next(l);
					if (tok_is(l, '[')) {
						lex_next(l);
						if (l->kind == TOK_IDENT)
							group = group_index(l->s, l->n);
						lex_skip(l, ']');
						if (tok_is(l, ']'))
							lex_next(l);
					}
					if (tok_is(l, '='))
						lex_next(l);
					if (l->kind == TOK_STRING)
						types_set(&deflt, group, l->s, l->n);
				}
			} else if (l->kind == TOK_KEYNAME) {
				code = names_find(p->names, l->s, l->n);
				lex_next(l);
			}
			if (tok_is(l, '{')) {
				lex_next(l);
				if (next == MERGE_REPLACE && code >= 0)
					memset(p->km->sym[code], 0, sizeof p->km->sym[code]);
				parse_key(l, p->km, code, base,
						  next == MERGE_AUGMENT ? MERGE_AUGMENT
												: MERGE_OVERRIDE, &deflt);
				if (tok_is(l, '}'))
					lex_next(l);
			}
		}
		next = merge;
		lex_skip(l, ';');
		if (tok_is(l, ';'))
			lex_next(l);
	}
}

bool keymap_parse_xkb_from(Keymap *km, const char *text, size_t len,
						   XkbSource *src)
{
	memset(km, 0, sizeof *km);

	XkbNames names = { .v = calloc(XKB_NAMES_MAX, sizeof(XkbName)) };
	if (!names.v)
		return false;
	Parse p = { .src = src, .names = &names, .km = km };

	Lex l = { .p = text, .end = text + len };
	bool any = false;
	lex_next(&l);
	while (l.kind != TOK_END) {
		const bool codes = tok_word(&l, "xkb_keycodes");
		const bool syms  = tok_word(&l, "xkb_symbols");
		lex_next(&l);
		if (!codes && !syms)
			continue;
		/* Past the section's quoted name to its brace. */
		while (l.kind != TOK_END && l.kind != TOK_PUNCT)
			lex_next(&l);
		if (!tok_is(&l, '{'))
			continue;
		lex_next(&l);
		if (codes) {
			parse_keycodes(&l, &p, MERGE_OVERRIDE);
		} else {
			parse_symbols(&l, &p, 0, MERGE_OVERRIDE);
			any = true;
		}
	}

	const bool ok = any && names.n > 0;
	free(names.v);
	return ok;
}

bool keymap_parse_xkb(Keymap *km, const char *text, size_t len)
{
	return keymap_parse_xkb_from(km, text, len, NULL);
}
