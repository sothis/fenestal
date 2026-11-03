/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * keymap_rules.c on text it was not written for: rules/evdev's, a console's
 * keyboard file's and FNL_KMS_LAYOUT's, which is the user's own line. The
 * first two are root's files, read as if they were not. One input is tried
 * as each of the three, for one layout and for two.
 */

#include "core/app.h"

#include "backend/keymap.h"

#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static KeymapNames names;
	static char keycodes[512], symbols[512];
	const char *text = (const char *)data;

	keymap_names_default(&names);
	snprintf(names.layout, sizeof names.layout, "de,us");
	snprintf(names.variant, sizeof names.variant, "nodeadkeys,");
	snprintf(names.options, sizeof names.options, "caps:none,grp:toggle");
	(void)keymap_rules_resolve(text, size, &names, keycodes, sizeof keycodes,
							   symbols, sizeof symbols);
	snprintf(names.layout, sizeof names.layout, "de");
	snprintf(names.variant, sizeof names.variant, "nodeadkeys");
	(void)keymap_rules_resolve(text, size, &names, keycodes, sizeof keycodes,
							   symbols, sizeof symbols);

	(void)keymap_names_from_conf(&names, text, size);

	/* An environment variable is terminated; the input is not. */
	char *spec = malloc(size + 1);
	if (spec) {
		memcpy(spec, data, size);
		spec[size] = '\0';
		(void)keymap_names_parse(&names, spec);
		free(spec);
	}
	return 0;
}
