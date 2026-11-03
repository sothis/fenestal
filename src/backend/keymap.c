/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The layout: keysyms to code points, the level a set of modifiers selects,
 * and the dead keys. backend/keymap.h has what this is a substitute for and
 * where it stops short; the reader for XKB's text keymaps is keymap_xkb.c.
 */

#include "core/app.h"

#include "backend/keymap.h"
#include "backend/keysym_table.h"

#define KEYSYM_UCS_ROWS (sizeof KEYSYM_UCS / sizeof KEYSYM_UCS[0])

/* keysymdef.h's blocks, by number. tools/mkkeysyms.py tests the same ranges
 * and refuses to make a table that the shortcuts below are untrue of. */
#define KEYSYM_FUNC_FIRST 0xfd00u
#define KEYSYM_FUNC_LAST  0xffffu
#define KEYSYM_DEAD_FIRST 0xfe50u
#define KEYSYM_DEAD_LAST  0xfe93u
#define KEYSYM_KP_FIRST   0xff80u
#define KEYSYM_KP_LAST    0xffbdu
#define KEYSYM_BACKSPACE  0xff08u
#define KEYSYM_ESCAPE     0xff1bu

/* ---- keysyms ------------------------------------------------------------- */

/* The keypad's keysyms, which Num Lock selects where Shift selects letters. */
static bool keysym_is_keypad(uint32_t sym)
{
	return sym >= KEYSYM_KP_FIRST && sym <= KEYSYM_KP_LAST;
}

uint32_t keysym_to_ucs(uint32_t sym)
{
	/* Latin-1 is numbered as itself, and so is everything Unicode has added
	 * since: the code point plus KEYSYM_UNICODE. Only the legacy ranges
	 * between the two -- Latin-2 to 9, Greek, Cyrillic, the keypad -- need
	 * the table. */
	if ((sym >= 0x20 && sym <= 0x7e) || (sym >= 0xa0 && sym <= 0xff))
		return sym;
	if ((sym & 0xff000000u) == KEYSYM_UNICODE)
		return sym & 0x00ffffffu;

	/* The function block prints nothing outside the keypad, and it is most
	 * of what an X server hands over: every Shift, arrow and function key. */
	if (sym >= KEYSYM_FUNC_FIRST && sym <= KEYSYM_FUNC_LAST &&
		!keysym_is_keypad(sym))
		return 0;

	size_t lo = 0, hi = KEYSYM_UCS_ROWS;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (KEYSYM_UCS[mid].sym == sym)
			return KEYSYM_UCS[mid].ucs;
		if (KEYSYM_UCS[mid].sym < sym)
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

/* ---- levels -------------------------------------------------------------- */

/* Upper case, for the scripts a keymap is likely to leave it to us in. This
 * is asked two ways: to tell whether a pair of levels is one letter in two
 * cases, which is what Caps Lock acts on, and to supply the upper case of a
 * key that lists only its lower. A script missing from here types correctly
 * and ignores Caps Lock. */
static uint32_t ucs_upper(uint32_t c)
{
	if (c >= 'a' && c <= 'z')
		return c - 0x20;
	if ((c >= 0xe0 && c <= 0xfe && c != 0xf7) ||       /* Latin-1 */
		(c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) ||    /* Greek */
		(c >= 0x430 && c <= 0x44f))                    /* Cyrillic */
		return c - 0x20;
	if (c >= 0x450 && c <= 0x45f)
		return c - 0x50;
	if (c == 0xff)
		return 0x178;
	if (c == 0x131)                 /* dotless i, whose capital is plain I */
		return 'I';
	/* Latin Extended-A, which alternates -- upper on the even code points up
	 * to 0x137 and from 0x14a, on the odd ones between and after. */
	if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))
		return (c & 1u) ? c - 1 : c;
	if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
		return (c & 1u) ? c : c - 1;
	return c;
}

/* The key's keysym and what it prints, chosen together so that neither
 * caller can pick a different level from the other. `sym` is what a dead key
 * is recognised by, since a dead key prints nothing. */
static uint32_t keymap_lookup(const Keymap *km, unsigned code,
							  const KeyMods *mods, uint32_t *sym)
{
	*sym = 0;
	if (code >= KEYMAP_CODES)
		return 0;

	/* A key that defines fewer groups than the layout has serves every group
	 * from the ones it does define -- Space is Space in both halves of a
	 * two-layout keyboard. */
	const uint32_t *lv =
		km->sym[code][mods->group < KEYMAP_GROUPS ? mods->group : 0];
	if (!lv[0] && !lv[1] && !lv[2] && !lv[3])
		lv = km->sym[code][0];

	/* AltGr moves to the second pair where there is one, and is ignored
	 * where there is not, so it never turns a letter into nothing. */
	const unsigned base = (mods->level3 && (lv[2] || lv[3])) ? 2 : 0;
	/* Either of the pair may be the only one there: a level left empty
	 * under one that is not is a key that prints only with Shift. */
	const uint32_t lo = lv[base], up = lv[base + 1];
	if (!lo && !up)
		return 0;

	/* The keypad: Num Lock picks the digit, and Shift picks it back. */
	if (mods->num && keysym_is_keypad(up)) {
		*sym = mods->shift ? lo : up;
		return keysym_to_ucs(*sym);
	}

	/* A key listing one symbol is that symbol in both cases, which for a
	 * letter means its upper case is implied. */
	const uint32_t lo_c = lo ? keysym_to_ucs(lo) : 0;
	const uint32_t up_c = up ? keysym_to_ucs(up) : ucs_upper(lo_c);

	/* Caps Lock inverts Shift, and only for a letter: Shift held under Caps
	 * Lock types lower case, and a digit row is untouched by it. */
	/* Turkish i is the one pair the rule misses: its capital has a dot. */
	const bool letter = lo_c && up_c != lo_c &&
						(ucs_upper(lo_c) == up_c ||
						 (lo_c == 'i' && up_c == 0x130));
	const bool shifted = mods->shift != (mods->lock && letter);
	*sym = shifted && up ? up : lo;
	return shifted ? up_c : lo_c;
}

/* ---- dead keys ----------------------------------------------------------- */

#define COMPOSE_ROWS (sizeof COMPOSE_TABLE / sizeof COMPOSE_TABLE[0])
#define SPACING_ROWS (sizeof DEAD_SPACING / sizeof DEAD_SPACING[0])

/* The dead_* block of keysymdef.h. Wider than the accents the table knows:
 * a dead key with no row still has to be swallowed as one, or it would fall
 * through as a key that types nothing and leave the next letter bare with no
 * sign that an accent had been asked for. */
static bool keysym_is_dead(uint32_t sym)
{
	return sym >= KEYSYM_DEAD_FIRST && sym <= KEYSYM_DEAD_LAST;
}

static uint32_t dead_spacing(uint32_t dead)
{
	for (size_t i = 0; i < SPACING_ROWS; i++)
		if (DEAD_SPACING[i][0] == dead)
			return DEAD_SPACING[i][1];
	return 0;
}

static uint32_t compose(uint32_t dead, uint32_t base)
{
	size_t lo = 0, hi = COMPOSE_ROWS;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const ComposeRow *r = &COMPOSE_TABLE[mid];
		if (r->dead == dead && r->base == base)
			return r->made;
		if (r->dead < dead || (r->dead == dead && r->base < base))
			lo = mid + 1;
		else
			hi = mid;
	}
	return 0;
}

uint32_t keymap_type(const Keymap *km, KeyCompose *kc, unsigned code,
					 const KeyMods *mods, bool repeat)
{
	uint32_t sym;
	if (mods->command)
		return 0;
	const uint32_t c = keymap_lookup(km, code, mods, &sym);

	if (keysym_is_dead(sym)) {
		if (repeat)
			return 0;
		/* The same accent twice is the accent itself. A different one
		 * replaces what was held: two accents on one letter is a sequence
		 * this does not know. */
		if (kc->dead == sym) {
			kc->dead = 0;
			return dead_spacing(sym);
		}
		kc->dead = sym;
		return 0;
	}

	/* BackSpace and Escape are how an accent typed by mistake is taken back,
	 * and neither prints, so they have to be named. */
	if (sym == KEYSYM_BACKSPACE || sym == KEYSYM_ESCAPE)
		kc->dead = 0;
	/* Anything else that prints nothing -- Shift on its way to a capital, an
	 * arrow, a function key -- leaves the accent held. */
	if (!c || !kc->dead)
		return c;

	const uint32_t dead = kc->dead;
	kc->dead = 0;
	if (c == ' ')
		return dead_spacing(dead);
	const uint32_t made = compose(dead, c);
	return made ? made : c;
}

void keymap_composing(const KeyCompose *kc, char out[8])
{
	const uint32_t c = kc->dead ? dead_spacing(kc->dead) : 0;
	out[c ? utf8_encode(c, out) : 0] = '\0';
}

/* ---- groups -------------------------------------------------------------- */

/* keysymdef.h's group keys, each a lock in XKB's compat map, and its _Lock
 * one more. */
#define KEYSYM_NEXT_GROUP  0xfe08u
#define KEYSYM_PREV_GROUP  0xfe0au
#define KEYSYM_FIRST_GROUP 0xfe0cu
#define KEYSYM_LAST_GROUP  0xfe0eu

/* What xkeyboard-config's compat binds with `repeat= False`: the modifiers,
 * the locks and the ISO level and group keys. */
static bool keysym_held_only(uint32_t sym)
{
	return (sym >= 0xffe1u && sym <= 0xffeeu) ||     /* Shift_L..Hyper_R */
		   (sym >= 0xfe01u && sym <= 0xfe13u) ||     /* ISO_Lock..Level5 */
		   sym == 0xff7eu ||                         /* Mode_switch */
		   sym == 0xff7fu ||                         /* Num_Lock */
		   sym == 0xff14u;                           /* Scroll_Lock */
}

bool keymap_key_repeats(const Keymap *km, unsigned code)
{
	if (code >= KEYMAP_CODES)
		return false;
	for (unsigned g = 0; g < KEYMAP_GROUPS; g++)
		if (keysym_held_only(km->sym[code][g][0]))
			return false;
	return true;
}

unsigned keymap_groups(const Keymap *km)
{
	unsigned groups = 1;
	for (unsigned code = 0; code < KEYMAP_CODES; code++)
		for (unsigned g = groups; g < KEYMAP_GROUPS; g++)
			for (unsigned level = 0; level < KEYMAP_LEVELS; level++)
				if (km->sym[code][g][level])
					groups = g + 1;
	return groups;
}

bool keymap_group_key(const Keymap *km, unsigned code, const KeyMods *mods,
					  unsigned groups, unsigned *group)
{
	if (code >= KEYMAP_CODES || groups == 0)
		return false;
	/* The group's own symbols, or the first group's where the key has none
	 * in it -- as typing serves them, and how a group key written once, in
	 * the first group, takes every group back out. */
	unsigned g = mods->group < KEYMAP_GROUPS ? mods->group : 0;
	const uint32_t *lv = km->sym[code][g];
	if (!lv[0] && !lv[1] && !lv[2] && !lv[3]) {
		g = 0;
		lv = km->sym[code][0];
	}
	bool second;
	switch ((KeySelect)km->select[code][g]) {
	case KEY_SELECT_ALT:   second = mods->alt;   break;
	case KEY_SELECT_CTRL:  second = mods->ctrl;  break;
	case KEY_SELECT_SUPER: second = mods->super; break;
	default:               second = mods->shift; break;
	}
	const uint32_t sym = lv[second ? 1 : 0];
	const unsigned now = mods->group % groups;
	switch (sym & ~1u) {            /* a _Lock is the one after */
	case KEYSYM_NEXT_GROUP:  *group = (now + 1) % groups;          break;
	case KEYSYM_PREV_GROUP:  *group = (now + groups - 1) % groups; break;
	case KEYSYM_FIRST_GROUP: *group = 0;                           break;
	case KEYSYM_LAST_GROUP:  *group = groups - 1;                  break;
	default:                 return false;
	}
	return true;
}
