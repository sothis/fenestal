/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Vulkan half of the frame: command-buffer recording, submit and present.
 * What goes in the scene's pass is the caller's (see on_record); what goes
 * over it is the client titlebar and whatever the caller appends through the
 * public overlay.
 *
 * Recording is generation-gated: a command buffer is re-recorded only when
 * record_gen has moved since it was. DESIGN *Recording*.
 */

#include "core/app.h"

#include "renderer/vulkan/vk_internal.h"

/* Finite, so the render thread re-checks a->quit instead of parking forever in
 * vkAcquireNextImageKHR when the compositor stops releasing images (occluded
 * or minimized under FIFO). */
#define ACQUIRE_TIMEOUT_NS 100000000ull   /* 100 ms */

/* On a display surface -- KMS, the window the output and the GPU driving it
 * -- Mesa's WSI before 26.2.6 reports an acquire that times out as a lost
 * surface, and breaks the swapchain: wsi_display_acquire_next_image()
 * tests its timed wait's C11 result against ETIMEDOUT, and
 * u_cnd_monotonic_timedwait() answers thrd_timedout (fixed by e102ea9f).
 * The bound stays while a Mesa before it is installed. The first presents at
 * a mode wait out its mode set, some 140 ms for schenker's HDMI monitor at
 * 3840x2160, so at 100 ms every arrival on it was a device rebuild. Long
 * enough to outlast a mode set, and still a bound for a display that
 * stops. DESIGN *KMS: no window server*. */
#define ACQUIRE_TIMEOUT_DISPLAY_NS 2000000000ull   /* 2 s */

/* How long the frame that acks a configure waits to be shown before the ack
 * goes anyway (FnlBackendVtbl::ack_when_shown): two periods at 60 Hz. */
#define ACK_SHOWN_WAIT_NS 34000000ull     /* 34 ms */

/* ---- the scene ------------------------------------------------------------
 *
 * Drawn straight into the window, or rendered into an image of its own and
 * stretched back over it (DESIGN *Render scale*).
 *
 * Whether the offscreen path is live is asked of the objects, not of the draw
 * state: scene_set_size() gives up rather than failing, so a frame can be
 * asked for a scene size whose image was never built. */
static bool scene_offscreen(const App *a, const FnlDrawState *ds)
{
	return ds->scene_w > 0 && VK(a)->scene.image != VK_NULL_HANDLE &&
		   a->scene_extent.w == ds->scene_w &&
		   a->scene_extent.h == ds->scene_h;
}

/* FSAA, likewise asked of what was built: the device may offer no sample
 * count, or the multisampled pair may have been refused. */
static bool scene_msaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MSAA && VK(a)->scene_ms_fb != VK_NULL_HANDLE;
}

/* MLAA reads the scene image, so it needs the offscreen path under it, and it
 * runs in one of two places. When the scene is already the size of the
 * rectangle it lands on, the filter writes straight into the window's pass:
 * an image of its own would be fetched back texel for texel into the same
 * format, and that copy would change nothing. */
static bool scene_mlaa_direct(const App *a, const FnlDrawState *ds)
{
	int ax, ay, aw, ah;

	if (ds->aa != FNL_AA_MLAA || !scene_offscreen(a, ds))
		return false;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	return ds->scene_w == aw && ds->scene_h == ah;
}

/* Otherwise -- a scene pinned smaller than its area -- the filter has a pass
 * of its own ahead of the fetch that scales the result. Asked of the
 * framebuffer, which is built on first use and may have been refused. */
static bool scene_mlaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MLAA && VK(a)->mlaa_fb != VK_NULL_HANDLE &&
		   !scene_mlaa_direct(a, ds);
}

/* The seam: the pass is begun, the viewport is set, and what goes in the
 * command buffer is the caller's. The buffer is stashed rather than passed,
 * because the public callback takes no Vulkan type; fnl_vulkan_scene_cmd()
 * reads it back, and only while it is set. So is the rectangle (x, y, w, h)
 * the viewport was set to, for fnl_vulkan_scene_rect(): under
 * FNL_TARGET_DIRECT it starts at the content's origin, past the titlebar
 * and the ring, and vkCmdClearAttachments takes no viewport. */
static void record_scene_draws(App *a, VkCommandBuffer cb,
							   const FnlDrawState *ds, FnlSceneTarget t,
							   int x, int y, int w, int h)
{
	if (!a->cb.on_record)
		return;

	/* Both sizes are reported: screen-space layout needs the area, rasterising
	 * the scene, and they differ when a render scale is pinned. */
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);

	FnlFrame f = {
		.index      = a->presents,
		.time       = now_seconds() - a->started_at,
		.area_w_px  = aw,
		.area_h_px  = ah,
		.scene_w_px = w,
		.scene_h_px = h,
		.target     = t,
		.samples    = t == FNL_TARGET_OFFSCREEN_MS ? (int)scene_samples(a)
												   : 1,
		.scene_time = ds->scene_time,
	};

	VK(a)->scene_cb = cb;
	VK(a)->scene_rect = (VkRect2D){ .offset = { x, y },
									.extent = { (uint32_t)w, (uint32_t)h } };
	a->cb.on_record(a->cb.ud, a, &f);
	VK(a)->scene_cb = VK_NULL_HANDLE;
}

static VkExtent2D win_vk_extent(const App *a)
{
	return (VkExtent2D){ (uint32_t)a->extent.w, (uint32_t)a->extent.h };
}

/* One conversion, so VkExtent2D stays out of a field the shared frame policy
 * also reads. */
static VkExtent2D scene_vk_extent(const App *a)
{
	return (VkExtent2D){ (uint32_t)a->scene_extent.w,
						 (uint32_t)a->scene_extent.h };
}

/* Viewport and scissor over the whole of a target of extent `e`. */
static void set_full_viewport(VkCommandBuffer cb, VkExtent2D e)
{
	VkViewport vp = { 0.0f, 0.0f, (float)e.width, (float)e.height,
					  0.0f, 1.0f };
	VkRect2D sc = { .offset = { 0, 0 }, .extent = e };
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);
}

static void record_scene_pass(App *a, VkCommandBuffer cb,
							  const FnlDrawState *ds)
{
	const bool ms = scene_msaa(a, ds);

	/* Three clear values when multisampling: the resolve target is an
	 * attachment too, and the count must cover it though its value goes
	 * unread. */
	VkClearValue clears[3] = {
		{ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0 } },
		{ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
	};
	VkRenderPassBeginInfo rp = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = ms ? VK(a)->scene_pass_ms : VK(a)->scene_pass,
		.framebuffer = ms ? VK(a)->scene_ms_fb : VK(a)->scene_fb,
		.renderArea.extent = scene_vk_extent(a),
		.clearValueCount = ms ? 3u : 2u,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

	set_full_viewport(cb, scene_vk_extent(a));

	const FnlSceneTarget t = ms ? FNL_TARGET_OFFSCREEN_MS
								: FNL_TARGET_OFFSCREEN;
	record_scene_draws(a, cb, ds, t, 0, 0,
					   a->scene_extent.w, a->scene_extent.h);

	vkCmdEndRenderPass(cb);
}

/* MLAA ahead of a scaling fetch: a pass that reads the scene image and writes
 * the filter's own, at the scene's resolution because the stair-steps it looks
 * for are in the scene's pixel grid, not the upscaled one. */
static void record_mlaa_pass(App *a, VkCommandBuffer cb)
{
	VkRenderPassBeginInfo rp = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = VK(a)->post_pass,
		.framebuffer = VK(a)->mlaa_fb,
		.renderArea.extent = scene_vk_extent(a),
	};
	vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

	set_full_viewport(cb, scene_vk_extent(a));

	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
					  VK(a)->mlaa_pipeline);
	/* The set the fetch uses to read the unfiltered scene: same view, same
	 * sampler, same binding. */
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
							VK(a)->blit_layout, 0, 1, &VK(a)->blit_set_scene,
							0, NULL);
	vkCmdDraw(cb, 3, 1, 0, 0);

	vkCmdEndRenderPass(cb);
}

/* The scene onto the window, inside the window's pass and under the overlay:
 * drawn directly at the area's own resolution, or the image it was rendered
 * into fetched across the letterboxed rectangle that keeps its aspect.
 *
 * Leaves the viewport and scissor on that rectangle; the caller restores the
 * full extent the overlay pipelines are recorded against. */
static void draw_scene(App *a, VkCommandBuffer cb, const FnlDrawState *ds)
{
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	if (aw < 1 || ah < 1)
		return;

	int dx = 0, dy = 0, dw = aw, dh = ah;
	const bool offscreen = scene_offscreen(a, ds);
	if (offscreen)
		scene_dst_rect(ds->scene_w, ds->scene_h, aw, ah, &dx, &dy, &dw, &dh);

	VkViewport vp = { (float)(ax + dx), (float)(ay + dy),
					  (float)dw, (float)dh, 0.0f, 1.0f };
	VkRect2D sc = { .offset = { ax + dx, ay + dy },
					.extent = { (uint32_t)dw, (uint32_t)dh } };
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdSetScissor(cb, 0, 1, &sc);

	if (offscreen) {
		/* The fetch, reading whichever image the chain ended in -- or the
		 * filter itself reading the scene, with the viewport exactly the
		 * scene's size so its uv steps a texel a pixel. Two sets rather than
		 * one rewritten on a toggle: rewriting a set a command buffer in
		 * flight names is what scene_set_size() drains for. */
		const bool direct = scene_mlaa_direct(a, ds);
		vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
						  direct ? VK(a)->mlaa_win_pipeline
								 : VK(a)->blit_pipeline);
		VkDescriptorSet set = scene_mlaa(a, ds) ? VK(a)->blit_set_mlaa
												: VK(a)->blit_set_scene;
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
								VK(a)->blit_layout, 0, 1, &set, 0, NULL);
		vkCmdDraw(cb, 3, 1, 0, 0);
	} else {
		record_scene_draws(a, cb, ds, FNL_TARGET_DIRECT, sc.offset.x,
						   sc.offset.y, dw, dh);
	}
}

/* ---- recording + submit ------------------------------------------------ */

/* The copy path's last step (RendererState::present_copy), and the
 * snapshot's (App::snap_owed): the finished image into `dst`, out of the
 * layout the pass left it in and back to it, so the present that follows
 * finds the image as it would without this; then the buffer made visible
 * to the host that reads it after the fence. Where the window is the
 * renderer's own images, and not Direct3D 11's too, the pass leaves them in
 * the copy's layout already (vk_window_layout()), and the two barriers
 * order the copy and change no layout. Legal because the image is this
 * frame's, acquired and not yet presented: a presented image is the
 * presentation engine's until it is acquired again, which is why the
 * snapshot is taken here and not at the teardown. */
static void record_readback(App *a, VkCommandBuffer cb, uint32_t img,
							VkBuffer dst)
{
	VkImageMemoryBarrier out = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = vk_window_layout(a),
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = VK(a)->sc_images[img],
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	/* BOTTOM_OF_PIPE as well: the window's pass declares no dependency
	 * out, so its final layout change is ordered by the implicit one, whose
	 * second scope is BOTTOM_OF_PIPE alone, and this barrier chains onto it
	 * only through that stage. */
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
							 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
						 1, &out);
	const VkBufferImageCopy region = {
		.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.imageExtent = { (uint32_t)a->extent.w, (uint32_t)a->extent.h, 1 },
	};
	vkCmdCopyImageToBuffer(cb, VK(a)->sc_images[img],
						   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1,
						   &region);
	VkImageMemoryBarrier back = out;
	back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	back.dstAccessMask = 0;
	back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	back.newLayout = vk_window_layout(a);
	const VkBufferMemoryBarrier host = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = dst,
		.size = VK_WHOLE_SIZE,
	};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_HOST_BIT |
						 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						 0, 0, NULL, 1, &host, 1, &back);
}

static bool record_command_buffer(App *a, VkCommandBuffer cb, uint32_t img,
								  const FnlDrawState *ds)
{
	VkResult rr = vkResetCommandBuffer(cb, 0);
	if (vk_lost(a, rr))
		return false;
	VK_TRY_FALSE(rr);

	VkCommandBufferBeginInfo bi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
	};
	rr = vkBeginCommandBuffer(cb, &bi);
	if (vk_lost(a, rr))
		return false;
	VK_TRY_FALSE(rr);
	/* From the frame's first command to the end of its drawing -- the copy
	 * path's readback in it, a snapshot's not -- is the GPU's time on it
	 * (fnl_frame_gpu_ms()); reset here, since the buffer is submitted
	 * again unrecorded while the draw state stands. */
	if (VK(a)->ts_pool) {
		vkCmdResetQueryPool(cb, VK(a)->ts_pool, 2 * img, 2);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							VK(a)->ts_pool, 2 * img);
	}
	/* A stamp at the bottom of the pipe is written once everything before
	 * it has finished, a GL_TIMESTAMP's meaning: so the first one's lead
	 * over the pair's is the time the previous frame's work still held the
	 * queue. */
	const VkQueryPool parts = VK(a)->ts_pool ? VK(a)->ts_parts
											 : VK_NULL_HANDLE;
	if (parts) {
		vkCmdResetQueryPool(cb, parts, 4 * img, 4);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, parts,
							4 * img);
	}

	/* The scene's pass first when it is offscreen: it fills an image the
	 * window's pass samples, so it has to be outside that pass and before
	 * it. */
	if (scene_offscreen(a, ds)) {
		record_scene_pass(a, cb, ds);
		if (scene_mlaa(a, ds))
			record_mlaa_pass(a, cb);
	}
	if (parts)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, parts,
							4 * img + 1);

	/* Opaque black, which all but the grab ring wants -- the letterbox bars
	 * are this clear left showing. The load-op clear is the driver's fast one,
	 * so the window gets it and the transparent ring is punched out below; the
	 * other way round is a second full-framebuffer write inside the pass. It
	 * is also what keeps a fullscreen frame opaque on a surface with no opaque
	 * composite alpha. */
	VkClearValue clears[2] = {
		{ .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
		{ .depthStencil = { 1.0f, 0 } },
	};
	VkRenderPassBeginInfo rp = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = VK(a)->render_pass,
		.framebuffer = VK(a)->framebuffers[img],
		.renderArea.extent = win_vk_extent(a),
		.clearValueCount = 2,
		.pClearValues = clears,
	};
	vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);

	/* The transparent grab ring, as the four strips the window rectangle
	 * leaves over -- derived from that rectangle's own clipped edges, so ring
	 * and window tile the buffer exactly however the scale rounds.
	 * vkCmdClearAttachments because it writes alpha zero rather than blending
	 * it; it reads neither the viewport nor the scissor, which are not set
	 * yet. */
	FnlRect win;
	if (a->draw_margin > 0 &&
		overlay_clip(a, 0, 0, logical_w(a), logical_h(a), &win)) {
		const uint32_t fw = (uint32_t)a->extent.w, fh = (uint32_t)a->extent.h;
		const uint32_t wx = (uint32_t)win.x, wy = (uint32_t)win.y;
		const uint32_t wr = wx + (uint32_t)win.w;
		const uint32_t wb = wy + (uint32_t)win.h;
		const VkRect2D ring[4] = {
			{ { 0, 0 },                { fw, wy } },                /* top    */
			{ { 0, (int32_t)wb },      { fw, fh - wb } },           /* bottom */
			{ { 0, (int32_t)wy },      { wx, wb - wy } },           /* left   */
			{ { (int32_t)wr, (int32_t)wy }, { fw - wr, wb - wy } }, /* right  */
		};
		VkClearRect cr[4];
		uint32_t k = 0;
		for (int i = 0; i < 4; i++)
			if (ring[i].extent.width && ring[i].extent.height)
				cr[k++] = (VkClearRect){ .rect = ring[i], .baseArrayLayer = 0,
										 .layerCount = 1 };
		if (k) {
			/* The API rejects a zero extent, and clipping is free to produce
			 * one -- hence the filter above, not a flat count of four. */
			VkClearAttachment att = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.colorAttachment = 0,
				.clearValue.color.float32 = { 0.0f, 0.0f, 0.0f, 0.0f },
			};
			vkCmdClearAttachments(cb, 1, &att, k, cr);
		}
	}

	/* The scene, under the overlay (see draw_scene); then the full extent the
	 * overlay pipelines are recorded against. */
	draw_scene(a, cb, ds);
	set_full_viewport(cb, win_vk_extent(a));

	/* Everything below appends instances; nothing touches the command buffer
	 * until the flushes. VK(a)->overlay_cb is what they record into: set here
	 * rather than threaded through every overlay call, so those name no Vulkan
	 * type, and cleared so a stray call outside the pass draws nothing. */
	VK(a)->overlay_pushed = false;  /* a new recording: nothing is pushed yet */
	VK(a)->overlay_cb = cb;
	FnlResult orc = draw_overlay(a, ds, img);
	VK(a)->overlay_cb = VK_NULL_HANDLE;
	if (orc < 0)
		return false;

	if (parts)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, parts,
							4 * img + 2);
	vkCmdEndRenderPass(cb);
	if (parts)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, parts,
							4 * img + 3);
	/* The copy path's readback is work every frame on that path costs, so
	 * the GPU's time on the frame holds it, as OpenGL's holds its own; a
	 * snapshot's copy, once a move, is no part of drawing the frame and
	 * comes after the end stamp (DESIGN *What a frame costs, compared*). */
	if (VK(a)->present_copy)
		record_readback(a, cb, img, VK(a)->readback[img]);
	if (VK(a)->ts_pool)
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
							VK(a)->ts_pool, 2 * img + 1);
	if (!VK(a)->present_copy && a->snap_owed &&
		VK(a)->snap_buf[0] != VK_NULL_HANDLE)
		record_readback(a, cb, img, VK(a)->snap_buf[0]);
	rr = vkEndCommandBuffer(cb);
	if (vk_lost(a, rr))
		return false;
	VK_TRY_FALSE(rr);
	return true;
}

/* Whether the window's bytes are blue first, as a readback hands them on. */
static bool surface_bgra(const App *a)
{
	const VkFormat f = VK(a)->surf_format.format;
	return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM;
}

/* The copy path's present: once the device has finished the frame, its
 * pixels to the backend, which shows them and, under vsync, waits for the
 * flip. Inside the present's timing, since that wait is the throttle here.
 * The same pixels are the snapshot, where a move asked for one. */
static void present_copied(App *a, uint32_t frame, uint32_t img)
{
	VkResult wr = vkWaitForFences(VK(a)->device, 1, &VK(a)->in_flight[frame],
								  VK_TRUE, UINT64_MAX);
	if (vk_lost(a, wr) || wr != VK_SUCCESS)
		return;
	const bool bgra = surface_bgra(a);
	BACKEND(a)->present_pixels(a, VK(a)->readback_map[img], a->extent.w,
							   a->extent.h, (size_t)a->extent.w * 4u, bgra);
	if (a->snap_owed)
		app_snapshot_set(a, VK(a)->readback_map[img], a->extent.w,
						 a->extent.h, (size_t)a->extent.w * 4u, bgra);
}

/* The host buffer a frame copies itself into for the snapshot, at this
 * extent: made the first time, and again for a larger one after a drain,
 * since the frame that last copied into it may still be in flight. Cached
 * memory where the device has it, as the copy path's readback asks. */
static bool ensure_snap_buf(App *a)
{
	const VkDeviceSize need = (VkDeviceSize)a->extent.w * 4u *
							  (VkDeviceSize)a->extent.h;
	if (need <= VK(a)->snap_bytes)
		return true;
	if (VK(a)->snap_bytes) {
		if (vkQueueWaitIdle(VK(a)->queue) != VK_SUCCESS)
			return false;
		destroy_instance_buffers(a, 1, VK(a)->snap_buf, &VK(a)->snap_mem,
								 VK(a)->snap_map);
		VK(a)->snap_bytes = 0;
	}
	if (create_instance_buffers(a, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
								VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1,
								VK(a)->snap_buf, &VK(a)->snap_mem,
								VK(a)->snap_map) < 0)
		return false;
	VK(a)->snap_bytes = need;
	return true;
}

/* Unwind a frame that acquired an image and will not present it. Answers
 * false.
 *
 * The acquire signalled img_available[frame], and the next acquire into the
 * slot must be handed an unsignalled one
 * (VUID-vkAcquireNextImageKHR-semaphore-01779). Past vkResetFences,
 * in_flight[frame] is unsignalled and only the submit would have signalled it;
 * left so, the slot's next vkWaitForFences never returns. Vulkan 1.0 can reset
 * neither from the host, so one empty submit settles both: it waits on the
 * semaphore and carries the fence.
 *
 * `fence_reset` says which side of vkResetFences the caller failed on:
 * submitting an already-signalled fence is invalid usage
 * (VUID-vkQueueSubmit-fence-00063), so one not yet reset is reset here and
 * carried all the same -- left signalled, the slot's next fence wait would
 * return at once and its acquire hand over the semaphore this submit may
 * still be waiting on. Skipped once the device is lost, where recovery
 * destroys all of these anyway. */
static bool frame_bail(App *a, uint32_t frame, bool fence_reset)
{
	if (a->device_lost || a->surface_lost)
		return false;
	/* The image stays acquired: nothing presents it, and Vulkan 1.0 has no
	 * other way to hand one back. A few bails in a row would hold every
	 * image and the acquire would never return one, so the swapchain is
	 * replaced, which takes them all back with it. */
	if (!VK(a)->surfaceless)
		a->resized = VK(a)->swap_spent = true;
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	/* No acquire, no semaphore: the renderer's own images are taken, not
	 * acquired (RendererState::surfaceless). */
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = VK(a)->surfaceless ? 0u : 1u,
		.pWaitSemaphores = &VK(a)->img_available[frame],
		.pWaitDstStageMask = &stage,
	};
	VkFence fence = VK(a)->in_flight[frame];
	if (!fence_reset &&
		vkResetFences(VK(a)->device, 1, &VK(a)->in_flight[frame]) !=
			VK_SUCCESS)
		fence = VK_NULL_HANDLE;
	/* A submit refused for any other reason leaves an unsignalled fence
	 * nothing will signal and a semaphore nothing will wait on, which the
	 * slot's next turn would block on for good; and one carrying no fence,
	 * its reset refused, leaves a signalled fence that lets the next acquire
	 * hand over the semaphore this submit may still be waiting on.
	 * Recovery is what makes both again. */
	const VkResult sr = vkQueueSubmit(VK(a)->queue, 1, &si, fence);
	if (!vk_lost(a, sr) && (sr != VK_SUCCESS || fence == VK_NULL_HANDLE))
		a->device_lost = true;
	return false;
}

/* FNL_COST_PARTS's cut of a span read as `r` (the pair, with availability
 * between): the lead before the queue was the frame's (the previous work
 * draining), the offscreen scene's passes, the window's pass -- with any
 * wait for its image where the driver holds it there -- the pass's end,
 * which carries the change to the present layout, and the copy path's
 * readback. */
static void gpu_parts_read(App *a, uint32_t img, const uint64_t r[4])
{
	static const char *const NAMES[] = {
		"span", "lead", "scene", "window", "endpass", "readback",
	};
	uint64_t q[8];
	if (!VK(a)->ts_parts ||
		vkGetQueryPoolResults(VK(a)->device, VK(a)->ts_parts, 4 * img, 4,
							  sizeof q, q, 2 * sizeof q[0],
							  VK_QUERY_RESULT_64_BIT |
							  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) < 0 ||
		!q[1] || !q[3] || !q[5] || !q[7])
		return;
	const uint64_t m = VK(a)->ts_mask;
	const double k = VK(a)->ts_ns * 1e-9;
	const uint64_t at[] = { r[0], q[0], q[2], q[4], q[6], r[2] };
	double s[6];
	s[0] = (double)((r[2] - r[0]) & m) * k;
	for (int i = 1; i < 6; i++)
		s[i] = (double)((at[i] - at[i - 1]) & m) * k;
	parts_gpu(a, NAMES, s, 6);
}

/* The GPU's time on this image's last frame, once that frame is done: its
 * fence -- the one images_in_flight still names -- has signalled, so the
 * pair is that run's and not one before it. Read once a run (ts_unread),
 * and never waited for: a pair not yet there is a sample skipped. */
static void gpu_time_read(App *a, uint32_t img)
{
	const VkFence f = VK(a)->images_in_flight[img];
	if (!VK(a)->ts_pool || !VK(a)->ts_unread[img] || f == VK_NULL_HANDLE ||
		vkGetFenceStatus(VK(a)->device, f) != VK_SUCCESS)
		return;
	uint64_t r[4];
	if (vkGetQueryPoolResults(VK(a)->device, VK(a)->ts_pool, 2 * img, 2,
							  sizeof r, r, 2 * sizeof r[0],
							  VK_QUERY_RESULT_64_BIT |
							  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) < 0 ||
		!r[1] || !r[3])
		return;
	VK(a)->ts_unread[img] = false;
	app_gpu_time(a, (double)((r[2] - r[0]) & VK(a)->ts_mask) *
					VK(a)->ts_ns * 1e-9, VK(a)->ts_index[img]);
	gpu_parts_read(a, img, r);
}

/* Every image's, past the current swapchain's count too: a pair a larger
 * one left unread is as good, its fence the frame slot's. */
void vk_gpu_time_poll(App *a)
{
	if (!VK(a)->ts_pool)
		return;
	for (uint32_t i = 0; i < OVERLAY_MAX_IMAGES; i++)
		gpu_time_read(a, i);
}

/* Record this image's command buffer, if the draw state has moved since it
 * was. False is a frame to bail out of.
 *
 * `*t_rec` is where recording started. It moves only if the fence wait runs:
 * that wait is the device being waited for, so it is charged to `wait` and
 * kept out of both `record` and the frame's own cost. */
static bool frame_record(App *a, uint32_t img, const FnlDrawState *ds,
						 double t_ready, double *t_rec)
{
	if (VK(a)->img_cmd_gen[img] == a->record_gen)
		return true;

	/* This image's command buffer may still be executing from an earlier
	 * frame; wait on that frame's fence before re-recording it. */
	if (VK(a)->images_in_flight[img] != VK_NULL_HANDLE) {
		VkResult wr = vkWaitForFences(VK(a)->device, 1,
									  &VK(a)->images_in_flight[img],
									  VK_TRUE, UINT64_MAX);
		if (vk_lost(a, wr) || VKR(wr) < 0)
			return false;
		/* Unconditional: t_rec is where late_cost starts too. */
		*t_rec = now_seconds();
		app_frame_mark(a, FRAME_MARK_REC);
		a->device_block += *t_rec - t_ready;
		if (a->timing)
			a->fc_wait += *t_rec - t_ready;
	}
	if (!record_command_buffer(a, VK(a)->img_cmd[img], img, ds))
		return false;
	/* The recording alone; everything from here to the present is
	 * submitting, which a frame the recording cache served still does. */
	if (a->timing)
		a->fc_record = now_seconds() - *t_rec;
	VK(a)->img_cmd_gen[img] = a->record_gen;
	return true;
}

/* The driver's answers to the presents asked about (VK_EXT_present_timing):
 * when each one's first pixel went out, on CLOCK_MONOTONIC, handed to
 * fnl_frame_shown() by its present id (app_present_numbered()). Only
 * complete answers come, in order, and each once; a present that was
 * never shown answers with no time. Never waits. */
static void timing_take(App *a)
{
#ifdef VK_EXT_present_timing
	if (!VK(a)->timing_owed)
		return;
	VkPresentStageTimeEXT st[TIMING_QUEUE];
	VkPastPresentationTimingEXT t[TIMING_QUEUE];
	for (uint32_t i = 0; i < TIMING_QUEUE; i++)
		t[i] = (VkPastPresentationTimingEXT){
			.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,
			.presentStageCount = 1,
			.pPresentStages = &st[i],
		};
	const VkPastPresentationTimingInfoEXT info = {
		.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
		.swapchain = VK(a)->swapchain,
	};
	VkPastPresentationTimingPropertiesEXT got = {
		.sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
		.presentationTimingCount = TIMING_QUEUE,
		.pPresentationTimings = t,
	};
	const VkResult r = VK(a)->timing_past(VK(a)->device, &info, &got);
	if (r != VK_SUCCESS && r != VK_INCOMPLETE)
		return;
	/* The stage's own clock, onto CLOCK_MONOTONIC; a calibration that
	 * fails keeps the last offset. */
	if (VK(a)->timing_local &&
		now_seconds() - VK(a)->timing_cal_at >= TIMING_CALIBRATE_S)
		(void)vk_timing_calibrate(a);
	for (uint32_t i = 0; i < got.presentationTimingCount &&
						 i < TIMING_QUEUE; i++) {
		if (!t[i].reportComplete)
			continue;
		if (VK(a)->timing_owed)
			VK(a)->timing_owed--;
		if (t[i].timeDomainId == VK(a)->timing_domain &&
			t[i].presentStageCount >= 1 && st[i].time &&
			st[i].stage == VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT)
			app_present_number_shown(
				a, (uint32_t)t[i].presentId,
				(double)((int64_t)st[i].time + VK(a)->timing_offset_ns) *
					1e-9, true);
	}
#else
	(void)a;
#endif
}

/* The same of VK_GOOGLE_display_timing: each past present once, by its id,
 * its time the driver's, which the bridge puts on CLOCK_MONOTONIC
 * (VkBridge::driver_clock). A present not yet shown is not among them.
 * Never waits. */
static void google_take(App *a)
{
	VkPastPresentationTimingGOOGLE t[TIMING_QUEUE];
	uint32_t n = TIMING_QUEUE;
	const VkResult r = VK(a)->google_past(VK(a)->device, VK(a)->swapchain,
										  &n, t);
	if (r != VK_SUCCESS && r != VK_INCOMPLETE)
		return;
	for (uint32_t i = 0; i < n && i < TIMING_QUEUE; i++)
		if (t[i].actualPresentTime)
			app_present_number_shown(
				a, t[i].presentID,
				VK(a)->bridge->driver_clock(a, t[i].actualPresentTime),
				true);
}

/* ---- the frame that acks, waited for until shown --------------------------
 *
 * Mesa's X11 present returns before the server has the frame, so the ack
 * goes out only once the frame is shown (FnlBackendVtbl::ack_when_shown).
 * Waited for on the render thread, the wait held the loop: it came back
 * some 4 ms after the refresh, the frame after it was spaced a period from
 * the rebuild before it, and a drag on X11 drew one refresh in five fewer
 * -- 45 to 48 fps in six rounds of pace.py's border row. So a thread of
 * the renderer's own waits, and releases the ack itself, and the loop goes
 * on at its rate: 59 and 60. The manager still sends the next size only
 * once the frame at the last one is shown, which is the guarantee, and
 * what sets how fast the window follows a drag. */

static void *shown_main(void *arg)
{
	App *a = arg;

	fnl_mutex_lock(&VK(a)->shown_lock);
	for (;;) {
		while (!VK(a)->shown_busy && !VK(a)->shown_quit)
			fnl_cond_wait_for(&VK(a)->shown_cond, &VK(a)->shown_lock, 1.0);
		if (!VK(a)->shown_busy)
			break;
		const VkSwapchainKHR sc = VK(a)->shown_swapchain;
		const uint64_t id = VK(a)->shown_id;
		fnl_mutex_unlock(&VK(a)->shown_lock);

		/* Bounded, and released whatever it answers: a frame that is
		 * never shown costs the manager two periods, not the drag. The
		 * spec asks the swapchain externally synchronised here, and this
		 * wait does not take it: the render thread's acquires and
		 * presents on it go on meanwhile, and only its retirement and
		 * destruction wait (shown_idle()). Held, it would put the wait
		 * back on the loop, which costs a drag one refresh in five. Let
		 * go, x11-vulkan passes the round under ThreadSanitizer with a
		 * clean log (DESIGN *X11: xcb, not Xlib*), which sees the
		 * library's races and not the driver's: measured, not proven. */
		(void)VK(a)->wait_for_present(VK(a)->device, sc, id,
									  ACK_SHOWN_WAIT_NS);
		fnl_atomic_store_bool(&a->ack_held, false);
		BACKEND(a)->ack_release(a);

		fnl_mutex_lock(&VK(a)->shown_lock);
		VK(a)->shown_busy = false;
		fnl_cond_signal(&VK(a)->shown_cond);
	}
	fnl_mutex_unlock(&VK(a)->shown_lock);
	return NULL;
}

/* Give the present to the waiter. False is "wait for it here": no thread
 * to be had. */
static bool shown_hand_over(App *a, uint64_t present_id)
{
	if (!VK(a)->shown_started) {
		fnl_mutex_init(&VK(a)->shown_lock);
		fnl_cond_init(&VK(a)->shown_cond);
		VK(a)->shown_busy = VK(a)->shown_quit = false;
		if (!fnl_thread_start(&VK(a)->shown_thread, shown_main, a)) {
			fnl_cond_destroy(&VK(a)->shown_cond);
			fnl_mutex_destroy(&VK(a)->shown_lock);
			return false;
		}
		VK(a)->shown_started = true;
	}
	/* Ahead of the flush after this present, which keeps the ack. */
	fnl_atomic_store_bool(&a->ack_held, true);
	fnl_mutex_lock(&VK(a)->shown_lock);
	VK(a)->shown_swapchain = VK(a)->swapchain;
	VK(a)->shown_id = present_id;
	VK(a)->shown_busy = true;
	fnl_mutex_unlock(&VK(a)->shown_lock);
	fnl_cond_signal(&VK(a)->shown_cond);
	return true;
}

/* Ahead of a swapchain retired or destroyed, which a wait must not name,
 * and of the next ack, whose value the last frame's word must not carry
 * out. A drag does not wait here: its next configure comes after the
 * release. */
void shown_idle(App *a)
{
	if (!VK(a)->shown_started)
		return;
	fnl_mutex_lock(&VK(a)->shown_lock);
	while (VK(a)->shown_busy)
		fnl_cond_wait_for(&VK(a)->shown_cond, &VK(a)->shown_lock, 1.0);
	fnl_mutex_unlock(&VK(a)->shown_lock);
}

void shown_stop(App *a)
{
	if (!VK(a)->shown_started)
		return;
	fnl_mutex_lock(&VK(a)->shown_lock);
	VK(a)->shown_quit = true;
	fnl_mutex_unlock(&VK(a)->shown_lock);
	fnl_cond_signal(&VK(a)->shown_cond);
	fnl_thread_join(VK(a)->shown_thread);
	fnl_cond_destroy(&VK(a)->shown_cond);
	fnl_mutex_destroy(&VK(a)->shown_lock);
	VK(a)->shown_started = false;
}

/* A swapchain made larger than the window, or chained so it may become
 * so, that the driver still calls out of date or suboptimal. Left on,
 * the next create would make another large one, the driver would refuse
 * it again, and a drag would rebuild every frame -- worse than one
 * recreate a size. Given up for this renderer's life, and said once.
 * The fault counts only on a fresh swapchain, including one whose
 * acquire succeeded: a later frame would advance a %N, and the present
 * below runs after the acquire has cleared swap_fresh. Under NDEBUG
 * FNL_FAULT is the constant false. */
static void scale_check(App *a, VkResult r)
{
	if (!VK(a)->present_scale || VK(a)->swap_agreed)
		return;
	const bool fault = VK(a)->swap_fresh &&
					   FNL_FAULT("swapchain-scale-refused");
	if (!fault && r != VK_ERROR_OUT_OF_DATE_KHR && r != VK_SUBOPTIMAL_KHR)
		return;
	VK(a)->present_scale = false;
	VK(a)->scale_refused = true;
	a->resized = true;
	VK(a)->swap_spent = true;
	const char *why = "out of date";
	if (fault)
		why = "refused (FNL_FAULT)";
	else if (r == VK_SUBOPTIMAL_KHR)
		why = "suboptimal";
	fnl_log(FNL_LOG_WARN, "a swapchain larger than the window was %s; "
			"sizes recreate it", why);
}

/* Present a submitted image, with the window-server calls that ride on the
 * present. Nothing here can unsubmit the frame, so nothing is returned. */
static void frame_present(App *a, uint32_t frame, uint32_t img,
						  uint32_t cfg_serial, bool cfg_pending,
						  double acq_block, double t_rec)
{
	if (cfg_pending && BACKEND(a)->ack_when_shown)
		shown_idle(a);
	/* Everything below rides on the wl_surface.commit that vkQueuePresentKHR
	 * performs, so it has to be requested before the present rather than
	 * after it, and no earlier: the geometry and scale describe this buffer,
	 * the ack tells the compositor this buffer is the answer to its
	 * configure, and the frame callback asks when the next one is wanted. */
	app_surface_sync(a, cfg_serial, cfg_pending);
	BACKEND(a)->frame_request(a);

	/* An id on every present where the device takes one, so the present
	 * that acks can be waited for below. */
	const uint64_t present_id = ++VK(a)->present_id;
	const VkPresentIdKHR pid = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
		.swapchainCount = 1,
		.pPresentIds = &present_id,
	};
	VkPresentInfoKHR pi = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext = VK(a)->present_wait ? &pid : NULL,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &VK(a)->render_done[img],
		.swapchainCount = 1,
		.pSwapchains = &VK(a)->swapchain,
		.pImageIndices = &img,
	};
	/* Where the driver stamps the display path's frames, while the caller
	 * counts: this present asked about, by the same id given the way the
	 * extension reads it, unless the answers owed would fill the queue.
	 * The display path has no ack to wait for, so present_id's struct is
	 * not needed beside it. */
	bool timed = false;
#ifdef VK_EXT_present_timing
	const VkPresentTimingInfoEXT ti = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
		.timeDomainId = VK(a)->timing_domain,
		.presentStageQueries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
		.targetTimeDomainPresentStage =
			VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT,
	};
	const VkPresentTimingsInfoEXT tis = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
		.swapchainCount = 1,
		.pTimingInfos = &ti,
	};
	const VkPresentId2KHR pid2 = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
		.pNext = &tis,
		.swapchainCount = 1,
		.pPresentIds = &present_id,
	};
	if (a->counting && !VK(a)->timing_on && VK(a)->timing_why[0] &&
		!VK(a)->timing_said) {
		fnl_log(FNL_LOG_INFO, "Vulkan: the display path's frames are not "
				"stamped: %s", VK(a)->timing_why);
		VK(a)->timing_said = true;
	}
	if (VK(a)->timing_on && a->counting && !VK(a)->surfaceless &&
		!VK(a)->dxgi) {
		timing_take(a);
		if (VK(a)->timing_owed < TIMING_QUEUE - 1) {
			pi.pNext = &pid2;
			timed = true;
		}
	}
#endif
	const VkPresentTimeGOOGLE gt = {
		.presentID = (uint32_t)present_id,
	};
	VkPresentTimesInfoGOOGLE gti = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
		.swapchainCount = 1,
		.pTimes = &gt,
	};
	bool google = false;
	if (VK(a)->google_timing && a->counting && !VK(a)->surfaceless &&
		!VK(a)->dxgi) {
		google_take(a);
		gti.pNext = pi.pNext;
		pi.pNext = &gti;
		google = true;
	}
	double t_present = now_seconds();
	app_frame_mark(a, FRAME_MARK_PRESENT);
	/* What this frame cost -- recording and submitting -- which the late-latch
	 * cap leaves room for (see late_latch_feedback). Measured from the far
	 * side of the device waits, never from App::latch_time: FIFO blocks in the
	 * acquire, and charging that wait to the frame drives the cap negative. */
	a->late_cost = a->late_cost > 0.0
				 ? a->late_cost * 0.9 + (t_present - t_rec) * 0.1
				 : t_present - t_rec;

	/* The rest of the same span is the submit. Subtracted rather than timed,
	 * so the two phases add up to what late_cost is measured over. */
	if (a->timing)
		a->fc_submit = (t_present - t_rec) - a->fc_record;

	/* Nothing to present where the window is the renderer's own images: the
	 * readback below is the present (RendererState::surfaceless). */
	VkResult pr = VK(a)->dxgi ? VK(a)->bridge->present_own(a, img)
				: VK(a)->surfaceless ? VK_SUCCESS
									 : vkQueuePresentKHR(VK(a)->queue, &pi);
	a->present_calls++;
	if (VK(a)->present_copy && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR))
		present_copied(a, frame, img);
	const double t_presented = now_seconds();
	app_frame_mark(a, FRAME_MARK_PRESENTED);
	if (a->timing)
		a->fc_present = t_presented - t_present;
	/* SUBOPTIMAL counts: the image was presented, it is only the swapchain that
	 * wants rebuilding. This gates the late-latch probe, which must not stall
	 * for as long as a compositor keeps saying so. */
	if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) {
		if (timed)
			VK(a)->timing_owed++;
		if (timed || google)
			app_present_numbered(a, a->presents, (uint32_t)present_id,
								 t_present);
		a->presents++;
		/* Both halves of the throttle, added: which call a driver blocks in is
		 * the driver's business (Mesa's FIFO waits in the acquire). Not on a
		 * software device, whose present waits for the rasteriser: measured
		 * on lavapipe, 33 ms of it that no hold made shorter. */
		if (!VK(a)->phys_software)
			a->present_block = acq_block + (t_presented - t_present);
	}
	/* A configure the window server must see answered only once its frame
	 * is shown: Mesa's X11 present hands the image to a thread of its own,
	 * so vkQueuePresentKHR returns before the server has it, and the
	 * counter the flush below sets would reach the manager first -- which
	 * then shows the new size with the old frame, the artifact the
	 * handshake exists to prevent. Waited for on the frame that acks and no
	 * other, by the waiter above, or here where there is none. */
	if (cfg_pending && BACKEND(a)->ack_when_shown && VK(a)->present_wait &&
		!VK(a)->surfaceless && (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) &&
		!shown_hand_over(a, present_id))
		(void)VK(a)->wait_for_present(VK(a)->device, VK(a)->swapchain,
									  present_id, ACK_SHOWN_WAIT_NS);

	/* Get the commit -- and with it the ack and the frame request -- onto the
	 * wire now. The main thread flushes too, but it may already be parked in
	 * epoll_wait(), and a frame callback whose request never left this process
	 * is one the resize path would wait out in full. */
	BACKEND(a)->flush(a);

	/* The acquire has already cleared swap_fresh, so this is the
	 * driver's answer and not a second count of the fault. */
	scale_check(a, pr);
	if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
		a->resized = VK(a)->swap_spent = true;
		app_frame_take_back(a);  /* refused, or shown regardless */
	} else if (pr == VK_SUBOPTIMAL_KHR) {
		/* presented anyway, so the request stands */
		a->resized = VK(a)->swap_spent = true;
	} else if (pr != VK_SUCCESS) {
		/* Anything else is a present that did not commit, so the frame request
		 * just made is never answered and must be cancelled -- else the loop
		 * waits it out, reads the timeout as "not on screen" and skips the
		 * frame that would have delivered it. The memory errors take this
		 * branch with the losses for that reason. */
		if (!vk_lost(a, pr)) {
			/* Recorded, not propagated: the frame *was* submitted. A loss is
			 * announced by the recovery already. Bound rather than cast:
			 * fnl_vk_result() is warn_unused_result. */
			const FnlResult prc = VKR(pr);
			(void)prc;
		}
		app_frame_take_back(a);
	}
}

/* How many swapchains in a row, each made at the surface's own extent, may
 * be out of date at their first acquire before the window is nudged. On
 * NVIDIA's X11 swapchain a run, once begun, never ends by itself; the
 * longest run that did end, over 140 rounds of tools/probe/wsidrag.c, was
 * three. At the testbed's 50 rebuilds a second eight is a sixth of one. */
#define SWAP_STALE_RUN 8

/* One acquire's verdict on the live swapchain, out of date or not. A run
 * of SWAP_STALE_RUN asks the pump to nudge the window's size (DESIGN *X11:
 * xcb, not Xlib*): nothing on this side frees it, and a new size from the
 * server does. Asked again at each further run, said once a run. */
static void swap_stale_note(App *a, bool out_of_date)
{
	const bool counts = out_of_date && VK(a)->swap_fresh && VK(a)->swap_agreed;
	VK(a)->swap_fresh = false;
	if (!out_of_date)
		VK(a)->swap_nudged = false;
	VK(a)->swap_stale_run = counts ? VK(a)->swap_stale_run + 1 : 0;
	if (VK(a)->swap_stale_run < SWAP_STALE_RUN && !FNL_FAULT("stale-swapchain"))
		return;
	if (!VK(a)->swap_nudged)
		fnl_log(FNL_LOG_WARN, "every swapchain at the window's own size is "
				"out of date at once (%d in a row); the window's size is "
				"nudged", VK(a)->swap_stale_run);
	VK(a)->swap_nudged = true;
	VK(a)->swap_stale_run = 0;
	app_request_size_nudge(a);
}

/* Returns true iff a frame was submitted to the queue; the retired-swapchain
 * countdown advances only on that. Every early return is false, which is why
 * the checked calls use VK_TRY_FALSE (see vk_internal.h) and never VK_TRY. */
bool draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
				uint32_t cfg_serial, bool cfg_pending)
{
	/* The near side of the two waits below, for --frame-times: between them
	 * they are where the GPU's own frame time shows up. */
	const double t_enter = a->timing ? now_seconds() : 0.0;

	/* No measurement until this frame reaches a present, so an early return
	 * cannot hand the frame policy an earlier turn's block. */
	a->present_block = -1.0;
	a->present_untimed = VK(a)->phys_software;

	/* The half of the offscreen target this frame's antialiasing wants, built
	 * the first time a frame wants it. Here because this is the first point
	 * that has the mode and the target together, on the thread that owns
	 * both, and ahead of the record decision because whatever is cached was
	 * recorded without it. */
	if (scene_offscreen(a, ds) &&
		scene_ensure_aa(a, ds->aa == FNL_AA_MSAA,
						ds->aa == FNL_AA_MLAA && !scene_mlaa_direct(a, ds)))
		a->record_gen++;

	/* Was the GPU far enough ahead that this frame will not wait for it? This
	 * slot's own fence, the one the wait below blocks on -- never the previous
	 * slot's, which holds the frame submitted moments ago and so answers no
	 * whatever the device is doing. Read before the wait. */
	if (a->timing && VK(a)->in_flight[frame] != VK_NULL_HANDLE &&
		vkGetFenceStatus(VK(a)->device, VK(a)->in_flight[frame]) == VK_SUCCESS)
		a->fc_idle++;

	const double t_fence = now_seconds();
	/* Checked: with an infinite timeout all this wait can report is
	 * VK_ERROR_DEVICE_LOST, which the caller needs to hear. */
	VkResult wr = vkWaitForFences(VK(a)->device, 1, &VK(a)->in_flight[frame],
								  VK_TRUE, UINT64_MAX);
	if (vk_lost(a, wr))
		return false;
	VK_TRY_FALSE(wr);

	/* Unconditional, unlike t_enter: under FIFO this acquire is where the CPU
	 * sits out the display, and how long is what late latching goes on where
	 * the window server invites nothing (see App::present_block). The fence
	 * wait above is deliberately outside it -- that is the GPU being behind,
	 * not spare time. */
	const double t_acq = now_seconds();
	a->device_block = t_acq - t_fence;

	uint32_t img;
	const uint64_t acquire_timeout =
		BACKEND(a)->is_output && !VK(a)->present_copy
			? ACQUIRE_TIMEOUT_DISPLAY_NS : ACQUIRE_TIMEOUT_NS;
	/* The renderer's own images are taken in turn; frame_record() waits for
	 * the frame that last used one before it is drawn again. */
	VkResult r = VK_SUCCESS;
	if (VK(a)->surfaceless) {
		img = VK(a)->own_next;
		VK(a)->own_next = (img + 1) % VK(a)->image_count;
	} else {
		r = vkAcquireNextImageKHR(VK(a)->device, VK(a)->swapchain,
								  acquire_timeout, VK(a)->img_available[frame],
								  VK_NULL_HANDLE, &img);
	}
	if (vk_lost(a, r))
		return false;
	/* Timed out with no image available: img_available[frame] is left
	 * unsignalled (the spec guarantees this), and the bail lets the loop
	 * re-check quit. */
	if (r == VK_TIMEOUT || r == VK_NOT_READY)
		return false;
	/* Before swap_stale_note(), which clears swap_fresh. */
	scale_check(a, r);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) {
		swap_stale_note(a, true);
		a->resized = VK(a)->swap_spent = true;
		return false;
	}
	/* Suboptimal still hands over an image, so this frame goes ahead and the
	 * rebuild is left to the next. Acted on here as well as at the present: a
	 * driver may say so only on acquire. */
	if (r == VK_SUBOPTIMAL_KHR)
		a->resized = VK(a)->swap_spent = true;
	else if (r != VK_SUCCESS)
		VK_TRY_FALSE(r);
	swap_stale_note(a, false);

	/* An image is in hand, so from here every failure hands the acquire's
	 * semaphore back through frame_bail(). */
	const double t_ready = now_seconds();
	/* What the acquire blocked for, to be added to the present's: a driver may
	 * throttle a FIFO swapchain in either call. */
	const double acq_block = t_ready - t_acq;
	if (a->timing)
		a->fc_wait = t_ready - t_enter;

	gpu_time_read(a, img);
	/* A frame asked to read itself back (App::snap_owed): recorded afresh
	 * with the copy in it, and marked stale again after, so the next
	 * recording drops it. The copy path's readback already has every
	 * frame. A surface whose images cannot be copied from gives none. */
	bool snap = a->snap_owed;
	if (snap && (!VK(a)->snap_usage ||
				 (!VK(a)->present_copy && !ensure_snap_buf(a)))) {
		app_snapshot_decline(a);
		snap = false;
	}
	if (snap)
		a->record_gen++;
	double t_rec = t_ready;
	app_frame_mark(a, FRAME_MARK_REC);
	if (!frame_record(a, img, ds, t_ready, &t_rec))
		return frame_bail(a, frame, false);
	VK(a)->images_in_flight[img] = VK(a)->in_flight[frame];

	/* A reset that failed left the fence signalled: the bail resets it
	 * again and carries it, or, failing that too, submits without it and
	 * hands the frame to recovery. */
	VkResult fr = vkResetFences(VK(a)->device, 1, &VK(a)->in_flight[frame]);
	if (vk_lost(a, fr) || VKR(fr) < 0)
		return frame_bail(a, frame, false);

	VkPipelineStageFlags wait_stage =
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	/* Neither semaphore where nothing is acquired or presented
	 * (RendererState::surfaceless): the fence alone orders the readback. */
	const uint32_t sems = VK(a)->surfaceless ? 0u : 1u;
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = VK(a)->dxgi ? VK(a)->bridge->submit_chain(a, img) : NULL,
		.waitSemaphoreCount = sems,
		.pWaitSemaphores = &VK(a)->img_available[frame],
		.pWaitDstStageMask = &wait_stage,
		.commandBufferCount = 1,
		.pCommandBuffers = &VK(a)->img_cmd[img],
		.signalSemaphoreCount = sems,
		.pSignalSemaphores = &VK(a)->render_done[img],
	};
	/* Past the reset: the fence is unsignalled and this submit was the only
	 * thing that would have signalled it, so the bail has to. */
	VkResult sr = vkQueueSubmit(VK(a)->queue, 1, &si, VK(a)->in_flight[frame]);
	if (vk_lost(a, sr) || VKR(sr) < 0)
		return frame_bail(a, frame, true);
	VK(a)->ts_unread[img] = VK(a)->ts_pool != VK_NULL_HANDLE;
	if (VK(a)->ts_unread[img])
		VK(a)->ts_index[img] = app_gpu_timed(a);

	frame_present(a, frame, img, cfg_serial, cfg_pending, acq_block, t_rec);

	/* The snapshot, once the frame has run: the one stall a move costs
	 * here, and the recording marked stale so the next frame drops the
	 * copy. The copy path handed its pixels over in present_copied(). */
	if (snap) {
		if (!VK(a)->present_copy) {
			const VkResult sw = vkWaitForFences(VK(a)->device, 1,
												&VK(a)->in_flight[frame],
												VK_TRUE, UINT64_MAX);
			if (!vk_lost(a, sw) && sw == VK_SUCCESS)
				app_snapshot_set(a, VK(a)->snap_map[0], a->extent.w,
								 a->extent.h, (size_t)a->extent.w * 4u,
								 surface_bgra(a));
			else
				app_snapshot_decline(a);
		}
		a->record_gen++;
	}

	/* As a present that reported a loss would leave things: submitted, and
	 * the flag up for the loop's next lost(). */
	if (FNL_FAULT("device-lost"))
		a->device_lost = true;
	if (FNL_FAULT("surface-lost"))
		a->surface_lost = true;

	/* Submitted, whatever the present made of it, which is all the
	 * retired-swapchain countdown depends on. */
	return true;
}

/* One sample means FSAA is not running, whatever was asked for. */
int vk_max_samples(const App *a)
{
	return (int)scene_samples(a);
}

int vk_active_samples(const App *a, const FnlDrawState *ds)
{
	return scene_msaa(a, ds) ? (int)scene_samples(a) : 1;
}
