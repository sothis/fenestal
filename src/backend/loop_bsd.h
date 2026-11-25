/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The FreeBSD event loop: a poll() set and a pipe for the wakeup. FreeBSD
 * has epoll and eventfd of its own in recent releases, but its epoll is
 * built on kqueue filters, which a character device's driver need not
 * provide -- and evdev's and DRM's are exactly the descriptors this set
 * carries. poll() asks every driver the same way.
 *
 * Included by loop.h, which has the descriptors' meaning, the wait's length
 * and the input stamps both systems share. The set itself needs a source
 * file (loop_bsd.c) because it grows.
 */

#ifndef FNL_LOOP_BSD_H
#define FNL_LOOP_BSD_H

#include "core/app.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

/* The registered descriptors and whether each also wants room to write,
 * beside the array poll() fills. Grown by loop_add(). */
struct Loop {
	struct pollfd *pfd;
	bool          *out;
	int            n;
	int            cap;
};

static inline void loop_init(Loop *l)
{
	memset(l, 0, sizeof *l);
}

FnlResult loop_create(Loop *l, int server_fd, int wake_fd);
void      loop_destroy(Loop *l);
bool      loop_add(Loop *l, int fd);
bool      loop_mod(Loop *l, int fd, bool want_write);
void      loop_del(Loop *l, int fd);
int       loop_wait(Loop *l, LoopEvent *evs, int max, int timeout_ms);

/* A pipe: the read end is watched, the write end is what a signal handler
 * writes. One descriptor cannot be its own peer as eventfd's is. */
static inline FNL_MUST_USE FnlResult loop_wake_create(LoopWake *w)
{
	int fds[2];
	if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) < 0)
		return fnl_fail(FNL_ERR_SYSTEM, "pipe2() failed: %s",
						strerror(errno));
	w->fd  = fds[0];
	w->wfd = fds[1];
	return FNL_OK;
}

static inline void loop_wake(const LoopWake *w)
{
	const char one = 1;
	ssize_t n      = write(w->wfd, &one, sizeof one);
	(void)n;
}

static inline void loop_wake_drain(const LoopWake *w)
{
	char buf[64];
	ssize_t got;
	while ((got = read(w->fd, buf, sizeof buf)) > 0)
		;
	(void)got;
}

static inline void loop_wake_destroy(LoopWake *w)
{
	if (w->fd >= 0)
		close(w->fd);
	if (w->wfd >= 0 && w->wfd != w->fd)
		close(w->wfd);
	w->fd = w->wfd = -1;
}

/* ---- GPU hotplug, which FreeBSD hears differently ---------------------
 *
 * There is no NETLINK_KOBJECT_UEVENT here; the model is devd(8) and
 * devctl(4), and the latter is privileged. A GPU that comes or goes is
 * therefore not heard, which is the same as a Linux kernel without the
 * group: the fd stays -1 and the list is the one the open found (the KMS
 * backend rescans its outputs on a deadline of its own). */
static inline FnlResult loop_uevent_open(int *fd, Loop *l)
{
	(void)l;
	*fd = -1;
	fnl_log(FNL_LOG_WARN,
			"no uevents here; a GPU plugged in later is not seen");
	return FNL_OK;
}

static inline bool loop_uevent_is_gpu(const App *a, const char *buf, size_t n)
{
	(void)a;
	(void)buf;
	(void)n;
	return false;
}

static inline void loop_uevent_drain(App *a, int fd)
{
	(void)a;
	(void)fd;
}

#endif /* FNL_LOOP_BSD_H */
