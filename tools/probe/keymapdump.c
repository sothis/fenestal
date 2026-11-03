/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The keymap the KMS backend applies for a layout, as a table, two ways:
 * from xkeyboard-config's files through keymap_rules.c, which is how the
 * backend reads it, or from a compiled keymap's text on stdin, which is
 * what `xkbcli compile-keymap` writes (or `setxkbmap -print | xkbcomp -xkb
 * - -`). tools/probe/xkbdiff.py runs both over every layout and variant
 * the data lists and prints each key the two disagree on: the measure of
 * the reader against the tool (DESIGN *KMS: no window server*).
 *
 *   cc -std=gnu11 -O2 -Isrc -Iinclude -Ibuild/linux/release/generated \
 *      tools/probe/keymapdump.c src/backend/keymap.c \
 *      src/backend/keymap_xkb.c src/backend/keymap_rules.c -o keymapdump
 *   ./keymapdump de nodeadkeys                 # the files
 *   xkbcli compile-keymap --layout de --variant nodeadkeys | \
 *      ./keymapdump --text                     # the tool's text
 *
 * The arguments are layout, variant, options and model, each optional
 * after the one before, as the console's file spells them. One line per
 * keysym: "<code> <group> <level> 0x<keysym> 0x<typed>", the code an X
 * keycode and `typed` what the level types through keymap.c -- the code
 * point, or the keysym itself for a dead key and for ISO_Level3_Shift,
 * which the backend acts on, and 0 for one that types nothing. xkbdiff.py
 * compares that column: a modifier spelled two ways is no difference.
 * Exit status 1 when no keymap came of it, the reason on stderr.
 */
#include "core/app.h"

#include "backend/keymap.h"

#define TEXT_MAX ((size_t)4 << 20)

static int usage(void)
{
	fprintf(stderr, "usage: keymapdump <layout> [<variant> [<options> "
					"[<model>]]]\n       keymapdump --text < keymap.xkb\n");
	return 2;
}

int main(int argc, char **argv)
{
	static Keymap km;

	if (argc < 2)
		return usage();
	if (!strcmp(argv[1], "--text")) {
		char *text = malloc(TEXT_MAX);
		if (!text)
			return 1;
		const size_t len = fread(text, 1, TEXT_MAX, stdin);
		const bool ok = keymap_parse_xkb(&km, text, len);
		free(text);
		if (!ok) {
			fprintf(stderr, "keymapdump: no keymap in the text\n");
			return 1;
		}
	} else {
		KeymapNames n;
		char detail[192];
		keymap_names_default(&n);
		snprintf(n.layout, sizeof n.layout, "%s", argv[1]);
		if (argc > 2)
			snprintf(n.variant, sizeof n.variant, "%s", argv[2]);
		if (argc > 3)
			snprintf(n.options, sizeof n.options, "%s", argv[3]);
		if (argc > 4)
			snprintf(n.model, sizeof n.model, "%s", argv[4]);
		if (!keymap_load_names(&km, &n, NULL, detail, sizeof detail)) {
			fprintf(stderr, "keymapdump: %s\n", detail);
			return 1;
		}
		if (detail[0])
			fprintf(stderr, "keymapdump: left out: %s\n", detail);
	}

	for (unsigned c = 0; c < KEYMAP_CODES; c++)
		for (unsigned g = 0; g < KEYMAP_GROUPS; g++)
			for (unsigned l = 0; l < KEYMAP_LEVELS; l++) {
				const uint32_t sym = km.sym[c][g][l];
				uint32_t typed = keysym_to_ucs(sym);
				if (!typed && ((sym >= 0xfe50 && sym <= 0xfe93) ||
							   sym == 0xfe03))
					typed = sym;
				if (sym)
					printf("%u %u %u 0x%x 0x%x\n", c, g, l, sym, typed);
			}
	return 0;
}
