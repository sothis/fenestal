/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * font_decode(), the decoder every atlas and the cell table come through.
 * The first four bytes, little-endian, are the output length asked for --
 * capped, since the shipped streams decode to a few hundred kilobytes -- and
 * the rest is the stream. The output is allocated to exactly that length, so
 * a write one byte past it is a report.
 */

#include "core/app.h"

#include "font_codec.h"

#include "fuzz.h"

#define OUT_MAX (1u << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 4)
		return 0;
	size_t out_len = ((size_t)data[0] | (size_t)data[1] << 8 |
					  (size_t)data[2] << 16 | (size_t)data[3] << 24) % OUT_MAX;
	unsigned char *out = malloc(out_len ? out_len : 1);
	if (!out)
		return 0;
	(void)font_decode(data + 4, size - 4, out, out_len);
	free(out);
	return 0;
}
