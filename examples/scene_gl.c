/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A scene drawn by the caller with OpenGL, the library choosing where it is
 * drawn: straight into the window, into a smaller image it scales up, or
 * multisampled. The scene here is one clear, so that the seam is all there
 * is to read; testbed/scene_gl.c is a real one, with programs.
 *
 * Shows: fnl_gl_proc fnl_gl_scene_layout fnl_gl_scene_framebuffer
 *        fnl_gl_version fnl_renderer fnl_gpu_name
 */

#include <fenestal/fnl_gl.h>

/* Khronos's header for the types and enums; the entry points are loaded
 * through fnl_gl_proc(), and none is declared. */
#include <GL/glcorearb.h>

#include <stdio.h>
#include <string.h>

typedef struct {
	PFNGLCLEARBUFFERFVPROC clear;
	FnlGlSceneLayout layout[FNL_TARGET_COUNT];
	bool             has[FNL_TARGET_COUNT];
	int              version;    /* 45, or 41 on a device with no 4.5 */
} Scene;

/* Entry points belong to the device, as everything built here does: loaded
 * again after every loss, which may land on another driver. */
static void on_gpu_created(void *ud, FnlApp *app)
{
	Scene *s = ud;
	/* 4.1's clear, which a 4.5 context has too: the scene's calls are the
	 * oldest context's it may meet, or chosen by the version. */
	s->clear = (PFNGLCLEARBUFFERFVPROC)fnl_gl_proc(app, "glClearBufferfv");
	s->version = fnl_gl_version(app);
	for (int t = 0; t < FNL_TARGET_COUNT; t++)
		s->has[t] = fnl_gl_scene_layout(app, (FnlSceneTarget)t,
										&s->layout[t]);
}

static void on_gpu_lost(void *ud, FnlApp *app)
{
	(void)app;
	Scene *s = ud;
	memset(s, 0, sizeof *s);
}

static void on_record(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Scene *s = ud;
	uint32_t fbo;
	if (!s->clear || !fnl_gl_scene_framebuffer(app, &fbo))
		return;
	/* Bound, with the scissor on frame->scene_*, which a clear obeys. GL
	 * keeps no recording, so this runs every frame shown; the colour still
	 * follows scene_time, the clock that moves only while the scene does. */
	const float t = frame->scene_time;
	const float colour[4] = { 0.1f + 0.1f * (t - (float)(int)t),
							  0.12f, 0.16f, 1.0f };
	s->clear(GL_COLOR, 0, colour);
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Scene *s = ud;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
	}
	char line[160];
	snprintf(line, sizeof line, "%s (%s %d.%d)  target %d, %d sample%s",
			 fnl_gpu_name(app),
			 fnl_renderer(app) == FNL_RENDERER_OPENGL ? "OpenGL" : "?",
			 s->version / 10, s->version % 10,
			 (int)frame->target, s->layout[frame->target].samples,
			 s->layout[frame->target].samples == 1 ? "" : "s");
	const FnlColor ink = { 0.9f, 0.9f, 0.9f, 1.0f };
	fnl_overlay_text(app, 16, 48, FNL_TEXT_BODY, ink, line);
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "scene";
	/* The renderer the scene is written for, which a build of several
	 * renderers must not trade for another. */
	desc.renderer = FNL_RENDERER_OPENGL;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	Scene s;
	memset(&s, 0, sizeof s);
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &s;
	cb.on_gpu_created = on_gpu_created;
	cb.on_gpu_lost = on_gpu_lost;
	cb.on_record = on_record;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}
