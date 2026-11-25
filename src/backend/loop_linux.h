/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Linux event loop: the eventfd that wakes a blocked pump, the epoll set
 * it sits in beside the window server's socket, and the netlink socket the
 * two GPU hotplugs are heard on. What the three Linux backends share and
 * none owns; eventfd and epoll are Linux rather than merely POSIX.
 *
 * Included by loop.h, which has the descriptors' meaning, the wait's length
 * and the input stamps both systems share.
 */

#ifndef FNL_LOOP_LINUX_H
#define FNL_LOOP_LINUX_H

#include "core/app.h"

#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

/* The epoll set itself. One per host, made beside the window server's
 * socket and the wakeup. */
struct Loop {
	int fd;
};

static inline void loop_init(Loop *l)
{
	l->fd = -1;
}

/* An epoll set watching the server's socket and the wakeup. Level-triggered
 * on purpose, which is epoll's default. */
static inline FNL_MUST_USE FnlResult loop_create(Loop *l, int server_fd,
												 int wake_fd)
{
	l->fd = epoll_create1(EPOLL_CLOEXEC);
	if (l->fd < 0)
		return fnl_fail(FNL_ERR_SYSTEM, "epoll_create1() failed: %s",
						strerror(errno));

	const int fds[2] = { server_fd, wake_fd };
	for (int i = 0; i < 2; i++) {
		/* A backend with no connection of its own adds them as windows
		 * open; a negative fd is one it has not. */
		if (fds[i] < 0)
			continue;
		struct epoll_event ev = { .events = EPOLLIN, .data.fd = fds[i] };
		if (epoll_ctl(l->fd, EPOLL_CTL_ADD, fds[i], &ev) < 0) {
			const int e = errno;
			close(l->fd);
			l->fd = -1;
			return fnl_fail(FNL_ERR_SYSTEM,
							"epoll_ctl(ADD, %d) failed: %s", fds[i],
							strerror(e));
		}
	}
	return FNL_OK;
}

static inline void loop_destroy(Loop *l)
{
	if (l->fd >= 0)
		close(l->fd);
	l->fd = -1;
}

/* Readiness alone; a descriptor with room is not asked for here. */
static inline bool loop_add(Loop *l, int fd)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	return epoll_ctl(l->fd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

/* Whether the descriptor's socket has room, which a flush that found it
 * full waits for. Level-triggered, so it is watched only when wanted. */
static inline bool loop_mod(Loop *l, int fd, bool want_write)
{
	struct epoll_event ev = {
		.events  = EPOLLIN | (want_write ? EPOLLOUT : 0u),
		.data.fd = fd,
	};
	return epoll_ctl(l->fd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

static inline void loop_del(Loop *l, int fd)
{
	epoll_ctl(l->fd, EPOLL_CTL_DEL, fd, NULL);
}

/* The pump's block: at most LOOP_EVENTS of them, each as an fd and what is
 * ready on it. A negative answer is the wait's own (EINTR). */
static inline int loop_wait(Loop *l, LoopEvent *out, int max, int timeout_ms)
{
	if (max > LOOP_EVENTS)
		max = LOOP_EVENTS;
	struct epoll_event evs[LOOP_EVENTS];
	const int n = epoll_wait(l->fd, evs, max, timeout_ms);
	for (int i = 0; i < n; i++) {
		unsigned events = 0;
		if (evs[i].events & EPOLLIN)
			events |= LOOP_IN;
		if (evs[i].events & EPOLLOUT)
			events |= LOOP_OUT;
		if (evs[i].events & EPOLLERR)
			events |= LOOP_ERR;
		if (evs[i].events & EPOLLHUP)
			events |= LOOP_HUP;
		out[i].fd     = evs[i].data.fd;
		out[i].events = events;
	}
	return n;
}

/* An eventfd for the set: another thread has no other way to break the pump
 * out of an indefinite wait. A plain counter, so one descriptor covers both
 * directions where a self-pipe would need two, and draining it is a single
 * read. Non-blocking, so neither side can stall on the other. */
static inline FNL_MUST_USE FnlResult loop_wake_create(LoopWake *w)
{
	w->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (w->fd < 0)
		return fnl_fail(FNL_ERR_SYSTEM, "eventfd() failed: %s",
						strerror(errno));
	w->wfd = w->fd;
	return FNL_OK;
}

/* Called from a signal handler, so it is a write() and nothing else. EAGAIN
 * means the counter is saturated, which is a wakeup pending already. */
static inline void loop_wake(const LoopWake *w)
{
	const uint64_t one = 1;
	ssize_t n          = write(w->wfd, &one, sizeof one);
	(void)n;
}

/* One read empties the counter. */
static inline void loop_wake_drain(const LoopWake *w)
{
	uint64_t drain;
	ssize_t got = read(w->fd, &drain, sizeof drain);
	(void)got;
}

static inline void loop_wake_destroy(LoopWake *w)
{
	if (w->fd >= 0)
		close(w->fd);
	w->fd = w->wfd = -1;
}

/* ---- GPU hotplug, the backends that hear none -------------------------
 *
 * A DRM card node added or removed is a GPU that came or went. KMS hears it
 * on a uevent socket of its own (kms.c's uevent_open()); the two Linux
 * window servers' backends have no socket to spare, so this is the one they
 * all share: a netlink group anyone may read, opened once at fnl_init() and
 * polled by the pump thread (app_uevent_pump()), which calls the same
 * app_gpu_hotplug() KMS's own uevent drain does. A kernel without the group
 * leaves the fd -1, and a hotplug is then heard by nobody -- as before.
 *
 * The reader is Linux's, like epoll and eventfd, so it is a header and not a
 * source a build has to list. */

/* A uevent is a GPU's when it is a DRM card node added or removed: the
 * kernel's strings, NUL-separated after the "action@device" header, carry
 * SUBSYSTEM=drm, ACTION=add/remove and a DEVNAME=dri/cardN. A move to
 * another kernel driver is not a hotplug: the card goes and comes with it,
 * and app_gpu_driver_owns() says which paths are the moved GPU's. Untested:
 * no GPU of this machine's can be plugged (TODO). */
static inline bool loop_uevent_is_gpu(const App *a, const char *buf, size_t n)
{
	bool drm = false, card = false, change = false, moved = false;
	for (size_t at = 0; at < n;) {
		const char *s    = buf + at;
		const size_t len = strnlen(s, n - at);
		drm |= strcmp(s, "SUBSYSTEM=drm") == 0;
		card |= strncmp(s, "DEVNAME=dri/card", 16) == 0;
		change |=
			strcmp(s, "ACTION=add") == 0 || strcmp(s, "ACTION=remove") == 0;
		if (strncmp(s, "DEVPATH=", 8) == 0)
			moved = app_gpu_driver_owns(a, s + 8);
		at += len + 1;
	}
	return drm && card && change && !moved;
}

/* Open the uevent socket and add it to the set, beside the window server's
 * socket and the wakeup. A failure leaves *fd -1 and logs once: nothing
 * hears a hotplug, which is what the caller asked for by naming a backend
 * that has no uevent socket of its own. */
static inline FnlResult loop_uevent_open(int *fd, Loop *l)
{
	*fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
				 NETLINK_KOBJECT_UEVENT);
	if (*fd < 0) {
		fnl_log(FNL_LOG_WARN,
				"no uevents (%s); a GPU plugged in later is not seen",
				strerror(errno));
		return FNL_OK;
	}
	struct sockaddr_nl at = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	if (bind(*fd, (struct sockaddr *)&at, sizeof at) < 0 ||
		!loop_add(l, *fd)) {
		fnl_log(FNL_LOG_WARN,
				"no uevents (%s); a GPU plugged in later is not seen",
				strerror(errno));
		close(*fd);
		*fd = -1;
	}
	return FNL_OK;
}

/* Every uevent there is, drained; one app_gpu_hotplug() each window for any
 * number of this machine's -- the list is a window's, and each draws with
 * the GPU that came or went. An overrun lost uevents nobody can read now,
 * so the GPU is said to have come or gone all the same -- a missed hotplug
 * is a stale list, which the next rebuild fixes. */
static inline void loop_uevent_drain(App *a, int fd)
{
	char buf[4096];
	bool gpu = false;
	ssize_t got;
	while ((got = recv(fd, buf, sizeof buf - 1, 0)) > 0) {
		buf[got] = '\0';
		gpu |= loop_uevent_is_gpu(a, buf, (size_t)got);
	}
	if (gpu || (got < 0 && errno == ENOBUFS))
		for (App *w = a->host->windows; w; w = w->next)
			app_gpu_hotplug(w);
}

#endif /* FNL_LOOP_LINUX_H */
