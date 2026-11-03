/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * The GPUs the window can be drawn on, and moving it from one to the next
 * while it stays open: Tab steps through the list, round to the first. The
 * list is printed at the start and whenever FNL_EVENT_GPU says the window
 * is on another GPU or the list grew -- the other renderers' GPUs join it
 * after the first frame.
 *
 * A program with a scene of its own sees on_gpu_lost and on_gpu_created
 * between the two, as after a device loss, and builds its scene again on
 * the new GPU -- for the renderer fnl_renderer() names then, which a GPU of
 * another renderer changes; this one draws the overlay alone and has
 * nothing to do.
 *
 * A GPU can be listed twice by one renderer, the second time drawn the
 * same way and presented another: Vulkan on Windows through a Direct3D 11
 * swapchain, fnl_gpu_via_at()'s "D3D11".
 *
 * Shows: fnl_gpu_count fnl_gpu_current fnl_gpu_name_at fnl_gpu_kind_at
 *        fnl_gpu_renderer_at fnl_gpu_via_at fnl_renderer_via
 *        fnl_request_gpu
 */

#include <fenestal/fenestal.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *kind_name(FnlGpuKind k)
{
	switch (k) {
	case FNL_GPU_KIND_INTEGRATED: return "integrated";
	case FNL_GPU_KIND_DISCRETE:   return "discrete";
	case FNL_GPU_KIND_VIRTUAL:    return "virtual";
	case FNL_GPU_KIND_SOFTWARE:   return "software";
	default:                      return "hardware";
	}
}

static const char *renderer_of(FnlRenderer r)
{
	switch (r) {
	case FNL_RENDERER_VULKAN: return "Vulkan";
	case FNL_RENDERER_OPENGL: return "OpenGL";
	case FNL_RENDERER_D3D12:  return "Direct3D 12";
	case FNL_RENDERER_METAL:  return "Metal";
	default:                  return "?";
	}
}

static void print_list(FnlApp *app)
{
	const int now = fnl_gpu_current(app);
	/* Copied: each snapshot is good until the next call on this thread. */
	char via[16];
	snprintf(via, sizeof via, "%s", fnl_renderer_via(app));
	printf("drawn with %s%s%s, %d GPUs:\n", fnl_renderer_name(app),
		   via[0] ? " via " : "", via, fnl_gpu_count(app));
	for (int i = 0; i < fnl_gpu_count(app); i++) {
		snprintf(via, sizeof via, "%s", fnl_gpu_via_at(app, i));
		printf("  %c %s (%s, %s%s%s)\n", i == now ? '*' : ' ',
			   fnl_gpu_name_at(app, i), kind_name(fnl_gpu_kind_at(app, i)),
			   renderer_of(fnl_gpu_renderer_at(app, i)),
			   via[0] ? " via " : "", via);
	}
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	bool *printed = ud;
	(void)frame;
	/* The first frame is the first moment the list is known. */
	if (!*printed) {
		*printed = true;
		print_list(app);
	}
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type == FNL_EVENT_GPU)
			print_list(app);
		if (ev.type == FNL_EVENT_KEY && ev.key.pressed && !ev.key.repeat &&
			ev.key.key == FNL_KEY_TAB && fnl_gpu_count(app) > 1)
			fnl_request_gpu(app, (fnl_gpu_current(app) + 1) %
								 fnl_gpu_count(app));
	}
}

int main(void)
{
	FnlInitDesc desc;
	memset(&desc, 0, sizeof desc);
	desc.size = sizeof desc;
	desc.header_version = FNL_VERSION;
	desc.title = "GPUs";
	/* No scene of its own, so any renderer the build has will do. */
	desc.renderers = FNL_RENDERERS_ANY;

	FnlApp *app;
	if (fnl_init(&desc, &app) < 0)
		return 1;
	bool printed = false;
	FnlCallbacks cb;
	memset(&cb, 0, sizeof cb);
	cb.size = sizeof cb;
	cb.ud = &printed;
	cb.on_overlay = on_overlay;
	const FnlResult rc = fnl_run(app, &cb);
	fnl_shutdown(app);
	return rc < 0;
}
