/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The X11 backend's keyboard: which key an event names, and what it types.
 */

#include "core/app.h"

#include "backend/x11/x11_state.h"
#include "backend/evdev_keys.h"

/* ---- keys -----------------------------------------------------------------
 *
 * An X11 keycode is the evdev code plus 8 under the XKB "evdev" ruleset, so the
 * table is the Wayland backend's behind a subtraction. A server whose keymap
 * is not evdev's -- xfree86's keycodes, or a VNC server's own -- numbers the
 * same keys otherwise, and there the subtraction names the wrong ones. What
 * every XKB keymap has instead is a name for each key, which says where it
 * sits and not what it types: <AC01> is the home row's first whatever the
 * layout or the keycode. So the names are read (x11_key_names_ask()) and
 * each is the evdev code of the key that bears it, through this table, which
 * is xkeyboard-config's keycodes/evdev for the keys EVDEV_KEYS has, plus the
 * one name another keycodes file uses for them (<MENU>). Matching on
 * keysyms instead would make FnlKey follow the layout, which it exists not
 * to do. A server without XKB has no names and keeps the subtraction. */
static const struct { char name[5]; uint8_t code; } KEY_NAME_CODES[] = {
	{ "AB01", 44 }, { "AB02", 45 }, { "AB03", 46 }, { "AB04", 47 },
	{ "AB05", 48 }, { "AB06", 49 }, { "AB07", 50 }, { "AB08", 51 },
	{ "AB09", 52 }, { "AB10", 53 }, { "AC01", 30 }, { "AC02", 31 },
	{ "AC03", 32 }, { "AC04", 33 }, { "AC05", 34 }, { "AC06", 35 },
	{ "AC07", 36 }, { "AC08", 37 }, { "AC09", 38 }, { "AC10", 39 },
	{ "AC11", 40 }, { "AD01", 16 }, { "AD02", 17 }, { "AD03", 18 },
	{ "AD04", 19 }, { "AD05", 20 }, { "AD06", 21 }, { "AD07", 22 },
	{ "AD08", 23 }, { "AD09", 24 }, { "AD10", 25 }, { "AD11", 26 },
	{ "AD12", 27 }, { "AE01", 2 }, { "AE02", 3 }, { "AE03", 4 }, { "AE04", 5 },
	{ "AE05", 6 }, { "AE06", 7 }, { "AE07", 8 }, { "AE08", 9 }, { "AE09", 10 },
	{ "AE10", 11 }, { "AE11", 12 }, { "AE12", 13 }, { "BKSL", 43 },
	{ "BKSP", 14 }, { "CAPS", 58 }, { "COMP", 127 }, { "DELE", 111 },
	{ "DOWN", 108 }, { "END", 107 }, { "ESC", 1 }, { "FK01", 59 },
	{ "FK02", 60 }, { "FK03", 61 }, { "FK04", 62 }, { "FK05", 63 },
	{ "FK06", 64 }, { "FK07", 65 }, { "FK08", 66 }, { "FK09", 67 },
	{ "FK10", 68 }, { "FK11", 87 }, { "FK12", 88 }, { "HOME", 102 },
	{ "INS", 110 }, { "KP0", 82 }, { "KP1", 79 }, { "KP2", 80 }, { "KP3", 81 },
	{ "KP4", 75 }, { "KP5", 76 }, { "KP6", 77 }, { "KP7", 71 }, { "KP8", 72 },
	{ "KP9", 73 }, { "KPAD", 78 }, { "KPDL", 83 }, { "KPDV", 98 },
	{ "KPEN", 96 }, { "KPMU", 55 }, { "KPSU", 74 }, { "LALT", 56 },
	{ "LCTL", 29 }, { "LEFT", 105 }, { "LFSH", 42 }, { "LSGT", 86 },
	{ "LWIN", 125 }, { "MENU", 127 }, { "NMLK", 69 }, { "PAUS", 119 },
	{ "PGDN", 109 }, { "PGUP", 104 }, { "PRSC", 99 }, { "RALT", 100 },
	{ "RCTL", 97 }, { "RGHT", 106 }, { "RTRN", 28 }, { "RTSH", 54 },
	{ "RWIN", 126 }, { "SCLK", 70 }, { "SPCE", 57 }, { "TAB", 15 },
	{ "TLDE", 41 }, { "UP", 103 },
};

void x11_keys_from_names(struct BackendState *p, const uint8_t *names,
						 int first, int count)
{
	uint8_t of[256] = { 0 };
	int named = 0;
	for (int i = 0; i < count && first + i < 256; i++) {
		const uint8_t *n = names + (size_t)i * 4;
		for (size_t j = 0; j < sizeof KEY_NAME_CODES / sizeof *KEY_NAME_CODES;
			 j++) {
			/* Four bytes, NUL-padded when the name is shorter. */
			if (strncmp((const char *)n, KEY_NAME_CODES[j].name, 4) == 0) {
				of[first + i] = (uint8_t)key_from_evdev(KEY_NAME_CODES[j].code);
				named += of[first + i] != FNL_KEY_UNKNOWN;
				break;
			}
		}
	}
	/* A keymap that names none of these keys says nothing about them; the
	 * subtraction is still the better guess. */
	p->key_names = named > 0;
	if (p->key_names)
		memcpy(p->key_of, of, sizeof of);
}

static FnlKey key_from_keycode(const struct BackendState *p, xcb_keycode_t kc)
{
	if (p->key_names)
		return (FnlKey)p->key_of[kc];
	return kc < 8 ? FNL_KEY_UNKNOWN : key_from_evdev((uint32_t)kc - 8u);
}

/* After the server said the keyboard changed: its names may have too. */
static void key_names_fresh(struct BackendState *p)
{
	if (p->key_names_stale)
		x11_key_names_take(p, x11_key_names_ask(p));
}

static bool key_bit_get(const struct BackendState *p, xcb_keycode_t kc)
{
	return (p->key_down[kc >> 3] >> (kc & 7)) & 1u;
}

static void key_bit_set(struct BackendState *p, xcb_keycode_t kc, bool down)
{
	const uint8_t mask = (uint8_t)(1u << (kc & 7));
	if (down)
		p->key_down[kc >> 3] |= mask;
	else
		p->key_down[kc >> 3] &= (uint8_t)~mask;
}

/* The keys already down as focus arrives, which fenestal.h promises: alt-tab in
 * with W held and the character is already walking. */
void x11_keys_seed(App *a)
{
	struct BackendState *p = a->backend_state;

	xcb_query_keymap_reply_t *r =
		xcb_query_keymap_reply(p->conn, xcb_query_keymap(p->conn), NULL);
	if (!r)
		return;

	key_names_fresh(p);
	memcpy(p->key_down, r->keys, sizeof p->key_down);
	for (int i = 0; i < 256; i++) {
		if (!((r->keys[i >> 3] >> (i & 7)) & 1u))
			continue;
		/* Through app_key_seed() and not app_key(): a key that was already
		 * down must not fire a binding, or alt-tabbing in with Escape held
		 * closes the window. */
		app_key_seed(a, key_from_keycode(p, (xcb_keycode_t)i));
	}
	free(r);
}

/* ---- the layout -----------------------------------------------------------
 *
 * What a key types, for FNL_EVENT_TEXT. From the *core* protocol and nothing
 * else: GetKeyboardMapping is the keysym table and GetModifierMapping says
 * which keys are on which modifier bit. An XKB server keeps the core table in
 * step with the layout, and lays it out to a fixed rule:
 *
 *     column   0     1     2     3     4     5     6     7
 *     means    G1L1  G1L2  G2L1  G2L2  G1L3  G1L4  G2L3  G2L4
 *
 * two groups of up to four levels, with the group in force in bits 13 and 14
 * of every key event's state. A third or fourth layout has no column, and a
 * key typed under one falls back to the first; that is the core protocol's
 * ceiling rather than something this could read harder to get past.
 *
 * UNVERIFIED on a server without XKB, where the second pair of columns is
 * reached through Mode_switch instead. */
#define KEYSYM_LEVEL3_SHIFT 0xfe03u     /* ISO_Level3_Shift: AltGr */
#define KEYSYM_MODE_SWITCH  0xff7eu
#define KEYSYM_NUM_LOCK     0xff7fu

/* The group in force, in a key event's state. */
#define STATE_GROUP_SHIFT 13
#define STATE_GROUP_MASK  3u

/* The layout, asked for: the keysym table and the modifier map, neither of
 * which waits on the other. Nothing is sent for a server that reports no
 * keycodes, and x11_keymap_take() knows it. */
X11KeymapAsk x11_keymap_ask(struct BackendState *p)
{
	const xcb_setup_t *setup = xcb_get_setup(p->conn);
	const int lo = setup->min_keycode, hi = setup->max_keycode;
	X11KeymapAsk ask = { .sent = hi >= lo };

	if (ask.sent) {
		ask.map = xcb_get_keyboard_mapping(p->conn, (xcb_keycode_t)lo,
										   (uint8_t)(hi - lo + 1));
		ask.mod = xcb_get_modifier_mapping(p->conn);
	}
	return ask;
}

void x11_keymap_take(struct BackendState *p, X11KeymapAsk ask)
{
	static const uint8_t COLUMN[8][2] = {
		{ 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 },
		{ 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 },
	};
	const xcb_setup_t *setup = xcb_get_setup(p->conn);
	const int lo = setup->min_keycode, hi = setup->max_keycode;

	/* Asked for again only by the next key that types: a layout change
	 * arrives as a burst of events, and each load is a round trip on the
	 * thread that pumps input. */
	p->keymap_stale = false;
	if (!ask.sent)
		return;

	xcb_get_keyboard_mapping_reply_t *r =
		xcb_get_keyboard_mapping_reply(p->conn, ask.map, NULL);
	xcb_get_modifier_mapping_reply_t *m =
		xcb_get_modifier_mapping_reply(p->conn, ask.mod, NULL);
	if (!r) {
		/* The table already held is kept: a layout one change old types
		 * better than none. */
		fnl_log(FNL_LOG_WARN, "the server sent no keyboard mapping; text "
							  "entry keeps the layout it had");
		free(m);
		return;
	}
	memset(&p->keymap, 0, sizeof p->keymap);
	p->mod_num = p->mod_level3 = p->mod_switch = 0;
	const int per = r->keysyms_per_keycode;
	const xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(r);
	const int total = xcb_get_keyboard_mapping_keysyms_length(r);
	/* The reply's own length decides how many rows are read, not the range
	 * that was asked for. */
	for (int kc = lo; per > 0 && kc <= hi && kc - lo < total / per; kc++)
		for (int c = 0; c < per && c < 8; c++)
			p->keymap.sym[kc][COLUMN[c][0]][COLUMN[c][1]] =
				syms[(kc - lo) * per + c];
	free(r);

	/* Eight modifiers, `per_mod` keycodes each, zero where a slot is unused.
	 * Shift, Lock and Control are the first three and need no finding. */
	if (!m)
		return;
	const int per_mod = m->keycodes_per_modifier;
	const xcb_keycode_t *kcs = xcb_get_modifier_mapping_keycodes(m);
	const int count = xcb_get_modifier_mapping_keycodes_length(m);
	for (int mod = 3; mod < 8; mod++)
		for (int i = 0; i < per_mod && mod * per_mod + i < count; i++) {
			const xcb_keycode_t kc = kcs[mod * per_mod + i];
			if (!kc)
				continue;
			for (int g = 0; g < KEYMAP_GROUPS; g++)
				for (int l = 0; l < KEYMAP_LEVELS; l++) {
					const uint32_t sym = p->keymap.sym[kc][g][l];
					const uint16_t bit = (uint16_t)(1u << mod);
					if (sym == KEYSYM_NUM_LOCK)
						p->mod_num |= bit;
					else if (sym == KEYSYM_LEVEL3_SHIFT)
						p->mod_level3 |= bit;
					else if (sym == KEYSYM_MODE_SWITCH)
						p->mod_switch |= bit;
				}
		}
	free(m);
}

static KeyMods key_mods(const struct BackendState *p, uint16_t state)
{
	unsigned group = (state >> STATE_GROUP_SHIFT) & STATE_GROUP_MASK;
	if (!group && (state & p->mod_switch))
		group = 1;
	/* Alt is Mod1 and Super is Mod4 on every server this has met. Whatever
	 * was found to be AltGr, Num Lock or the group switch is taken back out,
	 * so a layout that put one of them on those bits still types. */
	const uint16_t command =
		(uint16_t)((XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_4) &
				   ~(p->mod_num | p->mod_level3 | p->mod_switch));
	return (KeyMods){
		.shift   = (state & XCB_MOD_MASK_SHIFT) != 0,
		.lock    = (state & XCB_MOD_MASK_LOCK) != 0,
		.num     = (state & p->mod_num) != 0,
		.level3  = (state & p->mod_level3) != 0,
		.command = (state & command) != 0,
		.group   = group,
	};
}

/* The state a key event carries is the modifiers as they were just before it,
 * which is the set that applies to it. */
static void key_text(App *a, const xcb_key_press_event_t *e, bool repeat)
{
	struct BackendState *p = a->backend_state;
	if (!app_text_input(a))
		return;
	if (p->keymap_stale)
		x11_keymap_take(p, x11_keymap_ask(p));
	const KeyMods mods = key_mods(p, e->state);
	/* Through the composer: an accent key is held there and comes back on the
	 * letter that follows. What it holds is reported as the composition, so a
	 * text field can show the accent waiting. */
	const uint32_t c = keymap_type(&p->keymap, &p->compose, e->detail, &mods,
								   repeat);
	char held[8];
	keymap_composing(&p->compose, held);
	app_text(a, c);
	app_compose_set(a, held);
}

/* No input method to enable here, but a field that closed mid-accent must not
 * leave the accent for the next one. */
void x11_text_input_changed(App *a)
{
	if (app_text_input(a))
		return;
	a->backend_state->compose.dead = 0;
	app_compose_set(a, NULL);
}

void x11_on_key(App *a, const xcb_key_press_event_t *e, bool pressed)
{
	struct BackendState *p = a->backend_state;
	const xcb_keycode_t kc = e->detail;
	key_names_fresh(p);
	const FnlKey key = key_from_keycode(p, kc);

	if (pressed) {
		/* A server-generated repeat, where detectable auto-repeat was refused:
		 * a KeyRelease followed by a KeyPress carrying the *same timestamp*,
		 * which is what tells the pair from a key let go and pressed again.
		 * UNVERIFIED.
		 *
		 * The release has already reached the caller. The press must not reach
		 * app_key(), or a binding re-fires; app_key_seed() puts the held bit
		 * back without acting on it. */
		if (!p->detectable_repeat && kc == p->repeat_kc &&
			e->time == p->repeat_time) {
			key_bit_set(p, kc, true);
			app_key_seed(a, key);
			app_key_repeat(a, key);
			key_text(a, e, true);
			return;
		}
		/* Already down: a repeat under detectable auto-repeat, a press with no
		 * release before it. No binding fires and the held set does not move; a
		 * text field takes it as a held key. */
		if (key_bit_get(p, kc)) {
			app_key_repeat(a, key);
			key_text(a, e, true);
			return;
		}
	} else {
		p->repeat_kc   = kc;
		p->repeat_time = e->time;
	}

	key_bit_set(p, kc, pressed);
	app_key(a, key, pressed);
	if (pressed)
		key_text(a, e, false);
}
