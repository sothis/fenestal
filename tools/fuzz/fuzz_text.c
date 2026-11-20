/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * utf8_decode() and the text measure, on strings from the compositor and the
 * driver: an output's model name, a GPU name. Malformed input must measure
 * as U+FFFD, not read past the terminator. The input is terminated here
 * because every caller in the library is handed a C string; the copy is
 * exactly one byte longer for it.
 */

#include "core/app.h"

#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static bool loaded;
	if (!loaded) {
		if (text_load_cells() < 0)
			abort();
		loaded = true;
	}

	char *s = malloc(size + 1);
	if (!s)
		return 0;
	memcpy(s, data, size);
	s[size] = '\0';

	for (const char *p = s; *p; ) {
		uint32_t cp;
		bool     ok;
		const char *q = utf8_decode(p, &cp, &ok);
		if (q <= p || q > s + size)
			abort();
		if (ok && (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)))
			abort();
		p = q;
	}
	for (int sz = 0; sz < TEXT_SIZE_COUNT; sz++)
		(void)text_width(s, (TextSize)sz);

	/* fnl_text_elide(): what comes out fits the width it was given, ends
	 * inside its buffer, holds whole code points, and is the line's own
	 * bytes up to where it was cut -- a cut leaving no half sequence
	 * behind. */
	for (int sz = 0; sz < TEXT_SIZE_COUNT; sz++) {
		for (int max_w = 8; max_w <= 96; max_w += 11) {
			char buf[64];
			text_elide(buf, sizeof buf, s, max_w, (TextSize)sz);
			const size_t n = strlen(buf);
			if (n + 1 > sizeof buf) {
				fprintf(stderr, "elide: overran, n %zu\n", n);
				abort();
			}
			const int w = text_width(buf, (TextSize)sz);
			if (w > max_w) {
				fprintf(stderr, "elide: %d points for %d, sz %d: %s\n",
						w, max_w, sz, buf);
				abort();
			}
			const bool dots = n >= 3 && strcmp(buf + n - 3, "...") == 0;
			const size_t cut = dots ? n - 3 : n;
			if (cut && strncmp(buf, s, cut) != 0) {
				fprintf(stderr, "elide: not the line, cut %zu: %s\n",
						cut, buf);
				abort();
			}
			/* Only a real cut has a boundary: at zero the dots are all that
			 * fits, and what the line starts with is its own business. */
			if (dots && cut > 0 && s[cut] &&
				((unsigned char)s[cut] & 0xc0) == 0x80) {
				fprintf(stderr, "elide: cut %zu mid-sequence\n", cut);
				abort();
			}
		}
		char tiny[4];
		text_elide(tiny, sizeof tiny, s, 60, (TextSize)sz);
		if (strlen(tiny) + 1 > sizeof tiny) {
			fprintf(stderr, "elide: tiny overran\n");
			abort();
		}
		for (const char *p = tiny; *p; ) {
			uint32_t cp;
			bool     ok;
			const char *q = utf8_decode(p, &cp, &ok);
			if (q <= p) {
				fprintf(stderr, "elide: tiny stopped\n");
				abort();
			}
			p = q;
		}
	}

	/* fnl_text_wrap(): every line fits the width it was given (one code point
	 * wider than the width is the one exception), the bytes are the line's own
	 * and in order, and the lines it answers are the breaks plus one. */
	for (int sz = 0; sz < TEXT_SIZE_COUNT; sz++) {
		for (int max_w = 16; max_w <= 96; max_w += 13) {
			char buf[256];
			const int lines = text_wrap(buf, sizeof buf, s, max_w,
										(TextSize)sz);
			if (strlen(buf) + 1 > sizeof buf) {
				fprintf(stderr, "wrap: overran\n");
				abort();
			}
			/* Nothing wrapped is no line, and not the one an empty string
			 * would otherwise be counted as. */
			int n = 0;
			for (const char *ln = buf; buf[0]; ) {
				const char *e = strchr(ln, '\n');
				const size_t len = e ? (size_t)(e - ln) : strlen(ln);
				char one[256];
				if (len >= sizeof one) {
					fprintf(stderr, "wrap: line of %zu\n", len);
					abort();
				}
				memcpy(one, ln, len);
				one[len] = '\0';
				if (text_width(one, (TextSize)sz) > max_w) {
					/* Allowed only where one code point is wider than the
					 * width: the wrapper takes it rather than looping. */
					uint32_t cp;
					bool     ok;
					const char *q = utf8_decode(one, &cp, &ok);
					if (!ok || q == one || one[(size_t)(q - one)]) {
						fprintf(stderr, "wrap: %d points for %d: %s\n",
								text_width(one, (TextSize)sz), max_w, one);
						abort();
					}
				}
				n++;
				if (!e)
					break;
				ln = e + 1;
			}
			if (lines != n) {
				fprintf(stderr, "wrap: said %d lines, wrote %d, max_w %d "
						"sz %d\nsrc: %s\nout: %s\n", lines, n, max_w, sz,
						s, buf);
				abort();
			}
			/* Every byte is the line's own, in order; a space a break
			 * swallowed is simply not there. */
			const char *q = s;
			for (const char *b = buf; *b; b++) {
				if (*b == '\n')
					continue;
				while (*q && *q != *b)
					q++;
				if (!*q) {
					fprintf(stderr, "wrap: invented a byte\n");
					abort();
				}
				q++;
			}
		}
	}

	free(s);
	return 0;
}
