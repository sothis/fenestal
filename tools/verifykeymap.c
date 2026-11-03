/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Checks src/backend/keymap.c, keymap_xkb.c and keymap_rules.c: against a
 * keymap written out below, against a miniature xkeyboard-config tree
 * written for the run, against the build machine's own data where it has
 * any, and then against all of that damaged.
 *
 * The layout rules are the one part of text entry no driven round covers: a
 * round types on one layout, and on Wayland the compositor's input method
 * takes the dead keys first. So they are asked here, of a keymap with every
 * case in it, on each build. The files are the KMS backend's way to a
 * keymap, and what the reader must do with them -- follow an include
 * chain, pick a named section or the default one, let the includer's key
 * land over the included, leave a level alone for NoSymbol, keep the three
 * merges apart, put a piece in group 2, read the rules' $sets, wildcards,
 * indexed columns and options, end a cycle, step past a missing file --
 * is each one case in the tree below.
 *
 * The last part is the parsers against text that is not what it claims.
 * The text is another process's, need not be terminated, and is parsed on
 * the thread that pumps input. Each damaged copy lives in an allocation of
 * exactly its own length, and this is the one build-time tool the checker
 * trees instrument, so a read a byte past the end fails the asan build; in
 * the other trees what it proves is that the parser returns.
 */

#include "core/app.h"

#include "backend/keymap.h"

#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void expect(bool ok, const char *what, uint32_t got, uint32_t want)
{
	if (ok)
		return;
	failures++;
	fprintf(stderr, "verifykeymap: %s: got U+%04X, expected U+%04X\n", what,
			(unsigned)got, (unsigned)want);
}

#define EQ(what, got, want)                                                  \
	do {                                                                     \
		const uint32_t g_ = (got), w_ = (want);                              \
		expect(g_ == w_, what, g_, w_);                                      \
	} while (0)

static void expect_str(const char *what, const char *got, const char *want)
{
	if (!strcmp(got, want))
		return;
	failures++;
	fprintf(stderr, "verifykeymap: %s: got \"%s\", expected \"%s\"\n", what,
			got, want);
}

/* A damage loop that cannot have its copy has checked nothing, which is a
 * failure and not a pass. */
static void out_of_memory(const char *what)
{
	failures++;
	fprintf(stderr, "verifykeymap: out of memory for %s\n", what);
}

/* What xkbcommon writes, cut down to the keys asked about. The numbers are X
 * keycodes; the shapes -- a bare list, a named group, a type, an action list
 * to skip, a multi-keysym level, an alias -- are each one a compositor
 * sends. */
static const char KEYMAP[] =
	"xkb_keymap {\n"
	"xkb_keycodes \"(unnamed)\" {\n"
	"    minimum = 8;\n"
	"    maximum = 255;\n"
	"    <ESC>  = 9;\n"
	"    <AE01> = 10;\n"
	"    <AE02> = 11;\n"
	"    <BKSP> = 22;\n"
	"    <AD01> = 24;\n"
	"    <AD03> = 26;\n"
	"    <AD08> = 31;\n"
	"    <AD11> = 34;\n"
	"    <AC01> = 38;\n"
	"    <AC02> = 39;\n"
	"    <LFSH> = 50;\n"
	"    <AB02> = 53;\n"
	"    <SPCE> = 65;\n"
	"    <KP7>  = 79;\n"
	"    <RALT> = 108;\n"
	"    <I372> = 372;\n"
	"    alias <LatQ> = <AD01>;\n"
	"};\n"
	"xkb_types \"(unnamed)\" { type \"ONE_LEVEL\" { map[None] = 1; }; };\n"
	"xkb_compatibility \"(unnamed)\" {\n"
	"    interpret Shift_L { action = SetMods(modifiers=Shift); };\n"
	"};\n"
	"xkb_symbols \"(unnamed)\" {\n"
	"    name[Group1]=\"German\";\n"
	"    key <ESC>  { [ Escape ] };\n"
	"    key <AE01> { [ 1, exclam, onesuperior, exclamdown ] };\n"
	"    key <AE02> {\n"
	"        type= \"FOUR_LEVEL\",\n"
	"        symbols[Group1]= [ 2, quotedbl, twosuperior ],\n"
	"        symbols[Group2]= [ 2, at ]\n"
	"    };\n"
	"    key <BKSP> { [ BackSpace, BackSpace ] };\n"
	"    key <LatQ> { [ q, Q, at, Greek_OMEGA ] };\n"
	"    key <AD03> { [ e, E, EuroSign ], [ Cyrillic_u, Cyrillic_U ] };\n"
	"    key <AD08> { [ i, Iabovedot ] };\n"
	"    key <AD11> { [ dead_acute, dead_grave, dead_diaeresis ] };\n"
	"    key <AC01> { [ a ] };\n"
	"    key <AC02> {\n"
	"        repeat= No,\n"
	"        symbols[Group1]= [ idotless, I ],\n"
	"        actions[Group1]= [ NoAction(), SetMods(modifiers=Shift) ]\n"
	"    };\n"
	"    key <LFSH> { [ Shift_L ] };\n"
	"    key <AB02> { [ x, X, { a, b }, U20AC ] };\n"
	"    key <SPCE> { [ space ] };\n"
	"    key <KP7>  { type= \"KEYPAD\", [ KP_Home, KP_7 ] };\n"
	"    key <RALT> {\n"
	"        type= \"ONE_LEVEL\",\n"
	"        symbols[Group1]= [ ISO_Level3_Shift ]\n"
	"    };\n"
	"    key <I372> { [ XF86Favorites ] };\n"
	"    modifier_map Shift { <LFSH> };\n"
	"};\n"
	"};\n";

enum {
	K_ESC = 9, K_1 = 10, K_2 = 11, K_BKSP = 22, K_Q = 24, K_E = 26, K_I = 31,
	K_DEAD = 34, K_A = 38, K_DOTLESS = 39, K_SHIFT = 50, K_X = 53,
	K_SPACE = 65, K_KP7 = 79, K_RALT = 108,
};

static const KeyMods PLAIN;

/* What the key prints with no accent held: keymap_type() from a clean
 * composer each time, so one case cannot hand an accent to the next. */
static uint32_t keymap_text(const Keymap *km, unsigned code,
							const KeyMods *mods)
{
	KeyCompose kc = { 0 };
	return keymap_type(km, &kc, code, mods, false);
}

static void check_names(void)
{
	EQ("name a", keysym_from_name("a", 1), 0x61);
	EQ("name Udiaeresis", keysym_from_name("Udiaeresis", 10), 0xdc);
	EQ("name U20AC", keysym_from_name("U20AC", 5), 0x010020ac);
	EQ("name U458", keysym_from_name("U458", 4), 0x01000458);
	EQ("name U0001F12F", keysym_from_name("U0001F12F", 9), 0x0101f12f);
	EQ("name 0x1000123", keysym_from_name("0x1000123", 9), 0x1000123);
	EQ("name KP_Home", keysym_from_name("KP_Home", 7), 0xff95);
	EQ("name BackSpace", keysym_from_name("BackSpace", 9), 0xff08);
	EQ("name Escape", keysym_from_name("Escape", 6), 0xff1b);
	EQ("name dead_acute", keysym_from_name("dead_acute", 10), 0xfe51);
	/* Prints nothing and is wanted by name anyway: the KMS backend knows
	 * AltGr as the key whose plain level this is. */
	EQ("name ISO_Level3_Shift", keysym_from_name("ISO_Level3_Shift", 16),
	   0xfe03);
	EQ("name NoSymbol", keysym_from_name("NoSymbol", 8), 0);
	EQ("name any", keysym_from_name("any", 3), 0);
	EQ("name VoidSymbol", keysym_from_name("VoidSymbol", 10), 0xffffff);
	EQ("name none", keysym_from_name("none", 4), 0xffffff);
	EQ("name XF86AudioMute", keysym_from_name("XF86AudioMute", 13),
	   0xffffff);
	EQ("ucs of VoidSymbol", keysym_to_ucs(0xffffff), 0);
	/* A spelling xorgproto deprecated and xkeyboard-config still writes. */
	EQ("name masculine", keysym_from_name("masculine", 9), 0xba);
	EQ("name guillemotleft", keysym_from_name("guillemotleft", 13), 0xab);
	/* The table's two kinds of row: a keysym that fits in sixteen bits, and
	 * a Unicode keysym, which has to come back whole. */
	EQ("name Greek_OMEGA", keysym_from_name("Greek_OMEGA", 11), 0x7d9);
	EQ("name Cyrillic_u", keysym_from_name("Cyrillic_u", 10), 0x6d5);
	EQ("name KP_7", keysym_from_name("KP_7", 4), 0xffb7);
	EQ("name ohorn", keysym_from_name("ohorn", 5), 0x010001a1);
	EQ("name braille_blank", keysym_from_name("braille_blank", 13),
	   0x01002800);
	EQ("name with no row", keysym_from_name("Greek_OMEGAS", 12), 0);
	EQ("name before the first row", keysym_from_name("!", 1), 0);
	EQ("name after the last row", keysym_from_name("zzz", 3), 0);
	EQ("name of no length", keysym_from_name("a", 0), 0);
	/* A token inside somebody else's text: only `len` bytes are the name. */
	EQ("name cut from a longer word", keysym_from_name("exclamdown", 6), 0x21);

	EQ("ucs of a", keysym_to_ucs(0x61), 0x61);
	EQ("ucs of EuroSign", keysym_to_ucs(0x20ac), 0x20ac);
	EQ("ucs of a Unicode keysym", keysym_to_ucs(0x010020ac), 0x20ac);
	EQ("ucs of Cyrillic_a", keysym_to_ucs(0x6c1), 0x430);
	EQ("ucs of KP_7", keysym_to_ucs(0xffb7), '7');
	EQ("ucs of KP_Home", keysym_to_ucs(0xff95), 0);
	EQ("ucs of Shift_L", keysym_to_ucs(0xffe1), 0);
	EQ("ucs of dead_acute", keysym_to_ucs(0xfe51), 0);
	/* The value table, bisected: both of its ends, a row in the middle, and
	 * the gaps either side of one. */
	EQ("ucs of Aogonek", keysym_to_ucs(0x1a1), 0x104);
	EQ("ucs of Greek_OMEGA", keysym_to_ucs(0x7d9), 0x3a9);
	EQ("ucs of kana_A", keysym_to_ucs(0x4b1), 0x30a2);
	EQ("ucs of KP_Space", keysym_to_ucs(0xff80), ' ');
	EQ("ucs of KP_Equal", keysym_to_ucs(0xffbd), '=');
	EQ("ucs of ohorn", keysym_to_ucs(0x010001a1), 0x1a1);
	EQ("ucs of a keysym below the table", keysym_to_ucs(0x100), 0);
	EQ("ucs of a keysym between rows", keysym_to_ucs(0x1a4), 0);
	EQ("ucs of a keysym past sixteen bits", keysym_to_ucs(0x107d9), 0);
	EQ("ucs of no keysym", keysym_to_ucs(0), 0);
}

static void check_levels(const Keymap *km)
{
	KeyMods m;

	EQ("q", keymap_text(km, K_Q, &PLAIN), 'q');
	/* The symbols files' spelling of AltGr (level3(ralt_switch)), a type
	 * and a named group: read, or the KMS backend has no AltGr. */
	EQ("RALT is ISO_Level3_Shift", km->sym[K_RALT][0][0], 0xfe03);
	/* The layout's word on a repeat, for a key FnlKey does not name: a
	 * letter's text repeats, a modifier's or AltGr's does not. */
	EQ("q repeats", keymap_key_repeats(km, K_Q), 1);
	EQ("Shift does not repeat", keymap_key_repeats(km, K_SHIFT), 0);
	EQ("AltGr does not repeat", keymap_key_repeats(km, K_RALT), 0);
	m = PLAIN; m.shift = true;
	EQ("Shift+q", keymap_text(km, K_Q, &m), 'Q');
	m = PLAIN; m.lock = true;
	EQ("Caps q", keymap_text(km, K_Q, &m), 'Q');
	m.shift = true;
	EQ("Caps Shift+q", keymap_text(km, K_Q, &m), 'q');
	m = PLAIN; m.lock = true;
	EQ("Caps leaves a digit alone", keymap_text(km, K_1, &m), '1');
	m = PLAIN; m.shift = true;
	EQ("Shift+1", keymap_text(km, K_1, &m), '!');

	m = PLAIN; m.level3 = true;
	EQ("AltGr+q", keymap_text(km, K_Q, &m), '@');
	m.shift = true;
	EQ("AltGr+Shift+q", keymap_text(km, K_Q, &m), 0x3a9);
	m = PLAIN; m.level3 = true;
	EQ("AltGr where there is no third level", keymap_text(km, K_A, &m), 'a');
	EQ("a keysym given as a code point", keymap_text(km, K_X,
	   &(KeyMods){ .level3 = true, .shift = true }), 0x20ac);
	EQ("several keysyms on a level type nothing", keymap_text(km, K_X,
	   &(KeyMods){ .level3 = true }), 0);

	/* One symbol listed: its capital is implied. */
	m = PLAIN; m.shift = true;
	EQ("Shift+a, with only a listed", keymap_text(km, K_A, &m), 'A');
	m = PLAIN; m.lock = true;
	EQ("Caps a, with only a listed", keymap_text(km, K_A, &m), 'A');

	m = PLAIN; m.command = true;
	EQ("a key under Ctrl", keymap_text(km, K_Q, &m), 0);
	EQ("a key with no symbols", keymap_text(km, 200, &PLAIN), 0);
	EQ("a keycode off the table", keymap_text(km, 4000, &PLAIN), 0);
	EQ("a key that does not print", keymap_text(km, K_SHIFT, &PLAIN), 0);

	/* Groups: a named second group, a bare second list, and a key with one
	 * group serving both. */
	m = PLAIN; m.group = 1;
	EQ("group 2 of a named pair", keymap_text(km, K_2,
	   &(KeyMods){ .group = 1, .shift = true }), '@');
	EQ("group 2 of a bare pair", keymap_text(km, K_E, &m), 0x443);
	EQ("group 2 of a key with one", keymap_text(km, K_Q, &m), 'q');
	m.group = 7;
	EQ("a group off the table", keymap_text(km, K_Q, &m), 'q');
	m = PLAIN; m.level3 = true;
	EQ("AltGr+e", keymap_text(km, K_E, &m), 0x20ac);

	/* The keypad, where Num Lock is the Shift and Shift takes it back. */
	EQ("keypad 7 without Num Lock", keymap_text(km, K_KP7, &PLAIN), 0);
	m = PLAIN; m.num = true;
	EQ("keypad 7 with Num Lock", keymap_text(km, K_KP7, &m), '7');
	m.shift = true;
	EQ("keypad 7 with Num Lock and Shift", keymap_text(km, K_KP7, &m), 0);

	/* Turkish: the two pairs whose cases are not twenty apart. */
	m = PLAIN; m.lock = true;
	EQ("Caps i with a dotted capital", keymap_text(km, K_I, &m), 0x130);
	EQ("Caps dotless i", keymap_text(km, K_DOTLESS, &m), 'I');
}

/* Group keys (keymap_group_key()): which press switches, under which
 * modifier, and to what. The keys are the shapes xkeyboard-config's group
 * options write, each with the type that puts ISO_Next_Group under the
 * modifier it names. */
static const char GROUP_KEYMAP[] =
	"xkb_keymap {\n"
	"xkb_keycodes \"(unnamed)\" {\n"
	"    minimum = 8; maximum = 255;\n"
	"    <LCTL> = 37; <AC01> = 38; <LFSH> = 50; <LALT> = 64;\n"
	"    <SPCE> = 65; <CAPS> = 66; <RALT> = 108;\n"
	"};\n"
	"xkb_symbols \"(unnamed)\" {\n"
	"    key <AC01> { [ a, A ], [ Cyrillic_ef, Cyrillic_EF ] };\n"
	"    key <LALT> { [ NoSymbol, ISO_Next_Group ],"
	" type[group1] = \"TWO_LEVEL\" };\n"
	"    key <LFSH> { [ Shift_L, ISO_Next_Group ],"
	" type[group1] = \"PC_ALT_LEVEL2\" };\n"
	"    key <LCTL> { [ Control_L, ISO_Prev_Group_Lock ] };\n"
	"    key <SPCE> { [ space, ISO_Last_Group ],"
	" type[group1] = \"PC_SUPER_LEVEL2\" };\n"
	"    key <CAPS> { [ ISO_First_Group, Caps_Lock ],"
	" type[group1] = \"TWO_LEVEL\" };\n"
	"    key <RALT> { [ ISO_Next_Group ] };\n"
	"};\n"
	"};\n";

enum { G_LCTL = 37, G_A = 38, G_LFSH = 50, G_LALT = 64, G_SPCE = 65,
	   G_CAPS = 66, G_RALT = 108 };

/* The group a press moves to, or 0xffffffff where it is no group key. */
static uint32_t group_to(const Keymap *km, unsigned code, KeyMods m)
{
	unsigned to;
	return keymap_group_key(km, code, &m, keymap_groups(km), &to)
		   ? to : 0xffffffffu;
}

#define NO_GROUP 0xffffffffu

static void check_groups(void)
{
	Keymap *km = calloc(1, sizeof *km);
	if (!km || !keymap_parse_xkb(km, GROUP_KEYMAP, sizeof GROUP_KEYMAP - 1)) {
		failures++;
		fprintf(stderr, "verifykeymap: the group keymap did not parse\n");
		free(km);
		return;
	}
	EQ("groups: two filled", keymap_groups(km), 2);
	EQ("groups: one in an empty map", keymap_groups(&(Keymap){ 0 }), 1);

	EQ("groups: RAlt alone", group_to(km, G_RALT, (KeyMods){ 0 }), 1);
	EQ("groups: RAlt from group 2, by group 1's key",
	   group_to(km, G_RALT, (KeyMods){ .group = 1 }), 0);
	EQ("groups: LAlt alone is none", group_to(km, G_LALT, (KeyMods){ 0 }),
	   NO_GROUP);
	EQ("groups: LAlt under Shift",
	   group_to(km, G_LALT, (KeyMods){ .shift = true }), 1);
	EQ("groups: LShift alone is none",
	   group_to(km, G_LFSH, (KeyMods){ .shift = true }), NO_GROUP);
	EQ("groups: LShift under Alt, its type's",
	   group_to(km, G_LFSH, (KeyMods){ .shift = true, .alt = true }), 1);
	EQ("groups: a _Lock, back from the first",
	   group_to(km, G_LCTL, (KeyMods){ .shift = true }), 1);
	EQ("groups: Space alone is none", group_to(km, G_SPCE, (KeyMods){ 0 }),
	   NO_GROUP);
	EQ("groups: Space under Shift is none",
	   group_to(km, G_SPCE, (KeyMods){ .shift = true }), NO_GROUP);
	EQ("groups: Space under Super, the last",
	   group_to(km, G_SPCE, (KeyMods){ .super = true }), 1);
	EQ("groups: Caps, the first",
	   group_to(km, G_CAPS, (KeyMods){ .group = 1 }), 0);
	EQ("groups: Shift+Caps is Caps Lock",
	   group_to(km, G_CAPS, (KeyMods){ .shift = true }), NO_GROUP);
	EQ("groups: a letter is none", group_to(km, G_A, (KeyMods){ 0 }),
	   NO_GROUP);
	EQ("groups: typed in group 2",
	   keymap_text(km, G_A, &(KeyMods){ .group = 1 }), 0x444);
	free(km);
}

static void check_dead(const Keymap *km)
{
	KeyCompose kc = { 0 };
	KeyMods m;
	char held[8];

	EQ("an accent types nothing", keymap_type(km, &kc, K_DEAD, &PLAIN, false),
	   0);
	keymap_composing(&kc, held);
	EQ("and is shown as held", (unsigned char)held[0] << 8 |
	   (unsigned char)held[1], 0xc2b4);
	EQ("and joins the letter", keymap_type(km, &kc, K_E, &PLAIN, false), 0xe9);
	keymap_composing(&kc, held);
	EQ("and is then gone", (unsigned char)held[0], 0);

	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	EQ("its repeat is not a second press",
	   keymap_type(km, &kc, K_DEAD, &PLAIN, true), 0);
	EQ("Shift between the two is not a letter",
	   keymap_type(km, &kc, K_SHIFT, &PLAIN, false), 0);
	m = PLAIN; m.shift = true;
	EQ("and the capital is accented", keymap_type(km, &kc, K_E, &m, false),
	   0xc9);

	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	EQ("an accent twice is the accent",
	   keymap_type(km, &kc, K_DEAD, &PLAIN, false), 0xb4);
	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	EQ("an accent and a space is the accent",
	   keymap_type(km, &kc, K_SPACE, &PLAIN, false), 0xb4);
	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	EQ("a letter it does not go on arrives bare",
	   keymap_type(km, &kc, K_X, &PLAIN, false), 'x');

	m = PLAIN; m.shift = true;
	keymap_type(km, &kc, K_DEAD, &m, false);
	EQ("the key's other accent", keymap_type(km, &kc, K_A, &PLAIN, false),
	   0xe0);
	m = PLAIN; m.level3 = true;
	keymap_type(km, &kc, K_DEAD, &m, false);
	EQ("and its third", keymap_type(km, &kc, K_A, &PLAIN, false), 0xe4);

	/* A different accent replaces the one held. */
	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	m = PLAIN; m.shift = true;
	keymap_type(km, &kc, K_DEAD, &m, false);
	EQ("a second accent replaces the first",
	   keymap_type(km, &kc, K_E, &PLAIN, false), 0xe8);

	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	keymap_type(km, &kc, K_BKSP, &PLAIN, false);
	EQ("BackSpace takes an accent back",
	   keymap_type(km, &kc, K_E, &PLAIN, false), 'e');
	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	keymap_type(km, &kc, K_ESC, &PLAIN, false);
	EQ("and so does Escape", keymap_type(km, &kc, K_E, &PLAIN, false), 'e');

	keymap_type(km, &kc, K_DEAD, &PLAIN, false);
	m = PLAIN; m.command = true;
	EQ("a shortcut types nothing", keymap_type(km, &kc, K_E, &m, false), 0);
	EQ("and leaves the accent held",
	   keymap_type(km, &kc, K_E, &PLAIN, false), 0xe9);
}

/* Every prefix of the keymap, and copies with bytes overwritten. Nothing is
 * asked of the result but that it comes back. Deterministic, so a failure
 * repeats. */
static void check_damage(void)
{
	const size_t len = sizeof KEYMAP - 1;
	Keymap *km = malloc(sizeof *km);
	if (!km) {
		out_of_memory("the damaged keymaps");
		return;
	}

	for (size_t n = 0; n <= len; n++) {
		char *copy = malloc(n ? n : 1);
		if (!copy) {
			out_of_memory("the keymap's prefixes");
			break;
		}
		memcpy(copy, KEYMAP, n);
		keymap_parse_xkb(km, copy, n);
		free(copy);
	}

	static const char NOISE[] = "{}[]<>\"=;,\\/#\n\0x9U(";
	uint32_t seed = 0x2545f491u;
	char *copy = malloc(len);
	if (!copy)
		out_of_memory("the damaged keymaps");
	for (int round = 0; copy && round < 4000; round++) {
		memcpy(copy, KEYMAP, len);
		for (int hit = 0; hit <= round % 6; hit++) {
			seed = seed * 1664525u + 1013904223u;
			const size_t at = (seed >> 8) % len;
			seed = seed * 1664525u + 1013904223u;
			copy[at] = NOISE[(seed >> 8) % (sizeof NOISE - 1)];
		}
		keymap_parse_xkb(km, copy, len);
		/* Whatever it made of that, a lookup stays inside the table. */
		for (unsigned code = 0; code < KEYMAP_CODES; code += 7)
			(void)keymap_text(km, code, &PLAIN);
	}
	free(copy);
	free(km);
}


/* ---- the files ----------------------------------------------------------- */

/* A miniature xkeyboard-config tree, each file as the real ones are
 * written. Under the build directory for the length of the run, and a
 * second copy with no rules beside it. */
typedef struct {
	const char *path, *text;
} Fixture;

static const Fixture FIXTURE[] = {
	{ "rules/evdev",
	  "// the rules, cut to what the reader must do\n"
	  "! $qwertz = de tst\n"
	  "! $macs = mac \\\n"
	  "          applealu\n"
	  "! model = keycodes\n"
	  "  * = evdev\n"
	  "! layout = keycodes\n"
	  "  $qwertz = +aliases(qwertz)\n"
	  "  * = +aliases(qwerty)\n"
	  "! layout[1] = keycodes\n"
	  "  $qwertz = +aliases(qwertz)\n"
	  "  * = +aliases(qwerty)\n"
	  "! model = geometry\n"
	  "  * = pc(%m)\n"
	  "! model layout variant = symbols\n"
	  "  * tst over = pc+tst(over)\n"
	  "! model layout = symbols\n"
	  "  * * = pc+%l%(v)\n"
	  "! model layout[1] = symbols\n"
	  "  * * = pc+%l[1]%(v[1])\n"
	  "! model layout[2] = symbols\n"
	  "  * * = +%l[2]%(v[2]):2\n"
	  "! model = symbols\n"
	  "  $macs = +inet(mac)\n"
	  "  * = +inet(evdev)\n"
	  "! layout option = symbols\n"
	  "  * tst:deep = +tst(deep)\n"
	  "! option = symbols\n"
	  "  euro:e = +opt(e)\n"
	  "  tst:missing = +tst(missing)\n"
	  "  tst:gone = +nofile(x)\n" },
	{ "keycodes/evdev",
	  "default xkb_keycodes \"evdev\" {\n"
	  "    minimum = 8;\n"
	  "    maximum = 255;\n"
	  "    <AD01> = 24;\n"
	  "    <AD02> = 25;\n"
	  "    <AD03> = 26;\n"
	  "    <AD06> = 29;\n"
	  "    <AC01> = 38;\n"
	  "    <LSGT> = 94;\n"
	  "    <RALT> = 108;\n"
	  "    <I372> = 372;\n"
	  "    indicator 1 = \"Caps Lock\";\n"
	  "};\n" },
	{ "keycodes/aliases",
	  "default\n"
	  "xkb_keycodes \"qwerty\" {\n"
	  "    alias <LatQ> = <AD01>;\n"
	  "    alias <LatY> = <AD06>;\n"
	  "};\n"
	  "xkb_keycodes \"qwertz\" {\n"
	  "    alias <LatQ> = <AD01>;\n"
	  "    alias <LatZ> = <AD06>;\n"
	  "};\n" },
	{ "symbols/pc",
	  "default partial alphanumeric_keys modifier_keys\n"
	  "xkb_symbols \"pc105\" {\n"
	  "    key <RALT> {[ Alt_R ]};\n"
	  "    key <LSGT> {[ less, greater, bar, brokenbar ]};\n"
	  "    modifier_map Mod1 { Alt_L, Alt_R };\n"
	  "};\n" },
	{ "symbols/lvl",
	  "partial modifier_keys\n"
	  "xkb_symbols \"ralt\" {\n"
	  "  key <RALT> {[ ISO_Level3_Shift ], type[group1]=\"ONE_LEVEL\" };\n"
	  "};\n" },
	{ "symbols/base",
	  "default partial alphanumeric_keys\n"
	  "xkb_symbols \"basic\" {\n"
	  "    key <AD01> { [ q, Q, at ] };\n"
	  "    key <AD02> { [ w, W, U017F, section ] };\n"
	  "    key <AD03> { [ e, E ] };\n"
	  "    key <AD06> { [ y, Y ] };\n"
	  "    key <AC01> { [ a, A ] };\n"
	  "};\n" },
	{ "symbols/tst",
	  "// a layout the way the real ones are written\n"
	  "default partial alphanumeric_keys\n"
	  "xkb_symbols \"basic\" {\n"
	  "    include \"base\"\n"
	  "    name[Group1]=\"Test\";\n"
	  "    key <LatZ> { [ z, Z ] };\n"
	  "    key <LatQ> { [ NoSymbol, NoSymbol, EuroSign ] };\n"
	  "    include \"lvl(ralt)\"\n"
	  "};\n"
	  "partial alphanumeric_keys\n"
	  "xkb_symbols \"alt\" {\n"
	  "    include \"tst(basic)\"\n"
	  "    key <AD02> { [ v, V ] };\n"
	  "};\n"
	  "partial alphanumeric_keys\n"
	  "xkb_symbols \"over\" {\n"
	  "    include \"tst(alt)\"\n"
	  "    override key <AD03> { [ f, F ] };\n"
	  "    augment key <AD02> { [ x, X ] };\n"
	  "    replace key <AC01> { [ b ] };\n"
	  "};\n"
	  "partial xkb_symbols \"loop\" {\n"
	  "    include \"tst(loop2)\"\n"
	  "    key <AD01> { [ l ] };\n"
	  "};\n"
	  "partial xkb_symbols \"loop2\" {\n"
	  "    include \"tst(loop)\"\n"
	  "};\n"
	  "partial xkb_symbols \"missing\" {\n"
	  "    include \"nofile(section)\"\n"
	  "    include \"tst(nosuchsection)\"\n"
	  "    key <AD03> { [ m ] };\n"
	  "};\n"
	  "partial xkb_symbols \"deep\" {\n"
	  "    key <AD01> { [ d ] };\n"
	  "};\n"
	  "partial xkb_symbols \"typed\" {\n"
	  "    include \"base\"\n"
	  "    key.type[Group1] = \"THREE_LEVEL\";\n"
	  "    key <AD01> { [ q, Q, at, Greek_OMEGA ] };\n"
	  "    key <AD02> { type[group1]=\"TWO_LEVEL\", [ w, W ] };\n"
	  "    key <AD03> { [ e, E, none ] };\n"
	  "    key <AD06> { [ y, Y, any, any ] };\n"
	  "};\n"
	  "partial alphanumeric_keys\n"
	  "xkb_symbols \"pergroup\" {\n"
	  "    include \"tst(basic)\"\n"
	  "    key <AD02> {\n"
	  "        type[Group1] = \"PC_ALT_LEVEL2\",\n"
	  "        type[Group2] = \"FOUR_LEVEL\",\n"
	  "        symbols[Group1] = [ w, W, U017F, section ],\n"
	  "        symbols[Group2] = [ v, V, U017F, section ]\n"
	  "    };\n"
	  "};\n"
	  "partial alphanumeric_keys\n"
	  "xkb_symbols \"two\" {\n"
	  "    key <AD01> { type = \"TWO_LEVEL\", [ k, K ] };\n"
	  "    key <AD02> { [ x, X ] };\n"
	  "};\n"
	  "partial alphanumeric_keys\n"
	  "xkb_symbols \"augtype\" {\n"
	  "    include \"tst(pergroup)\"\n"
	  "    augment key <AD02> { type = \"TWO_LEVEL\", [ y, Y ] };\n"
	  "};\n" },
	{ "symbols/opt",
	  "partial xkb_symbols \"e\" {\n"
	  "    key <AD03> { [ NoSymbol, NoSymbol, EuroSign ] };\n"
	  "};\n" },
	{ "symbols/inet",
	  "partial xkb_symbols \"evdev\" {\n"
	  "    key <MUTE> { [ XF86AudioMute ] };\n"
	  "    key <I372> { [ XF86Favorites ] };\n"
	  "};\n"
	  "partial xkb_symbols \"mac\" {\n"
	  "    key <AC01> { [ NoSymbol, NoSymbol, NoSymbol, m ] };\n"
	  "};\n" },
};
#define FIXTURE_COUNT (sizeof FIXTURE / sizeof FIXTURE[0])

static const char *const FIXTURE_DIRS[] = { "rules", "keycodes", "symbols" };

/* The fixture's keys the tests ask about, beyond the keymap's above: W, the
 * key that is Y or Z, and the 102nd key; and in the real data the
 * o-umlaut's and the dead circumflex's. */
enum { F_W = 25, F_Z = 29, F_LSGT = 94, F_O = 47, F_TLDE = 49 };

static const char *fixture_text(const char *path)
{
	for (size_t i = 0; i < FIXTURE_COUNT; i++)
		if (!strcmp(FIXTURE[i].path, path))
			return FIXTURE[i].text;
	return "";
}

/* The tree under `root`, with or without its rules. */
static bool fixture_write(const char *root, bool rules)
{
	char path[512];
	if (mkdir(root, 0700) < 0 && errno != EEXIST)
		return false;
	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof path, "%s/%s", root, FIXTURE_DIRS[i]);
		if (mkdir(path, 0700) < 0 && errno != EEXIST)
			return false;
	}
	for (size_t i = 0; i < FIXTURE_COUNT; i++) {
		if (!rules && !strncmp(FIXTURE[i].path, "rules/", 6))
			continue;
		snprintf(path, sizeof path, "%s/%s", root, FIXTURE[i].path);
		FILE *f = fopen(path, "w");
		if (!f)
			return false;
		fputs(FIXTURE[i].text, f);
		if (fclose(f) != 0)
			return false;
	}
	return true;
}

static void fixture_remove(const char *root)
{
	char path[512];
	for (size_t i = 0; i < FIXTURE_COUNT; i++) {
		snprintf(path, sizeof path, "%s/%s", root, FIXTURE[i].path);
		unlink(path);
	}
	for (size_t i = 0; i < 3; i++) {
		snprintf(path, sizeof path, "%s/%s", root, FIXTURE_DIRS[i]);
		rmdir(path);
	}
	rmdir(root);
}

static void names_set(KeymapNames *n, const char *model, const char *layout,
					  const char *variant, const char *options)
{
	keymap_names_default(n);
	snprintf(n->model, sizeof n->model, "%s", model);
	snprintf(n->layout, sizeof n->layout, "%s", layout);
	snprintf(n->variant, sizeof n->variant, "%s", variant);
	snprintf(n->options, sizeof n->options, "%s", options);
}

/* Load `n` from `root`, expecting it to; false with the failure counted. */
static bool load(Keymap *km, const KeymapNames *n, const char *root,
				 char *detail, size_t detail_len, const char *what)
{
	if (keymap_load_names(km, n, root, detail, detail_len))
		return true;
	failures++;
	fprintf(stderr, "verifykeymap: %s: no keymap: %s\n", what, detail);
	return false;
}

static void check_files(Keymap *km)
{
	char root[] = "verifykeymap-XXXXXX";
	char norules[64], detail[192];
	KeymapNames n;
	KeyMods m;

	if (!mkdtemp(root)) {
		failures++;
		fprintf(stderr, "verifykeymap: cannot make the fixture directory\n");
		return;
	}
	if (!fixture_write(root, true)) {
		failures++;
		fprintf(stderr, "verifykeymap: cannot write the fixture tree\n");
		fixture_remove(root);
		return;
	}
	snprintf(norules, sizeof norules, "%s/norules", root);
	if (!fixture_write(norules, false)) {
		failures++;
		fprintf(stderr, "verifykeymap: cannot write the second tree\n");
	}

	/* The chain pc+tst+inet(evdev): base through tst, the alias through
	 * the keycodes' second piece, chosen by a $set, the includer's key
	 * over the included, NoSymbol leaving two levels, a named section of
	 * another file at the end. */
	names_set(&n, "pc105", "tst", "", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst")) {
		expect_str("files: nothing left out", detail, "");
		EQ("files: q from the included base", keymap_text(km, K_Q, &PLAIN),
		   'q');
		m = PLAIN; m.shift = true;
		EQ("files: Shift+q", keymap_text(km, K_Q, &m), 'Q');
		m = PLAIN; m.level3 = true;
		EQ("files: NoSymbol leaves the levels below EuroSign",
		   keymap_text(km, K_Q, &m), 0x20ac);
		EQ("files: the includer's key lands over the included",
		   keymap_text(km, F_Z, &PLAIN), 'z');
		EQ("files: RALT through a named section of another file",
		   km->sym[K_RALT][0][0], 0xfe03);
		EQ("files: the chain's first piece", keymap_text(km, F_LSGT, &PLAIN),
		   '<');
		EQ("files: a model's row is skipped past its match",
		   km->sym[K_A][0][3], 0);
	}

	/* A key's type cuts the levels a shorter one has no room for: the
	 * section's default, a key's own, and `none`, a level filled with
	 * nothing over the base's symbol, where `any` is nothing at all. */
	names_set(&n, "pc105", "tst", "typed", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst(typed)")) {
		m = PLAIN; m.level3 = true;
		EQ("files: a three-level default keeps the third",
		   keymap_text(km, K_Q, &m), '@');
		EQ("files: and cuts the fourth", km->sym[K_Q][0][3], 0);
		EQ("files: a two-level key loses the base's third",
		   km->sym[F_W][0][2], 0);
		EQ("files: and fourth", km->sym[F_W][0][3], 0);
		EQ("files: none covers the base's level", km->sym[K_E][0][2],
		   0xffffff);
		EQ("files: and types nothing", keymap_text(km, K_E, &m), 0);
		EQ("files: any leaves the base's level", keymap_text(km, F_Z, &m),
		   'y');
	}

	/* A type per group: each group named keeps its own -- its levels and
	 * the modifier that selects its second -- and not only the last one
	 * written. */
	names_set(&n, "pc105", "tst", "pergroup", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst(pergroup)")) {
		EQ("files: group 1's type cuts group 1", km->sym[F_W][0][2], 0);
		EQ("files: and selects its second level by Alt",
		   km->select[F_W][0], KEY_SELECT_ALT);
		EQ("files: group 2's type keeps group 2's four",
		   km->sym[F_W][1][3], 0xa7);
		EQ("files: and selects by Shift", km->select[F_W][1],
		   KEY_SELECT_SHIFT);
	}

	/* A piece at :2 types from the second group on: neither its unindexed
	 * type nor a key with none reaches back into the first's. */
	names_set(&n, "pc105", "tst,tst", "pergroup,two", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst(two):2")) {
		EQ("files: the second piece's key in group 2",
		   km->sym[K_Q][1][0], 'k');
		EQ("files: its type leaves group 1's third level",
		   km->sym[K_Q][0][2], 0x20ac);
		EQ("files: a key with no type leaves group 1's selector",
		   km->select[F_W][0], KEY_SELECT_ALT);
	}

	/* An augment fills in and replaces nothing: a group the key had keeps
	 * its type -- its selector, and the levels past the augment's. */
	names_set(&n, "pc105", "tst", "augtype", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst(augtype)")) {
		EQ("files: an augment leaves a level that is set",
		   km->sym[F_W][0][0], 'w');
		EQ("files: and the group's selector", km->select[F_W][0],
		   KEY_SELECT_ALT);
		EQ("files: and its levels past the augment's type",
		   km->sym[F_W][1][3], 0xa7);
	}

	/* %(v), and a section that includes the default one. */
	names_set(&n, "pc105", "tst", "alt", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst(alt)")) {
		EQ("files: the variant's key", keymap_text(km, F_W, &PLAIN), 'v');
		EQ("files: under the variant the basic keys stay",
		   keymap_text(km, F_Z, &PLAIN), 'z');
	}

	/* The three-column set's row, the two-column set's then dropped as a
	 * value that begins with no +; and the three merges. */
	names_set(&n, "pc105", "tst", "over", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst(over)")) {
		EQ("files: override takes the level", keymap_text(km, K_E, &PLAIN),
		   'f');
		EQ("files: augment leaves a level that is set",
		   keymap_text(km, F_W, &PLAIN), 'v');
		EQ("files: replace starts the key over", km->sym[K_A][0][0], 'b');
		EQ("files: and drops what it had", km->sym[K_A][0][1], 0);
	}

	/* Two layouts: the indexed sets, :2, and the unindexed sets skipped. */
	names_set(&n, "pc105", "tst,base", "", "");
	if (load(km, &n, root, detail, sizeof detail, "files: tst,base")) {
		EQ("files: group 1 of two", keymap_text(km, F_Z, &PLAIN), 'z');
		m = PLAIN; m.group = 1;
		EQ("files: group 2 of two", keymap_text(km, F_Z, &m), 'y');
		EQ("files: group 2 of a key the second has",
		   keymap_text(km, K_Q, &m), 'q');
	}

	/* Options: a row under `layout option`, rows under `option`, and the
	 * two that name what is not there, which the rest survives. */
	names_set(&n, "pc105", "tst", "",
			  "euro:e,tst:deep,tst:missing,tst:gone");
	if (load(km, &n, root, detail, sizeof detail, "files: options")) {
		m = PLAIN; m.level3 = true;
		EQ("files: an option adds a level", keymap_text(km, K_E, &m),
		   0x20ac);
		EQ("files: an option matched with the layout",
		   keymap_text(km, K_Q, &PLAIN), 'd');
		EQ("files: a key after two missing includes",
		   keymap_text(km, K_E, &PLAIN), 'm');
		EQ("files: the first miss is named",
		   strstr(detail, "nofile(section)") != NULL, 1);
	}

	/* A cycle ends, and the file's own key is still read. */
	names_set(&n, "pc105", "tst", "loop", "");
	if (load(km, &n, root, detail, sizeof detail, "files: a cycle")) {
		EQ("files: a cycle's own key", keymap_text(km, K_Q, &PLAIN), 'l');
		EQ("files: and the cycle is named", strstr(detail, "deep") != NULL,
		   1);
	}

	/* A $set with a continuation line, matched where * would have been. */
	names_set(&n, "mac", "tst", "", "");
	if (load(km, &n, root, detail, sizeof detail, "files: model mac")) {
		m = PLAIN; m.level3 = true; m.shift = true;
		EQ("files: a model's row from a $set", keymap_text(km, K_A, &m),
		   'm');
	}

	/* A layout no file has. */
	names_set(&n, "pc105", "nosuch", "", "");
	EQ("files: a layout with no file is refused",
	   keymap_load_names(km, &n, root, detail, sizeof detail), 0);
	EQ("files: and named", strstr(detail, "nosuch") != NULL, 1);

	/* No rules: the usual components. The qwerty aliases then have no
	 * <LatZ>, so tst's key for it is a key with no code. */
	names_set(&n, "pc105", "tst", "", "");
	if (load(km, &n, norules, detail, sizeof detail, "files: no rules")) {
		EQ("files: the usual chain without rules",
		   keymap_text(km, K_Q, &PLAIN), 'q');
		EQ("files: its qwerty aliases", keymap_text(km, F_Z, &PLAIN),
		   'y');
		EQ("files: RALT without rules", km->sym[K_RALT][0][0], 0xfe03);
		EQ("files: and the rules' absence is said",
		   strstr(detail, "rules/evdev") != NULL, 1);
	}

	fixture_remove(norules);
	fixture_remove(root);
}

/* The machine's own data, where the build runs on one that has it: the
 * German layout's stablest facts. */
static void check_real_tree(Keymap *km)
{
	const char *root = keymap_data_root();
	char detail[192];
	KeymapNames n;
	KeyMods m;

	if (!root) {
		fprintf(stderr, "verifykeymap: no xkeyboard-config data on this "
						"machine; the real tree is not checked\n");
		return;
	}
	names_set(&n, "pc105", "de", "nodeadkeys", "");
	if (load(km, &n, root, detail, sizeof detail, "data: de(nodeadkeys)")) {
		expect_str("data: nothing left out", detail, "");
		EQ("data: z where y is", keymap_text(km, F_Z, &PLAIN), 'z');
		EQ("data: odiaeresis", keymap_text(km, F_O, &PLAIN), 0xf6);
		m = PLAIN; m.level3 = true;
		EQ("data: AltGr+e", keymap_text(km, K_E, &m), 0x20ac);
		EQ("data: no dead circumflex", keymap_text(km, F_TLDE, &PLAIN), '^');
		EQ("data: RALT is ISO_Level3_Shift", km->sym[K_RALT][0][0], 0xfe03);
	}
	names_set(&n, "pc105", "de", "", "");
	if (load(km, &n, root, detail, sizeof detail, "data: de"))
		EQ("data: the dead circumflex", km->sym[F_TLDE][0][0], 0xfe52);
	names_set(&n, "pc105", "de,us", "", "");
	if (load(km, &n, root, detail, sizeof detail, "data: de,us")) {
		m = PLAIN; m.group = 1;
		EQ("data: group 2 is US", keymap_text(km, F_Z, &m), 'y');
	}
	/* The rules' option, its include and the types in it, read through. */
	names_set(&n, "pc105", "de,us", "", "grp:alt_shift_toggle");
	if (load(km, &n, root, detail, sizeof detail,
			 "data: de,us grp:alt_shift_toggle")) {
		EQ("data: two groups", keymap_groups(km), 2);
		EQ("data: LAlt under Shift",
		   group_to(km, G_LALT, (KeyMods){ .shift = true }), 1);
		EQ("data: LShift under Alt, from group 2",
		   group_to(km, G_LFSH, (KeyMods){ .shift = true, .alt = true,
										   .group = 1 }), 0);
		EQ("data: LShift alone is none",
		   group_to(km, G_LFSH, (KeyMods){ .shift = true }), NO_GROUP);
	}
}

static void check_names_text(void)
{
	KeymapNames n;

	EQ("spec: two layouts with options",
	   keymap_names_parse(&n, "de(nodeadkeys),us caps:none"), 1);
	expect_str("spec: the layouts", n.layout, "de,us");
	expect_str("spec: the variants", n.variant, "nodeadkeys,");
	expect_str("spec: the options", n.options, "caps:none");
	expect_str("spec: the model", n.model, "pc105");
	EQ("spec: one layout", keymap_names_parse(&n, "us"), 1);
	expect_str("spec: its layout", n.layout, "us");
	expect_str("spec: no variant", n.variant, "");
	EQ("spec: spaces around", keymap_names_parse(&n, "  de \t"), 1);
	expect_str("spec: trimmed", n.layout, "de");
	EQ("spec: nothing", keymap_names_parse(&n, ""), 0);
	expect_str("spec: nothing is the default", n.layout, "us");
	EQ("spec: an open parenthesis", keymap_names_parse(&n, "de("), 0);
	EQ("spec: a trailing comma", keymap_names_parse(&n, "de,"), 0);
	EQ("spec: five groups", keymap_names_parse(&n, "de,us,fr,it,es"), 0);
	EQ("spec: two words of options", keymap_names_parse(&n, "de x y"), 0);
	EQ("spec: a shell character", keymap_names_parse(&n, "de;rm"), 0);

	static const char CONF[] =
		"# KEYBOARD CONFIGURATION FILE\n\nXKBMODEL=\"pc104\"\n"
		"XKBLAYOUT=\"de\"\nXKBVARIANT=\"nodeadkeys\"\nXKBOPTIONS=\"\"\n"
		"BACKSPACE=\"guess\"\n";
	EQ("conf: read", keymap_names_from_conf(&n, CONF, sizeof CONF - 1), 1);
	expect_str("conf: model", n.model, "pc104");
	expect_str("conf: layout", n.layout, "de");
	expect_str("conf: variant", n.variant, "nodeadkeys");
	expect_str("conf: options", n.options, "");
	static const char BARE[] = "XKBLAYOUT=us # the default\nKEYMAP=us\n";
	EQ("conf: unquoted", keymap_names_from_conf(&n, BARE, sizeof BARE - 1),
	   1);
	expect_str("conf: unquoted layout", n.layout, "us");
	static const char NONE[] = "KEYMAP=de-latin1\n";
	EQ("conf: no layout", keymap_names_from_conf(&n, NONE, sizeof NONE - 1),
	   0);

	char keycodes[512], symbols[512];
	names_set(&n, "pc105", "de", "nodeadkeys", "");
	EQ("rules: empty text names nothing",
	   keymap_rules_resolve("", 0, &n, keycodes, sizeof keycodes, symbols,
							sizeof symbols), 0);
	const char *rules = fixture_text("rules/evdev");
	EQ("rules: the fixture resolves",
	   keymap_rules_resolve(rules, strlen(rules), &n, keycodes,
							sizeof keycodes, symbols, sizeof symbols), 1);
	expect_str("rules: keycodes", keycodes, "evdev+aliases(qwertz)");
	expect_str("rules: symbols", symbols, "pc+de(nodeadkeys)+inet(evdev)");
	names_set(&n, "pc105", "de,us", "nodeadkeys,", "caps:none");
	keymap_rules_resolve(rules, strlen(rules), &n, keycodes, sizeof keycodes,
						 symbols, sizeof symbols);
	expect_str("rules: two layouts", symbols,
			   "pc+de(nodeadkeys)+us:2+inet(evdev)");
}

/* A key name given twice: the later code under `+`, the first under `|`,
 * and an alias that names a key's own name dropped under either. */
static void check_keycode_merge(Keymap *km)
{
	static const char MOVED[] =
		"xkb_keycodes \"moved\" {\n"
		"    <AD01> = 25;\n"
		"    alias <AD03> = <AD01>;\n"
		"};\n";
	static const char *const BLOCK[2] = {
		"xkb_keymap { xkb_keycodes { include \"evdev+redef(moved)\" }; "
		"xkb_symbols { key <AD01> { [ k ] }; key <AD03> { [ m ] }; }; };",
		"xkb_keymap { xkb_keycodes { include \"evdev|redef(moved)\" }; "
		"xkb_symbols { key <AD01> { [ k ] }; key <AD03> { [ m ] }; }; };",
	};
	const char *codes = fixture_text("keycodes/evdev");
	for (int augment = 0; augment < 2; augment++) {
		XkbSource src = { 0 };
		xkb_source_lend(&src, "keycodes/evdev", codes, strlen(codes));
		xkb_source_lend(&src, "keycodes/redef", MOVED, sizeof MOVED - 1);
		const bool ok = keymap_parse_xkb_from(km, BLOCK[augment],
											  strlen(BLOCK[augment]), &src);
		xkb_source_free(&src);
		if (!ok) {
			expect(false, augment ? "names: evdev|redef parses"
								  : "names: evdev+redef parses", 0, 1);
			continue;
		}
		if (!augment) {
			EQ("names: override moves the key", km->sym[25][0][0], 'k');
			EQ("names: and leaves its old code", km->sym[K_Q][0][0], 0);
		} else {
			EQ("names: augment keeps the first code", km->sym[K_Q][0][0],
			   'k');
			EQ("names: and gives the second nothing", km->sym[25][0][0], 0);
		}
		EQ("names: an alias leaves a key's own name", km->sym[K_E][0][0],
		   'm');
	}
}

/* The readers of the files on damaged copies of them: the symbols file
 * lent in place of a disk, the rules as text, the override as a string.
 * Nothing is asked of the result but that it comes back. */
static void check_damage_files(Keymap *km)
{
	static const char NOISE[] = "{}[]<>\"=;,\\/#\n\0x9U(+|:!$%*";
	const char *tst = fixture_text("symbols/tst");
	const char *rules = fixture_text("rules/evdev");
	const char *codes = fixture_text("keycodes/evdev");
	const char *base = fixture_text("symbols/base");
	const char *lvl = fixture_text("symbols/lvl");
	static const char BLOCK[] =
		"xkb_keymap { xkb_keycodes { include \"evdev\" }; "
		"xkb_symbols { include \"pc+tst(over)+tst(loop)\" }; };";
	const size_t tlen = strlen(tst), rlen = strlen(rules);
	uint32_t seed = 0x6a09e667u;
	KeymapNames n;
	char keycodes[512], symbols[512];

	/* One block a file, each exactly its length: a shorter file's copy in
	 * the longer's block would leave its over-read inside the allocation,
	 * where no checker sees it. */
	char *copy = malloc(tlen);
	if (!copy)
		out_of_memory("the damaged symbols file");
	for (int round = 0; copy && round < 1500; round++) {
		memcpy(copy, tst, tlen);
		for (int hit = 0; hit <= round % 6; hit++) {
			seed = seed * 1664525u + 1013904223u;
			const size_t at = (seed >> 8) % tlen;
			seed = seed * 1664525u + 1013904223u;
			copy[at] = NOISE[(seed >> 8) % (sizeof NOISE - 1)];
		}
		XkbSource src = { 0 };
		xkb_source_lend(&src, "keycodes/evdev", codes, strlen(codes));
		xkb_source_lend(&src, "symbols/base", base, strlen(base));
		xkb_source_lend(&src, "symbols/lvl", lvl, strlen(lvl));
		xkb_source_lend(&src, "symbols/tst", copy, tlen);
		keymap_parse_xkb_from(km, BLOCK, sizeof BLOCK - 1, &src);
		xkb_source_free(&src);
		for (unsigned code = 0; code < KEYMAP_CODES; code += 7)
			(void)keymap_text(km, code, &PLAIN);
	}
	free(copy);
	copy = malloc(rlen);
	if (!copy)
		out_of_memory("the damaged rules file");
	for (int round = 0; copy && round < 1500; round++) {
		memcpy(copy, rules, rlen);
		for (int hit = 0; hit <= round % 6; hit++) {
			seed = seed * 1664525u + 1013904223u;
			const size_t at = (seed >> 8) % rlen;
			seed = seed * 1664525u + 1013904223u;
			copy[at] = NOISE[(seed >> 8) % (sizeof NOISE - 1)];
		}
		names_set(&n, "pc105", round & 1 ? "tst,base" : "tst", "", "euro:e");
		keymap_rules_resolve(copy, rlen, &n, keycodes, sizeof keycodes,
							 symbols, sizeof symbols);
		keymap_names_from_conf(&n, copy, rlen);
	}
	free(copy);

	static const char SPEC[] = "de(nodeadkeys),us(intl) caps:none,grp:toggle";
	char spec[sizeof SPEC];
	for (int round = 0; round < 1000; round++) {
		memcpy(spec, SPEC, sizeof SPEC);
		for (int hit = 0; hit <= round % 4; hit++) {
			seed = seed * 1664525u + 1013904223u;
			const size_t at = (seed >> 8) % (sizeof SPEC - 1);
			seed = seed * 1664525u + 1013904223u;
			spec[at] = NOISE[(seed >> 8) % (sizeof NOISE - 1)];
		}
		spec[sizeof SPEC - 1] = '\0';
		keymap_names_parse(&n, spec);
	}
}

int main(void)
{
	Keymap *km = malloc(sizeof *km);
	if (!km)
		return 1;

	check_names();

	if (!keymap_parse_xkb(km, KEYMAP, sizeof KEYMAP - 1)) {
		fprintf(stderr, "verifykeymap: the keymap did not parse\n");
		free(km);
		return 1;
	}
	check_levels(km);
	check_dead(km);
	check_groups();

	check_files(km);
	check_keycode_merge(km);
	check_real_tree(km);
	check_names_text();

	if (keymap_parse_xkb(&(Keymap){ 0 }, "not a keymap", 12))
		expect(false, "text that is not a keymap parsed", 1, 0);

	check_damage();
	check_damage_files(km);
	free(km);

	if (failures) {
		fprintf(stderr, "verifykeymap: %d failed\n", failures);
		return 1;
	}
	return 0;
}
