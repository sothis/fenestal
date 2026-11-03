/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Which code points a text size is baked for. One predicate, asked of two
 * tables -- mkfont's SIZE_RANGES and the FONT_SIZE_RANGE it emits, which
 * verifyfont reads -- so a set the emitter got wrong shows up as an absence
 * in the wrong place. A set is a flat array of inclusive first/last pairs.
 */

#ifndef FONT_CHARSET_H
#define FONT_CHARSET_H

/* Linear: a handful of ranges, asked once per glyph at the bake. */
static inline int charset_has(const unsigned *ranges, int n, unsigned cp)
{
	for (int i = 0; i < n; i++)
		if (cp >= ranges[2 * i] && cp <= ranges[2 * i + 1])
			return 1;
	return 0;
}

#endif
