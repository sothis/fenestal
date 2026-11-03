/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * JSON for the benchmark's record: a writer into a growing buffer, and the
 * one reading a replay needs -- the members of a document's top-level
 * "options" object, as text. Not a general parser: the reader skips any
 * value it is not asked for by matching brackets and strings, which is all
 * a well-formed document needs, and refuses what it cannot follow.
 */

#include "bench.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- the writer ---------------------------------------------------------- */

void bench_json_init(BenchJson *j)
{
	memset(j, 0, sizeof *j);
	j->first = true;
}

void bench_json_free(BenchJson *j)
{
	free(j->buf);
	memset(j, 0, sizeof *j);
}

static void put(BenchJson *j, const char *s, size_t n)
{
	if (j->failed)
		return;
	if (n + 1 > j->cap - j->len || !j->buf) {
		size_t cap = j->cap ? j->cap : 4096;
		while (n + 1 > cap - j->len)
			cap *= 2;
		char *b = realloc(j->buf, cap);
		if (!b) {
			j->failed = true;
			return;
		}
		j->buf = b;
		j->cap = cap;
	}
	memcpy(j->buf + j->len, s, n);
	j->len += n;
	j->buf[j->len] = '\0';
}

static void puts_(BenchJson *j, const char *s)
{
	put(j, s, strlen(s));
}

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void putf(BenchJson *j, const char *fmt, ...)
{
	char tmp[64];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0)
		put(j, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void indent(BenchJson *j)
{
	put(j, "\n", 1);
	for (int i = 0; i < j->depth; i++)
		put(j, "  ", 2);
}

static void quoted(BenchJson *j, const char *s)
{
	put(j, "\"", 1);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\') {
			const char e[2] = { '\\', (char)*p };
			put(j, e, 2);
		} else if (*p == '\n') {
			put(j, "\\n", 2);
		} else if (*p == '\t') {
			put(j, "\\t", 2);
		} else if (*p == '\r') {
			put(j, "\\r", 2);
		} else if (*p < 0x20) {
			putf(j, "\\u%04x", *p);
		} else {
			put(j, (const char *)p, 1);
		}
	}
	put(j, "\"", 1);
}

/* The comma, the line and the key that every member and element starts
 * with. */
static void member(BenchJson *j, const char *key)
{
	if (!j->first)
		put(j, ",", 1);
	j->first = false;
	if (j->depth > 0)
		indent(j);
	if (key) {
		quoted(j, key);
		put(j, ": ", 2);
	}
}

void bench_json_obj(BenchJson *j, const char *key)
{
	member(j, key);
	put(j, "{", 1);
	j->depth++;
	j->first = true;
}

void bench_json_arr(BenchJson *j, const char *key)
{
	member(j, key);
	put(j, "[", 1);
	j->depth++;
	j->first = true;
}

static void close_(BenchJson *j, const char *c)
{
	const bool empty = j->first;
	j->depth--;
	if (!empty)
		indent(j);
	puts_(j, c);
	j->first = false;
}

void bench_json_end_obj(BenchJson *j)
{
	close_(j, "}");
}

void bench_json_end_arr(BenchJson *j)
{
	close_(j, "]");
}

void bench_json_str(BenchJson *j, const char *key, const char *v)
{
	member(j, key);
	if (v)
		quoted(j, v);
	else
		puts_(j, "null");
}

static void number(BenchJson *j, double v)
{
	if (!isfinite(v))
		puts_(j, "null");
	else if (v == floor(v) && fabs(v) < 1e15)
		putf(j, "%.0f", v);
	else
		/* Nine digits: a time since the start of a long measure, in
		 * seconds, keeps its microseconds past a thousand of them, and a
		 * float's value reads back the same. */
		putf(j, "%.9g", v);
}

void bench_json_num(BenchJson *j, const char *key, double v)
{
	member(j, key);
	number(j, v);
}

void bench_json_int(BenchJson *j, const char *key, long long v)
{
	member(j, key);
	putf(j, "%lld", v);
}

void bench_json_bool(BenchJson *j, const char *key, bool v)
{
	member(j, key);
	puts_(j, v ? "true" : "false");
}

void bench_json_null(BenchJson *j, const char *key)
{
	member(j, key);
	puts_(j, "null");
}

void bench_json_row(BenchJson *j, const double *v, int n)
{
	member(j, NULL);
	put(j, "[", 1);
	for (int i = 0; i < n; i++) {
		if (i)
			put(j, ", ", 2);
		number(j, v[i]);
	}
	put(j, "]", 1);
}

/* ---- the reader ---------------------------------------------------------- */

typedef struct {
	const char *p, *end;
} Cursor;

static void skip_ws(Cursor *c)
{
	while (c->p < c->end && (*c->p == ' ' || *c->p == '\n' ||
							 *c->p == '\r' || *c->p == '\t'))
		c->p++;
}

/* A string at the cursor, unescaped into `out` (cut at n - 1); the cursor
 * past its closing quote. \u escapes past ASCII are kept as '?': no option
 * a replay sets carries one. */
static bool read_string(Cursor *c, char *out, size_t n)
{
	size_t k = 0;
	if (c->p >= c->end || *c->p != '"')
		return false;
	c->p++;
	while (c->p < c->end && *c->p != '"') {
		char ch = *c->p++;
		if (ch == '\\') {
			if (c->p >= c->end)
				return false;
			const char e = *c->p++;
			switch (e) {
			case 'n': ch = '\n'; break;
			case 't': ch = '\t'; break;
			case 'r': ch = '\r'; break;
			case 'b': ch = '\b'; break;
			case 'f': ch = '\f'; break;
			case 'u': {
				if (c->end - c->p < 4)
					return false;
				char hex[5] = { c->p[0], c->p[1], c->p[2], c->p[3], 0 };
				/* Four hex digits and nothing else: strtol() would take a
				 * sign or a space as well. */
				if (strspn(hex, "0123456789abcdefABCDEF") != 4)
					return false;
				const long u = strtol(hex, NULL, 16);
				ch = u < 0x80 ? (char)u : '?';
				c->p += 4;
				break;
			}
			default:  ch = e; break;
			}
		}
		if (out && k + 1 < n)
			out[k++] = ch;
	}
	if (c->p >= c->end)
		return false;
	c->p++;
	if (out && n)
		out[k] = '\0';
	return true;
}

/* Past one value of any kind. */
static bool skip_value(Cursor *c)
{
	skip_ws(c);
	if (c->p >= c->end)
		return false;
	if (*c->p == '"')
		return read_string(c, NULL, 0);
	if (*c->p == '{' || *c->p == '[') {
		int depth = 0;
		while (c->p < c->end) {
			if (*c->p == '"') {
				if (!read_string(c, NULL, 0))
					return false;
				continue;
			}
			if (*c->p == '{' || *c->p == '[')
				depth++;
			else if (*c->p == '}' || *c->p == ']')
				depth--;
			c->p++;
			if (depth == 0)
				return true;
		}
		return false;
	}
	while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
		   *c->p != ' ' && *c->p != '\n' && *c->p != '\r' && *c->p != '\t')
		c->p++;
	return true;
}

/* A scalar's text: a string's unescaped, anything else as written. */
static bool read_scalar(Cursor *c, char *out, size_t n)
{
	skip_ws(c);
	if (c->p < c->end && *c->p == '"')
		return read_string(c, out, n);
	const char *start = c->p;
	if (!skip_value(c))
		return false;
	size_t len = (size_t)(c->p - start);
	if (len >= n)
		len = n - 1;
	memcpy(out, start, len);
	out[len] = '\0';
	return true;
}

bool bench_json_options(const char *path,
						bool (*fn)(void *ud, const char *key,
								   const char *value),
						void *ud)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "--bench-replay: cannot open %s\n", path);
		return false;
	}
	char *text = NULL;
	size_t len = 0, cap = 0;
	for (;;) {
		if (cap - len < 4096) {
			cap = cap ? cap * 2 : 65536;
			char *t = realloc(text, cap);
			if (!t) {
				free(text);
				fclose(f);
				fprintf(stderr, "--bench-replay: out of memory\n");
				return false;
			}
			text = t;
		}
		const size_t got = fread(text + len, 1, cap - len, f);
		len += got;
		if (got == 0)
			break;
	}
	fclose(f);

	Cursor c = { text, text + len };
	bool ok = false, found = false;
	/* What was wrong with the "options" object, said here: the member
	 * callback says what it refuses itself. */
	const char *bad = NULL;
	skip_ws(&c);
	if (c.p < c.end && *c.p == '{') {
		c.p++;
		for (;;) {
			char key[64];
			skip_ws(&c);
			if (c.p < c.end && *c.p == '}')
				break;
			if (!read_string(&c, key, sizeof key))
				break;
			skip_ws(&c);
			if (c.p >= c.end || *c.p != ':')
				break;
			c.p++;
			skip_ws(&c);
			if (!strcmp(key, "options") && c.p < c.end && *c.p == '{') {
				found = true;
				c.p++;
				ok = true;
				for (;;) {
					char k[64], v[BENCH_TEXT_MAX];
					skip_ws(&c);
					if (c.p < c.end && *c.p == '}')
						break;
					if (!read_string(&c, k, sizeof k)) {
						bad = c.p < c.end ? "a key that is not a string, "
											"or a bad escape in one"
										  : "the file ends inside it";
						ok = false;
						break;
					}
					skip_ws(&c);
					if (c.p >= c.end || *c.p != ':') {
						bad = "no colon after a key";
						ok = false;
						break;
					}
					c.p++;
					if (!read_scalar(&c, v, sizeof v)) {
						bad = "a value cut short, or a bad escape in one";
						ok = false;
						break;
					}
					if (!fn(ud, k, v))
						ok = false;
					skip_ws(&c);
					if (c.p < c.end && *c.p == ',')
						c.p++;
				}
				break;
			}
			if (!skip_value(&c))
				break;
			skip_ws(&c);
			if (c.p < c.end && *c.p == ',')
				c.p++;
		}
	}
	const long at = (long)(c.p - text);
	free(text);
	if (!found)
		fprintf(stderr, "--bench-replay: %s has no \"options\" object\n",
				path);
	else if (bad)
		fprintf(stderr, "--bench-replay: %s: the \"options\" object is "
				"malformed at byte %ld: %s\n", path, at, bad);
	return ok;
}
