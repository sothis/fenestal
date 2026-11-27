/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The glyph atlas decoded ahead of the renderer that wants it. Its own file
 * because it starts a thread, which the text fuzz harness -- text_measure.c
 * and nothing else -- would otherwise have to link.
 */

#include "core/app.h"

#include "font_data.h"
#include "font_codec.h"

/* ---- the atlas, decoded ahead ---------------------------------------------
 *
 * The atlas decode needs no device -- 17 to 45 ms of range decoding at the
 * baked scales (DESIGN *The blob*) -- yet it ran inside the renderer's build,
 * on the path from the window to its first frame, after the instance, the
 * device and the pipelines. The scale the window will draw at is known once
 * the backend has opened, so a thread decodes that atlas while the renderer
 * is made, and the build takes the bytes instead of decoding them.
 *
 * One slot for the process, taken once: whichever build comes first settles
 * it, a scale it did not guess is decoded as before, and a decode nobody
 * asked for is freed. A scale change later finds the slot empty. */
static struct {
	FnlThread      thread;
	bool           started;
	int            si;
	size_t         bytes;
	unsigned char *buf; /* the thread's result; null once it failed */
} atlas_pre;
static FnlMutex atlas_pre_lock = FNL_MUTEX_STATIC;

static void *atlas_pre_main(void *arg)
{
	(void)arg;
	const size_t bytes = (size_t)FONT_ATLAS_W[atlas_pre.si] *
						 (size_t)FONT_ATLAS_H[atlas_pre.si];
	/* Zeroed as the renderers' scratch is: a decode that succeeds has written
	 * every byte, which -fanalyzer cannot follow through the decoder. */
	unsigned char *buf = calloc(1, bytes);
	if (buf && !font_decode(font_atlas_blob + FONT_ATLAS_OFF[atlas_pre.si],
							FONT_ATLAS_LEN[atlas_pre.si], buf, bytes)) {
		free(buf);
		buf = NULL;
	}
	atlas_pre.bytes = bytes;
	atlas_pre.buf   = buf;
	return NULL;
}

void text_atlas_prefetch(int si)
{
	if (si < 0 || si >= FONT_SCALE_COUNT)
		return;
	fnl_mutex_lock(&atlas_pre_lock);
	if (!atlas_pre.started) {
		atlas_pre.si  = si;
		atlas_pre.buf = NULL;
		/* A thread that does not start leaves the build to decode, as it
		 * always did. */
		atlas_pre.started = fnl_thread_start(&atlas_pre.thread, atlas_pre_main,
											 NULL);
	}
	fnl_mutex_unlock(&atlas_pre_lock);
}

unsigned char *text_atlas_take(int si, size_t bytes)
{
	fnl_mutex_lock(&atlas_pre_lock);
	if (!atlas_pre.started) {
		fnl_mutex_unlock(&atlas_pre_lock);
		return NULL;
	}
	/* Joined with the lock held: the thread takes none, and a second taker
	 * must find the slot settled, not half. */
	fnl_thread_join(atlas_pre.thread);
	atlas_pre.started = false;
	unsigned char *buf = atlas_pre.buf;
	atlas_pre.buf      = NULL;
	if (buf && (atlas_pre.si != si || atlas_pre.bytes != bytes)) {
		free(buf);
		buf = NULL;
	}
	fnl_mutex_unlock(&atlas_pre_lock);
	return buf;
}
