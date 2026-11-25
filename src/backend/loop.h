/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The event loop's plumbing: the descriptor set the pump blocks in, the
 * wakeup another thread or a signal handler writes, and how long a pump may
 * block while something is owed. What the backends with no character
 * delivery of their own share and none owns.
 *
 * The set is level-triggered on purpose: neither the Wayland nor the X11
 * backend's read drains its socket to EAGAIN, and a descriptor with data
 * still buffered must go on being reported. Linux's epoll and FreeBSD's
 * poll both are.
 *
 * Inline, so that this is a header a backend includes and not a source file
 * a build has to list. FreeBSD's poll() set lives in loop_bsd.c, which does
 * need listing.
 */

#ifndef FNL_LOOP_H
#define FNL_LOOP_H

#include "core/app.h"

/* What a wait found on a descriptor. The four are Linux's EPOLLIN/OUT/ERR/
 * HUP as FreeBSD's poll reports them; a backend tests the ones it cares
 * about. */
typedef struct {
	int      fd;
	unsigned events;
} LoopEvent;

#define LOOP_IN  (1u << 0)
#define LOOP_OUT (1u << 1)
#define LOOP_ERR (1u << 2)
#define LOOP_HUP (1u << 3)

/* One fd's worth of readiness in the wait's answer; the caller's array is
 * at most LOOP_EVENTS. */
#define LOOP_EVENTS 16

typedef struct Loop Loop;

/* An eventfd, or a pipe where the system has none: the two ends are one
 * descriptor on Linux and two on FreeBSD, because there a pipe end cannot
 * write to itself. Written from signal handlers, so nothing here but
 * write() and read(). */
typedef struct {
	int fd;
	int wfd;
} LoopWake;

#if defined(__linux__)
#include "backend/loop_linux.h"
#elif defined(__FreeBSD__)
#include "backend/loop_bsd.h"
#else
#error "the event loop's plumbing is Linux's or FreeBSD's"
#endif

/* How long the pump may block, in epoll_wait/poll milliseconds: for ever
 * unless a deadline is owed, and then until the nearer of the two, rounded
 * up so the wait cannot end a fraction of a millisecond early and spin. A
 * negative deadline is none; zero or a past one is no wait at all. */
static inline int loop_wait_ms(double a, double b)
{
	double left = 0.0;
	bool any = false;
	if (a >= 0.0) {
		left = a - now_seconds();
		any  = true;
	}
	if (b >= 0.0) {
		const double other = b - now_seconds();
		if (!any || other < left)
			left = other;
		any = true;
	}
	if (!any)
		return -1;
	return left > 0.0 ? (int)(left * 1e3) + 1 : 0;
}

/* An input's stamp, on CLOCK_MONOTONIC in microseconds -- the clock the
 * compositors, the X server and evdev (EVIOCSCLOCKID) stamp input on, and
 * the one now_seconds() reads here -- as the age app_input_age() takes. A
 * stamp on another clock is an age past INPUT_AGE_MAX, or below zero, and
 * is taken for none. */
static inline void loop_input_us(App *a, uint64_t us)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const uint64_t now =
		(uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
	app_input_age(a, now >= us ? (double)(now - us) * 1e-6 : -1.0);
}

/* Wayland's and the X server's: whole milliseconds of that clock, wrapping
 * in 32 bits, so the age is the difference in 32 bits. The stamp is the
 * millisecond the input fell in, so it is taken from that millisecond's
 * middle. */
static inline void loop_input_ms(App *a, uint32_t ms)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const uint64_t now_us =
		(uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
	const int32_t d = (int32_t)((uint32_t)(now_us / 1000u) - ms);
	app_input_age(
		a, d < 0 ? -1.0 : d * 1e-3 + (double)(now_us % 1000u) * 1e-6 - 0.5e-3);
}

#endif /* FNL_LOOP_H */
