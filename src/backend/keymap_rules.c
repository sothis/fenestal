/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The layout by its names -- model, layout, variant and options, as the
 * console's configuration and setxkbmap spell them -- turned into the files
 * that make it by xkeyboard-config's own rules, and read from them through
 * keymap_xkb.c. For the backend with no server to hand a keymap over:
 * libxkbcommon does this for a compositor, and the tool that would do it
 * on the command line (xkbcli) is not installed where the data is, so the
 * part of it that ends in keysyms is done here. DESIGN *KMS: no window
 * server*.
 *
 * rules/evdev is sets of rows under headers:
 *
 *     ! $qwertz = al ch cz de hr hu ro si sk
 *     ! model layout variant = symbols
 *       *     ben    basic   = pc+in(ben)
 *     ! model layout = symbols
 *       *     *      = pc+%l%(v)
 *     ! option = symbols
 *       caps:none = +capslock(none)
 *
 * A header names the columns a row matches -- model, layout, variant,
 * option, the two middle ones with [N] for the Nth of several -- and the
 * component its value goes to. A cell is a literal, a $set or *. In a
 * value %m, %l and %v are the names, %(v) the variant in parentheses or
 * nothing, %l[N] the Nth; a value beginning with + or | is appended to the
 * component, one that does not is the component while it is still empty.
 * A set's first matching row ends the set, except where it matches
 * options, every one of which may have a row. Columns without an index
 * are for one layout and indexed ones for several, never both. That is
 * libxkbcommon's reading of the file (rules.c), kept here for the
 * keycodes and symbols columns; types, compat and geometry are not read,
 * since a key's type is known by its name alone, from the standard types
 * keymap_xkb.c lists (type_levels(), type_select()), never from a types
 * file (DESIGN *Text entry*).
 */

#include "core/app.h"

#include "backend/keymap.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RULES_FILE_MAX   ((size_t)1 << 20)
#define CONF_FILE_MAX    ((size_t)64 << 10)
#define RULES_SETS_MAX   64         /* `! $name = ...` definitions */
#define RULES_COLUMNS    4
#define RULES_TOKENS     8
#define RULES_LAYOUTS    KEYMAP_GROUPS
#define COMPONENT_MAX    512

static const char *const CONSOLE_FILES[] = {
	"/etc/default/keyboard", "/etc/vconsole.conf",
};
#define KBD_MODEL_MAP "/usr/share/systemd/kbd-model-map"

/* ---- the rules ----------------------------------------------------------- */

typedef struct {
	const char *s;
	size_t      n;
} Span;

typedef struct {
	Span name;              /* without the $ */
	Span members;           /* the rest of its line, words */
} RuleSet;

typedef enum { COL_MODEL, COL_LAYOUT, COL_VARIANT, COL_OPTION } Column;
typedef enum { TO_KEYCODES, TO_SYMBOLS, TO_OTHER } Target;

/* The set of rows under one header. */
typedef struct {
	Column   col[RULES_COLUMNS];
	unsigned ncol;
	int      layout_idx;    /* [N] - 1, or -1 for an unindexed column */
	bool     has_option;
	Target   to;
	bool     skip;          /* not for this many layouts, or malformed */
	bool     done;          /* a row matched; the rest of the set is skipped */
} Header;

typedef struct {
	Span     model;
	Span     layouts[RULES_LAYOUTS], variants[RULES_LAYOUTS];
	unsigned nlayouts;
	Span     options;
	RuleSet  sets[RULES_SETS_MAX];
	unsigned nsets;
	char    *keycodes, *symbols;
	size_t   keycodes_size, symbols_size;
} Resolve;

static bool is_sep(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\\';
}

static bool span_is(Span a, const char *s)
{
	return strlen(s) == a.n && !memcmp(a.s, s, a.n);
}

static bool span_eq(Span a, Span b)
{
	return a.n == b.n && !memcmp(a.s, b.s, a.n);
}

/* The words of [s, e): `=` is one on its own, `//` ends the line. Those
 * past `max` are counted and not kept. */
static unsigned line_tokens(const char *s, const char *e, Span *tok,
							unsigned max)
{
	unsigned n = 0;
	while (s < e) {
		while (s < e && is_sep(*s))
			s++;
		if (s >= e || (s + 1 < e && s[0] == '/' && s[1] == '/'))
			break;
		const char *w = s;
		if (*s == '=')
			s++;
		else
			while (s < e && !is_sep(*s) && *s != '=')
				s++;
		if (n < max)
			tok[n] = (Span){ w, (size_t)(s - w) };
		n++;
	}
	return n;
}

/* A comma list's Nth element, empty past its end. */
static Span list_item(const char *list, unsigned idx)
{
	const char *s = list;
	for (unsigned i = 0; i < idx; i++) {
		const char *c = strchr(s, ',');
		if (!c)
			return (Span){ s, 0 };
		s = c + 1;
	}
	const char *c = strchr(s, ',');
	return (Span){ s, c ? (size_t)(c - s) : strlen(s) };
}

static unsigned list_count(const char *list)
{
	unsigned n = 1;
	for (const char *c = list; *c; c++)
		if (*c == ',')
			n++;
	return n;
}

static bool set_has(Span members, Span value)
{
	const char *s = members.s, *e = members.s + members.n;
	while (s < e) {
		while (s < e && is_sep(*s))
			s++;
		const char *w = s;
		while (s < e && !is_sep(*s))
			s++;
		if (s > w && span_eq((Span){ w, (size_t)(s - w) }, value))
			return true;
	}
	return false;
}

static bool cell_matches(const Resolve *r, Span cell, Span value)
{
	if (cell.n == 1 && cell.s[0] == '*')
		return true;
	if (cell.n > 1 && cell.s[0] == '$') {
		const Span name = { cell.s + 1, cell.n - 1 };
		for (unsigned i = 0; i < r->nsets; i++)
			if (span_eq(r->sets[i].name, name))
				return set_has(r->sets[i].members, value);
		/* A set nobody defined is one nothing is in. */
		return false;
	}
	return span_eq(cell, value);
}

static bool option_listed(const Resolve *r, Span cell)
{
	const char *s = r->options.s, *e = r->options.s + r->options.n;
	while (s < e) {
		const char *w = s;
		while (s < e && *s != ',')
			s++;
		if (span_eq((Span){ w, (size_t)(s - w) }, cell))
			return true;
		if (s < e)
			s++;
	}
	return false;
}

/* "layout[2]": the column and its index, or false for a word that is
 * neither column. */
static bool column_parse(Span w, Column *col, int *idx)
{
	static const struct { const char *name; Column col; } COLS[] = {
		{ "model", COL_MODEL }, { "layout", COL_LAYOUT },
		{ "variant", COL_VARIANT }, { "option", COL_OPTION },
	};
	*idx = -1;
	for (size_t i = 0; i < sizeof COLS / sizeof COLS[0]; i++) {
		const size_t n = strlen(COLS[i].name);
		if (w.n < n || memcmp(w.s, COLS[i].name, n))
			continue;
		*col = COLS[i].col;
		if (w.n == n)
			return true;
		if (w.n == n + 3 && w.s[n] == '[' && w.s[n + 2] == ']' &&
			w.s[n + 1] >= '1' && w.s[n + 1] <= '9') {
			*idx = w.s[n + 1] - '1';
			return true;
		}
		return false;
	}
	return false;
}

/* `! model layout[1] variant[1] = symbols`, after the `!`. */
static void header_parse(Resolve *r, Header *h, const Span *tok, unsigned n)
{
	memset(h, 0, sizeof *h);
	h->layout_idx = -1;
	h->to = TO_OTHER;
	unsigned i = 0;
	bool unindexed = false, indexed = false;
	while (i < n && !span_is(tok[i], "=")) {
		Column col = COL_MODEL;
		int idx = -1;
		if (h->ncol >= RULES_COLUMNS || !column_parse(tok[i], &col, &idx)) {
			h->skip = true;
			return;
		}
		if (col == COL_LAYOUT || col == COL_VARIANT) {
			if (idx < 0) {
				unindexed = true;
			} else {
				if (h->layout_idx >= 0 && h->layout_idx != idx) {
					h->skip = true;
					return;
				}
				h->layout_idx = idx;
				indexed = true;
			}
		}
		if (col == COL_OPTION)
			h->has_option = true;
		h->col[h->ncol++] = col;
		i++;
	}
	if (h->ncol == 0 || i + 1 >= n) {
		h->skip = true;
		return;
	}
	if (span_is(tok[i + 1], "keycodes"))
		h->to = TO_KEYCODES;
	else if (span_is(tok[i + 1], "symbols"))
		h->to = TO_SYMBOLS;
	/* An unindexed layout column is for one layout, an indexed one for
	 * several: libxkbcommon's own rule, which is what keeps `layout` and
	 * `layout[1]` rows from both applying. */
	if ((unindexed && r->nlayouts > 1) ||
		(indexed && (r->nlayouts == 1 ||
					 h->layout_idx >= (int)r->nlayouts)))
		h->skip = true;
}

static Span layout_at(const Resolve *r, int idx)
{
	if (idx < 0)
		return r->nlayouts == 1 ? r->layouts[0] : (Span){ "", 0 };
	return (r->nlayouts > 1 && idx < (int)r->nlayouts) ? r->layouts[idx]
													   : (Span){ "", 0 };
}

static Span variant_at(const Resolve *r, int idx)
{
	if (idx < 0)
		return r->nlayouts == 1 ? r->variants[0] : (Span){ "", 0 };
	return (r->nlayouts > 1 && idx < (int)r->nlayouts) ? r->variants[idx]
													   : (Span){ "", 0 };
}

/* `n` bytes onto `out` at `*at`, cut to what fits before the terminator.
 * The room is worked out first and the copy is one memcpy, which keeps
 * gcc's overflow analysis from reading the loop it would otherwise be as
 * unbounded. */
static void put(char *out, size_t size, size_t *at, const char *s, size_t n)
{
	size_t here = *at < size ? *at : size - 1;
	const size_t room = size - 1 - here;
	if (n > room)
		n = room;
	memcpy(out + here, s, n);
	here += n;
	out[here] = '\0';
	*at = here;
}

/* A row's value with its % forms filled in. */
static void value_expand(const Resolve *r, Span v, char *out, size_t size)
{
	size_t at = 0, i = 0;
	out[0] = '\0';
	while (i < v.n) {
		if (v.s[i] != '%') {
			put(out, size, &at, v.s + i, 1);
			i++;
			continue;
		}
		i++;
		const bool paren = i < v.n && v.s[i] == '(';
		if (paren)
			i++;
		if (i >= v.n)
			break;
		const char kind = v.s[i++];
		int idx = -1;
		if (i + 2 < v.n && v.s[i] == '[' && v.s[i + 2] == ']' &&
			v.s[i + 1] >= '1' && v.s[i + 1] <= '9') {
			idx = v.s[i + 1] - '1';
			i += 3;
		}
		if (paren && i < v.n && v.s[i] == ')')
			i++;
		Span name = { "", 0 };
		if (kind == 'm')
			name = r->model;
		else if (kind == 'l')
			name = layout_at(r, idx);
		else if (kind == 'v')
			name = variant_at(r, idx);
		if (name.n == 0)
			continue;
		if (paren)
			put(out, size, &at, "(", 1);
		put(out, size, &at, name.s, name.n);
		if (paren)
			put(out, size, &at, ")", 1);
	}
}

/* libxkbcommon's rule for a value against what the component holds:
 * +bar onto foo is foo+bar, bar onto +foo is bar+foo, bar onto nothing is
 * bar, and bar onto foo is foo still. */
static void component_add(char *to, size_t size, const char *add)
{
	char joined[2 * COMPONENT_MAX];
	if (!add[0])
		return;
	if (add[0] == '+' || add[0] == '|')
		snprintf(joined, sizeof joined, "%s%s", to, add);
	else if (to[0] == '+' || to[0] == '|')
		snprintf(joined, sizeof joined, "%s%s", add, to);
	else if (!to[0])
		snprintf(joined, sizeof joined, "%s", add);
	else
		return;
	snprintf(to, size, "%s", joined);
}

static void row_apply(Resolve *r, Header *h, const Span *tok, unsigned n)
{
	if (h->skip || h->done || h->to == TO_OTHER)
		return;
	if (n < h->ncol + 2 || !span_is(tok[h->ncol], "="))
		return;
	for (unsigned i = 0; i < h->ncol; i++) {
		bool ok = false;
		switch (h->col[i]) {
		case COL_MODEL:
			ok = cell_matches(r, tok[i], r->model);
			break;
		case COL_LAYOUT:
			ok = cell_matches(r, tok[i], layout_at(r, h->layout_idx));
			break;
		case COL_VARIANT:
			ok = cell_matches(r, tok[i], variant_at(r, h->layout_idx));
			break;
		default:
			ok = option_listed(r, tok[i]);
			break;
		}
		if (!ok)
			return;
	}
	char value[COMPONENT_MAX];
	value_expand(r, tok[h->ncol + 1], value, sizeof value);
	if (h->to == TO_KEYCODES)
		component_add(r->keycodes, r->keycodes_size, value);
	else
		component_add(r->symbols, r->symbols_size, value);
	if (!h->has_option)
		h->done = true;
}

bool keymap_rules_resolve(const char *rules, size_t len, const KeymapNames *n,
						  char *keycodes, size_t keycodes_size,
						  char *symbols, size_t symbols_size)
{
	Resolve r = {
		.model = { n->model, strlen(n->model) },
		.options = { n->options, strlen(n->options) },
		.keycodes = keycodes, .keycodes_size = keycodes_size,
		.symbols = symbols, .symbols_size = symbols_size,
	};
	keycodes[0] = symbols[0] = '\0';
	r.nlayouts = list_count(n->layout);
	if (r.nlayouts > RULES_LAYOUTS)
		r.nlayouts = RULES_LAYOUTS;
	for (unsigned i = 0; i < r.nlayouts; i++) {
		r.layouts[i] = list_item(n->layout, i);
		r.variants[i] = list_item(n->variant, i);
	}

	Header h = { .skip = true };
	const char *p = rules, *end = rules + len;
	while (p < end) {
		/* A logical line: a backslash before the newline continues it. */
		const char *ls = p, *le = p;
		for (;;) {
			while (le < end && *le != '\n')
				le++;
			if (le < end && le > ls && le[-1] == '\\') {
				le++;
				continue;
			}
			break;
		}
		p = le < end ? le + 1 : end;

		Span tok[RULES_TOKENS];
		unsigned ntok = line_tokens(ls, le, tok, RULES_TOKENS);
		if (ntok == 0)
			continue;
		if (ntok > RULES_TOKENS)
			ntok = RULES_TOKENS;
		if (!span_is(tok[0], "!")) {
			row_apply(&r, &h, tok, ntok);
			continue;
		}
		if (ntok >= 3 && tok[1].n > 1 && tok[1].s[0] == '$' &&
			span_is(tok[2], "=")) {
			if (r.nsets < RULES_SETS_MAX) {
				RuleSet *set = &r.sets[r.nsets++];
				set->name = (Span){ tok[1].s + 1, tok[1].n - 1 };
				set->members = (Span){ tok[2].s + 1,
									   (size_t)(le - tok[2].s - 1) };
			}
			continue;
		}
		header_parse(&r, &h, tok + 1, ntok - 1);
	}
	return symbols[0] != '\0';
}

/* ---- the names ----------------------------------------------------------- */

void keymap_names_default(KeymapNames *n)
{
	memset(n, 0, sizeof *n);
	snprintf(n->model, sizeof n->model, "pc105");
	snprintf(n->layout, sizeof n->layout, "us");
}

/* `NAME=value` or `NAME="value"` from shell-style text; false for a name
 * not there or with nothing after its sign, `out` then empty. */
static bool conf_value(const char *text, size_t len, const char *name,
					   char *out, size_t size)
{
	const size_t nlen = strlen(name);
	const char *p = text, *end = text + len;
	out[0] = '\0';
	while (p < end) {
		const char *le = memchr(p, '\n', (size_t)(end - p));
		const char *ln = p;
		if (!le)
			le = end;
		p = le < end ? le + 1 : end;
		while (ln < le && (*ln == ' ' || *ln == '\t'))
			ln++;
		if ((size_t)(le - ln) <= nlen || memcmp(ln, name, nlen) ||
			ln[nlen] != '=')
			continue;
		const char *v = ln + nlen + 1;
		const char quote = (v < le && (*v == '"' || *v == '\'')) ? *v : 0;
		if (quote)
			v++;
		size_t i = 0;
		while (v + i < le && v[i] != quote &&
			   (quote || (v[i] != ' ' && v[i] != '\t' && v[i] != '#')) &&
			   i + 1 < size) {
			out[i] = v[i];
			i++;
		}
		out[i] = '\0';
		return i > 0;
	}
	return false;
}

bool keymap_names_from_conf(KeymapNames *n, const char *text, size_t len)
{
	char model[sizeof n->model];
	keymap_names_default(n);
	if (!conf_value(text, len, "XKBLAYOUT", n->layout, sizeof n->layout)) {
		keymap_names_default(n);
		return false;
	}
	conf_value(text, len, "XKBVARIANT", n->variant, sizeof n->variant);
	conf_value(text, len, "XKBOPTIONS", n->options, sizeof n->options);
	if (conf_value(text, len, "XKBMODEL", model, sizeof model))
		memcpy(n->model, model, sizeof model);
	return true;
}

/* systemd's table from a console keymap to the X names, one line each:
 * `de-latin1-nodeadkeys  de  pc105  nodeadkeys  terminate:ctrl_alt_bksp`,
 * a `-` for none. */
static bool kbd_model_map(const char *keymap, KeymapNames *n)
{
	size_t len = 0;
	char *text = xkb_read_file(KBD_MODEL_MAP, CONF_FILE_MAX, &len);
	if (!text)
		return false;
	bool found = false;
	const char *p = text, *end = text + len;
	while (p < end && !found) {
		const char *le = memchr(p, '\n', (size_t)(end - p));
		if (!le)
			le = end;
		Span tok[RULES_TOKENS];
		const unsigned ntok = line_tokens(p, le, tok, RULES_TOKENS);
		p = le < end ? le + 1 : end;
		if (ntok < 2 || ntok > RULES_TOKENS || tok[0].s[0] == '#' ||
			!span_is(tok[0], keymap))
			continue;
		char *const field[] = { n->layout, n->model, n->variant, n->options };
		const size_t size[] = { sizeof n->layout, sizeof n->model,
								sizeof n->variant, sizeof n->options };
		for (unsigned i = 0; i < 4 && i + 1 < ntok; i++) {
			if (span_is(tok[i + 1], "-"))
				continue;
			snprintf(field[i], size[i], "%.*s", (int)tok[i + 1].n,
					 tok[i + 1].s);
		}
		found = true;
	}
	free(text);
	return found;
}

bool keymap_names_console(KeymapNames *n)
{
	for (size_t i = 0; i < sizeof CONSOLE_FILES / sizeof CONSOLE_FILES[0];
		 i++) {
		size_t len = 0;
		char *text = xkb_read_file(CONSOLE_FILES[i], CONF_FILE_MAX, &len);
		if (!text)
			continue;
		bool got = keymap_names_from_conf(n, text, len);
		/* The console keymap's own name, which an Arch console has in
		 * place of the X names. */
		char keymap[64];
		if (!got && conf_value(text, len, "KEYMAP", keymap, sizeof keymap)) {
			keymap_names_default(n);
			got = kbd_model_map(keymap, n);
		}
		free(text);
		if (got)
			return true;
	}
	keymap_names_default(n);
	return false;
}

static bool name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		   (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool keymap_names_parse(KeymapNames *n, const char *spec)
{
	keymap_names_default(n);
	const char *s = spec;
	while (*s == ' ' || *s == '\t')
		s++;
	const char *e = s;
	while (*e && *e != ' ' && *e != '\t')
		e++;
	const char *o = e;
	while (*o == ' ' || *o == '\t')
		o++;
	const char *oe = o + strlen(o);
	while (oe > o && (oe[-1] == ' ' || oe[-1] == '\t' || oe[-1] == '\n'))
		oe--;

	char layout[sizeof n->layout] = "", variant[sizeof n->variant] = "";
	size_t lat = 0, vat = 0;
	unsigned groups = 0;
	bool any_variant = false;
	const char *p = s;
	while (p < e) {
		if (groups == RULES_LAYOUTS)
			return false;
		const char *w = p;
		while (p < e && name_char(*p))
			p++;
		const size_t nlen = (size_t)(p - w);
		if (nlen == 0)
			return false;
		const char *v = p;
		size_t vlen = 0;
		if (p < e && *p == '(') {
			v = ++p;
			while (p < e && name_char(*p))
				p++;
			if (p >= e || *p != ')')
				return false;
			vlen = (size_t)(p - v);
			p++;
			any_variant = true;
		}
		if (p < e) {
			/* A comma, and something after it. */
			if (*p != ',' || p + 1 == e)
				return false;
			p++;
		}
		if (lat + nlen + 2 >= sizeof layout ||
			vat + vlen + 2 >= sizeof variant)
			return false;
		if (groups) {
			put(layout, sizeof layout, &lat, ",", 1);
			put(variant, sizeof variant, &vat, ",", 1);
		}
		put(layout, sizeof layout, &lat, w, nlen);
		put(variant, sizeof variant, &vat, v, vlen);
		groups++;
	}
	if (groups == 0)
		return false;
	for (const char *c = o; c < oe; c++)
		if (!name_char(*c) && *c != ':' && *c != ',' && *c != '+')
			return false;
	if ((size_t)(oe - o) >= sizeof n->options)
		return false;
	memcpy(n->layout, layout, sizeof layout);
	if (any_variant)
		memcpy(n->variant, variant, sizeof variant);
	memcpy(n->options, o, (size_t)(oe - o));
	n->options[oe - o] = '\0';
	return true;
}

/* ---- the files ----------------------------------------------------------- */

const char *keymap_data_root(void)
{
	static const char *const ROOTS[] = {
		"/usr/share/X11/xkb", "/usr/share/xkeyboard-config-2",
	};
	const char *env = getenv("XKB_CONFIG_ROOT");
	if (env && env[0])
		return env;
	for (size_t i = 0; i < sizeof ROOTS / sizeof ROOTS[0]; i++) {
		char path[256];
		struct stat st;
		snprintf(path, sizeof path, "%s/symbols", ROOTS[i]);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			return ROOTS[i];
	}
	return NULL;
}

static bool keymap_any(const Keymap *km)
{
	for (unsigned c = 0; c < KEYMAP_CODES; c++)
		for (unsigned g = 0; g < KEYMAP_GROUPS; g++)
			for (unsigned l = 0; l < KEYMAP_LEVELS; l++)
				if (km->sym[c][g][l])
					return true;
	return false;
}

/* Whether the chain's own piece that failed, if one did, is a layout's
 * file -- "symbols/de(nodeadkeyz): has no such section" for a layout of
 * `n` -- which is a layout that is not there, where a missing option or
 * an include inside a file is a note. */
static bool layout_missed(const XkbSource *src, const KeymapNames *n)
{
	static const char DIR[] = "symbols/";
	if (strncmp(src->top_miss, DIR, sizeof DIR - 1))
		return false;
	const char *file = src->top_miss + sizeof DIR - 1;
	size_t flen = 0;
	while (file[flen] && file[flen] != '(' && file[flen] != ':')
		flen++;
	for (unsigned i = 0; i < list_count(n->layout); i++) {
		const Span l = list_item(n->layout, i);
		if (l.n == flen && !memcmp(l.s, file, flen))
			return true;
	}
	return false;
}

bool keymap_load_names(Keymap *km, const KeymapNames *n, const char *root,
					   char *detail, size_t detail_len)
{
	detail[0] = '\0';
	memset(km, 0, sizeof *km);
	if (!root)
		root = keymap_data_root();
	if (!root) {
		snprintf(detail, detail_len, "no xkeyboard-config data: "
				 "XKB_CONFIG_ROOT is unset and neither /usr/share/X11/xkb "
				 "nor /usr/share/xkeyboard-config-2 has a symbols directory "
				 "(Debian's xkb-data, Arch's xkeyboard-config)");
		return false;
	}

	char keycodes[COMPONENT_MAX], symbols[COMPONENT_MAX], path[1024];
	snprintf(path, sizeof path, "%s/rules/evdev", root);
	size_t rlen = 0;
	char *rules = xkb_read_file(path, RULES_FILE_MAX, &rlen);
	const bool had_rules = rules != NULL;
	const bool resolved = had_rules &&
		keymap_rules_resolve(rules, rlen, n, keycodes, sizeof keycodes,
							 symbols, sizeof symbols);
	free(rules);
	if (!resolved) {
		/* What every PC model's rows come to: a rules file missing or
		 * silent is a note, not no layout. */
		const Span l = list_item(n->layout, 0), v = list_item(n->variant, 0);
		snprintf(keycodes, sizeof keycodes, "evdev+aliases(qwerty)");
		snprintf(symbols, sizeof symbols, "pc+%.*s%s%.*s%s+inet(evdev)",
				 (int)l.n, l.s, v.n ? "(" : "", (int)v.n, v.s,
				 v.n ? ")" : "");
		snprintf(detail, detail_len, "%s/rules/evdev %s; the usual "
				 "components assumed", root,
				 had_rules ? "names no symbols for it" : "is not there");
	}

	/* The component block xkbcomp takes, which is the parser's input as
	 * it stands. */
	char text[2 * COMPONENT_MAX + 96];
	snprintf(text, sizeof text, "xkb_keymap { xkb_keycodes { include \"%s\" "
			 "}; xkb_symbols { include \"%s\" }; };", keycodes, symbols);
	XkbSource src = { .root = root };
	const bool parsed = keymap_parse_xkb_from(km, text, strlen(text), &src);
	const bool typed = parsed && keymap_any(km) && !layout_missed(&src, n);
	if (!typed) {
		memset(km, 0, sizeof *km);
		snprintf(detail, detail_len, "%s (symbols %s under %s)",
				 src.top_miss[0] ? src.top_miss
				 : src.missing[0] ? src.missing
								  : "nothing in the files types",
				 symbols, root);
	} else if (!detail[0] && src.missing[0]) {
		snprintf(detail, detail_len, "%s", src.missing);
	}
	xkb_source_free(&src);
	return typed;
}
