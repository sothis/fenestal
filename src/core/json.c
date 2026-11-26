/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A reader for one JSON text held in memory and NUL-terminated: the MCP
 * port's requests (mcp.c), which are another program's bytes. Nothing is
 * built. A value is a pointer into the text, found by skipping, and a
 * string or a number is decoded only when it is asked for. Lenient about
 * what nothing here needs -- a bracket closed by the other kind skips as a
 * matched one does -- and never past the terminator, which is what
 * tools/fuzz/fuzz_json.c holds it to. The nesting is a count, not a
 * recursion, so no depth of brackets reaches the stack.
 *
 * Numbers are read here and not by strtod(), whose decimal point is the
 * locale's, and a caller may have set one that is a comma.
 */

#include "core/app.h"

static const char *ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	return p;
}

/* Past a string, `p` on its opening quote; NULL where it is never closed. */
static const char *skip_string(const char *p)
{
	for (p++; *p != '"'; p++) {
		if (!*p)
			return NULL;
		if (*p == '\\' && !*++p)
			return NULL;
	}
	return p + 1;
}

/* What a number, true, false or null is spelt with. Never the terminator. */
static bool scalar_char(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		   (c >= 'A' && c <= 'Z') || c == '-' || c == '+' || c == '.';
}

const char *json_skip(const char *p)
{
	int depth = 0;
	if (!p)
		return NULL;
	p = ws(p);
	do {
		const char c = *p;
		if (c == '"') {
			if (!(p = skip_string(p)))
				return NULL;
		} else if (c == '{' || c == '[') {
			depth++;
			p++;
		} else if (c == '}' || c == ']') {
			if (depth == 0)
				return NULL;
			depth--;
			p++;
		} else if (c == ',' || c == ':') {
			if (depth == 0)
				return NULL;
			p++;
		} else if (scalar_char(c)) {
			while (scalar_char(*p))
				p++;
		} else {
			return NULL;
		}
		p = ws(p);
	} while (depth > 0);
	return p;
}

/* A name is compared as it is spelt: one written with escapes is not found,
 * and no name this library asks for needs one. */
const char *json_get(const char *obj, const char *key)
{
	if (!obj || !key)
		return NULL;
	const size_t len = strlen(key);
	const char *p = ws(obj);
	if (*p != '{')
		return NULL;
	p = ws(p + 1);
	for (;;) {
		if (*p != '"')
			return NULL;
		const char *name = p + 1;
		if (!(p = skip_string(p)))
			return NULL;
		const bool hit = (size_t)(p - 1 - name) == len &&
						 memcmp(name, key, len) == 0;
		p = ws(p);
		if (*p != ':')
			return NULL;
		p = ws(p + 1);
		if (hit)
			return p;
		if (!(p = json_skip(p)) || *p != ',')
			return NULL;
		p = ws(p + 1);
	}
}

/* Four hex digits at `p`, or -1. Stops at the first that is not one, so a
 * terminator ends the read there. */
static int hex4(const char *p)
{
	int v = 0;
	for (int i = 0; i < 4; i++) {
		const char c = p[i];
		int d;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			d = c - 'A' + 10;
		else
			return -1;
		v = v * 16 + d;
	}
	return v;
}

bool json_str(const char *v, char *dst, size_t cap)
{
	size_t n = 0;
	if (!v || *v != '"' || !dst || cap == 0)
		return false;
	for (v++; *v != '"'; v++) {
		char buf[4];
		int k = 1;
		if (!*v)
			return false;
		buf[0] = *v;
		if (*v == '\\') {
			switch (*++v) {
			case '"':
			case '\\':
			case '/': buf[0] = *v; break;
			case 'b': buf[0] = '\b'; break;
			case 'f': buf[0] = '\f'; break;
			case 'n': buf[0] = '\n'; break;
			case 'r': buf[0] = '\r'; break;
			case 't': buf[0] = '\t'; break;
			case 'u': {
				const int hi = hex4(v + 1);
				if (hi < 0)
					return false;
				v += 4;
				uint32_t cp = (uint32_t)hi;
				/* A pair joins; either half alone is U+FFFD, as a
				 * malformed sequence is everywhere else in the library. */
				if (cp >= 0xd800 && cp < 0xdc00 && v[1] == '\\' &&
					v[2] == 'u') {
					const int lo = hex4(v + 3);
					if (lo >= 0xdc00 && lo < 0xe000) {
						cp = 0x10000 + ((cp - 0xd800) << 10) +
							 (uint32_t)(lo - 0xdc00);
						v += 6;
					}
				}
				k = utf8_encode(cp, buf);
				if (k == 0)
					k = utf8_encode(0xfffd, buf);
				break;
			}
			default: return false;
			}
		}
		if ((size_t)k >= cap - n)
			return false;
		memcpy(dst + n, buf, (size_t)k);
		n += (size_t)k;
	}
	dst[n] = '\0';
	return true;
}

bool json_num(const char *v, double *out)
{
	double x = 0.0;
	bool neg = false, any = false;
	if (!v || !out)
		return false;
	if (*v == '-') {
		neg = true;
		v++;
	}
	for (; *v >= '0' && *v <= '9'; v++) {
		x = x * 10.0 + (double)(*v - '0');
		any = true;
	}
	if (*v == '.') {
		double f = 0.1;
		for (v++; *v >= '0' && *v <= '9'; v++) {
			x += (double)(*v - '0') * f;
			f *= 0.1;
			any = true;
		}
	}
	if (!any)
		return false;
	if (*v == 'e' || *v == 'E') {
		int e = 0;
		bool down = false;
		v++;
		if (*v == '+' || *v == '-') {
			down = *v == '-';
			v++;
		}
		/* Past 400 every double is an infinity or a zero already. */
		for (; *v >= '0' && *v <= '9'; v++)
			if (e < 400)
				e = e * 10 + (*v - '0');
		while (e-- > 0)
			x = down ? x / 10.0 : x * 10.0;
	}
	x = neg ? -x : x;
	if (!isfinite(x))
		return false;
	*out = x;
	return true;
}

bool json_bool(const char *v, bool *out)
{
	if (!v || !out)
		return false;
	if (strncmp(v, "true", 4) == 0 && !scalar_char(v[4])) {
		*out = true;
		return true;
	}
	if (strncmp(v, "false", 5) == 0 && !scalar_char(v[5])) {
		*out = false;
		return true;
	}
	return false;
}
