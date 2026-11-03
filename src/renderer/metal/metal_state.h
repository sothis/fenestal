/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * Everything about the Metal renderer that no file outside src/renderer/metal/
 * may see: its state, behind App::renderer_state. metal_internal.h includes
 * it; nothing else does. Objective-C only, unlike the other renderers' state:
 * the objects are held as themselves, strongly, under ARC, which C cannot
 * spell -- and nothing in C needs to.
 *
 * App::renderer_state is allocated by create_instance() and freed by
 * destroy_instance(), and every object in it is let go before the free:
 * calloc and free know nothing of ARC's references (mt_destroy_instance()).
 * The [any] entries -- device_name, present_mode_name, max_samples -- read
 * only App's own atomics, and App::gpu_name under App::lock, and never it.
 */

#ifndef FNL_METAL_STATE_H
#define FNL_METAL_STATE_H

/* The renderer's state, from an App. */
#define MT(a) ((a)->renderer_state)

/* Drawables the layer keeps: three, so that a frame on the display, one
 * queued and one drawn need not wait for one another. */
#define MT_DRAWABLES 3

/* The largest texture every Mac GPU Metal runs on makes (MTLGPUFamilyMac2),
 * which the scale is resolved against. */
#define MT_MAX_EXTENT 16384

/* An image the GPU draws into, with its depth where it has one. Nil
 * together when there is none. */
typedef struct {
	__strong id<MTLTexture> color;
	__strong id<MTLTexture> depth;
} MtTarget;

/* frame.m's mailbox for the drawables' presented times. */
@class FnlShownBox;

struct RendererState {
	/* ---- the device ---------------------------------------------------- */
	__strong id<MTLDevice>       device;
	__strong id<MTLCommandQueue> queue;
	/* The layer the frame is presented into: the window's, which the
	 * backend made and keeps, taken by the bridge (surface_cocoa.m). */
	__strong CAMetalLayer       *layer;

	/* Per frame slot, the command buffer that last used its arenas, which
	 * the slot waits on before they are written again; nil once it is
	 * known to be done. */
	__strong id<MTLCommandBuffer> slot_cb[MAX_FRAMES_IN_FLIGHT];
	/* Whether that buffer's GPU time is no frame's to report: it carries a
	 * snapshot's blit, which Metal's whole-buffer time would hold, or it
	 * is the part of a frame that presented none. Such a buffer is not
	 * sampled (fnl_frame_gpu_ms()). slot_index is the frame a timed one
	 * drew (app_gpu_timed()). */
	bool slot_untimed[MAX_FRAMES_IN_FLIGHT];
	uint64_t slot_index[MAX_FRAMES_IN_FLIGHT];
	/* When each drawable reached the screen (fnl_frame_shown()), put
	 * there by its presented handler and taken out by the render thread
	 * (frame.m). Made at the first frame counted. */
	__strong FnlShownBox *shown_box;

	/* ---- the window ---------------------------------------------------- */
	/* The window's depth, at App::extent, for a scene drawn straight in. */
	__strong id<MTLTexture> window_depth;
	/* Whether the layer presents on the display's refresh: vsync, as the
	 * last rebuild set it. */
	bool display_sync;
	/* The drawable that did not come: said once, until one does. */
	bool drawable_late;

	/* ---- the scene's targets ----------------------------------------------
	 * As the other renderers': the scene at one resolution, sampled by the
	 * fetch; the multisampled pair FSAA draws into and resolves from; and
	 * MLAA's output. A refusal is remembered. */
	MtTarget scene;
	MtTarget scene_ms;
	bool     scene_ms_refused;
	__strong id<MTLTexture> mlaa;
	bool     mlaa_refused;
	int      samples;          /* what FSAA multisamples at */

	/* What on_record draws into, while it runs; nil otherwise.
	 * fnl_metal_scene() reads it back. */
	__strong id<MTLRenderCommandEncoder> scene_enc_now;
	__strong id<MTLCommandBuffer>        scene_cb_now;
	int scene_x_now, scene_y_now, scene_w_now, scene_h_now;

	/* ---- the overlay ---------------------------------------------------- */
	__strong id<MTLRenderPipelineState> pso_rect;
	__strong id<MTLRenderPipelineState> pso_shape;
	__strong id<MTLRenderPipelineState> pso_text;
	__strong id<MTLRenderPipelineState> pso_blit;
	__strong id<MTLRenderPipelineState> pso_mlaa;
	/* The cover: the fetch's shaders blended by a constant, drawing
	 * App::snap -- the frame read back before a move -- as a texture of
	 * this device, and the App::snap_gen it was uploaded from or last
	 * tried for (mt_overlay_cover()). And the shared buffer a frame's
	 * drawable is blitted into for that snapshot (frame.m). */
	__strong id<MTLRenderPipelineState> pso_cover;
	__strong id<MTLTexture>             cover_tex;
	uint32_t                            cover_gen;
	__strong id<MTLBuffer>              snap_buf;
	/* Neither tested nor written: everything the library draws over a
	 * depth it does not use. */
	__strong id<MTLDepthStencilState>   ds_off;
	/* Nearest for the atlas, baked for the pixel grid; linear for the
	 * scene, the whole filter of the scale. */
	__strong id<MTLSamplerState>        smp_nearest;
	__strong id<MTLSamplerState>        smp_linear;

	/* Per frame slot: the overlay's arenas, shared buffers the CPU writes
	 * straight into (App::text_map and App::ovl_map point into them). */
	__strong id<MTLBuffer> text_buf[MAX_FRAMES_IN_FLIGHT];
	__strong id<MTLBuffer> ovl_buf[MAX_FRAMES_IN_FLIGHT];

	__strong id<MTLTexture> font_tex;

	/* The encoder the overlay draws into, while it is being drawn: a flush
	 * outside it draws nothing, as one outside a Vulkan recording does. */
	__strong id<MTLRenderCommandEncoder> enc;
};

#endif /* FNL_METAL_STATE_H */
