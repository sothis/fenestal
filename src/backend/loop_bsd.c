/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The FreeBSD poll() set: the descriptors a pump watches, the room a flush
 * waits for, and what a wait found. loop_bsd.h has the wakeup and the
 * reasons; this is the set that grows.
 */

#include "core/app.h"

#include "backend/loop.h"

#include <stdlib.h>
#include <string.h>

static bool loop_grow(Loop *l)
{
	if (l->n < l->cap)
		return true;
	const int cap = l->cap ? l->cap * 2 : 8;
	struct pollfd *pfd = realloc(l->pfd, (size_t)cap * sizeof *pfd);
	if (!pfd)
		return false;
	/* pfd's old block is gone as soon as realloc returns; the set keeps
	 * the larger one even if the second allocation fails. */
	l->pfd = pfd;
	bool *out = realloc(l->out, (size_t)cap * sizeof *out);
	if (!out)
		return false;
	l->out = out;
	l->cap = cap;
	return true;
}

FnlResult loop_create(Loop *l, int server_fd, int wake_fd)
{
	memset(l, 0, sizeof *l);
	/* A negative fd is one a backend has not: X11 adds each window's
	 * connection as it opens. */
	if ((server_fd >= 0 && !loop_add(l, server_fd)) ||
		(wake_fd >= 0 && !loop_add(l, wake_fd))) {
		const int e = errno;
		loop_destroy(l);
		return fnl_fail(FNL_ERR_SYSTEM, "poll set add failed: %s",
						strerror(e));
	}
	return FNL_OK;
}

void loop_destroy(Loop *l)
{
	free(l->pfd);
	free(l->out);
	memset(l, 0, sizeof *l);
}

bool loop_add(Loop *l, int fd)
{
	if (!loop_grow(l))
		return false;
	l->pfd[l->n].fd      = fd;
	l->pfd[l->n].events  = POLLIN;
	l->pfd[l->n].revents = 0;
	l->out[l->n]         = false;
	l->n++;
	return true;
}

bool loop_mod(Loop *l, int fd, bool want_write)
{
	for (int i = 0; i < l->n; i++) {
		if (l->pfd[i].fd == fd) {
			l->out[i] = want_write;
			return true;
		}
	}
	return false;
}

void loop_del(Loop *l, int fd)
{
	for (int i = 0; i < l->n; i++) {
		if (l->pfd[i].fd != fd)
			continue;
		l->pfd[i] = l->pfd[l->n - 1];
		l->out[i] = l->out[l->n - 1];
		l->n--;
		return;
	}
}

/* poll() is level-triggered, as the epoll set is: a socket with data still
 * buffered is reported again until its reader drains it. */
int loop_wait(Loop *l, LoopEvent *evs, int max, int timeout_ms)
{
	for (int i = 0; i < l->n; i++) {
		l->pfd[i].events  = (short)(POLLIN | (l->out[i] ? POLLOUT : 0));
		l->pfd[i].revents = 0;
	}
	const int n = poll(l->pfd, (nfds_t)l->n, timeout_ms);
	if (n <= 0)
		return n;
	int k = 0;
	for (int i = 0; i < l->n && k < max; i++) {
		const short revents = l->pfd[i].revents;
		if (!revents)
			continue;
		unsigned events = 0;
		if (revents & POLLIN)
			events |= LOOP_IN;
		if (revents & POLLOUT)
			events |= LOOP_OUT;
		if (revents & POLLERR)
			events |= LOOP_ERR;
		if (revents & (POLLHUP | POLLNVAL))
			events |= LOOP_HUP;
		evs[k].fd     = l->pfd[i].fd;
		evs[k].events = events;
		k++;
	}
	return k;
}
