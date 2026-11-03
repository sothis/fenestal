/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The Metal half of the frame: the scene, the window's image, the overlay
 * over it, and the present.
 *
 * Recorded afresh every frame, as Direct3D 12 and GL are (fenestal.h
 * *on_record*, DESIGN *Recording*): a command buffer is used once, so there
 * is no record_gen test here. Whether a turn draws at all is the frame
 * policy's, as with every renderer.
 *
 * Every image has row 0 at the top and a viewport's y counts down, so the
 * viewports and scissors are the Vulkan renderer's numbers as they stand.
 *
 * A frame is one autorelease pool: a drawable and every object a command
 * buffer hands back are autoreleased, and the render thread has no run loop
 * to drain them.
 */

#include "core/app.h"

#include "renderer/metal/metal_internal.h"

static const MTLClearColor BLACK = { 0.0, 0.0, 0.0, 1.0 };

/* ---- what this frame's scene is drawn into --------------------------------
 * Asked of the objects, as on Vulkan: mt_scene_set_size() and
 * mt_scene_ensure_aa() give up rather than fail. */

static bool scene_offscreen(const App *a, const FnlDrawState *ds)
{
	return ds->scene_w > 0 && MT(a)->scene.color &&
		   a->scene_extent.w == ds->scene_w &&
		   a->scene_extent.h == ds->scene_h;
}

static bool scene_msaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MSAA && MT(a)->scene_ms.color;
}

/* MLAA straight into the window's image when the scene is already the size
 * it lands at; through an image of its own when it is then scaled. */
static bool scene_mlaa_direct(const App *a, const FnlDrawState *ds)
{
	int ax, ay, aw, ah;
	if (ds->aa != FNL_AA_MLAA || !scene_offscreen(a, ds))
		return false;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	return ds->scene_w == aw && ds->scene_h == ah;
}

static bool scene_mlaa(const App *a, const FnlDrawState *ds)
{
	return ds->aa == FNL_AA_MLAA && MT(a)->mlaa && !scene_mlaa_direct(a, ds);
}

int mt_active_samples(const App *a, const FnlDrawState *ds)
{
	return scene_msaa(a, ds) ? (int)fnl_atomic_load_u32(&a->msaa_samples)
							 : 1;
}

/* ---- passes -------------------------------------------------------------- */

static void set_rect(id<MTLRenderCommandEncoder> enc, int x, int y, int w,
					 int h)
{
	[enc setViewport:(MTLViewport){ x, y, w, h, 0.0, 1.0 }];
	[enc setScissorRect:(MTLScissorRect){ (NSUInteger)x, (NSUInteger)y,
										  (NSUInteger)w, (NSUInteger)h }];
}

/* A pass over `color`, cleared to opaque black and its depth to far -- what
 * a Vulkan pass's load-op clear leaves -- with the viewport and scissor on
 * the whole of it. `resolve`, where there is one, takes the samples as the
 * pass ends, averaged in linear light since both formats are _sRGB, as
 * Vulkan's resolve attachment does. A depth that only matches the
 * pipelines' format is cleared and dropped. */
static id<MTLRenderCommandEncoder> begin_pass(App *a, id<MTLCommandBuffer> cb,
											  id<MTLTexture> color,
											  id<MTLTexture> resolve,
											  id<MTLTexture> depth,
											  bool keep_depth)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor
									   renderPassDescriptor];
	rp.colorAttachments[0].texture = color;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].clearColor = BLACK;
	if (resolve) {
		rp.colorAttachments[0].resolveTexture = resolve;
		rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
	} else {
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	}
	rp.depthAttachment.texture = depth;
	rp.depthAttachment.loadAction = MTLLoadActionClear;
	rp.depthAttachment.clearDepth = 1.0;
	rp.depthAttachment.storeAction = keep_depth ? MTLStoreActionStore
												: MTLStoreActionDontCare;
	id<MTLRenderCommandEncoder> enc =
		[cb renderCommandEncoderWithDescriptor:rp];
	if (enc) {
		mt_own_state(a, enc);
		set_rect(enc, 0, 0, (int)color.width, (int)color.height);
	}
	return enc;
}

/* The seam: the encoder, its target bound and cleared, the viewport on the
 * rectangle, and what is drawn the caller's -- stashed for
 * fnl_metal_scene(), as Vulkan stashes its command buffer. */
static void record_scene(App *a, const FnlDrawState *ds,
						 id<MTLCommandBuffer> cb,
						 id<MTLRenderCommandEncoder> enc, FnlSceneTarget t,
						 int x, int y, int w, int h)
{
	if (!a->cb.on_record)
		return;
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
		.samples    = t == FNL_TARGET_OFFSCREEN_MS
						  ? (int)fnl_atomic_load_u32(&a->msaa_samples) : 1,
		.scene_time = ds->scene_time,
	};
	struct RendererState *g = MT(a);
	g->scene_enc_now = enc;
	g->scene_cb_now  = cb;
	g->scene_x_now = x;
	g->scene_y_now = y;
	g->scene_w_now = w;
	g->scene_h_now = h;
	a->cb.on_record(a->cb.ud, a, &f);
	g->scene_enc_now = nil;
	g->scene_cb_now  = nil;
}

/* The offscreen scene: rendered at its own resolution, resolved where it was
 * multisampled, and filtered into MLAA's image where it is then scaled. The
 * filter's pass takes the scene's depth only so that the one MLAA pipeline
 * matches it as it matches the window's. False is a pass Metal would not
 * open. */
static bool scene_pass(App *a, const FnlDrawState *ds,
					   id<MTLCommandBuffer> cb)
{
	struct RendererState *g = MT(a);
	const int w = a->scene_extent.w, h = a->scene_extent.h;
	const bool ms = scene_msaa(a, ds);
	id<MTLRenderCommandEncoder> enc =
		ms ? begin_pass(a, cb, g->scene_ms.color, g->scene.color,
						g->scene_ms.depth, false)
		   : begin_pass(a, cb, g->scene.color, nil, g->scene.depth, false);
	if (!enc)
		return false;
	record_scene(a, ds, cb, enc,
				 ms ? FNL_TARGET_OFFSCREEN_MS : FNL_TARGET_OFFSCREEN, 0, 0,
				 w, h);
	[enc endEncoding];

	if (scene_mlaa(a, ds)) {
		enc = begin_pass(a, cb, g->mlaa, nil, g->scene.depth, false);
		if (!enc)
			return false;
		mt_fetch(a, enc, g->pso_mlaa, g->scene.color);
		[enc endEncoding];
	}
	return true;
}

/* The scene onto the window's image, under the overlay: drawn straight in at
 * the area's own resolution, or the image it was rendered into fetched
 * across the letterboxed rectangle that keeps its aspect. */
static void draw_scene(App *a, const FnlDrawState *ds,
					   id<MTLCommandBuffer> cb,
					   id<MTLRenderCommandEncoder> enc)
{
	struct RendererState *g = MT(a);
	int ax, ay, aw, ah;
	content_px(a, ds, &ax, &ay, &aw, &ah);
	if (aw < 1 || ah < 1)
		return;

	int dx = 0, dy = 0, dw = aw, dh = ah;
	const bool offscreen = scene_offscreen(a, ds);
	if (offscreen)
		scene_dst_rect(ds->scene_w, ds->scene_h, aw, ah, &dx, &dy, &dw, &dh);
	set_rect(enc, ax + dx, ay + dy, dw, dh);

	if (!offscreen) {
		record_scene(a, ds, cb, enc, FNL_TARGET_DIRECT, ax + dx, ay + dy, dw,
					 dh);
		return;
	}
	if (scene_mlaa_direct(a, ds))
		mt_fetch(a, enc, g->pso_mlaa, g->scene.color);
	else
		mt_fetch(a, enc, g->pso_blit,
				 scene_mlaa(a, ds) ? g->mlaa : g->scene.color);
}

/* Everything drawn this frame into `target`, the drawable's texture. False
 * is a frame to bail out of: a pass would not open, or the overlay could
 * not be drawn. */
static bool draw(App *a, const FnlDrawState *ds, uint32_t slot,
				 id<MTLCommandBuffer> cb, id<MTLTexture> target)
{
	struct RendererState *g = MT(a);
	if (scene_offscreen(a, ds) && !scene_pass(a, ds, cb))
		return false;

	id<MTLRenderCommandEncoder> enc =
		begin_pass(a, cb, target, nil, g->window_depth, false);
	if (!enc)
		return false;
	draw_scene(a, ds, cb, enc);

	/* The overlay over the whole image, in a state of the library's own:
	 * on_record may have left anything set. */
	mt_own_state(a, enc);
	set_rect(enc, 0, 0, a->extent.w, a->extent.h);
	g->enc = enc;
	const FnlResult orc = draw_overlay(a, ds, slot);
	g->enc = nil;
	[enc endEncoding];
	return orc >= 0;
}

/* ---- when a frame reached the screen ---------------------------------------
 *
 * A drawable says when it was shown in a handler on a thread of Metal's,
 * which can run after the window and its App are gone: so the handler
 * reaches no App. It writes into this box, which the blocks keep alive, and
 * the render thread takes what is there once a frame and hands it to
 * app_frame_shown(). The box's lock is its own and never held with
 * App::lock. */

#define SHOWN_BOX_N 32

@interface FnlShownBox : NSObject
- (void)put:(uint64_t)index at:(double)mono;
- (int)take:(uint64_t *)index at:(double *)mono max:(int)max;
@end

@implementation FnlShownBox {
	NSLock  *lock_;
	uint64_t index_[SHOWN_BOX_N];
	double   mono_[SHOWN_BOX_N];
	int      n_;
}

- (instancetype)init
{
	if ((self = [super init]))
		lock_ = [[NSLock alloc] init];
	return self;
}

/* Full is a frame dropped: the render thread takes every frame. */
- (void)put:(uint64_t)index at:(double)mono
{
	[lock_ lock];
	if (n_ < SHOWN_BOX_N) {
		index_[n_] = index;
		mono_[n_] = mono;
		n_++;
	}
	[lock_ unlock];
}

- (int)take:(uint64_t *)index at:(double *)mono max:(int)max
{
	[lock_ lock];
	const int n = n_ < max ? n_ : max;
	for (int i = 0; i < n; i++) {
		index[i] = index_[i];
		mono[i] = mono_[i];
	}
	for (int i = n; i < n_; i++) {
		index_[i - n] = index_[i];
		mono_[i - n] = mono_[i];
	}
	n_ -= n;
	[lock_ unlock];
	return n;
}
@end

/* The drawable about to be presented as frame `index`: its presented time
 * put in the box. That time is CACurrentMediaTime()'s clock, which stops
 * while the machine sleeps and CLOCK_MONOTONIC does not, so it is moved
 * onto now_seconds()'s by the two read together in the handler. 0 is a
 * drawable that was never shown. */
static void shown_watch(App *a, id<MTLDrawable> drawable, uint64_t index)
{
	struct RendererState *g = MT(a);
	if (!g->shown_box)
		g->shown_box = [[FnlShownBox alloc] init];
	FnlShownBox *box = g->shown_box;
	if (@available(macOS 10.15.4, *)) {
		[drawable addPresentedHandler:^(id<MTLDrawable> d) {
			const CFTimeInterval t = d.presentedTime;
			if (t > 0.0)
				[box put:index at:t + (now_seconds() - CACurrentMediaTime())];
		}];
	}
}

/* What the handlers have put in the box since the last frame. */
static void shown_take(App *a)
{
	FnlShownBox *box = MT(a)->shown_box;
	if (!box)
		return;
	uint64_t index[SHOWN_BOX_N];
	double mono[SHOWN_BOX_N];
	const int n = [box take:index at:mono max:SHOWN_BOX_N];
	for (int i = 0; i < n; i++)
		app_frame_shown(a, index[i], mono[i], true);
}

/* ---- the frame ----------------------------------------------------------- */

/* Wait until the GPU has finished the frame that last used this slot, whose
 * arenas are about to be written again. False on a loss. */
static bool wait_slot(App *a, uint32_t slot)
{
	struct RendererState *g = MT(a);
	id<MTLCommandBuffer> cb = g->slot_cb[slot];
	if (!cb)
		return true;
	if (cb.status >= MTLCommandBufferStatusCompleted) {
		if (a->timing)
			a->fc_idle++;
	} else {
		[cb waitUntilCompleted];
	}
	g->slot_cb[slot] = nil;
	/* The GPU's time on that frame, which the buffer keeps once it is done
	 * (fnl_frame_gpu_ms()): read here, on this thread, and not by a
	 * completion handler, which could run after the App is gone. */
	if (cb.status == MTLCommandBufferStatusCompleted &&
		!g->slot_untimed[slot] && cb.GPUEndTime > cb.GPUStartTime)
		app_gpu_time(a, cb.GPUEndTime - cb.GPUStartTime,
					 g->slot_index[slot]);
	return !mt_cb_lost(a, cb);
}

/* Every slot whose buffer has completed: its time read, and the slot let
 * go as wait_slot() would. Only a buffer that completed without error,
 * which has no loss for wait_slot() to report. */
void mt_gpu_time_poll(App *a)
{
	struct RendererState *g = MT(a);
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		id<MTLCommandBuffer> cb = g->slot_cb[i];
		if (!cb || cb.status != MTLCommandBufferStatusCompleted)
			continue;
		g->slot_cb[i] = nil;
		if (!g->slot_untimed[i] && cb.GPUEndTime > cb.GPUStartTime)
			app_gpu_time(a, cb.GPUEndTime - cb.GPUStartTime,
						 g->slot_index[i]);
	}
}

/* The drawable's texture blitted into a shared buffer at the end of the
 * command buffer, for the snapshot a move takes (App::snap_owed): BGRA,
 * the drawable's order, row 0 the top, `*w` by `*h`. The buffer is made
 * for the largest extent yet; one a command buffer still copies into is
 * kept alive by it, so the old is simply let go. Nil where no buffer or
 * encoder is to be had. Needs a layer that is not framebuffer-only
 * (device.m). */
static id<MTLBuffer> snapshot_encode(App *a, id<MTLCommandBuffer> cb,
									 id<MTLTexture> tex, NSUInteger *w_out,
									 NSUInteger *h_out)
{
	struct RendererState *g = MT(a);
	/* No larger than the texture: drawableSize and the extent are set
	 * together (mt_window_images()), and a copy past the texture's edge
	 * would be a read out of bounds where they ever were not. */
	const NSUInteger w = MIN((NSUInteger)a->extent.w, tex.width);
	const NSUInteger h = MIN((NSUInteger)a->extent.h, tex.height);
	if (w == 0 || h == 0)
		return nil;
	*w_out = w;
	*h_out = h;
	const NSUInteger pitch = w * 4, need = pitch * h;
	if (!g->snap_buf || g->snap_buf.length < need)
		g->snap_buf =
			[g->device newBufferWithLength:need
								   options:MTLResourceStorageModeShared];
	if (!g->snap_buf)
		return nil;
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	if (!blit)
		return nil;
	[blit copyFromTexture:tex
			  sourceSlice:0
			  sourceLevel:0
			 sourceOrigin:MTLOriginMake(0, 0, 0)
			   sourceSize:MTLSizeMake(w, h, 1)
				 toBuffer:g->snap_buf
		destinationOffset:0
   destinationBytesPerRow:pitch
 destinationBytesPerImage:need];
	[blit endEncoding];
	return g->snap_buf;
}

bool mt_draw_frame(App *a, uint32_t frame, const FnlDrawState *ds,
				   uint32_t cfg_serial, bool cfg_pending)
{
	struct RendererState *g = MT(a);
	const double t_enter = a->timing ? now_seconds() : 0.0;
	a->present_block = -1.0;
	a->present_untimed = false;

	/* MAX_FRAMES_IN_FLIGHT slots, one a turn: the frame index is the slot,
	 * and the overlay's arenas are the slot's. */
	const uint32_t slot = frame % MAX_FRAMES_IN_FLIGHT;

	@autoreleasepool {
		const double t_fence = now_seconds();
		if (!wait_slot(a, slot) || mt_device_lost(a))
			return false;

		/* The layer's throttle, as Vulkan's acquire is: nextDrawable blocks
		 * until one of its drawables is free, which under display sync is
		 * the display taking a frame. Not a device wait, so not in
		 * device_block; added to the present's, as an acquire's is. A
		 * drawable that does not come in the layer's second is a window
		 * the display is not taking frames for: nothing is drawn, and the
		 * loop comes round. */
		const double t_acq = now_seconds();
		a->device_block = t_acq - t_fence;
		id<CAMetalDrawable> drawable = [g->layer nextDrawable];
		const double t_ready = now_seconds();
		const double acq_block = t_ready - t_acq;
		if (a->timing)
			a->fc_wait = t_ready - t_enter;
		if (!drawable) {
			if (!g->drawable_late)
				fnl_log(FNL_LOG_INFO, "the layer gave no drawable; frames "
						"wait for one");
			g->drawable_late = true;
			return false;
		}
		g->drawable_late = false;

		/* The half of the offscreen target this frame's antialiasing wants,
		 * built the first time a frame wants it. */
		if (scene_offscreen(a, ds))
			(void)mt_scene_ensure_aa(a, ds->aa == FNL_AA_MSAA,
									 ds->aa == FNL_AA_MLAA &&
										 !scene_mlaa_direct(a, ds));

		const double t_rec = now_seconds();
		app_frame_mark(a, FRAME_MARK_REC);
		id<MTLCommandBuffer> cb = [g->queue commandBuffer];
		if (!cb)
			return false;
		cb.label = @"fenestal frame";
		if (!draw(a, ds, slot, cb, drawable.texture)) {
			/* Committed all the same, with nothing presented: the slot is
			 * fenced by it as a finished frame's is, and its time, a part
			 * of a frame, is no frame's. */
			[cb commit];
			g->slot_cb[slot] = cb;
			g->slot_untimed[slot] = true;
			return false;
		}
		if (a->timing)
			a->fc_record = now_seconds() - t_rec;
		/* The frame read back for the snapshot a move takes: blitted at
		 * the end of this command buffer and waited for below, the one
		 * stall a move costs here. */
		id<MTLBuffer> snap = nil;
		NSUInteger snap_w = 0, snap_h = 0;
		if (a->snap_owed) {
			snap = snapshot_encode(a, cb, drawable.texture, &snap_w,
								   &snap_h);
			if (!snap)
				app_snapshot_decline(a);
		}

		/* What rides on the present is requested immediately before it, as
		 * on Vulkan (app_surface_sync()). */
		app_surface_sync(a, cfg_serial, cfg_pending);
		BACKEND(a)->frame_request(a);

		const double t_present = now_seconds();
		app_frame_mark(a, FRAME_MARK_PRESENT);
		a->late_cost = a->late_cost > 0.0
					 ? a->late_cost * 0.9 + (t_present - t_rec) * 0.1
					 : t_present - t_rec;
		if (a->timing)
			a->fc_submit = (t_present - t_rec) - a->fc_record;

		if (a->counting) {
			shown_take(a);
			shown_watch(a, drawable, a->presents);
		}
		[cb presentDrawable:drawable];
		[cb commit];
		g->slot_cb[slot] = cb;
		g->slot_untimed[slot] = snap != nil;
		if (!snap)
			g->slot_index[slot] = app_gpu_timed(a);
		a->present_calls++;
		a->presents++;
		const double t_presented = now_seconds();
		app_frame_mark(a, FRAME_MARK_PRESENTED);
		if (a->timing)
			a->fc_present = t_presented - t_present;
		/* Both halves of the throttle, added; the present itself only
		 * queues. */
		a->present_block = acq_block + (t_presented - t_present);
		if (snap) {
			[cb waitUntilCompleted];
			if (cb.status == MTLCommandBufferStatusCompleted)
				app_snapshot_set(a, snap.contents, (int)snap_w,
								 (int)snap_h, (size_t)snap_w * 4u, true);
			else
				app_snapshot_decline(a);
		}
		BACKEND(a)->flush(a);
	}

	if (FNL_FAULT("device-lost"))
		a->device_lost = true;
	if (FNL_FAULT("surface-lost"))
		a->surface_lost = true;

	/* Drawn and handed over, whatever the present made of it. */
	return true;
}
