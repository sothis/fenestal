/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Written in 2026 by Janos Sonntag and dedicated to the public domain.
 *
 * A scene recorded by the caller, with the library choosing where it is
 * drawn: straight into the window, into a smaller image it scales up, or
 * multisampled. The scene here is one clear, so that the seam is all there
 * is to read; testbed/scene_vulkan.c is a real one, with
 * pipelines.
 *
 * Shows: fnl_vulkan_gpu fnl_vulkan_scene_layout fnl_vulkan_scene_cmd
 *        fnl_vulkan_scene_rect fnl_set_antialias fnl_antialias fnl_max_samples
 *        fnl_offscreen_supported fnl_set_scene_size_px fnl_scene_size_px
 *        fnl_gpu_name fnl_renderer fnl_fps
 */

/* Every Vulkan call is asked of the library's loader, so the program links
 * none and still starts, on another renderer, where there is none. */
#define VK_NO_PROTOTYPES
#include <fenestal/fnl_vulkan.h>

#include <stdio.h>
#include <string.h>

typedef struct {
	FnlVulkanGpu         gpu;
	FnlVulkanSceneLayout layout[FNL_TARGET_COUNT];
	bool                 has[FNL_TARGET_COUNT];
	PFN_vkCmdClearAttachments vkCmdClearAttachments;
} Scene;

/* Everything that belongs to the device is built here and dropped in
 * on_gpu_lost; a pipeline would be built once per target the device has. */
static void on_gpu_created(void *ud, FnlApp *app)
{
	Scene *s = ud;
	if (!fnl_vulkan_gpu(app, &s->gpu))
		return;
	/* The instance is new after every loss, and so is each entry. */
	s->vkCmdClearAttachments = (PFN_vkCmdClearAttachments)
		s->gpu.get_instance_proc_addr(s->gpu.instance,
									  "vkCmdClearAttachments");
	for (int t = 0; t < FNL_TARGET_COUNT; t++)
		s->has[t] = fnl_vulkan_scene_layout(app, (FnlSceneTarget)t,
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
	VkCommandBuffer cb;
	VkRect2D        area;
	if (!s->vkCmdClearAttachments || !fnl_vulkan_scene_cmd(app, &cb) ||
		!fnl_vulkan_scene_rect(app, &area))
		return;
	/* Inside the pass, with the viewport and scissor on `area`. A clear
	 * takes neither, so it is given the rectangle: drawn straight into the
	 * window, the attachment is the whole window and the scene starts past
	 * the titlebar. The colour follows scene_time, which runs only while
	 * the scene's motion is on -- and is what re-records a frame that
	 * moves. */
	const float t = frame->scene_time;
	VkClearAttachment clear = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.colorAttachment = 0,
		.clearValue.color.float32 = { 0.1f + 0.1f * (t - (float)(int)t),
									  0.12f, 0.16f, 1.0f },
	};
	VkClearRect rect = { .rect = area, .layerCount = 1 };
	s->vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
}

static void on_overlay(void *ud, FnlApp *app, const FnlFrame *frame)
{
	(void)ud;
	FnlEvent ev;
	while (fnl_poll_event(app, &ev)) {
		if (ev.type != FNL_EVENT_KEY || !ev.key.pressed || ev.key.repeat)
			continue;
		/* Both need the offscreen target; the library refuses quietly
		 * where there is none, and the event says what took. */
		if (ev.key.key == FNL_KEY_A && fnl_offscreen_supported(app))
			fnl_set_antialias(app, fnl_antialias(app) == FNL_AA_NONE &&
								   fnl_max_samples(app) > 1
								   ? FNL_AA_MSAA : FNL_AA_NONE);
		if (ev.key.key == FNL_KEY_S)
			fnl_set_scene_size_px(app, frame->scene_w_px == frame->area_w_px
									   ? frame->area_w_px / 2 : 0,
									   frame->scene_w_px == frame->area_w_px
									   ? frame->area_h_px / 2 : 0);
	}

	int sw, sh;
	fnl_scene_size_px(app, &sw, &sh);
	char line[160];
	snprintf(line, sizeof line, "%s (%s)  scene %dx%d  target %d  %d fps",
			 fnl_gpu_name(app),
			 fnl_renderer(app) == FNL_RENDERER_VULKAN ? "Vulkan" : "?",
			 sw, sh, (int)frame->target, fnl_fps(app));
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
	desc.renderer = FNL_RENDERER_VULKAN;

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
