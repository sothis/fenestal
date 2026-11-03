/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * keymap_parse_xkb(), on text a compositor sends: another process's, not
 * terminated, and parsed on the thread that pumps input. Whatever it parses
 * is then typed through, every key under every mix of Shift, Caps Lock, Num
 * Lock and AltGr in every group and one past the last, so a map the parser
 * filled with nonsense is also a map keymap_type() survives. Alt, Ctrl and
 * Super ride on the first three, which is every value a group key's
 * KeySelect can read; as a shortcut they are left out, since keymap_type()
 * returns before it reads the map. The same bytes are also a symbols file
 * an include names, read through the path that follows includes, with a
 * keycodes file lent beside it.
 */

#include "core/app.h"

#include "backend/keymap.h"

#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static Keymap km;
	static const char CODES[] =
		"default xkb_keycodes \"evdev\" { <AC01> = 38; <AD01> = 24; "
		"alias <LatQ> = <AD01>; };";
	static const char BLOCK[] =
		"xkb_keymap { xkb_keycodes { include \"evdev\" }; "
		"xkb_symbols { include \"f+f(x)|f:2\" }; };";
	XkbSource src = { 0 };

	xkb_source_lend(&src, "keycodes/evdev", CODES, sizeof CODES - 1);
	xkb_source_lend(&src, "symbols/f", (const char *)data, size);
	(void)keymap_parse_xkb_from(&km, BLOCK, sizeof BLOCK - 1, &src);
	xkb_source_free(&src);

	memset(&km, 0, sizeof km);
	if (!keymap_parse_xkb(&km, (const char *)data, size))
		return 0;

	const unsigned groups = keymap_groups(&km);
	for (unsigned code = 0; code < KEYMAP_CODES; code++) {
		for (unsigned m = 0; m < 16 * (KEYMAP_GROUPS + 1); m++) {
			KeyMods mods = {
				.shift = m & 1, .lock = (m >> 1) & 1, .num = (m >> 2) & 1,
				.level3 = (m >> 3) & 1, .group = m >> 4,
				.alt = m & 1, .ctrl = (m >> 1) & 1, .super = (m >> 2) & 1,
			};
			KeyCompose kc = { 0 };
			char out[8];
			unsigned to;
			(void)keymap_type(&km, &kc, code, &mods, false);
			keymap_composing(&kc, out);
			(void)keymap_type(&km, &kc, code, &mods, false);
			if (keymap_group_key(&km, code, &mods, groups, &to) &&
				to >= groups)
				abort();
		}
	}
	return 0;
}
