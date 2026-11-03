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

	free(s);
	return 0;
}
