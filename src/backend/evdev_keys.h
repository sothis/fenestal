/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * evdev key codes to FnlKey: the one table three backends share. Wayland
 * and KMS deliver these codes as they are and X11 delivers them plus 8, so
 * its translation is this one behind a subtraction.
 *
 * The codes are written out rather than taken from <linux/input-event-codes.h>:
 * on X11 they are the server's numbering -- the XKB `evdev` keycodes, less
 * 8 -- which follows the kernel's by convention and is no part of it.
 *
 * Indexed by code. A code past the end, or one with no row, is
 * FNL_KEY_UNKNOWN -- 0, which is what the gaps in the initialiser hold.
 */

#ifndef FNL_EVDEV_KEYS_H
#define FNL_EVDEV_KEYS_H

#include "core/app.h"

_Static_assert(FNL_KEY_UNKNOWN == 0 && FNL_KEY_KP_ENTER <= UINT8_MAX,
			   "EVDEV_KEYS stores FnlKey in a byte and reads a gap as UNKNOWN");

static const uint8_t EVDEV_KEYS[128] = {
	[1]   = FNL_KEY_ESCAPE,          /* KEY_ESC        */
	[2]   = FNL_KEY_1,               /* KEY_1          */
	[3]   = FNL_KEY_2,
	[4]   = FNL_KEY_3,
	[5]   = FNL_KEY_4,
	[6]   = FNL_KEY_5,
	[7]   = FNL_KEY_6,
	[8]   = FNL_KEY_7,
	[9]   = FNL_KEY_8,
	[10]  = FNL_KEY_9,
	[11]  = FNL_KEY_0,               /* KEY_0          */
	[12]  = FNL_KEY_MINUS,           /* KEY_MINUS      */
	[13]  = FNL_KEY_EQUAL,           /* KEY_EQUAL      */
	[14]  = FNL_KEY_BACKSPACE,       /* KEY_BACKSPACE  */
	[15]  = FNL_KEY_TAB,             /* KEY_TAB        */
	[16]  = FNL_KEY_Q,
	[17]  = FNL_KEY_W,
	[18]  = FNL_KEY_E,
	[19]  = FNL_KEY_R,
	[20]  = FNL_KEY_T,
	[21]  = FNL_KEY_Y,
	[22]  = FNL_KEY_U,
	[23]  = FNL_KEY_I,
	[24]  = FNL_KEY_O,
	[25]  = FNL_KEY_P,
	[26]  = FNL_KEY_LBRACKET,        /* KEY_LEFTBRACE  */
	[27]  = FNL_KEY_RBRACKET,        /* KEY_RIGHTBRACE */
	[28]  = FNL_KEY_ENTER,           /* KEY_ENTER      */
	[29]  = FNL_KEY_LCTRL,           /* KEY_LEFTCTRL   */
	[30]  = FNL_KEY_A,
	[31]  = FNL_KEY_S,
	[32]  = FNL_KEY_D,
	[33]  = FNL_KEY_F,
	[34]  = FNL_KEY_G,
	[35]  = FNL_KEY_H,
	[36]  = FNL_KEY_J,
	[37]  = FNL_KEY_K,
	[38]  = FNL_KEY_L,
	[39]  = FNL_KEY_SEMICOLON,       /* KEY_SEMICOLON  */
	[40]  = FNL_KEY_APOSTROPHE,      /* KEY_APOSTROPHE */
	[41]  = FNL_KEY_GRAVE,           /* KEY_GRAVE      */
	[42]  = FNL_KEY_LSHIFT,          /* KEY_LEFTSHIFT  */
	[43]  = FNL_KEY_BACKSLASH,       /* KEY_BACKSLASH  */
	[44]  = FNL_KEY_Z,
	[45]  = FNL_KEY_X,
	[46]  = FNL_KEY_C,
	[47]  = FNL_KEY_V,
	[48]  = FNL_KEY_B,
	[49]  = FNL_KEY_N,
	[50]  = FNL_KEY_M,
	[51]  = FNL_KEY_COMMA,           /* KEY_COMMA      */
	[52]  = FNL_KEY_PERIOD,          /* KEY_DOT        */
	[53]  = FNL_KEY_SLASH,           /* KEY_SLASH      */
	[54]  = FNL_KEY_RSHIFT,          /* KEY_RIGHTSHIFT */
	[55]  = FNL_KEY_KP_MULTIPLY,     /* KEY_KPASTERISK */
	[56]  = FNL_KEY_LALT,            /* KEY_LEFTALT    */
	[57]  = FNL_KEY_SPACE,           /* KEY_SPACE      */
	[58]  = FNL_KEY_CAPS_LOCK,       /* KEY_CAPSLOCK   */
	[59]  = FNL_KEY_F1,              /* KEY_F1         */
	[60]  = FNL_KEY_F2,
	[61]  = FNL_KEY_F3,
	[62]  = FNL_KEY_F4,
	[63]  = FNL_KEY_F5,
	[64]  = FNL_KEY_F6,
	[65]  = FNL_KEY_F7,
	[66]  = FNL_KEY_F8,
	[67]  = FNL_KEY_F9,
	[68]  = FNL_KEY_F10,             /* KEY_F10        */
	[69]  = FNL_KEY_NUM_LOCK,        /* KEY_NUMLOCK    */
	[70]  = FNL_KEY_SCROLL_LOCK,     /* KEY_SCROLLLOCK */
	[71]  = FNL_KEY_KP_7,            /* KEY_KP7        */
	[72]  = FNL_KEY_KP_8,
	[73]  = FNL_KEY_KP_9,
	[74]  = FNL_KEY_KP_SUBTRACT,     /* KEY_KPMINUS    */
	[75]  = FNL_KEY_KP_4,
	[76]  = FNL_KEY_KP_5,
	[77]  = FNL_KEY_KP_6,
	[78]  = FNL_KEY_KP_ADD,          /* KEY_KPPLUS     */
	[79]  = FNL_KEY_KP_1,
	[80]  = FNL_KEY_KP_2,
	[81]  = FNL_KEY_KP_3,
	[82]  = FNL_KEY_KP_0,            /* KEY_KP0        */
	[83]  = FNL_KEY_KP_DECIMAL,      /* KEY_KPDOT      */
	[86]  = FNL_KEY_INTL_BACKSLASH,  /* KEY_102ND      */
	[87]  = FNL_KEY_F11,             /* KEY_F11        */
	[88]  = FNL_KEY_F12,             /* KEY_F12        */
	[96]  = FNL_KEY_KP_ENTER,        /* KEY_KPENTER    */
	[97]  = FNL_KEY_RCTRL,           /* KEY_RIGHTCTRL  */
	[98]  = FNL_KEY_KP_DIVIDE,       /* KEY_KPSLASH    */
	[99]  = FNL_KEY_PRINT_SCREEN,    /* KEY_SYSRQ      */
	[100] = FNL_KEY_RALT,            /* KEY_RIGHTALT   */
	[102] = FNL_KEY_HOME,            /* KEY_HOME       */
	[103] = FNL_KEY_UP,              /* KEY_UP         */
	[104] = FNL_KEY_PAGE_UP,         /* KEY_PAGEUP     */
	[105] = FNL_KEY_LEFT,            /* KEY_LEFT       */
	[106] = FNL_KEY_RIGHT,           /* KEY_RIGHT      */
	[107] = FNL_KEY_END,             /* KEY_END        */
	[108] = FNL_KEY_DOWN,            /* KEY_DOWN       */
	[109] = FNL_KEY_PAGE_DOWN,       /* KEY_PAGEDOWN   */
	[110] = FNL_KEY_INSERT,          /* KEY_INSERT     */
	[111] = FNL_KEY_DELETE,          /* KEY_DELETE     */
	[119] = FNL_KEY_PAUSE,           /* KEY_PAUSE      */
	[125] = FNL_KEY_LSUPER,          /* KEY_LEFTMETA   */
	[126] = FNL_KEY_RSUPER,          /* KEY_RIGHTMETA  */
	[127] = FNL_KEY_MENU,            /* KEY_COMPOSE    */
};

static inline FnlKey key_from_evdev(uint32_t code)
{
	return code < sizeof EVDEV_KEYS ? (FnlKey)EVDEV_KEYS[code]
									: FNL_KEY_UNKNOWN;
}

#endif /* FNL_EVDEV_KEYS_H */
