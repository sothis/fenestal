/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A scene drawn by the caller with Direct3D 12, the library choosing where
 * it is drawn: straight into the window, into a smaller image it scales up,
 * or multisampled. The scene here is one clear, so that the seam is all
 * there is to read; testbed/scene_d3d12.c is a real one, with
 * pipeline states.
 *
 * Shows: fnl_d3d12_gpu fnl_d3d12_scene_layout fnl_d3d12_scene
 *        fnl_renderer fnl_gpu_name
 */

/* COM's C macros, before the header that brings in <d3d12.h>. */
#define COBJMACROS

#include <fenestal/fnl_d3d12.h>

#include <stdio.h>
#include <string.h>

typedef struct {
	bool                ready;
	FnlD3D12SceneLayout layout[FNL_TARGET_COUNT];
	bool                has[FNL_TARGET_COUNT];
} Scene;

/* The device and the layouts belong to one device generation: asked again
 * after every loss, which may land on another adapter. A pipeline state
 * would be built here, one per target that has a layout. */
static void on_gpu_created(void *ud, FnlApp *app)
{
	Scene *s = ud;
	FnlD3D12Gpu gpu;
	s->ready = fnl_d3d12_gpu(app, &gpu);
	for (int t = 0; t < FNL_TARGET_COUNT; t++)
		s->has[t] = fnl_d3d12_scene_layout(app, (FnlSceneTarget)t,
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
	FnlD3D12Scene sc;
	if (!s->ready || !fnl_d3d12_scene(app, &sc))
		return;
	/* Bound, with the viewport and scissor on sc.rect. A clear ignores the
	 * scissor, so it is given the rectangle. Recorded every frame shown;
	 * the colour still follows scene_time, the clock that moves only while
	 * the scene does. */
	const float t = frame->scene_time;
	const float colour[4] = { 0.1f + 0.1f * (t - (float)(int)t),
							  0.12f, 0.16f, 1.0f };
	ID3D12GraphicsCommandList_ClearRenderTargetView(sc.list, sc.rtv, colour,
													1, &sc.rect);
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	Scene *s = ud;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
	}
	char line[160];
	snprintf(line, sizeof line, "%s (%s)  target %d, %d sample%s",
			 fnl_gpu_name(app),
			 fnl_renderer(app) == FNL_RENDERER_D3D12 ? "Direct3D 12" : "?",
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
	desc.renderer = FNL_RENDERER_D3D12;

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
