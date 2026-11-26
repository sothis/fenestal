/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The JSON reader (src/core/json.c), on what a client of the MCP port
 * sends: every member the port asks for, looked up and decoded as the port
 * does, at the top and one level down. Malformed input must be refused, not
 * read past the terminator. Terminated here, as the port terminates a line
 * at its newline; the copy is exactly one byte longer for it.
 */

#include "core/app.h"

#include "fuzz.h"

static const char *const KEYS[] = {
	"jsonrpc", "id", "method", "params", "name", "arguments",
	"protocolVersion", "key", "action", "text", "x", "y", "dx", "dy",
	"button", "count", "wait_ms", "max_width", "spec",
};

static void probe(const char *obj, const char *end)
{
	for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
		const char *v = json_get(obj, KEYS[i]);
		if (!v)
			continue;
		if (v < obj || v > end)
			abort();
		const char *after = json_skip(v);
		if (after && (after <= v || after > end))
			abort();
		/* Decoded into a buffer that is just short enough to overrun,
		 * and into one that is not. */
		char small[8], big[512];
		if (json_str(v, small, sizeof small) && strlen(small) >= sizeof small)
			abort();
		if (json_str(v, big, sizeof big) && strlen(big) >= sizeof big)
			abort();
		double d;
		bool b;
		if (json_num(v, &d) && !isfinite(d))
			abort();
		(void)json_bool(v, &b);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *s = malloc(size + 1);
	if (!s)
		return 0;
	memcpy(s, data, size);
	s[size] = '\0';
	const char *end = s + strlen(s);

	const char *after = json_skip(s);
	if (after && (after <= s || after > end))
		abort();
	probe(s, end);
	probe(json_get(s, "params"), end);
	probe(json_get(json_get(s, "params"), "arguments"), end);

	free(s);
	return 0;
}
