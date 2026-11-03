/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Raw input on a thread of its own: when each key and mouse report reached
 * the process, for fnl_input_latency_ms(), and a captured pointer's motion,
 * handed to the window's thread. See win32_rawin.c.
 */

#ifndef FNL_WIN32_RAWIN_H
#define FNL_WIN32_RAWIN_H

#include "backend/win32/win32_state.h"

struct RawIn;

/* A captured pointer's raw report, and now_seconds() as it arrived. */
typedef struct {
	RAWMOUSE m;
	double   t;
} RawForward;

/* The thread, its window and whichever of the process's raw keyboard and
 * mouse the program has not registered for itself; NULL where none of them
 * will be had. The window's messages then keep their GetMessageTime()
 * alone, and a capture registers raw input on the window itself. `target`
 * is the window forwarded reports are posted to, as FNL_WM_RAW. And the
 * stop, which gives back only what is still the thread's. */
struct RawIn *rawin_start(HINSTANCE instance, HWND target);
void rawin_stop(struct RawIn *r);
/* Whether the thread holds the raw mouse now, and so whether a capture's
 * motion is forwarded from it: not once the program has registered its
 * own. Window thread. */
bool rawin_has_mouse(const struct RawIn *r);

/* Whether the thread hands mouse reports on: while the pointer is
 * captured. And the ones handed on since the last call, oldest first, up
 * to `max`; how many. Window thread. */
void rawin_forward(struct RawIn *r, bool on);
int rawin_forwarded(struct RawIn *r, RawForward *out, int max);

/* The age, in seconds, of the report the message being handled stands for:
 * a key by its scancode, extended bit and direction, a button's or the
 * wheel's transition by its RI_MOUSE_* flag. `tick_age` is the message's
 * own, GetMessageTime()'s, which picks the report out among those alike.
 * Negative where none matches. Window thread. */
double rawin_key(struct RawIn *r, unsigned scancode, bool extended, bool up,
				 double tick_age);
double rawin_button(struct RawIn *r, unsigned flag, double tick_age);
/* The movements a WM_MOUSEMOVE stands for: their ages summed, the oldest
 * and how many; false for none. And every movement spent unread, while the
 * pointer is captured and its motion is forwarded instead. Window
 * thread. */
bool rawin_moves(struct RawIn *r, double tick_age, double *sum,
				 double *oldest, uint32_t *n);
void rawin_moves_spend(struct RawIn *r);
/* How many inputs were matched to their reports, and how many messages
 * found none and kept the tick's age: whether the matching holds on a
 * machine, which --frame-times says at close. Window thread. */
void rawin_tally(const struct RawIn *r, uint32_t *matched,
				 uint32_t *missed);

#endif /* FNL_WIN32_RAWIN_H */
