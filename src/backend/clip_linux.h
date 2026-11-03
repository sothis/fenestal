/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * A paste arriving from another program, as the Wayland and X11 backends
 * receive one: a buffer that grows as the pieces come, a limit on how much is
 * kept, and a deadline renewed by each piece. How the pieces come is each
 * backend's -- a pipe on Wayland, property pieces on X11 -- and the rest is
 * here, so that the two cannot answer the same paste differently. DESIGN
 * *The clipboard*.
 *
 * Inline, as loop_linux.h is. The buffer lives in a field of the backend's
 * state and nowhere else, which is the shape -fanalyzer can follow.
 */

#ifndef FNL_CLIP_LINUX_H
#define FNL_CLIP_LINUX_H

#include "core/app.h"

/* What is kept; a paste past it is answered with no text. */
#define CLIP_MAX ((size_t)64 << 20)

/* How long the owner may say nothing before the paste is given up: it is
 * another program, and may hang or die between two pieces. */
#define CLIP_PATIENCE_S 3.0

typedef struct {
	char   *buf;
	size_t  len, cap;
	double  deadline;
} ClipIn;

static inline void clip_in_renew(ClipIn *c)
{
	c->deadline = now_seconds() + CLIP_PATIENCE_S;
}

static inline bool clip_in_expired(const ClipIn *c)
{
	return now_seconds() >= c->deadline;
}

/* Room for `need` more bytes and the terminator the core adds. False is no
 * memory, which the caller answers as a paste cut short. */
static inline bool clip_in_room(ClipIn *c, size_t need)
{
	if (c->cap - c->len >= need + 1)
		return true;
	const size_t cap = (c->len + need) * 2 + 1;
	char *grown = realloc(c->buf, cap);
	if (!grown)
		return false;
	c->buf = grown;
	c->cap = cap;
	return true;
}

/* `n` more bytes have been written at buf + len. False once the paste has
 * passed CLIP_MAX -- checked here, where the bytes land, and not where the
 * buffer grows, which doubles. */
static inline bool clip_in_grew(ClipIn *c, size_t n)
{
	c->len += n;
	clip_in_renew(c);
	return c->len <= CLIP_MAX;
}

/* Hand the paste to the core: all of it, or -- for a transfer cut short,
 * over the limit or not text -- no text, never the part that arrived, which
 * may end inside a character. The buffer changes hands as it is. */
static inline void clip_in_finish(App *a, ClipIn *c, bool whole)
{
	char *buf = whole ? c->buf : NULL;
	const size_t len = whole ? c->len : 0;
	if (!whole)
		free(c->buf);
	c->buf = NULL;
	c->len = c->cap = 0;
	app_clipboard_take(a, buf, len);
}

static inline void clip_in_free(ClipIn *c)
{
	free(c->buf);
	c->buf = NULL;
	c->len = c->cap = 0;
}

#endif /* FNL_CLIP_LINUX_H */
