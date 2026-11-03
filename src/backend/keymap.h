/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What a key prints: the layout, for the three backends that apply it
 * themselves -- X11 and Wayland, whose servers hand over keysyms and leave
 * the client to, and KMS, which has no server. DESIGN *Text entry* has the
 * reasoning and what is left out.
 *
 * It is not an XKB implementation. A key's *type* is read for two things
 * alone: how many levels it keeps, and, for a group key, which modifier
 * selects its second level (KeySelect). Otherwise the four levels are taken
 * to be plain, Shift, AltGr and AltGr+Shift, and Caps Lock applies to a
 * level pair that is a letter in two cases and to nothing else. Dead keys
 * are composed here; the Compose key's sequences are not.
 *
 * Indexed by X keycode on all three, which for Wayland and KMS is the evdev
 * code plus 8: an XKB keymap numbers its keys that way wherever it came
 * from.
 */

#ifndef FNL_KEYMAP_H
#define FNL_KEYMAP_H

#include "core/app.h"

#define KEYMAP_CODES  256
#define KEYMAP_GROUPS 4
#define KEYMAP_LEVELS 4

/* A character with no keysym of its own has one all the same: its code point
 * plus this. */
#define KEYSYM_UNICODE 0x01000000u
/* VoidSymbol: a level a layout filled on purpose with nothing, so that what
 * it was given before stays covered. Types nothing, as its number says. */
#define KEYSYM_VOID    0xffffffu

/* The modifier that selects a key's second level, by its type: Shift for
 * every type but the three xkeyboard-config's group options use to put
 * ISO_Next_Group under another -- Alt+Shift, Ctrl+Shift, Super+Space. Read
 * only for a group key (keymap_group_key()); typing goes by the four-level
 * rule above. Zero is Shift, so an empty map is the rule. */
typedef enum {
	KEY_SELECT_SHIFT, KEY_SELECT_ALT, KEY_SELECT_CTRL, KEY_SELECT_SUPER,
} KeySelect;

/* Keysyms, by [keycode][group][level]. Zero is no symbol. And each key's
 * KeySelect, by [keycode][group]. */
typedef struct {
	uint32_t sym[KEYMAP_CODES][KEYMAP_GROUPS][KEYMAP_LEVELS];
	uint8_t  select[KEYMAP_CODES][KEYMAP_GROUPS];
} Keymap;

/* The modifiers in force, reduced to what the lookup asks. Each backend fills
 * this from its own mask: which bit is Num Lock or AltGr differs. */
typedef struct {
	bool     shift, lock, num, level3;
	/* Ctrl, Alt or Super is down: this is a shortcut, not text. */
	bool     command;
	unsigned group;
	/* Each of the three apart, for a group key's KeySelect alone; a backend
	 * whose server keeps the group leaves them false. */
	bool     alt, ctrl, super;
} KeyMods;

/* The code point a keysym stands for, or 0 for one that is not a character. */
uint32_t keysym_to_ucs(uint32_t sym);

/* A dead key waiting for its letter. Zeroed is none. */
typedef struct {
	uint32_t dead;
} KeyCompose;

/* What pressing the key at `code` types under `mods`, as a code point, or 0
 * for nothing. Control characters come back as they are; filtering them is
 * the core's.
 *
 * Dead keys are worked through `kc`: an accent key types nothing and is held,
 * and the next character comes back composed with it, or alone where the two
 * do not compose. A space, or the same accent again, types the accent's own
 * spacing form. Modifiers, shortcuts and a *repeat* of the accent key leave a
 * held accent alone: Shift has to be pressable between an accent and its
 * capital, and a held accent key must not count as pressed twice. */
uint32_t keymap_type(const Keymap *km, KeyCompose *kc, unsigned code,
					 const KeyMods *mods, bool repeat);

/* The held accent as something to show, UTF-8 and terminated, or the empty
 * string when none is held or it has no spacing form. */
void keymap_composing(const KeyCompose *kc, char out[8]);

/* Whether the layout lets a held key at `code` repeat: not where a group's
 * first level is a modifier, a lock or an ISO level or group key, as an X
 * server holding the compat bindings that say so repeats none of those. */
bool keymap_key_repeats(const Keymap *km, unsigned code);

/* Whether a held key at `code`, `k` to FnlKey, repeats its text: by its
 * name where FnlKey has one (app_key_repeats()), and FNL_KEY_UNKNOWN by the
 * layout, which is all there is to go by -- a yen key repeats as a letter
 * does. For the backends that decide repeats themselves: Wayland, which
 * times its own, and KMS, whose kernel repeats every key. Inline, so the
 * tools that link this file alone never reach for the core. */
static inline bool keymap_held_repeats(const Keymap *km, unsigned code,
									   FnlKey k)
{
	return k == FNL_KEY_UNKNOWN ? keymap_key_repeats(km, code)
								: app_key_repeats(k);
}

/* How many groups the layout fills: one past the highest group any key has
 * a symbol in, and at least one. */
unsigned keymap_groups(const Keymap *km);

/* Whether a press of the key at `code` is a group key -- ISO_Next_Group,
 * ISO_Prev_Group, ISO_First_Group, ISO_Last_Group or a _Lock of one -- on
 * the level `mods` select by its KeySelect, in group mods->group; and, when
 * it is, the group it moves to, of `groups`, into *group. XKB binds each to
 * a group lock, so every one of them locks; ISO_Group_Shift and
 * Mode_switch, which latch while held, are not acted on. For the backend
 * with no server to keep the group: KMS. */
bool keymap_group_key(const Keymap *km, unsigned code, const KeyMods *mods,
					  unsigned groups, unsigned *group);

/* ---- backend/keymap_xkb.c: only where a keymap arrives as text ----------- */

/* A keysym by the name an XKB keymap spells it with, or 0. The name is not
 * terminated: it is a token inside somebody else's text. */
uint32_t keysym_from_name(const char *name, size_t len);

/* Where an `include` met inside a section is read from: xkeyboard-config's
 * data root (keymap_data_root()), whose keycodes/ and symbols/ files
 * include one another. Each file read stays in `files` for the parse, since
 * a nested include is parsed from inside the file that names it; one lent
 * (xkb_source_lend) is held by the caller instead, which the verifier and
 * the fuzz harness use in place of a disk. `missing` is the first piece
 * that contributed nothing -- a file not there, a section not in it, an
 * include too deep -- for the caller's log line, and is empty when none;
 * `top_miss` is the first such piece of the chain itself, where a
 * layout's own file not found is no layout at all. */
#define XKB_SOURCE_FILES 32

typedef struct {
	char   *path;           /* "symbols/de", as named under the root */
	char   *text;
	size_t  len;
	bool    lent;           /* the caller's, not freed here */
} XkbFile;

typedef struct {
	const char *root;       /* NULL: lent files alone */
	XkbFile     files[XKB_SOURCE_FILES];
	size_t      file_count;
	size_t      bytes;      /* read so far, against XKB_BYTES_MAX */
	size_t      pieces;     /* sections included, against XKB_PIECES_MAX */
	char        missing[96];
	char        top_miss[96];
} XkbSource;

void xkb_source_lend(XkbSource *src, const char *path, const char *text,
					 size_t len);
void xkb_source_free(XkbSource *src);

/* A whole file of at most `budget` bytes, terminated, or NULL. */
char *xkb_read_file(const char *path, size_t budget, size_t *len);

/* Fill a keymap from XKB's text format, as a Wayland compositor sends it.
 * False when nothing usable was found, which leaves the map empty and text
 * entry silent rather than wrong. `text` need not be terminated. With no
 * source an include is skipped, as a compositor's text has every one
 * resolved already; with one, `include "pc+de(nodeadkeys)"` inside a
 * section reads those files' sections into the same map, each file's own
 * keys landing over what it included. */
bool keymap_parse_xkb(Keymap *km, const char *text, size_t len);
bool keymap_parse_xkb_from(Keymap *km, const char *text, size_t len,
						   XkbSource *src);

/* ---- backend/keymap_rules.c: the layout by name, from the data files ----
 *
 * For the backend with no server to hand a keymap over. Built for KMS
 * alone. */

/* The four names a keyboard is described by, as the console's files and
 * setxkbmap spell them: "de,us" and "nodeadkeys," for two groups. */
typedef struct {
	char model[32];
	char layout[64];
	char variant[64];
	char options[128];
} KeymapNames;

/* pc105, us, and nothing else: what the names are before a file fills them,
 * which is also xkeyboard-config's own default. */
void keymap_names_default(KeymapNames *n);

/* XKBLAYOUT and its three siblings from `NAME=value` text -- Debian's
 * /etc/default/keyboard, systemd's /etc/vconsole.conf -- over the
 * defaults. False, and the defaults, when it names no layout. */
bool keymap_names_from_conf(KeymapNames *n, const char *text, size_t len);

/* The console's own names: the first of those two files that names a
 * layout, or vconsole.conf's KEYMAP through systemd's kbd-model-map. False,
 * and the defaults, when nothing is configured. */
bool keymap_names_console(KeymapNames *n);

/* `de(nodeadkeys),us caps:none`: layouts with their variants, up to four
 * comma-separated for as many groups, and the XKB options after a space.
 * False, and the defaults, for text of another shape. */
bool keymap_names_parse(KeymapNames *n, const char *spec);

/* The keycodes and symbols components the rules name for `n` --
 * "evdev+aliases(qwertz)", "pc+de(nodeadkeys)+inet(evdev)" -- read from
 * rules/evdev's text, which need not be terminated. False when the rules
 * name no symbols, where the two are what every PC model's rows give. */
bool keymap_rules_resolve(const char *rules, size_t len, const KeymapNames *n,
						  char *keycodes, size_t keycodes_size,
						  char *symbols, size_t symbols_size);

/* Where the data is: XKB_CONFIG_ROOT, else /usr/share/X11/xkb, else
 * /usr/share/xkeyboard-config-2, the first with a symbols directory; NULL
 * with none. */
const char *keymap_data_root(void);

/* The keymap for those names from the files under `root`, or under
 * keymap_data_root() for NULL. True with each layout's file found and a
 * key that types; `detail` then holds what was assumed or left out -- a
 * rules file not there, the first include that contributed nothing -- or
 * nothing. False with the map empty and `detail` the reason. */
bool keymap_load_names(Keymap *km, const KeymapNames *n, const char *root,
					   char *detail, size_t detail_len);

#endif /* FNL_KEYMAP_H */
