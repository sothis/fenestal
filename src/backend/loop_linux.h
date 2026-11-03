/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Linux event loop's plumbing: the eventfd that wakes a blocked pump, the
 * epoll set it sits in beside the window server's socket, and how long a pump
 * may block while a paste is owed. What the three Linux backends share and
 * none owns; eventfd and epoll are Linux rather than merely POSIX.
 *
 * Inline, so that this is a header a backend includes and not a source file a
 * build has to list.
 */

#ifndef FNL_LOOP_LINUX_H
#define FNL_LOOP_LINUX_H

#include "core/app.h"

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

/* An eventfd for the epoll set: another thread has no other way to break the
 * pump out of an indefinite wait. A plain counter, so one descriptor covers
 * both directions where a self-pipe would need two, and draining it is a
 * single read. Non-blocking, so neither side can stall on the other. */
static inline FNL_MUST_USE FnlResult loop_wake_create(int *wake_fd)
{
	*wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (*wake_fd < 0)
		return fnl_fail(FNL_ERR_SYSTEM, "eventfd() failed: %s",
						strerror(errno));
	return FNL_OK;
}

/* Called from a signal handler, so it is a write() and nothing else. EAGAIN
 * means the counter is saturated, which is a wakeup pending already. */
static inline void loop_wake(int wake_fd)
{
	const uint64_t one = 1;
	ssize_t n = write(wake_fd, &one, sizeof one);
	(void)n;
}

/* One read empties the counter. */
static inline void loop_wake_drain(int wake_fd)
{
	uint64_t drain;
	ssize_t got = read(wake_fd, &drain, sizeof drain);
	(void)got;
}

/* An epoll set watching the server's socket and the wakeup. Level-triggered
 * on purpose: a descriptor with data still buffered must go on being
 * reported, because neither the Wayland nor the X11 backend's read drains
 * its socket to EAGAIN. */
static inline FNL_MUST_USE FnlResult loop_epoll_create(int *epoll_fd,
													   int server_fd,
													   int wake_fd)
{
	*epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (*epoll_fd < 0)
		return fnl_fail(FNL_ERR_SYSTEM, "epoll_create1() failed: %s",
						strerror(errno));

	const int fds[2] = { server_fd, wake_fd };
	for (int i = 0; i < 2; i++) {
		struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[i] };
		if (epoll_ctl(*epoll_fd, EPOLL_CTL_ADD, fds[i], &ev) < 0)
			return fnl_fail(FNL_ERR_SYSTEM, "epoll_ctl(ADD, %d) failed: %s",
							fds[i], strerror(errno));
	}
	return FNL_OK;
}

/* How long the pump may block, for epoll_wait: for ever, unless something is
 * owed by `deadline` on now_seconds()'s clock -- then until that, rounded up
 * so the wait cannot end a fraction of a millisecond early and spin. */
static inline int loop_wait_ms(bool owed, double deadline)
{
	if (!owed)
		return -1;
	const double left = deadline - now_seconds();
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
	const uint64_t now = (uint64_t)ts.tv_sec * 1000000u +
						 (uint64_t)ts.tv_nsec / 1000u;
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
	const uint64_t now_us = (uint64_t)ts.tv_sec * 1000000u +
							(uint64_t)ts.tv_nsec / 1000u;
	const int32_t d = (int32_t)((uint32_t)(now_us / 1000u) - ms);
	app_input_age(a, d < 0 ? -1.0
						   : d * 1e-3 + (double)(now_us % 1000u) * 1e-6
							 - 0.5e-3);
}

#endif /* FNL_LOOP_LINUX_H */
